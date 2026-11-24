# Module 6: Automating Real-Time SLA Gates, AI Patch Copilot, Fuzzing & SARIF in CI

**Estimated Time:** 7 minutes  
**Objective:** Turn kernel trace inspection from a manual post-mortem task into a fail-closed Pull Request gate combining `sla_policy.json`, `qtraceguard check-sla`, `qtraceguard diff`, **Compiler-Verified AI Patch Copilot** (`tools/qtg_ai_patch.py --verify-cc`), **LibFuzzer + ASan/UBSan**, and **GitHub Code Scanning SARIF 2.1.0**.

---

## 6.1 Defining Your Fail-Closed Real-Time SLA Contract

Inspect [qemu/sla_policy.json](../qemu/sla_policy.json):

```json
{
  "max_dispatch_latency_us": 250,
  "max_p99_dispatch_latency_us": 150,
  "max_ipc_rtt_us": 2000,
  "max_prio_inversion_us": 100,
  "max_deadlock_cycles": 0,
  "max_pulse_backlog": 16,
  "forbid_fixed_priority_inversion": true,
  "forbid_aid_priority_cap": true,
  "forbid_aps_bankruptcy": true
}
```

`qtraceguard check-sla` is strictly **fail-closed**:
- Exits **`0`** ONLY when all timing, cycle, inversion, pulse, and migration gates pass AND the trace is valid.
- Exits **`2`** on any real-time SLA violation **OR** on any silent failure condition: `0` decoded events (`EMPTY TRACE`), a misspelled `--focus` process matching `0` threads (`FOCUS MISMATCH`), dropped `tracelogger` buffers (`DROPPED TRACE BUFFERS`), or table overflow!

Verify fail-closed behavior right now:
```bash
./build/qtraceguard check-sla samples/qnx8_live/qnx8_healthy.kev \
    --policy qemu/sla_policy.json --focus nonexistent_typo
echo "Exit code (must be 2, never 0): $?"
```

---

## 6.2 Comparing Baseline vs. Candidate Builds (`qtraceguard diff`)

Catch regressions before merge by diffing a PR's `.kev` trace against `main`'s baseline trace:

```bash
./build/qtraceguard diff samples/qnx8_live/qnx8_healthy.kev \
    samples/qnx8_live/qnx8_prio-inversion.kev --focus qtg
echo "Diff exit code: $?"
```

`qtraceguard diff` prints a side-by-side latency & fault delta table (`Deadlock Cycles`, `Priority Inversions`, `Dispatch p99`, `IPC RTT max`, `SLA Verdict`) and returns exit code `2` on any regression.

---

## 6.3 Neuro-Symbolic Compiler-Verified AI Patch Copilot (`tools/qtg_ai_patch.py`)

When an SLA gate fails in CI, `tools/qtg_ai_patch.py` consumes the deterministic JSON trace report, produces a trace-grounded QNX OS 8.0 root-cause explanation + unified diff, and **compiles every generated C remediation snippet with `-Wall -Wextra -Werror`**:

```bash
# Analyze live QNX 8.0 deadlock capture and verify generated C fix with qcc/gcc -Werror
./build/qtraceguard analyze samples/qnx8_live/qnx8_deadlock.kev \
    --policy qemu/sla_policy.json --focus qtg --json out/ci_dl.json --no-color > /dev/null

python3 tools/qtg_ai_patch.py out/ci_dl.json \
    --out-md out/ci_dl_patch.md --out-json out/ci_dl_patch.json --verify-cc
cat out/ci_dl_patch.md
```

- **Air-Gapped Default (Zero API Keys Required):** Uses deterministic symbolic rules grounded 100% in the binary trace (zero hallucinated PIDs or latencies).
- **Opt-In Compiler-in-the-Loop LLM Mode (`--llm`):** Set `QTG_LLM_API_KEY` (or `OPENAI_API_KEY`), optional `QTG_LLM_BASE_URL` and `QTG_LLM_MODEL`, and pass `--llm --verify-cc`. QTraceGuard sends compact verified JSON trace facts to the LLM, compiles its synthesized QNX 8.0 C patch via `qcc`/`gcc -Werror`, and automatically feeds compiler errors back for a self-repair pass if compilation fails — falling back cleanly to the verified deterministic patch if offline!

---

## 6.4 Full GitHub Actions Pipeline, LibFuzzer & Code Scanning (SARIF 2.1.0)

Look at [.github/workflows/qnx-ci.yml](../.github/workflows/qnx-ci.yml):
1. Compiles host binaries (`make host`) and verifies `#ifdef __QNX__` target branches (`make qnx-compile-check`) with `-Wall -Wextra -Werror`.
2. Runs **13 C11 unit tests + 13 Python E2E tests** (including all 5 bundled live multi-process QNX OS 8.0 `.kev` captures and fail-closed checks) via `make test`.
3. Runs AddressSanitizer + UndefinedBehaviorSanitizer (`make asan`).
4. Runs **Clang LibFuzzer + ASan + UBSan** (`make fuzz FUZZ_TIME=10`) seeded with real QNX 8.0 `.kev` captures across binary `.kev`, `traceprinter`, analyzer, and JSON/SARIF emitters!
5. Runs `./qemu/qnx800_harness.sh --sim-ci`, generates interactive HTML5/SVG Trace Studios + compiler-verified AI patches for every live QNX 8.0 trace, uploads **SARIF 2.1.0 (`QTG001..QTG004`)** directly to **GitHub Code Scanning** via `github/codeql-action/upload-sarif@v3`, and publishes HTML dashboards as workflow artifacts.

Run the full local CI verification suite right now:
```bash
make test && make asan && ./qemu/qnx800_harness.sh --sim-ci
```

---

**Next:** [Module 7: QNX OS 8.0 Real-Time Cheatsheet & Self-Check Quiz →](07-cheatsheet-and-quiz.md)
