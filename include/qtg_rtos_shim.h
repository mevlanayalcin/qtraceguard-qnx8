/*
 * QTraceGuard - Dual-Target QNX OS 8.0 / Portable POSIX RTOS Abstraction Layer
 * SPDX-License-Identifier: MIT
 *
 * When compiled with QNX SDP 8.0 (`qcc -Vgcc_ntox86_64` or `qcc -Vgcc_ntoaarch64le`,
 * defining __QNX__ / __QNXNTO__), this layer invokes genuine QNX OS 8.0 microkernel
 * primitives:
 *   - ChannelCreate / ConnectAttach / MsgSendv / MsgReceive / MsgReply / MsgSendPulse
 *   - Resource Manager framework (dispatch_create, iofunc_func_init, iofunc_attr_init,
 *     resmgr_attach, dispatch_block, dispatch_handler)
 *   - Process Manager Abilities (procmgr_ability with PROCMGR_AID_PRIORITY)
 *   - QNX 8.0 Sporadic Scheduler (SCHED_SPORADIC) + runtime APS ENOSYS capability probe
 *   - Kernel Instrumentation (TraceEvent user markers for tracelogger .kev capture)
 *
 * When compiled on Linux / macOS (host simulation mode), this layer provides a
 * deterministic multithreaded synchronous IPC engine that enforces authentic QNX
 * SEND/RECEIVE/REPLY semantics, priority inheritance (with optional _NTO_CHF_FIXED_PRIORITY
 * and PROCMGR_AID_PRIORITY ceiling-cap simulation), and writes spec-compliant binary
 * .kev kernel trace files directly for host verification and CI testing.
 */

#ifndef QTG_RTOS_SHIM_H
#define QTG_RTOS_SHIM_H

#include "qtg_kev.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Portable channel flags matching QNX <sys/neutrino.h> semantics */
#define QTG_SHIM_CHF_NONE           0x00000000U
#define QTG_SHIM_CHF_FIXED_PRIORITY 0x00000001U
#define QTG_SHIM_CHF_UNBLOCK        0x00000002U
#define QTG_SHIM_CHF_PRIVATE        0x00000004U

#define QTG_SHIM_PULSE_CODE_PING    10
#define QTG_SHIM_PULSE_CODE_TICK    11
#define QTG_SHIM_PULSE_CODE_UNBLOCK (-32) /* Matches _PULSE_CODE_UNBLOCK */

typedef struct qtg_shim_pulse {
    uint16_t type;
    uint16_t subtype;
    int8_t   code;
    uint8_t  zero[3];
    uint32_t value;
    int32_t  scoid;
} qtg_shim_pulse_t;

typedef struct qtg_shim_msg_info {
    uint32_t pid;
    uint32_t tid;
    uint32_t chid;
    uint32_t priority;
    size_t   msglen;
} qtg_shim_msg_info_t;

typedef struct qtg_shim_aps_probe {
    bool     qnx_native;
    bool     aps_supported;
    int      aps_errno;
    bool     sporadic_supported;
    bool     has_aid_priority;
    uint32_t max_unpriv_priority;
    char     kernel_release[64];
} qtg_shim_aps_probe_t;

/* Initialize / tear down shim runtime (if kev_out_path != NULL on POSIX, emits binary .kev) */
int  qtg_shim_init(const char *kev_out_path, uint32_t num_cpus);
void qtg_shim_shutdown(void);

/* Logical process & thread identity registration (maps cleanly to QNX pid/tid in traces) */
uint32_t qtg_shim_register_proc(uint32_t pid_hint, const char *name, bool has_aid_priority);
void     qtg_shim_register_thread(uint32_t pid, uint32_t tid, const char *name,
                                  uint32_t base_priority, uint32_t cpu);

/* QNX OS 8.0 IPC primitives */
int  qtg_shim_channel_create(uint32_t owner_pid, uint32_t owner_tid,
                             uint32_t flags, const char *resmgr_path);
int  qtg_shim_connect_attach(uint32_t target_pid, int chid);
int  qtg_shim_connect_detach(int coid);
int  qtg_shim_msg_send(uint32_t caller_pid, uint32_t caller_tid, int coid,
                       const void *smsg, size_t sbytes, void *rmsg, size_t rbytes,
                       uint32_t timeout_ms);
int64_t qtg_shim_msg_receive(uint32_t server_pid, uint32_t server_tid, int chid,
                             void *msg, size_t bytes, qtg_shim_msg_info_t *info,
                             uint32_t timeout_ms);
int     qtg_shim_msg_reply(uint32_t server_pid, uint32_t server_tid, int64_t rcvid,
                           int64_t status, const void *msg, size_t bytes);
int     qtg_shim_msg_error(uint32_t server_pid, uint32_t server_tid, int64_t rcvid, int err);
int  qtg_shim_msg_send_pulse(uint32_t caller_pid, uint32_t caller_tid, int coid,
                             int priority, int code, uint32_t value);

/* QNX OS 8.0 Capabilities, Scheduling & Tracing primitives */
int  qtg_shim_probe_runtime(qtg_shim_aps_probe_t *out);
int  qtg_shim_drop_privileges_keep_priority(bool keep_aid_priority);
int  qtg_shim_configure_sporadic(uint32_t pid, uint32_t tid, uint32_t normal_prio,
                                 uint32_t low_prio, uint32_t repl_period_ms,
                                 uint32_t init_budget_ms, uint32_t max_repl);
void qtg_shim_trace_marker(uint32_t cpu, uint32_t user_ev_id, uint32_t arg1, uint32_t arg2);
void qtg_shim_trace_aps_legacy_record(uint32_t cpu, uint32_t aps_id, const char *name,
                                      uint32_t budget_pct, uint32_t crit_ms,
                                      uint32_t window_ms, bool bankruptcy);
void qtg_shim_simulate_work(uint32_t pid, uint32_t tid, uint32_t cpu, uint32_t work_us);
void qtg_shim_simulate_preempt(uint32_t victim_pid, uint32_t victim_tid,
                               uint32_t preemptor_pid, uint32_t preemptor_tid,
                               uint32_t preemptor_prio, uint32_t cpu, uint32_t duration_us);

/* Monotonic nanosecond clock */
uint64_t qtg_shim_now_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* QTG_RTOS_SHIM_H */
