# Module 1: QNX OS 8.0 Setup, QNX Everywhere License & QEMU Bring-Up

**Estimated Time:** 10 minutes  
**Objective:** Install QNX SDP 8.0 cleanly under the free non-commercial *QNX Everywhere* license, boot a KVM-accelerated instrumented QNX OS 8.0 x86_64 target (`procnto-smp-instr`), and verify live kernel capabilities using `qtg-probe`.

---

## 1.1 Licensing Hygiene (Important!)

QNX OS 8.0 is a proprietary RTOS made freely available for learning, research, and hackathon projects via **QNX Everywhere** (non-commercial license).

> **Clean-Room Rule:** Never commit QNX SDP headers, `libc.so`, `procnto-smp-instr`, or `.ifs` boot images to public Git repositories. `QTraceGuard` is 100% MIT-licensed clean-room source code; QNX binaries are resolved strictly from your local `$QNX_TARGET` at build time. Pure runtime `.kev` captures in `samples/qnx8_live/` allow zero-SDP reproduction without redistributing any SDK binary.

### Obtaining Your Free QNX Everywhere License
1. Create a free **myQNX** account at <https://www.qnx.com/getqnx>.
2. Accept the *QNX Everywhere* non-commercial developer agreement and install the **QNX Software Center (QSC)** (`qnxsoftwarecenter` or headless `qnxsoftwarecenter_clt`).
3. Inside QSC, install **QNX Software Development Platform 8.0** (`com.qnx.qnx800`) into `~/qnx800/`.
4. Ensure QEMU and hardware KVM (`/dev/kvm`, user in `kvm` group) are available on your Linux host:
   ```bash
   sudo apt-get update && sudo apt-get install -y qemu-system-x86 build-essential clang python3
   ls -l /dev/kvm
   ```

---

## 1.2 Initializing the QNX SDP 8.0 Shell Environment

Before invoking `qcc` or `mkqnximage`, source the environment script in your terminal:

```bash
source ~/qnx800/qnxsdp-env.sh
echo "QNX_HOST=$QNX_HOST"
echo "QNX_TARGET=$QNX_TARGET"
qcc -V
```

You should see `gcc_ntox86_64` and `gcc_ntoaarch64le` targets listed.

---

## 1.3 Cross-Compiling `QTraceGuard` Target Binaries

From the `QTraceGuard` repository root:

```bash
# Build host analyzer (`build/qtraceguard`)
make host

# Cross-compile QNX 8.0 x86_64 target binaries (for QEMU, -Wall -Wextra -Werror)
make qnx-x86_64

# Cross-compile for Raspberry Pi 5 / ARM64 (aarch64le, -Wall -Wextra -Werror)
make qnx-aarch64
```

*(Without QNX SDP installed, verify the `#ifdef __QNX__` code paths locally with `make qnx-compile-check` and run real QNX OS 8.0 traces immediately from `samples/qnx8_live/`!)*

---

## 1.4 Booting QNX OS 8.0 in QEMU (`mkqnximage` + Zero-Bridge User-Mode KVM)

Use **Terminal 1** to generate and boot the instrumented QNX OS 8.0 VM:

```bash
./qemu/qnx800_harness.sh --boot-qemu
```

Under the hood (`qemu/qnx800_harness.sh`), this avoids root/`brctl` bridge requirements completely:
```bash
mkdir -p build/qnx8-qemu-vm && cd build/qnx8-qemu-vm
mkqnximage --type=qemu --arch=x86_64 --proc=procnto-smp-instr \
    --cpu=4 --ram=2G --ssh-ident="$HOME/.ssh/id_rsa.pub" --noprompt --force --build

qemu-system-x86_64 -smp 4 -enable-kvm -cpu host -m 2G \
    -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device virtio-net-pci,netdev=net0 -nographic -kernel output/ifs.bin
```

In **Terminal 2**, deploy and trace any scenario automatically over SSH (`root@127.0.0.1:2222`):
```bash
./qemu/qnx800_harness.sh --check
./qemu/qnx800_harness.sh --qnx-qemu healthy
```

*(Customizable via `QTG_QEMU_CPUS`, `QTG_QEMU_RAM`, `QTG_TARGET_HOST`, `QTG_TARGET_SSH_PORT`, `QTG_TARGET_USER`, and `QTG_SSH_KEY`.)*

### Enabling Kernel Tracing (`procnto-smp-instr`)
To capture binary `.kev` kernel traces via `tracelogger`, the target OS image must boot the **instrumented microkernel** (`procnto-smp-instr`) instead of the standard kernel. See [qemu/qtg_qnx8.build](../qemu/qtg_qnx8.build) for the minimal QNX 8.0 `mkifs` buildfile configuration (note how legacy `[module=aps]` is intentionally omitted!).

---

## 1.5 Running Your First Live Target Diagnostic (`qtg-probe`)

`qtg-probe` calls `SchedCtl()` directly against the kernel, empirically tests `PROCMGR_AID_PRIORITY` at priority `64`, and runs a deterministic synchronous `MsgSend`/`MsgReply` microbenchmark:

```bash
# On host (POSIX CI simulation):
./build/qtg-probe --json --iters 200

# On live QNX OS 8.0 QEMU target (automatically saved to out/qnx8_probe.json by --qnx-qemu):
ssh -i "$HOME/.ssh/id_rsa" -p 2222 root@127.0.0.1 "/tmp/qtg-probe --json --iters 200"
```

Verified live QNX OS 8.0 output (`4 vCPU KVM`, `procnto-smp-instr`):
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
  "ipc_bench": {"iters": 200, "succeeded": 200, "p50_ns": 2580, "p99_ns": 120782, "max_ns": 141452}
}
```

Notice the two real QNX OS 8.0 kernel fingerprints confirmed live:
1. Calling `SchedCtl()` returns **`errno = 89` (`ENOSYS`)** on QNX OS 8.0 (`errno = 38` on Linux host simulation) — confirming legacy APS removal.
2. `SCHED_SPORADIC` is available, `PROCMGR_AID_PRIORITY` unprivileged ceiling (`63`) is empirically verified, and live synchronous IPC median RTT is **`2.58 µs`**.

---

**Next:** [Module 2: QNX Microkernel IPC & `iofunc_*` Resource Managers →](02-ipc-and-resmgr.md)
