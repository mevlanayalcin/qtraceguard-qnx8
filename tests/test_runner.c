/*
 * QTraceGuard - Comprehensive C11 Unit & Integration Test Suite
 * SPDX-License-Identifier: MIT
 */

#include "qtg_analyzer.h"
#include "qtg_kev.h"
#include "qtg_resmgr_demo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        g_fail++; \
        return; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    int before = g_fail; \
    fn(); \
    if (g_fail == before) { \
        printf("  [PASS] %s\n", #fn); \
        g_pass++; \
    } \
} while (0)

static void test_kev_binary_roundtrip_and_combine_smp(void) {
    const char *path = "out/test_roundtrip.kev";
    qtg_kev_header_t hdr;
    qtg_kev_header_defaults(&hdr);
    hdr.cycles_per_sec = 2000000000ULL; /* 2 GHz */
    hdr.cpu_num = 8U;
    hdr.little_endian = true;

    qtg_kev_writer_t w;
    ASSERT_TRUE(qtg_kev_writer_open(&w, path, &hdr) == 0, "writer open");

    /* Emit simple event + combine event + timestamp crossing 32-bit boundary! */
    ASSERT_TRUE(qtg_kev_writer_write_simple(&w, 1U, QTG_CLASS_USER, QTG_USER_MARKER,
                                            1000000ULL, 0xAAU, 0xBBU) == 0, "simple write");
    ASSERT_TRUE(qtg_kev_writer_write_name(&w, 2U, QTG_CLASS_PR_TH, QTG_PR_TH_CREATE_P_NAME,
                                          2000000ULL, 1U, 101U, "lidar_resmgr") == 0, "name write");
    /* Timestamp > 2^32 triggers CONTROL_TIME upper-word sync */
    uint64_t hi_ts = (2ULL << 32) + 4000000ULL;
    uint32_t payload[6] = { 7U, 102U, 101U, 1U, 1U, 50U };
    ASSERT_TRUE(qtg_kev_writer_write_combine(&w, 3U, QTG_CLASS_COMM, QTG_COMM_SMSG,
                                             hi_ts, payload, 6U) == 0, "combine write");
    ASSERT_TRUE(qtg_kev_writer_close(&w) == 0, "writer close");

    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[128] = {0};
    ASSERT_TRUE(qtg_kev_read_file(path, &tr, err, sizeof(err)) == 0, "kev read");
    ASSERT_TRUE(tr.header.cycles_per_sec == 2000000000ULL, "cycles_per_sec match");
    ASSERT_TRUE(tr.header.cpu_num == 8U, "cpu_num match");
    ASSERT_TRUE(tr.count == 3U, "3 decoded events");
    ASSERT_TRUE(tr.combine_events_reassembled == 2U, "2 combine events reassembled");

    ASSERT_TRUE(tr.events[0].cpu == 1U && tr.events[0].timestamp_ns == 500000ULL, "event 0 ns");
    ASSERT_TRUE(tr.events[0].words[0] == 0xAAU && tr.events[0].words[1] == 0xBBU, "event 0 payload");

    char pname[64] = {0};
    qtg_extract_ascii_from_words(&tr.events[1].words[2], tr.events[1].word_count - 2U, pname, sizeof(pname));
    ASSERT_TRUE(strcmp(pname, "lidar_resmgr") == 0, "extracted combine proc name");

    ASSERT_TRUE(tr.events[2].cpu == 3U && tr.events[2].timestamp_cycles == hi_ts, "64-bit cycle rollover");
    ASSERT_TRUE(tr.events[2].word_count == 6U && tr.events[2].words[5] == 50U, "6-word combine payload");

    qtg_trace_free(&tr);
}

static void test_kev_big_endian_swap(void) {
    const char *path = "out/test_be.kev";
    qtg_kev_header_t hdr;
    qtg_kev_header_defaults(&hdr);
    hdr.little_endian = false; /* Force big-endian file */
    hdr.cycles_per_sec = 1000000000ULL;

    qtg_kev_writer_t w;
    ASSERT_TRUE(qtg_kev_writer_open(&w, path, &hdr) == 0, "be writer open");
    ASSERT_TRUE(qtg_kev_writer_write_simple(&w, 2U, QTG_CLASS_COMM, QTG_COMM_SMSG,
                                            123456ULL, 9U, 202U) == 0, "be simple write");
    ASSERT_TRUE(qtg_kev_writer_close(&w) == 0, "be writer close");

    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[128] = {0};
    ASSERT_TRUE(qtg_kev_read_file(path, &tr, err, sizeof(err)) == 0, "be kev read");
    ASSERT_TRUE(!tr.header.little_endian, "big endian parsed");
    ASSERT_TRUE(tr.count == 1U, "1 event");
    ASSERT_TRUE(tr.events[0].cpu == 2U && tr.events[0].words[0] == 9U && tr.events[0].words[1] == 202U,
                "byte-swapped fields match");
    qtg_trace_free(&tr);
}

