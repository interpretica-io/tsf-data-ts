/** @file
 * @brief Quality group
 *
 * One iteration per data quality assertion, each against a fixture with
 * a known number of planted defects. The assertion must count exactly
 * the oracle's number; with no tolerance it must fail, and with a
 * threshold just above the defects it must pass - the threshold is
 * what decides, not the check. Each iteration charts violations against
 * the tolerance it was given.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "quality/checks"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

/* Run the iteration's check with threshold @p thr. */
static te_errno
run_check(const char *check, tsapi_data_session *s, tapi_data *orders,
          tapi_data *aux, const tapi_data_threshold *thr,
          tapi_data_check *res)
{
    if (strcmp(check, "nulls") == 0)
        return tapi_data_check_nulls(orders, "email", NULL, thr, res);
    if (strcmp(check, "unique") == 0)
        return tapi_data_check_unique(orders, "order_id", NULL, thr, res);
    if (strcmp(check, "range") == 0)
        return tapi_data_check_range(orders, "amount", "0", "100000", NULL,
                                     thr, res);
    if (strcmp(check, "pattern") == 0)
        return tapi_data_check_pattern(orders, "email", "@email", NULL, thr,
                                       res);
    if (strcmp(check, "reference") == 0)
        return tapi_data_check_reference(orders, "customer_id", aux,
                                         "customer_id", NULL, thr, res);
    if (strcmp(check, "freshness") == 0)
        return tapi_data_check_freshness(aux, "event_time",
                                         "ingest=ingest_time;"
                                         "max_delay_us=60000000;"
                                         "interval_us=60000000;"
                                         "id=event_id;seq=seq;"
                                         "now=2026-01-01T09:19:00Z;"
                                         "max_age_us=1800000000", thr, res);
    if (strcmp(check, "distribution") == 0)
    {
        te_string f = TE_STRING_INIT;
        double n = (double)tsapi_data_fact_int(s, "orders.rows");
        te_errno rc;

        /* Expected shares: the oracle's own counts. */
        te_string_append(&f, "freq=new:%.17g|paid:%.17g|shipped:%.17g|"
                         "cancelled:%.17g",
                         tsapi_data_fact_int(s, "status.new") / n,
                         tsapi_data_fact_int(s, "status.paid") / n,
                         tsapi_data_fact_int(s, "status.shipped") / n,
                         tsapi_data_fact_int(s, "status.cancelled") / n);
        rc = tapi_data_check_distribution(orders, "status", NULL, f.ptr, thr,
                                          res);
        te_string_free(&f);
        return rc;
    }
    if (strcmp(check, "pii") == 0)
        return tapi_data_check_pii(aux, NULL, NULL, thr, res);
    return TE_RC(TE_TAPI, TE_EINVAL);
}

