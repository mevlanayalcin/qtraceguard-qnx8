#!/usr/bin/env python3
"""
QTraceGuard - Deterministic 1080p60 Studio Demo Video Renderer (< 2:50 for Devpost)
SPDX-License-Identifier: MIT

Uses local Playwright + Google Chrome (HTML5/SVG 1920x1080 slides embedding live QNX 8.0
dashboard screenshots), edge-tts (en-US-AndrewNeural), and ffmpeg (libx264 + aac + xfade)
to produce a broadcast-grade < 3-minute YouTube submission video.
"""

from __future__ import annotations

import base64
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List, Tuple

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "out" / "demo_video_build"
OUT_MP4 = ROOT / "demo" / "qtraceguard_demo_1080p60.mp4"
FFMPEG = os.environ.get("QTG_FFMPEG") or shutil.which("ffmpeg") or "ffmpeg"
FFPROBE = os.environ.get("QTG_FFPROBE") or shutil.which("ffprobe") or "ffprobe"
EDGE_TTS = os.environ.get("QTG_EDGE_TTS") or shutil.which("edge-tts") or "edge-tts"
CHROME = os.environ.get("QTG_CHROME") or shutil.which("google-chrome") or shutil.which("chromium") or "/usr/bin/google-chrome"
VOICE = "en-US-AndrewNeural"


def _b64_png(rel_path: str) -> str:
    data = (ROOT / rel_path).read_bytes()
    return "data:image/png;base64," + base64.b64encode(data).decode("ascii")


def _probe_duration(media_path: Path) -> float:
    out = subprocess.check_output(
        [FFPROBE, "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", str(media_path)],
        text=True,
    ).strip()
    return float(out)


BASE_CSS = """
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    width: 1920px; height: 1080px; overflow: hidden;
    background: radial-gradient(circle at 18% 15%, #0f172a 0%, #020617 65%);
    color: #e2e8f0;
    font-family: system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif;
    display: flex; flex-direction: column; justify-content: space-between;
    padding: 46px 64px 36px 64px;
  }
  .topbar { display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid #1e293b; padding-bottom:18px; }
  .brand { display:flex; align-items:center; gap:14px; }
  .logo { width:40px; height:40px; border-radius:10px; background:linear-gradient(135deg,#0ea5e9,#10b981); display:flex; align-items:center; justify-content:center; font-weight:900; font-size:20px; color:#020617; }
  .brand-name { font-size:26px; font-weight:800; letter-spacing:-0.02em; color:#f8fafc; }
  .brand-sub { font-size:15px; color:#94a3b8; font-family:monospace; }
  .pill-row { display:flex; gap:10px; }
  .pill { padding:8px 16px; border-radius:999px; font-size:14px; font-weight:700; font-family:monospace; letter-spacing:0.03em; }
  .pill-green { background:#064e3b; color:#6ee7b7; border:1px solid #10b981; }
  .pill-blue  { background:#0c4a6e; color:#7dd3fc; border:1px solid #38bdf8; }
  .pill-red   { background:#450a0a; color:#fca5a5; border:1px solid #ef4444; }
  .pill-amber { background:#451a03; color:#fcd34d; border:1px solid #f59e0b; }
  .main { flex:1; display:flex; flex-direction:column; justify-content:center; gap:24px; padding:18px 0; }
  .split { display:grid; grid-template-columns:1fr 1.12fr; gap:30px; align-items:stretch; }
  .split-eq { display:grid; grid-template-columns:1fr 1fr; gap:28px; align-items:stretch; }
  h1 { font-size:58px; font-weight:900; letter-spacing:-0.03em; line-height:1.08; color:#f8fafc; }
  h2 { font-size:36px; font-weight:800; letter-spacing:-0.02em; color:#f8fafc; margin-bottom:6px; }
  .accent { color:#38bdf8; } .ok { color:#4ade80; } .err { color:#f87171; } .warn { color:#fbbf24; }
  .lead { font-size:23px; color:#cbd5e1; line-height:1.45; max-width:1420px; }
  .card { background:#0f172a; border:1px solid #1e293b; border-radius:16px; padding:24px 26px; box-shadow:0 18px 40px rgba(0,0,0,0.35); }
  .card-title { font-size:16px; font-weight:700; text-transform:uppercase; letter-spacing:0.06em; color:#38bdf8; margin-bottom:14px; font-family:monospace; }
  .term { background:#020617; border:1px solid #334155; border-radius:14px; padding:22px 24px; font-family:'DejaVu Sans Mono','Liberation Mono',monospace; font-size:17px; line-height:1.52; color:#e2e8f0; white-space:pre-wrap; }
  .term-sm { font-size:15.5px; line-height:1.45; }
  .cmd { color:#38bdf8; font-weight:700; }
  .dim { color:#64748b; }
  .grid-5 { display:grid; grid-template-columns:repeat(5, 1fr); gap:16px; }
  .grid-4 { display:grid; grid-template-columns:repeat(4, 1fr); gap:18px; }
  .grid-3 { display:grid; grid-template-columns:repeat(3, 1fr); gap:20px; }
  .kpi { background:#0f172a; border:1px solid #1e293b; border-radius:14px; padding:20px; }
  .kpi-val { font-size:34px; font-weight:800; font-family:monospace; margin-top:8px; }
  .kpi-lbl { font-size:13px; color:#94a3b8; text-transform:uppercase; letter-spacing:0.05em; font-weight:700; }
  .kpi-sub { font-size:14px; color:#cbd5e1; margin-top:6px; }
  .shot-frame { background:#020617; border:2px solid #334155; border-radius:14px; overflow:hidden; display:flex; align-items:center; justify-content:center; box-shadow:0 20px 50px rgba(0,0,0,0.5); }
  .shot-frame img { width:100%; height:100%; object-fit:contain; display:block; }
  .footer { display:flex; justify-content:space-between; align-items:center; border-top:1px solid #1e293b; padding-top:14px; font-size:15px; color:#94a3b8; font-family:monospace; }
  .prog-wrap { display:flex; gap:8px; align-items:center; }
  .dot { width:44px; height:6px; border-radius:4px; background:#1e293b; }
  .dot.active { background:#38bdf8; }
  .dot.done { background:#10b981; }
  .d-hdr { color:#38bdf8; font-weight:700; } .d-add { color:#4ade80; } .d-del { color:#f87171; }
  table { width:100%; border-collapse:collapse; font-size:17px; }
  th, td { text-align:left; padding:11px 14px; border-bottom:1px solid #1e293b; }
  th { color:#94a3b8; font-size:14px; text-transform:uppercase; font-family:monospace; }
  td.mono { font-family:monospace; }
"""


