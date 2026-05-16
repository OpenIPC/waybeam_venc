SHELL := /bin/bash

SOC_BUILD ?= star6e

TOOLCHAIN_URL := https://github.com/openipc/firmware/releases/download/toolchain
TOOLCHAIN_TGZ := toolchain.sigmastar-infinity6e.tgz
TOOLCHAIN_DIR := toolchain/toolchain.sigmastar-infinity6e
CC_BIN := $(TOOLCHAIN_DIR)/bin/arm-openipc-linux-gnueabihf-gcc
TOOLCHAIN_MARUKO_TGZ := toolchain.sigmastar-infinity6c.tgz
TOOLCHAIN_MARUKO_DIR := toolchain/toolchain.sigmastar-infinity6c
CC_MARUKO_BIN := $(TOOLCHAIN_MARUKO_DIR)/bin/arm-openipc-linux-musleabihf-gcc

# Infinity6C kernel source for building sensor .ko via drivers/Makefile.
# KSRC_MARUKO must point at an existing Infinity6C 5.10.61 kernel source
# tree (arch/arm + top-level Makefile). The tree is not hosted in this
# repo and is not downloaded; obtain it from the appropriate SigmaStar
# SDK and pass it on the command line.
KSRC_MARUKO ?=

STAR6E_CC ?= $(TOOLCHAIN_DIR)/bin/arm-openipc-linux-gnueabihf-gcc
MARUKO_CC ?= $(TOOLCHAIN_MARUKO_DIR)/bin/arm-openipc-linux-musleabihf-gcc

OUT_DIR := out/$(SOC_BUILD)
OBJ_DIR := $(OUT_DIR)/obj
TARGET := $(OUT_DIR)/waybeam
JSON_CLI_TARGET := $(OUT_DIR)/json_cli
REGSCAN_TARGET := $(OUT_DIR)/regscan
TIMING_PROBE_TARGET := rtp_timing_probe
TIMING_PROBE_SRC := tools/rtp_timing_probe.c

VENC_VERSION := $(shell cat VERSION 2>/dev/null || echo unknown)
# -MMD -MP emits per-object .d files so a one-line change rebuilds just
# that object + relink, instead of every source under the sun.  -s is in
# LDFLAGS only (it's a link-time strip flag; not valid during -c).
COMMON_CFLAGS := -Os -Iinclude -Ilib -include include/ssc338q_compat.h -DVENC_VERSION=\"$(VENC_VERSION)\" -D_GNU_SOURCE -MMD -MP
CONFIG_SRC := src/venc_config.c src/venc_httpd.c src/venc_api.c src/venc_webui.c src/venc_recordings.c src/sensor_select.c src/venc_ring.c lib/cJSON.c
HELPER_SRC := src/backend.c src/file_util.c src/h26x_util.c src/h26x_param_sets.c src/codec_config.c src/pipeline_common.c src/scene_detector.c src/sdk_quiet.c src/rtp_packetizer.c src/hevc_rtp.c src/intra_refresh.c src/isp_runtime.c src/rtp_session.c src/stream_metrics.c src/rtp_sidecar.c src/output_socket.c src/timing.c src/idr_rate_limit.c src/debug_osd.c src/debug_osd_draw.c src/imu_bmi270.c src/audio_codec.c src/venc_jpeg.c src/venc_respawn.c
MARUKO_ONLY_SRC := src/maruko_mi.c src/maruko_config.c src/maruko_video.c src/maruko_controls.c src/maruko_output.c src/maruko_pipeline.c src/maruko_runtime.c src/maruko_iq.c src/maruko_cus3a.c src/maruko_ts_recorder.c src/maruko_recorder.c src/maruko_audio.c src/maruko_jpeg.c
STAR6E_ONLY_SRC := src/star6e_output.c src/star6e_audio.c src/star6e_hevc_rtp.c src/star6e_video.c src/star6e_pipeline.c src/star6e_controls.c src/star6e_runtime.c src/star6e_cus3a.c src/star6e_iq.c src/star6e_jpeg.c
RECORDER_SRC := src/star6e_recorder.c src/star6e_ts_recorder.c src/ts_mux.c
LIB_RUNPATH ?= /usr/lib
COMMON_LDFLAGS := -s -Wl,-rpath,$(LIB_RUNPATH) -Wl,--no-as-needed

