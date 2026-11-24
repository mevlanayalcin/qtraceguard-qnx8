/*
 * QTraceGuard - Clean-room QNX Kernel Event (.kev) binary format codec
 * SPDX-License-Identifier: MIT
 *
 * Implements the 16-byte QNX Neutrino / QNX OS 8.0 traceevent_t stream format
 * with TRACE_HEADER_BEGIN .. TRACE_HEADER_END ASCII metadata header, endian
 * normalization, simple/combine (S/CB/CC/CE) event reassembly, and 64-bit
 * timestamp reconstruction via CONTROL_TIME events and wrap detection.
 */

#ifndef QTG_KEV_H
#define QTG_KEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 32-bit traceevent_t.header bitfield encoding (documented QNX Neutrino .kev spec) */
#define QTG_STRUCT_SHIFT      30U
#define QTG_STRUCT_MASK       (0x3U << QTG_STRUCT_SHIFT)
#define QTG_STRUCT_S          (0x0U << QTG_STRUCT_SHIFT) /* Simple 1-slot event */
#define QTG_STRUCT_CB         (0x1U << QTG_STRUCT_SHIFT) /* Combine begin       */
#define QTG_STRUCT_CC         (0x2U << QTG_STRUCT_SHIFT) /* Combine continue    */
#define QTG_STRUCT_CE         (0x3U << QTG_STRUCT_SHIFT) /* Combine end         */

#define QTG_CPU_SHIFT         24U
#define QTG_CPU_MASK          (0x3FU << QTG_CPU_SHIFT)

#define QTG_CLASS_SHIFT       10U
#define QTG_CLASS_MASK        (0x1FU << QTG_CLASS_SHIFT)
#define QTG_EVENT_MASK        0x000003FFU

#define QTG_HDR_GET_STRUCT(h) ((uint32_t)(h) & QTG_STRUCT_MASK)
#define QTG_HDR_GET_CPU(h)    (((uint32_t)(h) & QTG_CPU_MASK) >> QTG_CPU_SHIFT)
#define QTG_HDR_GET_CLASS(h)  ((uint32_t)(h) & QTG_CLASS_MASK)
#define QTG_HDR_GET_EVENT(h)  ((uint32_t)(h) & QTG_EVENT_MASK)

#define QTG_HDR_MAKE(st, cpu, cls, ev) \
    (((uint32_t)(st) & QTG_STRUCT_MASK) | \
     (((uint32_t)(cpu) & 0x3FU) << QTG_CPU_SHIFT) | \
     ((uint32_t)(cls) & QTG_CLASS_MASK) | \
     ((uint32_t)(ev) & QTG_EVENT_MASK))

/* Internal QNX trace event classes (_TRACE_*_C) */
#define QTG_CLASS_EMPTY       (0x00U << QTG_CLASS_SHIFT)
#define QTG_CLASS_CONTROL     (0x01U << QTG_CLASS_SHIFT)
#define QTG_CLASS_KERCALL     (0x02U << QTG_CLASS_SHIFT)
#define QTG_CLASS_INT         (0x03U << QTG_CLASS_SHIFT)
#define QTG_CLASS_PR_TH       (0x04U << QTG_CLASS_SHIFT)
#define QTG_CLASS_SYSTEM      (0x05U << QTG_CLASS_SHIFT)
#define QTG_CLASS_USER        (0x06U << QTG_CLASS_SHIFT)
#define QTG_CLASS_COMM        (0x07U << QTG_CLASS_SHIFT)

/* Control class events (public QNX OS 8.0 kernel tracing reference) */
#define QTG_EV_CTRL_TIME       0x001U
#define QTG_EV_CTRL_BUFFER     0x002U
#define QTG_EV_CTRL_BUFFER_END 0x004U
#define QTG_EV_CTRL_DROPPED    0x008U

