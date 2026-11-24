/*
 * QTraceGuard - Deterministic IPC Deadlock, Priority-Inversion & Scheduling
 *               Latency Verifier + QNX 7.1 APS -> QNX 8.0 SCHED_SPORADIC Advisor
 * SPDX-License-Identifier: MIT
 */

#include "qtg_analyzer.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Dynamic u64 vector for exact percentile calculation */
typedef struct qtg_u64_vec {
    uint64_t *data;
    size_t    len;
    size_t    cap;
} qtg_u64_vec_t;

static void qtg_u64_vec_push(qtg_u64_vec_t *v, uint64_t val) {
    if (v->len == v->cap) {
        size_t ncap = (v->cap == 0) ? 128U : (v->cap * 2U);
        uint64_t *nd = (uint64_t *)realloc(v->data, ncap * sizeof(uint64_t));
        if (!nd) return;
        v->data = nd;
        v->cap = ncap;
    }
    v->data[v->len++] = val;
}

static int qtg_cmp_u64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va < vb) ? -1 : (va > vb ? 1 : 0);
}

static uint64_t qtg_percentile(qtg_u64_vec_t *v, double pct) {
    if (!v || v->len == 0) return 0ULL;
    size_t idx = (size_t)((pct / 100.0) * (double)(v->len - 1U) + 0.5);
    if (idx >= v->len) idx = v->len - 1U;
    return v->data[idx];
}

void qtg_sla_policy_defaults(qtg_sla_policy_t *pol) {
    if (!pol) return;
    memset(pol, 0, sizeof(*pol));
    pol->max_dispatch_latency_us = 250ULL;
    pol->max_p99_dispatch_latency_us = 150ULL;
    pol->max_ipc_rtt_us = 2000ULL;
    pol->max_prio_inversion_us = 100ULL;
    pol->max_deadlock_cycles = 0U;
    pol->max_pulse_backlog = 16U;
    pol->forbid_fixed_priority_inversion = true;
    pol->forbid_aid_priority_cap = true;
    pol->forbid_aps_bankruptcy = true;
    pol->focus_proc[0] = '\0';
}

static bool qtg_json_extract_u64(const char *json, const char *key, uint64_t *out) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r')) p++;
    if (!isdigit((unsigned char)*p)) return false;
    *out = strtoull(p, NULL, 10);
    return true;
}

static bool qtg_json_extract_bool(const char *json, const char *key, bool *out) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r')) p++;
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static bool qtg_json_extract_str(const char *json, const char *key, char *out, size_t out_len) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r')) p++;
    if (*p != '"') return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1U < out_len) {
        if (*p == '\\' && p[1] != '\0') p++;
        out[n++] = *p++;
    }
    out[n] = '\0';
    return true;
}

int qtg_sla_policy_load_json(const char *path, qtg_sla_policy_t *pol, char *errbuf, size_t errlen) {
    if (!path || !pol) return -1;
    qtg_sla_policy_defaults(pol);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "cannot open SLA policy '%s': %s", path, strerror(errno));
        return -1;
    }
    char buf[16384];
    size_t n = fread(buf, 1U, sizeof(buf) - 1U, fp);
    fclose(fp);
    buf[n] = '\0';

    uint64_t tmp = 0;
    if (qtg_json_extract_u64(buf, "max_dispatch_latency_us", &tmp)) pol->max_dispatch_latency_us = tmp;
    if (qtg_json_extract_u64(buf, "max_p99_dispatch_latency_us", &tmp)) pol->max_p99_dispatch_latency_us = tmp;
    if (qtg_json_extract_u64(buf, "max_ipc_rtt_us", &tmp)) pol->max_ipc_rtt_us = tmp;
    if (qtg_json_extract_u64(buf, "max_prio_inversion_us", &tmp)) pol->max_prio_inversion_us = tmp;
    if (qtg_json_extract_u64(buf, "max_deadlock_cycles", &tmp)) pol->max_deadlock_cycles = (uint32_t)tmp;
    if (qtg_json_extract_u64(buf, "max_pulse_backlog", &tmp)) pol->max_pulse_backlog = (uint32_t)tmp;
    qtg_json_extract_bool(buf, "forbid_fixed_priority_inversion", &pol->forbid_fixed_priority_inversion);
    qtg_json_extract_bool(buf, "forbid_aid_priority_cap", &pol->forbid_aid_priority_cap);
    qtg_json_extract_bool(buf, "forbid_aps_bankruptcy", &pol->forbid_aps_bankruptcy);
    qtg_json_extract_str(buf, "focus_proc", pol->focus_proc, sizeof(pol->focus_proc));
    return 0;
}

/* Lookup / upsert helpers */
static qtg_proc_info_t *qtg_get_proc(qtg_report_t *rep, uint32_t pid) {
    for (size_t i = 0; i < rep->proc_count; i++) {
        if (rep->procs[i].pid == pid) return &rep->procs[i];
    }
    if (rep->proc_count >= QTG_MAX_PROCS) {
        rep->overflow_procs++;
        return NULL;
    }
    qtg_proc_info_t *p = &rep->procs[rep->proc_count++];
    memset(p, 0, sizeof(*p));
    p->pid = pid;
    snprintf(p->name, sizeof(p->name), "proc_%u", pid);
    p->has_aid_priority = true;
    p->prio_ceiling = 255U;
    return p;
}

static const qtg_channel_info_t *qtg_find_channel_ro(const qtg_report_t *rep, uint32_t owner_pid, uint32_t chid) {
    for (size_t i = 0; i < rep->channel_count; i++) {
        if (rep->channels[i].chid == chid &&
            (owner_pid == 0U || rep->channels[i].owner_pid == owner_pid)) {
            return &rep->channels[i];
        }
    }
    return NULL;
}

static bool qtg_proc_has_fixed_prio_channel(const qtg_report_t *rep, uint32_t owner_pid, uint32_t chid) {
    if (chid != 0U) {
        const qtg_channel_info_t *exact = qtg_find_channel_ro(rep, owner_pid, chid);
        if (exact && (exact->flags & QTG_CHF_FIXED_PRIORITY) != 0U) return true;
    }
    /* On native traces rcvid != chid; check process-level fixed-priority channel registrations */
    for (size_t i = 0; i < rep->channel_count; i++) {
        if (rep->channels[i].owner_pid == owner_pid &&
            (rep->channels[i].flags & QTG_CHF_FIXED_PRIORITY) != 0U &&
            (chid == 0U || rep->channels[i].chid == 0U || rep->channels[i].chid == chid)) {
            return true;
        }
    }
    return false;
}

static qtg_channel_info_t *qtg_get_channel(qtg_report_t *rep, uint32_t owner_pid, uint32_t chid) {
    for (size_t i = 0; i < rep->channel_count; i++) {
        if (rep->channels[i].chid == chid &&
            (owner_pid == 0U || rep->channels[i].owner_pid == owner_pid)) {
            return &rep->channels[i];
        }
    }
    if (rep->channel_count >= QTG_MAX_CHANNELS) {
        rep->overflow_channels++;
        return NULL;
    }
    qtg_channel_info_t *ch = &rep->channels[rep->channel_count++];
    memset(ch, 0, sizeof(*ch));
    ch->chid = chid;
    ch->owner_pid = owner_pid;
    ch->owner_tid = 1U;
    return ch;
}

static qtg_thread_stats_t *qtg_get_thread(qtg_report_t *rep, uint32_t pid, uint32_t tid) {
    if (tid == 0U) tid = 1U;
    for (size_t i = 0; i < rep->thread_count; i++) {
        if (rep->threads[i].pid == pid && rep->threads[i].tid == tid) {
            return &rep->threads[i];
        }
    }
    if (rep->thread_count >= QTG_MAX_THREADS) {
        rep->overflow_threads++;
        return NULL;
    }
    qtg_thread_stats_t *t = &rep->threads[rep->thread_count++];
    memset(t, 0, sizeof(*t));
    t->pid = pid;
    t->tid = tid;
    snprintf(t->name, sizeof(t->name), "T%u", tid);
    t->base_priority = 10U;
    t->effective_priority = 10U;
    t->current_state = QTG_TH_READY;
    qtg_get_proc(rep, pid);
    return t;
}

static qtg_wait_edge_t *qtg_get_edge(qtg_report_t *rep, uint32_t fp, uint32_t ft,
                                     uint32_t tp, uint32_t tt, uint32_t chid) {
    if (ft == 0U) ft = 1U;
    if (tt == 0U) tt = 1U;
    /* A thread cannot synchronously wait on itself via MsgSend/MsgReceive */
    if (fp == tp && ft == tt) return NULL;
    for (size_t i = 0; i < rep->edge_count; i++) {
        qtg_wait_edge_t *e = &rep->edges[i];
        if (e->from_pid == fp && e->from_tid == ft && e->to_pid == tp && e->to_tid == tt) {
            if (chid != 0U) e->chid = chid;
            return e;
        }
    }
    if (rep->edge_count >= QTG_MAX_EDGES) return NULL;
    qtg_wait_edge_t *e = &rep->edges[rep->edge_count++];
    memset(e, 0, sizeof(*e));
    e->from_pid = fp;
    e->from_tid = ft;
    e->to_pid = tp;
    e->to_tid = tt;
    e->chid = chid;
    return e;
}

static qtg_pulse_channel_stat_t *qtg_get_pulse_stat(qtg_report_t *rep, uint32_t pid, uint32_t chid) {
    for (size_t i = 0; i < rep->pulse_count; i++) {
        if (rep->pulses[i].pid == pid && rep->pulses[i].chid == chid) {
            return &rep->pulses[i];
        }
    }
    if (rep->pulse_count >= QTG_MAX_PULSE_LEAKS) return NULL;
    qtg_pulse_channel_stat_t *ps = &rep->pulses[rep->pulse_count++];
    memset(ps, 0, sizeof(*ps));
    ps->pid = pid;
    ps->chid = chid;
    return ps;
}

static qtg_aps_migration_t *qtg_get_aps(qtg_report_t *rep, uint32_t aps_id) {
    for (size_t i = 0; i < rep->aps_count; i++) {
        if (rep->aps_migrations[i].aps_id == aps_id) return &rep->aps_migrations[i];
    }
    if (rep->aps_count >= QTG_MAX_APS_PARTS) return NULL;
    qtg_aps_migration_t *a = &rep->aps_migrations[rep->aps_count++];
    memset(a, 0, sizeof(*a));
    a->aps_id = aps_id;
    snprintf(a->name, sizeof(a->name), "aps_part_%u", aps_id);
    a->window_ms = 100U;
    return a;
}

static void qtg_record_slice(qtg_report_t *rep, uint32_t cpu, uint32_t pid, uint32_t tid,
                             uint32_t state, uint32_t prio, uint64_t start_ns, uint64_t end_ns) {
    if (rep->slice_count >= QTG_MAX_TIMELINE_SLICE || end_ns <= start_ns) return;
    qtg_timeline_slice_t *s = &rep->slices[rep->slice_count++];
    s->cpu = cpu;
    s->pid = pid;
    s->tid = tid;
    s->state = state;
    s->priority = prio;
    s->start_ns = start_ns;
    s->end_ns = end_ns;
}

