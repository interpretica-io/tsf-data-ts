/** @file
 * @brief Profile group
 *
 * Approximate profiling, and telling it from exact. 300 000 rows are
 * profiled exactly (a generous budget), then under shrinking memory
 * limits. When a structure does not fit, the profile must switch to its
 * approximate counterpart and say so - HyperLogLog for distinct counts,
 * a histogram for quantiles, Misra-Gries for frequencies - with an
 * error bound the approximate answer actually stays within. mode=exact
 * under a limit too small must refuse (TE_E2BIG) rather than
 * approximate. The memory used stays under every limit.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "profile/approx"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include <math.h>

#include "tapi_data.h"
#include "tsapi_data.h"

#define SPEC \
    "x:float:uniform:min=0,max=1000;" \
    "k:int:uniform:min=1,max=50000;" \
    "cat:string:enum:values=a|b|c|d|e|f|g|h,weights=40|20|10|10|8|6|4|2"

#define PARAMS "columns=x,k,cat;quantiles=0.01,0.5,0.99;topk=3"

int
main(int argc, char **argv)
{
    static const char *const limits[] = { "64k", "256k", "1m", "4m", "64m" };
    tsapi_data_session sess = { 0 };
    tapi_data_gen gen = { 0 };
    tapi_data *ds = NULL;
    tapi_data_profile_info exact = { 0 };
    tapi_data_profile_info apx = { 0 };
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    te_string prm = TE_STRING_INIT;
    char *schema = NULL;
    unsigned int i;
    unsigned int k;

    TEST_START;

    TEST_STEP("Open a session and generate 300000 rows");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_profile_approx"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    tsapi_data_work(&sess, "big.csv", &p);
    CHECK_RC(tapi_data_generate(sess.pco, p.ptr, "csv", SPEC,
                                "rows=300000;seed=3", &gen));
    schema = tapi_data_unescape(gen.schema);
    CHECK_RC(tapi_data_open(sess.pco, p.ptr, NULL, NULL, schema, &ds));

    TEST_STEP("The exact profile, with room to spare");
    CHECK_RC(tapi_data_profile(ds, PARAMS ";mode=exact;mem_limit=512m",
                               &exact));
    tapi_data_profile_log(&exact);
    for (i = 0; i < exact.ncols; i++)
    {
        if (!exact.cols[i].distinct_exact ||
            (exact.cols[i].nq > 0 && !exact.cols[i].q_exact) ||
            !exact.cols[i].top_exact)
            TEST_VERDICT("Column %s is not profiled exactly",
                         exact.cols[i].name);
    }

    TEST_STEP("mode=exact under a 64 KiB limit refuses");
    rc = tapi_data_profile(ds, PARAMS ";mode=exact;mem_limit=64k", &apx);
    if (TE_RC_GET_ERROR(rc) != TE_E2BIG)
        TEST_VERDICT("An exact profile that cannot fit returned %r", rc);
    tapi_data_profile_free(&apx);

    mi = tsapi_mi_open();
    for (i = 0; i < TE_ARRAY_LEN(limits); i++)
    {
        const tapi_data_col_profile *e;
        const tapi_data_col_profile *a;
        double lim;

        TEST_STEP("Profile under mem_limit=%s", limits[i]);
        te_string_reset(&prm);
        te_string_append(&prm, PARAMS ";mem_limit=%s", limits[i]);
        CHECK_RC(tapi_data_profile(ds, prm.ptr, &apx));
        lim = strtod(limits[i], NULL) *
              (strchr(limits[i], 'k') ? 1024.0 : 1048576.0);
        RING("mem_limit=%s: peak %llu bytes, %u pass(es)", limits[i],
             (unsigned long long)apx.mem_peak, apx.passes);
        if ((double)apx.mem_peak > lim)
            TEST_VERDICT("The profile used %llu bytes over a %s limit",
                         (unsigned long long)apx.mem_peak, limits[i]);

        /* k: 50000 possible values - distinct and quantiles. */
        e = tapi_data_profile_col(&exact, "k");
        a = tapi_data_profile_col(&apx, "k");
        if (!a->distinct_exact)
        {
            if (strncmp(a->distinct_algo, "hll", 3) != 0 ||
                a->distinct_err <= 0)
                TEST_VERDICT("An approximate distinct count names no "
                             "algorithm or error");
            if (fabs(a->distinct - e->distinct) >
                4 * a->distinct_err * e->distinct)
                TEST_VERDICT("HyperLogLog: %.0f distinct for %.0f, beyond "
                             "4 standard errors", a->distinct, e->distinct);
        }
        else if (a->distinct != e->distinct)
            TEST_VERDICT("An exact distinct count differs: %.0f vs %.0f",
                         a->distinct, e->distinct);
        for (k = 0; k < a->nq; k++)
        {
            double qa = strtod(a->qv[k], NULL);
            double qe = strtod(e->qv[k], NULL);

            if (a->q_exact ? qa != qe : fabs(qa - qe) > a->q_err + 1e-9)
                TEST_VERDICT("Quantile %g of k: %g by %s (error bound %g), "
                             "exact %g", a->q[k], qa, a->q_algo, a->q_err, qe);
        }

        /* cat: 8 values, skewed - frequencies. */
        e = tapi_data_profile_col(&exact, "cat");
        a = tapi_data_profile_col(&apx, "cat");
        for (k = 0; k < a->ntop; k++)
        {
            unsigned int j;

            for (j = 0; j < e->ntop && strcmp(e->top[j], a->top[k]); j++)
                ;
            if (j == e->ntop ||
                (double)a->top_count[k] > (double)e->top_count[j] ||
                (double)a->top_count[k] < (double)e->top_count[j] - a->top_err)
                TEST_VERDICT("Top value %s: %llu by %s (bound %g), exact "
                             "%llu", a->top[k],
                             (unsigned long long)a->top_count[k], a->top_algo,
                             a->top_err, j < e->ntop ?
                             (unsigned long long)e->top_count[j] : 0ULL);
        }

        a = tapi_data_profile_col(&apx, "x");
        tsapi_mi_put(mi, "mem_limit_kib", lim / 1024);
        tsapi_mi_put(mi, "mem_peak_kib", (double)apx.mem_peak / 1024);
        tsapi_mi_put(mi, "q_err_x", a->q_err);
        tsapi_mi_put(mi, "distinct_exact_x", a->distinct_exact ? 1 : 0);
        tapi_data_profile_free(&apx);
    }
    tsapi_mi_graph(mi, "memory", "Profile memory: peak vs limit (KiB)",
                   "mem_limit_kib", "mem_peak_kib,mem_limit_kib");
    tsapi_mi_graph(mi, "error", "Quantile error bound and exactness vs "
                   "memory limit (KiB)", "mem_limit_kib",
                   "q_err_x,distinct_exact_x");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_profile_free(&apx);
    tapi_data_profile_free(&exact);
    tapi_data_close(ds);
    tapi_data_gen_free(&gen);
    free(schema);
    te_string_free(&p);
    te_string_free(&prm);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
