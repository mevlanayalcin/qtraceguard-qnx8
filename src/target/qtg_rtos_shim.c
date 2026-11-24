/*
 * QTraceGuard - Dual-Target QNX OS 8.0 / Portable POSIX RTOS Abstraction Layer
 * SPDX-License-Identifier: MIT
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
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#if defined(__QNX__) || defined(__QNXNTO__)
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <sys/trace.h>
#include <sys/syspage.h>
#if defined(QTG_HAVE_SCHED_APS_H)
#include <sys/sched_aps.h>
#endif
#endif

uint64_t qtg_shim_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#if defined(__QNX__) || defined(__QNXNTO__)

/* ===================== NATIVE QNX OS 8.0 IMPLEMENTATION ===================== */

int qtg_shim_init(const char *kev_out_path, uint32_t num_cpus) {
    (void)kev_out_path;
    (void)num_cpus;
    /* On QNX OS 8.0, kernel trace capture is performed out-of-band by
     * `tracelogger` against `procnto-smp-instr`. Emit a user marker. */
    TraceEvent(_NTO_TRACE_INSERTSUSEREVENT, QTG_USER_MARKER, 0x51544738U, 1U);
    return 0;
}

void qtg_shim_shutdown(void) {
    TraceEvent(_NTO_TRACE_INSERTSUSEREVENT, QTG_USER_MARKER, 0x51544738U, 0U);
}

uint32_t qtg_shim_register_proc(uint32_t pid_hint, const char *name, bool has_aid_priority) {
    (void)pid_hint;
    (void)name;
    (void)has_aid_priority;
    return (uint32_t)getpid();
}

void qtg_shim_register_thread(uint32_t pid, uint32_t tid, const char *name,
                              uint32_t base_priority, uint32_t cpu) {
    (void)pid;
    (void)tid;
    (void)cpu;
    if (name) {
        pthread_setname_np(pthread_self(), name);
    }
    if (base_priority > 0U) {
        struct sched_param sp;
        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = (int)base_priority;
        (void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }
}

int qtg_shim_channel_create(uint32_t owner_pid, uint32_t owner_tid,
                            uint32_t flags, const char *resmgr_path) {
    (void)owner_pid;
    (void)owner_tid;
    (void)resmgr_path;
    unsigned ch_flags = 0U;
    if (flags & QTG_SHIM_CHF_FIXED_PRIORITY) ch_flags |= _NTO_CHF_FIXED_PRIORITY;
    if (flags & QTG_SHIM_CHF_UNBLOCK)        ch_flags |= _NTO_CHF_UNBLOCK;
    if (flags & QTG_SHIM_CHF_PRIVATE)        ch_flags |= _NTO_CHF_PRIVATE;
    int chid = ChannelCreate(ch_flags);
    if (chid >= 0 && (flags & QTG_SHIM_CHF_FIXED_PRIORITY)) {
        TraceEvent(_NTO_TRACE_INSERTSUSEREVENT, 0x120U, (uint32_t)getpid(), flags);
    }
    return chid;
}

int qtg_shim_connect_attach(uint32_t target_pid, int chid) {
    return ConnectAttach(0U, (pid_t)target_pid, chid, _NTO_SIDE_CHANNEL, 0);
}

int qtg_shim_connect_detach(int coid) {
    return ConnectDetach(coid);
}

int qtg_shim_msg_send(uint32_t caller_pid, uint32_t caller_tid, int coid,
                      const void *smsg, size_t sbytes, void *rmsg, size_t rbytes,
                      uint32_t timeout_ms) {
    (void)caller_pid;
    (void)caller_tid;
    if (timeout_ms > 0U) {
        struct sigevent ev;
        memset(&ev, 0, sizeof(ev));
        SIGEV_UNBLOCK_INIT(&ev);
        uint64_t nsec = (uint64_t)timeout_ms * 1000000ULL;
        (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                           &ev, &nsec, NULL);
    }
    return (int)MsgSend(coid, smsg, sbytes, rmsg, rbytes);
}

int64_t qtg_shim_msg_receive(uint32_t server_pid, uint32_t server_tid, int chid,
                             void *msg, size_t bytes, qtg_shim_msg_info_t *info,
                             uint32_t timeout_ms) {
    (void)server_pid;
    (void)server_tid;
    if (timeout_ms > 0U) {
        struct sigevent ev;
        memset(&ev, 0, sizeof(ev));
        SIGEV_UNBLOCK_INIT(&ev);
        uint64_t nsec = (uint64_t)timeout_ms * 1000000ULL;
        (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, &ev, &nsec, NULL);
    }
    struct _msg_info minfo;
    memset(&minfo, 0, sizeof(minfo));
    rcvid_t rcvid = MsgReceive(chid, msg, bytes, &minfo);
    if (rcvid > 0 && info) {
        info->pid = (uint32_t)minfo.pid;
        info->tid = (uint32_t)minfo.tid;
        info->chid = (uint32_t)chid;
        info->priority = (uint32_t)minfo.priority;
        info->msglen = (size_t)minfo.msglen;
    }
    return (int64_t)rcvid;
}

int qtg_shim_msg_reply(uint32_t server_pid, uint32_t server_tid, int64_t rcvid,
                       int64_t status, const void *msg, size_t bytes) {
    (void)server_pid;
    (void)server_tid;
    return MsgReply((rcvid_t)rcvid, status, msg, bytes);
}

int qtg_shim_msg_error(uint32_t server_pid, uint32_t server_tid, int64_t rcvid, int err) {
    (void)server_pid;
    (void)server_tid;
    return MsgError((rcvid_t)rcvid, err);
}

int qtg_shim_msg_send_pulse(uint32_t caller_pid, uint32_t caller_tid, int coid,
                            int priority, int code, uint32_t value) {
    (void)caller_pid;
    (void)caller_tid;
    return MsgSendPulse(coid, priority, code, (int)value);
}

int qtg_shim_probe_runtime(qtg_shim_aps_probe_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->qnx_native = true;
    out->max_unpriv_priority = 63U;

    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(out->kernel_release, sizeof(out->kernel_release), "%.16s %.20s (%.16s)",
                 u.sysname, u.release, u.machine);
    } else {
        snprintf(out->kernel_release, sizeof(out->kernel_release), "QNX 8.0");
    }

    /* Live kernel probe: call SchedCtl directly on QNX OS 8.0!
     * Returns -1 / ENOSYS (errno=89) because QNX 8.0 removed legacy APS. */
#if defined(QTG_HAVE_SCHED_APS_H)
    sched_aps_info aps_info;
    memset(&aps_info, 0, sizeof(aps_info));
    APS_INIT_DATA(&aps_info);
    errno = 0;
    if (SchedCtl(SCHED_APS_QUERY_PARMS, &aps_info, sizeof(aps_info)) == 0) {
        out->aps_supported = true;
        out->aps_errno = 0;
    } else {
        out->aps_supported = false;
        out->aps_errno = errno;
    }
#else
    unsigned char aps_probe_buf[64];
    memset(aps_probe_buf, 0, sizeof(aps_probe_buf));
    errno = 0;
    if (SchedCtl(0, aps_probe_buf, sizeof(aps_probe_buf)) == 0) {
        out->aps_supported = true;
        out->aps_errno = 0;
    } else {
        out->aps_supported = false;
        out->aps_errno = errno;
    }
#endif

    out->sporadic_supported = (sched_get_priority_max(SCHED_SPORADIC) > 0);

    /* Empirically test PROCMGR_AID_PRIORITY by probing priority 64 (> unpriv ceiling 63) */
    int orig_pol = 0;
    struct sched_param orig_sp;
    memset(&orig_sp, 0, sizeof(orig_sp));
    if (pthread_getschedparam(pthread_self(), &orig_pol, &orig_sp) == 0) {
        struct sched_param test_sp = orig_sp;
        test_sp.sched_priority = 64;
        int rc64 = pthread_setschedparam(pthread_self(), SCHED_FIFO, &test_sp);
        if (rc64 == 0) {
            out->has_aid_priority = true;
            (void)pthread_setschedparam(pthread_self(), orig_pol, &orig_sp);
        } else {
            out->has_aid_priority = false;
        }
    } else {
        out->has_aid_priority = (geteuid() == 0);
    }
    return 0;
}

