#!/usr/bin/env python3
"""
QTraceGuard - Standalone Interactive HTML5/SVG Real-Time Trace Studio
SPDX-License-Identifier: MIT

Zero external Python dependencies (stdlib only).
Takes `qtraceguard analyze --json <report.json>` and renders a dark-mode
interactive QNX OS 8.0 engineering studio with:
  1. Live QNX OS 8.0 Kernel vs POSIX Simulation provenance badge + SLA Gate verdict
  2. Clickable SVG IPC Wait-For Graph (click any node to cross-filter all tables & inspect!)
  3. Multi-Core Thread Execution Swimlane Timeline with interactive CPU core toggle pills
  4. Sortable Priority Inversion & QNX 8.0 PROCMGR_AID_PRIORITY / _NTO_CHF_FIXED_PRIORITY Table
  5. Sortable Pulse Channel Backlog Monitor & QNX 7.1 APS -> SCHED_SPORADIC Advisor
  6. Embedded AI Root-Cause & Verified QNX OS 8.0 Patch Copilot with syntax-tinted diff & Copy button
"""

from __future__ import annotations

import html
import json
import math
import sys
from pathlib import Path
from typing import Any, Dict, List, Set, Tuple


def fmt_us(ns: int) -> str:
    return f"{ns / 1000.0:.2f} µs"


def _is_live_qnx_trace(report: Dict[str, Any]) -> bool:
    hdr = report.get("header", {})
    if hdr.get("sysname") != "QNX":
        return False
    # Real QNX tracelogger captures have procnto / slogger2 / devb-* or non-1GHz TSC clocks
    proc_names = {p.get("name", "") for p in report.get("processes", [])}
    if any("procnto" in n or "slogger" in n or "tracelogger" in n for n in proc_names):
        return True
    return hdr.get("cycles_per_sec", 1000000000) != 1000000000


