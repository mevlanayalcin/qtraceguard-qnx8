#!/usr/bin/env bash
# QTraceGuard - Reproducible QNX OS 8.0 QEMU Verification & CI Harness
# SPDX-License-Identifier: MIT
#
# Modes:
#   ./qemu/qnx800_harness.sh --check          # Pre-flight check (QNX SDP 8.0, KVM, SSH key, QEMU, host tools)
#   ./qemu/qnx800_harness.sh --sim-ci         # Full deterministic CI suite + live samples + AI patch verify
#   ./qemu/qnx800_harness.sh --boot-qemu      # Build & launch QNX 8.0 QEMU VM via mkqnximage (requires QNX SDP 8.0)
#   ./qemu/qnx800_harness.sh --qnx-qemu <sc>  # Cross-compile, deploy to QNX 8.0 QEMU, capture tracelogger .kev, verify SLA
#   ./qemu/qnx800_harness.sh --help           # Show usage and environment variables

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_WORKDIR="${QTG_QEMU_WORKDIR:-$ROOT_DIR/build/qnx8-qemu-vm}"
TARGET_HOST="${QTG_TARGET_HOST:-127.0.0.1}"
TARGET_PORT="${QTG_TARGET_SSH_PORT:-2222}"
TARGET_USER="${QTG_TARGET_USER:-root}"
SSH_KEY="${QTG_SSH_KEY:-$HOME/.ssh/id_rsa}"
QEMU_CPUS="${QTG_QEMU_CPUS:-4}"
QEMU_RAM="${QTG_QEMU_RAM:-2G}"
POLICY_FILE="${QTG_POLICY_FILE:-$ROOT_DIR/qemu/sla_policy.json}"
OUT_DIR="${QTG_OUT_DIR:-$ROOT_DIR/out}"

log()  { printf '[qtg-harness] %s\n' "$*"; }
warn() { printf '[qtg-harness] WARN: %s\n' "$*" >&2; }

print_help() {
    cat <<EOF
QTraceGuard QNX OS 8.0 QEMU & CI Verification Harness

Usage:
  $0 --check                 Check QNX SDP 8.0, /dev/kvm, SSH key, QEMU, and host build
  $0 --sim-ci                Run full deterministic POSIX CI + bundled live QNX 8.0 trace gates
  $0 --boot-qemu             Build (mkqnximage) and launch QNX OS 8.0 KVM/TCG QEMU target
  $0 --qnx-qemu <scenario>   Cross-compile, deploy to live QNX 8.0 QEMU, capture .kev & verify SLA
                             Scenarios: healthy | deadlock | prio-inversion | aid-cap | pulse-storm

Environment Variables:
  QTG_QEMU_WORKDIR  QEMU image dir      (default: build/qnx8-qemu-vm)
  QTG_TARGET_HOST   QNX target SSH host (default: 127.0.0.1)
  QTG_TARGET_SSH_PORT Target SSH port   (default: 2222)
  QTG_TARGET_USER   Target SSH user     (default: root)
  QTG_SSH_KEY       SSH private key     (default: \$HOME/.ssh/id_rsa)
  QTG_QEMU_CPUS     vCPU count          (default: 4)
  QTG_QEMU_RAM      VM memory           (default: 2G)
EOF
}

