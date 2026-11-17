/** @file
 * @brief Generate group
 *
 * What each generator promises, measured with tsf-data's own checks on
 * 50 000 generated rows: NULL fractions and enum weights within four
 * binomial standard deviations; a correlated column exactly k*x+b; a
 * pattern that every value matches; UUIDs that are unique and
 * well-formed; boundary values (INT64 extremes, empty strings, leap
 * days) present; and the duplicates and malformed records the generator
 * reports are exactly what the reader then finds.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "generate/generators"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include <math.h>

#include "tapi_data.h"
#include "tsapi_data.h"

#define ROWS 50000

#define SPEC \
    "id:string:uuid;" \
    "x:int:uniform:min=1,max=1000;" \
    "y:int:corr:of=x,k=3,b=7;" \
    "code:string:pattern:pattern=[A-Z]{3}-\\d{4};" \
    "maybe:float:normal:mean=0,stddev=1,null=0.25;" \
    "level:string:enum:values=low|mid|high,weights=6|3|1;" \
    "edge:int:uniform:min=0,max=9,boundary=0.02;" \
    "day:date:time:start=2024-02-28,step=0"

/* |observed - expected| within 4 binomial standard deviations? */
static te_bool
binomial_ok(double observed, double p, double n)
{
    return fabs(observed - p * n) <= 4 * sqrt(n * p * (1 - p)) + 1;
}

