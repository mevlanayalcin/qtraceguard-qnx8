/*
 * QTraceGuard - Deterministic IPC Deadlock, Priority-Inversion & Scheduling
 *               Latency Verifier + QNX 7.1 APS -> QNX 8.0 SCHED_SPORADIC Advisor
 * SPDX-License-Identifier: MIT
 */

#ifndef QTG_ANALYZER_H
#define QTG_ANALYZER_H

#include "qtg_kev.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QTG_MAX_THREADS        2048U
#define QTG_MAX_PROCS          512U
#define QTG_MAX_CHANNELS       1024U
#define QTG_MAX_EDGES          1024U
#define QTG_MAX_CYCLES         32U
#define QTG_MAX_CYCLE_LEN      32U
#define QTG_MAX_INVERSIONS     256U
#define QTG_MAX_PULSE_LEAKS    128U
#define QTG_MAX_APS_PARTS      32U
#define QTG_MAX_TIMELINE_SLICE 2048U

/* Wire-level QTraceGuard channel flags (normalized from OS flags in qtg_rtos_shim.c) */
#define QTG_CHF_FIXED_PRIORITY 0x00000001U /* Disables message-driven priority inheritance */
#define QTG_CHF_UNBLOCK        0x00000002U /* Delivers _PULSE_CODE_UNBLOCK on timeout      */
#define QTG_CHF_PRIVATE        0x00000004U /* Private channel restricted to owning process */

/* Unprivileged priority ceiling in QNX 8.0 without PROCMGR_AID_PRIORITY */
#define QTG_QNX8_UNPRIV_PRIO_CEILING 63U

typedef enum qtg_inversion_kind {
    QTG_INV_CLASSIC_PREEMPT = 0,     /* Low-prio server preempted by mid-prio task */
    QTG_INV_FIXED_PRIO_CHANNEL = 1,  /* Channel created with _NTO_CHF_FIXED_PRIORITY */
    QTG_INV_AID_PRIORITY_CAP = 2     /* Server capped at prio 63 lacking PROCMGR_AID_PRIORITY */
} qtg_inversion_kind_t;

typedef struct qtg_proc_info {
    uint32_t pid;
    char     name[64];
    bool     has_aid_priority;
    uint32_t prio_ceiling;
} qtg_proc_info_t;

typedef struct qtg_channel_info {
    uint32_t chid;
    uint32_t owner_pid;
    uint32_t owner_tid;
    uint32_t flags;
    char     resmgr_path[64];
} qtg_channel_info_t;

typedef struct qtg_thread_stats {
    uint32_t pid;
    uint32_t tid;
    char     name[64];
    uint32_t base_priority;
    uint32_t effective_priority;
    uint32_t current_state;
    uint32_t waiting_on_pid;
    uint32_t waiting_on_tid;
    uint32_t waiting_on_chid;
    uint64_t ready_enter_ns;
    bool     in_ready;
    uint64_t send_enter_ns;
    bool     in_send_or_reply;
    uint64_t running_enter_ns;
    bool     in_running;
    uint64_t total_running_ns;
    uint64_t dispatch_count;
    uint64_t max_dispatch_ns;
    uint64_t sum_dispatch_ns;
    uint64_t ipc_rtt_count;
    uint64_t max_ipc_rtt_ns;
    uint64_t sum_ipc_rtt_ns;
    uint32_t sporadic_repl_count;
    uint32_t last_cpu;
    bool     prio_known;
    bool     is_idle;
} qtg_thread_stats_t;

typedef struct qtg_wait_edge {
    uint32_t from_pid;
    uint32_t from_tid;
    uint32_t to_pid;
    uint32_t to_tid;
    uint32_t chid;
    uint32_t rcvid;
    uint64_t since_ns;
    bool     active;
    bool     observed_ever;
    uint64_t msg_count;
    uint64_t max_rtt_ns;
    uint64_t sum_rtt_ns;
} qtg_wait_edge_t;

typedef struct qtg_deadlock_cycle {
    uint32_t length;
    uint32_t pids[QTG_MAX_CYCLE_LEN];
    uint32_t tids[QTG_MAX_CYCLE_LEN];
    uint32_t chids[QTG_MAX_CYCLE_LEN];
    uint64_t detected_at_ns;
} qtg_deadlock_cycle_t;

typedef struct qtg_prio_inversion {
    qtg_inversion_kind_t kind;
    uint32_t high_pid;
    uint32_t high_tid;
    uint32_t high_prio;
    uint32_t server_pid;
    uint32_t server_tid;
    uint32_t server_prio;
    uint32_t preemptor_pid;
    uint32_t preemptor_tid;
    uint32_t preemptor_prio;
    uint32_t chid;
    uint64_t start_ns;
    uint64_t duration_ns;
} qtg_prio_inversion_t;

