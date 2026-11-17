/** @file
 * @brief Schema group
 *
 * Strict schema validation of orders.csv against its contract, and one
 * known-bad control per kind of violation: each control changes one
 * thing in the expected schema and must produce exactly one finding of
 * the expected kind, with the oracle's count where the finding counts
 * values. A validator that misses a control is broken, and its verdict
 * on the good schema means nothing.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "schema/validation"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

/* Does the report hold a finding "<sev>\t<code>\t<column>..."? */
static te_bool
has_finding(const tapi_data_check *c, const char *code, const char *column)
{
    te_string pat = TE_STRING_INIT;
    te_bool found;

    te_string_append(&pat, "\t%s\t%s\t", code, column);
    found = c->report.ptr != NULL && strstr(c->report.ptr, pat.ptr) != NULL;
    te_string_free(&pat);
    return found;
}

int
main(int argc, char **argv)
{
    tsapi_data_session sess = { 0 };
    tapi_data *ds = NULL;
    tapi_data_check chk = TAPI_DATA_CHECK_INIT;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    te_string sch = TE_STRING_INIT;
    const char *contract;
    unsigned int i;
    struct {
        const char *what;
        const char *schema;     /* replaces the contract, or appends */
        te_bool     append;
        const char *params;
        const char *code;
        const char *column;
        const char *count_key;  /* oracle count in the finding, or NULL */
        int64_t     errors;
    } ctl[] = {
        { "required column absent", ",discount:float!", true, NULL,
          "missing_column", "discount", NULL, 1 },
        { "column not in the contract", NULL, false, NULL,
          "unexpected_column", "note", NULL, 1 },
        { "values of the wrong type", NULL, false, NULL,
          "bad_values", "email", NULL, 1 },
        { "NULLs in a required column", NULL, false, NULL,
          "null_in_required", "email", "orders.null_email", 1 },
    };

    TEST_START;

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_schema_valid"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    contract = tsapi_data_fact(&sess, "orders.schema");
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "orders.csv", &p),
                            NULL, NULL, NULL, &ds));
    mi = tsapi_mi_open();

    TEST_STEP("The contract itself: no finding");
    CHECK_RC(tapi_data_check_schema(ds, contract, NULL, NULL, &chk));
    tapi_data_check_verdict(&chk);
    if (!chk.passed || tapi_data_kv_int(&chk.kv, "errors", -1) != 0 ||
        tapi_data_kv_int(&chk.kv, "rows_bad", -1) != 0)
        TEST_VERDICT("orders.csv does not validate against its contract");
    tsapi_mi_put(mi, "control", 0);
    tsapi_mi_put(mi, "errors", 0);

    for (i = 0; i < TE_ARRAY_LEN(ctl); i++)
    {
        TEST_STEP("Control %u: %s", i + 1, ctl[i].what);
        te_string_reset(&sch);
        if (ctl[i].append)
            te_string_append(&sch, "%s%s", contract, ctl[i].schema);
        else if (i == 1)
            /* The contract without its last column, "note". */
            te_string_append(&sch, "%.*s", (int)(strrchr(contract, ',') -
                                                 contract), contract);
        else if (i == 2)
        {
            te_string_append(&sch, "%s", contract);
            te_string_replace_substring(&sch, "email:string", "email:int");
        }
        else
        {
            te_string_append(&sch, "%s", contract);
            te_string_replace_substring(&sch, "email:string", "email:string!");
        }
        CHECK_RC(tapi_data_check_schema(ds, sch.ptr, ctl[i].params, NULL,
                                        &chk));
        RING("Control '%s':\n%s", ctl[i].what, chk.report.ptr);
        if (chk.passed)
            TEST_VERDICT("Control '%s' passed validation", ctl[i].what);
        if (!has_finding(&chk, ctl[i].code, ctl[i].column))
            TEST_VERDICT("Control '%s' did not report %s on %s", ctl[i].what,
                         ctl[i].code, ctl[i].column);
        if (tapi_data_kv_int(&chk.kv, "errors", -1) != ctl[i].errors)
            TEST_VERDICT("Control '%s': %lld errors, expected %lld",
                         ctl[i].what,
                         (long long)tapi_data_kv_int(&chk.kv, "errors", -1),
                         (long long)ctl[i].errors);
        if (ctl[i].count_key != NULL &&
            tapi_data_kv_int(&chk.kv, "rows_bad", -1) !=
            tsapi_data_fact_int(&sess, ctl[i].count_key))
            TEST_VERDICT("Control '%s': %lld bad rows, the oracle says %lld",
                         ctl[i].what,
                         (long long)tapi_data_kv_int(&chk.kv, "rows_bad", -1),
                         (long long)tsapi_data_fact_int(&sess,
                                                        ctl[i].count_key));
        tsapi_mi_put(mi, "control", i + 1);
        tsapi_mi_put(mi, "errors",
                     (double)tapi_data_kv_int(&chk.kv, "errors", 0));
    }

    TEST_STEP("An extra column is acceptable when allowed");
    te_string_reset(&sch);
    te_string_append(&sch, "%.*s", (int)(strrchr(contract, ',') - contract),
                     contract);
    CHECK_RC(tapi_data_check_schema(ds, sch.ptr, "allow_extra=1", NULL, &chk));
    if (!chk.passed || !has_finding(&chk, "added_column", "note"))
        TEST_VERDICT("allow_extra=1 still rejects the extra column");

    te_mi_logger_add_comment(mi, NULL, "control",
                             "0 contract; 1 missing, 2 unexpected, 3 type, "
                             "4 NULL in required");
    tsapi_mi_graph(mi, "controls", "Error findings per known-bad control",
                   "control", "errors");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_check_free(&chk);
    tapi_data_close(ds);
    te_string_free(&p);
    te_string_free(&sch);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
