# Module 4: Lab — Priority Inversion, `_NTO_CHF_FIXED_PRIORITY` & QNX 8.0 `PROCMGR_AID_PRIORITY`

**Estimated Time:** 10 minutes  
**Objective:** Reproduce QNX OS 8.0's two subtlest priority-inheritance traps — **both on bundled live multi-process QNX OS 8.0 kernel traces and in local simulation** — verify empirically how `procnto-smp-instr` caps non-root inheritance at priority `63`, and fix both traps cleanly.

---

## 4.1 Why QNX Priority Inheritance Can Still Fail Silently

Normally, when high-priority `qtg_sensor_rm` (prio `50`) sends a message through `qtg_planner_rm` (prio `45 -> 50`) to low-priority `qtg_actuator_rm` (base prio `12`), the QNX OS 8.0 microkernel automatically boosts `qtg_actuator_rm` to effective priority `50`. Background `qtg_telemetry_bg` (prio `30`) cannot preempt the brake command.

**Two QNX-specific configurations break this guarantee:**

### Trap A: `_NTO_CHF_FIXED_PRIORITY` on a Resource Manager Channel
If `qtg_actuator_rm` creates its channel with `_NTO_CHF_FIXED_PRIORITY`:
- The microkernel **disables message-driven priority inheritance** on that channel.
- `qtg_actuator_rm` remains at priority `12` while serving the priority `50` safety request.
- `qtg_telemetry_bg` (prio `30`) preempts `qtg_actuator_rm` on the same CPU for **`~422–431 µs`**, stalling the priority `50` braking loop!

### Trap B: QNX OS 8.0 `PROCMGR_AID_PRIORITY` Unprivileged Ceiling (`63`)
QNX OS 8.0 enforces least-privilege security via `procmgr_ability()` and `secpol`:
- Unprivileged (`setuid(1000)`) processes without **`PROCMGR_AID_PRIORITY`** are capped at priority **`63`**.
- **Empirically Verified Live on QNX OS 8.0 (`procnto-smp-instr` 2.8.0):** When a privileged safety client (`qtg_planner_rm`, prio **`120`**) calls `MsgSend()` into a non-root `qtg_actuator_rm` process that dropped root without `PROCMGR_AID_PRIORITY`, **the QNX OS 8.0 microkernel caps `qtg_actuator_rm`'s inherited priority at exactly `63`** (never `120`!).
- Any task running at priority **`64..119`** (e.g., `qtg_telemetry_bg` at prio `80`) can now preempt your priority-120 safety pipeline!