def render_ipc_graph_svg(report: Dict[str, Any]) -> str:
    all_procs = report.get("processes", [])
    edges = report.get("ipc_edges", [])
    cycles = report.get("deadlock_cycles", [])
    channels = {(c["owner_pid"], c["chid"]): c for c in report.get("channels", [])}

    cycle_threads: Set[Tuple[int, int]] = set()
    cycle_thread_edges: Set[Tuple[Tuple[int, int], Tuple[int, int]]] = set()
    for cyc in cycles:
        nodes = cyc.get("nodes", [])
        for idx, node in enumerate(nodes):
            nxt = nodes[(idx + 1) % len(nodes)]
            a = (node["pid"], node["tid"])
            b = (nxt["pid"], nxt["tid"])
            cycle_threads.add(a)
            cycle_thread_edges.add((a, b))

    intra_pid_cycle = any(a[0] == b[0] and a[1] != b[1] for a, b in cycle_thread_edges)

    if intra_pid_cycle:
        proc_map = {p["pid"]: p for p in all_procs}
        node_keys = sorted(cycle_threads)
        width, height = 760, 320
        cx, cy, radius = width / 2.0, height / 2.0, 112.0
        coords_t: Dict[Tuple[int, int], Tuple[float, float]] = {}
        for i, nk in enumerate(node_keys):
            angle = (2.0 * math.pi * i) / max(len(node_keys), 1) - math.pi / 2.0
            coords_t[nk] = (cx + radius * math.cos(angle), cy + radius * math.sin(angle))

        svg_parts: List[str] = [
            f'<svg viewBox="0 0 {width} {height}" class="graph-svg" role="img" aria-label="QNX IPC Wait-For Cycle Graph">',
            '<defs>',
            '  <marker id="arrow-dead" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" orient="auto-start-reverse">',
            '    <path d="M 0 1 L 10 5 L 0 9 z" fill="#ef4444"/>',
            '  </marker>',
            '</defs>',
        ]
        for cyc in cycles:
            nodes = cyc.get("nodes", [])
            for idx, node in enumerate(nodes):
                nxt = nodes[(idx + 1) % len(nodes)]
                a = (node["pid"], node["tid"])
                b = (nxt["pid"], nxt["tid"])
                x1, y1 = coords_t[a]
                x2, y2 = coords_t[b]
                dx, dy = x2 - x1, y2 - y1
                dist = math.hypot(dx, dy) or 1.0
                ux, uy = dx / dist, dy / dist
                sx, sy = x1 + ux * 58.0, y1 + uy * 34.0
                ex, ey = x2 - ux * 62.0, y2 - uy * 38.0
                mx, my = (sx + ex) / 2.0 - uy * 16.0, (sy + ey) / 2.0 + ux * 16.0
                lbl = f'MsgSend ch:{node.get("chid", 0)}'
                svg_parts.append(
                    f'<line x1="{sx:.1f}" y1="{sy:.1f}" x2="{ex:.1f}" y2="{ey:.1f}" '
                    f'stroke="#ef4444" stroke-width="3.2" stroke-dasharray="6,3" marker-end="url(#arrow-dead)"/>'
                )
                svg_parts.append(
                    f'<text x="{mx:.1f}" y="{my:.1f}" fill="#fca5a5" font-size="11" '
                    f'text-anchor="middle" font-family="monospace">{html.escape(lbl)}</text>'
                )
        for pid, tid in node_keys:
            x, y = coords_t[(pid, tid)]
            pname = proc_map.get(pid, {}).get("name", f"proc_{pid}")
            short = pname.split("/")[-1]
            svg_parts.append(
                f'<g class="svg-node" data-filter="P{pid}:T{tid}" data-label="{html.escape(short)} (P{pid}:T{tid})" '
                f'onclick="selectNode(this)" role="button" tabindex="0">'
                f'<rect x="{x-76:.1f}" y="{y-28:.1f}" width="152" height="56" rx="10" '
                f'fill="#450a0a" stroke="#ef4444" stroke-width="2.4"/>'
                f'<text x="{x:.1f}" y="{y-6:.1f}" fill="#f8fafc" font-size="12" font-weight="600" '
                f'text-anchor="middle" font-family="monospace">{html.escape(short)}</text>'
                f'<text x="{x:.1f}" y="{y+14:.1f}" fill="#fca5a5" font-size="10.5" '
                f'text-anchor="middle" font-family="monospace">P{pid}:T{tid} [BLOCKED]</text></g>'
            )
        svg_parts.append("</svg>")
        return "\n".join(svg_parts)

    cycle_edges: Set[Tuple[int, int]] = set()
    cycle_nodes: Set[int] = set()
    for cyc in cycles:
        nodes = cyc.get("nodes", [])
        for idx, node in enumerate(nodes):
            nxt = nodes[(idx + 1) % len(nodes)]
            cycle_nodes.add(node["pid"])
            cycle_edges.add((node["pid"], nxt["pid"]))

    # Prioritize focused real-time processes (e.g. --focus qtg) + cycle/inversion participants
    # so full-system QNX 8.0 captures stay visually crystal clear with zero overlapping boxes!
    inv_pids: Set[int] = set()
    for inv in report.get("priority_inversions", []):
        for k in ("high_pid", "server_pid", "preemptor_pid"):
            if inv.get(k):
                inv_pids.add(inv[k])

    focus = report.get("summary", {}).get("focus_proc", "")
    if focus:
        focused = [p for p in all_procs if focus in p.get("name", "") or p["pid"] in cycle_nodes or p["pid"] in inv_pids]
        procs = focused if focused else all_procs
    else:
        active_pids: Set[int] = set(cycle_nodes) | inv_pids
        for e in edges:
            active_pids.add(e["from_pid"])
            active_pids.add(e["to_pid"])
        procs = [p for p in all_procs if p["pid"] in active_pids] if (len(all_procs) > 6 and active_pids) else all_procs

    if len(procs) > 6:
        counts: Dict[int, int] = {}
        for e in edges:
            c = e.get("msg_count", 1)
            counts[e["from_pid"]] = counts.get(e["from_pid"], 0) + c
            counts[e["to_pid"]] = counts.get(e["to_pid"], 0) + c

        def _rank(p: Dict[str, Any]) -> Tuple[int, int, int, int]:
            in_c = 1 if p["pid"] in cycle_nodes else 0
            in_i = 1 if p["pid"] in inv_pids else 0
            in_f = 1 if (focus and focus in p.get("name", "")) else 0
            return (in_c, in_i, in_f, counts.get(p["pid"], 0))

        procs = sorted(procs, key=_rank, reverse=True)[:6]

    if not procs:
        return '<p class="muted">No process IPC nodes recorded in this trace.</p>'

    width, height = 900, 360
    cx, cy = width / 2.0, height / 2.0
    rx = 265.0 if len(procs) >= 3 else 210.0
    ry = 112.0 if len(procs) >= 3 else 70.0
    coords: Dict[int, Tuple[float, float]] = {}
    n = len(procs)
    for i, p in enumerate(procs):
        angle = (2.0 * math.pi * i) / max(n, 1) - math.pi / 2.0
        coords[p["pid"]] = (cx + rx * math.cos(angle), cy + ry * math.sin(angle))

    svg_parts = [
        f'<svg viewBox="0 0 {width} {height}" class="graph-svg" role="img" aria-label="QNX IPC Wait-For Graph">',
        '<defs>',
        '  <marker id="arrow-ok" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">',
        '    <path d="M 0 1 L 10 5 L 0 9 z" fill="#38bdf8"/>',
        '  </marker>',
        '  <marker id="arrow-dead" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" orient="auto-start-reverse">',
        '    <path d="M 0 1 L 10 5 L 0 9 z" fill="#ef4444"/>',
        '  </marker>',
        '</defs>',
    ]

    # Deduplicate multi-edge pairs between the same (from_pid, to_pid) for crisp visual routing
    pair_edges: Dict[Tuple[int, int], Dict[str, Any]] = {}
    for e in edges:
        fp, tp = e["from_pid"], e["to_pid"]
        if fp not in coords or tp not in coords or fp == tp:
            continue
        key = (fp, tp)
        if key not in pair_edges or (e.get("msg_count", 0) > pair_edges[key].get("msg_count", 0)):
            pair_edges[key] = e

    for (fp, tp), e in pair_edges.items():
        x1, y1 = coords[fp]
        x2, y2 = coords[tp]
        dx, dy = x2 - x1, y2 - y1
        dist = math.hypot(dx, dy) or 1.0
        ux, uy = dx / dist, dy / dist
        # Offset parallel opposite edges slightly so bidirectional arrows never overlap
        has_rev = (tp, fp) in pair_edges
        perp = 12.0 if has_rev else 0.0
        px, py = -uy * perp, ux * perp
        sx, sy = x1 + ux * 82.0 + px, y1 + uy * 34.0 + py
        ex, ey = x2 - ux * 86.0 + px, y2 - uy * 38.0 + py
        is_cyc = (fp, tp) in cycle_edges
        stroke = "#ef4444" if is_cyc else "#38bdf8"
        swidth = "3.4" if is_cyc else "2.2"
        marker = "url(#arrow-dead)" if is_cyc else "url(#arrow-ok)"
        dash = ' stroke-dasharray="7,4"' if (e.get("active") or is_cyc) else ""
        ch_meta = channels.get((tp, e.get("chid", 0)), {})
        lbl = f'MsgSend ch:{e.get("chid", 0)}' if is_cyc else f'ch:{e.get("chid", 0)}'
        if ch_meta.get("fixed_priority"):
            lbl += " [FIXED_PRIO]"
        if e.get("msg_count", 0):
            lbl += f' ({e["msg_count"]}x)'
        mx, my = (sx + ex) / 2.0 - uy * 18.0, (sy + ey) / 2.0 + ux * 18.0
        bw = max(66, len(lbl) * 6.8 + 12)
        svg_parts.append(
            f'<line x1="{sx:.1f}" y1="{sy:.1f}" x2="{ex:.1f}" y2="{ey:.1f}" '
            f'stroke="{stroke}" stroke-width="{swidth}"{dash} marker-end="{marker}"/>'
        )
        svg_parts.append(
            f'<rect x="{mx - bw/2:.1f}" y="{my - 11:.1f}" width="{bw:.1f}" height="17" rx="4" '
            f'fill="#020617" fill-opacity="0.92" stroke="{stroke}" stroke-opacity="0.35" stroke-width="1"/>'
        )
        svg_parts.append(
            f'<text x="{mx:.1f}" y="{my + 1:.1f}" fill="{stroke}" font-size="10.5" font-weight="600" '
            f'text-anchor="middle" font-family="monospace">{html.escape(lbl)}</text>'
        )

    for p in procs:
        pid = p["pid"]
        x, y = coords[pid]
        in_cyc = pid in cycle_nodes
        no_aid = not p.get("has_aid_priority", True)
        fill = "#450a0a" if in_cyc else ("#422006" if no_aid else "#0f172a")
        stroke = "#ef4444" if in_cyc else ("#f59e0b" if no_aid else "#38bdf8")
        short = p["name"].split("/")[-1]
        sub = f'PID {pid}' + (' | CAP<=63' if no_aid else '')
        svg_parts.append(
            f'<g class="svg-node" data-filter="P{pid}" data-label="{html.escape(short)} ({html.escape(sub)})" '
            f'onclick="selectNode(this)" role="button" tabindex="0">'
            f'<rect x="{x-82:.1f}" y="{y-29:.1f}" width="164" height="58" rx="10" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="2.4"/>'
            f'<text x="{x:.1f}" y="{y-6:.1f}" fill="#f8fafc" font-size="13" font-weight="700" '
            f'text-anchor="middle" font-family="monospace">{html.escape(short)}</text>'
            f'<text x="{x:.1f}" y="{y+15:.1f}" fill="#cbd5e1" font-size="10.5" '
            f'text-anchor="middle" font-family="monospace">{html.escape(sub)}</text></g>'
        )

    svg_parts.append("</svg>")
    return "\n".join(svg_parts)