int qtg_shim_drop_privileges_keep_priority(bool keep_aid_priority) {
    int rc;
    if (keep_aid_priority) {
        rc = procmgr_ability(0,
            PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_PRIORITY,
            PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_PATHSPACE,
            PROCMGR_ADN_NONROOT | PROCMGR_AOP_LOCK  | PROCMGR_AID_PRIORITY,
            PROCMGR_AID_EOL);
    } else {
        rc = procmgr_ability(0,
            PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_PRIORITY,
            PROCMGR_AID_EOL);
    }
    if (geteuid() == 0) {
        if (setuid(1000) != 0) {
            return -1;
        }
    }
    if (!keep_aid_priority) {
        TraceEvent(_NTO_TRACE_INSERTSUSEREVENT, 0x122U, (uint32_t)getpid(), 63U);
    }
    return rc;
}

int qtg_shim_configure_sporadic(uint32_t pid, uint32_t tid, uint32_t normal_prio,
                                uint32_t low_prio, uint32_t repl_period_ms,
                                uint32_t init_budget_ms, uint32_t max_repl) {
    (void)pid;
    (void)tid;
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = (int)normal_prio;
    sp.sched_ss_low_priority = (int)low_prio;
    sp.sched_ss_repl_period.tv_sec = (time_t)(repl_period_ms / 1000U);
    sp.sched_ss_repl_period.tv_nsec = (long)((repl_period_ms % 1000U) * 1000000U);
    sp.sched_ss_init_budget.tv_sec = (time_t)(init_budget_ms / 1000U);
    sp.sched_ss_init_budget.tv_nsec = (long)((init_budget_ms % 1000U) * 1000000U);
    sp.sched_ss_max_repl = (int)max_repl;
    return pthread_setschedparam(pthread_self(), SCHED_SPORADIC, &sp);
}

void qtg_shim_trace_marker(uint32_t cpu, uint32_t user_ev_id, uint32_t arg1, uint32_t arg2) {
    (void)cpu;
    TraceEvent(_NTO_TRACE_INSERTSUSEREVENT, user_ev_id, arg1, arg2);
}

void qtg_shim_trace_aps_legacy_record(uint32_t cpu, uint32_t aps_id, const char *name,
                                      uint32_t budget_pct, uint32_t crit_ms,
                                      uint32_t window_ms, bool bankruptcy) {
    (void)cpu; (void)aps_id; (void)name; (void)budget_pct;
    (void)crit_ms; (void)window_ms; (void)bankruptcy;
}

void qtg_shim_simulate_work(uint32_t pid, uint32_t tid, uint32_t cpu, uint32_t work_us) {
    (void)pid; (void)tid; (void)cpu;
    if (work_us <= 2000U) {
        /* Sub-2ms busy-spin avoids QNX 1ms default ClockPeriod quantization */
        uint64_t deadline = qtg_shim_now_ns() + (uint64_t)work_us * 1000ULL;
        while (qtg_shim_now_ns() < deadline) {
            /* deterministic CPU work */
        }
        return;
    }
    struct timespec ts = { (time_t)(work_us / 1000000U), (long)((work_us % 1000000U) * 1000U) };
    nanosleep(&ts, NULL);
}