def _wrap_slide(idx: int, total: int, badge_html: str, body_html: str, footer_left: str) -> str:
    dots = []
    for i in range(total):
        cls = "dot done" if i < idx else ("dot active" if i == idx else "dot")
        dots.append(f'<div class="{cls}"></div>')
    return f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"/><style>{BASE_CSS}</style></head>
<body>
  <div class="topbar">
    <div class="brand">
      <div class="logo">QT</div>
      <div>
        <div class="brand-name">QTraceGuard</div>
        <div class="brand-sub">QNX Global Hackathon 2026 &middot; MIT Clean-Room C11 Host Verifier &amp; Codelab</div>
      </div>
    </div>
    <div class="pill-row">{badge_html}</div>
  </div>
  <div class="main">{body_html}</div>
  <div class="footer">
    <div>{footer_left}</div>
    <div class="prog-wrap"><span>Scene {idx + 1}/{total}</span>{''.join(dots)}</div>
  </div>
</body></html>"""


def build_scenes() -> List[Tuple[str, str, str]]:
    """Return list of (scene_id, narration_text, html_doc)."""
    dl_png = _b64_png("docs/dashboard_live_deadlock.png")
    aid_png = _b64_png("docs/dashboard_live_aid_cap.png")
    total = 6

    scenes: List[Tuple[str, str, str]] = []

    # Scene 1: Hook & 5 QNX OS 8.0 Failure Modes
    s1_vo = (
        "Safety-critical QNX OS 8.0 teams rely on synchronous microkernel IPC and deterministic scheduling. "
        "Yet five QNX traps slip past unit tests: multi-process MsgSend deadlocks, fixed-priority inversions, "
        "non-root servers capped at priority 63 without PROCMGR_AID_PRIORITY, pulse storms, and legacy APS returning ENOSYS. "
        "Meet QTraceGuard: a fail-closed C11 trace verifier, compiler-verified AI patch copilot, and 7-module Codelab."
    )
    s1_html = _wrap_slide(
        0, total,
        '<span class="pill pill-green">VERIFIED LIVE ON QNX OS 8.0 KVM</span>'
        '<span class="pill pill-blue">ZERO EXTERNAL C DEPS &middot; MIT</span>',
        """
        <h1>Deterministic Real-Time Verification<br/>for <span class="accent">QNX OS 8.0</span> Microkernel Systems</h1>
        <p class="lead">Decodes native 16-byte binary <code class="accent">.kev</code> streams from <code class="ok">procnto-smp-instr</code> &amp; <code class="ok">tracelogger</code> into fail-closed CI gates, SARIF 2.1.0 alerts, interactive SVG trace studios, and <code class="ok">qcc -Werror</code> verified C remediation patches.</p>
        <div class="grid-5">
          <div class="card">
            <div class="card-title err">1. IPC Deadlocks</div>
            <p style="font-size:17px;color:#cbd5e1;line-height:1.4">Reconstructs live <code class="accent">MsgSend/MsgReply</code> wait-for graphs across distinct QNX PIDs to pinpoint exact cycles.</p>
          </div>
          <div class="card">
            <div class="card-title warn">2. FIXED_PRIORITY</div>
            <p style="font-size:17px;color:#cbd5e1;line-height:1.4">Catches channels where <code class="warn">_NTO_CHF_FIXED_PRIORITY</code> disables QNX priority inheritance.</p>
          </div>
          <div class="card">
            <div class="card-title warn">3. AID Ceiling 63</div>
            <p style="font-size:17px;color:#cbd5e1;line-height:1.4">Catches non-root servers capped at prio <b class="warn">63</b> when missing <code class="warn">PROCMGR_AID_PRIORITY</code>.</p>
          </div>
          <div class="card">
            <div class="card-title err">4. Pulse Storms</div>
            <p style="font-size:17px;color:#cbd5e1;line-height:1.4">Monitors kernel pulse queues and fails closed on saturation, dropped buffers, or focus typos.</p>
          </div>
          <div class="card">
            <div class="card-title ok">5. APS &rarr; SPORADIC</div>
            <p style="font-size:17px;color:#cbd5e1;line-height:1.4">Verifies live <code class="ok">SchedCtl ENOSYS</code> and generates QNX 8.0 <code class="ok">SCHED_SPORADIC</code> C code.</p>
          </div>
        </div>
        """,
        "Target Tracks: Developer Tooling & Host Tools · Best Use of QNX Technologies · Best Codelab"
    )
    scenes.append(("scene1_hook", s1_vo, s1_html))

    # Scene 2: Live QNX SDP 8.0 Build, Live qtg-probe & Healthy 3-Process Trace
    s2_vo = (
        "QTraceGuard cross-compiles cleanly with QNX SDP 8.0 qcc for x86_64 and ARM64 with zero warnings. "
        "Inside KVM-accelerated QNX OS 8.0, qtg-probe calls SchedCtl directly, proving errno 89 ENOSYS for APS, "
        "SCHED_SPORADIC support, and 2.58 microsecond median IPC latency. "
        "On a healthy 3-process /dev/qtg_sensor pipeline, QTraceGuard decodes 14,877 kernel events and passes every SLA gate."
    )
    s2_html = _wrap_slide(
        1, total,
        '<span class="pill pill-green">QCC X86_64 + AARCH64LE &middot; 0 WARNINGS</span>'
        '<span class="pill pill-green">SLA PASS &middot; EXIT 0</span>',
        """
        <h2>Real QNX SDP 8.0 Cross-Build, Live <span class="accent">qtg-probe</span> &amp; Healthy 3-Process Trace</h2>
        <div class="split-eq">
          <div class="term term-sm"><span class="cmd">$ source ~/qnx800/qnxsdp-env.sh &amp;&amp; make qnx-x86_64 qnx-aarch64</span>
