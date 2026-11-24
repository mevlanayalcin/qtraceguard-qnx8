/*
 * QTraceGuard - Host CLI (`qtraceguard`)
 * SPDX-License-Identifier: MIT
 *
 * Subcommands:
 *   analyze <trace.kev|trace.txt> [--policy sla.json] [--focus proc] [--json out.json] [--sarif out.sarif] [--no-color]
 *   check-sla <trace.kev|trace.txt> [--policy sla.json] [--focus proc] [--json out.json] [--sarif out.sarif] [--no-color]
 *   inspect <trace.kev|trace.txt> [--limit N]
 *   generate-demo <scenario> <out.kev> [--iters N]
 *   migrate-aps <legacy_71.kev> [--json out.json]
 *   diff <baseline.kev> <candidate.kev> [--focus proc]
 */

#include "qtg_analyzer.h"
#include "qtg_kev.h"
#include "qtg_resmgr_demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool str_ends_with(const char *s, const char *suf) {
    size_t sl = strlen(s), sufl = strlen(suf);
    return (sl >= sufl && strcmp(s + sl - sufl, suf) == 0);
}

static int load_trace_auto(const char *path, qtg_trace_t *tr, char *err, size_t errlen) {
    if (str_ends_with(path, ".txt") || str_ends_with(path, ".log")) {
        return qtg_traceprinter_read_file(path, tr, err, errlen);
    }
    int rc = qtg_kev_read_file(path, tr, err, errlen);
    if (rc != 0 && (!err || strstr(err, "truncated syspage") == NULL)) {
        /* Fallback attempt as traceprinter plain-text (never mask truncated binary .kev) */
        char tperr[256] = {0};
        if (qtg_traceprinter_read_file(path, tr, tperr, sizeof(tperr)) == 0 && tr->count > 0U) {
            return 0;
        }
    }
    return rc;
}

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "QTraceGuard v1.0.0 - QNX OS 8.0 Deterministic IPC Deadlock, Priority-Inversion & SLA Verifier\n\n"
        "Usage:\n"
        "  %s analyze <trace.kev|trace.txt> [--policy sla.json] [--focus proc] [--json out.json] [--sarif out.sarif] [--no-color]\n"
        "  %s check-sla <trace.kev|trace.txt> [--policy sla.json] [--focus proc] [--json out.json] [--sarif out.sarif] [--no-color]\n"
        "  %s inspect <trace.kev|trace.txt> [--limit N]\n"
        "  %s generate-demo <healthy|deadlock|prio-inversion|aid-cap|pulse-storm|aps-legacy> <out.kev> [--iters N]\n"
        "  %s migrate-aps <legacy_71.kev> [--json out.json]\n"
        "  %s diff <baseline.kev> <candidate.kev> [--focus proc]\n",
        argv0, argv0, argv0, argv0, argv0, argv0);
}

static int cmd_analyze_or_check(int argc, char **argv, bool enforce_sla) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }
    const char *trace_path  = argv[2];
    const char *policy_path = NULL;
    const char *focus_proc  = NULL;
    const char *json_path   = NULL;
    const char *sarif_path  = NULL;
    bool use_color = isatty(STDOUT_FILENO) != 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc) policy_path = argv[++i];
        else if (strcmp(argv[i], "--focus") == 0 && i + 1 < argc) focus_proc = argv[++i];
        else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) json_path = argv[++i];
        else if (strcmp(argv[i], "--sarif") == 0 && i + 1 < argc) sarif_path = argv[++i];
        else if (strcmp(argv[i], "--no-color") == 0) use_color = false;
    }

    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[256] = {0};
    if (load_trace_auto(trace_path, &tr, err, sizeof(err)) != 0) {
        fprintf(stderr, "Error reading trace '%s': %s\n", trace_path, err);
        qtg_trace_free(&tr);
        return 1;
    }

    qtg_sla_policy_t pol;
    qtg_sla_policy_defaults(&pol);
    if (policy_path && qtg_sla_policy_load_json(policy_path, &pol, err, sizeof(err)) != 0) {
        fprintf(stderr, "Error reading policy '%s': %s\n", policy_path, err);
        qtg_trace_free(&tr);
        return 1;
    }
    if (focus_proc) {
        snprintf(pol.focus_proc, sizeof(pol.focus_proc), "%s", focus_proc);
    }

    qtg_report_t *rep = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
    if (!rep) { qtg_trace_free(&tr); return 1; }
    qtg_analyze_trace(&tr, &pol, rep);

    if (!json_path || strcmp(json_path, "-") != 0) {
        qtg_report_print_text(rep, stdout, use_color);
    }
    if (json_path && qtg_report_write_json(rep, json_path) != 0) {
        fprintf(stderr, "Error writing JSON report to '%s'\n", json_path);
        free(rep);
        qtg_trace_free(&tr);
        return 1;
    }
    if (sarif_path && qtg_report_write_sarif(rep, trace_path, sarif_path) != 0) {
        fprintf(stderr, "Error writing SARIF report to '%s'\n", sarif_path);
        free(rep);
        qtg_trace_free(&tr);
        return 1;
    }

    int rc = (enforce_sla && !rep->sla.passed) ? 2 : 0;
    free(rep);
    qtg_trace_free(&tr);
    return rc;
}

