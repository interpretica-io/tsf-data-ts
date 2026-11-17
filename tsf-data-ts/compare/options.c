/** @file
 * @brief Compare group
 *
 * Comparison options, one at a time, each with a predictable effect:
 * NULL semantics (NULL = NULL, or NULL never equal), duplicate
 * treatment (first occurrence, or multiset pairing), relative
 * tolerance, string trimming, the column subset, and the bound on the
 * report (the counts and the digest still cover every difference).
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "compare/options"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

static unsigned int
lines(const te_string *s)
{
    unsigned int n = 0;
    const char *p;

    for (p = s->ptr; p != NULL && *p != '\0'; p++)
        n += *p == '\n';
    return n;
}

int
main(int argc, char **argv)
{
    tsapi_data_session sess = { 0 };
    tapi_data *base = NULL;
    tapi_data *dups = NULL;
    tapi_data *b = NULL;
    tapi_data_diff d = TAPI_DATA_DIFF_INIT;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    const char *schema;
    char digest[17];
    unsigned int step = 0;

    TEST_START;

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_cmp_options"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    schema = tsapi_data_fact(&sess, "orders.schema");
    CHECK_RC(tapi_data_open(sess.pco,
                            tsapi_data_path(&sess, "orders_base.csv", &p),
                            NULL, NULL, schema, &base));
    te_string_reset(&p);
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "orders.csv", &p),
                            NULL, NULL, schema, &dups));
    te_string_reset(&p);
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "orders_b.csv",
                                                      &p),
                            NULL, NULL, schema, &b));
    mi = tsapi_mi_open();
#define POINT(_v) \
    do {                                                    \
        tsapi_mi_put(mi, "option", step++);                 \
        tsapi_mi_put(mi, "differences", (double)(_v));      \
    } while (0)

    TEST_STEP("NULL = NULL by default; null_eq=0 makes NULL unequal");
    CHECK_RC(tapi_data_compare(base, base, "mode=key;keys=order_id", &d));
    if (d.equal != 1)
        TEST_VERDICT("A dataset differs from itself");
    POINT(d.mismatched);
    CHECK_RC(tapi_data_compare(base, base, "mode=key;keys=order_id;null_eq=0",
                               &d));
    /* The base has the 7 NULL e-mails, minus any that were on a removed
     * duplicate - orders.csv's duplicates are copies of rows 11, 500, 999,
     * none of them a NULL-email row. */
    if (d.mismatched != (uint64_t)tsapi_data_fact_int(&sess,
                                                      "orders.null_email"))
        TEST_VERDICT("null_eq=0: %llu rows differ, expected the NULL "
                     "e-mails", (unsigned long long)d.mismatched);
    POINT(d.mismatched);

    TEST_STEP("Duplicates: reported, and paired as a multiset on request");
    CHECK_RC(tapi_data_compare(dups, base, "mode=key;keys=order_id", &d));
    tapi_data_diff_log(&d);
    if (d.dup_a != (uint64_t)tsapi_data_fact_int(&sess, "orders.dups") ||
        d.extra != 0 || d.missing != 0)
        TEST_VERDICT("dups=first: %llu duplicates, %llu missing, %llu extra",
                     (unsigned long long)d.dup_a,
                     (unsigned long long)d.missing,
                     (unsigned long long)d.extra);
    POINT(d.dup_a);
    CHECK_RC(tapi_data_compare(dups, base, "mode=key;keys=order_id;"
                               "dups=multiset;ignore=note", &d));
    if (d.missing != (uint64_t)tsapi_data_fact_int(&sess, "orders.dups"))
        TEST_VERDICT("dups=multiset: the %lld unpaired copies are not "
                     "missing (%llu)",
                     (long long)tsapi_data_fact_int(&sess, "orders.dups"),
                     (unsigned long long)d.missing);
    POINT(d.missing);

    TEST_STEP("Relative tolerance absorbs the cent, not the five euros");
    CHECK_RC(tapi_data_compare(base, b, "mode=key;keys=order_id;"
                               "columns=amount;rel_tol=0.001", &d));
    /* +0.01 on amounts of at least one euro is below 0.1%; +5.00 is
     * above it for any amount under 5000. */
    if (d.mismatched != (uint64_t)tsapi_data_fact_int(&sess,
                                                      "cmp.amount_big"))
        TEST_VERDICT("rel_tol=0.001 on amount: %llu mismatched",
                     (unsigned long long)d.mismatched);
    POINT(d.mismatched);

    TEST_STEP("columns= compares only the subset");
    CHECK_RC(tapi_data_compare(base, b, "mode=key;keys=order_id;"
                               "columns=status,qty", &d));
    if (d.mismatched != 0 || d.missing == 0)
        TEST_VERDICT("Comparing untouched columns: %llu mismatched",
                     (unsigned long long)d.mismatched);
    POINT(d.mismatched);

    TEST_STEP("max_report bounds the report, not the counts");
    CHECK_RC(tapi_data_compare(base, b, "mode=unordered", &d));
    strcpy(digest, d.digest);
    {
        uint64_t total = d.report_total;

        CHECK_RC(tapi_data_compare(base, b, "mode=unordered;max_report=4",
                                   &d));
        if (lines(&d.report) != 4 || d.report_total != total ||
            strcmp(d.digest, digest) != 0)
            TEST_VERDICT("max_report=4: %u lines, %llu total (of %llu), "
                         "digest %s (full %s)", lines(&d.report),
                         (unsigned long long)d.report_total,
                         (unsigned long long)total, d.digest, digest);
        POINT(lines(&d.report));
    }
#undef POINT
    te_mi_logger_add_comment(mi, NULL, "option",
                             "0 self, 1 null_eq=0, 2 dups, 3 multiset, "
                             "4 rel_tol, 5 columns, 6 max_report=4");
    tsapi_mi_graph(mi, "options", "Differences reported per option",
                   "option", "differences");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_diff_free(&d);
    tapi_data_close(b);
    tapi_data_close(dups);
    tapi_data_close(base);
    te_string_free(&p);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
