/** @file
 * @brief Formats group
 *
 * Parquet against a second implementation. orders.parquet is written by
 * pyarrow with the physical types a real producer picks - int8/int32,
 * a dictionary-encoded string, decimal128, millisecond timestamps with
 * a zone, large_string, several row groups - and must carry exactly the
 * rows of orders.csv. nested.parquet adds lists, a struct, float32,
 * uint32 past 2^31, nanosecond timestamps, binary and booleans. And the
 * other way round: a file tsf-data writes is read by pyarrow, whose
 * count and sum must match.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "formats/interop"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"
#include "tapi_file.h"

#include "tapi_data.h"
#include "tsapi_data.h"

int
main(int argc, char **argv)
{
    tsapi_data_session sess = { 0 };
    tapi_data *csv = NULL;
    tapi_data *pq = NULL;
    tapi_data *nested = NULL;
    tapi_data_digest_info d_csv;
    tapi_data_digest_info d_pq;
    tapi_data_profile_info prof = { 0 };
    tapi_data_check chk = TAPI_DATA_CHECK_INIT;
    tapi_data_kv conv = TAPI_DATA_KV_INIT;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    te_string out = TE_STRING_INIT;
    char *back = NULL;
    const tapi_data_col_profile *col;
    int k;

    TEST_START;

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_fmt_interop"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    if (!tapi_data_supports(sess.pco, TAPI_DATA_FEAT_PARQUET))
        TEST_SKIP("The agent's engine has no Parquet (Arrow C++)");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    if (!tsapi_data_have_pyarrow(&sess))
        TEST_SKIP("The agent has no pyarrow to write the reference files");
    mi = tsapi_mi_open();

    TEST_STEP("pyarrow's orders.parquet carries the rows of orders.csv");
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "orders.csv", &p),
                            NULL, NULL, tsapi_data_fact(&sess, "orders.schema"),
                            &csv));
    te_string_reset(&p);
    CHECK_RC(tapi_data_open(sess.pco,
                            tsapi_data_path(&sess, "orders.parquet", &p),
                            NULL, NULL, NULL, &pq));
    for (k = 0; k < (int)pq->schema.n; k++)
        RING("parquet column %s: %s%s", pq->schema.cols[k].name,
             pq->schema.cols[k].type,
             pq->schema.cols[k].nullable ? "" : " (required)");
    k = tapi_data_schema_find(&pq->schema, "amount");
    if (k < 0 || strcmp(pq->schema.cols[k].type, "decimal(10,2)") != 0)
        TEST_VERDICT("decimal128(10,2) did not map to decimal(10,2)");
    k = tapi_data_schema_find(&pq->schema, "status");
    if (k < 0 || strcmp(pq->schema.cols[k].type, "string") != 0)
        TEST_VERDICT("A dictionary-encoded string did not map to string");
    CHECK_RC(tapi_data_digest(csv, NULL, &d_csv));
    CHECK_RC(tapi_data_digest(pq, NULL, &d_pq));
    RING("csv digest %s, parquet digest %s (%llu rows)", d_csv.digest,
         d_pq.digest, (unsigned long long)d_pq.rows);
    if (d_pq.rows != (uint64_t)tsapi_data_fact_int(&sess, "orders.rows"))
        TEST_VERDICT("orders.parquet has %llu rows",
                     (unsigned long long)d_pq.rows);
    if (strcmp(d_csv.digest, d_pq.digest) != 0)
        TEST_VERDICT("The rows pyarrow wrote differ from the CSV's");
    tsapi_mi_put(mi, "file", 1);
    tsapi_mi_put(mi, "rows", (double)d_pq.rows);

    TEST_STEP("nested.parquet: lists, struct, uint32, ns timestamps");
    te_string_reset(&p);
    CHECK_RC(tapi_data_open(sess.pco,
                            tsapi_data_path(&sess, "nested.parquet", &p),
                            NULL, NULL, NULL, &nested));
    for (k = 0; k < (int)nested->schema.n; k++)
        RING("nested column %s: %s", nested->schema.cols[k].name,
             nested->schema.cols[k].type);
    k = tapi_data_schema_find(&nested->schema, "tags");
    if (k < 0 || strcmp(nested->schema.cols[k].type, "array<string>") != 0)
        TEST_VERDICT("list<string> did not map to array<string>");
    CHECK_RC(tapi_data_profile(nested, "columns=u32,f32,flag", &prof));
    tapi_data_profile_log(&prof);
    col = tapi_data_profile_col(&prof, "u32");
    if (col == NULL || !tsapi_data_near(col->sum,
            tsapi_data_fact_dbl(&sess, "nested_pq.u32_sum"), 1e-12))
        TEST_VERDICT("uint32 values past 2^31 were not read exactly");
    col = tapi_data_profile_col(&prof, "f32");
    if (col == NULL || col->nulls != 1 || !tsapi_data_near(col->sum, 9.0,
                                                           1e-12))
        TEST_VERDICT("float32 column: wrong sum or NULLs");
    tsapi_mi_put(mi, "file", 2);
    tsapi_mi_put(mi, "rows", (double)prof.rows);

    TEST_STEP("The struct's NULL zip is caught by a nested schema");
    CHECK_RC(tapi_data_check_schema(nested,
                                    "id:int, tags:array<string>, "
                                    "addr:object{city:string,zip:string!}, "
                                    "f32:float, u32:int, ns:timestamp, "
                                    "bin:binary, flag:bool",
                                    "allow_extra=1", NULL, &chk));
    if (tapi_data_kv_int(&chk.kv, "rows_bad", -1) != 1)
        TEST_VERDICT("A struct with a NULL required field: %lld bad rows",
                     (long long)tapi_data_kv_int(&chk.kv, "rows_bad", -1));

    TEST_STEP("pyarrow reads what tsf-data writes");
    tsapi_data_work(&sess, "ours.parquet", &out);
    CHECK_RC(tapi_data_convert(csv, out.ptr, "parquet", NULL, &conv));
    CHECK_RC(tsapi_data_sh(&sess, "python3 -c \"import pyarrow.parquet as pq;"
                           "t=pq.read_table('%s');"
                           "print('%%d %%s %%d' %% (t.num_rows, "
                           "sum(x for x in t.column('amount').to_pylist() "
                           "if x is not None), t.num_row_groups))\" > %s.txt",
                           out.ptr, out.ptr));
    te_string_append(&out, ".txt");
    CHECK_RC(tapi_file_read_ta(sess.ta, out.ptr, &back));
    RING("pyarrow on our file: %s", back);
    {
        unsigned long long rows = 0;
        char sum[64] = "";
        unsigned int groups = 0;

        if (sscanf(back, "%llu %63s %u", &rows, sum, &groups) != 3 ||
            rows != (unsigned long long)tsapi_data_fact_int(&sess,
                                                            "orders.rows") ||
            !tsapi_data_near(strtod(sum, NULL),
                             tsapi_data_fact_dbl(&sess, "amount.sum"), 1e-12))
            TEST_VERDICT("pyarrow does not read back the rows tsf-data "
                         "wrote");
        tsapi_mi_put(mi, "file", 3);
        tsapi_mi_put(mi, "rows", (double)rows);
    }
    tsapi_mi_graph(mi, "rows", "Rows read across implementations (1 pyarrow"
                   "->tsf-data, 2 nested, 3 tsf-data->pyarrow)", "file",
                   "rows");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    free(back);
    tapi_data_kv_free(&conv);
    tapi_data_check_free(&chk);
    tapi_data_profile_free(&prof);
    tapi_data_close(nested);
    tapi_data_close(pq);
    tapi_data_close(csv);
    te_string_free(&p);
    te_string_free(&out);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