/* Canonicalize & deduplicate detected IPC wait-for deadlock cycle */
static void qtg_add_cycle(qtg_report_t *rep, const qtg_deadlock_cycle_t *cand) {
    if (cand->length == 0U || rep->cycle_count >= QTG_MAX_CYCLES) return;
    qtg_deadlock_cycle_t norm = *cand;
    /* Rotate so smallest (pid, tid) is at index 0 */
    uint32_t min_idx = 0U;
    for (uint32_t i = 1U; i < norm.length; i++) {
        if (norm.pids[i] < norm.pids[min_idx] ||
            (norm.pids[i] == norm.pids[min_idx] && norm.tids[i] < norm.tids[min_idx])) {
            min_idx = i;
        }
    }
    if (min_idx > 0U) {
        qtg_deadlock_cycle_t rot = norm;
        for (uint32_t i = 0U; i < norm.length; i++) {
            uint32_t src = (min_idx + i) % norm.length;
            rot.pids[i]  = norm.pids[src];
            rot.tids[i]  = norm.tids[src];
            rot.chids[i] = norm.chids[src];
        }
        norm = rot;
    }
    for (size_t i = 0; i < rep->cycle_count; i++) {
        if (rep->cycles[i].length != norm.length) continue;
        bool same = true;
        for (uint32_t k = 0; k < norm.length; k++) {
            if (rep->cycles[i].pids[k] != norm.pids[k] || rep->cycles[i].tids[k] != norm.tids[k]) {
                same = false;
                break;
            }
        }
        if (same) return;
    }
    rep->cycles[rep->cycle_count++] = norm;
}

/* Deterministic DFS cycle search across currently active IPC wait edges */
static int qtg_find_thread_idx(const qtg_report_t *rep, uint32_t pid, uint32_t tid) {
    for (size_t i = 0; i < rep->thread_count; i++) {
        if (rep->threads[i].pid == pid && rep->threads[i].tid == tid) return (int)i;
    }
    return -1;
}

static void qtg_detect_active_cycles(qtg_report_t *rep, uint64_t now_ns) {
    uint8_t state[QTG_MAX_THREADS];
    int     stack_node[QTG_MAX_THREADS];
    uint32_t stack_chid[QTG_MAX_THREADS];
    memset(state, 0, sizeof(state));

    for (size_t start = 0; start < rep->thread_count; start++) {
        if (state[start] != 0U) continue;

        /* Follow deterministic wait chain(s) via iterative DFS */
        size_t sp = 0;
        int cur = (int)start;
        while (cur >= 0 && sp < QTG_MAX_THREADS) {
            if (state[cur] == 2U) break;
            if (state[cur] == 1U) {
                /* Cycle found! Extract from stack back to cur */
                size_t pos = 0;
                while (pos < sp && stack_node[pos] != cur) pos++;
                if (pos < sp) {
                    qtg_deadlock_cycle_t cyc;
                    memset(&cyc, 0, sizeof(cyc));
                    cyc.detected_at_ns = now_ns;
                    for (size_t k = pos; k < sp && cyc.length < QTG_MAX_CYCLE_LEN; k++) {
                        int nidx = stack_node[k];
                        cyc.pids[cyc.length]  = rep->threads[nidx].pid;
                        cyc.tids[cyc.length]  = rep->threads[nidx].tid;
                        cyc.chids[cyc.length] = stack_chid[k];
                        cyc.length++;
                    }
                    qtg_add_cycle(rep, &cyc);
                }
                break;
            }
            state[cur] = 1U;
            stack_node[sp] = cur;
            stack_chid[sp] = 0U;

            /* Find an outgoing active wait edge from threads[cur] */
            int next_node = -1;
            for (size_t e = 0; e < rep->edge_count; e++) {
                const qtg_wait_edge_t *we = &rep->edges[e];
                if (!we->active) continue;
                if (we->from_pid == rep->threads[cur].pid && we->from_tid == rep->threads[cur].tid) {
                    stack_chid[sp] = we->chid;
                    next_node = qtg_find_thread_idx(rep, we->to_pid, we->to_tid);
                    break;
                }
            }
            sp++;
            cur = next_node;
        }
        for (size_t k = 0; k < sp; k++) {
            state[stack_node[k]] = 2U;
        }
    }
}

static bool qtg_proc_matches_focus(const qtg_report_t *rep, uint32_t pid, const char *focus);

/* Recompute effective priority for server thread from all active waiting clients */
static void qtg_recompute_server_priority(qtg_report_t *rep, uint32_t srv_pid, uint32_t srv_tid,
                                          uint32_t chid, const char *focus) {
    qtg_thread_stats_t *srv = qtg_get_thread(rep, srv_pid, srv_tid);
    qtg_proc_info_t    *sp  = qtg_get_proc(rep, srv_pid);
    if (!srv || !sp) return;

    uint32_t eff = srv->base_priority;
    bool fixed_prio = qtg_proc_has_fixed_prio_channel(rep, srv_pid, chid);

    for (size_t i = 0; i < rep->edge_count; i++) {
        const qtg_wait_edge_t *e = &rep->edges[i];
        if (!e->active || e->to_pid != srv_pid || e->to_tid != srv_tid) continue;
        qtg_thread_stats_t *cli = qtg_get_thread(rep, e->from_pid, e->from_tid);
        if (!cli) continue;
        if (fixed_prio) {
            continue; /* _NTO_CHF_FIXED_PRIORITY suppresses inheritance */
        }
        uint32_t boosted = cli->effective_priority;
        if (!sp->has_aid_priority && boosted > sp->prio_ceiling) {
            boosted = sp->prio_ceiling;
            if (qtg_proc_matches_focus(rep, cli->pid, focus) ||
                qtg_proc_matches_focus(rep, srv_pid, focus)) {
                /* Record QNX 8.0 PROCMGR_AID_PRIORITY ceiling cap observation (dedup by e->since_ns) */
                bool already = false;
                for (size_t k = 0; k < rep->inversion_count; k++) {
                    if (rep->inversions[k].kind == QTG_INV_AID_PRIORITY_CAP &&
                        rep->inversions[k].high_pid == cli->pid &&
                        rep->inversions[k].high_tid == cli->tid &&
                        rep->inversions[k].server_pid == srv_pid &&
                        rep->inversions[k].server_tid == srv_tid &&
                        rep->inversions[k].start_ns == e->since_ns) {
                        already = true;
                        break;
                    }
                }
                if (!already && rep->inversion_count < QTG_MAX_INVERSIONS) {
                    qtg_prio_inversion_t *inv = &rep->inversions[rep->inversion_count++];
                    memset(inv, 0, sizeof(*inv));
                    inv->kind = QTG_INV_AID_PRIORITY_CAP;
                    inv->high_pid = cli->pid;
                    inv->high_tid = cli->tid;
                    inv->high_prio = cli->effective_priority;
                    inv->server_pid = srv_pid;
                    inv->server_tid = srv_tid;
                    inv->server_prio = boosted;
                    inv->chid = e->chid;
                    inv->start_ns = e->since_ns;
                    inv->duration_ns = 0ULL;
                }
            }
        }
        if (boosted > eff) eff = boosted;
    }
    srv->effective_priority = eff;
}

typedef struct qtg_active_inv {
    bool   active;
    size_t inv_idx;
} qtg_active_inv_t;

#define QTG_MAX_PENDING_SMSG 256U

typedef struct qtg_pending_smsg {
    bool     active;
    uint32_t srv_pid;
    uint32_t rcvid;
    uint32_t cli_pid;
    uint32_t cli_tid;
    uint64_t send_ns;
} qtg_pending_smsg_t;

static bool qtg_proc_matches_focus(const qtg_report_t *rep, uint32_t pid, const char *focus) {
    if (!focus || focus[0] == '\0') return true;
    const char *pname = qtg_report_proc_name(rep, pid);
    return (pname && strstr(pname, focus) != NULL);
}

/* Check whether (start_pid, start_tid) currently reaches (goal_pid, goal_tid) via active IPC edges */
static bool qtg_can_reach_active(const qtg_report_t *rep,
                                 uint32_t start_pid, uint32_t start_tid,
                                 uint32_t goal_pid, uint32_t goal_tid) {
    int start_idx = qtg_find_thread_idx(rep, start_pid, start_tid);
    int goal_idx  = qtg_find_thread_idx(rep, goal_pid, goal_tid);
    if (start_idx < 0 || goal_idx < 0) return false;
    uint8_t vis[QTG_MAX_THREADS];
    memset(vis, 0, sizeof(vis));
    int cur = start_idx;
    while (cur >= 0 && !vis[cur]) {
        if (cur == goal_idx) return true;
        vis[cur] = 1U;
        int next = -1;
        for (size_t e = 0; e < rep->edge_count; e++) {
            const qtg_wait_edge_t *we = &rep->edges[e];
            if (we->active &&
                we->from_pid == rep->threads[cur].pid &&
                we->from_tid == rep->threads[cur].tid) {
                next = qtg_find_thread_idx(rep, we->to_pid, we->to_tid);
                break;
            }
        }
        cur = next;
    }
    return false;
}

static void qtg_clear_client_edges(qtg_report_t *rep, uint32_t cli_pid, uint32_t cli_tid,
                                   uint64_t ts, bool record_rtt, const char *focus) {
    for (size_t e = 0; e < rep->edge_count; e++) {
        qtg_wait_edge_t *we = &rep->edges[e];
        if (we->active && we->from_pid == cli_pid && we->from_tid == cli_tid) {
            if (record_rtt && ts >= we->since_ns) {
                uint64_t rtt = ts - we->since_ns;
                we->sum_rtt_ns += rtt;
                if (rtt > we->max_rtt_ns) we->max_rtt_ns = rtt;
            }
            uint32_t sp = we->to_pid;
            uint32_t st = we->to_tid;
            uint32_t ch = we->chid;
            we->active = false;
            qtg_recompute_server_priority(rep, sp, st, ch, focus);
        }
    }
}

static void qtg_clear_server_edges(qtg_report_t *rep, uint32_t srv_pid, uint32_t srv_tid) {
    for (size_t e = 0; e < rep->edge_count; e++) {
        qtg_wait_edge_t *we = &rep->edges[e];
        if (we->active && we->to_pid == srv_pid && we->to_tid == srv_tid) {
            we->active = false;
        }
    }
}

