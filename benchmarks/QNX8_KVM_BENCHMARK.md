# QTraceGuard — Live QNX OS 8.0 KVM Verification & SMP Telemetry Report

All telemetry below was collected for **QTraceGuard host verifier calibration and real-time SLA gate validation** (not a competitive OS benchmark) on a live instrumented **QNX OS 8.0 kernel** (`QNX noname 8.0.0 2026/10/30-09:10:05EDT x86pc`, `procnto-smp-instr` kernel `2.8.0`) running under **QEMU/KVM (AMD-V / EPYC-Rome, 2.80 GHz TSC)** with binaries cross-compiled via QNX SDP 8.0 (`qcc -Vgcc_ntox86_64` and `-Vgcc_ntoaarch64le`, `-Wall -Wextra -Werror`).

## 1. Multi-Process Live QNX OS 8.0 Verification (5 Bundled `.kev` Captures, 4 vCPU KVM)

All five traces in [samples/qnx8_live/](../samples/qnx8_live/) were captured with `tracelogger -w -s 1 -n 48` (`0` dropped buffers) against our multi-process workload (`fork()`+`execv()` distinct PIDs `qtg_sensor_rm`, `qtg_planner_rm`, `qtg_actuator_rm`, `qtg_telemetry_bg`, plus `/dev/qtg_sensor` `resmgr_attach`):

| Scenario | File Size | Decoded Events | `check-sla --focus qtg` Exit | Verified Real-Time Kernel Finding |
| :--- | ---: | ---: | :---: | :--- |
| `qnx8_healthy.kev` | `542 KB` | `14,877` | **`0` (PASS)** | 3 distinct processes + `/dev/qtg_sensor` `iofunc_read`; Dispatch `p50=7.60 µs`, `p99=111.30 µs`, `max=172.03 µs`; IPC `p50=13.41 µs`, `max=1237.00 µs`; `0` deadlocks, `0` inversions |
| `qnx8_deadlock.kev` | `461 KB` | `12,650` | **`2` (FAIL)** | Exact 3-PID cycle caught at `+101.454 ms`: `P4055062(qtg_sensor_rm) -> P4063256(qtg_planner_rm) -> P4059159(qtg_actuator_rm) -> P4055062` |
| `qnx8_prio-inversion.kev` | `700 KB` | `22,525` | **`2` (FAIL)** | **12/12** `_NTO_CHF_FIXED_PRIORITY` inversions (`max window 431.39 µs`, `qtg_telemetry_bg` prio `30` preempting `qtg_actuator_rm` `eff_prio 12`) |
| `qnx8_aid-cap.kev` | `575 KB` | `15,317` | **`2` (FAIL)** | **12/12** live `PROCMGR_AID_PRIORITY` ceiling-63 caps: non-root `setuid(1000)` `qtg_actuator_rm` capped at `eff_prio 63` by `procnto-smp-instr` when serving `prio 120` `qtg_planner_rm` |
| `qnx8_pulse-storm.kev` | `360 KB` | `9,815` | **`2` (FAIL)** | Real kernel pulse queue saturation on `qtg_actuator_rm`: `sent=40, recv=4, max_backlog=40, pending=36 [SATURATED]` |

---

## 2. QTraceGuard SLA Gate Behavior Across 1 / 2 / 4 vCPU KVM Topologies

Each single-capture diagnostic run booted `procnto-smp-instr` with `-smp N -enable-kvm -cpu host`, ran `qtg-probe --json --iters 1000` (1,000 synchronous `MsgSend`/`MsgReply` round-trips) and `tracelogger -w -s 1 -n 16` around a single-process 24-iteration healthy workload, then evaluated the trace with `qtraceguard check-sla --focus qtg --policy qemu/sla_policy.json`:

