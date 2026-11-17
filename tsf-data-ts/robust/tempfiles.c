/** @file
 * @brief Robust group
 *
 * Temporary files are safe and do not outlive their process. While a
 * partitioned comparison of a large dataset is running, a second RPC
 * server on the same agent looks at the first one from outside: its
 * spill files must already be unlinked (the spill directory is empty,
 * yet the process holds open "(deleted)" files) and readable by their
 * owner only (mode 0600). Then the comparison either finishes, or the
 * RPC server is killed outright mid-operation; in both cases nothing is
 * left in the spill directory, because there was never anything to
 * leave.
 *
 * Copyright (C) 2026 Interpretica Unipessoal Lda
 */

#define TE_TEST_NAME    "robust/tempfiles"

#include "te_config.h"
#include "tapi_test.h"
#include "te_string.h"
#include "tapi_file.h"
#include "tapi_rpc_unistd.h"
#include "tapi_rpc_signal.h"

#include "tapi_data.h"
#include "tsapi_data.h"

int
main(int argc, char **argv)
{
    tsapi_data_session sess = { 0 };
    const char *end;
    rcf_rpc_server *aux = NULL;
    tapi_data_gen gen = { 0 };
    tapi_data *a = NULL;
    tapi_data *b = NULL;
    tapi_data_diff d = TAPI_DATA_DIFF_INIT;
    te_mi_logger *mi = NULL;
    te_string p = TE_STRING_INIT;
    te_string spill = TE_STRING_INIT;
    te_string out = TE_STRING_INIT;
    te_string prm = TE_STRING_INIT;
    char *seen = NULL;
    char *schema = NULL;
    pid_t pid;
    te_bool killed = false;

    TEST_START;
    TEST_GET_STRING_PARAM(end);

    TEST_STEP("Open a session and generate a dataset worth spilling");
    CHECK_RC(tsapi_data_session_init(&sess, "pco_robust_tmp"));
    if (!tapi_data_available(sess.pco))
        TEST_SKIP("The agent has no dataset engine");
    rc = tsapi_data_fixtures(&sess);
    if (TE_RC_GET_ERROR(rc) == TE_EOPNOTSUPP)
        TEST_SKIP("The agent has no python3 for the fixtures");
    CHECK_RC(rc);
    if (tsapi_data_sh(&sess, "test -d /proc/self/fd") != 0)
        TEST_SKIP("The agent has no /proc to look into a process with");
    tsapi_data_work(&sess, "spill", &spill);
    tsapi_data_work(&sess, "seen.txt", &out);
    CHECK_RC(tsapi_data_sh(&sess, "mkdir -p %s", spill.ptr));
    tsapi_data_work(&sess, "big.csv", &p);
    CHECK_RC(tapi_data_generate(sess.pco, p.ptr, "csv",
                                "id:int:seq;v:string:uuid;w:float:uniform",
                                "rows=1500000;seed=4", &gen));
    schema = tapi_data_unescape(gen.schema);
    CHECK_RC(tapi_data_open(sess.pco, p.ptr, NULL, NULL, schema, &a));
    CHECK_RC(tapi_data_open(sess.pco, p.ptr, NULL, NULL, schema, &b));
    CHECK_RC(rcf_rpc_server_create(sess.ta, "pco_robust_aux", &aux));
    pid = rpc_getpid(sess.pco);
    mi = tsapi_mi_open();

    TEST_STEP("Start a partitioned comparison without waiting for it");
    te_string_append(&prm, "mode=unordered;mem_limit=1m;tmpdir=%s",
                     spill.ptr);
    sess.pco->op = RCF_RPC_CALL;
    CHECK_RC(tapi_data_compare(a, b, prm.ptr, &d));

    TEST_STEP("From outside: spill files open, unlinked, mode 0600");
    {
        te_string cmd = TE_STRING_INIT;
        rpc_wait_status st;

        /* Up to a minute: wait for the first spill file, then judge. */
        te_string_append(&cmd,
            "for i in $(seq 1 600); do "
            "n=$(ls -l /proc/%d/fd 2>/dev/null | "
            "grep -c 'ta_data_spill_.*(deleted)'); "
            "if [ \"$n\" -gt 0 ]; then "
            "[ -z \"$(ls -A %s)\" ] || exit 3; "
            "for f in /proc/%d/fd/*; do "
            "case \"$(readlink $f)\" in *ta_data_spill_*) "
            "[ \"$(stat -L -c %%a $f)\" = 600 ] || exit 4;; esac; done; "
            "echo $n > %s; exit 0; fi; sleep 0.1; done; exit 2",
            (int)pid, spill.ptr, (int)pid, out.ptr);
        RPC_AWAIT_IUT_ERROR(aux);
        aux->timeout = 120000;
        st = rpc_system(aux, cmd.ptr);
        te_string_free(&cmd);
        if (st.flag != RPC_WAIT_STATUS_EXITED)
            TEST_FAIL("The observer did not finish");
        switch (st.value)
        {
            case 0:
                break;
            case 2:
                TEST_VERDICT("No spill file was ever seen open");
            case 3:
                TEST_VERDICT("Spill files are visible in the directory "
                             "while in use");
            case 4:
                TEST_VERDICT("A spill file is readable beyond its owner");
            default:
                TEST_FAIL("The observer failed (%u)", st.value);
        }
        CHECK_RC(tapi_file_read_ta(sess.ta, out.ptr, &seen));
        RING("%d unlinked 0600 spill files held open by the RPC server",
             atoi(seen));
        tsapi_mi_put(mi, "phase", 0);
        tsapi_mi_put(mi, "spill_fds", atoi(seen));
    }

    if (strcmp(end, "kill") == 0)
    {
        TEST_STEP("Kill the RPC server mid-operation");
        CHECK_RC(rpc_kill(aux, pid, RPC_SIGKILL));
        killed = true;
        CHECK_RC(rcf_rpc_server_restart(sess.pco));
        tapi_data_forget(a);
        tapi_data_forget(b);
        a = b = NULL;
    }
    else
    {
        TEST_STEP("Let it finish");
        sess.pco->op = RCF_RPC_WAIT;
        CHECK_RC(tapi_data_compare(a, b, prm.ptr, &d));
        if (d.equal != 1 || d.partitions < 2)
            TEST_VERDICT("The comparison did not finish as expected");
    }

    TEST_STEP("Nothing is left in the spill directory");
    if (tsapi_data_sh(&sess, "test -z \"$(ls -A %s)\"", spill.ptr) != 0)
        TEST_VERDICT("Spill files outlived the %s", killed ?
                     "killed process" : "operation");
    tsapi_mi_put(mi, "phase", 1);
    tsapi_mi_put(mi, "spill_fds", 0);
    tsapi_mi_graph(mi, "spill", "Spill files held open: during and after",
                   "phase", "spill_fds");
    tsapi_mi_close(&mi);

    TEST_SUCCESS;

cleanup:
    tsapi_mi_close(&mi);
    tapi_data_diff_free(&d);
    tapi_data_close(b);
    tapi_data_close(a);
    tapi_data_gen_free(&gen);
    if (aux != NULL)
        rcf_rpc_server_destroy(aux);
    free(seen);
    free(schema);
    te_string_free(&p);
    te_string_free(&spill);
    te_string_free(&out);
    te_string_free(&prm);
    tsapi_data_session_fini(&sess);
    TEST_END;
}