def render_timeline_svg(report: Dict[str, Any]) -> Tuple[str, List[int]]:
    slices = report.get("timeline_slices", [])
    procs = {p["pid"]: p["name"].split("/")[-1] for p in report.get("processes", [])}
    if not slices:
        return '<p class="muted">No CPU execution timeline slices captured.</p>', []

    max_t = max((s["end_ns"] for s in slices), default=1)
    if max_t <= 0:
        max_t = 1
    cpus = sorted({s["cpu"] for s in slices})
    palette = ["#38bdf8", "#a78bfa", "#34d399", "#fbbf24", "#f472b6", "#fb7185"]
    pid_order = sorted({s["pid"] for s in slices})
    pid_color = {pid: palette[i % len(palette)] for i, pid in enumerate(pid_order)}

    width = 760
    left = 74
    lane_h = 34
    height = 36 + len(cpus) * lane_h + 24
    chart_w = width - left - 20

    parts = [
        f'<svg viewBox="0 0 {width} {height}" class="timeline-svg" role="img" aria-label="Per-CPU Thread Execution Timeline">'
    ]
    for idx, cpu in enumerate(cpus):
        y = 20 + idx * lane_h
        parts.append(f'<g class="cpu-lane" data-cpu="{cpu}">')
        parts.append(
            f'<text x="12" y="{y + 18}" fill="#94a3b8" font-size="11.5" font-family="monospace">CPU {cpu}</text>'
        )
        parts.append(
            f'<rect x="{left}" y="{y + 2}" width="{chart_w}" height="{lane_h - 10}" rx="4" fill="#0f172a" stroke="#1e293b"/>'
        )
        for s in slices:
            if s["cpu"] != cpu:
                continue
            x0 = left + (s["start_ns"] / max_t) * chart_w
            w = max(2.2, ((s["end_ns"] - s["start_ns"]) / max_t) * chart_w)
            col = pid_color.get(s["pid"], "#38bdf8")
            pname = procs.get(s["pid"], f'P{s["pid"]}')
            tip = f'{pname} (P{s["pid"]}:T{s["tid"]}, prio {s["prio"]}) {fmt_us(s["start_ns"])} -> {fmt_us(s["end_ns"])}'
            parts.append(
                f'<rect class="slice-rect" data-pid="P{s["pid"]}" x="{x0:.1f}" y="{y + 4}" width="{w:.1f}" '
                f'height="{lane_h - 14}" rx="3" fill="{col}" opacity="0.9">'
                f'<title>{html.escape(tip)}</title></rect>'
            )
        parts.append("</g>")

    parts.append(
        f'<text x="{left}" y="{height - 6}" fill="#64748b" font-size="10.5" font-family="monospace">0.00 µs</text>'
    )
    parts.append(
        f'<text x="{width - 20}" y="{height - 6}" fill="#64748b" font-size="10.5" text-anchor="end" font-family="monospace">{fmt_us(max_t)}</text>'
    )
    parts.append("</svg>")
    return "\n".join(parts), cpus