*(And `QTraceGuard` detects BOTH traps on arbitrary third-party QNX 8.0 `.kev` traces even with zero custom markers, by checking whether the server's kernel `THRUNNING` priority stays pinned at `base_priority` or caps at `63` while a higher-priority client is `REPLY`-blocked!)*

---

## 4.2 Lab 4A: Catching `_NTO_CHF_FIXED_PRIORITY` Inversion

### Live 4-Process QNX OS 8.0 Kernel Trace (Zero SDP Needed)
```bash
./build/qtraceguard check-sla samples/qnx8_live/qnx8_prio-inversion.kev \
    --policy qemu/sla_policy.json --focus qtg \
    --json out/lab4_live_fixed.json
echo "Exit code: $?"
```

Verified live QNX OS 8.0 output (`22,525` events, `4` distinct QNX processes, `12/12` inversions caught):
```text
[3] Priority Inversions & QNX 8.0 Priority-Inheritance Diagnostics (12 detected)
  [FIXED_PRIORITY_CHANNEL] High P4309017:T1(tmp/qtg_planner_rm, prio 50) blocked on Server P4304920:T1(tmp/qtg_actuator_rm, eff_prio 12, ch:1) preempted by P4300823:T1(tmp/qtg_telemetry_bg, prio 30) for 431.39 us
...
[SLA VERDICT] FAIL (exit 2)
```

### Local Deterministic Simulation
```bash
./build/qtraceguard generate-demo prio-inversion out/lab4_fixed_prio.kev --iters 8
./build/qtraceguard check-sla out/lab4_fixed_prio.kev --policy qemu/sla_policy.json
```
Exact simulated output:
```text
  [FIXED_PRIORITY_CHANNEL] High P102:T1(qtg_planner_rm, prio 50) blocked on Server P103:T1(qtg_actuator_rm, eff_prio 12, ch:3) preempted by P104:T1(qtg_telemetry_bg, prio 30) for 422.00 us
```

---

## 4.3 Lab 4B: Catching the Live QNX OS 8.0 `PROCMGR_AID_PRIORITY` Ceiling Cap (`63`)

### Live 4-Process QNX OS 8.0 Kernel Trace (Real `setuid(1000)` Cap at `63`!)
```bash
./build/qtraceguard check-sla samples/qnx8_live/qnx8_aid-cap.kev \
    --policy qemu/sla_policy.json --focus qtg \
    --json out/lab4_live_aid.json
python3 tools/qtg_report.py out/lab4_live_aid.json out/lab4_live_aid.html
python3 tools/qtg_ai_patch.py out/lab4_live_aid.json --out-md out/lab4_aid_patch.md --verify-cc
```

Verified live QNX OS 8.0 output (`15,317` events, `12/12` live kernel ceiling caps caught):
```text
[3] Priority Inversions & QNX 8.0 Priority-Inheritance Diagnostics (12 detected)
  [PROCMGR_AID_PRIORITY_CAP] High P4550681:T1(tmp/qtg_planner_rm, prio 120) blocked on Server P4546584:T1(tmp/qtg_actuator_rm, eff_prio 63, ch:1)
[SLA VERDICT] FAIL
  - QNX 8.0 PROCMGR_AID_PRIORITY CAP: 12 server inheritance cap event(s) at ceiling 63
```

### Local Deterministic Simulation
```bash
./build/qtraceguard generate-demo aid-cap out/lab4_aid_cap.kev --iters 8
./build/qtraceguard check-sla out/lab4_aid_cap.kev --policy qemu/sla_policy.json
```
Exact simulated output:
```text
  [PROCMGR_AID_PRIORITY_CAP] High P102:T1(qtg_planner_rm, prio 120) blocked on Server P103:T1(qtg_actuator_rm, eff_prio 63, ch:3) preempted by P104:T1(qtg_telemetry_bg, prio 80) for 382.00 us
```

---

## 4.4 Hands-On Coding Exercise: Apply the QNX 8.0 Least-Privilege Fix

In `src/target/qtg_rtos_shim.c` (`qtg_shim_drop_privileges_keep_priority`), notice how QTraceGuard retains AND locks `PROCMGR_AID_PRIORITY` **before** calling `setuid(1000)`:

```c
#include <sys/procmgr.h>
#include <unistd.h>

procmgr_ability(0,
    PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_PRIORITY,
    PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AID_PATHSPACE,
    PROCMGR_ADN_NONROOT | PROCMGR_AOP_LOCK  | PROCMGR_AID_PRIORITY,
    PROCMGR_AID_EOL);
setuid(1000); /* Now runs non-root, yet can inherit safety priorities > 63! */
```

**Try it yourself:** In `src/target/qtg_resmgr_node.c`, change `act_flags |= QTG_SHIM_CHF_FIXED_PRIORITY;` (line 184) to `act_flags = QTG_SHIM_CHF_UNBLOCK;`, rebuild (`make host`), regenerate `prio-inversion`, and watch `qtraceguard check-sla` flip from `FAIL (exit 2)` to **`PASS (exit 0)`**! *(Revert with `git checkout src/target/qtg_resmgr_node.c` after verifying.)*

---

**Next:** [Module 5: Why APS Was Removed in QNX 8.0 & Migrating to `SCHED_SPORADIC` →](05-aps-to-sporadic-qnx8.md)