qcc -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Werror ...
<span class="ok">[OK] Built QNX 8.0 x86_64 target binaries in build-qnx-x86_64/</span>
qcc -Vgcc_ntoaarch64le -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Werror ...
<span class="ok">[OK] Built QNX 8.0 aarch64le target binaries in build-qnx-aarch64/</span>

<span class="cmd">$ ssh root@qnx8-kvm "/tmp/qtg-probe --json --iters 200"</span>
{
  "qnx_native": <span class="ok">true</span>,
  "kernel_release": "<span class="accent">QNX 8.0.0 (x86pc)</span>",
  "aps_supported": <span class="warn">false</span>,  "aps_errno": <span class="warn">89</span>,  <span class="dim">// Live SchedCtl() ENOSYS!</span>
  "sporadic_supported": <span class="ok">true</span>,  "max_unpriv_priority": <span class="accent">63</span>,
  "ipc_bench": {"succeeded": <span class="ok">200</span>, "p50_ns": <span class="ok">2580</span>}  <span class="dim">// 2.58 us median</span>
}</div>
          <div class="term term-sm"><span class="cmd">$ ./build/qtraceguard check-sla samples/qnx8_live/qnx8_healthy.kev \\
    --policy qemu/sla_policy.json --focus qtg</span>
<span class="dim">=== QTraceGuard QNX OS 8.0 Deterministic Trace Report ===</span>
Target : <span class="accent">QNX 8.0.0 (x86pc)</span> on noname | CPUs: 4 | Clock: 2.80 GHz
Trace  : <span class="ok">14,877 events</span> | 995.24 ms | Procs: 29 | 0 dropped buffers
ResMgr : <span class="ok">Mounted &amp; read /dev/qtg_sensor (iofunc_read + TraceEvent)</span>