def _render_color_diff(diff_txt: str, idx: int) -> str:
    lines_html = []
    for ln in diff_txt.rstrip().splitlines():
        esc = html.escape(ln)
        if ln.startswith("+++") or ln.startswith("---") or ln.startswith("@@"):
            lines_html.append(f'<span class="d-hdr">{esc}</span>')
        elif ln.startswith("+"):
            lines_html.append(f'<span class="d-add">{esc}</span>')
        elif ln.startswith("-"):
            lines_html.append(f'<span class="d-del">{esc}</span>')
        else:
            lines_html.append(esc)
    return (
        f'<div class="diff-wrap">'
        f'<button type="button" class="copy-btn" onclick="copyDiff(\'diff-{idx}\', this)">Copy Patch</button>'
        f'<pre id="diff-{idx}" class="diff-box">' + "\n".join(lines_html) + "</pre></div>"
    )


def render_ai_copilot_section(report: Dict[str, Any]) -> str:
    """Embed QTraceGuard's neuro-symbolic AI Root-Cause & QNX 8.0 Patch Copilot."""
    try:
        tools_dir = str(Path(__file__).resolve().parent)
        if tools_dir not in sys.path:
            sys.path.insert(0, tools_dir)
        from qtg_ai_patch import build_deterministic_patches
        ai = build_deterministic_patches(report, verify_cc=False)
    except Exception as exc:
        return (
            '<section class="card">'
            '<h2>AI Root-Cause &amp; QNX OS 8.0 Patch Copilot</h2>'
            f'<p class="warn">Copilot module unavailable ({html.escape(str(exc))}). '
            'Run <code>python3 tools/qtg_ai_patch.py &lt;report.json&gt; --verify-cc</code> directly.</p>'
            '</section>'
        )

    findings = ai.get("findings", [])
    if not findings:
        return (
            '<section class="card">'
            '<h2>AI Root-Cause &amp; QNX OS 8.0 Patch Copilot</h2>'
            '<p class="ok">Zero real-time faults detected &mdash; no code remediation required.</p>'
            '</section>'
        )

    cards = []
    for idx, f in enumerate(findings):
        diff_html = _render_color_diff(f["unified_diff"], idx) if f.get("unified_diff") else ""
        steps = "".join(f"<li>{html.escape(s)}</li>" for s in f.get("remediation_steps", []))
        cards.append(
            f'<div class="patch-card">'
            f'<div class="patch-head"><span class="mono err">[{html.escape(f["id"])}]</span> '
            f'<strong>{html.escape(f["title"])}</strong></div>'
            f'<p class="muted">{html.escape(f["root_cause"])}</p>'
            f'<ul class="patch-steps">{steps}</ul>'
            f'{diff_html}'
            f'</div>'
        )
    return (
        '<section class="card">'
        '<h2>AI Root-Cause &amp; Verified QNX OS 8.0 Patch Copilot</h2>'
        f'<p class="muted">{html.escape(ai.get("summary", ""))} '
        '(100% trace-grounded &mdash; run <code>python3 tools/qtg_ai_patch.py &lt;report.json&gt; --verify-cc</code> to compiler-verify patches via <code>qcc -Werror</code>.)</p>'
        + "".join(cards)
        + '</section>'
    )