static void test_traceprinter_text_import(void) {
    const char *path = "out/test_tp.txt";
    FILE *fp = fopen(path, "w");
    ASSERT_TRUE(fp != NULL, "create traceprinter fixture");
    fprintf(fp,
        "TRACEPRINTER version 1.02\n"
        "-- HEADER FILE INFORMATION --\n"
        "TRACE_CYCLES_PER_SEC:: 1000000000\n"
        "TRACE_CPU_NUM:: 2\n"
        "TRACE_SYSNAME:: QNX\n"
        "TRACE_SYS_RELEASE:: 8.0.0\n"
        "-- KERNEL EVENTS --\n"
        "t:0x00001000 CPU:00 PR_TH :PROCCREATE_NAME ppid:1 pid:10 name:alpha_rm\n"
        "t:0x00001100 CPU:00 PR_TH :PROCCREATE_NAME ppid:1 pid:20 name:beta_rm\n"
        "t:0x00002000 CPU:00 COMM :SND_MESSAGE chid:1 pid:20 cpid:10 ctid:1 stid:1 prio:40\n"
        "t:0x00003000 CPU:01 COMM :SND_MESSAGE chid:2 pid:10 cpid:20 ctid:1 stid:1 prio:40\n");
    fclose(fp);

    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[128] = {0};
    ASSERT_TRUE(qtg_traceprinter_read_file(path, &tr, err, sizeof(err)) == 0, "read traceprinter");
    ASSERT_TRUE(tr.count == 4U, "4 traceprinter events");

    qtg_report_t *rep = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
    ASSERT_TRUE(rep != NULL, "alloc report");
    ASSERT_TRUE(qtg_analyze_trace(&tr, NULL, rep) == 0, "analyze traceprinter");
    ASSERT_TRUE(rep->cycle_count == 1U, "2-node deadlock cycle detected from traceprinter text");
    ASSERT_TRUE(rep->cycles[0].length == 2U, "cycle length == 2");
    ASSERT_TRUE(!rep->sla.passed, "SLA fails on deadlock");

    free(rep);
    qtg_trace_free(&tr);
}

static void test_scenario_healthy_passes_sla(void) {
    const char *path = "out/unit_healthy.kev";
    ASSERT_TRUE(qtg_run_demo_scenario(QTG_SCEN_HEALTHY, path, 14U) == 0, "run healthy scenario");

    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[128] = {0};
    ASSERT_TRUE(qtg_kev_read_file(path, &tr, err, sizeof(err)) == 0, "read healthy kev");
    ASSERT_TRUE(tr.count > 50U, "non-trivial event stream");

    qtg_report_t *rep = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
    qtg_sla_policy_t pol;
    qtg_sla_policy_defaults(&pol);
    ASSERT_TRUE(qtg_analyze_trace(&tr, &pol, rep) == 0, "analyze healthy");
    ASSERT_TRUE(rep->cycle_count == 0U, "zero deadlock cycles");
    ASSERT_TRUE(rep->inversion_count == 0U, "zero priority inversions");
    ASSERT_TRUE(rep->ipc_rtt_p99_ns > 0ULL, "measured IPC RTT");
    ASSERT_TRUE(rep->sla.passed, "healthy scenario passes SLA");

    free(rep);
    qtg_trace_free(&tr);
}

static void test_scenario_deadlock_3node_cycle(void) {
    const char *path = "out/unit_deadlock.kev";
    ASSERT_TRUE(qtg_run_demo_scenario(QTG_SCEN_DEADLOCK, path, 1U) == 0, "run deadlock scenario");

    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[128] = {0};
    ASSERT_TRUE(qtg_kev_read_file(path, &tr, err, sizeof(err)) == 0, "read deadlock kev");

    qtg_report_t *rep = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
    ASSERT_TRUE(qtg_analyze_trace(&tr, NULL, rep) == 0, "analyze deadlock");
    ASSERT_TRUE(rep->cycle_count == 1U, "exactly 1 canonical 3-node cycle detected");
    ASSERT_TRUE(rep->cycles[0].length == 3U, "cycle length == 3 (sensor->planner->actuator->sensor)");
    ASSERT_TRUE(rep->cycles[0].pids[0] == 101U && rep->cycles[0].pids[1] == 102U &&
                rep->cycles[0].pids[2] == 103U, "exact PIDs 101->102->103 in cycle");
    ASSERT_TRUE(!rep->sla.passed, "deadlock fails SLA");

    free(rep);
    qtg_trace_free(&tr);
}

