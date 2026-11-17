/** @file
 * @brief Formats group
 *
 * Schema inference and explicit schemas. typed.csv has one column per
 * logical type (and a nullable one, and one whose only non-integer is
 * the last row); the inferred types must be the generator's. An
 * explicit schema overrides inference. Then the sample size: inferring
 * from fewer rows than reach the odd value types that column int, and
 * the scan reports the one row that does not fit - charted against the
 * number of rows inferred from.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "formats/inference"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

/* "a:int,b:string" of a schema as read back. */
static void
schema_text(const tapi_data_schema_info *s, te_string *out)
{
    size_t i;

    for (i = 0; i < s->n; i++)
        te_string_append(out, "%s%s:%s", i > 0 ? "," : "", s->cols[i].name,
                         s->cols[i].type);
}

int
main(int argc, char **argv)
{
    static const unsigned int samples[] = { 1, 10, 40, 49, 50, 1000 };
    tsapi_data_session sess = { 0 };
    tapi_data *ds = NULL;
    tapi_data_counts cnt = { 0 };
    te_mi_logger *mi = NULL;
    te_string path = TE_STRING_INIT;
    te_string got = TE_STRING_INIT;
    unsigned int i;
    int k;

    TEST_START;

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_fmt_infer"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    tsapi_data_path(&sess, "typed.csv", &path);

    TEST_STEP("Infer typed.csv: one column per logical type");
    CHECK_RC(tapi_data_open(sess.pco, path.ptr, NULL, NULL, NULL, &ds));
    schema_text(&ds->schema, &got);
    RING("Inferred: %s", got.ptr);
    if (strcmp(got.ptr, tsapi_data_fact(&sess, "typed.expect")) != 0)
        TEST_VERDICT("Inferred schema '%s' is not '%s'", got.ptr,
                     tsapi_data_fact(&sess, "typed.expect"));
    if (!ds->schema.inferred)
        TEST_VERDICT("The schema is not reported as inferred");
    k = tapi_data_schema_find(&ds->schema, "n");
    if (k < 0 || !ds->schema.cols[k].nullable)
        TEST_VERDICT("Column 'n' has blanks but is not nullable");
    k = tapi_data_schema_find(&ds->schema, "i");
    if (k < 0 || ds->schema.cols[k].nullable)
        TEST_VERDICT("Column 'i' has no blanks but is nullable");
    CHECK_RC(tapi_data_close(ds));
    ds = NULL;

    TEST_STEP("An explicit schema overrides inference");
    CHECK_RC(tapi_data_open(sess.pco, path.ptr, NULL, NULL,
                            "b:string, i:string, f:float, d:date, "
                            "t:timestamp, s:string, n:int, mixed:string",
                            &ds));
    if (ds->schema.inferred || strcmp(ds->schema.cols[1].type, "string") != 0)
        TEST_VERDICT("The explicit schema was not used");
    CHECK_RC(tapi_data_count(ds, NULL, &cnt));
    if (cnt.rows != 50 || cnt.mismatched != 0)
        TEST_VERDICT("Explicit schema: %llu rows, %llu mismatched",
                     (unsigned long long)cnt.rows,
                     (unsigned long long)cnt.mismatched);
    tapi_data_counts_free(&cnt);
    CHECK_RC(tapi_data_close(ds));
    ds = NULL;

    TEST_STEP("JSON Lines: types come from the JSON values");
    te_string_reset(&path);
    tsapi_data_path(&sess, "orders.jsonl", &path);
    CHECK_RC(tapi_data_open(sess.pco, path.ptr, NULL, NULL, NULL, &ds));
    te_string_reset(&got);
    schema_text(&ds->schema, &got);
    RING("orders.jsonl inferred: %s", got.ptr);
    if (strstr(got.ptr, "order_id:int") == NULL ||
        strstr(got.ptr, "amount:float") == NULL ||
        strstr(got.ptr, "created:timestamp") == NULL ||
        strstr(got.ptr, "ship_date:date") == NULL ||
        strstr(got.ptr, "email:string") == NULL)
        TEST_VERDICT("orders.jsonl inferred as '%s'", got.ptr);
    k = tapi_data_schema_find(&ds->schema, "email");
    if (k < 0 || !ds->schema.cols[k].nullable)
        TEST_VERDICT("email holds JSON nulls but is not nullable");
    CHECK_RC(tapi_data_close(ds));
    ds = NULL;

    TEST_STEP("Sample size: too small a sample mis-types 'mixed'");
    te_string_reset(&path);
    tsapi_data_path(&sess, "typed.csv", &path);
    mi = tsapi_mi_open();
    for (i = 0; i < TE_ARRAY_LEN(samples); i++)
    {
        te_string p = TE_STRING_INIT;
        tapi_data_schema_info s = { 0 };
        te_bool as_int;

        te_string_append(&p, "infer_rows=%u;on_error=count", samples[i]);
        CHECK_RC(tapi_data_open(sess.pco, path.ptr, NULL, p.ptr, NULL, &ds));
        k = tapi_data_schema_find(&ds->schema, "mixed");
        as_int = k >= 0 && strcmp(ds->schema.cols[k].type, "int") == 0;
        CHECK_RC(tapi_data_count(ds, NULL, &cnt));
        RING("infer_rows=%u: mixed is %s, %llu row(s) do not fit",
             samples[i], k >= 0 ? ds->schema.cols[k].type : "?",
             (unsigned long long)cnt.mismatched);
        /* Row 50 holds the only "x": a sample that stops before it types
         * the column int, and then exactly that row does not fit. */
        if (as_int != (samples[i] < 50))
            TEST_VERDICT("infer_rows=%u typed 'mixed' as %s", samples[i],
                         ds->schema.cols[k].type);
        if (cnt.mismatched != (as_int ? 1 : 0))
            TEST_VERDICT("infer_rows=%u: %llu mismatched rows", samples[i],
                         (unsigned long long)cnt.mismatched);
        tsapi_mi_put(mi, "infer_rows", samples[i]);
        tsapi_mi_put(mi, "mismatched", (double)cnt.mismatched);
        tsapi_mi_put(mi, "columns_int", as_int ? 1 : 0);
        tapi_data_counts_free(&cnt);
        CHECK_RC(tapi_data_schema(ds, &s));
        if (s.n != 8)
            TEST_VERDICT("The schema read back has %zu columns", s.n);
        tapi_data_schema_free(&s);
        CHECK_RC(tapi_data_close(ds));
        ds = NULL;
        te_string_free(&p);
    }
    tsapi_mi_graph(mi, "sample", "Rows not fitting the inferred type vs "
                   "rows inferred from", "infer_rows",
                   "mismatched,columns_int");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_counts_free(&cnt);
    tapi_data_close(ds);
    te_string_free(&path);
    te_string_free(&got);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
