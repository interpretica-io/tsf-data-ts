/** @file
 * @brief Quality group
 *
 * Data quality assertions as TE verdicts. The same battery of checks
 * runs over a clean dataset and over a defective one; on the clean one
 * the test passes with no verdict, on the defective one every failing
 * check emits its verdict through tapi_data_check_verdict() and the test
 * fails. The verdict texts carry counts and thresholds only, so TRC
 * matches them run after run: conf/trc.xml expects this iteration to
 * fail with exactly these verdicts.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "quality/verdict"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

int
main(int argc, char **argv)
{
    tsapi_data_session sess = { 0 };
    const char *dataset;
    tapi_data *ds = NULL;
    tapi_data *cust = NULL;
    tapi_data_check chk = TAPI_DATA_CHECK_INIT;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    unsigned int failed = 0;
    unsigned int i;

    TEST_START;
    TEST_GET_STRING_PARAM(dataset);

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_quality_verdict"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    /* customers.csv is clean; orders.csv carries the planted defects. */
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess,
                                strcmp(dataset, "clean") == 0 ?
                                "customers.csv" : "orders.csv", &p),
                            NULL, NULL, NULL, &ds));
    te_string_reset(&p);
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "customers.csv",
                                                      &p),
                            NULL, NULL, NULL, &cust));
    mi = tsapi_mi_open();

    for (i = 0; i < 3; i++)
    {
        switch (i)
        {
            case 0:
                TEST_STEP("No duplicate customer_id/order_id");
                CHECK_RC(tapi_data_check_unique(ds,
                         strcmp(dataset, "clean") == 0 ? "customer_id" :
                         "order_id", NULL, NULL, &chk));
                break;
            case 1:
                TEST_STEP("Every customer_id is a known customer");
                CHECK_RC(tapi_data_check_reference(ds, "customer_id", cust,
                                                   NULL, NULL, NULL, &chk));
                break;
            default:
                TEST_STEP("No NULL in the first column");
                CHECK_RC(tapi_data_check_nulls(ds,
                         strcmp(dataset, "clean") == 0 ? "name" : "email",
                         NULL, NULL, &chk));
                break;
        }
        if (!tapi_data_check_verdict(&chk))
            failed++;
        tsapi_mi_put(mi, "check", i);
        tsapi_mi_put(mi, "violations", (double)chk.violations);
    }
    te_mi_logger_add_comment(mi, NULL, "check",
                             "0 unique, 1 reference, 2 nulls");
    tsapi_mi_graph(mi, "verdicts", "Violations per check", "check",
                   "violations");
    tsapi_mi_close(&mi);

    if (failed > 0)
        TEST_STOP;
    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_check_free(&chk);
    tapi_data_close(cust);
    tapi_data_close(ds);
    te_string_free(&p);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