static void test_scenario_fixed_priority_inversion(void) {
    const char *path = "out/unit_prio_inv.kev";
    ASSERT_TRUE(qtg_run_demo_scenario(QTG_SCEN_PRIO_INVERSION, path, 6U) == 0, "run prio-inversion");

    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[128] = {0};
    ASSERT_TRUE(qtg_kev_read_file(path, &tr, err, sizeof(err)) == 0, "read prio-inv kev");

    qtg_report_t *rep = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
    ASSERT_TRUE(qtg_analyze_trace(&tr, NULL, rep) == 0, "analyze prio-inv");
    ASSERT_TRUE(rep->inversion_count >= 1U, "detected priority inversion");
    bool saw_fixed = false;
    for (size_t i = 0; i < rep->inversion_count; i++) {
        if (rep->inversions[i].kind == QTG_INV_FIXED_PRIO_CHANNEL &&
            rep->inversions[i].duration_ns >= 350000ULL) {
            saw_fixed = true;
        }
    }
    ASSERT_TRUE(saw_fixed, "classified as _NTO_CHF_FIXED_PRIORITY inversion with >=350us duration");
    ASSERT_TRUE(!rep->sla.passed, "prio-inversion fails SLA");

    free(rep);
    qtg_trace_free(&tr);
}

static void test_scenario_qnx8_aid_priority_cap(void) {
    const char *path = "out/unit_aid_cap.kev";
    ASSERT_TRUE(qtg_run_demo_scenario(QTG_SCEN_AID_CAP, path, 6U) == 0, "run aid-cap");

    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[128] = {0};
    ASSERT_TRUE(qtg_kev_read_file(path, &tr, err, sizeof(err)) == 0, "read aid-cap kev");

    qtg_report_t *rep = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
    ASSERT_TRUE(qtg_analyze_trace(&tr, NULL, rep) == 0, "analyze aid-cap");
    bool saw_cap = false;
    for (size_t i = 0; i < rep->inversion_count; i++) {
        if (rep->inversions[i].kind == QTG_INV_AID_PRIORITY_CAP &&
            rep->inversions[i].server_prio == 63U &&
            rep->inversions[i].high_prio > 63U) {
            saw_cap = true;
        }
    }
    ASSERT_TRUE(saw_cap, "detected QNX 8.0 PROCMGR_AID_PRIORITY ceiling cap at 63");
    ASSERT_TRUE(!rep->sla.passed, "aid-cap fails SLA");

    free(rep);
    qtg_trace_free(&tr);
}

static void test_scenario_pulse_storm_leak(void) {
    const char *path = "out/unit_pulse.kev";
    ASSERT_TRUE(qtg_run_demo_scenario(QTG_SCEN_PULSE_STORM, path, 1U) == 0, "run pulse-storm");

    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[128] = {0};
    ASSERT_TRUE(qtg_kev_read_file(path, &tr, err, sizeof(err)) == 0, "read pulse-storm kev");

    qtg_report_t *rep = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
    ASSERT_TRUE(qtg_analyze_trace(&tr, NULL, rep) == 0, "analyze pulse-storm");
    ASSERT_TRUE(rep->pulse_count >= 1U, "pulse channel tracked");
    ASSERT_TRUE(rep->pulses[0].sent_count == 40ULL && rep->pulses[0].received_count == 4ULL,
                "40 sent / 4 received");
    ASSERT_TRUE(rep->pulses[0].leak_detected, "pulse queue saturation flagged");
    ASSERT_TRUE(!rep->sla.passed, "pulse-storm fails SLA");

    free(rep);
    qtg_trace_free(&tr);
}

