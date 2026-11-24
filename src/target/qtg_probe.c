/*
 * QTraceGuard - QNX OS 8.0 Target Diagnostic Probe (`qtg-probe`)
 * SPDX-License-Identifier: MIT
 *
 * Runs directly on a QNX OS 8.0 target (QEMU / Raspberry Pi 5) or POSIX host to:
 *  1. Probe live QNX 8.0 kernel capabilities (live SchedCtl ENOSYS confirmation,
 *     SCHED_SPORADIC availability, empirical PROCMGR_AID_PRIORITY ceiling check).
 *  2. Execute a deterministic IPC round-trip microbenchmark (MsgSend -> MsgReceive -> MsgReply).
 *  3. Emit JSON or human-readable target health telemetry.
 */

#define _POSIX_C_SOURCE 200809L
#if defined(__QNX__) || defined(__QNXNTO__)
#ifndef _QNX_SOURCE
#define _QNX_SOURCE
#endif
#endif

#include "qtg_rtos_shim.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__QNX__) || defined(__QNXNTO__)
#include <sys/neutrino.h>
#endif

typedef struct bench_ctx {
    uint32_t srv_pid;
    uint32_t cli_pid;
    int      chid;
    uint32_t iters;
} bench_ctx_t;

static void *echo_server(void *arg) {
    bench_ctx_t *ctx = (bench_ctx_t *)arg;
#if defined(__QNX__) || defined(__QNXNTO__)
    qtg_shim_register_thread(ctx->srv_pid, 1U, "echo_srv", 40U, 1U);
#endif
    union {
        uint32_t         words[4];
        qtg_shim_pulse_t pulse;
#if defined(__QNX__) || defined(__QNXNTO__)
        struct _pulse    qnx_pulse;
#endif
    } buf;

    uint32_t handled = 0U;
    while (handled < ctx->iters) {
        int64_t rcvid = qtg_shim_msg_receive(ctx->srv_pid, 1U, ctx->chid, &buf, sizeof(buf), NULL, 500U);
        if (rcvid < 0) break;
        if (rcvid == 0) {
#if defined(__QNX__) || defined(__QNXNTO__)
            if (buf.qnx_pulse.code == _PULSE_CODE_UNBLOCK) {
                (void)qtg_shim_msg_error(ctx->srv_pid, 1U, (int64_t)buf.qnx_pulse.value.sival_int, EINTR);
            }
#endif
            continue;
        }
        buf.words[0] ^= 0x5A5A5A5AU;
        (void)qtg_shim_msg_reply(ctx->srv_pid, 1U, rcvid, 0, buf.words, sizeof(buf.words));
        handled++;
    }
    return NULL;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y ? 1 : 0);
}

static void print_json_escaped(const char *s) {
    putchar('"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { putchar('\\'); putchar((int)c); }
        else if (c >= 0x20U && c < 0x7FU) { putchar((int)c); }
        else { printf("\\u%04x", (unsigned int)c); }
    }
    putchar('"');
}

int main(int argc, char **argv) {
    bool json_mode = false;
    uint32_t iters = 200U;
    const char *kev_out = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json_mode = true;
        else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) iters = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) kev_out = argv[++i];
    }
    if (iters < 10U) iters = 10U;
    if (iters > 5000U) iters = 5000U;

    qtg_shim_aps_probe_t probe;
    qtg_shim_probe_runtime(&probe);

    if (qtg_shim_init(kev_out, 4U) != 0) {
        fprintf(stderr, "qtg-probe: failed to initialize shim\n");
        return 1;
    }

    bench_ctx_t b;
    b.srv_pid = qtg_shim_register_proc(201U, "qtg_probe_srv", true);
    b.cli_pid = qtg_shim_register_proc(202U, "qtg_probe_cli", true);
#if !defined(__QNX__) && !defined(__QNXNTO__)
    qtg_shim_register_thread(b.srv_pid, 1U, "echo_srv", 40U, 1U);