typedef struct qtg_qnx_preempt_arg {
    uint32_t prio;
    uint32_t cpu;
    uint32_t duration_us;
} qtg_qnx_preempt_arg_t;

static void *qtg_qnx_preemptor_fn(void *arg) {
    qtg_qnx_preempt_arg_t *p = (qtg_qnx_preempt_arg_t *)arg;
    uintptr_t mask = (uintptr_t)(1U << (p->cpu & 31U));
    (void)ThreadCtl(_NTO_TCTL_RUNMASK, (void *)mask);
    /* Spin on target CPU with zero syscalls so QNX 8.0 holds victim preempted for full duration_us */
    uint64_t deadline = qtg_shim_now_ns() + (uint64_t)p->duration_us * 1000ULL;
    while (qtg_shim_now_ns() < deadline) {
        /* hold CPU */
    }
    return NULL;
}

void qtg_shim_simulate_preempt(uint32_t victim_pid, uint32_t victim_tid,
                               uint32_t preemptor_pid, uint32_t preemptor_tid,
                               uint32_t preemptor_prio, uint32_t cpu, uint32_t duration_us) {
    (void)victim_pid; (void)victim_tid; (void)preemptor_pid; (void)preemptor_tid;
    /* Pin current (victim) thread to target CPU so medium-priority thread preempts it */
    uintptr_t mask = (uintptr_t)(1U << (cpu & 31U));
    (void)ThreadCtl(_NTO_TCTL_RUNMASK, (void *)mask);

    qtg_qnx_preempt_arg_t parg = { .prio = preemptor_prio, .cpu = cpu, .duration_us = duration_us };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = (int)preemptor_prio;
    pthread_attr_setschedparam(&attr, &sp);

    pthread_t pth;
    if (pthread_create(&pth, &attr, qtg_qnx_preemptor_fn, &parg) == 0) {
        /* Stay runnable on same CPU so QNX 8.0 preempts victim for the entire duration_us window */
        uint64_t end = qtg_shim_now_ns() + (uint64_t)duration_us * 1000ULL + 50000ULL;
        while (qtg_shim_now_ns() < end) {
            /* preempted on real QNX 8.0 while preemptor runs */
        }
        pthread_join(pth, NULL);
    }
    pthread_attr_destroy(&attr);
    /* Restore unrestricted SMP runmask on victim thread */
    (void)ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)0xFFFFFFFFU);
}

#else

/* ===================== PORTABLE POSIX DETERMINISTIC SHIM ===================== */

#define SIM_MAX_PROCS    64
#define SIM_MAX_THREADS  128
#define SIM_MAX_CHANNELS 64
#define SIM_MAX_CONNS    128
#define SIM_MAX_PENDING  64
#define SIM_MAX_PULSES   128
#define SIM_MSG_BUF_SZ   256

typedef struct sim_proc {
    uint32_t pid;
    char     name[64];
    bool     has_aid_priority;
} sim_proc_t;

typedef struct sim_thread {
    uint32_t pid;
    uint32_t tid;
    char     name[64];
    uint32_t base_prio;
    uint32_t eff_prio;
    uint32_t cpu;
} sim_thread_t;

typedef struct sim_msg_slot {
    bool     used;
    int      rcvid;
    uint32_t client_pid;
    uint32_t client_tid;
    uint32_t client_prio;
    int      chid;
    uint8_t  req[SIM_MSG_BUF_SZ];
    size_t   req_len;
    uint8_t  rep[SIM_MSG_BUF_SZ];
    size_t   rep_len;
    bool     received;
    bool     replied;
    bool     timed_out;
    int64_t  reply_status;
} sim_msg_slot_t;

typedef struct sim_channel {
    bool     used;
    int      chid;
    uint32_t owner_pid;
    uint32_t owner_tid;
    uint32_t flags;
    char     resmgr_path[64];
    qtg_shim_pulse_t pulses[SIM_MAX_PULSES];
    size_t   pulse_head;
    size_t   pulse_tail;
    size_t   pulse_count;
} sim_channel_t;

typedef struct sim_conn {
    bool     used;
    int      coid;
    uint32_t target_pid;
    int      chid;
} sim_conn_t;

static struct {
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
    bool             initialized;
    bool             tracing;
    qtg_kev_writer_t writer;
    uint64_t         start_ns;
    uint64_t         sim_clock_ns;
    uint32_t         num_cpus;

    sim_proc_t       procs[SIM_MAX_PROCS];
    size_t           proc_count;
    sim_thread_t     threads[SIM_MAX_THREADS];
    size_t           thread_count;
    sim_channel_t    channels[SIM_MAX_CHANNELS];
    sim_conn_t       conns[SIM_MAX_CONNS];
    sim_msg_slot_t   slots[SIM_MAX_PENDING];
    int              next_chid;
    int              next_coid;
    int              next_rcvid;
} g_sim = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
    .initialized = false
};

static uint64_t sim_tick_locked(uint64_t min_delta_ns) {
    /* Advance deterministic virtual RTOS clock without host Linux scheduling jitter */
    g_sim.sim_clock_ns += (min_delta_ns > 0ULL ? min_delta_ns : 1000ULL);
    return g_sim.sim_clock_ns;
}

