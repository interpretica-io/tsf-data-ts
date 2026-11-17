/** @file
 * @brief Formats group
 *
 * Every mandatory format carries the same logical rows. A deterministic
 * synthetic dataset is generated straight into the format under test,
 * read back with the schema it was generated with and with an inferred
 * one, and converted into every other format; the content digest - a
 * hash of the logical rows, independent of the encoding - must be the
 * generator's own everywhere.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "formats/roundtrip"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

/* Every logical type the writers carry, with NULLs and awkward text. */
#define SPEC \
    "id:int:seq;" \
    "flag:bool:bool:p=0.3;" \
    "qty:int:uniform:min=-50,max=50,null=0.05;" \
    "price:decimal(12,3):uniform:min=-1000,max=1000;" \
    "ratio:float:normal:mean=0,stddev=1e6;" \
    "name:string:pattern:pattern=[A-Z][a-z]{6} [A-Z]\\.,boundary=0.05;" \
    "code:string:uuid;" \
    "day:date:time:start=2024-02-27,step=1;" \
    "at:timestamp:time:start=2026-01-01T00:00:00Z,step=1500000,jitter=999;" \
    "kind:string:enum:values=alpha|beta|gamma,weights=1|2|3,null=0.1"

int
main(int argc, char **argv)
{
    static const char *const formats[] = { "csv", "json", "jsonl",
                                           "parquet" };
    tsapi_data_session sess = { 0 };
    const char *format;
    tapi_data_gen gen = { 0 };
    tapi_data *ds = NULL;
    tapi_data *inf = NULL;
    tapi_data_counts cnt = { 0 };
    tapi_data_digest_info dg;
    te_mi_logger *mi = NULL;
    te_string path = TE_STRING_INIT;
    char *schema = NULL;
    unsigned int i;

    TEST_START;
    TEST_GET_STRING_PARAM(format);

    TEST_STEP("Open a session on the agent");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_fmt_roundtrip"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    if (strcmp(format, "parquet") == 0 &&
        !tapi_data_supports(sess.pco, TAPI_DATA_FEAT_PARQUET))
        TEST_SKIP("The agent's engine has no Parquet (Arrow C++)");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    mi = tsapi_mi_open();

    TEST_STEP("Generate 5000 rows directly in %s", format);
    te_string_append(&path, "rt.%s", format);
    {
        te_string p = TE_STRING_INIT;

        tsapi_data_work(&sess, path.ptr, &p);
        te_string_reset(&path);
        te_string_append(&path, "%s", p.ptr);
        te_string_free(&p);
    }
    CHECK_RC(tapi_data_generate(sess.pco, path.ptr, format, SPEC,
                                "rows=5000;seed=11", &gen));
    if (gen.written != 5000)
        TEST_VERDICT("The generator wrote %llu rows, not 5000",
                     (unsigned long long)gen.written);
    schema = tapi_data_unescape(gen.schema);

    TEST_STEP("Read it back with the generated schema");
    CHECK_RC(tapi_data_open(sess.pco, path.ptr, format, NULL, schema, &ds));
    CHECK_RC(tapi_data_count(ds, NULL, &cnt));
    if (cnt.rows != 5000 || cnt.malformed != 0 || cnt.mismatched != 0)
        TEST_VERDICT("%s read back as %llu rows, %llu malformed, %llu "
                     "mismatched", format, (unsigned long long)cnt.rows,
                     (unsigned long long)cnt.malformed,
                     (unsigned long long)cnt.mismatched);
    CHECK_RC(tapi_data_digest(ds, NULL, &dg));
    if (strcmp(dg.digest, gen.digest) != 0)
        TEST_VERDICT("%s does not carry the generated rows: digest %s, "
                     "generator %s", format, dg.digest, gen.digest);
    tapi_data_counts_free(&cnt);

    TEST_STEP("Read it back with an inferred schema");
    CHECK_RC(tapi_data_open(sess.pco, path.ptr, format, NULL, NULL, &inf));
    CHECK_RC(tapi_data_count(inf, NULL, &cnt));
    if (cnt.rows != 5000 || inf->schema.n != 10)
        TEST_VERDICT("Inferred %s: %llu rows over %zu columns", format,
                     (unsigned long long)cnt.rows, inf->schema.n);
    tapi_data_counts_free(&cnt);
    tsapi_mi_put(mi, "format", 0);
    tsapi_mi_put_type(mi, TE_MI_MEAS_MEMORY, "bytes", (double)gen.bytes,
                      TE_MI_MEAS_MULTIPLIER_PLAIN);

    TEST_STEP("Convert into every other format: the rows survive");
    for (i = 0; i < TE_ARRAY_LEN(formats); i++)
    {
        te_string out = TE_STRING_INIT;
        te_string name = TE_STRING_INIT;
        tapi_data_kv res = TAPI_DATA_KV_INIT;
        tapi_data *back = NULL;

        if (strcmp(formats[i], "parquet") == 0 &&
            !tapi_data_supports(sess.pco, TAPI_DATA_FEAT_PARQUET))
            continue;
        te_string_append(&name, "conv.%s", formats[i]);
        tsapi_data_work(&sess, name.ptr, &out);
        CHECK_RC(tapi_data_convert(ds, out.ptr, formats[i], NULL, &res));
        CHECK_RC(tapi_data_open(sess.pco, out.ptr, formats[i], NULL, schema,
                                &back));
        CHECK_RC(tapi_data_digest(back, NULL, &dg));
        RING("%s -> %s: %s bytes, digest %s", format, formats[i],
             tapi_data_kv_get(&res, "bytes", "?"), dg.digest);
        if (strcmp(dg.digest, gen.digest) != 0)
            TEST_VERDICT("%s converted to %s changes the rows", format,
                         formats[i]);
        tsapi_mi_put(mi, "format", i + 1);
        tsapi_mi_put_type(mi, TE_MI_MEAS_MEMORY, "bytes",
                          (double)tapi_data_kv_int(&res, "bytes", 0),
                          TE_MI_MEAS_MULTIPLIER_PLAIN);
        CHECK_RC(tapi_data_close(back));
        tapi_data_kv_free(&res);
        te_string_free(&out);
        te_string_free(&name);
    }
    te_mi_logger_add_comment(mi, NULL, "format",
                             "0 = generated %s; 1 csv, 2 json, 3 jsonl, "
                             "4 parquet", format);
    tsapi_mi_graph_type(mi, "size", "Size of the same 5000 rows by format "
                        "(bytes)", "format", TE_MI_MEAS_MEMORY, "bytes");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_counts_free(&cnt);
    tapi_data_close(inf);
    tapi_data_close(ds);
    tapi_data_gen_free(&gen);
    free(schema);
    te_string_free(&path);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
