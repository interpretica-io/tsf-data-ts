/** @file
 * @brief Robust group
 *
 * Resource cleanup after failure. Operations that spill to disk leave
 * nothing in their temporary directory whether they succeed or fail
 * half-way; datasets whose open fails leave no handle behind; a
 * hundred datasets opened and closed leave the RPC server as it was;
 * and a failed write leaves no partial output. Charted: datasets open
 * and spill files left over across the sequence.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "robust/cleanup"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

int
main(int argc, char **argv)
{
    tsapi_data_session sess = { 0 };
    tapi_data *a = NULL;
    tapi_data *bad = NULL;
    tapi_data *many[100];
    tapi_data_diff d = TAPI_DATA_DIFF_INIT;
    tapi_data_check chk = TAPI_DATA_CHECK_INIT;
    tapi_data_kv res = TAPI_DATA_KV_INIT;
    tapi_data_stats st;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    te_string spill = TE_STRING_INIT;
    te_string prm = TE_STRING_INIT;
    unsigned int step = 0;
    unsigned int i;

    memset(many, 0, sizeof(many));
    TEST_START;

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_robust_cleanup"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    tsapi_data_work(&sess, "spill", &spill);
    CHECK_RC(tsapi_data_sh(&sess, "mkdir -p %s", spill.ptr));
    te_string_append(&prm, "mem_limit=16k;tmpdir=%s", spill.ptr);
    CHECK_RC(tapi_data_open(sess.pco,
                            tsapi_data_path(&sess, "orders_base.csv", &p),
                            NULL, NULL, tsapi_data_fact(&sess,
                                                        "orders.schema"),
                            &a));
    mi = tsapi_mi_open();

#define LEFTOVERS(_what) \
    do {                                                                \
        CHECK_RC(tapi_data_get_stats(sess.pco, &st));                   \
        rc = tsapi_data_sh(&sess, "test -z \"$(ls -A %s)\"", spill.ptr);\
        if (rc != 0)                                                    \
            TEST_VERDICT("%s left files in the spill directory", _what);\
        tsapi_mi_put(mi, "step", step++);                               \
        tsapi_mi_put(mi, "open_datasets", st.open);                     \
    } while (0)

    TEST_STEP("A partitioned comparison that succeeds");
    {
        te_string cp = TE_STRING_INIT;

        te_string_append(&cp, "mode=unordered;%s", prm.ptr);
        CHECK_RC(tapi_data_compare(a, a, cp.ptr, &d));
        te_string_free(&cp);
    }
    if (d.equal != 1 || d.partitions < 2)
        TEST_VERDICT("The comparison did not partition or not succeed");
    LEFTOVERS("A successful comparison");

    TEST_STEP("A partitioned comparison that fails half-way");
    te_string_reset(&p);
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "trunc.jsonl",
                                                      &p),
                            NULL, NULL, tsapi_data_fact(&sess,
                                                        "orders.schema"),
                            &bad));
    {
        te_string cp = TE_STRING_INIT;

        te_string_append(&cp, "mode=key;keys=order_id;%s", prm.ptr);
        rc = tapi_data_compare(a, bad, cp.ptr, &d);
        te_string_free(&cp);
    }
    if (TE_RC_GET_ERROR(rc) != TE_EILSEQ)
        TEST_VERDICT("Comparing with a truncated dataset gave %r", rc);
    LEFTOVERS("A failed comparison");
    if (st.open != 2)
        TEST_VERDICT("%u datasets open after the failure, expected 2",
                     st.open);

    TEST_STEP("A uniqueness check that fails half-way");
    {
        te_string cp = TE_STRING_INIT;

        te_string_append(&cp, "%s", prm.ptr);
        rc = tapi_data_check_unique(bad, "order_id", cp.ptr, NULL, &chk);
        te_string_free(&cp);
    }
    if (TE_RC_GET_ERROR(rc) != TE_EILSEQ)
        TEST_VERDICT("unique over a truncated dataset gave %r", rc);
    LEFTOVERS("A failed uniqueness check");

    TEST_STEP("A write into a missing directory leaves nothing");
    te_string_reset(&p);
    tsapi_data_work(&sess, "no/such/dir/out.csv", &p);
    rc = tapi_data_convert(a, p.ptr, "csv", NULL, &res);
    if (rc == 0)
        TEST_VERDICT("Writing into a missing directory succeeded");
    if (tsapi_data_sh(&sess, "test ! -e %s", p.ptr) != 0)
        TEST_VERDICT("A failed write left a partial file");
    LEFTOVERS("A failed write");

    TEST_STEP("Failed opens leave no handle");
    for (i = 0; i < 10; i++)
    {
        tapi_data *x = NULL;

        te_string_reset(&p);
        rc = tapi_data_open(sess.pco,
                            tsapi_data_path(&sess, i % 2 ? "empty.csv" :
                                            "missing.csv", &p),
                            NULL, NULL, NULL, &x);
        if (rc == 0 || x != NULL)
            TEST_VERDICT("A broken dataset opened");
    }
    LEFTOVERS("Failed opens");
    if (st.open != 2)
        TEST_VERDICT("%u datasets open after failed opens", st.open);

    TEST_STEP("A hundred datasets opened and closed");
    for (i = 0; i < TE_ARRAY_LEN(many); i++)
    {
        te_string_reset(&p);
        CHECK_RC(tapi_data_open(sess.pco,
                                tsapi_data_path(&sess, "customers.csv", &p),
                                NULL, NULL, NULL, &many[i]));
    }
    LEFTOVERS("A hundred open datasets");
    if (st.open != 102)
        TEST_VERDICT("%u datasets open, expected 102", st.open);
    for (i = 0; i < TE_ARRAY_LEN(many); i++)
    {
        CHECK_RC(tapi_data_close(many[i]));
        many[i] = NULL;
    }
    CHECK_RC(tapi_data_close(bad));
    bad = NULL;
    CHECK_RC(tapi_data_close(a));
    a = NULL;
    LEFTOVERS("Closing everything");
    if (st.open != 0)
        TEST_VERDICT("%u datasets still open after closing all", st.open);
#undef LEFTOVERS

    tsapi_mi_graph(mi, "open", "Datasets open on the agent across the "
                   "sequence", "step", "open_datasets");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    for (i = 0; i < TE_ARRAY_LEN(many); i++)
        tapi_data_close(many[i]);
    tapi_data_kv_free(&res);
    tapi_data_check_free(&chk);
    tapi_data_diff_free(&d);
    tapi_data_close(bad);
    tapi_data_close(a);
    te_string_free(&p);
    te_string_free(&spill);
    te_string_free(&prm);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