[1] Scheduling &amp; IPC Latency Summary
  Dispatch (READY-&gt;RUNNING) : p50=  <span class="ok">7.60 us</span> | p99= <span class="ok">111.30 us</span> | max= <span class="ok">172.03 us</span>
  IPC RTT  (Send-&gt;Reply)    : p50= <span class="ok">13.41 us</span> | p99= <span class="ok">791.24 us</span> | max=<span class="ok">1237.00 us</span>

[2] Deadlock Cycles         : <span class="ok">0 detected</span>
[3] Priority Inversions     : <span class="ok">0 detected</span>
<span class="ok">[SLA VERDICT] PASS (all deterministic real-time gates satisfied)  [exit 0]</span></div>
        </div>
        """,
        "Bundled live clean-room trace: samples/qnx8_live/qnx8_healthy.kev (0 proprietary SDK files redistributed)"
    )
    scenes.append(("scene2_healthy", s2_vo, s2_html))

    # Scene 3: Live 3-Process QNX 8.0 Deadlock + Interactive Trace Studio
    s3_vo = (
        "When qtg_actuator_rm calls back synchronously into qtg_sensor_rm on live QNX OS 8.0, "
        "QTraceGuard parses 12,650 binary events with per-CPU CONTROL_TIME ordering, pinpoints the exact three-PID deadlock at 101 milliseconds, "
        "fails CI with exit code 2, uploads SARIF 2.1.0 to GitHub Code Scanning, and renders this interactive SVG Trace Studio."
    )
    s3_html = _wrap_slide(
        2, total,
        '<span class="pill pill-red">LIVE 3-PROCESS DEADLOCK &middot; EXIT 2</span>'
        '<span class="pill pill-blue">SARIF 2.1.0 + INTERACTIVE SVG</span>',
        f"""
        <h2>Catching a Live <span class="err">3-Process QNX OS 8.0 Deadlock</span> &amp; Interactive Studio</h2>
        <div class="split">
          <div class="term term-sm"><span class="cmd">$ ./build/qtraceguard check-sla samples/qnx8_live/qnx8_deadlock.kev \\
    --policy qemu/sla_policy.json --focus qtg \\
    --json out/dl.json --sarif out/dl.sarif</span>

Target : <span class="accent">QNX 8.0.0 (x86pc)</span> | CPUs: 4 | 12,650 events | 0 dropped

<span class="err">[2] IPC Wait-For Deadlock Cycles (1 detected)</span>
  <span class="err">CYCLE #1 (at 101.454 ms):</span>
    P4055062:T1(<span class="accent">qtg_sensor_rm</span>)   --[ch:1]--&gt;
    P4063256:T1(<span class="accent">qtg_planner_rm</span>)  --[ch:1]--&gt;
    P4059159:T1(<span class="accent">qtg_actuator_rm</span>) --[ch:1]--&gt;
    P4055062:T1(<span class="accent">qtg_sensor_rm</span>)