# BASE_LIBS is set per-SOC below — all MI libs are loaded via dlopen at runtime.
ifeq ($(SOC_BUILD),maruko)
CC := $(MARUKO_CC)
SRC := src/main.c src/backend_maruko.c $(MARUKO_ONLY_SRC) $(RECORDER_SRC) $(HELPER_SRC) $(CONFIG_SRC)
# Stock OpenIPC Infinity6C firmware does NOT ship MI vendor libs; bundle them.
# Star6E doesn't need this — Infinity6E firmware has them in /rom/usr/lib/.
DRV := vendor-libs/maruko
DRV_EXTRA :=
SOC_CFLAGS :=
SOC_DEFS := -DPLATFORM_STAR6E -DPLATFORM_MARUKO -DHAVE_BACKEND_MARUKO=1
SOC_LDFLAGS :=
SOC_LIBS := -lm
BASE_LIBS := -Wl,--start-group -lpthread -ldl -lrt -Wl,--end-group
BUILD_TESTS := 0
TOOLCHAIN_TARGET := toolchain-maruko
else ifeq ($(SOC_BUILD),star6e)
CC := $(STAR6E_CC)
SRC := src/main.c src/backend_star6e.c src/star6e_mi.c $(STAR6E_ONLY_SRC) $(RECORDER_SRC) $(HELPER_SRC) $(CONFIG_SRC)
DRV :=
DRV_EXTRA :=
SOC_CFLAGS := -mfpu=neon-vfpv4 -mfloat-abi=hard -ftree-vectorize
SOC_DEFS := -DPLATFORM_STAR6E -DHAVE_BACKEND_STAR6E=1
SOC_LDFLAGS :=
SOC_LIBS := -lm
BASE_LIBS := -Wl,--start-group -lpthread -ldl -lrt -Wl,--end-group
BUILD_TESTS := 1
TOOLCHAIN_TARGET := toolchain
else
$(error Unsupported SOC_BUILD '$(SOC_BUILD)'; expected 'star6e' or 'maruko')
endif

CFLAGS += $(COMMON_CFLAGS) $(SOC_CFLAGS) $(SOC_DEFS)
LDFLAGS += $(COMMON_LDFLAGS) $(SOC_LDFLAGS)

.PHONY: help all build lint stage clean toolchain toolchain-maruko ksrc-maruko \
        drivers-maruko maruko-pull maruko-deploy maruko-full json_cli regscan \
        remote-test verify pre-pr \
        check check-soc-stamp print-config test test-werror test-asan test-tsan test-ci \
        webui webui-check

help:
	@echo "Targets:"
	@echo "  make build       Build standalone binaries (default, SOC_BUILD=star6e)"
	@echo "  make build SOC_BUILD=maruko"
	@echo "  make lint        Fast warning check (-Wall -Werror, compile only)"
	@echo "  make lint SOC_BUILD=maruko"
	@echo "  make stage       Build and stage runtime bundle in out/"
	@echo "  make test        Run host-native unit tests"
	@echo "  make test-ci     Run all test variants (test + asan + tsan)"
	@echo "  make clean       Clean build outputs"
	@echo "  make toolchain   Ensure Star6E cross-toolchain is present"
	@echo "  make toolchain-maruko Ensure Maruko cross-toolchain is present"
	@echo "  make ksrc-maruko KSRC_MARUKO=/path/to/kernel  Validate Infinity6C kernel source tree"
	@echo "  make drivers-maruko KSRC_MARUKO=/path/to/kernel  Build sensors/maruko/sensor_imx*_maruko.ko"
	@echo "  make json_cli SOC_BUILD=maruko  Build out/<soc>/json_cli (vendored from waybeam-hub)"
	@echo "  make regscan SOC_BUILD=maruko   Build out/<soc>/regscan (IMX335/IMX415 i2c register dumper)"
	@echo "  make maruko-pull HOST=root@<ip>  Pull libs/drivers/isp-bins from a device"
	@echo "  make maruko-deploy HOST=root@<ip>  Build + deploy venc (binary only) cycle"
	@echo "  make maruko-full   HOST=root@<ip>  Full bring-up: binary + libs + drivers + ISP bins"
	@echo "  make remote-test Run bounded remote CLI/test-binary workflow (pass ARGS='...')"
	@echo "  scripts/star6e_direct_deploy.sh cycle  Preferred Star6E venc deploy+HTTP smoke test"
	@echo "  scripts/maruko_direct_deploy.sh cycle  Preferred Maruko venc deploy+HTTP smoke test"
	@echo "  make verify      Build both backends and verify binaries exist"
	@echo "  make pre-pr      Full pre-PR checklist (version, changelog, build)"
	@echo "  make webui       Regenerate src/venc_webui.c from web/dashboard.html"
	@echo "  make webui-check Fail if src/venc_webui.c is stale vs HTML source"

