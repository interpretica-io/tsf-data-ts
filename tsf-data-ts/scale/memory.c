/** @file
 * @brief Scale group
 *
 * Large datasets in bounded memory. A dataset many times larger than the
 * memory limit is generated on the agent, then counted, profiled,
 * de-duplicated and compared with itself under mem_limit. After each
 * operation the RPC server's peak resident set size must have grown by
 * no more than the limit plus a fixed allowance for the libraries'
 * own buffers - so the data was streamed, not loaded - and the engine
 * received only the small results. Charted: peak RSS per operation
 * against the file size and the limit.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "scale/memory"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"

#include "tapi_data.h"
#include "tsapi_data.h"

/** Room for fixed I/O buffers, Arrow's pools and the allocator. */
#define SLACK_KB (64 * 1024)

#define SPEC \
    "id:int:seq;" \
    "k:int:uniform:min=1,max=2000000;" \
    "amount:decimal(12,2):uniform:min=-1000,max=100000;" \
    "who:string:pattern:pattern=[a-z]{8}@[a-z]{6}\\.com;" \
    "at:timestamp:time:start=2025-01-01T00:00:00Z,step=1000000,jitter=999;" \
    "kind:string:enum:values=a|b|c|d"

int
main(int argc, char **argv)
{
    tsapi_data_session sess = { 0 };
    unsigned int rows;
    const char *mem_limit;
    const char *format;
    tapi_data_gen gen = { 0 };
    tapi_data *ds = NULL;
    tapi_data *other = NULL;
    tapi_data_counts cnt = { 0 };
    tapi_data_profile_info prof = { 0 };
    tapi_data_check chk = TAPI_DATA_CHECK_INIT;
    tapi_data_diff d = TAPI_DATA_DIFF_INIT;
    tapi_data_stats st;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    te_string prm = TE_STRING_INIT;
    char *schema = NULL;
    long base_kb;
    long limit_kb;
    unsigned int op = 0;

    TEST_START;
    TEST_GET_UINT_PARAM(rows);
    TEST_GET_STRING_PARAM(mem_limit);
    TEST_GET_STRING_PARAM(format);
    limit_kb = strtol(mem_limit, NULL, 10) *
               (strchr(mem_limit, 'g') != NULL ? 1024 * 1024 : 1024);

    TEST_STEP("Open a session");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_scale"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    if (strcmp(format, "parquet") == 0 &&
        !tapi_data_supports(sess.pco, TAPI_DATA_FEAT_PARQUET))
        TEST_SKIP("The agent's engine has no Parquet (Arrow C++)");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    mi = tsapi_mi_open();

    TEST_STEP("Generate %u rows of %s on the agent", rows, format);
    te_string_append(&prm, "%s/big.%s", sess.work, format);
    te_string_append(&p, "%s", prm.ptr);
    te_string_reset(&prm);
    te_string_append(&prm, "rows=%u;seed=77", rows);
    CHECK_RC(tapi_data_generate(sess.pco, p.ptr, format, SPEC, prm.ptr,
                                &gen));
    schema = tapi_data_unescape(gen.schema);
    RING("%u rows, %llu bytes on the agent; limit %s", rows,
         (unsigned long long)gen.bytes, mem_limit);
    if ((double)gen.bytes / 1024 < 2.0 * (limit_kb + SLACK_KB))
        WARN("The file is not much larger than the memory bound: the "
             "test proves less");

    /* The baseline: a fresh RPC server after one small operation. */
    CHECK_RC(tapi_data_open(sess.pco, p.ptr, format, NULL, schema, &ds));
    CHECK_RC(tapi_data_get_stats(sess.pco, &st));
    base_kb = st.maxrss_kb;
    RING("Baseline peak RSS %ld KiB", base_kb);
    te_string_reset(&prm);
    te_string_append(&prm, "mem_limit=%s;tmpdir=%s", mem_limit, sess.work);