<span class="err">[SLA VERDICT] FAIL (2 violation(s))</span>
  <span class="err">- IPC DEADLOCK: detected 1 wait-for cycle(s) (limit: 0)</span>
  <span class="err">- IPC RTT MAX: 3674.32 us exceeds limit 2000 us</span>
<span class="cmd">$ echo $?</span>  =&gt;  <span class="err">2</span>  <span class="dim">(Blocks merge + GitHub Code Scanning QTG001)</span></div>
          <div class="shot-frame"><img src="{dl_png}" alt="Live QNX 8.0 3-Process Deadlock Dashboard"/></div>
        </div>
        """,
        "Zero-SDP verification: ./build/qtraceguard check-sla samples/qnx8_live/qnx8_deadlock.kev --focus qtg"
    )
    scenes.append(("scene3_deadlock", s3_vo, s3_html))

    # Scene 4: Live Priority-Inversion Traps (_NTO_CHF_FIXED_PRIORITY & PROCMGR_AID_PRIORITY Ceiling 63)
    s4_vo = (
        "QTraceGuard also catches QNX 8.0 priority-inheritance traps live — even on third-party traces with zero markers. "
        "It catches all 12 fixed-priority preemptions at 431 microseconds, and when qtg_actuator_rm drops root via setuid 1000 without PROCMGR_AID_PRIORITY, "
        "procnto-smp-instr caps its inherited priority from 120 down to 63, flagged in amber inside Trace Studio."
    )
    s4_html = _wrap_slide(
        3, total,
        '<span class="pill pill-amber">LIVE _NTO_CHF_FIXED_PRIORITY &middot; 12/12</span>'
        '<span class="pill pill-amber">LIVE SETUID(1000) CAP=63 &middot; 12/12</span>',
        f"""
        <h2>Live QNX 8.0 <span class="warn">Priority-Inversion</span> &amp; <span class="warn">PROCMGR_AID_PRIORITY</span> Traps</h2>
        <div class="split">
          <div class="term term-sm"><span class="cmd">$ ./build/qtraceguard check-sla samples/qnx8_live/qnx8_prio-inversion.kev --focus qtg</span>
<span class="warn">[FIXED_PRIORITY_CHANNEL]</span> High P4309017(<span class="accent">qtg_planner_rm</span>, prio 50)
  blocked on Server P4304920(<span class="accent">qtg_actuator_rm</span>, <span class="err">eff_prio 12</span>)
  preempted by P4300823(<span class="accent">qtg_telemetry_bg</span>, prio 30) for <span class="err">431.39 us</span>
<span class="err">- _NTO_CHF_FIXED_PRIORITY TRAP: 12 inversion(s) caught [exit 2]</span>

<span class="cmd">$ ./build/qtraceguard check-sla samples/qnx8_live/qnx8_aid-cap.kev --focus qtg</span>
<span class="warn">[PROCMGR_AID_PRIORITY_CAP]</span> High P4550681(<span class="accent">qtg_planner_rm</span>, prio <span class="ok">120</span>)
  blocked on non-root Server P4546584(<span class="accent">qtg_actuator_rm</span>, <span class="warn">eff_prio 63</span>)
<span class="err">- QNX 8.0 PROCMGR_AID_PRIORITY CAP: 12 ceiling-63 caps [exit 2]</span>