all: build

build: $(TOOLCHAIN_TARGET) check check-soc-stamp | $(OUT_DIR)
build: $(TARGET)

# Per-source object list — one .o per .c, deps tracked via .d files.
# Maps both src/foo.c and lib/bar.c to $(OBJ_DIR)/src/foo.o, $(OBJ_DIR)/lib/bar.o.
OBJS := $(addprefix $(OBJ_DIR)/,$(SRC:.c=.o))
DEPS := $(OBJS:.o=.d)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

check-soc-stamp:
	@true

check:
	@test -x "$(CC)" || { echo "Compiler missing: $(CC)"; exit 1; }
	@if [ -n "$(DRV)" ]; then \
		test -d "$(DRV)" || { echo "Library dir missing: $(DRV)"; exit 1; }; \
	fi
	@if [ -n "$(DRV_EXTRA)" ]; then \
		test -d "$(DRV_EXTRA)" || { echo "Extra library dir missing: $(DRV_EXTRA)"; exit 1; }; \
	fi

lint: $(TOOLCHAIN_TARGET) check
	$(CC) $(CFLAGS) -Wall -Wextra -Werror -Wno-unused-parameter -Wno-old-style-declaration -fsyntax-only $(SRC)

# Pattern rule — compile any source under src/, lib/, or vendor/ into
# the mirrored layout under $(OBJ_DIR)/.  Header dependencies are
# captured by -MMD -MP in COMMON_CFLAGS; the resulting .d files are
# -include'd below.
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(if $(DRV),-L$(DRV),) $(if $(DRV_EXTRA),-L$(DRV_EXTRA),) $(if $(DRV),-Ltools,) $(BASE_LIBS) $(SOC_LIBS) -o $@

-include $(DEPS)

# Host-native timing probe (no cross-compiler or SDK libs needed)
$(TIMING_PROBE_TARGET): $(TIMING_PROBE_SRC) include/rtp_sidecar.h
	$(HOST_CC) -std=c99 -Wall -Wextra -O2 -D_GNU_SOURCE -Iinclude $(TIMING_PROBE_SRC) -lm -o $@

# json_cli — vendored from waybeam-hub (tools/json_cli/{json_cli.c,jsmn.h}).
# Cross-compiled with the SOC's toolchain so the same binary that runs venc
# can also read/patch /etc/venc.json on the target.
json_cli: $(TOOLCHAIN_TARGET) | $(OUT_DIR)
json_cli: $(JSON_CLI_TARGET)

$(JSON_CLI_TARGET): tools/json_cli/json_cli.c tools/json_cli/jsmn.h
	@mkdir -p $(@D)
	$(CC) -Os -s -Wall -Wextra -std=c11 -D_GNU_SOURCE -Itools/json_cli -o $@ $< -lm

# regscan — Sony IMX335/IMX415 sensor register dumper.  Vendored from
# tipoman9/star6c_sensor; see tools/regscan/NOTICE.  Built with the SOC
# toolchain so it can read /dev/i2c-* on the target alongside venc.
# Used by scripts/maruko_sensor_init_diff.sh for firstboot/majestic/venc
# register-state diffs.
regscan: $(TOOLCHAIN_TARGET) | $(OUT_DIR)
regscan: $(REGSCAN_TARGET)

