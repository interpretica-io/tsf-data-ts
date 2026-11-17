/** @file
 * @brief Compare group
 *
 * Every comparison mode against a known difference. orders_b.csv is
 * orders_base.csv shuffled, with 3 rows removed, 2 added and 7 rows
 * changed (2 amounts by a cent, 2 by five euros, 1 e-mail upper-cased,
 * 2 timestamps by a second); orders_c.csv renames a column and adds one.
 * Each mode must report exactly what follows from those changes, and the
 * report must name the rows.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "compare/modes"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

static void
expect(const char *what, uint64_t got, int64_t want)
{
    if ((int64_t)got != want)
        TEST_VERDICT("%s: %llu, expected %lld", what,
                     (unsigned long long)got, (long long)want);
}

int
main(int argc, char **argv)
{
    tsapi_data_session sess = { 0 };
    const char *mode;
    tapi_data *a = NULL;
    tapi_data *b = NULL;
    tapi_data *c = NULL;
    tapi_data *g = NULL;
    tapi_data_diff d = TAPI_DATA_DIFF_INIT;
    tapi_data_gen gen = { 0 };
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    const char *schema;
    int64_t rows;
    int64_t missing;
    int64_t extra;
    int64_t changed;

    TEST_START;
    TEST_GET_STRING_PARAM(mode);

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_cmp_modes"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    schema = tsapi_data_fact(&sess, "orders.schema");
    rows = tsapi_data_fact_int(&sess, "base.rows");
    missing = tsapi_data_fact_int(&sess, "cmp.missing");
    extra = tsapi_data_fact_int(&sess, "cmp.extra");
    changed = tsapi_data_fact_int(&sess, "cmp.changed");
    CHECK_RC(tapi_data_open(sess.pco,
                            tsapi_data_path(&sess, "orders_base.csv", &p),
                            NULL, NULL, schema, &a));
    te_string_reset(&p);
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "orders_b.csv",
                                                      &p),
                            NULL, NULL, schema, &b));
    mi = tsapi_mi_open();

    if (strcmp(mode, "key") == 0)
    {
        TEST_STEP("By order_id: removed, added and changed rows");
        CHECK_RC(tapi_data_compare(a, b, "mode=key;keys=order_id", &d));
        tapi_data_diff_log(&d);
        expect("missing", d.missing, missing);
        expect("extra", d.extra, extra);
        expect("mismatched", d.mismatched, changed);
        expect("matched", d.matched, rows - missing - changed);
        if (strstr(d.report.ptr, "mismatch\t") == NULL ||
            strstr(d.report.ptr, "\tamount\t") == NULL)
            TEST_VERDICT("The report does not name the changed column");
        tsapi_mi_put(mi, "tolerance", 0);
        tsapi_mi_put(mi, "mismatched", (double)d.mismatched);

        TEST_STEP("With tolerances only the five-euro changes remain");
        CHECK_RC(tapi_data_compare(a, b, "mode=key;keys=order_id;"
                                   "abs_tol=0.02;nocase=1;"
                                   "ts_tol_us=1000000", &d));
        expect("mismatched within tolerances", d.mismatched,
               tsapi_data_fact_int(&sess, "cmp.amount_big"));
        tsapi_mi_put(mi, "tolerance", 1);
        tsapi_mi_put(mi, "mismatched", (double)d.mismatched);
        tsapi_mi_graph(mi, "tol", "Mismatched rows without and with "
                       "tolerances", "tolerance", "mismatched");
    }
    else if (strcmp(mode, "unordered") == 0)
    {
        TEST_STEP("As multisets: a changed row is one missing, one extra");
        CHECK_RC(tapi_data_compare(a, b, "mode=unordered", &d));
        tapi_data_diff_log(&d);
        expect("missing", d.missing, missing + changed);
        expect("extra", d.extra, extra + changed);
        expect("matched", d.matched, rows - missing - changed);
        tsapi_mi_put(mi, "kind", 0);
        tsapi_mi_put(mi, "rows", (double)d.matched);
        tsapi_mi_put(mi, "kind", 1);
        tsapi_mi_put(mi, "rows", (double)d.missing);
        tsapi_mi_put(mi, "kind", 2);
        tsapi_mi_put(mi, "rows", (double)d.extra);
        tsapi_mi_graph(mi, "multiset", "Unordered: matched, missing, extra",
                       "kind", "rows");
    }
    else if (strcmp(mode, "exact") == 0)
    {
        tapi_data_kv res = TAPI_DATA_KV_INIT;
        te_string q = TE_STRING_INIT;

        TEST_STEP("Row by row, a copy is equal");
        tsapi_data_work(&sess, "copy.jsonl", &q);
        CHECK_RC(tapi_data_convert(a, q.ptr, "jsonl", NULL, &res));
        tapi_data_kv_free(&res);
        CHECK_RC(tapi_data_open(sess.pco, q.ptr, NULL, NULL, schema, &c));
        te_string_free(&q);
        CHECK_RC(tapi_data_compare(a, c, "mode=exact", &d));
        if (d.equal != 1 || d.matched != (uint64_t)rows)
            TEST_VERDICT("A JSON Lines copy is not equal row by row");
        tsapi_mi_put(mi, "dataset", 0);
        tsapi_mi_put(mi, "mismatched", (double)d.mismatched);

        TEST_STEP("Row by row, a shuffled one is not");
        CHECK_RC(tapi_data_compare(a, b, "mode=exact;max_report=3", &d));
        tapi_data_diff_log(&d);
        expect("rows_b", d.rows_b, rows - missing + extra);
        if (d.equal != 0 || d.mismatched < (uint64_t)(rows / 2))
            TEST_VERDICT("A shuffled dataset compares equal row by row");
        expect("rows A has beyond B", d.missing, missing - extra);
        if (d.report_total <= 3)
            TEST_VERDICT("report_total %llu does not count every "
                         "difference", (unsigned long long)d.report_total);
        tsapi_mi_put(mi, "dataset", 1);
        tsapi_mi_put(mi, "mismatched", (double)d.mismatched);
        tsapi_mi_graph(mi, "exact", "Row-by-row mismatches: copy vs "
                       "shuffled", "dataset", "mismatched");
    }
    else if (strcmp(mode, "schema") == 0)
    {
        TEST_STEP("Schemas: renamed and added columns");
        te_string_reset(&p);
        CHECK_RC(tapi_data_open(sess.pco,
                                tsapi_data_path(&sess, "orders_c.csv", &p),
                                NULL, NULL, NULL, &c));
        CHECK_RC(tapi_data_compare(a, c, "mode=schema", &d));
        tapi_data_diff_log(&d);
        expect("missing columns", d.missing_columns, 1);
        expect("extra columns", d.extra_columns, 2);
        tsapi_mi_put(mi, "options", 0);
        tsapi_mi_put(mi, "differences",
                     (double)(d.missing_columns + d.extra_columns));

        TEST_STEP("A column mapping and an ignore list reconcile them");
        CHECK_RC(tapi_data_compare(a, c, "mode=key;keys=order_id;"
                                   "map=qty:quantity;ignore=channel", &d));
        if (d.equal != 1)
            TEST_VERDICT("Mapped and ignoring the new column, the data "
                         "still differs: %llu mismatched, %llu extra "
                         "columns", (unsigned long long)d.mismatched,
                         (unsigned long long)d.extra_columns);
        tsapi_mi_put(mi, "options", 1);
        tsapi_mi_put(mi, "differences",
                     (double)(d.missing_columns + d.extra_columns));
        tsapi_mi_graph(mi, "schema", "Column differences without and with "
                       "map/ignore", "options", "differences");
    }
    else if (strcmp(mode, "distribution") == 0)
    {
        double ks_same;

        TEST_STEP("Distributions: A and B are nearly the same");
        CHECK_RC(tapi_data_compare(a, b, "mode=distribution;"
                                   "columns=amount,status", &d));
        tapi_data_diff_log(&d);
        ks_same = tapi_data_kv_dbl(&d.kv, "max_stat", 1);
        if (ks_same > 0.02)
            TEST_VERDICT("A and B differ in distribution: %g", ks_same);
        tsapi_mi_put(mi, "dataset", 0);
        tsapi_mi_put(mi, "max_stat", ks_same);

        TEST_STEP("...and unlike amounts drawn from [0, 100]");
        te_string_reset(&p);
        tsapi_data_work(&sess, "small.csv", &p);
        CHECK_RC(tapi_data_generate(sess.pco, p.ptr, "csv",
                                    "amount:decimal(10,2):uniform:min=0,"
                                    "max=100;status:string:enum:values=new",
                                    "rows=1000;seed=9", &gen));
        CHECK_RC(tapi_data_open(sess.pco, p.ptr, NULL, NULL,
                                "amount:decimal(10,2),status:string", &g));
        CHECK_RC(tapi_data_compare(a, g, "mode=distribution;"
                                   "columns=amount,status", &d));
        tapi_data_diff_log(&d);
        if (tapi_data_kv_dbl(&d.kv, "max_stat", 0) < 0.5)
            TEST_VERDICT("Different distributions compare close: %g",
                         tapi_data_kv_dbl(&d.kv, "max_stat", 0));
        tsapi_mi_put(mi, "dataset", 1);
        tsapi_mi_put(mi, "max_stat", tapi_data_kv_dbl(&d.kv, "max_stat", 0));
        tsapi_mi_graph(mi, "dist", "Largest KS/TVD statistic: B vs a "
                       "different population", "dataset", "max_stat");
    }
    else
        TEST_FAIL("Unknown mode '%s'", mode);
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_gen_free(&gen);
    tapi_data_diff_free(&d);
    tapi_data_close(g);
    tapi_data_close(c);
    tapi_data_close(b);
    tapi_data_close(a);
    te_string_free(&p);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
