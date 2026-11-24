#!/usr/bin/env python3
"""
QTraceGuard - End-to-End CLI, JSON/SARIF Schema, HTML Dashboard & CI Harness Tests
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN_QTG = ROOT / "build" / "qtraceguard"
BIN_PROBE = ROOT / "build" / "qtg-probe"
BIN_RESMGR = ROOT / "build" / "qtg-resmgr-node"
REPORT_PY = ROOT / "tools" / "qtg_report.py"
AI_PATCH_PY = ROOT / "tools" / "qtg_ai_patch.py"
HARNESS_SH = ROOT / "qemu" / "qnx800_harness.sh"
POLICY = ROOT / "qemu" / "sla_policy.json"
LIVE_QNX8_DIR = ROOT / "samples" / "qnx8_live"
OUT = ROOT / "out" / "e2e"


def run(cmd: list[str], expect_rc: int = 0) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    env["OMP_NUM_THREADS"] = "4"
    cp = subprocess.run(cmd, cwd=str(ROOT), env=env, capture_output=True, text=True)
    if cp.returncode != expect_rc:
        raise AssertionError(
            f"Command failed (expected rc={expect_rc}, got rc={cp.returncode}): {' '.join(cmd)}\n"
            f"STDOUT:\n{cp.stdout}\nSTDERR:\n{cp.stderr}"
        )
    return cp


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    passed = 0

    # 1. qtg-probe JSON output & APS ENOSYS check
    cp = run([str(BIN_PROBE), "--json", "--iters", "100", "--out", str(OUT / "probe.kev")])
    probe_data = json.loads(cp.stdout)
    assert probe_data["aps_supported"] is False, "QNX 8.0 model expects APS unsupported"
    assert probe_data["sporadic_supported"] is True, "SCHED_SPORADIC expected available"
    assert probe_data["ipc_bench"]["succeeded"] == 100
    print("  [PASS] e2e_qtg_probe_json_and_bench")
    passed += 1

    # 2. qtg-resmgr-node standalone binary
    run([str(BIN_RESMGR), "--scenario", "healthy", "--iters", "8", "--out", str(OUT / "standalone.kev")])
    run([str(BIN_QTG), "check-sla", str(OUT / "standalone.kev"), "--policy", str(POLICY), "--no-color"], expect_rc=0)
    print("  [PASS] e2e_qtg_resmgr_node_standalone")
    passed += 1

    # 3. CLI inspect subcommand
    cp = run([str(BIN_QTG), "inspect", str(OUT / "standalone.kev"), "--limit", "15"])
    assert "TRACE_CYCLES_PER_SEC" in cp.stdout and "PROCCREATE_NAME" in cp.stdout
    print("  [PASS] e2e_cli_inspect_kev")
    passed += 1

    # 4. Deadlock check-sla exits 2 + valid JSON + valid SARIF 2.1.0 + HTML SVG cycle highlight
    run([str(BIN_QTG), "generate-demo", "deadlock", str(OUT / "deadlock.kev")])
    run(
        [
            str(BIN_QTG), "check-sla", str(OUT / "deadlock.kev"),
            "--policy", str(POLICY),
            "--json", str(OUT / "deadlock.json"),
            "--sarif", str(OUT / "deadlock.sarif"),
            "--no-color",
        ],
        expect_rc=2,
    )
    dl_json = json.loads((OUT / "deadlock.json").read_text(encoding="utf-8"))
    assert dl_json["sla"]["passed"] is False
    assert len(dl_json["deadlock_cycles"]) == 1
    assert len(dl_json["deadlock_cycles"][0]["nodes"]) == 3

    sarif = json.loads((OUT / "deadlock.sarif").read_text(encoding="utf-8"))
    assert sarif["version"] == "2.1.0"
    assert sarif["runs"][0]["results"][0]["ruleId"] == "QTG001"

    run([sys.executable, str(REPORT_PY), str(OUT / "deadlock.json"), str(OUT / "deadlock.html")])
    html_txt = (OUT / "deadlock.html").read_text(encoding="utf-8")
    assert "SLA FAIL" in html_txt and "arrow-dead" in html_txt and "qtg_sensor_rm" in html_txt
    print("  [PASS] e2e_deadlock_json_sarif_html")
    passed += 1

    # 5. QNX 7.1 APS -> QNX 8.0 SCHED_SPORADIC migrate-aps codegen
    run([str(BIN_QTG), "generate-demo", "aps-legacy", str(OUT / "aps.kev"), "--iters", "4"])
    cp = run([str(BIN_QTG), "migrate-aps", str(OUT / "aps.kev"), "--json", str(OUT / "aps.json")])
    assert "SCHED_SPORADIC" in cp.stdout and "PROCMGR_AID_PRIORITY" in cp.stdout and "ADAS_Control" in cp.stdout
    print("  [PASS] e2e_migrate_aps_codegen")
    passed += 1

    # 6. CI regression diff (healthy vs prio-inversion => exit 2)
    run([str(BIN_QTG), "generate-demo", "prio-inversion", str(OUT / "prio.kev"), "--iters", "4"])
    cp = run([str(BIN_QTG), "diff", str(OUT / "standalone.kev"), str(OUT / "prio.kev")], expect_rc=2)
    assert "REGRESSION!" in cp.stdout
    print("  [PASS] e2e_ci_regression_diff")
    passed += 1

    # 7. QEMU harness --check & --sim-ci end-to-end
    cp = run(["bash", str(HARNESS_SH), "--check"], expect_rc=0)
    assert "QTraceGuard QNX OS 8.0 Pre-Flight Check" in cp.stdout
    cp = run(["bash", str(HARNESS_SH), "--sim-ci"], expect_rc=0)
    assert "ALL CI DETERMINISM GATES VERIFIED." in cp.stdout
    print("  [PASS] e2e_qemu_harness_check_and_sim_ci")
    passed += 1

    # 8. Bundled live QNX OS 8.0 healthy capture -> PASS (exit 0)
    live_healthy = LIVE_QNX8_DIR / "qnx8_healthy.kev"
    assert live_healthy.is_file(), f"Missing bundled live QNX 8.0 trace: {live_healthy}"
    run(
        [
            str(BIN_QTG), "check-sla", str(live_healthy),
            "--policy", str(POLICY), "--focus", "qtg",
            "--json", str(OUT / "live_healthy.json"), "--no-color",
        ],
        expect_rc=0,
    )
    lh = json.loads((OUT / "live_healthy.json").read_text(encoding="utf-8"))
    assert lh["header"]["sysname"] == "QNX" and lh["header"]["sys_release"] == "8.0.0"
    assert lh["sla"]["passed"] is True
    assert len(lh["deadlock_cycles"]) == 0
    assert len(lh["priority_inversions"]) == 0
    print("  [PASS] e2e_live_qnx8_healthy_trace")
    passed += 1

    # 9. Bundled live QNX OS 8.0 deadlock capture -> FAIL (exit 2) + AI Patch Copilot (--verify-cc) + HTML
    live_dl = LIVE_QNX8_DIR / "qnx8_deadlock.kev"
    run(
        [
            str(BIN_QTG), "check-sla", str(live_dl),
            "--policy", str(POLICY), "--focus", "qtg",
            "--json", str(OUT / "live_deadlock.json"),
            "--sarif", str(OUT / "live_deadlock.sarif"), "--no-color",
        ],
        expect_rc=2,
    )
    ldl = json.loads((OUT / "live_deadlock.json").read_text(encoding="utf-8"))
    assert ldl["sla"]["passed"] is False
    assert len(ldl["deadlock_cycles"]) == 1 and len(ldl["deadlock_cycles"][0]["nodes"]) == 3
    run(
        [
            sys.executable, str(AI_PATCH_PY), str(OUT / "live_deadlock.json"),
            "--out-md", str(OUT / "live_deadlock_patch.md"),
            "--out-json", str(OUT / "live_deadlock_patch.json"), "--verify-cc",
        ],
        expect_rc=0,
    )
    dl_patch = json.loads((OUT / "live_deadlock_patch.json").read_text(encoding="utf-8"))
    assert len(dl_patch["findings"]) >= 1
    assert all(f["compiler_verified"] is True for f in dl_patch["findings"])
    run([sys.executable, str(REPORT_PY), str(OUT / "live_deadlock.json"), str(OUT / "live_deadlock.html")])
    dl_html = (OUT / "live_deadlock.html").read_text(encoding="utf-8")
    assert "arrow-dead" in dl_html and "AI Root-Cause" in dl_html
    print("  [PASS] e2e_live_qnx8_deadlock_ai_patch_and_html")
    passed += 1

    # 10. Bundled live QNX OS 8.0 _NTO_CHF_FIXED_PRIORITY trap -> FAIL (exit 2) + verified AI patch
    live_pi = LIVE_QNX8_DIR / "qnx8_prio-inversion.kev"
    run(
        [
            str(BIN_QTG), "check-sla", str(live_pi),
            "--policy", str(POLICY), "--focus", "qtg",
            "--json", str(OUT / "live_prio.json"), "--no-color",
        ],
        expect_rc=2,
    )
    lpi = json.loads((OUT / "live_prio.json").read_text(encoding="utf-8"))
    assert len(lpi["priority_inversions"]) >= 1
    assert any(inv.get("kind") == "FIXED_PRIORITY_CHANNEL" for inv in lpi["priority_inversions"])
    run(
        [
            sys.executable, str(AI_PATCH_PY), str(OUT / "live_prio.json"),
            "--out-json", str(OUT / "live_prio_patch.json"), "--verify-cc",
        ],
        expect_rc=0,
    )
    pi_patch = json.loads((OUT / "live_prio_patch.json").read_text(encoding="utf-8"))
    assert len(pi_patch["findings"]) >= 1 and all(f["compiler_verified"] is True for f in pi_patch["findings"])
    print("  [PASS] e2e_live_qnx8_prio_inversion_fixed_trap")
    passed += 1

    # 11. Bundled live QNX OS 8.0 pulse-storm saturation -> FAIL (exit 2) + verified AI patch
    live_ps = LIVE_QNX8_DIR / "qnx8_pulse-storm.kev"
    run(
        [
            str(BIN_QTG), "check-sla", str(live_ps),
            "--policy", str(POLICY), "--focus", "qtg",
            "--json", str(OUT / "live_pulse.json"), "--no-color",
        ],
        expect_rc=2,
    )
    lps = json.loads((OUT / "live_pulse.json").read_text(encoding="utf-8"))
    assert any(p.get("leak_detected") is True for p in lps["pulse_channels"])
    run(
        [
            sys.executable, str(AI_PATCH_PY), str(OUT / "live_pulse.json"),
            "--out-json", str(OUT / "live_pulse_patch.json"), "--verify-cc",
        ],
        expect_rc=0,
    )
    ps_patch = json.loads((OUT / "live_pulse_patch.json").read_text(encoding="utf-8"))
    assert len(ps_patch["findings"]) >= 1 and all(f["compiler_verified"] is True for f in ps_patch["findings"])
    print("  [PASS] e2e_live_qnx8_pulse_storm_and_ai_patch")
    passed += 1

    # 12. Bundled live QNX OS 8.0 PROCMGR_AID_PRIORITY ceiling-63 capture -> FAIL (exit 2) + verified AI patch
    live_aid = LIVE_QNX8_DIR / "qnx8_aid-cap.kev"
    run(
        [
            str(BIN_QTG), "check-sla", str(live_aid),
            "--policy", str(POLICY), "--focus", "qtg",
            "--json", str(OUT / "live_aid.json"), "--no-color",
        ],
        expect_rc=2,
    )
    laid = json.loads((OUT / "live_aid.json").read_text(encoding="utf-8"))
    assert any(inv.get("kind") == "PROCMGR_AID_PRIORITY_CAP" for inv in laid["priority_inversions"])
    run(
        [
            sys.executable, str(AI_PATCH_PY), str(OUT / "live_aid.json"),
            "--out-json", str(OUT / "live_aid_patch.json"), "--verify-cc",
        ],
        expect_rc=0,
    )
    aid_patch = json.loads((OUT / "live_aid_patch.json").read_text(encoding="utf-8"))
    assert len(aid_patch["findings"]) >= 1 and all(f["compiler_verified"] is True for f in aid_patch["findings"])
    print("  [PASS] e2e_live_qnx8_aid_cap_ceiling_63_and_ai_patch")
    passed += 1

    # 13. Fail-closed CLI check: typo in --focus must FAIL (exit 2), never silently pass!
    cp_fc = run(
        [
            str(BIN_QTG), "check-sla", str(live_healthy),
            "--policy", str(POLICY), "--focus", "typo_no_such_proc", "--no-color",
        ],
        expect_rc=2,
    )
    assert "FOCUS MISMATCH" in cp_fc.stdout
    print("  [PASS] e2e_fail_closed_focus_mismatch")
    passed += 1

    print(f"\nE2E Result: {passed} passed, 0 failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