/* Thread state events inside QTG_CLASS_PR_TH (public QNX OS 8.0 thread state reference) */
#define QTG_TH_DEAD           0x000U
#define QTG_TH_RUNNING        0x001U
#define QTG_TH_READY          0x002U
#define QTG_TH_STOPPED        0x003U
#define QTG_TH_SEND           0x004U
#define QTG_TH_RECEIVE        0x005U
#define QTG_TH_REPLY          0x006U
#define QTG_TH_STACK          0x007U
#define QTG_TH_WAITTHREAD     0x008U
#define QTG_TH_WAITPAGE       0x009U
#define QTG_TH_SIGSUSPEND     0x00AU
#define QTG_TH_SIGWAITINFO    0x00BU
#define QTG_TH_NANOSLEEP      0x00CU
#define QTG_TH_MUTEX          0x00DU
#define QTG_TH_CONDVAR        0x00EU
#define QTG_TH_JOIN           0x00FU
#define QTG_TH_INTR           0x010U
#define QTG_TH_SEM            0x011U

/* Process / thread metadata events inside QTG_CLASS_PR_TH (public QNX OS 8.0 reference) */
#define QTG_PR_TH_CREATE_T         24U         /* STATE_CREATE  (0x018) */
#define QTG_PR_TH_DESTROY_T        25U         /* STATE_DESTROY (0x019) */
#define QTG_PR_TH_CREATE_P         (0x01U << 6) /* 0x040 */
#define QTG_PR_TH_DESTROY_P        (0x02U << 6) /* 0x080 */
#define QTG_PR_TH_CREATE_P_NAME    (0x03U << 6) /* 0x0C0 */
#define QTG_PR_TH_DESTROY_P_NAME   (0x04U << 6) /* 0x100 */
#define QTG_PR_TH_NAME_T           (0x05U << 6) /* 0x140 */

/* Communication class events (QTG_CLASS_COMM — public QNX OS 8.0 COMM reference) */
#define QTG_COMM_SMSG         0x000U /* MsgSend */
#define QTG_COMM_SPULSE       0x001U /* MsgSendPulse */
#define QTG_COMM_RMSG         0x002U /* MsgReceive (message) */
#define QTG_COMM_RPULSE       0x003U /* MsgReceive (pulse) */
#define QTG_COMM_SPULSE_EXE   0x004U /* Pulse execution */
#define QTG_COMM_SPULSE_DIS   0x005U /* Pulse disconnect */
#define QTG_COMM_SPULSE_DEA   0x006U /* Pulse death */
#define QTG_COMM_SPULSE_UN    0x007U /* _PULSE_CODE_UNBLOCK */
#define QTG_COMM_SPULSE_QUN   0x008U /* Pulse Q-unblock */
#define QTG_COMM_SIGNAL       0x009U /* Signal delivery */
#define QTG_COMM_REPLY        0x00AU /* MsgReply */
#define QTG_COMM_ERROR        0x00BU /* MsgError */

/* System class events (QTG_CLASS_SYSTEM — public QNX 7.1 APS reference) */
#define QTG_SYS_PATHMGR       0x002U /* Path manager event */
#define QTG_SYS_APS_NAME      0x003U /* Legacy QNX 7.1 APS partition name */
#define QTG_SYS_APS_BUDGETS   0x004U /* Legacy QNX 7.1 APS partition budgets */
#define QTG_SYS_APS_BNKR      0x005U /* Legacy QNX 7.1 APS bankruptcy */
/* QTraceGuard extended diagnostic records (non-overlapping high range >= 0x120) */
#define QTG_SYS_CH_FLAGS      0x120U /* ChannelCreate flags (_NTO_CHF_*) */
#define QTG_SYS_SPORADIC_REPL 0x121U /* QNX 8.0 SCHED_SPORADIC replenishment */
#define QTG_SYS_ABILITY_CAP   0x122U /* QNX 8.0 PROCMGR_AID_PRIORITY ceiling cap */
#define QTG_SYS_RESMGR_ATTACH 0x123U /* Resource manager pathname attach */

/* User class events (QTG_CLASS_USER) */
#define QTG_USER_MARKER       0x001U
#define QTG_USER_STRING       0x002U

/* Raw 16-byte on-disk QNX traceevent_t slot */
typedef struct qtg_raw_event {
    uint32_t header;
    uint32_t data[3];
} qtg_raw_event_t;