| vCPUs | Kernel & Clock | `qtg-probe` `MsgSend` p50 / p99 (`1,000` iters) | Captured `.kev` Events (Threads) | Dispatch p50 / p99 / max (`READY→RUNNING`) | IPC RTT p50 / p99 (`Send→Reply`) | Deadlocks / Inv. | SLA Gate |
| ---: | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **1** | QNX 8.0.0 (`2.80 GHz`) | `3.46 µs` / `4.40 µs` | `6,254` (`98` thr) | `1.42` / **`158.28`** / **`1185.38 µs`** | `23.31` / `80.70 µs` | `0` / `0` | **FAIL** (single-CPU contention) |
| **2** | QNX 8.0.0 (`2.80 GHz`) | `2.48 µs` / `15.09 µs` | `6,910` (`112` thr) | `2.03` / **`106.14`** / **`144.37 µs`** | `25.16` / `639.98 µs` | `0` / `0` | **PASS** (contention eliminated) |
| **4** | QNX 8.0.0 (`2.80 GHz`) | `2.48 µs` / `205.42 µs` | `9,181` (`139` thr) | `2.25` / **`136.63`** / **`218.65 µs`** | `23.92` / `593.10 µs` | `0` / `0` | **PASS** (full 4-core SMP) |

*(Note: Section 2 used a 24-iteration single-process workload with `-n 16` to isolate vCPU scheduling contention; Section 1 uses the upgraded 3–4 process `fork()`+`execv()` + `/dev/qtg_sensor` workload with `-n 48`.)*

### Key Empirical Kernel Observations
- **Live `SchedCtl()` `ENOSYS` Confirmation:** `qtg-probe` calls `SchedCtl(0, ...)` directly against `procnto-smp-instr` and observes `ret = -1, errno = 89 (ENOSYS)`, confirming legacy APS is omitted on QNX OS 8.0 (`<sys/sched_aps.h>` is not shipped in SDP 8.0), while `SCHED_SPORADIC` is available (`max_unpriv_priority = 63`).
- **SMP Runmask Remediation (`QTG-FIX-SMP-RUNMASK`):** On 1-vCPU contention traces (like `trace_1cpu.json` above), `tools/qtg_ai_patch.py` detects dispatch-tail SLA breaches and emits a compiler-verified `ThreadCtl(_NTO_TCTL_RUNMASK, ...)` affinity patch!

Raw telemetry JSONs: [qnx8_kvm_smp_benchmark.json](qnx8_kvm_smp_benchmark.json), [probe_1cpu.json](probe_1cpu.json), [probe_2cpu.json](probe_2cpu.json), [probe_4cpu.json](probe_4cpu.json).

---

## 3. Parser Security Hardening — LibFuzzer + ASan + UBSan

Our fuzz harness ([tests/fuzz_kev.c](../tests/fuzz_kev.c)) exercises **both** the binary `.kev` parser (`qtg_kev_read_buffer`) and the `traceprinter` plain-text parser (`qtg_traceprinter_read_buffer`), followed by `qtg_analyze_trace` and both JSON and SARIF 2.1.0 serializers/escapers:

- **Initial Campaign (`-max_len=16384`, 26s):** `47,642` executions (`1,832 exec/s`), `1,214` coverage units, **`0` crashes / `0` ASan/UBSan findings**.
- **Full-Length Multi-Process Seeded Campaign (`-max_len=327680`, 21s, seeded with real `samples/qnx8_live/*.kev`):** `10,794` executions (`514 exec/s`), `203` new corpus units across `.kev` + `traceprinter` + JSON/SARIF, **`0` crashes / `0` UB / `0` leaks** (peak RSS `371 MB`).

---

## 4. Real QNX SDP 8.0 Cross-Compilation & Compiler-Verified AI Patches

- **Dual-Architecture `qcc` Builds:** `make qnx-x86_64` (`qcc -Vgcc_ntox86_64`) and `make qnx-aarch64` (`qcc -Vgcc_ntoaarch64le`) both compile with `-std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Werror` (**0 warnings, 0 errors**).
- **Compiler-Verified AI Remediation (`tools/qtg_ai_patch.py --verify-cc`):**
  - On our QNX SDP 8.0 build host: **5/5** generated QNX OS 8.0 C patches (`deadlock`, `prio-inversion`, `aid-cap`, `pulse-storm`, `aps-legacy`) + `QTG-FIX-SMP-RUNMASK` compile cleanly under real **`qcc -Vgcc_ntox86_64 -std=c11 -c -Wall -Wextra -Werror -o /dev/null`**.
  - On Linux CI runners without SDP: the same `--verify-cc` flag enforces `-Wall -Wextra -Werror` syntax verification against clean-room stubs in `tests/qnx_stub_headers/`, exiting `1` if any patch fails.
