/** @file
 * @brief Schema group
 *
 * Type coercion is opt-in and reported. orders.jsonl holds qty as JSON
 * integers; read against qty:string they do not fit, and every one is
 * an error - unless coerce=1, when every one is converted and counted as
 * coerced, and the check says so in a warning. Text interpretation is
 * not coercion: CSV text read as int, or a JSON string read as a
 * timestamp, counts nothing.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "schema/coercion"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

int
main(int argc, char **argv)
{
    tsapi_data_session sess = { 0 };
    tapi_data *js = NULL;
    tapi_data *cs = NULL;
    tapi_data *view = NULL;
    tapi_data_check chk = TAPI_DATA_CHECK_INIT;
    tapi_data_counts cnt = { 0 };
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    te_string sch = TE_STRING_INIT;
    int64_t rows;

    TEST_START;

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_schema_coerce"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    rows = tsapi_data_fact_int(&sess, "orders.rows");
    te_string_append(&sch, "%s", tsapi_data_fact(&sess, "orders.schema"));
    te_string_replace_substring(&sch, "qty:int", "qty:string");
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "orders.jsonl",
                                                      &p),
                            NULL, NULL, NULL, &js));
    mi = tsapi_mi_open();

    TEST_STEP("JSON integers against qty:string, no coercion: errors");
    CHECK_RC(tapi_data_check_schema(js, sch.ptr, NULL, NULL, &chk));
    if (chk.passed || tapi_data_kv_int(&chk.kv, "rows_bad", -1) != rows ||
        tapi_data_kv_int(&chk.kv, "coerced", -1) != 0)
        TEST_VERDICT("Without coerce: %lld bad rows, %lld coerced",
                     (long long)tapi_data_kv_int(&chk.kv, "rows_bad", -1),
                     (long long)tapi_data_kv_int(&chk.kv, "coerced", -1));
    tsapi_mi_put(mi, "coerce", 0);
    tsapi_mi_put(mi, "rows_bad", (double)rows);
    tsapi_mi_put(mi, "coerced", 0);

    TEST_STEP("coerce=1: every value converted, counted and reported");
    CHECK_RC(tapi_data_check_schema(js, sch.ptr, "coerce=1", NULL, &chk));
    RING("%s", chk.report.ptr);
    if (!chk.passed || tapi_data_kv_int(&chk.kv, "coerced", -1) != rows ||
        tapi_data_kv_int(&chk.kv, "warnings", -1) != 1 ||
        strstr(chk.report.ptr, "\tcoerced\t") == NULL)
        TEST_VERDICT("With coerce: passed=%d, %lld coerced, %lld warnings",
                     chk.passed,
                     (long long)tapi_data_kv_int(&chk.kv, "coerced", -1),
                     (long long)tapi_data_kv_int(&chk.kv, "warnings", -1));
    tsapi_mi_put(mi, "coerce", 1);
    tsapi_mi_put(mi, "rows_bad", 0);
    tsapi_mi_put(mi, "coerced", (double)rows);

    TEST_STEP("A scan reports coercion in its counts too");
    CHECK_RC(tapi_data_open(sess.pco, js->path, NULL, "coerce=1", sch.ptr,
                            &view));
    CHECK_RC(tapi_data_count(view, NULL, &cnt));
    if ((int64_t)cnt.coerced != rows || cnt.mismatched != 0)
        TEST_VERDICT("The scan coerced %llu and mismatched %llu",
                     (unsigned long long)cnt.coerced,
                     (unsigned long long)cnt.mismatched);
    tapi_data_counts_free(&cnt);

    TEST_STEP("Interpreting text is not coercion");
    te_string_reset(&p);
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "orders.csv", &p),
                            NULL, "coerce=1",
                            tsapi_data_fact(&sess, "orders.schema"), &cs));
    CHECK_RC(tapi_data_count(cs, NULL, &cnt));
    if (cnt.coerced != 0)
        TEST_VERDICT("Reading CSV text as typed values counted %llu "
                     "coercions", (unsigned long long)cnt.coerced);
    tapi_data_counts_free(&cnt);
    CHECK_RC(tapi_data_check_schema(js, tsapi_data_fact(&sess,
                                                        "orders.schema"),
                                    "coerce=1", NULL, &chk));
    if (tapi_data_kv_int(&chk.kv, "coerced", -1) != 0)
        TEST_VERDICT("JSON timestamp strings and numbers into decimal "
                     "counted %lld coercions",
                     (long long)tapi_data_kv_int(&chk.kv, "coerced", -1));

    tsapi_mi_graph(mi, "coerce", "Values not fitting vs values coerced, "
                   "without and with coerce=1", "coerce",
                   "rows_bad,coerced");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_counts_free(&cnt);
    tapi_data_check_free(&chk);
    tapi_data_close(view);
    tapi_data_close(cs);
    tapi_data_close(js);
    te_string_free(&p);
    te_string_free(&sch);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