$(REGSCAN_TARGET): tools/regscan/regscan.c
	@mkdir -p $(@D)
	$(CC) -Os -s -Wall -Wextra -std=c11 -D_GNU_SOURCE -o $@ $<

stage: build
	@if [ -n "$(DRV)" ] || [ -n "$(DRV_EXTRA)" ]; then mkdir -p $(OUT_DIR)/lib; fi
	@if [ -n "$(DRV)" ]; then cp -f $(DRV)/*.so $(OUT_DIR)/lib/; fi
	@if [ -n "$(DRV_EXTRA)" ]; then cp -f "$(DRV_EXTRA)"/*.so $(OUT_DIR)/lib/; fi
	@# Maruko-only: also stage sensor .ko (renamed _maruko.ko → _mipi.ko for
	@# drop-in install over stock OpenIPC names) and ISP .bin if cached locally.
	@if [ "$(SOC_BUILD)" = "maruko" ]; then \
		if ls sensors/maruko/sensor_*_maruko.ko >/dev/null 2>&1; then \
			mkdir -p $(OUT_DIR)/drivers; \
			for f in sensors/maruko/sensor_*_maruko.ko; do \
				base="$$(basename "$$f" _maruko.ko)"; \
				cp -f "$$f" "$(OUT_DIR)/drivers/$${base}_mipi.ko"; \
			done; \
		fi; \
		if ls iq-profiles/maruko-bin/*.bin >/dev/null 2>&1; then \
			mkdir -p $(OUT_DIR)/isp-bins; cp -f iq-profiles/maruko-bin/*.bin $(OUT_DIR)/isp-bins/; \
		fi; \
	fi

print-config:
	@echo "SOC_BUILD=$(SOC_BUILD)"
	@echo "CC=$(CC)"
	@echo "DRV=$(DRV)"
	@echo "DRV_EXTRA=$(DRV_EXTRA)"
	@echo "BUILD_TESTS=$(BUILD_TESTS)"

# ── Host-native unit tests (x86_64, no cross-compiler needed) ────────

HOST_CC      := cc
HOST_CFLAGS  := -std=c99 -Wall -Wextra -g -O0 -D_GNU_SOURCE \
                -Iinclude -Ilib -Itests
TEST_RUNNER  := tests/test_runner
TEST_SRCS    := tests/test_runner.c tests/test_venc_config.c \
                tests/test_venc_api.c tests/test_venc_httpd.c \
                tests/test_sensor_select.c tests/test_venc_ring.c \
                tests/test_file_util.c tests/test_h26x_util.c \
                tests/test_h26x_param_sets.c \
                tests/test_maruko_config.c \
                tests/test_pipeline_common.c \
                tests/test_codec_config.c tests/test_sdk_quiet.c \
                tests/test_rtp_packetizer.c \
                tests/test_hevc_rtp.c \
                tests/test_isp_runtime.c tests/test_rtp_session.c \
                tests/test_stream_metrics.c \
                tests/test_star6e_hevc_rtp.c tests/test_star6e_output.c \
                tests/test_star6e_audio.c tests/test_star6e_video.c \
                tests/test_star6e_recorder.c \
                tests/test_ts_mux.c tests/test_audio_ring.c \
                tests/test_star6e_ts_recorder.c \
                tests/test_idr_rate_limit.c \
                tests/test_backend.c \
                tests/test_debug_osd.c \
                tests/test_intra_refresh.c \
                tests/test_venc_jpeg.c
# Production sources compiled into the test binary (pure-logic modules only).
# sensor_select.c is included here; its MI_SNR_* deps are stubbed in test_sensor_select.c.
TEST_LIB_SRCS := src/backend.c src/venc_config.c src/venc_api.c src/venc_httpd.c src/venc_webui.c src/venc_recordings.c src/sensor_select.c src/venc_ring.c src/file_util.c src/h26x_util.c src/h26x_param_sets.c src/intra_refresh.c src/isp_runtime.c src/maruko_config.c src/codec_config.c src/pipeline_common.c src/rtp_session.c src/sdk_quiet.c src/rtp_packetizer.c src/hevc_rtp.c src/star6e_hevc_rtp.c src/star6e_output.c src/star6e_audio.c src/audio_codec.c src/star6e_video.c src/star6e_recorder.c src/star6e_ts_recorder.c src/ts_mux.c src/rtp_sidecar.c src/stream_metrics.c src/output_socket.c src/timing.c src/idr_rate_limit.c src/debug_osd_draw.c src/venc_jpeg.c lib/cJSON.c

$(TEST_RUNNER): $(TEST_SRCS) $(TEST_LIB_SRCS) tests/test_helpers.h include/backend.h include/h26x_param_sets.h include/hevc_rtp.h include/isp_runtime.h include/maruko_config.h include/pipeline_common.h include/rtp_packetizer.h include/rtp_session.h include/rtp_sidecar.h include/star6e_audio.h include/star6e_hevc_rtp.h include/star6e_output.h include/star6e_recorder.h include/star6e_ts_recorder.h include/ts_mux.h include/audio_ring.h include/star6e_video.h include/stream_metrics.h
	$(HOST_CC) $(HOST_CFLAGS) $(TEST_SRCS) $(TEST_LIB_SRCS) -lpthread -ldl -o $@

test: $(TEST_RUNNER)
	./$(TEST_RUNNER)

test-werror: HOST_CFLAGS += -Werror
test-werror: $(TEST_RUNNER)
	./$(TEST_RUNNER)

test-asan:
	$(HOST_CC) $(HOST_CFLAGS) -Werror -fsanitize=address,undefined $(TEST_SRCS) $(TEST_LIB_SRCS) -lpthread -ldl -o $(TEST_RUNNER)
	./$(TEST_RUNNER)

test-tsan:
	$(HOST_CC) $(HOST_CFLAGS) -Werror -fsanitize=thread $(TEST_SRCS) $(TEST_LIB_SRCS) -lpthread -ldl -o $(TEST_RUNNER)
	./$(TEST_RUNNER)

test-ci: test test-asan test-tsan

toolchain:
	@if [ ! -x "$(CC_BIN)" ]; then \
		echo "Fetching $(TOOLCHAIN_TGZ)..."; \
		wget -c -q --show-progress "$(TOOLCHAIN_URL)/$(TOOLCHAIN_TGZ)" -P "$$(pwd)"; \
		mkdir -p "$(TOOLCHAIN_DIR)"; \
		tar -xf "$(TOOLCHAIN_TGZ)" -C "$(TOOLCHAIN_DIR)" --strip-components=1; \
		rm -f "$(TOOLCHAIN_TGZ)"; \
	fi

toolchain-maruko:
	@if [ ! -x "$(CC_MARUKO_BIN)" ]; then \
		echo "Fetching $(TOOLCHAIN_MARUKO_TGZ)..."; \
		wget -c -q --show-progress "$(TOOLCHAIN_URL)/$(TOOLCHAIN_MARUKO_TGZ)" -P "$$(pwd)"; \
		mkdir -p "$(TOOLCHAIN_MARUKO_DIR)"; \
		tar -xf "$(TOOLCHAIN_MARUKO_TGZ)" -C "$(TOOLCHAIN_MARUKO_DIR)" --strip-components=1; \
		rm -f "$(TOOLCHAIN_MARUKO_TGZ)"; \
	fi

# ── Maruko drivers ────────────────────────────────────────────────────
#
# Validate that KSRC_MARUKO points at a usable Infinity6C 5.10.61 kernel
# tree. The tree is not redistributable from this repo, so we never
# fetch — pass KSRC_MARUKO=/path/to/kernel on the command line:
#
#   make drivers-maruko KSRC_MARUKO=/path/to/infinity6c-kernel
#
# If you cannot get the kernel source, populate sensors/maruko/ via
# scripts/maruko_pull_artifacts.sh drivers from a known-good device
# instead.
ksrc-maruko:
	@if [ -z "$(KSRC_MARUKO)" ]; then \
		echo "ERROR: KSRC_MARUKO is not set."; \
		echo "       Pass KSRC_MARUKO=/path/to/infinity6c-kernel on the command line."; \
		echo "       The Infinity6C 5.10.61 kernel source is not hosted by this repo."; \
		echo "       Alternative: scripts/maruko_pull_artifacts.sh drivers to pull"; \
		echo "       prebuilt .ko from a working bench device into sensors/maruko/."; \
		exit 1; \
	fi
	@if [ ! -d "$(KSRC_MARUKO)" ] || [ ! -f "$(KSRC_MARUKO)/Makefile" ] || [ ! -d "$(KSRC_MARUKO)/arch/arm" ]; then \
		echo "ERROR: KSRC_MARUKO=$(KSRC_MARUKO) is not a valid kernel source tree"; \
		echo "       (need a directory containing Makefile and arch/arm/)."; \
		exit 1; \
	fi
	@echo "Using Maruko kernel source at $(KSRC_MARUKO)"

drivers-maruko: toolchain-maruko ksrc-maruko
	$(MAKE) -C drivers sensor KSRC="$(KSRC_MARUKO)" \
		CROSS="$(abspath $(TOOLCHAIN_MARUKO_DIR))/bin/arm-openipc-linux-musleabihf-"
	@mkdir -p sensors/maruko
	@cp -f drivers/sensor_imx*_maruko.ko sensors/maruko/ 2>/dev/null || true
	@echo ""
	@echo "Built modules in sensors/maruko/:"
	@ls -lh sensors/maruko/*.ko 2>/dev/null || echo "  (none — check drivers/ build output)"

# ── Maruko deploy convenience wrappers ────────────────────────────────

maruko-pull:
	@HOST="$${HOST:-root@192.168.2.12}"; \
	./scripts/maruko_pull_artifacts.sh --host "$${HOST}"

maruko-deploy:
	@HOST="$${HOST:-root@192.168.2.12}"; \
	./scripts/maruko_direct_deploy.sh --host "$${HOST}" cycle

maruko-full:
	@HOST="$${HOST:-root@192.168.2.12}"; \
	./scripts/maruko_direct_deploy.sh --host "$${HOST}" full

remote-test:
	SOC_BUILD=$(SOC_BUILD) ./scripts/remote_test.sh $(ARGS)

# ── Verification targets ──────────────────────────────────────────────

STAR6E_BINS := out/star6e/waybeam
MARUKO_BINS := out/maruko/waybeam

webui:
	python3 tools/build_webui.py

webui-check:
	python3 tools/build_webui.py --check

verify: webui-check
	@echo "=== Building Maruko backend ==="
	$(MAKE) build SOC_BUILD=maruko
	@echo ""
	@echo "=== Verifying Maruko binaries ==="
	@for f in $(MARUKO_BINS); do \
		if [ -x "$$f" ]; then echo "  OK  $$f"; \
		else echo "  FAIL  $$f not found or not executable"; exit 1; fi; \
	done
	@echo ""
	@echo "=== Building Star6E backend ==="
	$(MAKE) build SOC_BUILD=star6e
	@echo ""
	@echo "=== Verifying Star6E binaries ==="
	@for f in $(STAR6E_BINS); do \
		if [ -x "$$f" ]; then echo "  OK  $$f"; \
		else echo "  FAIL  $$f not found or not executable"; exit 1; fi; \
	done
	@echo ""
	@echo "=== Verify passed ==="

pre-pr: verify
	@echo ""
	@echo "=== Pre-PR checks ==="
	@if [ ! -f VERSION ]; then echo "  FAIL  VERSION file missing"; exit 1; fi
	@echo "  VERSION: $$(cat VERSION)"
	@if ! grep -q "$$(cat VERSION)" HISTORY.md; then \
		echo "  WARN   VERSION $$(cat VERSION) not found in HISTORY.md"; \
		echo "         Add a changelog entry before opening a PR."; \
	else \
		echo "  OK  HISTORY.md has entry for $$(cat VERSION)"; \
	fi
	@echo ""
	@echo "=== Pre-PR complete ==="

clean:
	rm -rf out/star6e out/maruko
	rm -f $(TIMING_PROBE_TARGET)
	rm -f $(TEST_RUNNER)
	rm -f .build_soc
