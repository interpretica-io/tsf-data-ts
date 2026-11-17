/** @file
 * @brief Compare group
 *
 * Reproducible reports. The same unordered and key-based comparisons
 * run under memory limits from generous to tiny, which changes how many
 * on-disk partitions the work is split into; the counts, the report and
 * its digest must not change at all, run after run, and the memory used
 * must stay under each limit. Charted: partitions and peak memory
 * against the limit.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "compare/reproducible"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

int
main(int argc, char **argv)
{
    static const struct {
        const char *limit;
        double      bytes;
    } limits[] = {
        { "256m", 268435456 }, { "4m", 4194304 }, { "512k", 524288 },
        { "64k", 65536 }, { "16k", 16384 },
    };
    static const char *const modes[] = {
        "mode=unordered", "mode=key;keys=order_id",
    };
    tsapi_data_session sess = { 0 };
    tapi_data *a = NULL;
    tapi_data *b = NULL;
    tapi_data_diff d = TAPI_DATA_DIFF_INIT;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    te_string prm = TE_STRING_INIT;
    te_string ref_report = TE_STRING_INIT;
    char ref_digest[17];
    uint64_t ref_counts[4];
    const char *schema;
    unsigned int m;
    unsigned int i;
    unsigned int run;

    TEST_START;

    TEST_STEP("Open a session and make the fixtures");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_cmp_repro"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    schema = tsapi_data_fact(&sess, "orders.schema");
    CHECK_RC(tapi_data_open(sess.pco,
                            tsapi_data_path(&sess, "orders_base.csv", &p),
                            NULL, NULL, schema, &a));
    te_string_reset(&p);
    CHECK_RC(tapi_data_open(sess.pco, tsapi_data_path(&sess, "orders_b.csv",
                                                      &p),
                            NULL, NULL, schema, &b));
    mi = tsapi_mi_open();

    for (m = 0; m < TE_ARRAY_LEN(modes); m++)
    {
        TEST_STEP("%s under shrinking memory limits", modes[m]);
        for (i = 0; i < TE_ARRAY_LEN(limits); i++)
        {
            for (run = 0; run < 2; run++)
            {
                uint64_t counts[4];

                te_string_reset(&prm);
                te_string_append(&prm, "%s;mem_limit=%s;max_report=50",
                                 modes[m], limits[i].limit);
                CHECK_RC(tapi_data_compare(a, b, prm.ptr, &d));
                RING("%s: %u partition(s), %llu bytes peak, digest %s",
                     prm.ptr, d.partitions,
                     (unsigned long long)d.mem_peak, d.digest);
                counts[0] = d.matched;
                counts[1] = d.missing;
                counts[2] = d.extra;
                counts[3] = d.mismatched;
                if ((double)d.mem_peak > limits[i].bytes)
                    TEST_VERDICT("%s used %llu bytes over its limit",
                                 prm.ptr, (unsigned long long)d.mem_peak);
                if (i == 0 && run == 0)
                {
                    memcpy(ref_counts, counts, sizeof(counts));
                    strcpy(ref_digest, d.digest);
                    te_string_reset(&ref_report);
                    te_string_append(&ref_report, "%s", d.report.ptr);
                }
                else if (memcmp(ref_counts, counts, sizeof(counts)) != 0 ||
                         strcmp(ref_digest, d.digest) != 0 ||
                         strcmp(ref_report.ptr, d.report.ptr) != 0)
                    TEST_VERDICT("%s: the result changed with the memory "
                                 "limit or the run", modes[m]);
                if (run == 0)
                {
                    tsapi_mi_put(mi, "mem_limit_kib", limits[i].bytes / 1024);
                    tsapi_mi_put(mi, m == 0 ? "partitions_unordered" :
                                 "partitions_key", d.partitions);
                    tsapi_mi_put(mi, m == 0 ? "peak_kib_unordered" :
                                 "peak_kib_key", (double)d.mem_peak / 1024);
                }
            }
        }
        if (i > 0 && d.partitions <= 1)
            TEST_VERDICT("%s: a 16 KiB limit did not partition the work",
                         modes[m]);
    }
    tsapi_mi_graph(mi, "partitions", "Spill partitions vs memory limit "
                   "(KiB)", "mem_limit_kib",
                   "partitions_unordered,partitions_key");
    tsapi_mi_graph(mi, "peak", "Peak memory vs limit (KiB)",
                   "mem_limit_kib", "peak_kib_unordered,peak_kib_key");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_diff_free(&d);
    tapi_data_close(b);
    tapi_data_close(a);
    te_string_free(&p);
    te_string_free(&prm);
    te_string_free(&ref_report);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