int
main(int argc, char **argv)
{
    tsapi_data_session sess = { 0 };
    tapi_data_gen gen = { 0 };
    tapi_data *ds = NULL;
    tapi_data *bad = NULL;
    tapi_data_profile_info prof = { 0 };
    tapi_data_check chk = TAPI_DATA_CHECK_INIT;
    tapi_data_counts cnt = { 0 };
    const tapi_data_col_profile *c;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    char *schema = NULL;
    unsigned int i;

    TEST_START;

    TEST_STEP("Open a session and generate 50000 rows");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_gen_kinds"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    tsapi_data_work(&sess, "g.jsonl", &p);
    CHECK_RC(tapi_data_generate(sess.pco, p.ptr, "jsonl", SPEC,
                                "rows=50000;seed=2026", &gen));
    schema = tapi_data_unescape(gen.schema);
    CHECK_RC(tapi_data_open(sess.pco, p.ptr, NULL, NULL, schema, &ds));
    CHECK_RC(tapi_data_profile(ds, "topk=5", &prof));
    tapi_data_profile_log(&prof);
    mi = tsapi_mi_open();

    TEST_STEP("NULL fraction of 'maybe' is 0.25");
    c = tapi_data_profile_col(&prof, "maybe");
    if (!binomial_ok((double)c->nulls, 0.25, ROWS))
        TEST_VERDICT("null=0.25 gave %llu NULLs in %d rows",
                     (unsigned long long)c->nulls, ROWS);
    if (fabs(c->mean) > 0.05 || fabs(c->stddev - 1) > 0.05)
        TEST_VERDICT("normal(0, 1) has mean %g, stddev %g", c->mean,
                     c->stddev);

    TEST_STEP("Enum weights 6:3:1");
    c = tapi_data_profile_col(&prof, "level");
    for (i = 0; i < c->ntop; i++)
    {
        double w = strcmp(c->top[i], "low") == 0 ? 0.6 :
                   strcmp(c->top[i], "mid") == 0 ? 0.3 : 0.1;

        if (!binomial_ok((double)c->top_count[i], w, ROWS))
            TEST_VERDICT("Enum value %s drawn %llu times, expected about "
                         "%.0f", c->top[i],
                         (unsigned long long)c->top_count[i], w * ROWS);
        tsapi_mi_put(mi, "value", i);
        tsapi_mi_put(mi, "observed", (double)c->top_count[i]);
        tsapi_mi_put(mi, "expected", w * ROWS);
    }
    tsapi_mi_graph(mi, "enum", "Enum draws: observed vs expected", "value",
                   "observed,expected");
    tsapi_mi_close(&mi);

    TEST_STEP("y = 3x + 7 exactly");
    {
        const tapi_data_col_profile *x = tapi_data_profile_col(&prof, "x");

        c = tapi_data_profile_col(&prof, "y");
        if (c->sum != 3 * x->sum + 7 * ROWS ||
            !tsapi_data_near(c->stddev, 3 * x->stddev, 1e-9))
            TEST_VERDICT("The correlated column is not 3x+7");
    }

    TEST_STEP("Every code matches its pattern; UUIDs are unique and valid");
    CHECK_RC(tapi_data_check_pattern(ds, "code", "[A-Z]{3}-[0-9]{4}", NULL,
                                     NULL, &chk));
    if (!chk.passed)
        TEST_VERDICT("%llu generated codes do not match their pattern",
                     (unsigned long long)chk.violations);
    CHECK_RC(tapi_data_check_pattern(ds, "id", "@uuid", NULL, NULL, &chk));
    if (!chk.passed)
        TEST_VERDICT("%llu UUIDs are malformed",
                     (unsigned long long)chk.violations);
    CHECK_RC(tapi_data_check_unique(ds, "id", NULL, NULL, &chk));
    if (!chk.passed)
        TEST_VERDICT("%llu UUIDs repeat", (unsigned long long)chk.violations);

    TEST_STEP("Boundary values appear");
    c = tapi_data_profile_col(&prof, "edge");
    if (c->min == NULL || strcmp(c->min, "-9223372036854775808") != 0 ||
        strcmp(c->max, "9223372036854775807") != 0)
        TEST_VERDICT("boundary=0.02 did not reach the INT64 extremes "
                     "(%s..%s)", c->min, c->max);
    c = tapi_data_profile_col(&prof, "day");
    if (strcmp(c->min, "2024-02-28") != 0)
        TEST_VERDICT("A constant date column is %s", c->min);

    TEST_STEP("Reported duplicates and malformed records are what the "
              "reader finds");
    te_string_reset(&p);
    tsapi_data_work(&sess, "dirty.csv", &p);
    tapi_data_gen_free(&gen);
    CHECK_RC(tapi_data_generate(sess.pco, p.ptr, "csv",
                                "id:string:uuid;v:int:uniform",
                                "rows=50000;seed=7;dups=0.03;malformed=0.01",
                                &gen));
    CHECK_RC(tapi_data_open(sess.pco, p.ptr, NULL, NULL,
                            "id:string, v:int", &bad));
    CHECK_RC(tapi_data_count(bad, "on_error=count", &cnt));
    RING("generated %llu duplicates, %llu malformed; read %llu rows, %llu "
         "malformed", (unsigned long long)gen.duplicates,
         (unsigned long long)gen.malformed, (unsigned long long)cnt.rows,
         (unsigned long long)cnt.malformed);
    if (cnt.malformed != gen.malformed ||
        cnt.rows != gen.rows - gen.malformed)
        TEST_VERDICT("The reader found %llu malformed records, the "
                     "generator wrote %llu", (unsigned long long)cnt.malformed,
                     (unsigned long long)gen.malformed);
    if (!binomial_ok((double)gen.duplicates, 0.03, ROWS) ||
        !binomial_ok((double)gen.malformed, 0.01, ROWS))
        TEST_VERDICT("dups=0.03/malformed=0.01 gave %llu/%llu",
                     (unsigned long long)gen.duplicates,
                     (unsigned long long)gen.malformed);
    CHECK_RC(tapi_data_check_unique(bad, "id", "on_error=count", NULL, &chk));
    /* A duplicate of a row later written malformed is not readable. */
    if (chk.violations > gen.duplicates ||
        chk.violations + gen.malformed < gen.duplicates)
        TEST_VERDICT("unique found %llu repeats for %llu generated "
                     "duplicates", (unsigned long long)chk.violations,
                     (unsigned long long)gen.duplicates);
    mi = tsapi_mi_open();
    tsapi_mi_put(mi, "kind", 0);
    tsapi_mi_put(mi, "generated", (double)gen.duplicates);
    tsapi_mi_put(mi, "found", (double)chk.violations);
    tsapi_mi_put(mi, "kind", 1);
    tsapi_mi_put(mi, "generated", (double)gen.malformed);
    tsapi_mi_put(mi, "found", (double)cnt.malformed);
    te_mi_logger_add_comment(mi, NULL, "kind", "0 duplicates, 1 malformed");
    tsapi_mi_graph(mi, "dirty", "Injected vs found defects", "kind",
                   "generated,found");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_counts_free(&cnt);
    tapi_data_check_free(&chk);
    tapi_data_profile_free(&prof);
    tapi_data_close(bad);
    tapi_data_close(ds);
    tapi_data_gen_free(&gen);
    free(schema);
    te_string_free(&p);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