typedef struct qtg_pulse_channel_stat {
    uint32_t pid;
    uint32_t chid;
    uint64_t sent_count;
    uint64_t received_count;
    uint64_t unblock_pulses;
    uint64_t max_backlog;
    uint64_t current_backlog;
    bool     leak_detected;
} qtg_pulse_channel_stat_t;

/* Legacy QNX 7.1 APS record + computed QNX 8.0 SCHED_SPORADIC migration */
typedef struct qtg_aps_migration {
    uint32_t aps_id;
    char     name[64];
    uint32_t budget_percent;
    uint32_t critical_budget_ms;
    uint32_t window_ms;
    uint32_t bankruptcy_events;
    /* Recommended QNX OS 8.0 SCHED_SPORADIC parameters */
    uint32_t rec_repl_period_ms;
    uint32_t rec_init_budget_ms;
    uint32_t rec_normal_priority;
    uint32_t rec_low_priority;
    uint32_t rec_max_repl;
    bool     requires_aid_priority;
} qtg_aps_migration_t;

typedef struct qtg_timeline_slice {
    uint32_t cpu;
    uint32_t pid;
    uint32_t tid;
    uint32_t state;
    uint32_t priority;
    uint64_t start_ns;
    uint64_t end_ns;
} qtg_timeline_slice_t;

/* Configurable CI Real-Time SLA Policy */
typedef struct qtg_sla_policy {
    uint64_t max_dispatch_latency_us;
    uint64_t max_p99_dispatch_latency_us;
    uint64_t max_ipc_rtt_us;
    uint64_t max_prio_inversion_us;
    uint32_t max_deadlock_cycles;
    uint32_t max_pulse_backlog;
    bool     forbid_fixed_priority_inversion;
    bool     forbid_aid_priority_cap;
    bool     forbid_aps_bankruptcy;
    char     focus_proc[64]; /* Optional process substring filter for full-system QNX traces */
} qtg_sla_policy_t;

typedef struct qtg_sla_verdict {
    bool passed;
    uint32_t violation_count;
    char violations[16][192];
} qtg_sla_verdict_t;

typedef struct qtg_report {
    qtg_kev_header_t header;
    uint64_t         total_events;
    uint64_t         dropped_buffer_events;
    uint32_t         overflow_procs;
    uint32_t         overflow_threads;
    uint32_t         overflow_channels;
    uint32_t         matched_focus_threads;
    char             focus_proc[64];
    uint64_t         trace_start_ns;
    uint64_t         trace_end_ns;
    uint64_t         trace_duration_ns;

    qtg_proc_info_t  procs[QTG_MAX_PROCS];
    size_t           proc_count;

    qtg_channel_info_t channels[QTG_MAX_CHANNELS];
    size_t             channel_count;

    qtg_thread_stats_t threads[QTG_MAX_THREADS];
    size_t             thread_count;

    qtg_wait_edge_t    edges[QTG_MAX_EDGES];
    size_t             edge_count;

    qtg_deadlock_cycle_t cycles[QTG_MAX_CYCLES];
    size_t               cycle_count;

    qtg_prio_inversion_t inversions[QTG_MAX_INVERSIONS];
    size_t               inversion_count;

    qtg_pulse_channel_stat_t pulses[QTG_MAX_PULSE_LEAKS];
    size_t                   pulse_count;

    qtg_aps_migration_t aps_migrations[QTG_MAX_APS_PARTS];
    size_t              aps_count;

    qtg_timeline_slice_t slices[QTG_MAX_TIMELINE_SLICE];
    size_t               slice_count;

    /* Aggregate latency percentiles (nanoseconds) */
    uint64_t dispatch_p50_ns;
    uint64_t dispatch_p95_ns;
    uint64_t dispatch_p99_ns;
    uint64_t dispatch_max_ns;
    uint64_t ipc_rtt_p50_ns;
    uint64_t ipc_rtt_p95_ns;
    uint64_t ipc_rtt_p99_ns;
    uint64_t ipc_rtt_max_ns;

    qtg_sla_verdict_t sla;
} qtg_report_t;

/* Policy helpers */
void qtg_sla_policy_defaults(qtg_sla_policy_t *pol);
int  qtg_sla_policy_load_json(const char *path, qtg_sla_policy_t *pol, char *errbuf, size_t errlen);

/* Core analysis entry point */
int  qtg_analyze_trace(const qtg_trace_t *tr, const qtg_sla_policy_t *pol, qtg_report_t *out_report);

/* Formatters */
void qtg_report_print_text(const qtg_report_t *rep, FILE *out, bool use_color);
int  qtg_report_write_json(const qtg_report_t *rep, const char *path);
int  qtg_report_write_sarif(const qtg_report_t *rep, const char *trace_path, const char *path);

/* Name lookup helper */
const char *qtg_report_proc_name(const qtg_report_t *rep, uint32_t pid);
const char *qtg_inversion_kind_str(qtg_inversion_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif /* QTG_ANALYZER_H */