static sim_thread_t *sim_find_thread_locked(uint32_t pid, uint32_t tid) {
    for (size_t i = 0; i < g_sim.thread_count; i++) {
        if (g_sim.threads[i].pid == pid && g_sim.threads[i].tid == tid) return &g_sim.threads[i];
    }
    return NULL;
}

static sim_proc_t *sim_find_proc_locked(uint32_t pid) {
    for (size_t i = 0; i < g_sim.proc_count; i++) {
        if (g_sim.procs[i].pid == pid) return &g_sim.procs[i];
    }
    return NULL;
}

static sim_channel_t *sim_find_channel_locked(int chid) {
    for (int i = 0; i < SIM_MAX_CHANNELS; i++) {
        if (g_sim.channels[i].used && g_sim.channels[i].chid == chid) return &g_sim.channels[i];
    }
    return NULL;
}

static void sim_emit_thread_state_locked(uint32_t cpu, uint32_t state,
                                         uint32_t pid, uint32_t tid, uint32_t prio,
                                         uint64_t ts_ns) {
    if (!g_sim.tracing) return;
    uint32_t words[3] = { pid, tid, prio };
    qtg_kev_writer_write_combine(&g_sim.writer, cpu, QTG_CLASS_PR_TH, state, ts_ns, words, 3U);
}

int qtg_shim_init(const char *kev_out_path, uint32_t num_cpus) {
    pthread_mutex_lock(&g_sim.mu);
    if (g_sim.tracing) {
        qtg_kev_writer_close(&g_sim.writer);
        g_sim.tracing = false;
    }
    g_sim.proc_count = 0;
    g_sim.thread_count = 0;
    memset(g_sim.channels, 0, sizeof(g_sim.channels));
    memset(g_sim.conns, 0, sizeof(g_sim.conns));
    memset(g_sim.slots, 0, sizeof(g_sim.slots));
    g_sim.next_chid = 1;
    g_sim.next_coid = 100;
    g_sim.next_rcvid = 1000;
    g_sim.start_ns = qtg_shim_now_ns();
    g_sim.sim_clock_ns = 10000ULL;
    g_sim.num_cpus = (num_cpus > 0U && num_cpus <= 64U) ? num_cpus : 4U;

    if (kev_out_path) {
        qtg_kev_header_t hdr;
        qtg_kev_header_defaults(&hdr);
        snprintf(hdr.file_name, sizeof(hdr.file_name), "%s", kev_out_path);
        hdr.cpu_num = g_sim.num_cpus;
        hdr.cycles_per_sec = 1000000000ULL; /* 1 cycle = 1 ns */
        snprintf(hdr.sysname, sizeof(hdr.sysname), "QNX");
        snprintf(hdr.sys_release, sizeof(hdr.sys_release), "8.0.0-sim");
        snprintf(hdr.nodename, sizeof(hdr.nodename), "qnx8-posix-sim");
        if (qtg_kev_writer_open(&g_sim.writer, kev_out_path, &hdr) != 0) {
            pthread_mutex_unlock(&g_sim.mu);
            return -1;
        }
        g_sim.tracing = true;
    }
    g_sim.initialized = true;
    pthread_mutex_unlock(&g_sim.mu);
    return 0;
}

void qtg_shim_shutdown(void) {
    pthread_mutex_lock(&g_sim.mu);
    if (g_sim.tracing) {
        qtg_kev_writer_close(&g_sim.writer);
        g_sim.tracing = false;
    }
    g_sim.initialized = false;
    pthread_mutex_unlock(&g_sim.mu);
}

uint32_t qtg_shim_register_proc(uint32_t pid_hint, const char *name, bool has_aid_priority) {
    pthread_mutex_lock(&g_sim.mu);
    uint32_t pid = (pid_hint != 0U) ? pid_hint : (uint32_t)(100U + g_sim.proc_count + 1U);
    if (g_sim.proc_count < SIM_MAX_PROCS) {
        sim_proc_t *p = &g_sim.procs[g_sim.proc_count++];
        p->pid = pid;
        snprintf(p->name, sizeof(p->name), "%s", name ? name : "proc");
        p->has_aid_priority = has_aid_priority;
    }
    if (g_sim.tracing) {
        uint64_t ts = sim_tick_locked(2000ULL);
        qtg_kev_writer_write_name(&g_sim.writer, 0U, QTG_CLASS_PR_TH, QTG_PR_TH_CREATE_P_NAME,
                                  ts, 1U, pid, name ? name : "proc");
        uint32_t ab_words[3] = { pid, has_aid_priority ? 1U : 0U, has_aid_priority ? 255U : 63U };
        qtg_kev_writer_write_combine(&g_sim.writer, 0U, QTG_CLASS_SYSTEM, QTG_SYS_ABILITY_CAP,
                                     sim_tick_locked(1000ULL), ab_words, 3U);
    }
    pthread_mutex_unlock(&g_sim.mu);
    return pid;
}

void qtg_shim_register_thread(uint32_t pid, uint32_t tid, const char *name,
                              uint32_t base_priority, uint32_t cpu) {
    pthread_mutex_lock(&g_sim.mu);
    sim_thread_t *t = sim_find_thread_locked(pid, tid);
    if (!t && g_sim.thread_count < SIM_MAX_THREADS) {
        t = &g_sim.threads[g_sim.thread_count++];
        memset(t, 0, sizeof(*t));
        t->pid = pid;
        t->tid = tid;
    }
    if (t) {
        snprintf(t->name, sizeof(t->name), "%s", name ? name : "worker");
        t->base_prio = base_priority;
        t->eff_prio  = base_priority;
        t->cpu = cpu % g_sim.num_cpus;
    }
    if (g_sim.tracing) {
        uint64_t ts = sim_tick_locked(2000ULL);
        qtg_kev_writer_write_name(&g_sim.writer, cpu % g_sim.num_cpus, QTG_CLASS_PR_TH,
                                  QTG_PR_TH_NAME_T, ts, pid, tid, name ? name : "worker");
        sim_emit_thread_state_locked(cpu % g_sim.num_cpus, QTG_TH_READY, pid, tid, base_priority,
                                     sim_tick_locked(1000ULL));
    }
    pthread_mutex_unlock(&g_sim.mu);
}

