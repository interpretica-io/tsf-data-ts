/** @file
 * @brief Formats group
 *
 * Corrupted and truncated inputs. Each case is a file with a known
 * defect; tsf-data must refuse it with a reason that says where
 * (on_error=fail, the default), or - when the format allows reading
 * past the damage - count exactly the damaged records and carry on
 * (on_error=count). A file that cannot be a dataset at all (an empty
 * CSV, a Parquet file cut before its footer, random bytes) is refused at
 * open. Nothing may stay open on the agent afterwards.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "formats/corrupted"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

typedef struct corrupt_case {
    const char *name;
    const char *file;
    te_bool     parquet;
    te_errno    open_err;       /* expected at open, 0 = opens */
    int64_t     good;           /* rows on_error=count delivers, -1 = any */
    int64_t     bad;            /* malformed records counted */
} corrupt_case;

int
main(int argc, char **argv)
{
    static const corrupt_case cases[] = {
        { "trunc_quote_csv", "trunc_quote.csv", false, 0, 1, 1 },
        { "bad_fields_csv", "bad_fields.csv", false, 0, 3, 2 },
        { "trunc_json", "trunc.json", false, 0, -1, 1 },
        { "trunc_jsonl", "trunc.jsonl", false, 0, -2, 1 },
        { "trunc_parquet", "trunc.parquet", true, TE_ECORRUPTED, 0, 0 },
        { "garbage_parquet", "garbage.parquet", true, TE_ECORRUPTED, 0, 0 },
        { "empty_csv", "empty.csv", false, TE_ECORRUPTED, 0, 0 },
        { "missing_file", "no-such-file.csv", false, TE_ENOENT, 0, 0 },
    };
    tsapi_data_session sess = { 0 };
    const char *kind;
    const corrupt_case *c = NULL;
    tapi_data *ds = NULL;
    tapi_data_counts cnt = { 0 };
    tapi_data_stats st;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    unsigned int i;

    TEST_START;
    TEST_GET_STRING_PARAM(kind);
    for (i = 0; i < TE_ARRAY_LEN(cases); i++)
    {
        if (strcmp(cases[i].name, kind) == 0)
            c = &cases[i];
    }
    if (c == NULL)
        TEST_FAIL("Unknown case '%s'", kind);

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_fmt_corrupt"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    if (c->parquet && !tapi_data_supports(sess.pco, TAPI_DATA_FEAT_PARQUET))
        TEST_SKIP("The agent's engine has no Parquet (Arrow C++)");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    if (c->parquet && !tsapi_data_have_pyarrow(&sess))
        TEST_SKIP("The agent has no pyarrow to write the Parquet fixture");
    tsapi_data_path(&sess, c->file, &p);
    mi = tsapi_mi_open();

    TEST_STEP("Open %s", c->file);
    rc = tapi_data_open(sess.pco, p.ptr, NULL, NULL, NULL, &ds);
    if (c->open_err != 0)
    {
        if (TE_RC_GET_ERROR(rc) != (unsigned int)c->open_err)
            TEST_VERDICT("Opening %s gave %r, expected %r", c->file, rc,
                         c->open_err);
        if (*tapi_data_last_error() == '\0')
            TEST_VERDICT("The refusal carries no reason");
        RING("Refused as expected: %s", tapi_data_last_error());
        goto check_leaks;
    }
    CHECK_RC(rc);

    TEST_STEP("on_error=fail: the damage is an error that names the record");
    rc = tapi_data_count(ds, NULL, &cnt);
    if (TE_RC_GET_ERROR(rc) != TE_EILSEQ)
        TEST_VERDICT("Counting %s with on_error=fail gave %r, not "
                     "TE_EILSEQ", c->file, rc);
    if (strstr(tapi_data_last_error(), "record") == NULL &&
        strstr(tapi_data_last_error(), "line") == NULL)
        TEST_VERDICT("The reason does not say which record: '%s'",
                     tapi_data_last_error());
    RING("Failed as expected: %s", tapi_data_last_error());
    tapi_data_counts_free(&cnt);

    TEST_STEP("on_error=count: the damaged records are counted, not hidden");
    CHECK_RC(tapi_data_count(ds, "on_error=count", &cnt));
    if ((int64_t)cnt.malformed != c->bad)
        TEST_VERDICT("%s: %llu malformed records counted, expected %lld",
                     c->file, (unsigned long long)cnt.malformed,
                     (long long)c->bad);
    if (c->good >= 0 && (int64_t)cnt.rows != c->good)
        TEST_VERDICT("%s: %llu good rows, expected %lld", c->file,
                     (unsigned long long)cnt.rows, (long long)c->good);
    if (c->good == -2 &&
        cnt.rows != (uint64_t)tsapi_data_fact_int(&sess, "orders.rows") - 1)
        TEST_VERDICT("%s: %llu good rows, expected all but the cut one",
                     c->file, (unsigned long long)cnt.rows);
    if (c->good == -1 &&
        (cnt.rows == 0 ||
         cnt.rows >= (uint64_t)tsapi_data_fact_int(&sess, "orders.rows")))
        TEST_VERDICT("%s: %llu good rows from half a file", c->file,
                     (unsigned long long)cnt.rows);
    if (cnt.first_error == NULL)
        TEST_VERDICT("The count has no first_error for the damage");
    tsapi_mi_put(mi, "good_rows", (double)cnt.rows);
    tsapi_mi_put(mi, "malformed", (double)cnt.malformed);
    CHECK_RC(tapi_data_close(ds));
    ds = NULL;

check_leaks:
    TEST_STEP("Nothing stays open on the agent");
    CHECK_RC(tapi_data_get_stats(sess.pco, &st));
    if (st.open != 0)
        TEST_VERDICT("%u dataset(s) left open after the failure", st.open);
    tsapi_mi_put(mi, "open_after", st.open);
    tsapi_mi_graph(mi, "damage", "Rows kept and records rejected",
                   NULL, "good_rows,malformed,open_after");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_counts_free(&cnt);
    tapi_data_close(ds);
    te_string_free(&p);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