static void test_scenario_aps_to_qnx8_sporadic_advisor(void) {
    const char *path = "out/unit_aps.kev";
    ASSERT_TRUE(qtg_run_demo_scenario(QTG_SCEN_APS_LEGACY, path, 6U) == 0, "run aps-legacy");

    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[128] = {0};
    ASSERT_TRUE(qtg_kev_read_file(path, &tr, err, sizeof(err)) == 0, "read aps-legacy kev");

    qtg_report_t *rep = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
    ASSERT_TRUE(qtg_analyze_trace(&tr, NULL, rep) == 0, "analyze aps-legacy");
    ASSERT_TRUE(rep->aps_count == 3U, "parsed 3 legacy QNX 7.1 APS partitions");
    const qtg_aps_migration_t *adas = &rep->aps_migrations[1];
    ASSERT_TRUE(strcmp(adas->name, "ADAS_Control") == 0, "ADAS_Control partition parsed");
    ASSERT_TRUE(adas->bankruptcy_events == 1U, "APS bankruptcy recorded");
    ASSERT_TRUE(adas->rec_init_budget_ms == 60U, "50ms + 20% bankruptcy safety headroom -> 60ms");
    ASSERT_TRUE(adas->requires_aid_priority, "critical partition prio 72 flags PROCMGR_AID_PRIORITY");
    ASSERT_TRUE(!rep->sla.passed, "APS bankruptcy fails SLA");

    free(rep);
    qtg_trace_free(&tr);
}

static void test_fail_closed_empty_focus_and_dropped(void) {
    /* 1. Empty trace must fail closed */
    qtg_trace_t empty;
    qtg_trace_init(&empty);
    qtg_report_t *rep = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
    ASSERT_TRUE(qtg_analyze_trace(&empty, NULL, rep) == 0, "analyze empty trace");
    ASSERT_TRUE(!rep->sla.passed, "empty trace fails closed");

    /* 2. Focus typo matching 0 threads must fail closed */
    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[128] = {0};
    ASSERT_TRUE(qtg_kev_read_file("out/unit_healthy.kev", &tr, err, sizeof(err)) == 0, "read unit_healthy.kev");
    qtg_sla_policy_t pol;
    qtg_sla_policy_defaults(&pol);
    snprintf(pol.focus_proc, sizeof(pol.focus_proc), "nonexistent_proc_xyz");
    ASSERT_TRUE(qtg_analyze_trace(&tr, &pol, rep) == 0, "analyze focus mismatch");
    ASSERT_TRUE(rep->matched_focus_threads == 0U && !rep->sla.passed, "focus mismatch fails closed");

    /* 3. Dropped tracelogger buffers must fail closed */
    tr.dropped_buffer_events = 2ULL;
    qtg_sla_policy_defaults(&pol);
    ASSERT_TRUE(qtg_analyze_trace(&tr, &pol, rep) == 0, "analyze dropped buffers");
    ASSERT_TRUE(!rep->sla.passed, "dropped trace buffers fail closed");

    free(rep);
    qtg_trace_free(&tr);
    qtg_trace_free(&empty);
}

static void test_smp_per_cpu_control_time_ordering(void) {
    const char *path = "out/unit_smp_ctrl.kev";
    qtg_kev_writer_t w;
    ASSERT_TRUE(qtg_kev_writer_open(&w, path, NULL) == 0, "open smp ctrl kev");
    /* CPU0 emits at 0x1_00000010, then CPU1 (flushed out of order) emits at 0x0_FFFFFF00 */
    ASSERT_TRUE(qtg_kev_writer_write_simple(&w, 0U, QTG_CLASS_PR_TH, QTG_TH_RUNNING,
                                            0x100000010ULL, 100U, 1U) == 0, "write cpu0 hi=1");
    ASSERT_TRUE(qtg_kev_writer_write_simple(&w, 1U, QTG_CLASS_PR_TH, QTG_TH_RUNNING,
                                            0x0FFFFFF00ULL, 200U, 1U) == 0, "write cpu1 hi=0");
    ASSERT_TRUE(qtg_kev_writer_close(&w) == 0, "close smp ctrl kev");

    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[128] = {0};
    ASSERT_TRUE(qtg_kev_read_file(path, &tr, err, sizeof(err)) == 0, "read smp ctrl kev");
    ASSERT_TRUE(tr.count == 2U, "2 events decoded");
    ASSERT_TRUE(tr.events[0].cpu == 1U && tr.events[0].timestamp_cycles == 0x0FFFFFF00ULL,
                "CPU1 earlier timestamp sorted first");
    ASSERT_TRUE(tr.events[1].cpu == 0U && tr.events[1].timestamp_cycles == 0x100000010ULL,
                "CPU0 later timestamp sorted second");
    qtg_trace_free(&tr);
}

