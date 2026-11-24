# QTraceGuard — < 3-Minute YouTube Demo Script (Target: ~2 min 35 sec)

**Format:** 1080p 60fps studio presentation (`1920x1080`, `libx264 yuv420p`, `aac 192k`), synced neural voiceover (`en-US-AndrewNeural`), real live QNX OS 8.0 KVM captures (`samples/qnx8_live/`), rendered deterministically via `python3 demo/render_demo_video.py`.

---

## Scene-by-Scene Runsheet

1. **[0:00 – 0:22] Hook & Real QNX OS 8.0 Failure Modes (22s)**
   - Visual: QTraceGuard Hero + Verified Live on `procnto-smp-instr` 2.8.0 (4 vCPU KVM) + 5 QNX OS 8.0 production traps.
   - Voiceover: Introduces QTraceGuard as a fail-closed C11 `.kev` verifier, compiler-verified AI Patch Copilot, interactive HTML5/SVG Trace Studio, QEMU/KVM harness, and 7-module Codelab — 100% MIT clean-room.

2. **[0:22 – 0:48] Native QNX SDP 8.0 Builds, Live `qtg-probe` & Healthy 3-Process Trace (26s)**
   - Visual: Real `qcc -Vgcc_ntox86_64` & `qcc -Vgcc_ntoaarch64le` 0-warning builds, live `qtg-probe` (`SchedCtl errno=89 ENOSYS`, `p50=2.58 us`), and live 3-process `qnx8_healthy.kev` + `/dev/qtg_sensor` `resmgr_attach` passing SLA (`exit 0`, `14,877` events, `p99=111.30 us`).

3. **[0:48 – 1:18] Catching a Live 3-Process QNX OS 8.0 Deadlock + Interactive SVG Studio (30s)**
   - Visual: Live `qnx8_deadlock.kev` catching `P4055062(qtg_sensor_rm) -> P4063256(qtg_planner_rm) -> P4059159(qtg_actuator_rm) -> P4055062` at `+101.454 ms` (`exit 2`, SARIF 2.1.0) paired with our live Interactive HTML5/SVG Trace Studio screenshot (`docs/dashboard_live_deadlock.png`).

4. **[1:18 – 1:46] Live Priority-Inversion Traps: `_NTO_CHF_FIXED_PRIORITY` & `PROCMGR_AID_PRIORITY` Cap 63 (28s)**
   - Visual: Live 4-process `qnx8_prio-inversion.kev` (`12/12`, `431.39 us`) and live non-root `setuid(1000)` `qnx8_aid-cap.kev` (capped at `eff_prio 63` by `procnto-smp-instr`, `12/12` caught) paired with `docs/dashboard_live_aid_cap.png`.

5. **[1:46 – 2:14] Neuro-Symbolic Compiler-Verified AI Patch Copilot (`qcc -Werror`) & APS Migration (28s)**
   - Visual: `tools/qtg_ai_patch.py --verify-cc` generating syntax-highlighted unified C diffs verified `5/5` under real `qcc -Vgcc_ntox86_64 -Werror` (plus optional `--llm` compiler-in-the-loop repair), alongside `qtraceguard migrate-aps` generating QNX 8.0 `procmgr_ability()` + `SCHED_SPORADIC` code.

6. **[2:14 – 2:38] 1/2/4 vCPU KVM Scaling, 58,000+ LibFuzzer Hardening & 7-Module Codelab Close (24s)**
   - Visual: 1/2/4 vCPU KVM latency chart (`1185 us FAIL -> 144 us PASS`), `13/13` C11 + `13/13` E2E + `13/13` ASan/UBSan + `58,436` LibFuzzer runs (`0` crashes), fail-closed CI + 7-module Codelab (`codelab/index.md`) with 10-second zero-SDP Quick Start.