check_env() {
    log "=== QTraceGuard QNX OS 8.0 Pre-Flight Check ==="
    local has_qnx=0
    if [[ -n "${QNX_HOST:-}" && -n "${QNX_TARGET:-}" && -x "${QNX_HOST}/usr/bin/qcc" ]]; then
        log "QNX SDP 8.0  : FOUND (QNX_TARGET=${QNX_TARGET})"
        has_qnx=1
    else
        warn "QNX SDP 8.0  : NOT SOURCED (run 'source ~/qnx800/qnxsdp-env.sh' after myQNX QNX Everywhere install)"
    fi

    if command -v mkqnximage >/dev/null 2>&1; then
        log "mkqnximage   : FOUND ($(command -v mkqnximage))"
    else
        warn "mkqnximage   : NOT IN PATH (provided by QNX SDP 8.0)"
    fi

    if command -v qemu-system-x86_64 >/dev/null 2>&1; then
        log "QEMU x86_64  : FOUND ($(qemu-system-x86_64 --version | head -n 1))"
    else
        warn "QEMU x86_64  : NOT INSTALLED (sudo apt install qemu-system-x86)"
    fi

    if [[ -w /dev/kvm ]]; then
        log "Hardware KVM : WRITABLE (/dev/kvm enabled)"
    else
        warn "Hardware KVM : NOT WRITABLE (/dev/kvm missing; QEMU will fall back to TCG)"
    fi

    if [[ -f "${SSH_KEY}" ]]; then
        log "SSH Key      : FOUND (${SSH_KEY})"
    else
        warn "SSH Key      : NOT FOUND (${SSH_KEY}; set QTG_SSH_KEY or run ssh-keygen)"
    fi

    if (exec 3<>"/dev/tcp/${TARGET_HOST}/${TARGET_PORT}") 2>/dev/null; then
        exec 3>&-
        log "QNX Target   : REACHABLE (${TARGET_HOST}:${TARGET_PORT})"
    else
        log "QNX Target   : not listening on ${TARGET_HOST}:${TARGET_PORT} (run --boot-qemu in terminal 1)"
    fi

    if [[ -x "$ROOT_DIR/build/qtraceguard" ]]; then
        log "qtraceguard  : BUILT ($ROOT_DIR/build/qtraceguard)"
    else
        log "qtraceguard  : not built yet (run 'make host')"
    fi

    if [[ "$has_qnx" -eq 1 ]]; then
        log "READY for native QNX 8.0 QEMU deployment (--boot-qemu / --qnx-qemu)."
    else
        log "READY for deterministic POSIX simulation + bundled live QNX 8.0 trace CI mode (--sim-ci)."
    fi
}

run_sim_ci() {
    log "Running full deterministic CI verification pipeline..."
    mkdir -p "$OUT_DIR"
    make -C "$ROOT_DIR" host >/dev/null

    log "[1/4] Running qtg-probe capability & IPC round-trip benchmark..."
    "$ROOT_DIR/build/qtg-probe" --iters 200 --out "$OUT_DIR/probe.kev"
    "$ROOT_DIR/build/qtraceguard" check-sla "$OUT_DIR/probe.kev" --policy "$POLICY_FILE" --no-color

    log "[2/4] Verifying healthy QNX 8.0 pipeline PASSES SLA gate..."
    "$ROOT_DIR/build/qtraceguard" generate-demo healthy "$OUT_DIR/ci_healthy.kev" --iters 16
    "$ROOT_DIR/build/qtraceguard" check-sla "$OUT_DIR/ci_healthy.kev" \
        --policy "$POLICY_FILE" --json "$OUT_DIR/ci_healthy.json" \
        --sarif "$OUT_DIR/ci_healthy.sarif" --no-color
    python3 "$ROOT_DIR/tools/qtg_report.py" "$OUT_DIR/ci_healthy.json" "$OUT_DIR/ci_healthy.html"

    log "[3/4] Verifying fault scenarios deterministically FAIL SLA gate (exit code 2) + AI patch verify..."
    for scen in deadlock prio-inversion aid-cap pulse-storm aps-legacy; do
        "$ROOT_DIR/build/qtraceguard" generate-demo "$scen" "$OUT_DIR/ci_${scen}.kev" --iters 12
        set +e
        "$ROOT_DIR/build/qtraceguard" check-sla "$OUT_DIR/ci_${scen}.kev" \
            --policy "$POLICY_FILE" --json "$OUT_DIR/ci_${scen}.json" \
            --sarif "$OUT_DIR/ci_${scen}.sarif" --no-color > "$OUT_DIR/ci_${scen}.txt"
        rc=$?
        set -e
        if [[ "$rc" -ne 2 ]]; then
            echo "ERROR: Expected exit code 2 (SLA violation) for '$scen', got $rc" >&2
            exit 1
        fi
        python3 "$ROOT_DIR/tools/qtg_report.py" "$OUT_DIR/ci_${scen}.json" "$OUT_DIR/ci_${scen}.html"
        python3 "$ROOT_DIR/tools/qtg_ai_patch.py" "$OUT_DIR/ci_${scen}.json" \
            --out-md "$OUT_DIR/ci_${scen}_patch.md" --out-json "$OUT_DIR/ci_${scen}_patch.json" --verify-cc >/dev/null
        log "  [VERIFIED] '$scen' rejected (exit=2) + compiler-verified AI patch -> $OUT_DIR/ci_${scen}.html"
    done

    log "[4/4] Verifying bundled real QNX OS 8.0 KVM .kev captures (samples/qnx8_live/)..."
    if [[ -f "$ROOT_DIR/samples/qnx8_live/qnx8_healthy.kev" ]]; then
        "$ROOT_DIR/build/qtraceguard" check-sla "$ROOT_DIR/samples/qnx8_live/qnx8_healthy.kev" \
            --policy "$POLICY_FILE" --focus qtg --no-color >/dev/null
        for live_fail in deadlock prio-inversion aid-cap pulse-storm; do
            local lf="$ROOT_DIR/samples/qnx8_live/qnx8_${live_fail}.kev"
            [[ -f "$lf" ]] || continue
            set +e
            "$ROOT_DIR/build/qtraceguard" check-sla "$lf" \
                --policy "$POLICY_FILE" --focus qtg --no-color >/dev/null
            rc=$?
            set -e
            if [[ "$rc" -ne 2 ]]; then
                echo "ERROR: Expected exit code 2 on live QNX 8.0 sample '$lf', got $rc" >&2
                exit 1
            fi
        done
        log "  [VERIFIED] All bundled live QNX OS 8.0 kernel traces passed SLA regression checks."
    fi

    log "ALL CI DETERMINISM GATES VERIFIED."
}

