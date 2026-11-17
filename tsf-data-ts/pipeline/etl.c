/** @file
 * @brief Pipeline group
 *
 * ETL validation. The pipeline under test (lib/py/etl.py, run on the
 * agent) loads orders, drops the cancelled ones, joins the customer
 * tier, converts amounts to cents and aggregates per status; each
 * iteration switches on one defect, or none. The validation battery is
 * the same every time:
 *
 * - reconciliation: every non-cancelled source order reaches the target
 *   once, nothing appears from nowhere;
 * - uniqueness of the target key (duplicate elimination);
 * - the joined tier is never NULL (referential completeness);
 * - the target's schema contract;
 * - the conservation invariant: total cents = the source's kept total;
 * - aggregation: tsf-data's own GROUP BY over the target equals the
 *   pipeline's aggregate output.
 *
 * Exactly the checks the defect should trip must fail - no more (a
 * false alarm) and no fewer (a missed defect).
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "pipeline/etl"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

#define CONTRACT \
    "order_id:int!, customer_id:int!, tier:string, amount_cents:int!, " \
    "status:string!, created:timestamp!"

enum {
    C_RECONCILE = 1 << 0,
    C_UNIQUE = 1 << 1,
    C_TIER = 1 << 2,
    C_SCHEMA = 1 << 3,
    C_SUM = 1 << 4,
    C_AGG = 1 << 5,
};

static const char *const check_names[] = {
    "reconcile", "unique", "tier", "schema", "sum", "aggregate",
};

int
main(int argc, char **argv)
{
    static const struct {
        const char *bug;
        unsigned int expect;
    } bugs[] = {
        { "ok", 0 },
        { "drop", C_RECONCILE | C_SUM },
        { "dup", C_RECONCILE | C_UNIQUE | C_SUM },
        { "append", C_RECONCILE | C_UNIQUE | C_SUM },
        { "agg", C_AGG },
        { "orphan", C_TIER },
        { "schema", C_SCHEMA | C_SUM | C_AGG },
    };
    tsapi_data_session sess = { 0 };
    const char *bug;
    unsigned int expect = 0;
    unsigned int failed = 0;
    tapi_data *src = NULL;
    tapi_data *fact = NULL;
    tapi_data *agg = NULL;
    tapi_data *ours = NULL;
    tapi_data *cust = NULL;
    tapi_data_recon rec = TAPI_DATA_RECON_INIT;
    tapi_data_check chk = TAPI_DATA_CHECK_INIT;
    tapi_data_profile_info prof = { 0 };
    tapi_data_diff d = TAPI_DATA_DIFF_INIT;
    tapi_data_kv res = TAPI_DATA_KV_INIT;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    unsigned int i;

    TEST_START;
    TEST_GET_STRING_PARAM(bug);
    for (i = 0; i < TE_ARRAY_LEN(bugs); i++)
    {
        if (strcmp(bugs[i].bug, bug) == 0)
            expect = bugs[i].expect;
    }

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_pipe_etl"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);

    TEST_STEP("Run the pipeline (%s)", bug);
    CHECK_RC(tsapi_data_etl(&sess, "out", bug, 0));
    if (strcmp(bug, "append") == 0)
    {
        /* Re-processing the same input: an idempotent pipeline gives the
         * same output, this one appends a second copy. */
        CHECK_RC(tsapi_data_etl(&sess, "out", bug, 0));
    }
    CHECK_RC(tapi_data_open(sess.pco,
                            tsapi_data_path(&sess, "orders_base.csv", &p),
                            NULL, NULL, tsapi_data_fact(&sess,
                                                        "orders.schema"),
                            &src));
    te_string_reset(&p);
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_work(&sess, "out/fact.csv",
                                                      &p),
                            NULL, NULL, NULL, &fact));
    mi = tsapi_mi_open();

    TEST_STEP("Reconcile source and target by order_id");
    CHECK_RC(tapi_data_reconcile(src, fact, "keys=order_id", &rec));
    RING("lost %llu (cancelled %lld), phantom %llu, target duplicates %llu",
         (unsigned long long)rec.keys_lost,
         (long long)tsapi_data_fact_int(&sess, "base.cancelled"),
         (unsigned long long)rec.keys_phantom,
         (unsigned long long)rec.dup_dst);
    if ((int64_t)rec.keys_lost != tsapi_data_fact_int(&sess, "base.cancelled") ||
        rec.keys_phantom != 0 || rec.dup_dst != 0)
        failed |= C_RECONCILE;
    tsapi_mi_put(mi, "check", 0);
    tsapi_mi_put(mi, "violations",
                 (double)(rec.keys_lost - (uint64_t)tsapi_data_fact_int(&sess,
                          "base.cancelled") + rec.keys_phantom + rec.dup_dst));

    TEST_STEP("The target key is unique");
    CHECK_RC(tapi_data_check_unique(fact, "order_id", NULL, NULL, &chk));
    if (!chk.passed)
        failed |= C_UNIQUE;
    tsapi_mi_put(mi, "check", 1);
    tsapi_mi_put(mi, "violations", (double)chk.violations);

    TEST_STEP("Every order found its customer's tier");
    {
        /* The source carries orphan customers of its own (a known source
         * defect): an order may lack a tier only when its customer is
         * unknown, so the orphans in the target are the tolerance. */
        tapi_data_threshold thr = TAPI_DATA_NONE_ALLOWED;

        te_string_reset(&p);
        CHECK_RC(tapi_data_open(sess.pco,
                                tsapi_data_path(&sess, "customers.csv", &p),
                                NULL, NULL, NULL, &cust));
        CHECK_RC(tapi_data_check_reference(fact, "customer_id", cust, NULL,
                                           NULL, NULL, &chk));
        thr.max_count = (int64_t)chk.violations;
        CHECK_RC(tapi_data_check_nulls(fact, "tier", NULL, &thr, &chk));
        RING("%s", chk.summary);
    }
    if (!chk.passed)
        failed |= C_TIER;
    tsapi_mi_put(mi, "check", 2);
    tsapi_mi_put(mi, "violations", (double)chk.violations);

    TEST_STEP("The target keeps its schema contract");
    CHECK_RC(tapi_data_check_schema(fact, CONTRACT, NULL, NULL, &chk));
    if (!chk.passed)
        failed |= C_SCHEMA;
    tsapi_mi_put(mi, "check", 3);
    tsapi_mi_put(mi, "violations", (double)chk.violations);

    TEST_STEP("Conservation: total cents equal the source's kept total");
    if (tapi_data_schema_find(&fact->schema, "amount_cents") < 0)
        failed |= C_SUM | C_AGG;
    else
    {
        const tapi_data_col_profile *c;

        CHECK_RC(tapi_data_profile(fact, "columns=amount_cents;topk=0",
                                   &prof));
        c = tapi_data_profile_col(&prof, "amount_cents");
        RING("target total %.0f, source kept total %lld (exact sum: %s)",
             c->sum, (long long)tsapi_data_fact_int(&sess, "base.kept_cents"),
             tapi_data_kv_get(&c->kv, "sum", "?"));
        if (c->sum != (double)tsapi_data_fact_int(&sess, "base.kept_cents"))
            failed |= C_SUM;
        tsapi_mi_put(mi, "check", 4);
        tsapi_mi_put(mi, "violations", c->sum != (double)
                     tsapi_data_fact_int(&sess, "base.kept_cents"));

        TEST_STEP("tsf-data's aggregate equals the pipeline's");
        te_string_reset(&p);
        tsapi_data_work(&sess, "ours.csv", &p);
        CHECK_RC(tapi_data_aggregate(fact, p.ptr, "csv",
                                     "group_by=status;"
                                     "aggs=count:*,sum:amount_cents", &res));
        CHECK_RC(tapi_data_open(sess.pco, p.ptr, NULL, NULL, NULL, &ours));
        te_string_reset(&p);
        CHECK_RC(tapi_data_open(sess.pco, tsapi_data_work(&sess,
                                                          "out/agg.csv", &p),
                                NULL, NULL, NULL, &agg));
        CHECK_RC(tapi_data_compare(ours, agg, "mode=key;keys=status", &d));
        tapi_data_diff_log(&d);
        if (d.equal != 1)
            failed |= C_AGG;
        tsapi_mi_put(mi, "check", 5);
        tsapi_mi_put(mi, "violations", (double)(d.mismatched + d.missing +
                                                d.extra));
    }
    te_mi_logger_add_comment(mi, NULL, "check", "0 reconcile, 1 unique, "
                             "2 tier, 3 schema, 4 sum, 5 aggregate");
    tsapi_mi_graph(mi, "battery", "Validation findings per check", "check",
                   "violations");
    tsapi_mi_close(&mi);

    TEST_STEP("Exactly the checks the defect should trip have failed");
    for (i = 0; i < TE_ARRAY_LEN(check_names); i++)
    {
        unsigned int bit = 1u << i;

        if ((failed & bit) && !(expect & bit))
            TEST_VERDICT("Pipeline '%s': the %s check raised a false alarm",
                         bug, check_names[i]);
        if (!(failed & bit) && (expect & bit))
            TEST_VERDICT("Pipeline '%s': the %s check missed the defect",
                         bug, check_names[i]);
    }

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_kv_free(&res);
    tapi_data_diff_free(&d);
    tapi_data_profile_free(&prof);
    tapi_data_check_free(&chk);
    tapi_data_recon_free(&rec);
    tapi_data_close(ours);
    tapi_data_close(cust);
    tapi_data_close(agg);
    tapi_data_close(fact);
    tapi_data_close(src);
    te_string_free(&p);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