<span class="dim">// Empirically verified on live QNX 8.0.0 procnto-smp-instr:
// non-root server lacking PROCMGR_AID_PRIORITY inherits ceiling 63!</span></div>
          <div class="shot-frame"><img src="{aid_png}" alt="Live QNX 8.0 PROCMGR_AID_PRIORITY Cap Dashboard"/></div>
        </div>
        """,
        "Zero-marker third-party trace detection: automatically infers broken priority inheritance from kernel THRUNNING events"
    )
    scenes.append(("scene4_prio_aid", s4_vo, s4_html))

    # Scene 5: Compiler-Verified Neuro-Symbolic AI Patch Copilot + APS -> SCHED_SPORADIC
    s5_vo = (
        "Finding a bug is half the battle — QTraceGuard fixes it with compiler-verified proof. "
        "Our neuro-symbolic AI Patch Copilot emits trace-grounded QNX OS 8.0 C diffs compiled under qcc Werror, "
        "plus optional LLM compiler-in-the-loop self-repair, while migrate-aps turns QNX 7.1 APS traces into SCHED_SPORADIC code."
    )
    s5_html = _wrap_slide(
        4, total,
        '<span class="pill pill-green">6/6 PATCHES VERIFIED WITH QCC -WERROR</span>'
        '<span class="pill pill-blue">NEURO-SYMBOLIC AI COPILOT</span>',
        """
        <h2>Compiler-Verified <span class="ok">AI Patch Copilot</span> &amp; <span class="accent">APS &rarr; SCHED_SPORADIC</span></h2>
        <div class="split-eq">
          <div class="term term-sm"><span class="cmd">$ python3 tools/qtg_ai_patch.py out/qnx8_deadlock.json --verify-cc</span>
<span class="ok">[qtg-ai-patch] Generated 1 patch (verified=1/1)</span>
<span class="ok">Compiler: VERIFIED (qcc QNX SDP 8.0 gcc_ntox86_64 -Werror, 0 errors)</span>

<span class="d-hdr">--- a/src/target/qtg_resmgr_node.c
+++ b/src/target/qtg_resmgr_node.c
@@ -493,3 +493,5 @@</span>
<span class="d-del">-  qtg_shim_msg_send(getpid(), 1U, sensor_coid, &amp;cb, ..., 75U);</span>
<span class="d-add">+  /* QTraceGuard Fix: keep IPC graph a strict DAG via MsgSendPulse */
+  if (MsgSendPulse(sensor_coid, -1, QTG_SHIM_PULSE_CODE_PING,
+                   (int)req.seq) == -1) { /* backpressure counter */ }</span>

<span class="dim">✓ Air-gapped deterministic default (zero hallucinated PIDs)
✓ Opt-in --llm mode: LLM -&gt; qcc -Werror -&gt; compiler-error self-repair</span></div>
          <div class="term term-sm"><span class="cmd">$ ./build/qtraceguard migrate-aps out/aps_legacy.kev</span>
<span class="dim">/* Partition #1: 'ADAS_Control' (50%, 1 bankruptcy -&gt; +20% headroom) */</span>
<span class="ok">procmgr_ability</span>(0, PROCMGR_ADN_NONROOT|PROCMGR_AOP_ALLOW|
                   <span class="accent">PROCMGR_AID_PRIORITY</span>, PROCMGR_AID_EOL);
struct sched_param sp_1 = {0};
sp_1.sched_priority              = <span class="accent">72</span>;
sp_1.sched_ss_low_priority       = 10;
sp_1.sched_ss_repl_period.tv_nsec = 100L * 1000000L;
sp_1.sched_ss_init_budget.tv_nsec =  <span class="ok">60L</span> * 1000000L;
sp_1.sched_ss_max_repl           = 4;
<span class="ok">pthread_setschedparam</span>(tid_1, <span class="accent">SCHED_SPORADIC</span>, &amp;sp_1);