static int cmd_inspect(int argc, char **argv) {
    if (argc < 3) { print_usage(argv[0]); return 1; }
    const char *path = argv[2];
    size_t limit = 40U;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = (size_t)strtoul(argv[++i], NULL, 10);
        }
    }
    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[256] = {0};
    if (load_trace_auto(path, &tr, err, sizeof(err)) != 0) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }
    printf("-- QTraceGuard KEV Header --\n");
    printf("TRACE_SYSNAME        :: %s %s (%s)\n", tr.header.sysname, tr.header.sys_release, tr.header.machine);
    printf("TRACE_CPU_NUM        :: %u\n", tr.header.cpu_num);
    printf("TRACE_CYCLES_PER_SEC :: %llu\n", (unsigned long long)tr.header.cycles_per_sec);
    printf("TRACE_LITTLE_ENDIAN  :: %s\n", tr.header.little_endian ? "TRUE" : "FALSE");
    printf("RAW_SLOTS / EVENTS   :: %llu slots -> %zu decoded (%llu combine)\n\n",
           (unsigned long long)tr.raw_slots_read, tr.count,
           (unsigned long long)tr.combine_events_reassembled);

    size_t n = (tr.count < limit) ? tr.count : limit;
    for (size_t i = 0; i < n; i++) {
        const qtg_event_t *ev = &tr.events[i];
        printf("[%4zu] t=%9.3f us CPU:%02u %-7s : %-20s (%s, %u words) w0=%u w1=%u",
               i, (double)ev->timestamp_ns / 1000.0, ev->cpu,
               qtg_class_name(ev->event_class), qtg_event_name(ev->event_class, ev->event_id),
               ev->is_combine ? "COMB" : "SIMP", ev->word_count,
               ev->word_count > 0 ? ev->words[0] : 0U,
               ev->word_count > 1 ? ev->words[1] : 0U);
        if (ev->word_count > 2U) printf(" w2=%u", ev->words[2]);
        printf("\n");
    }
    if (tr.count > n) printf("... (%zu more events omitted; use --limit N)\n", tr.count - n);
    qtg_trace_free(&tr);
    return 0;
}

static int cmd_generate_demo(int argc, char **argv) {
    if (argc < 4) { print_usage(argv[0]); return 1; }
    qtg_scenario_t scen;
    if (qtg_parse_scenario_name(argv[2], &scen) != 0) {
        fprintf(stderr, "Unknown scenario '%s'\n", argv[2]);
        return 1;
    }
    const char *out_kev = argv[3];
    uint32_t iters = 12U;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = (uint32_t)strtoul(argv[++i], NULL, 10);
        }
    }
    if (qtg_run_demo_scenario(scen, out_kev, iters) != 0) {
        fprintf(stderr, "Failed generating demo scenario '%s'\n", argv[2]);
        return 1;
    }
    printf("Generated QNX-compatible binary trace '%s' (scenario=%s, iters=%u)\n",
           out_kev, argv[2], iters);
    return 0;
}

static int cmd_migrate_aps(int argc, char **argv) {
    if (argc < 3) { print_usage(argv[0]); return 1; }
    const char *trace_path = argv[2];
    const char *json_path  = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) json_path = argv[++i];
    }
    qtg_trace_t tr;
    qtg_trace_init(&tr);
    char err[256] = {0};
    if (load_trace_auto(trace_path, &tr, err, sizeof(err)) != 0) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }
    qtg_report_t *rep = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
    if (!rep) { qtg_trace_free(&tr); return 1; }
    qtg_analyze_trace(&tr, NULL, rep);

    printf("=== QNX 7.1 APS -> QNX OS 8.0 SCHED_SPORADIC Migration Code Generator ===\n");
    if (rep->aps_count == 0U) {
        printf("No legacy APS partition events found in '%s'.\n", trace_path);
    } else {
        for (size_t i = 0; i < rep->aps_count; i++) {
            const qtg_aps_migration_t *a = &rep->aps_migrations[i];
            printf("\n/* Partition #%u: '%s' (legacy APS budget=%u%%, crit=%ums, win=%ums, bankruptcies=%u) */\n",
                   a->aps_id, a->name, a->budget_percent, a->critical_budget_ms,
                   a->window_ms, a->bankruptcy_events);
            if (a->requires_aid_priority) {
                printf("/* Grant PROCMGR_AID_PRIORITY BEFORE calling pthread_setschedparam for prio %u > 63: */\n",
                       a->rec_normal_priority);
                printf("procmgr_ability(0, PROCMGR_ADN_NONROOT|PROCMGR_AOP_ALLOW|PROCMGR_AID_PRIORITY, PROCMGR_AID_EOL);\n");
            }
            printf("struct sched_param sp_%u = {0};\n", a->aps_id);
            printf("sp_%u.sched_priority              = %u;\n", a->aps_id, a->rec_normal_priority);
            printf("sp_%u.sched_ss_low_priority       = %u;\n", a->aps_id, a->rec_low_priority);
            printf("sp_%u.sched_ss_repl_period.tv_sec  = %u;\n", a->aps_id, a->rec_repl_period_ms / 1000U);
            printf("sp_%u.sched_ss_repl_period.tv_nsec = %uL * 1000000L;\n", a->aps_id, a->rec_repl_period_ms % 1000U);
            printf("sp_%u.sched_ss_init_budget.tv_sec  = %u;\n", a->aps_id, a->rec_init_budget_ms / 1000U);
            printf("sp_%u.sched_ss_init_budget.tv_nsec = %uL * 1000000L;\n", a->aps_id, a->rec_init_budget_ms % 1000U);
            printf("sp_%u.sched_ss_max_repl           = %u;\n", a->aps_id, a->rec_max_repl);
            printf("pthread_setschedparam(tid_%u, SCHED_SPORADIC, &sp_%u);\n", a->aps_id, a->aps_id);
        }
    }
    if (json_path) qtg_report_write_json(rep, json_path);
    free(rep);
    qtg_trace_free(&tr);
    return 0;
}