#define QTG_MAX_EVENT_WORDS 64U
#define QTG_MAX_STR_LEN     128U

/* Decoded high-level kernel event (simple or reassembled combine) */
typedef struct qtg_event {
    uint64_t timestamp_cycles;
    uint64_t timestamp_ns;
    uint64_t seq;
    uint32_t cpu;
    uint32_t event_class;
    uint32_t event_id;
    uint32_t word_count;
    uint32_t words[QTG_MAX_EVENT_WORDS]; /* Payload words excluding data[0] timestamp */
    bool     is_combine;
} qtg_event_t;

/* Metadata parsed from TRACE_HEADER_BEGIN .. TRACE_HEADER_END */
typedef struct qtg_kev_header {
    char     file_name[128];
    char     date[64];
    char     sysname[64];
    char     nodename[64];
    char     sys_release[64];
    char     machine[64];
    char     tracelogger_args[128];
    uint32_t ver_major;
    uint32_t ver_minor;
    bool     little_endian;
    uint64_t cycles_per_sec;
    uint32_t cpu_num;
    uint32_t syspage_len;
} qtg_kev_header_t;

/* In-memory trace stream */
typedef struct qtg_trace {
    qtg_kev_header_t header;
    qtg_event_t     *events;
    size_t           count;
    size_t           capacity;
    uint64_t         raw_slots_read;
    uint64_t         combine_events_reassembled;
    uint64_t         dropped_buffer_events;
} qtg_trace_t;

/* Streaming writer for emitting spec-compliant .kev binary files */
typedef struct qtg_kev_writer {
    FILE            *fp;
    qtg_kev_header_t header;
    uint32_t         last_ts_hi;
    uint32_t         last_ts_lo;
    bool             has_ts;
    uint64_t         slots_written;
} qtg_kev_writer_t;

/* Trace lifecycle */
void qtg_trace_init(qtg_trace_t *tr);
void qtg_trace_free(qtg_trace_t *tr);
int  qtg_trace_push(qtg_trace_t *tr, const qtg_event_t *ev);

/* Binary .kev I/O */
void qtg_kev_header_defaults(qtg_kev_header_t *hdr);
int  qtg_kev_read_file(const char *path, qtg_trace_t *out_trace, char *errbuf, size_t errlen);
int  qtg_kev_read_buffer(const uint8_t *buf, size_t len, qtg_trace_t *out_trace, char *errbuf, size_t errlen);

/* traceprinter text fallback reader */
int  qtg_traceprinter_read_file(const char *path, qtg_trace_t *out_trace, char *errbuf, size_t errlen);
int  qtg_traceprinter_read_buffer(const char *buf, size_t len, qtg_trace_t *out_trace, char *errbuf, size_t errlen);

/* Binary .kev writer */
int  qtg_kev_writer_open(qtg_kev_writer_t *w, const char *path, const qtg_kev_header_t *hdr);
int  qtg_kev_writer_write_simple(qtg_kev_writer_t *w, uint32_t cpu, uint32_t cls, uint32_t ev,
                                 uint64_t cycles, uint32_t w1, uint32_t w2);
int  qtg_kev_writer_write_combine(qtg_kev_writer_t *w, uint32_t cpu, uint32_t cls, uint32_t ev,
                                  uint64_t cycles, const uint32_t *words, size_t word_count);
int  qtg_kev_writer_write_name(qtg_kev_writer_t *w, uint32_t cpu, uint32_t cls, uint32_t ev,
                               uint64_t cycles, uint32_t id1, uint32_t id2, const char *name);
int  qtg_kev_writer_close(qtg_kev_writer_t *w);

/* Utilities */
uint64_t    qtg_cycles_to_ns(uint64_t cycles, uint64_t cycles_per_sec);
const char *qtg_class_name(uint32_t cls);
const char *qtg_event_name(uint32_t cls, uint32_t ev);
void        qtg_extract_ascii_from_words(const uint32_t *words, size_t word_count,
                                         char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* QTG_KEV_H */
