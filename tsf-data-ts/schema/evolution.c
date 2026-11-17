/** @file
 * @brief Schema group
 *
 * Backward-compatible schema evolution. Version 1 of a dataset is the
 * contract; later versions change it one way each, written as typed
 * Parquet (or, without Parquet, JSON Lines with explicit schemas) by the
 * generator. In strict mode every change is an error; in compatible mode
 * an added nullable column and a widened type are information, a
 * relaxed nullability is a warning, and a removed required column or an
 * added required one are still errors. The renamed-column CSV shows the
 * same from an untyped source.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "schema/evolution"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

#define V1 "id:int!, amount:int, name:string"

typedef struct version {
    const char *what;
    const char *spec;           /* generator spec of this version */
    int64_t     strict_errors;
    int64_t     compat_errors;
    int64_t     compat_warnings;
    int64_t     compat_infos;
} version;

int
main(int argc, char **argv)
{
    static const version versions[] = {
        { "unchanged",
          "id:int:seq:required=1;amount:int:uniform;name:string:uuid",
          0, 0, 0, 0 },
        { "nullable column added",
          "id:int:seq:required=1;amount:int:uniform;name:string:uuid;"
          "note:string:uuid", 1, 0, 0, 1 },
        { "amount widened to float",
          "id:int:seq:required=1;amount:float:uniform;name:string:uuid",
          1, 0, 0, 1 },
        { "required id relaxed to nullable",
          "id:int:seq;amount:int:uniform;name:string:uuid", 1, 0, 1, 0 },
        { "required id removed",
          "amount:int:uniform;name:string:uuid", 1, 1, 0, 0 },
        { "required column added",
          "id:int:seq:required=1;amount:int:uniform;name:string:uuid;"
          "rev:int:seq:required=1", 1, 1, 0, 0 },
    };
    tsapi_data_session sess = { 0 };
    tapi_data *ds = NULL;
    tapi_data_check chk = TAPI_DATA_CHECK_INIT;
    tapi_data_gen gen = { 0 };
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    const char *fmt;
    unsigned int i;

    TEST_START;

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_schema_evol"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    /* A typed source declares types and nullability. */
    fmt = tapi_data_supports(sess.pco, TAPI_DATA_FEAT_PARQUET) ? "parquet" :
          "jsonl";
    mi = tsapi_mi_open();

    for (i = 0; i < TE_ARRAY_LEN(versions); i++)
    {
        const version *v = &versions[i];
        char *schema;

        TEST_STEP("Version %u: %s", i + 2, v->what);
        te_string_reset(&p);
        te_string_append(&p, "%s/v%u.%s", sess.work, i, fmt);
        CHECK_RC(tapi_data_generate(sess.pco, p.ptr, fmt, v->spec,
                                    "rows=200;seed=5", &gen));
        schema = tapi_data_unescape(gen.schema);
        /* JSON Lines carries no schema: give it the generated one. */
        CHECK_RC(tapi_data_open(sess.pco, p.ptr, fmt, NULL,
                                strcmp(fmt, "jsonl") == 0 ? schema : NULL,
                                &ds));
        free(schema);
        tapi_data_gen_free(&gen);

        CHECK_RC(tapi_data_check_schema(ds, V1, "mode=strict", NULL, &chk));
        RING("strict:\n%s", chk.report.ptr);
        if (tapi_data_kv_int(&chk.kv, "errors", -1) != v->strict_errors)
            TEST_VERDICT("'%s' strict: %lld errors, expected %lld", v->what,
                         (long long)tapi_data_kv_int(&chk.kv, "errors", -1),
                         (long long)v->strict_errors);
        tsapi_mi_put(mi, "version", i + 2);
        tsapi_mi_put(mi, "strict_errors",
                     (double)tapi_data_kv_int(&chk.kv, "errors", 0));

        CHECK_RC(tapi_data_check_schema(ds, V1, "mode=compatible", NULL,
                                        &chk));
        RING("compatible:\n%s", chk.report.ptr);
        if (tapi_data_kv_int(&chk.kv, "errors", -1) != v->compat_errors ||
            tapi_data_kv_int(&chk.kv, "warnings", -1) != v->compat_warnings ||
            tapi_data_kv_int(&chk.kv, "infos", -1) != v->compat_infos)
            TEST_VERDICT("'%s' compatible: %lld/%lld/%lld errors/warnings/"
                         "infos, expected %lld/%lld/%lld", v->what,
                         (long long)tapi_data_kv_int(&chk.kv, "errors", -1),
                         (long long)tapi_data_kv_int(&chk.kv, "warnings", -1),
                         (long long)tapi_data_kv_int(&chk.kv, "infos", -1),
                         (long long)v->compat_errors,
                         (long long)v->compat_warnings,
                         (long long)v->compat_infos);
        tsapi_mi_put(mi, "compatible_errors",
                     (double)tapi_data_kv_int(&chk.kv, "errors", 0));
        CHECK_RC(tapi_data_close(ds));
        ds = NULL;
    }

    TEST_STEP("An untyped CSV that renamed qty and added a column");
    te_string_reset(&p);
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "orders_c.csv",
                                                      &p),
                            NULL, NULL, NULL, &ds));
    CHECK_RC(tapi_data_check_schema(ds, tsapi_data_fact(&sess, "orders.schema"),
                                    "mode=compatible", NULL, &chk));
    RING("orders_c.csv against v1:\n%s", chk.report.ptr);
    /* qty is nullable in the contract, so its absence is a warning;
     * quantity and channel are additions. */
    if (tapi_data_kv_int(&chk.kv, "errors", -1) != 0 ||
        tapi_data_kv_int(&chk.kv, "warnings", -1) != 1 ||
        tapi_data_kv_int(&chk.kv, "infos", -1) != 2)
        TEST_VERDICT("The renamed-column CSV is judged %lld/%lld/%lld",
                     (long long)tapi_data_kv_int(&chk.kv, "errors", -1),
                     (long long)tapi_data_kv_int(&chk.kv, "warnings", -1),
                     (long long)tapi_data_kv_int(&chk.kv, "infos", -1));

    tsapi_mi_graph(mi, "evolution", "Schema errors per version: strict vs "
                   "backward-compatible", "version",
                   "strict_errors,compatible_errors");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_gen_free(&gen);
    tapi_data_check_free(&chk);
    tapi_data_close(ds);
    te_string_free(&p);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
