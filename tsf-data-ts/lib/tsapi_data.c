/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Suite helpers
 *
 * @author Maxim Menshikov <maxim.menshikov@interpretica.io>
 */

#define TE_LGR_USER "TSAPI DATA"

#include "te_config.h"

#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "logger_api.h"
#include "te_alloc.h"
#include "te_string.h"
#include "tapi_file.h"
#include "tapi_cfg_base.h"
#include "tapi_rpc_stdio.h"

#include "tsapi_data.h"
#include "tsapi_py.h"

/* See description in tsapi_data.h */
te_errno
tsapi_data_session_init(tsapi_data_session *s, const char *name)
{
    te_errno rc;

    memset(s, 0, sizeof(*s));
    s->ta = TSAPI_DATA_TA;
    rc = rcf_rpc_server_create(s->ta, name, &s->pco);
    if (rc != 0)
        ERROR("Cannot create the RPC server '%s' on %s: %r", name, s->ta, rc);
    return rc;
}

static te_errno
run(tsapi_data_session *s, const char *cmd)
{
    rpc_wait_status st;

    RPC_AWAIT_IUT_ERROR(s->pco);
    s->pco->timeout = 30 * 60 * 1000;
    st = rpc_system(s->pco, cmd);
    if (st.flag != RPC_WAIT_STATUS_EXITED || st.value != 0)
    {
        WARN("'%s' did not succeed (flag %d, value %u)", cmd, st.flag,
             st.value);
        return TE_RC(TE_TAPI, TE_ESHCMD);
    }
    return 0;
}

/* See description in tsapi_data.h */
te_errno
tsapi_data_sh(tsapi_data_session *s, const char *fmt, ...)
{
    te_string cmd = TE_STRING_INIT;
    va_list ap;
    te_errno rc;

    va_start(ap, fmt);
    te_string_append_va(&cmd, fmt, ap);
    va_end(ap);
    rc = run(s, cmd.ptr);
    te_string_free(&cmd);
    return rc;
}

/* See description in tsapi_data.h */
te_errno
tsapi_data_fixtures(tsapi_data_session *s)
{
    char *tmp = NULL;
    te_string base = TE_STRING_INIT;
    te_string path = TE_STRING_INIT;
    char *oracle = NULL;
    te_errno rc;

    tmp = tapi_cfg_base_get_ta_dir(s->ta, TAPI_CFG_BASE_TA_DIR_TMP);
    if (tmp == NULL)
        return TE_RC(TE_TAPI, TE_EFAIL);
    tapi_file_make_custom_pathname(&base, tmp, "-tsfdata");
    s->dir = TE_STRDUP(base.ptr);
    te_string_append(&base, "-work");
    s->work = TE_STRDUP(base.ptr);
    free(tmp);
    te_string_free(&base);

    rc = tsapi_data_sh(s, "mkdir -p %s %s", s->dir, s->work);
    if (rc != 0)
        goto out;
    tsapi_data_path(s, "datagen.py", &path);
    rc = tapi_file_create_ta(s->ta, path.ptr, "%s", tsapi_py_datagen);
    te_string_reset(&path);
    if (rc == 0)
    {
        tsapi_data_path(s, "etl.py", &path);
        rc = tapi_file_create_ta(s->ta, path.ptr, "%s", tsapi_py_etl);
        te_string_reset(&path);
    }
    if (rc != 0)
    {
        ERROR("Cannot write the agent-side scripts onto %s: %r", s->ta, rc);
        goto out;
    }
    rc = tsapi_data_sh(s, "python3 %s/datagen.py %s", s->dir, s->dir);
    if (rc != 0)
    {
        rc = TE_RC(TE_TAPI, TE_EOPNOTSUPP);
        goto out;
    }
    tsapi_data_path(s, "oracle.txt", &path);
    rc = tapi_file_read_ta(s->ta, path.ptr, &oracle);
    if (rc != 0)
        goto out;
    tapi_data_kv_parse(&s->oracle, oracle);
    RING("Fixtures in %s on %s: %zu oracle facts (pyarrow %s)", s->dir,
         s->ta, s->oracle.n,
         *tsapi_data_fact(s, "pyarrow") != '\0' ?
         tsapi_data_fact(s, "pyarrow") : "absent");

out:
    free(oracle);
    te_string_free(&path);
    return rc;
}

/* See description in tsapi_data.h */
const char *
tsapi_data_path(const tsapi_data_session *s, const char *name, te_string *out)
{
    te_string_append(out, "%s/%s", s->dir != NULL ? s->dir : ".", name);
    return out->ptr;
}

/* See description in tsapi_data.h */
const char *
tsapi_data_work(const tsapi_data_session *s, const char *name, te_string *out)
{
    te_string_append(out, "%s/%s", s->work != NULL ? s->work : ".", name);
    return out->ptr;
}

