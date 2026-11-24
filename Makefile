# QTraceGuard Build System (Host POSIX + QNX SDP 8.0 Cross-Compilation)
# SPDX-License-Identifier: MIT

CC          ?= gcc
CFLAGS      ?= -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude
LDFLAGS     ?= -pthread -lm
QCC         ?= qcc
BUILD_DIR   := build
QNX_X86_DIR := build-qnx-x86_64
QNX_ARM_DIR := build-qnx-aarch64

HOST_SRCS   := src/host/qtg_kev.c src/host/qtg_analyzer.c src/target/qtg_rtos_shim.c src/target/qtg_resmgr_node.c src/host/main.c
RESMGR_SRCS := src/host/qtg_kev.c src/target/qtg_rtos_shim.c src/target/qtg_resmgr_node.c
PROBE_SRCS  := src/host/qtg_kev.c src/target/qtg_rtos_shim.c src/target/qtg_probe.c
TEST_SRCS   := src/host/qtg_kev.c src/host/qtg_analyzer.c src/target/qtg_rtos_shim.c src/target/qtg_resmgr_node.c tests/test_runner.c

.PHONY: all host test asan fuzz qnx-compile-check qnx-x86_64 qnx-aarch64 demo clean

all: host

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

host: $(BUILD_DIR)/qtraceguard $(BUILD_DIR)/qtg-resmgr-node $(BUILD_DIR)/qtg-probe

$(BUILD_DIR)/qtraceguard: $(HOST_SRCS) include/*.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(HOST_SRCS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/qtg-resmgr-node: $(RESMGR_SRCS) include/*.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DQTG_BUILD_RESMGR_BIN $(RESMGR_SRCS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/qtg-probe: $(PROBE_SRCS) include/*.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(PROBE_SRCS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_runner: $(TEST_SRCS) include/*.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_SRCS) -o $@ $(LDFLAGS)

# Verify #ifdef __QNX__ target branches compile cleanly with -Wall -Wextra -Werror
qnx-compile-check: | $(BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -D__QNX__ -D__QNXNTO__ -DQTG_HAVE_SCHED_APS_H -DQTG_BUILD_RESMGR_BIN \
		-Itests/qnx_stub_headers -Iinclude -c src/target/qtg_rtos_shim.c -o $(BUILD_DIR)/qnx_check_shim.o
	$(CC) -std=c11 -Wall -Wextra -Werror -D__QNX__ -D__QNXNTO__ -DQTG_BUILD_RESMGR_BIN \
		-Itests/qnx_stub_headers -Iinclude -c src/target/qtg_resmgr_node.c -o $(BUILD_DIR)/qnx_check_resmgr.o
	$(CC) -std=c11 -Wall -Wextra -Werror -D__QNX__ -D__QNXNTO__ \
		-Itests/qnx_stub_headers -Iinclude -c src/target/qtg_probe.c -o $(BUILD_DIR)/qnx_check_probe.o
	@echo "[OK] QNX OS 8.0 #ifdef __QNX__ API compilation check passed with 0 warnings."

# Native QNX SDP 8.0 cross-compilation (requires: source ~/qnx800/qnxsdp-env.sh)
qnx-x86_64:
	@test -n "$(QNX_TARGET)" || (echo "ERROR: QNX_TARGET not set. Run: source ~/qnx800/qnxsdp-env.sh"; exit 1)
	@mkdir -p $(QNX_X86_DIR)
	$(QCC) -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Werror -Iinclude -DQTG_BUILD_RESMGR_BIN \
		$(RESMGR_SRCS) -o $(QNX_X86_DIR)/qtg-resmgr-node
	$(QCC) -Vgcc_ntox86_64 -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Werror -Iinclude \
		$(PROBE_SRCS) -o $(QNX_X86_DIR)/qtg-probe
	@echo "[OK] Built QNX 8.0 x86_64 target binaries in $(QNX_X86_DIR)/"

qnx-aarch64:
	@test -n "$(QNX_TARGET)" || (echo "ERROR: QNX_TARGET not set. Run: source ~/qnx800/qnxsdp-env.sh"; exit 1)
	@mkdir -p $(QNX_ARM_DIR)
	$(QCC) -Vgcc_ntoaarch64le -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Werror -Iinclude -DQTG_BUILD_RESMGR_BIN \
		$(RESMGR_SRCS) -o $(QNX_ARM_DIR)/qtg-resmgr-node
	$(QCC) -Vgcc_ntoaarch64le -std=c11 -D_QNX_SOURCE -O2 -Wall -Wextra -Werror -Iinclude \
		$(PROBE_SRCS) -o $(QNX_ARM_DIR)/qtg-probe
	@echo "[OK] Built QNX 8.0 aarch64le target binaries in $(QNX_ARM_DIR)/"

test: host $(BUILD_DIR)/test_runner qnx-compile-check
	@mkdir -p out
	OMP_NUM_THREADS=4 ./$(BUILD_DIR)/test_runner
	OMP_NUM_THREADS=4 python3 tests/test_e2e.py

asan: | $(BUILD_DIR)
	@mkdir -p out
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
		-Iinclude $(TEST_SRCS) -o $(BUILD_DIR)/test_runner_asan $(LDFLAGS)
	OMP_NUM_THREADS=4 ./$(BUILD_DIR)/test_runner_asan

# LibFuzzer + ASan + UBSan fuzzing harness for binary .kev parser & analyzer (requires clang)
FUZZ_TIME ?= 30
fuzz: | $(BUILD_DIR)
	@mkdir -p out/fuzz_corpus
	@if [ -d samples/qnx8_live ]; then cp -f samples/qnx8_live/*.kev out/fuzz_corpus/ 2>/dev/null || true; fi
	clang -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		-Iinclude src/host/qtg_kev.c src/host/qtg_analyzer.c tests/fuzz_kev.c -o $(BUILD_DIR)/fuzz_kev -lm
	./$(BUILD_DIR)/fuzz_kev out/fuzz_corpus -max_total_time=$(FUZZ_TIME) -max_len=327680 -print_final_stats=1

demo: host
	@mkdir -p out
	@for s in healthy deadlock prio-inversion aid-cap pulse-storm aps-legacy; do \
		OMP_NUM_THREADS=4 ./$(BUILD_DIR)/qtraceguard generate-demo $$s out/$$s.kev --iters 16; \
		OMP_NUM_THREADS=4 ./$(BUILD_DIR)/qtraceguard analyze out/$$s.kev --policy qemu/sla_policy.json \
			--json out/$$s.json --sarif out/$$s.sarif --no-color > out/$$s.txt; \
		OMP_NUM_THREADS=4 python3 tools/qtg_report.py out/$$s.json out/$$s.html; \
	done
	@echo "[OK] Generated all 6 scenarios (.kev, .json, .sarif, .html) in out/"

clean:
	rm -rf $(BUILD_DIR) $(QNX_X86_DIR) $(QNX_ARM_DIR) out/*.kev out/*.json out/*.sarif out/*.html out/*.txt