int
main(int argc, char **argv)
{
    tsapi_data_session sess = { 0 };
    const char *check;
    tapi_data *orders = NULL;
    tapi_data *aux = NULL;
    tapi_data_check chk = TAPI_DATA_CHECK_INIT;
    tapi_data_threshold thr;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    int64_t expect;
    int64_t tol;

    TEST_START;
    TEST_GET_STRING_PARAM(check);

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_quality"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "orders.csv", &p),
                            NULL, NULL, tsapi_data_fact(&sess, "orders.schema"),
                            &orders));
    te_string_reset(&p);
    if (strcmp(check, "reference") == 0)
        CHECK_RC(tapi_data_open(sess.pco,
                                tsapi_data_path(&sess, "customers.csv", &p),
                                NULL, NULL, NULL, &aux));
    else if (strcmp(check, "freshness") == 0)
        CHECK_RC(tapi_data_open(sess.pco,
                                tsapi_data_path(&sess, "events.jsonl", &p),
                                NULL, NULL, NULL, &aux));
    else if (strcmp(check, "pii") == 0)
        CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "pii.csv", &p),
                                NULL, NULL, NULL, &aux));

    /* What the oracle planted, per check. */
    if (strcmp(check, "nulls") == 0)
        expect = tsapi_data_fact_int(&sess, "orders.null_email");
    else if (strcmp(check, "unique") == 0)
        expect = tsapi_data_fact_int(&sess, "orders.dups");
    else if (strcmp(check, "range") == 0)
        expect = tsapi_data_fact_int(&sess, "orders.neg_amount");
    else if (strcmp(check, "pattern") == 0)
        expect = tsapi_data_fact_int(&sess, "orders.bad_email");
    else if (strcmp(check, "reference") == 0)
        expect = tsapi_data_fact_int(&sess, "orders.orphans");
    else if (strcmp(check, "freshness") == 0)
        /* stale + late + gap + missing sequence numbers + duplicate ids */
        expect = 1 + tsapi_data_fact_int(&sess, "events.late") +
                 tsapi_data_fact_int(&sess, "events.gaps") +
                 tsapi_data_fact_int(&sess, "events.seq_missing") +
                 tsapi_data_fact_int(&sess, "events.dup_ids");
    else if (strcmp(check, "pii") == 0)
        /* 5 detected columns + 5 telling column names */
        expect = 10;
    else
        expect = 0;
    mi = tsapi_mi_open();

    TEST_STEP("Run %s with no tolerance", check);
    thr = TAPI_DATA_NONE_ALLOWED;
    if (strcmp(check, "distribution") == 0)
        thr.max_stat = 1e-9;
    CHECK_RC(run_check(check, &sess, orders, aux, &thr, &chk));
    RING("%s\n%s", chk.summary, chk.report.ptr);
    if ((int64_t)chk.violations != expect)
        TEST_VERDICT("%s counted %llu violations, the oracle planted %lld",
                     check, (unsigned long long)chk.violations,
                     (long long)expect);
    if (chk.passed != (expect == 0))
        TEST_VERDICT("%s with no tolerance %s", check,
                     chk.passed ? "passed despite the defects" :
                     "failed on clean data");

    if (strcmp(check, "freshness") == 0)
    {
        TEST_SUBSTEP("Each freshness finding is the oracle's");
#define FACT(_k, _f) \
        if (tapi_data_kv_int(&chk.kv, _k, -1) !=                    \
            tsapi_data_fact_int(&sess, _f))                         \
            TEST_VERDICT("freshness %s = %lld, oracle %s = %lld", _k, \
                         (long long)tapi_data_kv_int(&chk.kv, _k, -1),\
                         _f, (long long)tsapi_data_fact_int(&sess, _f))
        FACT("delay_violations", "events.late");
        FACT("gaps", "events.gaps");
        FACT("out_of_order", "events.out_of_order");
        FACT("dup_ids", "events.dup_ids");
        FACT("seq_missing", "events.seq_missing");
        FACT("checked", "events.rows");
#undef FACT
        if (strcmp(tapi_data_kv_get(&chk.kv, "latest_event", ""),
                   tsapi_data_fact(&sess, "events.latest")) != 0 ||
            tapi_data_kv_int(&chk.kv, "stale", 0) != 1)
            TEST_VERDICT("freshness: latest event %s, stale %lld",
                         tapi_data_kv_get(&chk.kv, "latest_event", ""),
                         (long long)tapi_data_kv_int(&chk.kv, "stale", 0));
    }
    if (strcmp(check, "pii") == 0)
    {
        static const char *const want[] = {
            "contact\temail\t", "phone\tphone\t", "card\tcredit_card\t",
            "iban\tiban\t", "ssn\tus_ssn\t",
        };
        static const char *const what[] = {
            "e-mail", "phone", "card number", "IBAN", "SSN",
        };
        unsigned int i;

        TEST_SUBSTEP("Every planted kind is found, the control is not");
        for (i = 0; i < TE_ARRAY_LEN(want); i++)
        {
            if (strstr(chk.report.ptr, want[i]) == NULL)
                TEST_VERDICT("pii: the planted %s was not found", what[i]);
        }
        if (strstr(chk.report.ptr, "not_a_card\tcredit_card") != NULL ||
            strstr(chk.report.ptr, "comment\t") != NULL)
            TEST_VERDICT("pii: a Luhn-invalid number or plain text was "
                         "reported as a card number");
        if (strstr(chk.report.ptr, "possible sensitive data") == NULL)
            TEST_VERDICT("pii findings are not worded as possible findings");
    }
    tsapi_mi_put(mi, "tolerance", 0);
    tsapi_mi_put(mi, "violations", (double)chk.violations);
    tsapi_mi_put(mi, "passed", chk.passed ? 1 : 0);

    TEST_STEP("The threshold decides: tolerances around the defects");
    for (tol = expect > 0 ? expect - 1 : 0; tol <= expect + 1; tol++)
    {
        thr = TAPI_DATA_NONE_ALLOWED;
        thr.max_count = tol;
        if (strcmp(check, "distribution") == 0)
        {
            thr.max_count = -1;
            thr.max_stat = 0.01 * (double)tol;
        }
        CHECK_RC(run_check(check, &sess, orders, aux, &thr, &chk));
        RING("tolerance %lld: %s", (long long)tol, chk.summary);
        if (strcmp(check, "distribution") != 0 &&
            chk.passed != (expect <= tol))
            TEST_VERDICT("%s with %lld allowed %s at %lld violations", check,
                         (long long)tol, chk.passed ? "passed" : "failed",
                         (long long)expect);
        tsapi_mi_put(mi, "tolerance", (double)tol);
        tsapi_mi_put(mi, "violations", (double)chk.violations);
        tsapi_mi_put(mi, "passed", chk.passed ? 1 : 0);
    }
    tsapi_mi_graph(mi, "threshold", "Violations and pass/fail vs tolerance",
                   "tolerance", "violations,passed");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_check_free(&chk);
    tapi_data_close(aux);
    tapi_data_close(orders);
    te_string_free(&p);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
