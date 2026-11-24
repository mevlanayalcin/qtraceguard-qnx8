# Module 5: Lab — Why APS Was Removed in QNX OS 8.0 & Migrating to `SCHED_SPORADIC`

**Estimated Time:** 8 minutes  
**Objective:** Understand QNX OS 8.0's architectural shift away from the legacy Adaptive Partitioning Scheduler (APS), verify live `SchedCtl()` `ENOSYS` behavior with `qtg-probe`, and automatically generate QNX OS 8.0 `SCHED_SPORADIC` migration code from legacy QNX 7.1 APS `.kev` traces.

---

## 5.1 What Changed Between QNX 7.1 and QNX OS 8.0?

In QNX OS 7.1 and earlier, teams often added `[module=aps]` to their boot buildfile and invoked `SchedCtl(SCHED_APS_CREATE_PARTITION, ...)` to cap rogue threads using percentage-based CPU budgets.

**In QNX OS 8.0, BlackBerry QNX omitted the legacy APS module (`<sys/sched_aps.h>` is no longer shipped in QNX SDP 8.0):**
- QNX OS 8.0 re-engineered its microkernel scheduler around scalable per-core scheduling across multi-core automotive/robotics SoCs.
- Calling `SchedCtl()` for legacy APS queries on QNX OS 8.0 returns **`-1` with `errno = 89` (`ENOSYS` — Function not implemented)**.
- **Official QNX OS 8.0 Replacements:**
  1. **Sporadic Scheduling (`SCHED_SPORADIC`)** — gives a real-time thread a deterministic execution budget (`sched_ss_init_budget`) per replenishment period (`sched_ss_repl_period`) at high priority (`sched_priority`), automatically dropping it to `sched_ss_low_priority` if it exhausts its budget.
  2. **Per-Thread CPU Runtime Clocks** (`pthread_getcpuclockid()` / `CLOCK_THREAD_CPUTIME_ID`) for runtime monitoring and watchdog enforcement.
  3. **CPU Affinity / Runmasks** (`ThreadCtl(_NTO_TCTL_RUNMASK, ...)`) for spatial core isolation across SMP cores.

*(Important architectural distinction: legacy APS guaranteed a minimum CPU share to a **group** of threads, whereas `SCHED_SPORADIC` bounds execution of **individual threads**. Combine `SCHED_SPORADIC` with core runmasks for group isolation.)*

---

## 5.2 Probing `SchedCtl()` Live at Runtime (`qtg-probe`)

Always verify scheduler support cleanly at startup rather than hardcoding legacy APS assumptions:

```bash
./build/qtg-probe --json
```

Verified live output captured on QNX OS 8.0 KVM (`benchmarks/probe_4cpu.json`, calling `SchedCtl(0, ...)` directly against `procnto-smp-instr`):
```json
{
  "tool": "qtg-probe",
  "qnx_native": true,
  "kernel_release": "QNX 8.0.0 (x86pc)",
  "aps_supported": false,
  "aps_errno": 89,
  "sporadic_supported": true,
  "has_aid_priority": true,
  "max_unpriv_priority": 63,
  "ipc_bench": {"iters": 1000, "succeeded": 1000, "p50_ns": 2480, "p99_ns": 205422, "max_ns": 3785810}
}
```

Inspect `src/target/qtg_rtos_shim.c` (`qtg_shim_probe_runtime`) to see how `QTraceGuard` invokes `SchedCtl()` live, confirms `sched_get_priority_max(SCHED_SPORADIC) > 0`, and empirically tests `PROCMGR_AID_PRIORITY` at priority `64`.

---

## 5.3 Automatically Migrating Legacy QNX 7.1 APS Traces to QNX OS 8.0

Generate a trace containing legacy QNX 7.1 APS partition definitions (`APS_NAME`, `APS_BUDGETS`) and an APS bankruptcy event (`APS_BANKRUPTCY`), then run `migrate-aps` and compiler-verify the generated QNX 8.0 C patch:

```bash
./build/qtraceguard generate-demo aps-legacy out/lab5_aps71.kev --iters 6
./build/qtraceguard migrate-aps out/lab5_aps71.kev --json out/lab5_aps71.json
python3 tools/qtg_ai_patch.py out/lab5_aps71.json --out-md out/lab5_aps71_patch.md --verify-cc
python3 tools/qtg_report.py out/lab5_aps71.json out/lab5_aps71.html
```

Observe the generated C11 QNX OS 8.0 migration code (note `procmgr_ability()` is placed **BEFORE** `pthread_setschedparam()` so priority `72 > 63` succeeds cleanly, and `tv_sec`/`tv_nsec` are normalized for windows `>= 1000 ms`):
```c
/* Partition #1: 'ADAS_Control' (legacy APS budget=50%, crit=15ms, win=100ms, bankruptcies=1) */
/* Grant PROCMGR_AID_PRIORITY BEFORE calling pthread_setschedparam for prio 72 > 63: */
procmgr_ability(0, PROCMGR_ADN_NONROOT|PROCMGR_AOP_ALLOW|PROCMGR_AID_PRIORITY, PROCMGR_AID_EOL);
struct sched_param sp_1 = {0};
sp_1.sched_priority              = 72;
sp_1.sched_ss_low_priority       = 10;
sp_1.sched_ss_repl_period.tv_sec  = 0;
sp_1.sched_ss_repl_period.tv_nsec = 100L * 1000000L;
sp_1.sched_ss_init_budget.tv_sec  = 0;
sp_1.sched_ss_init_budget.tv_nsec = 60L * 1000000L;
sp_1.sched_ss_max_repl           = 4;
pthread_setschedparam(tid_1, SCHED_SPORADIC, &sp_1);
```

Notice two automatic safeguards built into `QTraceGuard`:
1. Because `ADAS_Control` suffered `1` bankruptcy at `50%` (`50 ms / 100 ms`), `QTraceGuard` automatically adds a **+20% deterministic headroom margin** (`60 ms / 100 ms`, clamped safely to `<= repl_period`).
2. Because `ADAS_Control` had a critical budget (`15 ms`) and was assigned normal priority `72` (`> 63`), `QTraceGuard` emits `procmgr_ability(..., PROCMGR_AID_PRIORITY, ...)` **prior to** `pthread_setschedparam()` and verifies compilation with `qcc -Vgcc_ntox86_64 -Werror`!

---

**Next:** [Module 6: Automating Real-Time SLA Gates, AI Patch Copilot, Fuzzing & SARIF in CI →](06-ci-sla-gates.md)