int qtg_shim_channel_create(uint32_t owner_pid, uint32_t owner_tid,
                            uint32_t flags, const char *resmgr_path) {
    pthread_mutex_lock(&g_sim.mu);
    int chid = -1;
    for (int i = 0; i < SIM_MAX_CHANNELS; i++) {
        if (!g_sim.channels[i].used) {
            sim_channel_t *ch = &g_sim.channels[i];
            memset(ch, 0, sizeof(*ch));
            ch->used = true;
            ch->chid = g_sim.next_chid++;
            ch->owner_pid = owner_pid;
            ch->owner_tid = owner_tid;
            ch->flags = flags;
            if (resmgr_path) snprintf(ch->resmgr_path, sizeof(ch->resmgr_path), "%s", resmgr_path);
            chid = ch->chid;
            break;
        }
    }
    if (chid > 0 && g_sim.tracing) {
        uint32_t fw[4] = { owner_pid, (uint32_t)chid, flags, owner_tid };
        qtg_kev_writer_write_combine(&g_sim.writer, 0U, QTG_CLASS_SYSTEM, QTG_SYS_CH_FLAGS,
                                     sim_tick_locked(1500ULL), fw, 4U);
        if (resmgr_path && resmgr_path[0] != '\0') {
            qtg_kev_writer_write_name(&g_sim.writer, 0U, QTG_CLASS_SYSTEM, QTG_SYS_RESMGR_ATTACH,
                                      sim_tick_locked(1500ULL), owner_pid, (uint32_t)chid, resmgr_path);
        }
    }
    pthread_mutex_unlock(&g_sim.mu);
    return chid;
}

int qtg_shim_connect_attach(uint32_t target_pid, int chid) {
    pthread_mutex_lock(&g_sim.mu);
    int coid = -1;
    for (int i = 0; i < SIM_MAX_CONNS; i++) {
        if (!g_sim.conns[i].used) {
            g_sim.conns[i].used = true;
            g_sim.conns[i].coid = g_sim.next_coid++;
            g_sim.conns[i].target_pid = target_pid;
            g_sim.conns[i].chid = chid;
            coid = g_sim.conns[i].coid;
            break;
        }
    }
    pthread_mutex_unlock(&g_sim.mu);
    return coid;
}

