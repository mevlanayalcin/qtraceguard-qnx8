# Module 7: QNX OS 8.0 Real-Time Systems Cheatsheet & Self-Check Quiz

**Estimated Time:** 5 minutes

---

## 7.1 QNX OS 8.0 Diagnostic & Tracing Quick Reference

| Task | Verified QNX OS 8.0 Target / Host Command |
| :--- | :--- |
| Source SDP 8.0 environment | `source ~/qnx800/qnxsdp-env.sh` |
| Cross-compile for x86_64 (QEMU) | `qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Werror app.c -o app` |
| Cross-compile for aarch64le (RPi 5) | `qcc -Vgcc_ntoaarch64le -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Werror app.c -o app` |
| Boot instrumented QNX 8.0 KVM VM | `./qemu/qnx800_harness.sh --boot-qemu` |
| Capture wide-mode binary `.kev` trace | `tracelogger -w -s 1 -n 48 -f /dev/shmem/qtg.kev` |
| Dump `.kev` to text (QNX native) | `traceprinter -f /dev/shmem/qtg.kev > trace.txt` |
| Dump `.kev` on Linux/macOS (open-source) | `./build/qtraceguard inspect samples/qnx8_live/qnx8_deadlock.kev --limit 50` |
| Inspect live process/thread IPC blocks | `pidin -F "%a %b %N %h %J %p"` |
| Run fail-closed SLA gate + SARIF (`--focus`) | `./build/qtraceguard check-sla samples/qnx8_live/qnx8_deadlock.kev --policy qemu/sla_policy.json --focus qtg --json out/dl.json --sarif out/dl.sarif` |
| Generate compiler-verified AI C patch | `python3 tools/qtg_ai_patch.py out/dl.json --out-md out/dl_patch.md --verify-cc` |
| Generate interactive HTML5/SVG Trace Studio | `python3 tools/qtg_report.py out/dl.json out/dl.html` |
| Convert QNX 7.1 APS trace to QNX 8.0 | `./build/qtraceguard migrate-aps legacy_71.kev` |
| Fuzz binary `.kev` + `traceprinter` parser | `make fuzz FUZZ_TIME=30` |

---

## 7.2 QNX Binary `.kev` 32-Bit Event Header Layout (`traceevent_t`)

Each raw event slot in a QNX `.kev` file is **16 bytes** (`uint32_t header; uint32_t data[3];`):

```text
 31  30 29        24 23          15 14      10 9                 0
+------+------------+--------------+----------+-------------------+
|STRUCT|   CPU ID   |   reserved   |  CLASS   |     EVENT ID      |
|2 bits|   6 bits   |    9 bits    |  5 bits  |     10 bits       |
+------+------------+--------------+----------+-------------------+
  0=S     0..63                     1=CONTROL   0..1023
  1=CB                              2=KERCALL   (COMM: 0=SMSG,
  2=CC                              4=PR_TH      1=SPULSE, 2=RMSG,
  3=CE                              5=SYSTEM     3=RPULSE, 10=REPLY,
                                    6=USER       11=ERROR)
                                    7=COMM
```
- `data[0]`: lower 32 bits of CPU cycle counter (upper 32 bits synchronized **per CPU** via `CONTROL_TIME` events).
- `data[1..2]`: event payload words; multi-slot combine events (`CB -> CC* -> CE`) must be reassembled **per CPU ID** on SMP systems, and out-of-order SMP `tracebuf_t` buffers must be chronologically sorted!

---

## 7.3 Self-Check Quiz

1. **Why must `procnto-smp-instr` be used instead of the standard non-instrumented kernel when capturing `.kev` traces?**
   <details><summary>Answer</summary>Only the instrumented microkernel (`procnto-smp-instr`) includes the low-overhead kernel event trace hooks that feed `tracelogger` and `TraceEvent(_NTO_TRACE_INSERTSUSEREVENT, ...)`.</details>

2. **What happens if Process A calls `MsgSend` to Process B while Process B is already blocked in `MsgSend` to Process A?**
   <details><summary>Answer</summary>Both threads block permanently in `STATE_SEND`/`STATE_REPLY` (a synchronous IPC wait-for deadlock cycle) unless unblocked by a `TimerTimeout()` / signal or broken architecturally via a strict DAG or asynchronous `MsgSendPulse()`.</details>

3. **What subtle real-time risk does `ChannelCreate(_NTO_CHF_FIXED_PRIORITY)` introduce on a safety-critical resource manager?**
   <details><summary>Answer</summary>It disables QNX's automatic message-driven priority inheritance, allowing medium-priority tasks to preempt the server while a high-priority client is blocked waiting for a reply.</details>

4. **In QNX OS 8.0, a privileged safety client at priority `120` calls a non-root (`setuid(1000)`) server lacking `PROCMGR_AID_PRIORITY`. What effective priority does the server inherit?**
   <details><summary>Answer</summary>Priority `63` (verified empirically on live QNX OS 8.0 `procnto-smp-instr`). Any task at priorities `64..119` can preempt the server unless the server retains `PROCMGR_AID_PRIORITY` via `procmgr_ability()` BEFORE dropping root (or via `secpol`).</details>

5. **Why does `SchedCtl()` return `ENOSYS` (`errno=89`) on QNX OS 8.0, and what should you use instead?**
   <details><summary>Answer</summary>QNX OS 8.0 omitted the legacy APS module (`<sys/sched_aps.h>` is no longer shipped). Use POSIX `SCHED_SPORADIC` for per-thread execution budgets, `CLOCK_THREAD_CPUTIME_ID` for runtime monitoring, and `ThreadCtl(_NTO_TCTL_RUNMASK, ...)` for SMP core isolation.</details>

6. **Why must a `.kev` parser track both combine events (`CB/CC/CE`) AND `CONTROL_TIME` wraparound per CPU ID rather than globally?**
   <details><summary>Answer</summary>On SMP QNX 8.0 systems, multiple CPUs write independent 16 KB `tracebuf_t` buffers that flush out-of-order into the `.kev` file; global `ts_hi`/`last_ts_lo` tracking causes 2^32 cycle timestamp reversals across CPUs.</details>

7. **Why does `qtraceguard check-sla` require `--focus <proc>` on full-system QNX 8.0 captures, and what happens if you misspell the `--focus` process name?**
   <details><summary>Answer</summary>A full-system QNX 8.0 trace captures 25–30 system daemons (`sshd`, `io-sock`, `devb-eide`) alongside your real-time control pipeline. `--focus` scopes latency, cycle, inversion, and pulse gates to your target processes — and `qtraceguard` fails CLOSED (exit code `2`, `FOCUS MISMATCH`) if 0 threads match, preventing silent false passes in CI.</details>

8. **What does `tools/qtg_ai_patch.py --verify-cc` guarantee beyond ordinary LLM code suggestions?**
   <details><summary>Answer</summary>Every generated QNX OS 8.0 C remediation snippet is compiled under `-std=c11 -Wall -Wextra -Werror` (via real `qcc -Vgcc_ntox86_64` when QNX SDP 8.0 is sourced, or `gcc` clean-room stubs in Linux CI); if any patch fails compilation, the tool exits non-zero (and in `--llm` mode runs a compiler-feedback repair pass).</details>

---

Congratulations! You have completed the **QTraceGuard QNX OS 8.0 Codelab**. Return to [Codelab Home](index.md) or explore the full source code in `include/`, `src/`, `tools/`, `samples/qnx8_live/`, `benchmarks/`, and `qemu/`.