<span class="dim">✓ Also emits _NTO_TCTL_RUNMASK affinity patches for dispatch-tail breaches</span></div>
        </div>
        """,
        "All 6 fault families (deadlock, fixed-prio, aid-cap, pulse-storm, aps-legacy, smp-runmask) compile clean under real qcc -Werror"
    )
    scenes.append(("scene5_ai_aps", s5_vo, s5_html))

    # Scene 6: 1/2/4 vCPU KVM Scaling, 58,000+ LibFuzzer Runs, Fail-Closed CI & 7-Module Codelab
    s6_vo = (
        "Across 1, 2, and 4 vCPU QNX OS 8.0 KVM topologies, single-CPU contention hits 1.18 milliseconds and fails SLA, "
        "while 2 and 4 vCPUs drop latency under 220 microseconds and pass. "
        "Backed by 39 passing test and sanitizer checks, 58,436 zero-crash LibFuzzer runs, and a 7-module Codelab you can run in 10 seconds — try QTraceGuard on GitHub."
    )
    s6_html = _wrap_slide(
        5, total,
        '<span class="pill pill-green">58,436 LIBFUZZER RUNS &middot; 0 CRASHES</span>'
        '<span class="pill pill-blue">7-MODULE HANDS-ON CODELAB</span>',
        """
        <h2>SMP KVM Telemetry, <span class="ok">LibFuzzer Hardening</span> &amp; <span class="accent">7-Module Codelab</span></h2>
        <div class="split-eq">
          <div class="card">
            <div class="card-title">Live QNX OS 8.0 KVM Scaling (procnto-smp-instr, 2.80 GHz)</div>
            <table>
              <thead><tr><th>vCPUs</th><th>Events</th><th>Dispatch p99 / Max</th><th>SLA Gate</th></tr></thead>
              <tbody>
                <tr><td class="mono"><b>1 vCPU</b></td><td class="mono">6,254</td><td class="mono err">158.28 / 1185.38 µs</td><td class="err"><b>FAIL</b> (contention)</td></tr>
                <tr><td class="mono"><b>2 vCPU</b></td><td class="mono">6,910</td><td class="mono ok">106.14 / 144.37 µs</td><td class="ok"><b>PASS</b> (8.2x lower)</td></tr>
                <tr><td class="mono"><b>4 vCPU</b></td><td class="mono">9,181</td><td class="mono ok">136.63 / 218.65 µs</td><td class="ok"><b>PASS</b> (full SMP)</td></tr>
              </tbody>
            </table>
            <div class="grid-3" style="margin-top:20px">
              <div class="kpi"><div class="kpi-lbl">Unit + E2E + ASan</div><div class="kpi-val ok">39/39</div><div class="kpi-sub">13 C11 · 13 E2E · 13 ASan</div></div>
              <div class="kpi"><div class="kpi-lbl">LibFuzzer + UBSan</div><div class="kpi-val ok">58,436</div><div class="kpi-sub">0 crashes · 0 leaks</div></div>
              <div class="kpi"><div class="kpi-lbl">Bundled Live .kev</div><div class="kpi-val accent">5 Traces</div><div class="kpi-sub">Run in 10s, no SDP</div></div>
            </div>
          </div>
          <div class="card">
            <div class="card-title">7-Module Hands-On QNX OS 8.0 Codelab (codelab/index.md)</div>
            <div class="term term-sm" style="margin-top:6px"><span class="accent">Module 1</span>  QNX Everywhere License &amp; Zero-Bridge KVM Bring-Up
<span class="accent">Module 2</span>  Microkernel IPC &amp; <span class="ok">/dev/qtg_sensor</span> iofunc_* ResMgr
<span class="accent">Module 3</span>  Lab: Inject, Catch &amp; Fix a 3-Process Deadlock
<span class="accent">Module 4</span>  Lab: <span class="warn">_NTO_CHF_FIXED_PRIORITY</span> &amp; <span class="warn">PROCMGR_AID_PRIORITY</span>
<span class="accent">Module 5</span>  Lab: Legacy QNX 7.1 APS &rarr; QNX 8.0 <span class="ok">SCHED_SPORADIC</span>
<span class="accent">Module 6</span>  Fail-Closed CI, AI Patch Copilot, LibFuzzer &amp; SARIF
<span class="accent">Module 7</span>  QNX 8.0 Real-Time Cheatsheet &amp; 8-Question Quiz