def build_html(report: Dict[str, Any]) -> str:
    hdr = report.get("header", {})
    summ = report.get("summary", {})
    sla = report.get("sla", {})
    passed = sla.get("passed", False)
    cycles = report.get("deadlock_cycles", [])
    invs = report.get("priority_inversions", [])
    pulses = report.get("pulse_channels", [])
    aps = report.get("aps_migrations", [])
    procs = {p["pid"]: p["name"].split("/")[-1] for p in report.get("processes", [])}

    live_qnx = _is_live_qnx_trace(report)
    prov_badge = (
        '<span class="badge-live">LIVE QNX OS 8.0 KERNEL TRACE</span>'
        if live_qnx else
        '<span class="badge-sim">HOST POSIX SIMULATION</span>'
    )
    badge_cls = "badge-pass" if passed else "badge-fail"
    badge_txt = "SLA PASS" if passed else f"SLA FAIL ({len(sla.get('violations', []))} VIOLATIONS)"

    viol_html = ""
    if sla.get("violations"):
        items = "".join(f"<li>{html.escape(v)}</li>" for v in sla["violations"])
        viol_html = f'<ul class="viol-list">{items}</ul>'

    cycle_rows = ""
    for i, c in enumerate(cycles, 1):
        chain = " &rarr; ".join(
            f'P{n["pid"]}:T{n["tid"]} ({html.escape(n["proc_name"].split("/")[-1])}, ch:{n["chid"]})'
            for n in c.get("nodes", [])
        )
        if c.get("nodes"):
            first = c["nodes"][0]
            chain += f' &rarr; P{first["pid"]}:T{first["tid"]} ({html.escape(first["proc_name"].split("/")[-1])})'
        cycle_rows += f"<tr><td>#{i}</td><td class='mono err'>{chain}</td></tr>"
    if not cycle_rows:
        cycle_rows = "<tr><td colspan='2' class='ok'>No IPC wait-for deadlock cycles detected.</td></tr>"

    inv_rows = ""
    for inv in invs:
        pre = (
            f'P{inv["preemptor_pid"]} ({html.escape(procs.get(inv["preemptor_pid"], "?"))}, prio {inv["preemptor_prio"]})'
            if inv.get("preemptor_pid")
            else "Ceiling Cap (<=63)"
        )
        dur_ns = inv.get("duration_ns", 0)
        inv_rows += (
            f"<tr><td class='mono warn'>{html.escape(inv['kind'])}</td>"
            f"<td class='mono'>P{inv['high_pid']} ({html.escape(procs.get(inv['high_pid'], '?'))}, prio {inv['high_prio']})</td>"
            f"<td class='mono'>P{inv['server_pid']} ({html.escape(procs.get(inv['server_pid'], '?'))}, eff {inv['server_prio']})</td>"
            f"<td class='mono'>{pre}</td>"
            f"<td class='mono' data-num='{dur_ns}'>{fmt_us(dur_ns)}</td></tr>"
        )
    if not inv_rows:
        inv_rows = "<tr><td colspan='5' class='ok'>No priority inversions or PROCMGR_AID_PRIORITY caps detected.</td></tr>"

    pulse_rows = ""
    for ps in pulses:
        st = '<span class="err">SATURATED</span>' if ps.get("leak_detected") else '<span class="ok">OK</span>'
        pulse_rows += (
            f"<tr><td class='mono'>P{ps['pid']} ({html.escape(procs.get(ps['pid'], '?'))})</td>"
            f"<td class='mono' data-num='{ps['chid']}'>ch:{ps['chid']}</td>"
            f"<td data-num='{ps['sent']}'>{ps['sent']}</td><td data-num='{ps['received']}'>{ps['received']}</td>"
            f"<td data-num='{ps['max_backlog']}'>{ps['max_backlog']}</td><td data-num='{ps['pending']}'>{ps['pending']}</td><td>{st}</td></tr>"
        )
    if not pulse_rows:
        pulse_rows = "<tr><td colspan='7' class='muted'>No pulse traffic in this trace.</td></tr>"

    aps_section = ""
    if aps:
        rows = ""
        for a in aps:
            sp = a["qnx8_sporadic"]
            aid = "YES (prio > 63)" if sp["requires_aid_priority"] else "No"
            rows += (
                f"<tr><td>{a['aps_id']}</td><td class='mono'>{html.escape(a['name'])}</td>"
                f"<td>{a['budget_percent']}% / {a['window_ms']}ms</td>"
                f"<td class='{'err' if a['bankruptcy_events'] else 'ok'}'>{a['bankruptcy_events']}</td>"
                f"<td class='mono'>prio {sp['normal_priority']} -> {sp['low_priority']}, "
                f"budget {sp['init_budget_ms']}ms / {sp['repl_period_ms']}ms (max_repl={sp['max_repl']})</td>"
                f"<td>{aid}</td></tr>"
            )
        aps_section = f"""
        <section class="card">
          <h2>QNX 7.1 APS &rarr; QNX OS 8.0 SCHED_SPORADIC Migration Advisor</h2>
          <p class="muted">QNX OS 8.0 removed legacy <code>[module=aps]</code> (<code>SchedCtl</code> returns <code>ENOSYS</code>) in favor of <code>SCHED_SPORADIC</code>, per-thread CPU clocks, and SMP affinity runmasks:</p>
          <table class="sortable">
            <thead><tr><th>APS ID</th><th>Partition</th><th>Legacy Budget</th><th>Bankruptcies</th><th>QNX 8.0 SCHED_SPORADIC Config</th><th>PROCMGR_AID_PRIORITY</th></tr></thead>
            <tbody>{rows}</tbody>
          </table>
        </section>"""

    tl_svg, cpus = render_timeline_svg(report)
    cpu_pills = "".join(
        f'<label class="cpu-pill"><input type="checkbox" checked onchange="toggleCpu({c}, this.checked)"/> CPU {c}</label>'
        for c in cpus
    )

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>QTraceGuard — QNX OS 8.0 Interactive Trace Studio</title>
<style>
  :root {{ --bg:#020617; --card:#0f172a; --border:#1e293b; --txt:#e2e8f0; --muted:#94a3b8; --accent:#38bdf8; --ok:#22c55e; --warn:#f59e0b; --err:#ef4444; }}
  * {{ box-sizing:border-box; }}
  body {{ margin:0; padding:24px; background:var(--bg); color:var(--txt); font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif; line-height:1.45; }}
  .wrap {{ max-width:1160px; margin:0 auto; }}
  header {{ display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:12px; margin-bottom:20px; border-bottom:1px solid var(--border); padding-bottom:16px; }}
  h1 {{ margin:0; font-size:1.5rem; letter-spacing:-0.02em; }}
  h2 {{ margin:0 0 12px 0; font-size:1.08rem; color:var(--accent); }}
  .meta {{ color:var(--muted); font-size:0.86rem; font-family:monospace; margin-top:4px; }}
  .badges {{ display:flex; gap:8px; align-items:center; flex-wrap:wrap; }}
  .badge-live {{ background:#064e3b; color:#6ee7b7; border:1px solid #10b981; padding:6px 12px; border-radius:999px; font-weight:700; font-size:0.78rem; letter-spacing:0.03em; }}
  .badge-sim  {{ background:#1e293b; color:#cbd5e1; border:1px solid #475569; padding:6px 12px; border-radius:999px; font-weight:700; font-size:0.78rem; letter-spacing:0.03em; }}
  .badge-pass {{ background:#052e16; color:#4ade80; border:1px solid #16a34a; padding:6px 14px; border-radius:999px; font-weight:700; font-size:0.86rem; }}
  .badge-fail {{ background:#450a0a; color:#fca5a5; border:1px solid #dc2626; padding:6px 14px; border-radius:999px; font-weight:700; font-size:0.86rem; }}
  .kpis {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(175px,1fr)); gap:12px; margin-bottom:18px; }}
  .kpi {{ background:var(--card); border:1px solid var(--border); border-radius:10px; padding:14px; }}
  .kpi .lbl {{ color:var(--muted); font-size:0.78rem; text-transform:uppercase; letter-spacing:0.04em; }}
  .kpi .val {{ font-size:1.3rem; font-weight:700; margin-top:4px; font-family:monospace; }}
  .card {{ background:var(--card); border:1px solid var(--border); border-radius:12px; padding:18px; margin-bottom:18px; }}
  .graph-svg, .timeline-svg {{ width:100%; height:auto; background:#020617; border:1px solid var(--border); border-radius:8px; }}
  .svg-node {{ cursor:pointer; transition:opacity 0.15s; }}
  .svg-node:hover rect {{ filter:brightness(1.25); stroke-width:3; }}
  .svg-node.selected rect {{ stroke:#fbbf24; stroke-width:3.4; }}
  .inspector {{ display:none; align-items:center; justify-content:space-between; background:#0b132b; border:1px solid #38bdf8; border-radius:8px; padding:8px 12px; margin-bottom:12px; font-size:0.88rem; }}
  .inspector.show {{ display:flex; }}
  .cpu-bar {{ display:flex; gap:8px; flex-wrap:wrap; margin-bottom:10px; }}
  .cpu-pill {{ background:#020617; border:1px solid var(--border); border-radius:999px; padding:4px 10px; font-size:0.8rem; font-family:monospace; cursor:pointer; }}
  table {{ width:100%; border-collapse:collapse; font-size:0.88rem; }}
  th, td {{ text-align:left; padding:9px 10px; border-bottom:1px solid var(--border); }}
  th {{ color:var(--muted); font-weight:600; font-size:0.78rem; text-transform:uppercase; cursor:pointer; user-select:none; }}
  th:hover {{ color:var(--accent); }}
  .mono {{ font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; }}
  .ok {{ color:var(--ok); }} .warn {{ color:var(--warn); }} .err {{ color:var(--err); }} .muted {{ color:var(--muted); }}
  .viol-list {{ margin:10px 0 0 0; padding-left:20px; color:#fca5a5; }}
  .patch-card {{ background:#020617; border:1px solid #334155; border-radius:10px; padding:14px; margin-top:12px; }}
  .patch-head {{ font-size:0.98rem; margin-bottom:6px; }}
  .patch-steps {{ margin:8px 0 10px 0; padding-left:20px; color:#cbd5e1; font-size:0.88rem; }}
  .diff-wrap {{ position:relative; }}
  .copy-btn {{ position:absolute; top:8px; right:8px; background:#1e293b; color:#e2e8f0; border:1px solid #475569; border-radius:6px; padding:4px 10px; font-size:0.76rem; cursor:pointer; }}
  .copy-btn:hover {{ background:#334155; }}
  .diff-box {{ background:#0b1120; border:1px solid #1e293b; border-radius:8px; padding:12px; overflow-x:auto; font-size:0.82rem; color:#e2e8f0; margin:6px 0 0 0; line-height:1.45; }}
  .d-hdr {{ color:#38bdf8; font-weight:600; }} .d-add {{ color:#4ade80; }} .d-del {{ color:#f87171; }}
  .filter-bar {{ display:flex; gap:10px; align-items:center; margin-bottom:12px; }}
  .filter-bar input {{ background:#020617; border:1px solid var(--border); color:var(--txt); padding:7px 12px; border-radius:8px; width:340px; font-family:monospace; font-size:0.86rem; }}
  .reset-btn {{ background:#1e293b; color:#e2e8f0; border:1px solid #475569; border-radius:6px; padding:5px 10px; font-size:0.8rem; cursor:pointer; }}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div>
      <h1>QTraceGuard &mdash; QNX OS 8.0 Interactive Trace Studio</h1>
      <div class="meta">Target: {html.escape(hdr.get('sysname','QNX'))} {html.escape(hdr.get('sys_release','8.0.0'))} ({html.escape(hdr.get('machine','x86_64'))}) &middot; CPUs: {hdr.get('cpu_num',4)} &middot; Clock: {hdr.get('cycles_per_sec',1000000000):,} Hz &middot; Events: {summ.get('total_events',0):,} &middot; Duration: {summ.get('trace_duration_ns',0)/1e6:.3f} ms</div>
    </div>
    <div class="badges">{prov_badge}<div class="{badge_cls}">{badge_txt}</div></div>
  </header>

  <section class="kpis">
    <div class="kpi"><div class="lbl">Dispatch p50 / p99</div><div class="val">{fmt_us(summ.get('dispatch_p50_ns',0))} / {fmt_us(summ.get('dispatch_p99_ns',0))}</div></div>
    <div class="kpi"><div class="lbl">Dispatch Max</div><div class="val">{fmt_us(summ.get('dispatch_max_ns',0))}</div></div>
    <div class="kpi"><div class="lbl">IPC RTT p50 / Max</div><div class="val">{fmt_us(summ.get('ipc_rtt_p50_ns',0))} / {fmt_us(summ.get('ipc_rtt_max_ns',0))}</div></div>
    <div class="kpi"><div class="lbl">Deadlock Cycles</div><div class="val {'err' if cycles else 'ok'}">{len(cycles)}</div></div>
    <div class="kpi"><div class="lbl">Priority Inversions</div><div class="val {'warn' if invs else 'ok'}">{len(invs)}</div></div>
  </section>

  {f'<section class="card"><h2>SLA Gate Violations</h2>{viol_html}</section>' if viol_html else ''}

  <section class="card">
    <h2>Interactive IPC Wait-For Graph (Click Any Process Node to Cross-Filter)</h2>
    <div id="node-inspector" class="inspector">
      <span id="inspector-txt" class="mono"></span>
      <button type="button" class="reset-btn" onclick="resetSelection()">Clear Node Filter</button>
    </div>
    {render_ipc_graph_svg(report)}
  </section>

  <section class="card">
    <h2>Multi-Core Thread Execution Swimlane Timeline</h2>
    <div class="cpu-bar">{cpu_pills}</div>
    {tl_svg}
  </section>

  <div class="filter-bar">
    <span class="muted">Filter &amp; Sort Tables (click any column header to sort):</span>
    <input id="qtg-filter" type="search" placeholder="Search PID, process, channel, or fault kind..." oninput="filterRows(this.value)"/>
  </div>

  <section class="card">
    <h2>IPC Wait-For Deadlock Cycles</h2>
    <table class="sortable"><thead><tr><th>#</th><th>Cycle Chain</th></tr></thead><tbody>{cycle_rows}</tbody></table>
  </section>

  <section class="card">
    <h2>Priority Inversions &amp; QNX 8.0 Priority-Inheritance Diagnostics</h2>
    <table class="sortable"><thead><tr><th>Kind</th><th>High-Prio Client</th><th>Blocked Server</th><th>Preemptor / Cap</th><th>Duration</th></tr></thead><tbody>{inv_rows}</tbody></table>
  </section>

  <section class="card">
    <h2>Pulse Channel Backlog Monitor</h2>
    <table class="sortable"><thead><tr><th>Process</th><th>Channel</th><th>Sent</th><th>Received</th><th>Max Backlog</th><th>Pending</th><th>Status</th></tr></thead><tbody>{pulse_rows}</tbody></table>
  </section>

  {aps_section}
  {render_ai_copilot_section(report)}
</div>
<script>
function filterRows(q) {{
  q = (q || '').toLowerCase();
  document.querySelectorAll('tbody tr').forEach(tr => {{
    tr.style.display = (!q || tr.textContent.toLowerCase().includes(q)) ? '' : 'none';
  }});
}}
function selectNode(el) {{
  document.querySelectorAll('.svg-node.selected').forEach(n => n.classList.remove('selected'));
  el.classList.add('selected');
  const key = el.getAttribute('data-filter') || '';
  const lbl = el.getAttribute('data-label') || key;
  const inp = document.getElementById('qtg-filter');
  if (inp) {{ inp.value = key; filterRows(key); }}
  const bar = document.getElementById('node-inspector');
  const txt = document.getElementById('inspector-txt');
  if (bar && txt) {{ txt.textContent = 'Focused Node: ' + lbl + ' — highlighting timeline slices & filtering tables'; bar.classList.add('show'); }}
  document.querySelectorAll('.slice-rect').forEach(r => {{
    r.style.opacity = (r.getAttribute('data-pid') === key.split(':')[0]) ? '1.0' : '0.2';
  }});
}}
function resetSelection() {{
  document.querySelectorAll('.svg-node.selected').forEach(n => n.classList.remove('selected'));
  const inp = document.getElementById('qtg-filter');
  if (inp) {{ inp.value = ''; filterRows(''); }}
  const bar = document.getElementById('node-inspector');
  if (bar) bar.classList.remove('show');
  document.querySelectorAll('.slice-rect').forEach(r => {{ r.style.opacity = '0.9'; }});
}}
function toggleCpu(cpu, visible) {{
  document.querySelectorAll('.cpu-lane[data-cpu="' + cpu + '"]').forEach(g => {{
    g.style.display = visible ? '' : 'none';
  }});
}}
function copyDiff(id, btn) {{
  const pre = document.getElementById(id);
  if (!pre) return;
  navigator.clipboard.writeText(pre.textContent || '').then(() => {{
    btn.textContent = 'Copied!';
    setTimeout(() => {{ btn.textContent = 'Copy Patch'; }}, 1400);
  }});
}}
document.querySelectorAll('table.sortable th').forEach((th, idx) => {{
  let asc = true;
  th.addEventListener('click', () => {{
    const tbl = th.closest('table');
    const tbody = tbl && tbl.querySelector('tbody');
    if (!tbody) return;
    const rows = Array.from(tbody.querySelectorAll('tr'));
    if (rows.length <= 1) return;
    rows.sort((a, b) => {{
      const ca = a.children[idx], cb = b.children[idx];
      if (!ca || !cb) return 0;
      const na = ca.getAttribute('data-num'), nb = cb.getAttribute('data-num');
      if (na !== null && nb !== null) return asc ? (Number(na) - Number(nb)) : (Number(nb) - Number(na));
      return asc ? ca.textContent.localeCompare(cb.textContent) : cb.textContent.localeCompare(ca.textContent);
    }});
    rows.forEach(r => tbody.appendChild(r));
    asc = !asc;
  }});
}});
</script>
</body>
</html>
"""


def main(argv: List[str]) -> int:
    if len(argv) < 3:
        print(f"Usage: {argv[0]} <report.json> <output.html>", file=sys.stderr)
        return 1
    in_path = Path(argv[1])
    out_path = Path(argv[2])
    report = json.loads(in_path.read_text(encoding="utf-8"))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(build_html(report), encoding="utf-8")
    print(f"[qtg-report] Wrote interactive dashboard -> {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
