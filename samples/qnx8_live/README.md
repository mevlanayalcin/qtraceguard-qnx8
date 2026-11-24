# Bundled Live Multi-Process QNX OS 8.0 Kernel Trace Captures (Clean-Room Telemetry)

These **5 binary `.kev` files** were recorded directly from a live instrumented **QNX OS 8.0 kernel** (`QNX noname 8.0.0 2026/10/30-09:10:05EDT x86pc`, `procnto-smp-instr` 2.8.0, 4 vCPU KVM) using `tracelogger -w -s 1 -n 48` against our MIT-licensed multi-process `qtg-resmgr-node` workload (`qtg_sensor_rm`, `qtg_planner_rm`, `qtg_actuator_rm`, `qtg_telemetry_bg`, plus `/dev/qtg_sensor` `resmgr_attach`).

They contain **zero proprietary QNX SDK headers, binaries, or OS images** — only clean-room runtime kernel trace telemetry (verified free of hostnames/IPs/credentials) so any reviewer or CI runner can verify QTraceGuard on genuine multi-process QNX OS 8.0 binary traces in seconds without needing QNX SDP installed.

## Instant 1-Command Verification (No QNX SDP Required)

```bash
make host

# 1. Healthy 3-process QNX OS 8.0 pipeline + /dev/qtg_sensor resmgr -> Exit 0 (PASS)
./build/qtraceguard check-sla samples/qnx8_live/qnx8_healthy.kev \
    --policy qemu/sla_policy.json --focus qtg

# 2. Live 3-process MsgSend deadlock -> Exit 2 (FAIL, exact 3-PID cycle caught at +101.454 ms)
./build/qtraceguard check-sla samples/qnx8_live/qnx8_deadlock.kev \
    --policy qemu/sla_policy.json --focus qtg \
    --json out/qnx8_deadlock.json --sarif out/qnx8_deadlock.sarif

# 3. Live 4-process _NTO_CHF_FIXED_PRIORITY trap (12 inversions, 431.39 us) -> Exit 2 (FAIL)
./build/qtraceguard check-sla samples/qnx8_live/qnx8_prio-inversion.kev \
    --policy qemu/sla_policy.json --focus qtg --json out/qnx8_prio.json

# 4. Live 4-process PROCMGR_AID_PRIORITY non-root setuid(1000) ceiling-63 cap (12 caps) -> Exit 2 (FAIL)
./build/qtraceguard check-sla samples/qnx8_live/qnx8_aid-cap.kev \
    --policy qemu/sla_policy.json --focus qtg --json out/qnx8_aid.json

# 5. Live 2-process kernel pulse queue saturation (40 sent / 4 recv) -> Exit 2 (FAIL)
./build/qtraceguard check-sla samples/qnx8_live/qnx8_pulse-storm.kev \
    --policy qemu/sla_policy.json --focus qtg --json out/qnx8_pulse.json

# 6. Render Interactive HTML5/SVG Trace Studio + Compiler-Verified AI Patch Report
python3 tools/qtg_report.py out/qnx8_deadlock.json out/qnx8_deadlock.html
python3 tools/qtg_ai_patch.py out/qnx8_deadlock.json \
    --out-md out/qnx8_deadlock_patch.md --out-json out/qnx8_deadlock_patch.json --verify-cc
```

## Capture Inventory (All 0 Dropped Buffers, Live 4-vCPU KVM QNX OS 8.0)

| File | Size | Events | Processes Captured | Expected `check-sla --focus qtg` Exit | Verified Live QNX OS 8.0 Finding |
| :--- | ---: | ---: | :--- | :---: | :--- |
| `qnx8_healthy.kev` | `542 KB` | `14,877` | `qtg_sensor_rm`, `qtg_planner_rm`, `qtg_actuator_rm` + `/dev/qtg_sensor` | **`0` (PASS)** | Dispatch `p50=7.60 µs`, `p99=111.30 µs`, `max=172.03 µs`; IPC `p50=13.41 µs`, `max=1237.00 µs`; `0` deadlocks, `0` inversions |
| `qnx8_deadlock.kev` | `461 KB` | `12,650` | `qtg_sensor_rm`, `qtg_planner_rm`, `qtg_actuator_rm` | **`2` (FAIL)** | Exact 3-PID cycle at `+101.454 ms`: `P4055062 -> P4063256 -> P4059159 -> P4055062` |
| `qnx8_prio-inversion.kev` | `700 KB` | `22,525` | `qtg_sensor_rm`, `qtg_planner_rm`, `qtg_actuator_rm`, `qtg_telemetry_bg` | **`2` (FAIL)** | **12/12** `_NTO_CHF_FIXED_PRIORITY` inversions (`prio 50` blocked on `eff_prio 12`, preempted by `prio 30` for `431.39 µs`) |
| `qnx8_aid-cap.kev` | `575 KB` | `15,317` | `qtg_sensor_rm`, `qtg_planner_rm`, `qtg_actuator_rm` (`uid=1000`), `qtg_telemetry_bg` | **`2` (FAIL)** | **12/12** live `PROCMGR_AID_PRIORITY` ceiling-63 inheritance caps (`prio 120` client capped at `eff_prio 63` by `procnto-smp-instr`) |
| `qnx8_pulse-storm.kev` | `360 KB` | `9,815` | `qtg_sensor_rm`, `qtg_actuator_rm` | **`2` (FAIL)** | Kernel pulse queue backlog `40/40` (`sent=40, recv=4, pending=36 [SATURATED]`) |