int qtg_shim_connect_detach(int coid) {
    pthread_mutex_lock(&g_sim.mu);
    for (int i = 0; i < SIM_MAX_CONNS; i++) {
        if (g_sim.conns[i].used && g_sim.conns[i].coid == coid) {
            g_sim.conns[i].used = false;
            pthread_mutex_unlock(&g_sim.mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_sim.mu);
    return -1;
}

int qtg_shim_msg_send(uint32_t caller_pid, uint32_t caller_tid, int coid,
                      const void *smsg, size_t sbytes, void *rmsg, size_t rbytes,
                      uint32_t timeout_ms) {
    pthread_mutex_lock(&g_sim.mu);
    sim_conn_t *cn = NULL;
    for (int i = 0; i < SIM_MAX_CONNS; i++) {
        if (g_sim.conns[i].used && g_sim.conns[i].coid == coid) { cn = &g_sim.conns[i]; break; }
    }
    if (!cn) { pthread_mutex_unlock(&g_sim.mu); errno = EBADF; return -1; }
    sim_channel_t *ch = sim_find_channel_locked(cn->chid);
    if (!ch) { pthread_mutex_unlock(&g_sim.mu); errno = ESRCH; return -1; }

    sim_msg_slot_t *slot = NULL;
    for (int i = 0; i < SIM_MAX_PENDING; i++) {
        if (!g_sim.slots[i].used) { slot = &g_sim.slots[i]; break; }
    }
    if (!slot) { pthread_mutex_unlock(&g_sim.mu); errno = EAGAIN; return -1; }

    sim_thread_t *cli = sim_find_thread_locked(caller_pid, caller_tid);
    uint32_t cli_prio = cli ? cli->eff_prio : 10U;
    uint32_t cli_cpu  = cli ? cli->cpu : 0U;

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->rcvid = g_sim.next_rcvid++;
    slot->client_pid = caller_pid;
    slot->client_tid = caller_tid;
    slot->client_prio = cli_prio;
    slot->chid = ch->chid;
    if (smsg && sbytes > 0U) {
        size_t cp = (sbytes > SIM_MSG_BUF_SZ) ? SIM_MSG_BUF_SZ : sbytes;
        memcpy(slot->req, smsg, cp);
        slot->req_len = cp;
    }

    if (g_sim.tracing) {
        uint64_t ts = sim_tick_locked(4000ULL);
        sim_emit_thread_state_locked(cli_cpu, QTG_TH_RUNNING, caller_pid, caller_tid, cli_prio, ts);
        uint32_t sw[6] = { (uint32_t)ch->chid, ch->owner_pid, caller_pid, caller_tid,
                           ch->owner_tid, cli_prio };
        qtg_kev_writer_write_combine(&g_sim.writer, cli_cpu, QTG_CLASS_COMM, QTG_COMM_SMSG,
                                     sim_tick_locked(2500ULL), sw, 6U);
        sim_emit_thread_state_locked(cli_cpu, QTG_TH_SEND, caller_pid, caller_tid, cli_prio,
                                     sim_tick_locked(1500ULL));
    }

    pthread_cond_broadcast(&g_sim.cv);

    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    uint32_t to = (timeout_ms > 0U) ? timeout_ms : 200U;
    abstime.tv_sec  += (time_t)(to / 1000U);
    abstime.tv_nsec += (long)((to % 1000U) * 1000000U);
    if (abstime.tv_nsec >= 1000000000L) {
        abstime.tv_sec++;
        abstime.tv_nsec -= 1000000000L;
    }

    int wait_rc = 0;
    while (!slot->replied && wait_rc == 0) {
        wait_rc = pthread_cond_timedwait(&g_sim.cv, &g_sim.mu, &abstime);
    }

    int ret = -1;
    if (slot->replied) {
        if (rmsg && rbytes > 0U && slot->rep_len > 0U) {
            size_t cp = (slot->rep_len < rbytes) ? slot->rep_len : rbytes;
            memcpy(rmsg, slot->rep, cp);
        }
        ret = (int)slot->reply_status;
    } else {
        slot->timed_out = true;
        if (g_sim.tracing && (ch->flags & QTG_SHIM_CHF_UNBLOCK)) {
            qtg_kev_writer_write_simple(&g_sim.writer, cli_cpu, QTG_CLASS_COMM, QTG_COMM_SPULSE_UN,
                                        sim_tick_locked(5000ULL), (uint32_t)ch->chid, ch->owner_pid);
        }
        errno = ETIMEDOUT;
        ret = -1;
    }
    slot->used = false;
    pthread_mutex_unlock(&g_sim.mu);
    return ret;
}

int64_t qtg_shim_msg_receive(uint32_t server_pid, uint32_t server_tid, int chid,
                             void *msg, size_t bytes, qtg_shim_msg_info_t *info,
                             uint32_t timeout_ms) {
    pthread_mutex_lock(&g_sim.mu);
    sim_channel_t *ch = sim_find_channel_locked(chid);
    if (!ch) { pthread_mutex_unlock(&g_sim.mu); errno = ESRCH; return -1; }
    sim_thread_t *srv = sim_find_thread_locked(server_pid, server_tid);
    uint32_t srv_cpu = srv ? srv->cpu : 0U;

    if (g_sim.tracing) {
        sim_emit_thread_state_locked(srv_cpu, QTG_TH_RECEIVE, server_pid, server_tid,
                                     srv ? srv->eff_prio : 10U, sim_tick_locked(1500ULL));
    }

    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    uint32_t to = (timeout_ms > 0U) ? timeout_ms : 200U;
    abstime.tv_sec  += (time_t)(to / 1000U);
    abstime.tv_nsec += (long)((to % 1000U) * 1000000U);
    if (abstime.tv_nsec >= 1000000000L) {
        abstime.tv_sec++;
        abstime.tv_nsec -= 1000000000L;
    }

    while (1) {
        /* 1. Check pulses first (QNX delivers pulses with rcvid == 0) */
        if (ch->pulse_count > 0U) {
            qtg_shim_pulse_t p = ch->pulses[ch->pulse_head];
            ch->pulse_head = (ch->pulse_head + 1U) % SIM_MAX_PULSES;
            ch->pulse_count--;
            if (msg && bytes >= sizeof(p)) memcpy(msg, &p, sizeof(p));
            if (g_sim.tracing) {
                qtg_kev_writer_write_simple(&g_sim.writer, srv_cpu, QTG_CLASS_COMM, QTG_COMM_RPULSE,
                                            sim_tick_locked(3000ULL), (uint32_t)chid, server_pid);
            }
            pthread_mutex_unlock(&g_sim.mu);
            return 0;
        }
        /* 2. Pick highest-priority pending synchronous message on this channel */
        sim_msg_slot_t *best = NULL;
        for (int i = 0; i < SIM_MAX_PENDING; i++) {
            sim_msg_slot_t *s = &g_sim.slots[i];
            if (s->used && s->chid == chid && !s->received && !s->timed_out) {
                if (!best || s->client_prio > best->client_prio) best = s;
            }
        }
        if (best) {
            best->received = true;
            if (msg && bytes > 0U && best->req_len > 0U) {
                size_t cp = (best->req_len < bytes) ? best->req_len : bytes;
                memcpy(msg, best->req, cp);
            }
            if (info) {
                info->pid = best->client_pid;
                info->tid = best->client_tid;
                info->chid = (uint32_t)chid;
                info->priority = best->client_prio;
                info->msglen = best->req_len;
            }
            /* Compute inherited priority respecting _NTO_CHF_FIXED_PRIORITY & PROCMGR_AID_PRIORITY */
            uint32_t eff = srv ? srv->base_prio : 10U;
            sim_proc_t *sp = sim_find_proc_locked(server_pid);
            if (!(ch->flags & QTG_SHIM_CHF_FIXED_PRIORITY)) {
                uint32_t boosted = best->client_prio;
                if (sp && !sp->has_aid_priority && boosted > 63U) {
                    boosted = 63U;
                }
                if (boosted > eff) eff = boosted;
            }
            if (srv) srv->eff_prio = eff;

            if (g_sim.tracing) {
                uint64_t ts_ready = sim_tick_locked(4000ULL);
                sim_emit_thread_state_locked(srv_cpu, QTG_TH_READY, server_pid, server_tid, eff, ts_ready);
                uint64_t ts_run = sim_tick_locked(6000ULL); /* Deterministic ~6us dispatch */
                sim_emit_thread_state_locked(srv_cpu, QTG_TH_RUNNING, server_pid, server_tid, eff, ts_run);
                uint32_t rw[6] = { (uint32_t)best->rcvid, best->client_pid, server_pid,
                                   server_tid, best->client_tid, (uint32_t)chid };
                qtg_kev_writer_write_combine(&g_sim.writer, srv_cpu, QTG_CLASS_COMM, QTG_COMM_RMSG,
                                             sim_tick_locked(2000ULL), rw, 6U);
            }
            int rcvid = best->rcvid;
            pthread_mutex_unlock(&g_sim.mu);
            return rcvid;
        }
        int wrc = pthread_cond_timedwait(&g_sim.cv, &g_sim.mu, &abstime);
        if (wrc != 0) {
            pthread_mutex_unlock(&g_sim.mu);
            errno = ETIMEDOUT;
            return -1;
        }
    }
}

int qtg_shim_msg_reply(uint32_t server_pid, uint32_t server_tid, int64_t rcvid,
                       int64_t status, const void *msg, size_t bytes) {
    pthread_mutex_lock(&g_sim.mu);
    sim_msg_slot_t *slot = NULL;
    for (int i = 0; i < SIM_MAX_PENDING; i++) {
        if (g_sim.slots[i].used && (int64_t)g_sim.slots[i].rcvid == rcvid) { slot = &g_sim.slots[i]; break; }
    }
    if (!slot) { pthread_mutex_unlock(&g_sim.mu); errno = ESRCH; return -1; }
    if (msg && bytes > 0U) {
        size_t cp = (bytes > SIM_MSG_BUF_SZ) ? SIM_MSG_BUF_SZ : bytes;
        memcpy(slot->rep, msg, cp);
        slot->rep_len = cp;
    }
    slot->reply_status = status;
    slot->replied = true;

    sim_thread_t *srv = sim_find_thread_locked(server_pid, server_tid);
    sim_thread_t *cli = sim_find_thread_locked(slot->client_pid, slot->client_tid);
    uint32_t srv_cpu = srv ? srv->cpu : 0U;
    uint32_t cli_cpu = cli ? cli->cpu : 0U;

    if (g_sim.tracing) {
        uint32_t rw[5] = { slot->client_tid, slot->client_pid, server_pid, server_tid,
                           (uint32_t)slot->chid };
        qtg_kev_writer_write_combine(&g_sim.writer, srv_cpu, QTG_CLASS_COMM, QTG_COMM_REPLY,
                                     sim_tick_locked(4000ULL), rw, 5U);
        if (srv) srv->eff_prio = srv->base_prio;
        if (cli) {
            sim_emit_thread_state_locked(cli_cpu, QTG_TH_READY, cli->pid, cli->tid, cli->eff_prio,
                                         sim_tick_locked(3000ULL));
            sim_emit_thread_state_locked(cli_cpu, QTG_TH_RUNNING, cli->pid, cli->tid, cli->eff_prio,
                                         sim_tick_locked(5000ULL));
        }
    }
    pthread_cond_broadcast(&g_sim.cv);
    pthread_mutex_unlock(&g_sim.mu);
    return 0;
}

int qtg_shim_msg_error(uint32_t server_pid, uint32_t server_tid, int64_t rcvid, int err) {
    (void)err;
    return qtg_shim_msg_reply(server_pid, server_tid, rcvid, -1, NULL, 0);
}

int qtg_shim_msg_send_pulse(uint32_t caller_pid, uint32_t caller_tid, int coid,
                            int priority, int code, uint32_t value) {
    (void)caller_pid;
    (void)caller_tid;
    pthread_mutex_lock(&g_sim.mu);
    sim_conn_t *cn = NULL;
    for (int i = 0; i < SIM_MAX_CONNS; i++) {
        if (g_sim.conns[i].used && g_sim.conns[i].coid == coid) { cn = &g_sim.conns[i]; break; }
    }
    if (!cn) { pthread_mutex_unlock(&g_sim.mu); errno = EBADF; return -1; }
    sim_channel_t *ch = sim_find_channel_locked(cn->chid);
    if (!ch) { pthread_mutex_unlock(&g_sim.mu); errno = ESRCH; return -1; }

    if (ch->pulse_count < SIM_MAX_PULSES) {
        qtg_shim_pulse_t *p = &ch->pulses[ch->pulse_tail];
        memset(p, 0, sizeof(*p));
        p->code = (int8_t)code;
        p->value = value;
        p->scoid = coid;
        ch->pulse_tail = (ch->pulse_tail + 1U) % SIM_MAX_PULSES;
        ch->pulse_count++;
    }
    if (g_sim.tracing) {
        uint32_t pw[4] = { (uint32_t)ch->chid, ch->owner_pid, (uint32_t)(priority > 0 ? priority : 10),
                           (uint32_t)(uint8_t)code };
        qtg_kev_writer_write_combine(&g_sim.writer, 0U, QTG_CLASS_COMM, QTG_COMM_SPULSE,
                                     sim_tick_locked(2500ULL), pw, 4U);
    }
    pthread_cond_broadcast(&g_sim.cv);
    pthread_mutex_unlock(&g_sim.mu);
    return 0;
}

int qtg_shim_probe_runtime(qtg_shim_aps_probe_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->qnx_native = false;
    out->aps_supported = false;
    out->aps_errno = ENOSYS; /* Models QNX OS 8.0 APS removal */
    out->sporadic_supported = true;
    out->has_aid_priority = true;
    out->max_unpriv_priority = 63U;
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(out->kernel_release, sizeof(out->kernel_release), "POSIX-sim (%.16s %.24s)",
                 u.sysname, u.release);
    } else {
        snprintf(out->kernel_release, sizeof(out->kernel_release), "POSIX-sim");
    }
    return 0;
}

int qtg_shim_drop_privileges_keep_priority(bool keep_aid_priority) {
    (void)keep_aid_priority;
    return 0;
}

int qtg_shim_configure_sporadic(uint32_t pid, uint32_t tid, uint32_t normal_prio,
                                uint32_t low_prio, uint32_t repl_period_ms,
                                uint32_t init_budget_ms, uint32_t max_repl) {
    (void)normal_prio; (void)low_prio; (void)max_repl;
    pthread_mutex_lock(&g_sim.mu);
    if (g_sim.tracing) {
        uint32_t sw[4] = { pid, tid, init_budget_ms * 1000U, repl_period_ms * 1000U };
        qtg_kev_writer_write_combine(&g_sim.writer, 0U, QTG_CLASS_SYSTEM, QTG_SYS_SPORADIC_REPL,
                                     sim_tick_locked(3000ULL), sw, 4U);
    }
    pthread_mutex_unlock(&g_sim.mu);
    return 0;
}

void qtg_shim_trace_marker(uint32_t cpu, uint32_t user_ev_id, uint32_t arg1, uint32_t arg2) {
    pthread_mutex_lock(&g_sim.mu);
    if (g_sim.tracing) {
        qtg_kev_writer_write_simple(&g_sim.writer, cpu % g_sim.num_cpus, QTG_CLASS_USER,
                                    user_ev_id, sim_tick_locked(1000ULL), arg1, arg2);
    }
    pthread_mutex_unlock(&g_sim.mu);
}

void qtg_shim_trace_aps_legacy_record(uint32_t cpu, uint32_t aps_id, const char *name,
                                      uint32_t budget_pct, uint32_t crit_ms,
                                      uint32_t window_ms, bool bankruptcy) {
    pthread_mutex_lock(&g_sim.mu);
    if (g_sim.tracing) {
        qtg_kev_writer_write_name(&g_sim.writer, cpu % g_sim.num_cpus, QTG_CLASS_SYSTEM,
                                  QTG_SYS_APS_NAME, sim_tick_locked(2000ULL), aps_id, 0U, name);
        uint32_t bw[4] = { aps_id, budget_pct, crit_ms, window_ms };
        qtg_kev_writer_write_combine(&g_sim.writer, cpu % g_sim.num_cpus, QTG_CLASS_SYSTEM,
                                     QTG_SYS_APS_BUDGETS, sim_tick_locked(2000ULL), bw, 4U);
        if (bankruptcy) {
            uint32_t bkw[3] = { aps_id, 101U, 1U };
            qtg_kev_writer_write_combine(&g_sim.writer, cpu % g_sim.num_cpus, QTG_CLASS_SYSTEM,
                                         QTG_SYS_APS_BNKR, sim_tick_locked(2000ULL), bkw, 3U);
        }
    }
    pthread_mutex_unlock(&g_sim.mu);
}

void qtg_shim_simulate_work(uint32_t pid, uint32_t tid, uint32_t cpu, uint32_t work_us) {
    pthread_mutex_lock(&g_sim.mu);
    sim_thread_t *t = sim_find_thread_locked(pid, tid);
    uint32_t prio = t ? t->eff_prio : 10U;
    if (g_sim.tracing) {
        sim_emit_thread_state_locked(cpu % g_sim.num_cpus, QTG_TH_RUNNING, pid, tid, prio,
                                     sim_tick_locked(2000ULL));
        sim_tick_locked((uint64_t)work_us * 1000ULL);
    }
    pthread_mutex_unlock(&g_sim.mu);
}

void qtg_shim_simulate_preempt(uint32_t victim_pid, uint32_t victim_tid,
                               uint32_t preemptor_pid, uint32_t preemptor_tid,
                               uint32_t preemptor_prio, uint32_t cpu, uint32_t duration_us) {
    pthread_mutex_lock(&g_sim.mu);
    sim_thread_t *v = sim_find_thread_locked(victim_pid, victim_tid);
    uint32_t v_prio = v ? v->eff_prio : 10U;
    uint32_t c = cpu % g_sim.num_cpus;
    if (g_sim.tracing) {
        /* Victim is preempted back to READY */
        sim_emit_thread_state_locked(c, QTG_TH_READY, victim_pid, victim_tid, v_prio,
                                     sim_tick_locked(2000ULL));
        /* Medium-priority preemptor runs on CPU for duration_us */
        sim_emit_thread_state_locked(c, QTG_TH_RUNNING, preemptor_pid, preemptor_tid, preemptor_prio,
                                     sim_tick_locked(2000ULL));
        /* Advance simulated time by duration_us while preemptor holds CPU */
        uint64_t resume_ts = sim_tick_locked((uint64_t)duration_us * 1000ULL);
        sim_emit_thread_state_locked(c, QTG_TH_READY, preemptor_pid, preemptor_tid, preemptor_prio,
                                     resume_ts);
        /* Victim finally gets CPU back */
        sim_emit_thread_state_locked(c, QTG_TH_RUNNING, victim_pid, victim_tid, v_prio,
                                     sim_tick_locked(2000ULL));
    }
    pthread_mutex_unlock(&g_sim.mu);
}

#endif /* __QNX__ */
