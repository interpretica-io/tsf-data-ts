/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Suite helpers
 *
 * Every test needs the same things: an RPC server on the agent (the
 * dataset engine runs in it), the fixtures on that agent, and the
 * oracle - facts about the fixtures computed independently, by Python's
 * standard library (and pyarrow, when present) in lib/py/datagen.py.
 * The fixtures carry an exact, known set of defects; the tests assert
 * tsf-data finds exactly those.
 *
 * Plus thin wrappers over TE's MI measurement API: every test charts
 * what it measured.
 *
 * @author Maxim Menshikov <maxim.menshikov@interpretica.io>
 */

#ifndef __TSAPI_DATA_H__
#define __TSAPI_DATA_H__

#include "rcf_rpc.h"
#include "te_errno.h"
#include "te_string.h"
#include "te_mi_log.h"

#include "tapi_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The agent this suite works on. */
#define TSAPI_DATA_TA   "Agt_A"

/** One test's view of the agent. Zero-initialise before init. */
typedef struct tsapi_data_session {
    const char     *ta;
    rcf_rpc_server *pco;
    char           *dir;        /**< fixture directory on the agent */
    char           *work;       /**< scratch directory for this test */
    tapi_data_kv    oracle;     /**< facts from datagen.py */
} tsapi_data_session;

/** Create the RPC server; TEST_SKIP-able when the engine is missing. */
extern te_errno tsapi_data_session_init(tsapi_data_session *s,
                                        const char *name);

/**
 * Write the agent-side scripts, run the fixture generator and load the
 * oracle. TE_EOPNOTSUPP when the agent has no python3.
 */
extern te_errno tsapi_data_fixtures(tsapi_data_session *s);

/** A fixture's path on the agent (appended to @p out). */
extern const char *tsapi_data_path(const tsapi_data_session *s,
                                   const char *name, te_string *out);

/** A path in the test's scratch directory (appended to @p out). */
extern const char *tsapi_data_work(const tsapi_data_session *s,
                                   const char *name, te_string *out);

/** Oracle facts. */
extern const char *tsapi_data_fact(const tsapi_data_session *s,
                                   const char *key);
extern int64_t tsapi_data_fact_int(const tsapi_data_session *s,
                                   const char *key);
extern double tsapi_data_fact_dbl(const tsapi_data_session *s,
                                  const char *key);

/** Run a shell command on the agent; 0 when it exits 0. */
extern te_errno tsapi_data_sh(tsapi_data_session *s, const char *fmt, ...)
    TE_LIKE_PRINTF(2, 3);

/**
 * Run the ETL pipeline under test (lib/py/etl.py) on the agent.
 *
 * @param out   Output directory (scratch-relative name).
 * @param mode  ok, drop, dup, agg, append, orphan, schema.
 * @param run   Incremental run number, 0 for a full load.
 */
extern te_errno tsapi_data_etl(tsapi_data_session *s, const char *out,
                               const char *mode, int run);

/** Does the agent's Python have pyarrow (fixtures written by it)? */
extern te_bool tsapi_data_have_pyarrow(const tsapi_data_session *s);

extern void tsapi_data_session_fini(tsapi_data_session *s);

/** |a - b| <= rel * max(|a|, |b|, 1e-300) */
extern te_bool tsapi_data_near(double a, double b, double rel);

/* ---- MI charts -------------------------------------------------- */

/** A measurement artifact for tool "tsf-data". */
extern te_mi_logger *tsapi_mi_open(void);

/** One unitless data point of metric @p name. */
extern void tsapi_mi_put(te_mi_logger *l, const char *name, double v);

/** One data point of a typed metric (memory, time, percentage...). */
extern void tsapi_mi_put_type(te_mi_logger *l, te_mi_meas_type type,
                              const char *name, double v,
                              te_mi_meas_multiplier mult);

/**
 * A line graph. @p x names the X metric (NULL: the sequence number);
 * @p ys is a comma-separated list of Y metrics.
 */
extern void tsapi_mi_graph(te_mi_logger *l, const char *view,
                           const char *title, const char *x,
                           const char *ys);

/** Same for typed Y metrics, all of one type. */
extern void tsapi_mi_graph_type(te_mi_logger *l, const char *view,
                                const char *title, const char *x,
                                te_mi_meas_type type, const char *ys);

/** Flush and free (NULL-safe). */
extern void tsapi_mi_close(te_mi_logger **l);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSAPI_DATA_H__ */
