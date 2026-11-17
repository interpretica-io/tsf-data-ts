/** @file
 * @brief Profile group
 *
 * Statistical profiles against a reference. Every statistic tsf-data
 * computes over orders.csv - count, NULLs, min/max, mean, sample and
 * population standard deviation, sum, five quantiles, distinct count,
 * value frequencies, string length statistics, timestamp range - is
 * compared with the value Python's statistics module computed over the
 * same file. Exact algorithms must agree to 1e-9 relative; they must
 * also say they are exact.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "profile/reference"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

#define REL 1e-9

static void
expect_near(const char *what, double got, double want)
{
    if (!tsapi_data_near(got, want, REL))
        TEST_VERDICT("%s is %.17g, the reference says %.17g", what, got,
                     want);
}

int
main(int argc, char **argv)
{
    static const char *const qs[] = { "0.1", "0.25", "0.5", "0.75", "0.9" };
    tsapi_data_session sess = { 0 };
    tapi_data *ds = NULL;
    tapi_data_profile_info prof = { 0 };
    const tapi_data_col_profile *c;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    unsigned int i;

    TEST_START;

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_profile_ref"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "orders.csv", &p),
                            NULL, NULL, tsapi_data_fact(&sess, "orders.schema"),
                            &ds));

    TEST_STEP("Profile with exact algorithms");
    CHECK_RC(tapi_data_profile(ds, "quantiles=0.1,0.25,0.5,0.75,0.9;topk=4;"
                               "mode=exact", &prof));
    tapi_data_profile_log(&prof);
    if (prof.rows != (uint64_t)tsapi_data_fact_int(&sess, "orders.rows"))
        TEST_VERDICT("The profile covers %llu rows",
                     (unsigned long long)prof.rows);

    TEST_STEP("amount: moments, extremes, quantiles");
    c = tapi_data_profile_col(&prof, "amount");
    if (c == NULL || !c->has_moments || !c->q_exact || !c->distinct_exact)
        TEST_VERDICT("amount has no exact moments/quantiles/distinct");
    expect_near("mean(amount)", c->mean,
                tsapi_data_fact_dbl(&sess, "amount.mean"));
    expect_near("stdev(amount)", c->stddev,
                tsapi_data_fact_dbl(&sess, "amount.stdev"));
    expect_near("pstdev(amount)", c->stddev_pop,
                tsapi_data_fact_dbl(&sess, "amount.pstdev"));
    expect_near("sum(amount)", c->sum,
                tsapi_data_fact_dbl(&sess, "amount.sum"));
    if (strcmp(c->min, tsapi_data_fact(&sess, "amount.min")) != 0 ||
        strcmp(c->max, tsapi_data_fact(&sess, "amount.max")) != 0)
        TEST_VERDICT("amount spans %s..%s, the reference %s..%s", c->min,
                     c->max, tsapi_data_fact(&sess, "amount.min"),
                     tsapi_data_fact(&sess, "amount.max"));
    if (c->q_err != 0)
        TEST_VERDICT("Exact quantiles claim an error of %g", c->q_err);
    mi = tsapi_mi_open();
    for (i = 0; i < TE_ARRAY_LEN(qs); i++)
    {
        te_string key = TE_STRING_INIT;
        double q = strtod(qs[i], NULL);
        double got = tapi_data_profile_quantile(c, q);
        double want;

        te_string_append(&key, "amount.q%s", qs[i]);
        want = tsapi_data_fact_dbl(&sess, key.ptr);
        expect_near(key.ptr, got, want);
        tsapi_mi_put(mi, "q", q);
        tsapi_mi_put(mi, "tsf_data", got);
        tsapi_mi_put(mi, "python", want);
        te_string_free(&key);
    }
    tsapi_mi_graph(mi, "quantiles", "amount quantiles: tsf-data vs Python "
                   "statistics", "q", "tsf_data,python");
    tsapi_mi_close(&mi);

    TEST_STEP("qty: distinct and sum; status: exact frequencies");
    c = tapi_data_profile_col(&prof, "qty");
    if (c == NULL || c->distinct != tsapi_data_fact_dbl(&sess, "qty.distinct"))
        TEST_VERDICT("qty has %.0f distinct values", c == NULL ? -1 :
                     c->distinct);
    expect_near("sum(qty)", c->sum, tsapi_data_fact_dbl(&sess, "qty.sum"));
    c = tapi_data_profile_col(&prof, "status");
    if (c == NULL || !c->top_exact || c->ntop != 4)
        TEST_VERDICT("status has no exact top-4");
    mi = tsapi_mi_open();
    for (i = 0; i < c->ntop; i++)
    {
        te_string key = TE_STRING_INIT;

        te_string_append(&key, "status.%s", c->top[i]);
        if ((int64_t)c->top_count[i] != tsapi_data_fact_int(&sess, key.ptr))
            TEST_VERDICT("status %s counted %llu times", c->top[i],
                         (unsigned long long)c->top_count[i]);
        if (i > 0 && c->top_count[i] > c->top_count[i - 1])
            TEST_VERDICT("The frequencies are not in descending order");
        tsapi_mi_put(mi, "rank", i + 1);
        tsapi_mi_put(mi, "count", (double)c->top_count[i]);
        te_string_free(&key);
    }
    tsapi_mi_graph(mi, "freq", "status frequencies by rank", "rank",
                   "count");
    tsapi_mi_close(&mi);

    TEST_STEP("email: NULLs; note: lengths; created: time range");
    c = tapi_data_profile_col(&prof, "email");
    if (c == NULL || c->nulls != (uint64_t)tsapi_data_fact_int(&sess,
                                                    "orders.null_email"))
        TEST_VERDICT("email has %llu NULLs", c == NULL ? 0ULL :
                     (unsigned long long)c->nulls);
    expect_near("null_pct(email)", c->null_pct,
                100.0 * tsapi_data_fact_dbl(&sess, "orders.null_email") /
                tsapi_data_fact_dbl(&sess, "orders.rows"));
    c = tapi_data_profile_col(&prof, "note");
    if (c == NULL || !c->has_len)
        TEST_VERDICT("note has no length statistics");
    expect_near("min length(note)", c->len_min,
                tsapi_data_fact_dbl(&sess, "note.len_min"));
    expect_near("max length(note)", c->len_max,
                tsapi_data_fact_dbl(&sess, "note.len_max"));
    expect_near("mean length(note)", c->len_mean,
                tsapi_data_fact_dbl(&sess, "note.len_mean"));
    c = tapi_data_profile_col(&prof, "created");
    if (c == NULL || strcmp(c->min, tsapi_data_fact(&sess, "created.min")) ||
        strcmp(c->max, tsapi_data_fact(&sess, "created.max")))
        TEST_VERDICT("created spans %s..%s", c == NULL ? "?" : c->min,
                     c == NULL ? "?" : c->max);

    mi = tsapi_mi_open();
    for (i = 0; i < prof.ncols; i++)
    {
        tsapi_mi_put(mi, "column", i);
        tsapi_mi_put(mi, "distinct", prof.cols[i].distinct);
        tsapi_mi_put(mi, "nulls", (double)prof.cols[i].nulls);
    }
    tsapi_mi_graph(mi, "columns", "Distinct values and NULLs per column",
                   "column", "distinct,nulls");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_profile_free(&prof);
    tapi_data_close(ds);
    te_string_free(&p);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
