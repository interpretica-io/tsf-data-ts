/** @file
 * @brief Pipeline group
 *
 * Incremental ingestion, late-arriving records and idempotent
 * re-processing. The pipeline is run day by day (run N loads what was
 * created before day N+1, so each run picks up records that arrived
 * after the previous cut-off); after every run:
 *
 * - the target only grows, and nothing loaded before disappears
 *   (reconciliation of the previous snapshot against the new target);
 * - the newest record is no older than the cut-off allows (freshness,
 *   with the cut-off as "now");
 * - re-running the same day changes nothing (the content digest is the
 *   same) - unless the pipeline appends on re-processing, which the
 *   uniqueness check and the digest must both catch.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "pipeline/incremental"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

int
main(int argc, char **argv)
{
    tsapi_data_session sess = { 0 };
    const char *mode;
    te_bool idempotent;
    tapi_data *fact = NULL;
    tapi_data *prev = NULL;
    tapi_data_recon rec = TAPI_DATA_RECON_INIT;
    tapi_data_check chk = TAPI_DATA_CHECK_INIT;
    tapi_data_digest_info d1;
    tapi_data_digest_info d2;
    tapi_data_kv res = TAPI_DATA_KV_INIT;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    te_string snap = TE_STRING_INIT;
    te_string prm = TE_STRING_INIT;
    uint64_t last_rows = 0;
    te_bool caught = false;
    int day;

    TEST_START;
    TEST_GET_STRING_PARAM(mode);
    idempotent = strcmp(mode, "ok") == 0;

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_pipe_incr"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    tsapi_data_work(&sess, "out/fact.csv", &p);
    tsapi_data_work(&sess, "snapshot.csv", &snap);
    mi = tsapi_mi_open();

    for (day = 1; day <= 5; day++)
    {
        TEST_STEP("Day %d: load what arrived before the cut-off", day);
        CHECK_RC(tsapi_data_etl(&sess, "out", mode, day));
        CHECK_RC(tapi_data_open(sess.pco, p.ptr, NULL, NULL, NULL, &fact));

        if (prev != NULL)
        {
            /* Nothing loaded yesterday may be gone today. */
            CHECK_RC(tapi_data_reconcile(prev, fact, "keys=order_id", &rec));
            RING("day %d: %llu -> %llu rows, %llu new, %llu lost", day,
                 (unsigned long long)rec.rows_src,
                 (unsigned long long)rec.rows_dst,
                 (unsigned long long)rec.keys_phantom,
                 (unsigned long long)rec.keys_lost);
            if (rec.keys_lost != 0)
                TEST_VERDICT("Day %d lost %llu records of day %d", day,
                             (unsigned long long)rec.keys_lost, day - 1);
            if (rec.rows_dst < last_rows)
                TEST_VERDICT("The target shrank on day %d", day);
            tsapi_mi_put(mi, "day", day);
            tsapi_mi_put(mi, "new_rows", (double)rec.keys_phantom);
            tsapi_mi_put(mi, "rows", (double)rec.rows_dst);
            last_rows = rec.rows_dst;
            tapi_data_recon_free(&rec);
            CHECK_RC(tapi_data_close(prev));
            prev = NULL;
        }

        TEST_SUBSTEP("Freshness against the day's cut-off");
        te_string_reset(&prm);
        te_string_append(&prm, "now=2026-01-%02dT00:00:00Z;"
                         "max_age_us=86400000000", day + 1);
        CHECK_RC(tapi_data_check_freshness(fact, "created", prm.ptr, NULL,
                                           &chk));
        RING("day %d: latest %s, age %lld us", day,
             tapi_data_kv_get(&chk.kv, "latest_event", "?"),
             (long long)tapi_data_kv_int(&chk.kv, "age_us", -1));
        if (tapi_data_kv_int(&chk.kv, "stale", 1) != 0 ||
            tapi_data_kv_int(&chk.kv, "age_us", -1) < 0)
            TEST_VERDICT("Day %d: the newest record is stale or from the "
                         "future", day);

        TEST_SUBSTEP("Re-process the same day");
        CHECK_RC(tapi_data_digest(fact, NULL, &d1));
        CHECK_RC(tapi_data_close(fact));
        fact = NULL;
        CHECK_RC(tsapi_data_etl(&sess, "out", mode, day));
        CHECK_RC(tapi_data_open(sess.pco, p.ptr, NULL, NULL, NULL, &fact));
        CHECK_RC(tapi_data_digest(fact, NULL, &d2));
        CHECK_RC(tapi_data_check_unique(fact, "order_id", NULL, NULL, &chk));
        if (idempotent && (strcmp(d1.digest, d2.digest) != 0 || !chk.passed))
            TEST_VERDICT("Re-processing day %d changed the target", day);
        if (!idempotent && (strcmp(d1.digest, d2.digest) != 0 &&
                            !chk.passed))
            caught = true;
        tsapi_mi_put(mi, "rerun_duplicates", (double)chk.violations);

        /* Today's target is tomorrow's baseline. */
        te_string_reset(&prm);
        CHECK_RC(tapi_data_convert(fact, snap.ptr, "csv", NULL, &res));
        tapi_data_kv_free(&res);
        CHECK_RC(tapi_data_open(sess.pco, snap.ptr, NULL, NULL, NULL, &prev));
        CHECK_RC(tapi_data_close(fact));
        fact = NULL;
    }
    if (!idempotent && !caught)
        TEST_VERDICT("A pipeline that appends on re-processing went "
                     "unnoticed");
    tsapi_mi_graph(mi, "growth", "Target rows, new rows per day and "
                   "duplicates after a re-run", "day",
                   "rows,new_rows,rerun_duplicates");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_kv_free(&res);
    tapi_data_check_free(&chk);
    tapi_data_recon_free(&rec);
    tapi_data_close(prev);
    tapi_data_close(fact);
    te_string_free(&p);
    te_string_free(&snap);
    te_string_free(&prm);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