/* See description in tsapi_data.h */
const char *
tsapi_data_fact(const tsapi_data_session *s, const char *key)
{
    const char *v = tapi_data_kv_get(&s->oracle, key, NULL);

    if (v == NULL)
    {
        WARN("No oracle fact '%s'", key);
        return "";
    }
    return v;
}

/* See description in tsapi_data.h */
int64_t
tsapi_data_fact_int(const tsapi_data_session *s, const char *key)
{
    return strtoll(tsapi_data_fact(s, key), NULL, 10);
}

/* See description in tsapi_data.h */
double
tsapi_data_fact_dbl(const tsapi_data_session *s, const char *key)
{
    return strtod(tsapi_data_fact(s, key), NULL);
}

/* See description in tsapi_data.h */
te_errno
tsapi_data_etl(tsapi_data_session *s, const char *out, const char *mode,
               int run_no)
{
    return tsapi_data_sh(s, "python3 %s/etl.py %s/orders_base.csv "
                         "%s/customers.csv %s/%s %s %d", s->dir, s->dir,
                         s->dir, s->work, out, mode, run_no);
}

/* See description in tsapi_data.h */
te_bool
tsapi_data_have_pyarrow(const tsapi_data_session *s)
{
    return *tsapi_data_fact(s, "pyarrow") != '\0';
}

/* See description in tsapi_data.h */
void
tsapi_data_session_fini(tsapi_data_session *s)
{
    if (s->pco != NULL && s->work != NULL)
        tsapi_data_sh(s, "rm -rf %s", s->work);
    free(s->dir);
    free(s->work);
    s->dir = s->work = NULL;
    tapi_data_kv_free(&s->oracle);
    if (s->pco != NULL)
    {
        rcf_rpc_server_destroy(s->pco);
        s->pco = NULL;
    }
}

/* See description in tsapi_data.h */
te_bool
tsapi_data_near(double a, double b, double rel)
{
    double m = fmax(fmax(fabs(a), fabs(b)), 1e-300);

    return fabs(a - b) <= rel * m;
}

/* ---- MI ----------------------------------------------------------- */

/* See description in tsapi_data.h */
te_mi_logger *
tsapi_mi_open(void)
{
    te_mi_logger *l = NULL;

    if (te_mi_logger_meas_create("tsf-data", &l) != 0)
        return NULL;
    return l;
}

/* See description in tsapi_data.h */
void
tsapi_mi_put(te_mi_logger *l, const char *name, double v)
{
    if (l != NULL)
        te_mi_logger_add_meas(l, NULL, TE_MI_MEAS_UNITLESS_VALUE, name,
                              TE_MI_MEAS_AGGR_SINGLE, v,
                              TE_MI_MEAS_MULTIPLIER_PLAIN);
}

/* See description in tsapi_data.h */
void
tsapi_mi_put_type(te_mi_logger *l, te_mi_meas_type type, const char *name,
                  double v, te_mi_meas_multiplier mult)
{
    if (l != NULL)
        te_mi_logger_add_meas(l, NULL, type, name, TE_MI_MEAS_AGGR_SINGLE, v,
                              mult);
}

static void
graph(te_mi_logger *l, const char *view, const char *title, const char *x,
      te_mi_meas_type type, const char *ys)
{
    char *list;
    char *tok;
    char *save = NULL;

    if (l == NULL)
        return;
    te_mi_logger_add_meas_view(l, NULL, TE_MI_MEAS_VIEW_LINE_GRAPH, view,
                               title);
    te_mi_logger_meas_graph_axis_add_name(l, NULL, TE_MI_MEAS_VIEW_LINE_GRAPH,
                                          view, TE_MI_GRAPH_AXIS_X,
                                          x != NULL ? x :
                                          TE_MI_GRAPH_AUTO_SEQNO);
    list = TE_STRDUP(ys);
    for (tok = strtok_r(list, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save))
        te_mi_logger_meas_graph_axis_add(l, NULL, TE_MI_MEAS_VIEW_LINE_GRAPH,
                                         view, TE_MI_GRAPH_AXIS_Y, type, tok);
    free(list);
}

/* See description in tsapi_data.h */
void
tsapi_mi_graph(te_mi_logger *l, const char *view, const char *title,
               const char *x, const char *ys)
{
    graph(l, view, title, x, TE_MI_MEAS_UNITLESS_VALUE, ys);
}

/* See description in tsapi_data.h */
void
tsapi_mi_graph_type(te_mi_logger *l, const char *view, const char *title,
                    const char *x, te_mi_meas_type type, const char *ys)
{
    graph(l, view, title, x, type, ys);
}

/* See description in tsapi_data.h */
void
tsapi_mi_close(te_mi_logger **l)
{
    if (*l != NULL)
        te_mi_logger_destroy(*l);
    *l = NULL;
}