<span class="ok">github.com/mevlanayalcin/qtraceguard-qnx8</span></div>
          </div>
        </div>
        """,
        "100% MIT-Licensed Clean-Room Open Source · Zero Proprietary QNX SDK Binaries Redistributed"
    )
    scenes.append(("scene6_close", s6_vo, s6_html))

    return scenes


def main() -> int:
    WORK.mkdir(parents=True, exist_ok=True)
    scenes = build_scenes()

    # 1. Generate neural TTS audio per scene
    audio_files: List[Path] = []
    durations: List[float] = []
    for sid, vo, _ in scenes:
        mp3 = WORK / f"{sid}.mp3"
        subprocess.run(
            [EDGE_TTS, "--voice", VOICE, "--rate=+8%", "--text", vo, "--write-media", str(mp3)],
            check=True,
        )
        dur = _probe_duration(mp3)
        audio_files.append(mp3)
        durations.append(dur)
        print(f"[tts] {sid}: {dur:.2f}s")

    total_vo = sum(durations)
    print(f"[tts] Raw voiceover total: {total_vo:.2f}s")

    # 2. Render crisp 1920x1080 PNG frames via local Playwright + Google Chrome
    from playwright.sync_api import sync_playwright
    png_files: List[Path] = []
    with sync_playwright() as p:
        browser = p.chromium.launch(executable_path=CHROME, headless=True)
        page = browser.new_page(viewport={"width": 1920, "height": 1080}, device_scale_factor=1)
        for sid, _, html_doc in scenes:
            html_path = WORK / f"{sid}.html"
            png_path = WORK / f"{sid}.png"
            html_path.write_text(html_doc, encoding="utf-8")
            page.goto(html_path.as_uri(), wait_until="load")
            page.wait_for_timeout(120)
            page.screenshot(path=str(png_path), full_page=False)
            png_files.append(png_path)
            print(f"[slide] Rendered {png_path.name} (1920x1080)")
        browser.close()

    # 3. Build individual 1080p60 scene clips (with 0.55s trailing breathing room) and xfade assemble
    fade = 0.45
    clip_paths: List[Path] = []
    clip_durs: List[float] = []
    for i, (sid, _, _) in enumerate(scenes):
        clip = WORK / f"{sid}.mp4"
        pad_tail = 0.70 if i == len(scenes) - 1 else 0.55
        cdur = durations[i] + pad_tail
        clip_durs.append(cdur)
        subprocess.run(
            [
                FFMPEG, "-y", "-hide_banner", "-loglevel", "error",
                "-loop", "1", "-framerate", "60", "-t", f"{cdur:.3f}", "-i", str(png_files[i]),
                "-i", str(audio_files[i]),
                "-vf", "scale=1920:1080:flags=lanczos,format=yuv420p",
                "-af", f"apad=pad_dur={pad_tail:.3f}",
                "-c:v", "libx264", "-preset", "veryfast", "-tune", "stillimage", "-crf", "18", "-r", "60",
                "-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-ac", "2",
                "-t", f"{cdur:.3f}",
                str(clip),
            ],
            check=True,
        )
        clip_paths.append(clip)

    # 4. Assemble all 6 clips with smooth 0.45s xfade transitions + acrossfade audio
    inputs: List[str] = []
    for cp in clip_paths:
        inputs += ["-i", str(cp)]

    v_filters: List[str] = []
    a_filters: List[str] = []
    offset = clip_durs[0] - fade
    prev_v = "[0:v]"
    prev_a = "[0:a]"
    for i in range(1, len(clip_paths)):
        out_v = f"[v{i}]" if i < len(clip_paths) - 1 else "[vout]"
        out_a = f"[a{i}]" if i < len(clip_paths) - 1 else "[aout]"
        v_filters.append(f"{prev_v}[{i}:v]xfade=transition=fade:duration={fade:.2f}:offset={offset:.3f}{out_v}")
        a_filters.append(f"{prev_a}[{i}:a]acrossfade=d={fade:.2f}:c1=tri:c2=tri{out_a}")
        prev_v = out_v
        prev_a = out_a
        offset += clip_durs[i] - fade

    fc = ";".join(v_filters + a_filters)
    OUT_MP4.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            FFMPEG, "-y", "-hide_banner", "-loglevel", "error",
            *inputs,
            "-filter_complex", fc,
            "-map", "[vout]", "-map", "[aout]",
            "-c:v", "libx264", "-preset", "veryfast", "-tune", "stillimage", "-crf", "19", "-pix_fmt", "yuv420p", "-r", "60",
            "-c:a", "aac", "-b:a", "192k", "-ar", "48000",
            "-movflags", "+faststart",
            str(OUT_MP4),
        ],
        check=True,
    )

    final_dur = _probe_duration(OUT_MP4)
    size_mb = OUT_MP4.stat().st_size / (1024 * 1024)
    print(f"[DONE] Rendered {OUT_MP4} | duration={final_dur:.2f}s ({int(final_dur)//60}:{int(final_dur)%60:02d}) | size={size_mb:.2f} MB")
    assert final_dur < 178.0, f"Video exceeded 3-minute Devpost cap ({final_dur:.2f}s)!"
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
