/** @file
 * @brief Schema group
 *
 * Nested structures. nested.jsonl holds arrays and objects; two records
 * lack a required nested field and one has an array element of the
 * wrong type. A nested schema must find exactly those three records and
 * name the column of each; relaxing the nested field to optional leaves
 * only the array element; typing the array of strings as array<int>
 * fails every record.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "schema/nested"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

int
main(int argc, char **argv)
{
    static const struct {
        const char *what;
        const char *schema;
        int64_t     rows_bad;   /* -1: the oracle's nested.bad_rows */
    } cases[] = {
        { "the contract", NULL, -1 },
        { "zip optional",
          "id:int!,tags:array<string>,addr:object{city:string,zip:string},"
          "scores:array<int>", 1 },
        { "tags mistyped",
          "id:int!,tags:array<int>,addr:object{city:string,zip:string},"
          "scores:array<int>", 100 },
        { "scores as floats",
          "id:int!,tags:array<string>,addr:object{city:string,zip:string},"
          "scores:array<float>", 1 },
    };
    tsapi_data_session sess = { 0 };
    tapi_data *ds = NULL;
    tapi_data_check chk = TAPI_DATA_CHECK_INIT;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    unsigned int i;

    TEST_START;

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_schema_nested"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "nested.jsonl",
                                                      &p),
                            NULL, NULL, NULL, &ds));
    i = (unsigned int)tapi_data_schema_find(&ds->schema, "addr");
    if (strcmp(ds->schema.cols[i].type, "object") != 0)
        TEST_VERDICT("A JSON object column is inferred as %s",
                     ds->schema.cols[i].type);
    mi = tsapi_mi_open();

    for (i = 0; i < TE_ARRAY_LEN(cases); i++)
    {
        const char *schema = cases[i].schema != NULL ? cases[i].schema :
                             tsapi_data_fact(&sess, "nested.schema");
        int64_t want = cases[i].rows_bad >= 0 ? cases[i].rows_bad :
                       tsapi_data_fact_int(&sess, "nested.bad_rows");

        TEST_STEP("Validate against %s", cases[i].what);
        CHECK_RC(tapi_data_check_schema(ds, schema, NULL, NULL, &chk));
        RING("%s:\n%s", cases[i].what, chk.report.ptr);
        if (tapi_data_kv_int(&chk.kv, "rows_bad", -1) != want)
            TEST_VERDICT("Against %s: %lld bad records, expected %lld",
                         cases[i].what,
                         (long long)tapi_data_kv_int(&chk.kv, "rows_bad", -1),
                         (long long)want);
        if (i == 0 && (strstr(chk.report.ptr, "\taddr\t") == NULL ||
                       strstr(chk.report.ptr, "\tscores\t") == NULL))
            TEST_VERDICT("The findings do not name the nested columns");
        tsapi_mi_put(mi, "case", i);
        tsapi_mi_put(mi, "rows_bad", (double)want);
        tsapi_mi_put(mi, "errors",
                     (double)tapi_data_kv_int(&chk.kv, "errors", 0));
    }
    te_mi_logger_add_comment(mi, NULL, "case", "0 contract, 1 zip optional, "
                             "2 tags mistyped, 3 scores as floats");
    tsapi_mi_graph(mi, "nested", "Records failing each nested schema",
                   "case", "rows_bad,errors");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_check_free(&chk);
    tapi_data_close(ds);
    te_string_free(&p);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