static void test_no_self_edge_false_deadlock(void) {
    qtg_trace_t tr;
    qtg_trace_init(&tr);
    qtg_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.timestamp_cycles = 1000ULL;
    ev.timestamp_ns = 1000ULL;
    ev.cpu = 0U;
    ev.event_class = QTG_CLASS_COMM;
    ev.event_id = QTG_COMM_SMSG;
    ev.is_combine = true;
    ev.word_count = 6U;
    ev.words[0] = 1U; ev.words[1] = 500U; ev.words[2] = 500U; ev.words[3] = 1U;
    ev.words[4] = 0U; /* unknown srv_tid on same PID must NOT become self-edge (500,1)->(500,1) */
    ev.words[5] = 30U;
    ASSERT_TRUE(qtg_trace_push(&tr, &ev) == 0, "push self-pid smsg");

    qtg_report_t *rep = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
    ASSERT_TRUE(qtg_analyze_trace(&tr, NULL, rep) == 0, "analyze self-pid smsg");
    ASSERT_TRUE(rep->cycle_count == 0U, "no false 1-node self-edge cycle");
    free(rep);
    qtg_trace_free(&tr);
}

static void test_traceprinter_token_boundary_and_sarif_qtg004(void) {
    const char *tp =
        "TRACE_CYCLES_PER_SEC:: 1000000000\n"
        "t:0x00001000 CPU:00 PROCESS :PROCCREATE_NAME ppid:1 pid:4242 name:victim_rm\n"
        "t:0x00002000 CPU:00 THREAD  :THREADY pid:4242 tid:1 prio:50\n"
        "t:0x00090000 CPU:00 THREAD  :THRUNNING pid:4242 tid:1 prio:50\n";
    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[128] = {0};
    ASSERT_TRUE(qtg_traceprinter_read_buffer(tp, strlen(tp), &tr, err, sizeof(err)) == 0, "parse tp buf");

    qtg_report_t *rep = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
    ASSERT_TRUE(qtg_analyze_trace(&tr, NULL, rep) == 0, "analyze tp buf");
    ASSERT_TRUE(strcmp(qtg_report_proc_name(rep, 4242U), "victim_rm") == 0,
                "token-boundary pid:4242 gets name victim_rm (not ppid:1)");
    ASSERT_TRUE(strcmp(qtg_report_proc_name(rep, 1U), "victim_rm") != 0,
                "ppid:1 not clobbered by victim_rm");
    ASSERT_TRUE(!rep->sla.passed, "dispatch max latency breach fails SLA");
    ASSERT_TRUE(qtg_report_write_sarif(rep, "tp.txt", "out/unit_tp.sarif") == 0, "write tp sarif");
    FILE *fp = fopen("out/unit_tp.sarif", "r");
    ASSERT_TRUE(fp != NULL, "open unit_tp.sarif");
    char sbuf[4096] = {0};
    size_t n = fread(sbuf, 1U, sizeof(sbuf) - 1U, fp);
    fclose(fp);
    (void)n;
    ASSERT_TRUE(strstr(sbuf, "QTG004") != NULL, "SARIF includes QTG004 latency violation");

    free(rep);
    qtg_trace_free(&tr);
}

int main(void) {
    if (mkdir("out", 0755) != 0 && errno != EEXIST) {
        perror("mkdir out");
        return 1;
    }
    printf("=== QTraceGuard C11 Unit & Integration Test Suite ===\n");
    RUN_TEST(test_kev_binary_roundtrip_and_combine_smp);
    RUN_TEST(test_kev_big_endian_swap);
    RUN_TEST(test_traceprinter_text_import);
    RUN_TEST(test_scenario_healthy_passes_sla);
    RUN_TEST(test_scenario_deadlock_3node_cycle);
    RUN_TEST(test_scenario_fixed_priority_inversion);
    RUN_TEST(test_scenario_qnx8_aid_priority_cap);
    RUN_TEST(test_scenario_pulse_storm_leak);
    RUN_TEST(test_scenario_aps_to_qnx8_sporadic_advisor);
    RUN_TEST(test_fail_closed_empty_focus_and_dropped);
    RUN_TEST(test_smp_per_cpu_control_time_ordering);
    RUN_TEST(test_no_self_edge_false_deadlock);
    RUN_TEST(test_traceprinter_token_boundary_and_sarif_qtg004);

    printf("\nResult: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
