/** @file
 * @brief Generate group
 *
 * Deterministic generation. The same spec and seed give the same rows
 * every time - the same content digest and the same file, byte for
 * byte - and the same rows in every format; another seed gives other
 * rows; and each column has its own random stream, so adding a column
 * leaves the others exactly as they were.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "generate/determinism"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

#define SPEC \
    "id:int:seq;u:string:uuid;x:float:normal:mean=10,stddev=3;" \
    "k:int:uniform:min=0,max=9,null=0.2;" \
    "t:timestamp:time:start=2026-03-01T00:00:00Z,step=1000,jitter=500;" \
    "e:string:enum:values=x|y|z,weights=1|1|2,boundary=0.1"

int
main(int argc, char **argv)
{
    tsapi_data_session sess = { 0 };
    tapi_data_gen g1 = { 0 };
    tapi_data_gen g2 = { 0 };
    tapi_data *d = NULL;
    tapi_data_digest_info dg;
    te_mi_logger *mi = NULL;
    te_string p1 = TE_STRING_INIT;
    te_string p2 = TE_STRING_INIT;
    te_string prm = TE_STRING_INIT;
    char first[17];
    unsigned int seed;

    TEST_START;

    TEST_STEP("Open a session");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_gen_det"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    tsapi_data_work(&sess, "a.csv", &p1);
    tsapi_data_work(&sess, "b.csv", &p2);
    mi = tsapi_mi_open();

    TEST_STEP("The same seed twice: the same rows and the same bytes");
    CHECK_RC(tapi_data_generate(sess.pco, p1.ptr, "csv", SPEC,
                                "rows=20000;seed=42", &g1));
    CHECK_RC(tapi_data_generate(sess.pco, p2.ptr, "csv", SPEC,
                                "rows=20000;seed=42", &g2));
    if (strcmp(g1.digest, g2.digest) != 0 || g1.bytes != g2.bytes)
        TEST_VERDICT("Two runs with seed 42 differ");
    CHECK_RC(tsapi_data_sh(&sess, "cmp -s %s %s", p1.ptr, p2.ptr));
    strcpy(first, g1.digest);

    TEST_STEP("The same rows in every format");
    {
        static const char *const fmts[] = { "json", "jsonl", "parquet" };
        unsigned int i;
        char *schema = tapi_data_unescape(g1.schema);

        for (i = 0; i < TE_ARRAY_LEN(fmts); i++)
        {
            te_string p = TE_STRING_INIT;

            if (strcmp(fmts[i], "parquet") == 0 &&
                !tapi_data_supports(sess.pco, TAPI_DATA_FEAT_PARQUET))
                continue;
            te_string_append(&p, "%s/f.%s", sess.work, fmts[i]);
            tapi_data_gen_free(&g2);
            CHECK_RC(tapi_data_generate(sess.pco, p.ptr, fmts[i], SPEC,
                                        "rows=20000;seed=42", &g2));
            CHECK_RC(tapi_data_open(sess.pco, p.ptr, fmts[i], NULL, schema,
                                    &d));
            CHECK_RC(tapi_data_digest(d, NULL, &dg));
            if (strcmp(dg.digest, first) != 0 ||
                strcmp(g2.digest, first) != 0)
                TEST_VERDICT("Seed 42 in %s gives other rows", fmts[i]);
            CHECK_RC(tapi_data_close(d));
            d = NULL;
            te_string_free(&p);
        }
        free(schema);
    }

    TEST_STEP("Other seeds: other rows, never the same digest");
    for (seed = 1; seed <= 8; seed++)
    {
        te_string_reset(&prm);
        te_string_append(&prm, "rows=20000;seed=%u", seed);
        tapi_data_gen_free(&g2);
        CHECK_RC(tapi_data_generate(sess.pco, p2.ptr, "csv", SPEC, prm.ptr,
                                    &g2));
        if (strcmp(g2.digest, first) == 0 && seed != 42)
            TEST_VERDICT("Seed %u reproduces seed 42", seed);
        tsapi_mi_put(mi, "seed", seed);
        tsapi_mi_put_type(mi, TE_MI_MEAS_MEMORY, "bytes", (double)g2.bytes,
                          TE_MI_MEAS_MULTIPLIER_PLAIN);
    }
    tsapi_mi_graph_type(mi, "bytes", "File size per seed (bytes)", "seed",
                        TE_MI_MEAS_MEMORY, "bytes");
    tsapi_mi_close(&mi);

    TEST_STEP("Adding a column leaves the other columns unchanged");
    tapi_data_gen_free(&g2);
    CHECK_RC(tapi_data_generate(sess.pco, p2.ptr, "csv",
                                SPEC ";extra:string:uuid",
                                "rows=20000;seed=42", &g2));
    {
        tapi_data *a = NULL;
        tapi_data *b = NULL;
        tapi_data_digest_info da;
        tapi_data_digest_info db;
        char *sa = tapi_data_unescape(g1.schema);
        char *sb = tapi_data_unescape(g2.schema);
        const char *cols = "columns=id,u,x,k,t,e";

        CHECK_RC(tapi_data_open(sess.pco, p1.ptr, NULL, NULL, sa, &a));
        CHECK_RC(tapi_data_open(sess.pco, p2.ptr, NULL, NULL, sb, &b));
        CHECK_RC(tapi_data_digest(a, cols, &da));
        CHECK_RC(tapi_data_digest(b, cols, &db));
        free(sa);
        free(sb);
        tapi_data_close(a);
        tapi_data_close(b);
        if (strcmp(da.digest, db.digest) != 0)
            TEST_VERDICT("A new column changed the values of the others");
    }

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_close(d);
    tapi_data_gen_free(&g1);
    tapi_data_gen_free(&g2);
    te_string_free(&p1);
    te_string_free(&p2);
    te_string_free(&prm);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