static int cmd_diff(int argc, char **argv) {
    if (argc < 4) { print_usage(argv[0]); return 1; }
    qtg_trace_t t1, t2;
    qtg_trace_init(&t1);
    qtg_trace_init(&t2);
    char err[256] = {0};
    if (load_trace_auto(argv[2], &t1, err, sizeof(err)) != 0) {
        fprintf(stderr, "Baseline error: %s\n", err);
        return 1;
    }
    if (load_trace_auto(argv[3], &t2, err, sizeof(err)) != 0) {
        fprintf(stderr, "Candidate error: %s\n", err);
        qtg_trace_free(&t1);
        return 1;
    }
    qtg_sla_policy_t pol;
    qtg_sla_policy_defaults(&pol);
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--focus") == 0 && i + 1 < argc) {
            snprintf(pol.focus_proc, sizeof(pol.focus_proc), "%s", argv[++i]);
        }
    }
    qtg_report_t *r1 = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
    qtg_report_t *r2 = (qtg_report_t *)calloc(1U, sizeof(qtg_report_t));
    if (!r1 || !r2) {
        free(r1); free(r2);
        qtg_trace_free(&t1); qtg_trace_free(&t2);
        fprintf(stderr, "Out of memory allocating reports\n");
        return 1;
    }
    qtg_analyze_trace(&t1, &pol, r1);
    qtg_analyze_trace(&t2, &pol, r2);

    printf("=== QTraceGuard CI Trace Regression Diff ===\n");
    printf("Baseline  : %s\nCandidate : %s\n\n", argv[2], argv[3]);
    printf("Metric                  | Baseline       | Candidate      | Delta\n");
    printf("------------------------+----------------+----------------+----------------\n");
    printf("Deadlock Cycles         | %14zu | %14zu | %+ld\n",
           r1->cycle_count, r2->cycle_count, (long)r2->cycle_count - (long)r1->cycle_count);
    printf("Priority Inversions     | %14zu | %14zu | %+ld\n",
           r1->inversion_count, r2->inversion_count, (long)r2->inversion_count - (long)r1->inversion_count);
    printf("Dispatch p99 (us)       | %14.2f | %14.2f | %+.2f us\n",
           (double)r1->dispatch_p99_ns / 1000.0, (double)r2->dispatch_p99_ns / 1000.0,
           ((double)r2->dispatch_p99_ns - (double)r1->dispatch_p99_ns) / 1000.0);
    printf("IPC RTT max (us)        | %14.2f | %14.2f | %+.2f us\n",
           (double)r1->ipc_rtt_max_ns / 1000.0, (double)r2->ipc_rtt_max_ns / 1000.0,
           ((double)r2->ipc_rtt_max_ns - (double)r1->ipc_rtt_max_ns) / 1000.0);
    printf("SLA Verdict             | %14s | %14s | %s\n",
           r1->sla.passed ? "PASS" : "FAIL", r2->sla.passed ? "PASS" : "FAIL",
           (!r2->sla.passed && r1->sla.passed) ? "REGRESSION!" : "OK");

    bool regressed = (!r2->sla.passed) || (r2->cycle_count > r1->cycle_count) ||
                     (r2->inversion_count > r1->inversion_count);
    free(r1); free(r2);
    qtg_trace_free(&t1); qtg_trace_free(&t2);
    return regressed ? 2 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }
    if (strcmp(argv[1], "analyze") == 0)       return cmd_analyze_or_check(argc, argv, false);
    if (strcmp(argv[1], "check-sla") == 0)     return cmd_analyze_or_check(argc, argv, true);
    if (strcmp(argv[1], "inspect") == 0)       return cmd_inspect(argc, argv);
    if (strcmp(argv[1], "generate-demo") == 0) return cmd_generate_demo(argc, argv);
    if (strcmp(argv[1], "migrate-aps") == 0)   return cmd_migrate_aps(argc, argv);
    if (strcmp(argv[1], "diff") == 0)          return cmd_diff(argc, argv);
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    fprintf(stderr, "Unknown subcommand: %s\n", argv[1]);
    print_usage(argv[0]);
    return 1;
}