boot_qnx_qemu() {
    if [[ -z "${QNX_TARGET:-}" ]] || ! command -v mkqnximage >/dev/null 2>&1; then
        echo "ERROR: QNX SDP 8.0 environment not found." >&2
        echo "  1. Sign up for free non-commercial QNX Everywhere at https://www.qnx.com/getqnx" >&2
        echo "  2. Install QNX SDP 8.0 via QNX Software Center (myQNX account)" >&2
        echo "  3. Run: source ~/qnx800/qnxsdp-env.sh" >&2
        exit 1
    fi
    mkdir -p "$QEMU_WORKDIR"
    local pub_key="${SSH_KEY}.pub"
    log "Building and launching QNX OS 8.0 QEMU x86_64 target in $QEMU_WORKDIR ..."
    log "SSH port forwarding: host:${TARGET_PORT} -> guest:22 (user-mode networking, zero brctl/root needed)"
    (
        cd "$QEMU_WORKDIR"
        if [[ ! -f output/disk-qemu.vmdk || ! -f output/ifs.bin ]]; then
            mkqnximage --type=qemu --arch=x86_64 --proc=procnto-smp-instr \
                --cpu="${QEMU_CPUS}" --ram="${QEMU_RAM}" \
                --ssh-ident="${pub_key}" --noprompt --force --build
        fi
        local kvm_flags=(-cpu max)
        if [[ -w /dev/kvm ]]; then
            kvm_flags=(-enable-kvm -cpu host)
            log "Hardware KVM (/dev/kvm) ENABLED."
        else
            warn "/dev/kvm not writable; falling back to QEMU TCG software emulation."
        fi
        exec qemu-system-x86_64 \
            -smp "${QEMU_CPUS}" "${kvm_flags[@]}" -m "${QEMU_RAM}" \
            -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
            -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:${TARGET_PORT}-:22" \
            -device virtio-net-pci,netdev=net0 \
            -pidfile output/qemu.pid -nographic \
            -kernel output/ifs.bin \
            -serial file:output/qemu.out \
            -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0
    )
}