int qtg_analyze_trace(const qtg_trace_t *tr, const qtg_sla_policy_t *pol_in, qtg_report_t *rep) {
    if (!tr || !rep) return -1;
    memset(rep, 0, sizeof(*rep));
    rep->header = tr->header;
    rep->total_events = tr->count;
    rep->dropped_buffer_events = tr->dropped_buffer_events;

    qtg_sla_policy_t pol;
    if (pol_in) pol = *pol_in;
    else qtg_sla_policy_defaults(&pol);
    snprintf(rep->focus_proc, sizeof(rep->focus_proc), "%s", pol.focus_proc);

    qtg_u64_vec_t dispatch_samples = {0};
    qtg_u64_vec_t ipc_rtt_samples  = {0};

    /* Track currently running thread per CPU (for native QNX 8.0 2-word COMM & preemption) */
    uint32_t running_pid[64];
    uint32_t running_tid[64];
    memset(running_pid, 0, sizeof(running_pid));
    memset(running_tid, 0, sizeof(running_tid));

    qtg_active_inv_t active_inv_on_cpu[64];
    memset(active_inv_on_cpu, 0, sizeof(active_inv_on_cpu));

    qtg_pending_smsg_t pending_smsg[QTG_MAX_PENDING_SMSG];
    memset(pending_smsg, 0, sizeof(pending_smsg));

    if (tr->count > 0U) {
        rep->trace_start_ns = tr->events[0].timestamp_ns;
        rep->trace_end_ns   = tr->events[tr->count - 1U].timestamp_ns;
        rep->trace_duration_ns = (rep->trace_end_ns >= rep->trace_start_ns)
            ? (rep->trace_end_ns - rep->trace_start_ns) : 0ULL;
    }

    for (size_t i = 0; i < tr->count; i++) {
        const qtg_event_t *ev = &tr->events[i];
        uint32_t cpu = ev->cpu & 63U;
        uint64_t ts  = ev->timestamp_ns;

        if (ev->event_class == QTG_CLASS_PR_TH) {
            if (ev->event_id == QTG_PR_TH_CREATE_P_NAME && ev->word_count >= 2U) {
                uint32_t pid = ev->words[1];
                qtg_proc_info_t *p = qtg_get_proc(rep, pid);
                if (p && ev->word_count > 2U) {
                    qtg_extract_ascii_from_words(&ev->words[2], ev->word_count - 2U,
                                                 p->name, sizeof(p->name));
                }
            } else if (ev->event_id == QTG_PR_TH_NAME_T && ev->word_count >= 2U) {
                qtg_thread_stats_t *t = qtg_get_thread(rep, ev->words[0], ev->words[1]);
                if (t && ev->word_count > 2U) {
                    qtg_extract_ascii_from_words(&ev->words[2], ev->word_count - 2U,
                                                 t->name, sizeof(t->name));
                    if (strncmp(t->name, "idle", 4) == 0) {
                        t->is_idle = true;
                        t->base_priority = 0U;
                        t->effective_priority = 0U;
                        t->prio_known = true;
                    }
                }
            } else if (ev->event_id == QTG_TH_READY && ev->word_count >= 2U) {
                qtg_thread_stats_t *t = qtg_get_thread(rep, ev->words[0], ev->words[1]);
                if (t) {
                    /* Unblocked client (timeout/signal/reply) entering READY is no longer blocked in IPC */
                    if (t->in_send_or_reply) {
                        t->in_send_or_reply = false;
                        t->waiting_on_pid = 0U;
                        t->waiting_on_tid = 0U;
                        qtg_clear_client_edges(rep, t->pid, t->tid, ts, false, pol.focus_proc);
                    }
                    if (ev->word_count >= 3U) {
                        uint32_t ev_prio = ev->words[2];
                        if (ev_prio == 0U) {
                            t->is_idle = true;
                            t->base_priority = 0U;
                            t->effective_priority = 0U;
                            t->prio_known = true;
                        } else {
                            if (!t->prio_known) {
                                t->base_priority = ev_prio;
                                t->prio_known = true;
                            }
                            t->effective_priority = ev_prio;
                        }
                    }
                    if (t->in_running && ts > t->running_enter_ns) {
                        uint64_t dur = ts - t->running_enter_ns;
                        t->total_running_ns += dur;
                        qtg_record_slice(rep, t->last_cpu, t->pid, t->tid, QTG_TH_RUNNING,
                                         t->effective_priority, t->running_enter_ns, ts);
                        t->in_running = false;
                        if (running_pid[t->last_cpu & 63U] == t->pid && running_tid[t->last_cpu & 63U] == t->tid) {
                            running_pid[t->last_cpu & 63U] = 0U;
                            running_tid[t->last_cpu & 63U] = 0U;
                        }
                    }
                    t->current_state = QTG_TH_READY;
                    t->ready_enter_ns = ts;
                    t->in_ready = true;
                    t->last_cpu = cpu;
                }
            } else if (ev->event_id == QTG_TH_RUNNING && ev->word_count >= 2U) {
                uint32_t pid = ev->words[0];
                uint32_t tid = ev->words[1];
                qtg_thread_stats_t *t = qtg_get_thread(rep, pid, tid);

                /* If this thread migrated CPUs without an intermediate state event, close old CPU slice */
                if (t && t->in_running && t->last_cpu != cpu) {
                    if (ts > t->running_enter_ns) {
                        t->total_running_ns += (ts - t->running_enter_ns);
                        qtg_record_slice(rep, t->last_cpu, t->pid, t->tid, QTG_TH_RUNNING,
                                         t->effective_priority, t->running_enter_ns, ts);
                    }
                    if (running_pid[t->last_cpu & 63U] == pid && running_tid[t->last_cpu & 63U] == tid) {
                        running_pid[t->last_cpu & 63U] = 0U;
                        running_tid[t->last_cpu & 63U] = 0U;
                    }
                    t->in_running = false;
                }

                /* Close previous running slice on this CPU if different thread */
                if (running_pid[cpu] != 0U &&
                    (running_pid[cpu] != pid || running_tid[cpu] != tid)) {
                    qtg_thread_stats_t *prev = qtg_get_thread(rep, running_pid[cpu], running_tid[cpu]);
                    if (prev && prev->in_running && ts > prev->running_enter_ns) {
                        prev->total_running_ns += (ts - prev->running_enter_ns);
                        qtg_record_slice(rep, cpu, prev->pid, prev->tid, QTG_TH_RUNNING,
                                         prev->effective_priority, prev->running_enter_ns, ts);
                        prev->in_running = false;
                    }
                }
                /* Close any active inversion on this CPU */
                if (active_inv_on_cpu[cpu].active) {
                    qtg_prio_inversion_t *inv = &rep->inversions[active_inv_on_cpu[cpu].inv_idx];
                    if (ts > inv->start_ns) inv->duration_ns = ts - inv->start_ns;
                    active_inv_on_cpu[cpu].active = false;
                }

                if (t) {
                    /* A running thread cannot still be blocked in synchronous MsgSend/REPLY */
                    if (t->in_send_or_reply) {
                        t->in_send_or_reply = false;
                        t->waiting_on_pid = 0U;
                        t->waiting_on_tid = 0U;
                        qtg_clear_client_edges(rep, pid, tid, ts, false, pol.focus_proc);
                    }
                    if (ev->word_count >= 3U) {
                        uint32_t ev_prio = ev->words[2];
                        if (ev_prio == 0U) {
                            t->is_idle = true;
                            t->base_priority = 0U;
                            t->effective_priority = 0U;
                            t->prio_known = true;
                        } else {
                            if (!t->prio_known) {
                                t->base_priority = ev_prio;
                                t->prio_known = true;
                            }
                            t->effective_priority = ev_prio;
                        }
                    }
                    if (!t->is_idle && t->in_ready && ts >= t->ready_enter_ns) {
                        uint64_t lat = ts - t->ready_enter_ns;
                        t->dispatch_count++;
                        t->sum_dispatch_ns += lat;
                        if (lat > t->max_dispatch_ns) t->max_dispatch_ns = lat;
                        if (qtg_proc_matches_focus(rep, pid, pol.focus_proc)) {
                            qtg_u64_vec_push(&dispatch_samples, lat);
                        }
                        t->in_ready = false;
                    }
                    t->current_state = QTG_TH_RUNNING;
                    t->running_enter_ns = ts;
                    t->in_running = true;
                    t->last_cpu = cpu;
                    running_pid[cpu] = pid;
                    running_tid[cpu] = tid;

                    /* SMP-AWARE PRIORITY INVERSION CHECK (exclude idle threads prio=0):
                     * Require same CPU as the preempted READY server (or single-CPU trace)
                     * so independent work on another SMP core never triggers a false positive.
                     * Supports BOTH explicit markers AND zero-marker third-party QNX 8.0 traces! */
                    if (!t->is_idle && t->effective_priority > 0U) {
                        for (size_t e = 0; e < rep->edge_count; e++) {
                            const qtg_wait_edge_t *we = &rep->edges[e];
                            if (!we->active) continue;
                            qtg_thread_stats_t *cli = qtg_get_thread(rep, we->from_pid, we->from_tid);
                            qtg_thread_stats_t *srv = qtg_get_thread(rep, we->to_pid, we->to_tid);
                            if (!cli || !srv || cli->is_idle || srv->is_idle) continue;
                            if (srv->pid == t->pid && srv->tid == t->tid) continue;
                            if (cli->pid == t->pid && cli->tid == t->tid) continue;
                            if (rep->header.cpu_num > 1U && srv->last_cpu != cpu) continue;
                            if (!qtg_proc_matches_focus(rep, cli->pid, pol.focus_proc) &&
                                !qtg_proc_matches_focus(rep, srv->pid, pol.focus_proc)) {
                                continue;
                            }

                            if (srv->current_state == QTG_TH_READY &&
                                cli->effective_priority > t->effective_priority &&
                                t->effective_priority > srv->effective_priority) {
                                qtg_proc_info_t *sp = qtg_get_proc(rep, srv->pid);
                                qtg_inversion_kind_t kind = QTG_INV_CLASSIC_PREEMPT;
                                if (qtg_proc_has_fixed_prio_channel(rep, srv->pid, we->chid)) {
                                    kind = QTG_INV_FIXED_PRIO_CHANNEL;
                                } else if ((sp && !sp->has_aid_priority && cli->effective_priority > sp->prio_ceiling) ||
                                           (cli->effective_priority > QTG_QNX8_UNPRIV_PRIO_CEILING &&
                                            srv->effective_priority == QTG_QNX8_UNPRIV_PRIO_CEILING &&
                                            srv->base_priority < QTG_QNX8_UNPRIV_PRIO_CEILING)) {
                                    /* Either explicit ability marker OR native QNX 8.0 kernel ceiling-63 cap! */
                                    kind = QTG_INV_AID_PRIORITY_CAP;
                                } else if (srv->prio_known &&
                                           srv->effective_priority == srv->base_priority &&
                                           cli->effective_priority > srv->base_priority) {
                                    /* Zero-marker native QNX 8.0 inference: server did not inherit client prio */
                                    kind = QTG_INV_FIXED_PRIO_CHANNEL;
                                }

                                /* Upgrade existing cap-only record for this active request if present */
                                size_t target_idx = rep->inversion_count;
                                if (kind == QTG_INV_AID_PRIORITY_CAP && rep->inversion_count > 0U) {
                                    qtg_prio_inversion_t *last = &rep->inversions[rep->inversion_count - 1U];
                                    if (last->kind == QTG_INV_AID_PRIORITY_CAP &&
                                        last->preemptor_pid == 0U &&
                                        last->high_pid == cli->pid &&
                                        last->server_pid == srv->pid &&
                                        last->chid == we->chid) {
                                        target_idx = rep->inversion_count - 1U;
                                    }
                                }
                                if (target_idx < QTG_MAX_INVERSIONS) {
                                    if (target_idx == rep->inversion_count) rep->inversion_count++;
                                    qtg_prio_inversion_t *inv = &rep->inversions[target_idx];
                                    memset(inv, 0, sizeof(*inv));
                                    inv->kind = kind;
                                    inv->high_pid = cli->pid;
                                    inv->high_tid = cli->tid;
                                    inv->high_prio = cli->effective_priority;
                                    inv->server_pid = srv->pid;
                                    inv->server_tid = srv->tid;
                                    inv->server_prio = srv->effective_priority;
                                    inv->preemptor_pid = t->pid;
                                    inv->preemptor_tid = t->tid;
                                    inv->preemptor_prio = t->effective_priority;
                                    inv->chid = we->chid;
                                    inv->start_ns = ts;
                                    active_inv_on_cpu[cpu].active = true;
                                    active_inv_on_cpu[cpu].inv_idx = target_idx;
                                }
                                break;
                            }
                        }
                    }
                }
            } else if ((ev->event_id == QTG_TH_SEND || ev->event_id == QTG_TH_RECEIVE ||
                        ev->event_id == QTG_TH_REPLY || ev->event_id == QTG_TH_MUTEX ||
                        ev->event_id == QTG_TH_DEAD) && ev->word_count >= 2U) {
                qtg_thread_stats_t *t = qtg_get_thread(rep, ev->words[0], ev->words[1]);
                if (t) {
                    if (t->in_running && ts > t->running_enter_ns) {
                        t->total_running_ns += (ts - t->running_enter_ns);
                        qtg_record_slice(rep, t->last_cpu, t->pid, t->tid, QTG_TH_RUNNING,
                                         t->effective_priority, t->running_enter_ns, ts);
                        t->in_running = false;
                        if (running_pid[t->last_cpu & 63U] == t->pid && running_tid[t->last_cpu & 63U] == t->tid) {
                            running_pid[t->last_cpu & 63U] = 0U;
                            running_tid[t->last_cpu & 63U] = 0U;
                        }
                    }
                    t->current_state = ev->event_id;
                    t->in_ready = false;
                    if (ev->event_id == QTG_TH_DEAD) {
                        t->in_send_or_reply = false;
                        t->waiting_on_pid = 0U;
                        t->waiting_on_tid = 0U;
                        qtg_clear_client_edges(rep, t->pid, t->tid, ts, false, pol.focus_proc);
                        qtg_clear_server_edges(rep, t->pid, t->tid);
                    }
                    if (ev->word_count >= 3U) {
                        uint32_t ev_prio = ev->words[2];
                        if (ev_prio == 0U) {
                            t->is_idle = true;
                            t->effective_priority = 0U;
                        } else {
                            if (!t->prio_known) {
                                t->base_priority = ev_prio;
                                t->prio_known = true;
                            }
                            t->effective_priority = ev_prio;
                        }
                    }
                    /* Native QNX 8.0 blocked-SMSG deadlock closing-edge check:
                     * Only infer a blocked closing edge when the caller actually entered THSEND
                     * AND the target server process has no live runnable/receiving worker thread
                     * AND a blocked thread in srv_pid already transitively waits on this caller! */
                    if (ev->event_id == QTG_TH_SEND && t->in_send_or_reply && t->waiting_on_pid != 0U) {
                        uint32_t spid = t->waiting_on_pid;
                        bool srv_has_free_worker = false;
                        for (size_t k = 0; k < rep->thread_count; k++) {
                            const qtg_thread_stats_t *w = &rep->threads[k];
                            if (w->pid != spid) continue;
                            if (w->pid == t->pid && w->tid == t->tid) continue;
                            if (w->current_state == QTG_TH_RECEIVE ||
                                w->current_state == QTG_TH_RUNNING ||
                                w->current_state == QTG_TH_READY) {
                                srv_has_free_worker = true;
                                break;
                            }
                        }
                        if (!srv_has_free_worker) {
                            for (size_t k = 0; k < rep->thread_count; k++) {
                                const qtg_thread_stats_t *cand = &rep->threads[k];
                                if (cand->pid != spid) continue;
                                if (cand->pid == t->pid && cand->tid == t->tid) continue;
                                if (cand->current_state != QTG_TH_SEND &&
                                    cand->current_state != QTG_TH_REPLY) {
                                    continue;
                                }
                                if (qtg_can_reach_active(rep, cand->pid, cand->tid, t->pid, t->tid)) {
                                    qtg_wait_edge_t *we = qtg_get_edge(rep, t->pid, t->tid,
                                                                       cand->pid, cand->tid,
                                                                       t->waiting_on_chid);
                                    if (we) {
                                        we->active = true;
                                        we->observed_ever = true;
                                        we->since_ns = t->send_enter_ns ? t->send_enter_ns : ts;
                                        we->msg_count++;
                                        t->waiting_on_tid = cand->tid;
                                        qtg_detect_active_cycles(rep, ts);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        } else if (ev->event_class == QTG_CLASS_COMM) {
            bool native_qnx_comm = (!ev->is_combine && ev->word_count == 2U);

            if (ev->event_id == QTG_COMM_SMSG && ev->word_count >= 2U) {
                if (native_qnx_comm) {
                    /* Native QNX OS 8.0 2-word SEND_MESSAGE: w0=rcvid, w1=server_pid;
                     * sender is the currently RUNNING thread on ev->cpu. */
                    uint32_t rcvid   = ev->words[0];
                    uint32_t srv_pid = ev->words[1];
                    uint32_t cli_pid = running_pid[cpu];
                    uint32_t cli_tid = running_tid[cpu];
                    if (cli_pid != 0U && srv_pid != 0U) {
                        qtg_thread_stats_t *cli = qtg_get_thread(rep, cli_pid, cli_tid);
                        if (cli) {
                            cli->send_enter_ns = ts;
                            cli->in_send_or_reply = true;
                            cli->waiting_on_pid = srv_pid;
                            cli->waiting_on_tid = 0U;
                            cli->waiting_on_chid = rcvid;
                            cli->current_state = QTG_TH_SEND;
                        }
                        /* Record pending SMSG for exact RMSG correlation */
                        size_t slot = 0U;
                        bool placed = false;
                        for (size_t k = 0; k < QTG_MAX_PENDING_SMSG; k++) {
                            if (!pending_smsg[k].active ||
                                (pending_smsg[k].srv_pid == srv_pid && pending_smsg[k].rcvid == rcvid)) {
                                slot = k;
                                placed = true;
                                break;
                            }
                        }
                        if (!placed) slot = rcvid % QTG_MAX_PENDING_SMSG;
                        pending_smsg[slot].active  = true;
                        pending_smsg[slot].srv_pid = srv_pid;
                        pending_smsg[slot].rcvid   = rcvid;
                        pending_smsg[slot].cli_pid = cli_pid;
                        pending_smsg[slot].cli_tid = cli_tid;
                        pending_smsg[slot].send_ns = ts;
                    }
                } else {
                    /* Extended combine SMSG (POSIX simulation / enriched trace) */
                    uint32_t chid    = ev->words[0];
                    uint32_t srv_pid = ev->words[1];
                    uint32_t cli_pid = (ev->word_count >= 3U && ev->words[2] != 0U) ? ev->words[2] : running_pid[cpu];
                    uint32_t cli_tid = (ev->word_count >= 4U && ev->words[3] != 0U) ? ev->words[3] : running_tid[cpu];
                    uint32_t srv_tid = (ev->word_count >= 5U && ev->words[4] != 0U) ? ev->words[4] : 0U;
                    uint32_t c_prio  = (ev->word_count >= 6U) ? ev->words[5] : 0U;

                    if (cli_pid != 0U && srv_pid != 0U) {
                        /* Only default unknown srv_tid to 1 when cross-process (never forge self-edge!) */
                        if (srv_tid == 0U && srv_pid != cli_pid) srv_tid = 1U;
                        qtg_thread_stats_t *cli = qtg_get_thread(rep, cli_pid, cli_tid);
                        if (cli && c_prio > 0U) {
                            cli->base_priority = c_prio;
                            cli->prio_known = true;
                            if (cli->effective_priority < c_prio) cli->effective_priority = c_prio;
                        }
                        if (cli) {
                            cli->send_enter_ns = ts;
                            cli->in_send_or_reply = true;
                            cli->waiting_on_pid = srv_pid;
                            cli->waiting_on_tid = srv_tid;
                            cli->waiting_on_chid = chid;
                            cli->current_state = QTG_TH_SEND;
                        }
                        if (srv_tid != 0U && !(cli_pid == srv_pid && cli_tid == srv_tid)) {
                            qtg_thread_stats_t *srv = qtg_get_thread(rep, srv_pid, srv_tid);
                            qtg_wait_edge_t *we = qtg_get_edge(rep, cli_pid, cli_tid, srv_pid, srv_tid, chid);
                            if (we) {
                                we->active = true;
                                we->observed_ever = true;
                                we->since_ns = ts;
                                we->msg_count++;
                            }
                            if (srv) {
                                qtg_recompute_server_priority(rep, srv_pid, srv_tid, chid, pol.focus_proc);
                            }
                            qtg_detect_active_cycles(rep, ts);
                        }
                    }
                }
            } else if (ev->event_id == QTG_COMM_RMSG && ev->word_count >= 2U) {
                if (native_qnx_comm) {
                    /* Native QNX OS 8.0 2-word REC_MESSAGE: w0=rcvid, w1=server_pid;
                     * server thread is currently running on ev->cpu. Correlate with pending SMSG. */
                    uint32_t rcvid   = ev->words[0];
                    uint32_t srv_pid = ev->words[1];
                    uint32_t srv_tid = (running_pid[cpu] == srv_pid && running_tid[cpu] != 0U)
                                       ? running_tid[cpu] : 0U;
                    uint32_t cli_pid = 0U, cli_tid = 0U;
                    uint64_t send_ts = ts;
                    for (size_t k = 0; k < QTG_MAX_PENDING_SMSG; k++) {
                        if (pending_smsg[k].active &&
                            pending_smsg[k].srv_pid == srv_pid &&
                            pending_smsg[k].rcvid == rcvid) {
                            cli_pid = pending_smsg[k].cli_pid;
                            cli_tid = pending_smsg[k].cli_tid;
                            send_ts = pending_smsg[k].send_ns;
                            pending_smsg[k].active = false;
                            break;
                        }
                    }
                    if (srv_tid == 0U && cli_pid != 0U && srv_pid != cli_pid) {
                        srv_tid = 1U;
                    }
                    if (cli_pid != 0U && srv_pid != 0U && srv_tid != 0U &&
                        !(cli_pid == srv_pid && cli_tid == srv_tid)) {
                        qtg_thread_stats_t *cli = qtg_get_thread(rep, cli_pid, cli_tid);
                        if (cli) {
                            cli->current_state = QTG_TH_REPLY;
                            cli->waiting_on_pid = srv_pid;
                            cli->waiting_on_tid = srv_tid;
                            cli->waiting_on_chid = rcvid;
                            cli->in_send_or_reply = true;
                        }
                        qtg_wait_edge_t *we = qtg_get_edge(rep, cli_pid, cli_tid, srv_pid, srv_tid, rcvid);
                        if (we) {
                            we->rcvid = rcvid;
                            we->active = true;
                            we->observed_ever = true;
                            we->since_ns = send_ts;
                            we->msg_count++;
                        }
                        qtg_recompute_server_priority(rep, srv_pid, srv_tid, rcvid, pol.focus_proc);
                        qtg_detect_active_cycles(rep, ts);
                    }
                } else {
                    uint32_t rcvid   = ev->words[0];
                    uint32_t cli_pid = ev->words[1];
                    uint32_t srv_pid = (ev->word_count >= 3U && ev->words[2] != 0U) ? ev->words[2] : running_pid[cpu];
                    uint32_t srv_tid = (ev->word_count >= 4U && ev->words[3] != 0U) ? ev->words[3] : running_tid[cpu];
                    uint32_t cli_tid = (ev->word_count >= 5U && ev->words[4] != 0U) ? ev->words[4] : 0U;
                    uint32_t chid    = (ev->word_count >= 6U) ? ev->words[5] : 0U;

                    if (cli_tid == 0U && cli_pid != srv_pid) cli_tid = 1U;
                    if (srv_tid == 0U && cli_pid != srv_pid) srv_tid = 1U;
                    if (cli_pid != 0U && srv_pid != 0U && cli_tid != 0U && srv_tid != 0U &&
                        !(cli_pid == srv_pid && cli_tid == srv_tid)) {
                        qtg_thread_stats_t *cli = qtg_get_thread(rep, cli_pid, cli_tid);
                        if (cli) cli->current_state = QTG_TH_REPLY;
                        qtg_wait_edge_t *we = qtg_get_edge(rep, cli_pid, cli_tid, srv_pid, srv_tid, chid);
                        if (we) {
                            we->rcvid = rcvid;
                            if (!we->active) {
                                we->active = true;
                                we->observed_ever = true;
                                we->since_ns = ts;
                            }
                        }
                        qtg_recompute_server_priority(rep, srv_pid, srv_tid, chid, pol.focus_proc);
                    }
                }
            } else if ((ev->event_id == QTG_COMM_REPLY || ev->event_id == QTG_COMM_ERROR) &&
                       ev->word_count >= 2U) {
                uint32_t cli_tid = ev->words[0];
                uint32_t cli_pid = ev->words[1];
                uint32_t srv_pid = (ev->word_count >= 3U && ev->words[2] != 0U) ? ev->words[2] : 0U;
                uint32_t srv_tid = (ev->word_count >= 4U && ev->words[3] != 0U) ? ev->words[3] : 0U;
                uint32_t chid    = (ev->word_count >= 5U) ? ev->words[4] : 0U;

                qtg_thread_stats_t *cli = qtg_get_thread(rep, cli_pid, cli_tid);
                if (cli && cli->in_send_or_reply && ts >= cli->send_enter_ns) {
                    uint64_t rtt = ts - cli->send_enter_ns;
                    cli->ipc_rtt_count++;
                    cli->sum_ipc_rtt_ns += rtt;
                    if (rtt > cli->max_ipc_rtt_ns) cli->max_ipc_rtt_ns = rtt;
                    if (qtg_proc_matches_focus(rep, cli_pid, pol.focus_proc)) {
                        qtg_u64_vec_push(&ipc_rtt_samples, rtt);
                    }
                    cli->in_send_or_reply = false;
                    cli->waiting_on_pid = 0U;
                    cli->waiting_on_tid = 0U;
                }
                /* Clear ALL active outgoing wait edges for this client */
                qtg_clear_client_edges(rep, cli_pid, cli_tid, ts, true, pol.focus_proc);
                if (srv_pid != 0U) {
                    qtg_recompute_server_priority(rep, srv_pid, srv_tid ? srv_tid : 1U, chid, pol.focus_proc);
                }
            } else if (ev->event_id == QTG_COMM_SPULSE || ev->event_id == QTG_COMM_SPULSE_UN ||
                       ev->event_id == QTG_COMM_SPULSE_EXE || ev->event_id == QTG_COMM_SPULSE_DIS ||
                       ev->event_id == QTG_COMM_SPULSE_DEA || ev->event_id == QTG_COMM_SPULSE_QUN) {
                if (ev->word_count >= 2U) {
                    qtg_pulse_channel_stat_t *ps = qtg_get_pulse_stat(rep, ev->words[1], ev->words[0]);
                    if (ps) {
                        ps->sent_count++;
                        ps->current_backlog++;
                        if (ev->event_id == QTG_COMM_SPULSE_UN) ps->unblock_pulses++;
                        if (ps->current_backlog > ps->max_backlog) {
                            ps->max_backlog = ps->current_backlog;
                        }
                    }
                }
            } else if (ev->event_id == QTG_COMM_RPULSE && ev->word_count >= 2U) {
                qtg_pulse_channel_stat_t *ps = qtg_get_pulse_stat(rep, ev->words[1], ev->words[0]);
                if (ps) {
                    ps->received_count++;
                    if (ps->current_backlog > 0ULL) ps->current_backlog--;
                }
            }
        } else if (ev->event_class == QTG_CLASS_SYSTEM) {
            if (ev->event_id == QTG_SYS_CH_FLAGS && ev->word_count >= 3U) {
                qtg_channel_info_t *ch = qtg_get_channel(rep, ev->words[0], ev->words[1]);
                if (ch) {
                    ch->flags = ev->words[2];
                    if (ev->word_count >= 4U && ev->words[3] != 0U) ch->owner_tid = ev->words[3];
                }
            } else if (ev->event_id == QTG_SYS_RESMGR_ATTACH && ev->word_count >= 2U) {
                qtg_channel_info_t *ch = qtg_get_channel(rep, ev->words[0], ev->words[1]);
                if (ch && ev->word_count > 2U) {
                    qtg_extract_ascii_from_words(&ev->words[2], ev->word_count - 2U,
                                                 ch->resmgr_path, sizeof(ch->resmgr_path));
                }
            } else if (ev->event_id == QTG_SYS_ABILITY_CAP && ev->word_count >= 3U) {
                qtg_proc_info_t *p = qtg_get_proc(rep, ev->words[0]);
                if (p) {
                    p->has_aid_priority = (ev->words[1] != 0U);
                    p->prio_ceiling = (ev->words[2] > 0U) ? ev->words[2] : QTG_QNX8_UNPRIV_PRIO_CEILING;
                }
            } else if (ev->event_id == QTG_SYS_SPORADIC_REPL && ev->word_count >= 2U) {
                qtg_thread_stats_t *t = qtg_get_thread(rep, ev->words[0], ev->words[1]);
                if (t) t->sporadic_repl_count++;
            } else if (ev->event_id == QTG_SYS_APS_NAME && ev->word_count >= 2U) {
                qtg_aps_migration_t *a = qtg_get_aps(rep, ev->words[0]);
                if (a && ev->word_count > 2U) {
                    qtg_extract_ascii_from_words(&ev->words[2], ev->word_count - 2U,
                                                 a->name, sizeof(a->name));
                }
            } else if (ev->event_id == QTG_SYS_APS_BUDGETS && ev->word_count >= 2U) {
                qtg_aps_migration_t *a = qtg_get_aps(rep, ev->words[0]);
                if (a) {
                    a->budget_percent = ev->words[1];
                    if (ev->word_count >= 3U) a->critical_budget_ms = ev->words[2];
                    if (ev->word_count >= 4U && ev->words[3] > 0U) a->window_ms = ev->words[3];
                }
            } else if (ev->event_id == QTG_SYS_APS_BNKR && ev->word_count >= 1U) {
                qtg_aps_migration_t *a = qtg_get_aps(rep, ev->words[0]);
                if (a) a->bankruptcy_events++;
            }
        } else if (ev->event_class == QTG_CLASS_USER && ev->word_count >= 2U) {
            /* Live QNX OS 8.0 TraceEvent(_NTO_TRACE_INSERTSUSEREVENT, ...) diagnostic markers */
            if (ev->event_id == QTG_SYS_CH_FLAGS) {
                qtg_channel_info_t *ch = qtg_get_channel(rep, ev->words[0], 0U);
                if (ch) ch->flags |= ev->words[1];
            } else if (ev->event_id == QTG_SYS_ABILITY_CAP) {
                qtg_proc_info_t *p = qtg_get_proc(rep, ev->words[0]);
                if (p) {
                    p->has_aid_priority = false;
                    p->prio_ceiling = (ev->words[1] > 0U) ? ev->words[1] : QTG_QNX8_UNPRIV_PRIO_CEILING;
                }
            }
        }
    }

    /* Final sweep for any still-open inversions or residual deadlock cycles at end-of-trace */
    for (uint32_t c = 0; c < 64U; c++) {
        if (active_inv_on_cpu[c].active) {
            qtg_prio_inversion_t *inv = &rep->inversions[active_inv_on_cpu[c].inv_idx];
            if (rep->trace_end_ns > inv->start_ns) {
                inv->duration_ns = rep->trace_end_ns - inv->start_ns;
            }
        }
    }
    qtg_detect_active_cycles(rep, rep->trace_end_ns);

    /* Filter cycles by --focus if specified */
    if (pol.focus_proc[0] != '\0' && rep->cycle_count > 0U) {
        size_t kept = 0U;
        for (size_t i = 0; i < rep->cycle_count; i++) {
            bool match = false;
            for (uint32_t k = 0; k < rep->cycles[i].length; k++) {
                if (qtg_proc_matches_focus(rep, rep->cycles[i].pids[k], pol.focus_proc)) {
                    match = true;
                    break;
                }
            }
            if (match) {
                if (kept != i) rep->cycles[kept] = rep->cycles[i];
                kept++;
            }
        }
        rep->cycle_count = kept;
    }

    /* Count focused threads for fail-closed validation */
    rep->matched_focus_threads = 0U;
    for (size_t i = 0; i < rep->thread_count; i++) {
        if (!rep->threads[i].is_idle &&
            qtg_proc_matches_focus(rep, rep->threads[i].pid, pol.focus_proc)) {
            rep->matched_focus_threads++;
        }
    }

    /* Finalize pulse leak detection (respects --focus filter) */
    for (size_t i = 0; i < rep->pulse_count; i++) {
        qtg_pulse_channel_stat_t *ps = &rep->pulses[i];
        if (!qtg_proc_matches_focus(rep, ps->pid, pol.focus_proc)) continue;
        if (ps->max_backlog > pol.max_pulse_backlog || ps->current_backlog > pol.max_pulse_backlog) {
            ps->leak_detected = true;
        }
    }

    /* Compute QNX 7.1 APS -> QNX 8.0 SCHED_SPORADIC migration recommendations */
    for (size_t i = 0; i < rep->aps_count; i++) {
        qtg_aps_migration_t *a = &rep->aps_migrations[i];
        uint32_t win = (a->window_ms > 0U) ? a->window_ms : 100U;
        uint32_t pct = a->budget_percent;
        if (pct == 0U) pct = 1U;
        if (pct > 100U) pct = 100U;
        uint32_t budget = (win * pct) / 100U;
        if (budget == 0U) budget = 1U;
        if (a->bankruptcy_events > 0U) {
            /* Add 20% deterministic safety margin when legacy APS experienced bankruptcies */
            uint32_t boosted = (budget * 120U + 99U) / 100U;
            if (boosted > win) boosted = win;
            budget = boosted;
        }
        a->rec_repl_period_ms  = win;
        a->rec_init_budget_ms  = budget;
        a->rec_normal_priority = (a->critical_budget_ms > 0U) ? 72U : 40U;
        a->rec_low_priority    = 10U;
        a->rec_max_repl        = 4U;
        a->requires_aid_priority = (a->rec_normal_priority > QTG_QNX8_UNPRIV_PRIO_CEILING);
    }

    /* Compute exact aggregate latency percentiles */
    if (dispatch_samples.len > 0U) {
        qsort(dispatch_samples.data, dispatch_samples.len, sizeof(uint64_t), qtg_cmp_u64);
        rep->dispatch_p50_ns = qtg_percentile(&dispatch_samples, 50.0);
        rep->dispatch_p95_ns = qtg_percentile(&dispatch_samples, 95.0);
        rep->dispatch_p99_ns = qtg_percentile(&dispatch_samples, 99.0);
        rep->dispatch_max_ns = dispatch_samples.data[dispatch_samples.len - 1U];
    }
    if (ipc_rtt_samples.len > 0U) {
        qsort(ipc_rtt_samples.data, ipc_rtt_samples.len, sizeof(uint64_t), qtg_cmp_u64);
        rep->ipc_rtt_p50_ns = qtg_percentile(&ipc_rtt_samples, 50.0);
        rep->ipc_rtt_p95_ns = qtg_percentile(&ipc_rtt_samples, 95.0);
        rep->ipc_rtt_p99_ns = qtg_percentile(&ipc_rtt_samples, 99.0);
        rep->ipc_rtt_max_ns = ipc_rtt_samples.data[ipc_rtt_samples.len - 1U];
    }
    free(dispatch_samples.data);
    free(ipc_rtt_samples.data);

    /* Evaluate Real-Time SLA Gates (FAIL-CLOSED on empty, dropped, overflow, or unmatched focus!) */
    rep->sla.passed = true;
    rep->sla.violation_count = 0U;
    #define ADD_VIOL(...) do { \
        rep->sla.passed = false; \
        if (rep->sla.violation_count < 16U) { \
            snprintf(rep->sla.violations[rep->sla.violation_count++], 192U, __VA_ARGS__); \
        } \
    } while (0)

    if (rep->total_events == 0ULL) {
        ADD_VIOL("EMPTY TRACE: 0 kernel events decoded (fail-closed safety gate)");
    }
    if (pol.focus_proc[0] != '\0' && rep->matched_focus_threads == 0U) {
        ADD_VIOL("FOCUS MISMATCH: --focus '%s' matched 0 threads in trace (fail-closed)", pol.focus_proc);
    }
    if (rep->dropped_buffer_events > 0ULL) {
        ADD_VIOL("DROPPED TRACE BUFFERS: tracelogger dropped %llu kernel event buffer(s) (incomplete trace)",
                 (unsigned long long)rep->dropped_buffer_events);
    }
    if (rep->overflow_procs > 0U || rep->overflow_threads > 0U) {
        ADD_VIOL("TABLE CAPACITY OVERFLOW: dropped %u proc(s), %u thread(s) beyond static cap",
                 rep->overflow_procs, rep->overflow_threads);
    }
    if (rep->cycle_count > pol.max_deadlock_cycles) {
        ADD_VIOL("IPC DEADLOCK: detected %zu wait-for cycle(s) (limit: %u)",
                 rep->cycle_count, pol.max_deadlock_cycles);
    }
    if (pol.max_dispatch_latency_us <= (UINT64_MAX / 1000ULL) &&
        rep->dispatch_max_ns > pol.max_dispatch_latency_us * 1000ULL) {
        ADD_VIOL("DISPATCH MAX LATENCY: %.2f us exceeds limit %llu us",
                 (double)rep->dispatch_max_ns / 1000.0,
                 (unsigned long long)pol.max_dispatch_latency_us);
    }
    if (pol.max_p99_dispatch_latency_us <= (UINT64_MAX / 1000ULL) &&
        rep->dispatch_p99_ns > pol.max_p99_dispatch_latency_us * 1000ULL) {
        ADD_VIOL("DISPATCH P99 LATENCY: %.2f us exceeds limit %llu us",
                 (double)rep->dispatch_p99_ns / 1000.0,
                 (unsigned long long)pol.max_p99_dispatch_latency_us);
    }
    if (pol.max_ipc_rtt_us <= (UINT64_MAX / 1000ULL) &&
        rep->ipc_rtt_max_ns > pol.max_ipc_rtt_us * 1000ULL) {
        ADD_VIOL("IPC RTT MAX: %.2f us exceeds limit %llu us",
                 (double)rep->ipc_rtt_max_ns / 1000.0,
                 (unsigned long long)pol.max_ipc_rtt_us);
    }

    uint64_t max_inv_ns = 0ULL;
    size_t fixed_inv_cnt = 0U, aid_cap_cnt = 0U;
    for (size_t i = 0; i < rep->inversion_count; i++) {
        if (rep->inversions[i].duration_ns > max_inv_ns) max_inv_ns = rep->inversions[i].duration_ns;
        if (rep->inversions[i].kind == QTG_INV_FIXED_PRIO_CHANNEL) fixed_inv_cnt++;
        if (rep->inversions[i].kind == QTG_INV_AID_PRIORITY_CAP) aid_cap_cnt++;
    }
    if (pol.max_prio_inversion_us <= (UINT64_MAX / 1000ULL) &&
        max_inv_ns > pol.max_prio_inversion_us * 1000ULL) {
        ADD_VIOL("PRIORITY INVERSION: max window %.2f us exceeds limit %llu us",
                 (double)max_inv_ns / 1000.0, (unsigned long long)pol.max_prio_inversion_us);
    }
    if (pol.forbid_fixed_priority_inversion && fixed_inv_cnt > 0U) {
        ADD_VIOL("_NTO_CHF_FIXED_PRIORITY TRAP: %zu priority-inversion event(s) on fixed-priority channel",
                 fixed_inv_cnt);
    }
    if (pol.forbid_aid_priority_cap && aid_cap_cnt > 0U) {
        ADD_VIOL("QNX 8.0 PROCMGR_AID_PRIORITY CAP: %zu server inheritance cap event(s) at ceiling 63",
                 aid_cap_cnt);
    }
    for (size_t i = 0; i < rep->pulse_count; i++) {
        if (rep->pulses[i].leak_detected) {
            ADD_VIOL("PULSE QUEUE SATURATION: %s ch:%u max_backlog=%llu unreceived=%llu (limit: %u)",
                     qtg_report_proc_name(rep, rep->pulses[i].pid),
                     rep->pulses[i].chid,
                     (unsigned long long)rep->pulses[i].max_backlog,
                     (unsigned long long)rep->pulses[i].current_backlog,
                     pol.max_pulse_backlog);
        }
    }
    if (pol.forbid_aps_bankruptcy) {
        for (size_t i = 0; i < rep->aps_count; i++) {
            if (rep->aps_migrations[i].bankruptcy_events > 0U) {
                ADD_VIOL("LEGACY APS BANKRUPTCY: partition '%s' had %u bankruptcy event(s) -> migrate to QNX 8.0 SCHED_SPORADIC",
                         rep->aps_migrations[i].name, rep->aps_migrations[i].bankruptcy_events);
            }
        }
    }
    #undef ADD_VIOL

    return 0;
}

const char *qtg_report_proc_name(const qtg_report_t *rep, uint32_t pid) {
    if (!rep) return "unknown";
    for (size_t i = 0; i < rep->proc_count; i++) {
        if (rep->procs[i].pid == pid) return rep->procs[i].name;
    }
    return "unknown";
}

const char *qtg_inversion_kind_str(qtg_inversion_kind_t kind) {
    switch (kind) {
        case QTG_INV_CLASSIC_PREEMPT:    return "CLASSIC_PREEMPT";
        case QTG_INV_FIXED_PRIO_CHANNEL: return "FIXED_PRIORITY_CHANNEL";
        case QTG_INV_AID_PRIORITY_CAP:   return "PROCMGR_AID_PRIORITY_CAP";
        default:                         return "UNKNOWN";
    }
}

void qtg_report_print_text(const qtg_report_t *rep, FILE *out, bool use_color) {
    if (!rep || !out) return;
    const char *C_RESET = use_color ? "\033[0m" : "";
    const char *C_BOLD  = use_color ? "\033[1m" : "";
    const char *C_RED   = use_color ? "\033[31m" : "";
    const char *C_GRN   = use_color ? "\033[32m" : "";
    const char *C_YLW   = use_color ? "\033[33m" : "";
    const char *C_CYN   = use_color ? "\033[36m" : "";

    fprintf(out, "%s=== QTraceGuard QNX OS 8.0 Deterministic Trace Report ===%s\n", C_BOLD, C_RESET);
    fprintf(out, "Target     : %s %s (%s) on %s | CPUs: %u | Clock: %llu Hz\n",
            rep->header.sysname, rep->header.sys_release, rep->header.machine,
            rep->header.nodename, rep->header.cpu_num,
            (unsigned long long)rep->header.cycles_per_sec);
    fprintf(out, "Trace      : %llu events | Duration: %.3f ms | Procs: %zu | Threads: %zu | IPC Edges: %zu\n",
            (unsigned long long)rep->total_events,
            (double)rep->trace_duration_ns / 1e6,
            rep->proc_count, rep->thread_count, rep->edge_count);

    fprintf(out, "\n%s[1] Scheduling & IPC Latency Summary%s\n", C_CYN, C_RESET);
    fprintf(out, "  Dispatch (READY->RUNNING) : p50=%7.2f us | p95=%7.2f us | p99=%7.2f us | max=%7.2f us\n",
            (double)rep->dispatch_p50_ns / 1000.0, (double)rep->dispatch_p95_ns / 1000.0,
            (double)rep->dispatch_p99_ns / 1000.0, (double)rep->dispatch_max_ns / 1000.0);
    fprintf(out, "  IPC RTT  (Send->Reply)    : p50=%7.2f us | p95=%7.2f us | p99=%7.2f us | max=%7.2f us\n",
            (double)rep->ipc_rtt_p50_ns / 1000.0, (double)rep->ipc_rtt_p95_ns / 1000.0,
            (double)rep->ipc_rtt_p99_ns / 1000.0, (double)rep->ipc_rtt_max_ns / 1000.0);

    fprintf(out, "\n%s[2] IPC Wait-For Deadlock Cycles (%zu detected)%s\n", C_CYN, rep->cycle_count, C_RESET);
    if (rep->cycle_count == 0U) {
        fprintf(out, "  %sOK%s No synchronous MsgSend/MsgReply cycles detected.\n", C_GRN, C_RESET);
    } else {
        for (size_t i = 0; i < rep->cycle_count; i++) {
            const qtg_deadlock_cycle_t *c = &rep->cycles[i];
            uint64_t rel_ns = (c->detected_at_ns >= rep->trace_start_ns)
                              ? (c->detected_at_ns - rep->trace_start_ns) : 0ULL;
            fprintf(out, "  %sCYCLE #%zu%s (at %.3f ms): ", C_RED, i + 1U, C_RESET,
                    (double)rel_ns / 1e6);
            for (uint32_t k = 0; k < c->length; k++) {
                fprintf(out, "P%u:T%u(%s) --[ch:%u]--> ",
                        c->pids[k], c->tids[k], qtg_report_proc_name(rep, c->pids[k]), c->chids[k]);
            }
            fprintf(out, "P%u:T%u(%s)\n",
                    c->pids[0], c->tids[0], qtg_report_proc_name(rep, c->pids[0]));
        }
    }

    fprintf(out, "\n%s[3] Priority Inversions & QNX 8.0 Priority-Inheritance Diagnostics (%zu detected)%s\n",
            C_CYN, rep->inversion_count, C_RESET);
    if (rep->inversion_count == 0U) {
        fprintf(out, "  %sOK%s No priority inversions or PROCMGR_AID_PRIORITY ceiling caps detected.\n",
                C_GRN, C_RESET);
    } else {
        for (size_t i = 0; i < rep->inversion_count; i++) {
            const qtg_prio_inversion_t *inv = &rep->inversions[i];
            fprintf(out, "  %s[%s]%s High P%u:T%u(%s, prio %u) blocked on Server P%u:T%u(%s, eff_prio %u, ch:%u)",
                    C_YLW, qtg_inversion_kind_str(inv->kind), C_RESET,
                    inv->high_pid, inv->high_tid, qtg_report_proc_name(rep, inv->high_pid), inv->high_prio,
                    inv->server_pid, inv->server_tid, qtg_report_proc_name(rep, inv->server_pid),
                    inv->server_prio, inv->chid);
            if (inv->preemptor_pid != 0U) {
                fprintf(out, " preempted by P%u:T%u(%s, prio %u) for %.2f us",
                        inv->preemptor_pid, inv->preemptor_tid,
                        qtg_report_proc_name(rep, inv->preemptor_pid),
                        inv->preemptor_prio, (double)inv->duration_ns / 1000.0);
            }
            fprintf(out, "\n");
        }
    }

    if (rep->pulse_count > 0U) {
        fprintf(out, "\n%s[4] Pulse Queue Health (%zu channel(s))%s\n", C_CYN, rep->pulse_count, C_RESET);
        for (size_t i = 0; i < rep->pulse_count; i++) {
            const qtg_pulse_channel_stat_t *ps = &rep->pulses[i];
            fprintf(out, "  P%u(%s) ch:%u | sent=%llu recv=%llu unblock=%llu max_backlog=%llu pending=%llu [%s%s%s]\n",
                    ps->pid, qtg_report_proc_name(rep, ps->pid), ps->chid,
                    (unsigned long long)ps->sent_count, (unsigned long long)ps->received_count,
                    (unsigned long long)ps->unblock_pulses, (unsigned long long)ps->max_backlog,
                    (unsigned long long)ps->current_backlog,
                    ps->leak_detected ? C_RED : C_GRN,
                    ps->leak_detected ? "SATURATED" : "OK", C_RESET);
        }
    }

    if (rep->aps_count > 0U) {
        fprintf(out, "\n%s[5] QNX 7.1 APS -> QNX OS 8.0 SCHED_SPORADIC Migration Advisor%s\n", C_CYN, C_RESET);
        fprintf(out, "  Note: QNX OS 8.0 omits legacy [module=aps] (SchedCtl returns ENOSYS) in favor of\n");
        fprintf(out, "        SCHED_SPORADIC + per-thread runtime clocks for linear 2-64 core scalability.\n");
        for (size_t i = 0; i < rep->aps_count; i++) {
            const qtg_aps_migration_t *a = &rep->aps_migrations[i];
            fprintf(out, "  APS#%u '%s' (budget=%u%%, crit=%ums, win=%ums, bankruptcies=%u)\n",
                    a->aps_id, a->name, a->budget_percent, a->critical_budget_ms,
                    a->window_ms, a->bankruptcy_events);
            fprintf(out, "    -> QNX 8.0: SCHED_SPORADIC normal_prio=%u low_prio=%u repl_period=%ums init_budget=%ums max_repl=%u%s\n",
                    a->rec_normal_priority, a->rec_low_priority, a->rec_repl_period_ms,
                    a->rec_init_budget_ms, a->rec_max_repl,
                    a->requires_aid_priority ? " [REQUIRES PROCMGR_AID_PRIORITY]" : "");
        }
    }

    fprintf(out, "\n%s[SLA VERDICT]%s %s%s%s", C_BOLD, C_RESET,
            rep->sla.passed ? C_GRN : C_RED,
            rep->sla.passed ? "PASS" : "FAIL", C_RESET);
    if (!rep->sla.passed) {
        fprintf(out, " (%u violation(s))\n", rep->sla.violation_count);
        for (uint32_t i = 0; i < rep->sla.violation_count; i++) {
            fprintf(out, "  %s- %s%s\n", C_RED, rep->sla.violations[i], C_RESET);
        }
    } else {
        fprintf(out, " (all deterministic real-time gates satisfied)\n");
    }
}

static void qtg_json_escape(FILE *fp, const char *s) {
    fputc('"', fp);
    for (; s && *s; s++) {
        unsigned char uc = (unsigned char)*s;
        if (uc == '"' || uc == '\\') { fputc('\\', fp); fputc((int)uc, fp); }
        else if (uc == '\n') { fputs("\\n", fp); }
        else if (uc == '\r') { fputs("\\r", fp); }
        else if (uc == '\t') { fputs("\\t", fp); }
        else if (uc >= 0x20U && uc < 0x7FU) { fputc((int)uc, fp); }
        else { fprintf(fp, "\\u%04x", (unsigned int)uc); }
    }
    fputc('"', fp);
}

int qtg_report_write_json(const qtg_report_t *rep, const char *path) {
    if (!rep || !path) return -1;
    FILE *fp = (strcmp(path, "-") == 0) ? stdout : fopen(path, "w");
    if (!fp) return -1;

    fprintf(fp, "{\n");
    fprintf(fp, "  \"tool\": \"QTraceGuard\",\n");
    fprintf(fp, "  \"version\": \"1.0.0\",\n");
    fprintf(fp, "  \"header\": {\n");
    fprintf(fp, "    \"sysname\": "); qtg_json_escape(fp, rep->header.sysname); fprintf(fp, ",\n");
    fprintf(fp, "    \"sys_release\": "); qtg_json_escape(fp, rep->header.sys_release); fprintf(fp, ",\n");
    fprintf(fp, "    \"machine\": "); qtg_json_escape(fp, rep->header.machine); fprintf(fp, ",\n");
    fprintf(fp, "    \"nodename\": "); qtg_json_escape(fp, rep->header.nodename); fprintf(fp, ",\n");
    fprintf(fp, "    \"cpu_num\": %u,\n", rep->header.cpu_num);
    fprintf(fp, "    \"cycles_per_sec\": %llu,\n", (unsigned long long)rep->header.cycles_per_sec);
    fprintf(fp, "    \"little_endian\": %s\n", rep->header.little_endian ? "true" : "false");
    fprintf(fp, "  },\n");

    fprintf(fp, "  \"summary\": {\n");
    fprintf(fp, "    \"total_events\": %llu,\n", (unsigned long long)rep->total_events);
    fprintf(fp, "    \"dropped_buffer_events\": %llu,\n", (unsigned long long)rep->dropped_buffer_events);
    fprintf(fp, "    \"matched_focus_threads\": %u,\n", rep->matched_focus_threads);
    fprintf(fp, "    \"focus_proc\": "); qtg_json_escape(fp, rep->focus_proc); fprintf(fp, ",\n");
    fprintf(fp, "    \"trace_start_ns\": %llu,\n", (unsigned long long)rep->trace_start_ns);
    fprintf(fp, "    \"trace_duration_ns\": %llu,\n", (unsigned long long)rep->trace_duration_ns);
    fprintf(fp, "    \"dispatch_p50_ns\": %llu,\n", (unsigned long long)rep->dispatch_p50_ns);
    fprintf(fp, "    \"dispatch_p95_ns\": %llu,\n", (unsigned long long)rep->dispatch_p95_ns);
    fprintf(fp, "    \"dispatch_p99_ns\": %llu,\n", (unsigned long long)rep->dispatch_p99_ns);
    fprintf(fp, "    \"dispatch_max_ns\": %llu,\n", (unsigned long long)rep->dispatch_max_ns);
    fprintf(fp, "    \"ipc_rtt_p50_ns\": %llu,\n", (unsigned long long)rep->ipc_rtt_p50_ns);
    fprintf(fp, "    \"ipc_rtt_p95_ns\": %llu,\n", (unsigned long long)rep->ipc_rtt_p95_ns);
    fprintf(fp, "    \"ipc_rtt_p99_ns\": %llu,\n", (unsigned long long)rep->ipc_rtt_p99_ns);
    fprintf(fp, "    \"ipc_rtt_max_ns\": %llu\n", (unsigned long long)rep->ipc_rtt_max_ns);
    fprintf(fp, "  },\n");

    fprintf(fp, "  \"sla\": {\n");
    fprintf(fp, "    \"passed\": %s,\n", rep->sla.passed ? "true" : "false");
    fprintf(fp, "    \"violations\": [");
    for (uint32_t i = 0; i < rep->sla.violation_count; i++) {
        if (i > 0U) fprintf(fp, ", ");
        qtg_json_escape(fp, rep->sla.violations[i]);
    }
    fprintf(fp, "]\n  },\n");

    fprintf(fp, "  \"processes\": [\n");
    for (size_t i = 0; i < rep->proc_count; i++) {
        const qtg_proc_info_t *p = &rep->procs[i];
        fprintf(fp, "    {\"pid\": %u, \"name\": ", p->pid);
        qtg_json_escape(fp, p->name);
        fprintf(fp, ", \"has_aid_priority\": %s, \"prio_ceiling\": %u}%s\n",
                p->has_aid_priority ? "true" : "false", p->prio_ceiling,
                (i + 1U < rep->proc_count) ? "," : "");
    }
    fprintf(fp, "  ],\n");

    fprintf(fp, "  \"channels\": [\n");
    for (size_t i = 0; i < rep->channel_count; i++) {
        const qtg_channel_info_t *ch = &rep->channels[i];
        fprintf(fp, "    {\"chid\": %u, \"owner_pid\": %u, \"owner_tid\": %u, \"flags\": %u, \"fixed_priority\": %s, \"resmgr_path\": ",
                ch->chid, ch->owner_pid, ch->owner_tid, ch->flags,
                (ch->flags & QTG_CHF_FIXED_PRIORITY) ? "true" : "false");
        qtg_json_escape(fp, ch->resmgr_path);
        fprintf(fp, "}%s\n", (i + 1U < rep->channel_count) ? "," : "");
    }
    fprintf(fp, "  ],\n");

    fprintf(fp, "  \"threads\": [\n");
    for (size_t i = 0; i < rep->thread_count; i++) {
        const qtg_thread_stats_t *t = &rep->threads[i];
        fprintf(fp, "    {\"pid\": %u, \"tid\": %u, \"proc_name\": ", t->pid, t->tid);
        qtg_json_escape(fp, qtg_report_proc_name(rep, t->pid));
        fprintf(fp, ", \"name\": "); qtg_json_escape(fp, t->name);
        fprintf(fp, ", \"base_priority\": %u, \"effective_priority\": %u, \"dispatch_count\": %llu, "
                    "\"max_dispatch_ns\": %llu, \"ipc_rtt_count\": %llu, \"max_ipc_rtt_ns\": %llu, "
                    "\"total_running_ns\": %llu, \"sporadic_repl_count\": %u}%s\n",
                t->base_priority, t->effective_priority,
                (unsigned long long)t->dispatch_count, (unsigned long long)t->max_dispatch_ns,
                (unsigned long long)t->ipc_rtt_count, (unsigned long long)t->max_ipc_rtt_ns,
                (unsigned long long)t->total_running_ns, t->sporadic_repl_count,
                (i + 1U < rep->thread_count) ? "," : "");
    }
    fprintf(fp, "  ],\n");

    fprintf(fp, "  \"ipc_edges\": [\n");
    for (size_t i = 0; i < rep->edge_count; i++) {
        const qtg_wait_edge_t *e = &rep->edges[i];
        fprintf(fp, "    {\"from_pid\": %u, \"from_tid\": %u, \"to_pid\": %u, \"to_tid\": %u, "
                    "\"chid\": %u, \"active\": %s, \"msg_count\": %llu, \"max_rtt_ns\": %llu}%s\n",
                e->from_pid, e->from_tid, e->to_pid, e->to_tid, e->chid,
                e->active ? "true" : "false", (unsigned long long)e->msg_count,
                (unsigned long long)e->max_rtt_ns, (i + 1U < rep->edge_count) ? "," : "");
    }
    fprintf(fp, "  ],\n");

    fprintf(fp, "  \"deadlock_cycles\": [\n");
    for (size_t i = 0; i < rep->cycle_count; i++) {
        const qtg_deadlock_cycle_t *c = &rep->cycles[i];
        fprintf(fp, "    {\"detected_at_ns\": %llu, \"nodes\": [", (unsigned long long)c->detected_at_ns);
        for (uint32_t k = 0; k < c->length; k++) {
            fprintf(fp, "{\"pid\": %u, \"tid\": %u, \"chid\": %u, \"proc_name\": ",
                    c->pids[k], c->tids[k], c->chids[k]);
            qtg_json_escape(fp, qtg_report_proc_name(rep, c->pids[k]));
            fprintf(fp, "}%s", (k + 1U < c->length) ? ", " : "");
        }
        fprintf(fp, "]}%s\n", (i + 1U < rep->cycle_count) ? "," : "");
    }
    fprintf(fp, "  ],\n");

    fprintf(fp, "  \"priority_inversions\": [\n");
    for (size_t i = 0; i < rep->inversion_count; i++) {
        const qtg_prio_inversion_t *inv = &rep->inversions[i];
        fprintf(fp, "    {\"kind\": "); qtg_json_escape(fp, qtg_inversion_kind_str(inv->kind));
        fprintf(fp, ", \"high_pid\": %u, \"high_tid\": %u, \"high_prio\": %u, "
                    "\"server_pid\": %u, \"server_tid\": %u, \"server_prio\": %u, "
                    "\"preemptor_pid\": %u, \"preemptor_tid\": %u, \"preemptor_prio\": %u, "
                    "\"chid\": %u, \"start_ns\": %llu, \"duration_ns\": %llu}%s\n",
                inv->high_pid, inv->high_tid, inv->high_prio,
                inv->server_pid, inv->server_tid, inv->server_prio,
                inv->preemptor_pid, inv->preemptor_tid, inv->preemptor_prio,
                inv->chid, (unsigned long long)inv->start_ns, (unsigned long long)inv->duration_ns,
                (i + 1U < rep->inversion_count) ? "," : "");
    }
    fprintf(fp, "  ],\n");

    fprintf(fp, "  \"pulse_channels\": [\n");
    for (size_t i = 0; i < rep->pulse_count; i++) {
        const qtg_pulse_channel_stat_t *ps = &rep->pulses[i];
        fprintf(fp, "    {\"pid\": %u, \"chid\": %u, \"sent\": %llu, \"received\": %llu, "
                    "\"unblock_pulses\": %llu, \"max_backlog\": %llu, \"pending\": %llu, \"leak_detected\": %s}%s\n",
                ps->pid, ps->chid, (unsigned long long)ps->sent_count, (unsigned long long)ps->received_count,
                (unsigned long long)ps->unblock_pulses, (unsigned long long)ps->max_backlog,
                (unsigned long long)ps->current_backlog, ps->leak_detected ? "true" : "false",
                (i + 1U < rep->pulse_count) ? "," : "");
    }
    fprintf(fp, "  ],\n");

    fprintf(fp, "  \"aps_migrations\": [\n");
    for (size_t i = 0; i < rep->aps_count; i++) {
        const qtg_aps_migration_t *a = &rep->aps_migrations[i];
        fprintf(fp, "    {\"aps_id\": %u, \"name\": ", a->aps_id);
        qtg_json_escape(fp, a->name);
        fprintf(fp, ", \"budget_percent\": %u, \"critical_budget_ms\": %u, \"window_ms\": %u, "
                    "\"bankruptcy_events\": %u, \"qnx8_sporadic\": {\"repl_period_ms\": %u, "
                    "\"init_budget_ms\": %u, \"normal_priority\": %u, \"low_priority\": %u, "
                    "\"max_repl\": %u, \"requires_aid_priority\": %s}}%s\n",
                a->budget_percent, a->critical_budget_ms, a->window_ms, a->bankruptcy_events,
                a->rec_repl_period_ms, a->rec_init_budget_ms, a->rec_normal_priority,
                a->rec_low_priority, a->rec_max_repl, a->requires_aid_priority ? "true" : "false",
                (i + 1U < rep->aps_count) ? "," : "");
    }
    fprintf(fp, "  ],\n");

    fprintf(fp, "  \"timeline_slices\": [\n");
    size_t max_s = (rep->slice_count < 512U) ? rep->slice_count : 512U;
    for (size_t i = 0; i < max_s; i++) {
        const qtg_timeline_slice_t *s = &rep->slices[i];
        fprintf(fp, "    {\"cpu\": %u, \"pid\": %u, \"tid\": %u, \"prio\": %u, \"start_ns\": %llu, \"end_ns\": %llu}%s\n",
                s->cpu, s->pid, s->tid, s->priority,
                (unsigned long long)(s->start_ns >= rep->trace_start_ns ? s->start_ns - rep->trace_start_ns : 0ULL),
                (unsigned long long)(s->end_ns >= rep->trace_start_ns ? s->end_ns - rep->trace_start_ns : 0ULL),
                (i + 1U < max_s) ? "," : "");
    }
    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");

    if (fp != stdout) fclose(fp);
    return 0;
}

int qtg_report_write_sarif(const qtg_report_t *rep, const char *trace_path, const char *path) {
    if (!rep || !path) return -1;
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    fprintf(fp, "{\n");
    fprintf(fp, "  \"version\": \"2.1.0\",\n");
    fprintf(fp, "  \"$schema\": \"https://json.schemastore.org/sarif-2.1.0.json\",\n");
    fprintf(fp, "  \"runs\": [{\n");
    fprintf(fp, "    \"tool\": {\"driver\": {\"name\": \"QTraceGuard\", \"version\": \"1.0.0\", \"rules\": [\n");
    fprintf(fp, "      {\"id\": \"QTG001\", \"shortDescription\": {\"text\": \"Synchronous QNX IPC Wait-For Deadlock Cycle\"}},\n");
    fprintf(fp, "      {\"id\": \"QTG002\", \"shortDescription\": {\"text\": \"Priority Inversion or QNX 8.0 PROCMGR_AID_PRIORITY Ceiling Cap\"}},\n");
    fprintf(fp, "      {\"id\": \"QTG003\", \"shortDescription\": {\"text\": \"QNX Resource Manager Pulse Queue Saturation\"}},\n");
    fprintf(fp, "      {\"id\": \"QTG004\", \"shortDescription\": {\"text\": \"Real-Time SLA Latency or Legacy APS Migration Violation\"}}\n");
    fprintf(fp, "    ]}},\n");
    fprintf(fp, "    \"results\": [\n");

    bool first = true;
    for (size_t i = 0; i < rep->cycle_count; i++) {
        if (!first) fprintf(fp, ",\n");
        first = false;
        char msg[256];
        snprintf(msg, sizeof(msg), "Cyclic MsgSend/MsgReply deadlock of length %u starting at P%u:T%u (%s)",
                 rep->cycles[i].length, rep->cycles[i].pids[0], rep->cycles[i].tids[0],
                 qtg_report_proc_name(rep, rep->cycles[i].pids[0]));
        fprintf(fp, "      {\"ruleId\": \"QTG001\", \"level\": \"error\", \"message\": {\"text\": ");
        qtg_json_escape(fp, msg);
        fprintf(fp, "}, \"locations\": [{\"physicalLocation\": {\"artifactLocation\": {\"uri\": ");
        qtg_json_escape(fp, trace_path ? trace_path : "tracebuffer.kev");
        fprintf(fp, "}}}]}");
    }
    for (size_t i = 0; i < rep->inversion_count; i++) {
        if (!first) fprintf(fp, ",\n");
        first = false;
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: high-prio P%u(%u) blocked on P%u(%u) ch:%u for %.2f us",
                 qtg_inversion_kind_str(rep->inversions[i].kind),
                 rep->inversions[i].high_pid, rep->inversions[i].high_prio,
                 rep->inversions[i].server_pid, rep->inversions[i].server_prio,
                 rep->inversions[i].chid, (double)rep->inversions[i].duration_ns / 1000.0);
        fprintf(fp, "      {\"ruleId\": \"QTG002\", \"level\": \"error\", \"message\": {\"text\": ");
        qtg_json_escape(fp, msg);
        fprintf(fp, "}, \"locations\": [{\"physicalLocation\": {\"artifactLocation\": {\"uri\": ");
        qtg_json_escape(fp, trace_path ? trace_path : "tracebuffer.kev");
        fprintf(fp, "}}}]}");
    }
    for (size_t i = 0; i < rep->pulse_count; i++) {
        if (!rep->pulses[i].leak_detected) continue;
        if (!first) fprintf(fp, ",\n");
        first = false;
        char msg[256];
        snprintf(msg, sizeof(msg), "Pulse queue saturation on P%u (%s) ch:%u max_backlog=%llu",
                 rep->pulses[i].pid, qtg_report_proc_name(rep, rep->pulses[i].pid),
                 rep->pulses[i].chid, (unsigned long long)rep->pulses[i].max_backlog);
        fprintf(fp, "      {\"ruleId\": \"QTG003\", \"level\": \"warning\", \"message\": {\"text\": ");
        qtg_json_escape(fp, msg);
        fprintf(fp, "}, \"locations\": [{\"physicalLocation\": {\"artifactLocation\": {\"uri\": ");
        qtg_json_escape(fp, trace_path ? trace_path : "tracebuffer.kev");
        fprintf(fp, "}}}]}");
    }
    if (!rep->sla.passed) {
        for (uint32_t i = 0; i < rep->sla.violation_count; i++) {
            const char *v = rep->sla.violations[i];
            /* Avoid duplicating deadlock/inversion/pulse items already emitted under QTG001..QTG003 */
            if (rep->cycle_count > 0U && strncmp(v, "IPC DEADLOCK:", 13) == 0) continue;
            if (rep->inversion_count > 0U &&
                (strncmp(v, "PRIORITY INVERSION:", 19) == 0 ||
                 strncmp(v, "_NTO_CHF_FIXED_PRIORITY", 23) == 0 ||
                 strncmp(v, "QNX 8.0 PROCMGR_AID_PRIORITY", 28) == 0)) continue;
            if (strncmp(v, "PULSE QUEUE SATURATION:", 23) == 0) continue;
            if (!first) fprintf(fp, ",\n");
            first = false;
            fprintf(fp, "      {\"ruleId\": \"QTG004\", \"level\": \"error\", \"message\": {\"text\": ");
            qtg_json_escape(fp, v);
            fprintf(fp, "}, \"locations\": [{\"physicalLocation\": {\"artifactLocation\": {\"uri\": ");
            qtg_json_escape(fp, trace_path ? trace_path : "tracebuffer.kev");
            fprintf(fp, "}}}]}");
        }
    }
    fprintf(fp, "\n    ]\n  }]}\n");
    fclose(fp);
    return 0;
}