#endif
    qtg_shim_register_thread(b.cli_pid, 1U, "echo_cli", 45U, 0U);
    b.chid = qtg_shim_channel_create(b.srv_pid, 1U, QTG_SHIM_CHF_UNBLOCK, "/dev/qtg_probe");
    b.iters = iters;

    pthread_t srv_th;
    if (pthread_create(&srv_th, NULL, echo_server, &b) != 0) {
        fprintf(stderr, "qtg-probe: pthread_create failed\n");
        qtg_shim_shutdown();
        return 1;
    }

    int coid = qtg_shim_connect_attach(b.srv_pid, b.chid);
    uint64_t *rtts = (uint64_t *)calloc(iters, sizeof(uint64_t));
    if (!rtts) {
        qtg_shim_connect_detach(coid);
        pthread_join(srv_th, NULL);
        qtg_shim_shutdown();
        return 1;
    }
    uint32_t ok_cnt = 0U;
    for (uint32_t i = 0; i < iters; i++) {
        uint32_t req[4] = { i, 0x1234U, 0U, 0U };
        uint32_t rep[4] = { 0 };
        uint64_t t0 = qtg_shim_now_ns();
        int rc = qtg_shim_msg_send(b.cli_pid, 1U, coid, req, sizeof(req), rep, sizeof(rep), 500U);
        uint64_t t1 = qtg_shim_now_ns();
        if (rc == 0 && rep[0] == (i ^ 0x5A5A5A5AU)) {
            rtts[ok_cnt++] = t1 - t0;
        }
    }
    qtg_shim_connect_detach(coid);
    pthread_join(srv_th, NULL);
    qtg_shim_shutdown();

    uint64_t p50 = 0, p99 = 0, max_ns = 0;
    if (ok_cnt > 0U) {
        qsort(rtts, ok_cnt, sizeof(uint64_t), cmp_u64);
        p50 = rtts[ok_cnt / 2U];
        p99 = rtts[(size_t)((double)(ok_cnt - 1U) * 0.99)];
        max_ns = rtts[ok_cnt - 1U];
    }
    free(rtts);

    if (json_mode) {
        printf("{\n"
               "  \"tool\": \"qtg-probe\",\n"
               "  \"qnx_native\": %s,\n"
               "  \"kernel_release\": ",
               probe.qnx_native ? "true" : "false");
        print_json_escaped(probe.kernel_release);
        printf(",\n"
               "  \"aps_supported\": %s,\n"
               "  \"aps_errno\": %d,\n"
               "  \"sporadic_supported\": %s,\n"
               "  \"has_aid_priority\": %s,\n"
               "  \"max_unpriv_priority\": %u,\n"
               "  \"ipc_bench\": {\"iters\": %u, \"succeeded\": %u, \"p50_ns\": %llu, \"p99_ns\": %llu, \"max_ns\": %llu}\n"
               "}\n",
               probe.aps_supported ? "true" : "false",
               probe.aps_errno,
               probe.sporadic_supported ? "true" : "false",
               probe.has_aid_priority ? "true" : "false",
               probe.max_unpriv_priority,
               iters, ok_cnt,
               (unsigned long long)p50, (unsigned long long)p99, (unsigned long long)max_ns);
    } else {
        printf("=== QTraceGuard Target Probe (`qtg-probe`) ===\n");
        printf("Runtime             : %s (%s)\n", probe.kernel_release,
               probe.qnx_native ? "NATIVE QNX OS 8.0" : "POSIX CI Simulation");
        printf("Legacy APS Support  : %s (errno=%d => %s)\n",
               probe.aps_supported ? "YES" : "NO", probe.aps_errno,
               probe.aps_supported ? "legacy 7.x APS active" : "QNX 8.0 expected ENOSYS; use SCHED_SPORADIC");
        printf("SCHED_SPORADIC      : %s\n", probe.sporadic_supported ? "AVAILABLE" : "UNAVAILABLE");
        printf("PROCMGR_AID_PRIORITY: %s (unpriv ceiling=%u)\n",
               probe.has_aid_priority ? "GRANTED" : "NOT GRANTED (capped at 63)",
               probe.max_unpriv_priority);
        printf("IPC Microbenchmark  : %u/%u MsgSend/MsgReply OK | p50=%.2f us | p99=%.2f us | max=%.2f us\n",
               ok_cnt, iters, (double)p50 / 1000.0, (double)p99 / 1000.0, (double)max_ns / 1000.0);
    }
    return (ok_cnt == iters) ? 0 : 1;
}