run_qnx_qemu_scenario() {
    local scen="${1:-healthy}"
    if [[ -z "${QNX_TARGET:-}" ]]; then
        echo "ERROR: QNX_TARGET not set. Source ~/qnx800/qnxsdp-env.sh first." >&2
        exit 1
    fi
    mkdir -p "$OUT_DIR"
    log "Cross-compiling QTraceGuard target binaries for QNX OS 8.0 x86_64..."
    make -C "$ROOT_DIR" host qnx-x86_64

    local ssh_opts=(-n -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -p "$TARGET_PORT")
    local scp_opts=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -P "$TARGET_PORT")
    if [[ -f "${SSH_KEY}" ]]; then
        ssh_opts+=(-i "${SSH_KEY}")
        scp_opts+=(-i "${SSH_KEY}")
    fi

    log "Waiting for QNX 8.0 QEMU SSH (${TARGET_USER}@${TARGET_HOST}:${TARGET_PORT})..."
    local ready=0
    for _ in $(seq 1 20); do
        if ssh "${ssh_opts[@]}" "${TARGET_USER}@${TARGET_HOST}" "uname -a" >/dev/null 2>&1; then
            ready=1
            break
        fi
        sleep 1
    done
    if [[ "$ready" -ne 1 ]]; then
        echo "ERROR: QNX 8.0 target unreachable via SSH on ${TARGET_HOST}:${TARGET_PORT}" >&2
        exit 1
    fi

    log "Deploying qtg-resmgr-node, qtg-probe & tracelogger to QNX 8.0 QEMU..."
    scp -q "${scp_opts[@]}" \
        "$ROOT_DIR/build-qnx-x86_64/qtg-resmgr-node" \
        "$ROOT_DIR/build-qnx-x86_64/qtg-probe" \
        "$QNX_TARGET/x86_64/usr/sbin/tracelogger" \
        "${TARGET_USER}@${TARGET_HOST}:/tmp/"

    log "Running live qtg-probe and capturing wide-mode kernel trace via tracelogger (-w -s 1) during '${scen}'..."
    ssh "${ssh_opts[@]}" "${TARGET_USER}@${TARGET_HOST}" "/tmp/qtg-probe --json --iters 200" \
        > "$OUT_DIR/qnx8_probe.json"
    ssh "${ssh_opts[@]}" "${TARGET_USER}@${TARGET_HOST}" "
        chmod +x /tmp/qtg-resmgr-node /tmp/qtg-probe /tmp/tracelogger
        ln -sf /tmp/qtg-resmgr-node /tmp/qtg_sensor_rm
        /tmp/tracelogger -w -s 1 -n 48 -f /dev/shmem/qtg_${scen}.kev &
        TL_PID=\$!
        sleep 0.08
        /tmp/qtg_sensor_rm --scenario ${scen} --iters 12
        WK_RC=\$?
        wait \$TL_PID || true
        exit \$WK_RC
    "

    log "Pulling binary /dev/shmem/qtg_${scen}.kev from QNX 8.0 target..."
    scp -q "${scp_opts[@]}" "${TARGET_USER}@${TARGET_HOST}:/dev/shmem/qtg_${scen}.kev" \
        "$OUT_DIR/qnx8_${scen}.kev"

    log "Analyzing real QNX 8.0 kernel trace with host qtraceguard (--focus qtg)..."
    set +e
    "$ROOT_DIR/build/qtraceguard" check-sla "$OUT_DIR/qnx8_${scen}.kev" \
        --policy "$POLICY_FILE" --focus qtg \
        --json "$OUT_DIR/qnx8_${scen}.json" \
        --sarif "$OUT_DIR/qnx8_${scen}.sarif"
    local rc=$?
    set -e
    python3 "$ROOT_DIR/tools/qtg_report.py" "$OUT_DIR/qnx8_${scen}.json" "$OUT_DIR/qnx8_${scen}.html"
    python3 "$ROOT_DIR/tools/qtg_ai_patch.py" "$OUT_DIR/qnx8_${scen}.json" \
        --out-md "$OUT_DIR/qnx8_${scen}_patch.md" --out-json "$OUT_DIR/qnx8_${scen}_patch.json" --verify-cc || true
    log "Interactive HTML dashboard generated at: $OUT_DIR/qnx8_${scen}.html (SLA exit=${rc})"
    return "$rc"
}

case "${1:---check}" in
    --check)    check_env ;;
    --sim-ci)   run_sim_ci ;;
    --boot-qemu) boot_qnx_qemu ;;
    --qnx-qemu) run_qnx_qemu_scenario "${2:-healthy}" ;;
    --help|-h)  print_help ;;
    *)
        print_help >&2
        exit 1
        ;;
esac
