/** @file
 * @brief Formats group
 *
 * SQL query results as a dataset. orders.db (written by Python's
 * sqlite3) is read through a query: the column types come from the
 * declared types, the rows must be orders.csv's exactly, a WHERE clause
 * must select the oracle's count, and an aggregate computed by SQLite
 * must equal the same aggregate computed by tsf-data over the CSV.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "formats/sqlite"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

int
main(int argc, char **argv)
{
    tsapi_data_session sess = { 0 };
    tapi_data *csv = NULL;
    tapi_data *sql = NULL;
    tapi_data *paid = NULL;
    tapi_data *grp = NULL;
    tapi_data *agg = NULL;
    tapi_data_digest_info d1;
    tapi_data_digest_info d2;
    tapi_data_counts cnt = { 0 };
    tapi_data_kv res = TAPI_DATA_KV_INIT;
    tapi_data_diff diff = TAPI_DATA_DIFF_INIT;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    te_string db = TE_STRING_INIT;
    te_string out = TE_STRING_INIT;
    static const char *const status[] = { "new", "paid", "shipped",
                                          "cancelled" };
    unsigned int i;

    TEST_START;

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_fmt_sqlite"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    if (!tapi_data_supports(sess.pco, TAPI_DATA_FEAT_SQLITE))
        TEST_SKIP("The agent's engine has no SQLite");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    tsapi_data_path(&sess, "orders.db", &db);

    TEST_STEP("SELECT * carries the rows of orders.csv");
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "orders.csv", &p),
                            NULL, NULL, tsapi_data_fact(&sess, "orders.schema"),
                            &csv));
    CHECK_RC(tapi_data_open(sess.pco, db.ptr, "sqlite",
                            "query=SELECT * FROM orders", NULL, &sql));
    i = (unsigned int)tapi_data_schema_find(&sql->schema, "amount");
    if (strcmp(sql->schema.cols[i].type, "decimal(10,2)") != 0)
        TEST_VERDICT("DECIMAL(10,2) declared column read as %s",
                     sql->schema.cols[i].type);
    CHECK_RC(tapi_data_digest(csv, NULL, &d1));
    CHECK_RC(tapi_data_digest(sql, NULL, &d2));
    if (strcmp(d1.digest, d2.digest) != 0)
        TEST_VERDICT("The query result differs from orders.csv");

    TEST_STEP("A WHERE clause selects the oracle's rows");
    CHECK_RC(tapi_data_open(sess.pco, db.ptr, "sqlite",
                            "query=SELECT order_id, amount FROM orders WHERE "
                            "status = 'paid'", NULL, &paid));
    CHECK_RC(tapi_data_count(paid, NULL, &cnt));
    if (cnt.rows != (uint64_t)tsapi_data_fact_int(&sess, "sql.paid"))
        TEST_VERDICT("WHERE status='paid' gave %llu rows, not %lld",
                     (unsigned long long)cnt.rows,
                     (long long)tsapi_data_fact_int(&sess, "sql.paid"));

    TEST_STEP("SQLite's GROUP BY equals tsf-data's aggregate of the CSV");
    CHECK_RC(tapi_data_open(sess.pco, db.ptr, "sqlite",
                            "query=SELECT status, COUNT(*) AS count, "
                            "SUM(qty) AS sum_qty FROM orders GROUP BY status",
                            NULL, &grp));
    tsapi_data_work(&sess, "agg.csv", &out);
    CHECK_RC(tapi_data_aggregate(csv, out.ptr, "csv",
                                 "group_by=status;aggs=count:*,sum:qty",
                                 &res));
    CHECK_RC(tapi_data_open(sess.pco, out.ptr, NULL, NULL, NULL, &agg));
    CHECK_RC(tapi_data_compare(grp, agg, "mode=key;keys=status", &diff));
    tapi_data_diff_log(&diff);
    if (diff.equal != 1)
        TEST_VERDICT("SQL and tsf-data aggregates differ: %llu mismatched, "
                     "%llu missing, %llu extra",
                     (unsigned long long)diff.mismatched,
                     (unsigned long long)diff.missing,
                     (unsigned long long)diff.extra);

    TEST_STEP("Chart the rows per status, as SQLite and the oracle count");
    mi = tsapi_mi_open();
    for (i = 0; i < TE_ARRAY_LEN(status); i++)
    {
        te_string q = TE_STRING_INIT;
        te_string key = TE_STRING_INIT;
        tapi_data *one = NULL;
        tapi_data_counts c = { 0 };

        te_string_append(&q, "query=SELECT order_id FROM orders WHERE "
                         "status = '%s'", status[i]);
        te_string_append(&key, "status.%s", status[i]);
        CHECK_RC(tapi_data_open(sess.pco, db.ptr, "sqlite", q.ptr, NULL,
                                &one));
        CHECK_RC(tapi_data_count(one, NULL, &c));
        if (c.rows != (uint64_t)tsapi_data_fact_int(&sess, key.ptr))
            TEST_VERDICT("status %s: %llu rows", status[i],
                         (unsigned long long)c.rows);
        tsapi_mi_put(mi, "status", i);
        tsapi_mi_put(mi, "sqlite_rows", (double)c.rows);
        tsapi_mi_put(mi, "oracle_rows",
                     (double)tsapi_data_fact_int(&sess, key.ptr));
        tapi_data_counts_free(&c);
        CHECK_RC(tapi_data_close(one));
        te_string_free(&q);
        te_string_free(&key);
    }
    te_mi_logger_add_comment(mi, NULL, "status",
                             "0 new, 1 paid, 2 shipped, 3 cancelled");
    tsapi_mi_graph(mi, "status", "Orders per status: SQL query vs oracle",
                   "status", "sqlite_rows,oracle_rows");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_diff_free(&diff);
    tapi_data_kv_free(&res);
    tapi_data_counts_free(&cnt);
    tapi_data_close(agg);
    tapi_data_close(grp);
    tapi_data_close(paid);
    tapi_data_close(sql);
    tapi_data_close(csv);
    te_string_free(&p);
    te_string_free(&db);
    te_string_free(&out);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