#define MEASURE(_what) \
    do {                                                                \
        CHECK_RC(tapi_data_get_stats(sess.pco, &st));                   \
        RING("%s: peak RSS %ld KiB (+%ld over the baseline, bound %ld)",\
             _what, st.maxrss_kb, st.maxrss_kb - base_kb,               \
             limit_kb + SLACK_KB);                                      \
        if (st.maxrss_kb - base_kb > limit_kb + SLACK_KB)               \
            TEST_VERDICT("%s grew the RPC server by %ld KiB, over the " \
                         "%ld KiB bound", _what, st.maxrss_kb - base_kb,\
                         limit_kb + SLACK_KB);                          \
        tsapi_mi_put(mi, "op", op++);                                   \
        tsapi_mi_put_type(mi, TE_MI_MEAS_MEMORY, "rss_growth",       \
                          (double)(st.maxrss_kb - base_kb) * 1024,      \
                          TE_MI_MEAS_MULTIPLIER_PLAIN);                 \
        tsapi_mi_put_type(mi, TE_MI_MEAS_MEMORY, "bound",            \
                          (double)(limit_kb + SLACK_KB) * 1024,         \
                          TE_MI_MEAS_MULTIPLIER_PLAIN);                 \
        tsapi_mi_put_type(mi, TE_MI_MEAS_MEMORY, "file",             \
                          (double)gen.bytes,                            \
                          TE_MI_MEAS_MULTIPLIER_PLAIN);                 \
    } while (0)

    TEST_STEP("Count every row");
    CHECK_RC(tapi_data_count(ds, prm.ptr, &cnt));
    if (cnt.rows != rows)
        TEST_VERDICT("Counted %llu rows of %u", (unsigned long long)cnt.rows,
                     rows);
    MEASURE("count");

    TEST_STEP("Profile every column");
    CHECK_RC(tapi_data_profile(ds, prm.ptr, &prof));
    tapi_data_profile_log(&prof);
    if (prof.mem_peak > (uint64_t)limit_kb * 1024)
        TEST_VERDICT("The profile's working set reached %llu bytes",
                     (unsigned long long)prof.mem_peak);
    MEASURE("profile");

    TEST_STEP("Unique id (exact, partitioned on disk)");
    CHECK_RC(tapi_data_check_unique(ds, "id", prm.ptr, NULL, &chk));
    if (!chk.passed || chk.checked != rows)
        TEST_VERDICT("A sequence column has %llu duplicates",
                     (unsigned long long)chk.violations);
    RING("unique: %s partitions", tapi_data_kv_get(&chk.kv, "partitions",
                                                    "?"));
    MEASURE("unique");

    TEST_STEP("Compare the dataset with itself, unordered");
    CHECK_RC(tapi_data_open(sess.pco, p.ptr, format, NULL, schema, &other));
    {
        te_string cp = TE_STRING_INIT;

        te_string_append(&cp, "mode=unordered;%s", prm.ptr);
        CHECK_RC(tapi_data_compare(ds, other, cp.ptr, &d));
        te_string_free(&cp);
    }
    if (d.equal != 1 || d.matched != rows)
        TEST_VERDICT("The dataset differs from itself");
    RING("compare: %u partitions, %llu bytes peak", d.partitions,
         (unsigned long long)d.mem_peak);
    MEASURE("compare");
#undef MEASURE

    te_mi_logger_add_comment(mi, NULL, "op",
                             "0 count, 1 profile, 2 unique, 3 compare");
    te_mi_logger_add_meas_key(mi, NULL, "rows", "%u", rows);
    te_mi_logger_add_meas_key(mi, NULL, "mem_limit", "%s", mem_limit);
    tsapi_mi_graph_type(mi, "rss", "RPC server peak RSS growth vs the "
                        "memory bound and the file size (bytes)", "op",
                        TE_MI_MEAS_MEMORY, "rss_growth,bound,file");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_diff_free(&d);
    tapi_data_check_free(&chk);
    tapi_data_profile_free(&prof);
    tapi_data_counts_free(&cnt);
    tapi_data_close(other);
    tapi_data_close(ds);
    tapi_data_gen_free(&gen);
    free(schema);
    te_string_free(&p);
    te_string_free(&prm);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
