SHELL := /bin/bash

SOC_BUILD ?= star6e

TOOLCHAIN_URL := https://github.com/openipc/firmware/releases/download/toolchain
TOOLCHAIN_TGZ := toolchain.sigmastar-infinity6e.tgz
TOOLCHAIN_DIR := toolchain/toolchain.sigmastar-infinity6e
CC_BIN := $(TOOLCHAIN_DIR)/bin/arm-openipc-linux-gnueabihf-gcc
TOOLCHAIN_MARUKO_TGZ := toolchain.sigmastar-infinity6c.tgz
TOOLCHAIN_MARUKO_DIR := toolchain/toolchain.sigmastar-infinity6c
CC_MARUKO_BIN := $(TOOLCHAIN_MARUKO_DIR)/bin/arm-openipc-linux-musleabihf-gcc

STAR6E_CC ?= $(TOOLCHAIN_DIR)/bin/arm-openipc-linux-gnueabihf-gcc
MARUKO_CC ?= $(TOOLCHAIN_MARUKO_DIR)/bin/arm-openipc-linux-musleabihf-gcc

STAR6E_DRV ?= libs/star6e
MARUKO_DRV ?= libs/maruko
MARUKO_UCLIBC_DIR ?= libs/maruko/uclibc
# Backward-compatible overrides:
MARUKO_MI_LIB_DIR ?= $(MARUKO_DRV)
MARUKO_COMMON_LIB_DIR ?= $(MARUKO_DRV)

OUT_DIR := out/$(SOC_BUILD)
TARGET := $(OUT_DIR)/venc
TEST_TARGET := $(OUT_DIR)/snr_toggle_test
PROBE_TARGET := $(OUT_DIR)/snr_sequence_probe
TIMING_PROBE_TARGET := rtp_timing_probe
TEST_SRC := src/snr_toggle_test.c
PROBE_SRC := src/snr_sequence_probe.c
TIMING_PROBE_SRC := tools/rtp_timing_probe.c
MARUKO_SHIM_SRC := tools/maruko_uclibc_shim.c
MARUKO_SHIM_SO := tools/libmaruko_uclibc_shim.so

VENC_VERSION := $(shell cat VERSION 2>/dev/null || echo unknown)
COMMON_CFLAGS := -Os -s -Iinclude -Ilib -include include/ssc338q_compat.h -DVENC_VERSION=\"$(VENC_VERSION)\"
CONFIG_SRC := src/venc_config.c src/venc_httpd.c src/venc_api.c src/venc_webui.c src/sensor_select.c src/venc_ring.c lib/cJSON.c
HELPER_SRC := src/backend.c src/file_util.c src/h26x_util.c src/h26x_param_sets.c src/codec_config.c src/pipeline_common.c src/sdk_quiet.c src/rtp_packetizer.c src/isp_runtime.c src/rtp_session.c src/stream_metrics.c src/rtp_sidecar.c
MARUKO_ONLY_SRC := src/maruko_config.c src/maruko_video.c src/maruko_controls.c src/maruko_output.c src/maruko_pipeline.c src/maruko_runtime.c
STAR6E_ONLY_SRC := src/star6e_output.c src/star6e_audio.c src/star6e_hevc_rtp.c src/star6e_video.c src/star6e_pipeline.c src/star6e_controls.c src/star6e_runtime.c src/star6e_cus3a.c src/star6e_recorder.c src/star6e_ts_recorder.c src/ts_mux.c src/imu_bmi270.c src/eis.c src/eis_gyroglide.c src/star6e_iq.c src/debug_osd.c
LIB_RUNPATH ?= /usr/lib
COMMON_LDFLAGS := -Wl,-rpath,$(LIB_RUNPATH) -Wl,--no-as-needed
BASE_LIBS := -Wl,--start-group \
	-lmi_vif -lmi_vpe -lmi_venc -lmi_isp -lmi_sensor -lmi_sys \
	-lcus3a -lispalgo -lcam_os_wrapper -lpthread -ldl -lrt \
	-Wl,--end-group

ifeq ($(SOC_BUILD),maruko)
CC := $(MARUKO_CC)
SRC := src/main.c src/backend_maruko.c $(MARUKO_ONLY_SRC) $(HELPER_SRC) $(CONFIG_SRC)
DRV := $(MARUKO_MI_LIB_DIR)
DRV_EXTRA := $(MARUKO_COMMON_LIB_DIR)
ifeq ($(abspath $(DRV_EXTRA)),$(abspath $(DRV)))
DRV_EXTRA :=
endif
SOC_CFLAGS :=
SOC_DEFS := -DPLATFORM_STAR6E -DPLATFORM_MARUKO -DHAVE_BACKEND_MARUKO=1
SOC_LDFLAGS := -Wl,-rpath-link,$(DRV) -Wl,--unresolved-symbols=ignore-in-shared-libs -Wl,-rpath-link,tools
ifneq ($(strip $(DRV_EXTRA)),)
SOC_LDFLAGS += -Wl,-rpath-link,$(DRV_EXTRA)
endif
SOC_LIBS := -lmi_common -lmaruko_uclibc_shim -lm
BUILD_TESTS := 0
BUILD_MARUKO_SHIM := 1
TOOLCHAIN_TARGET := toolchain-maruko
else ifeq ($(SOC_BUILD),star6e)
CC := $(STAR6E_CC)
SRC := src/main.c src/backend_star6e.c $(STAR6E_ONLY_SRC) $(HELPER_SRC) $(CONFIG_SRC)
DRV := $(STAR6E_DRV)
DRV_EXTRA :=
SOC_CFLAGS := -mfpu=neon-vfpv4 -mfloat-abi=hard -ftree-vectorize
SOC_DEFS := -DPLATFORM_STAR6E -DHAVE_BACKEND_STAR6E=1
SOC_LDFLAGS := -Wl,-rpath-link,$(DRV)
SOC_LIBS := -lm
BUILD_TESTS := 1
BUILD_MARUKO_SHIM := 0
TOOLCHAIN_TARGET := toolchain
else
$(error Unsupported SOC_BUILD '$(SOC_BUILD)'; expected 'star6e' or 'maruko')
endif

CFLAGS += $(COMMON_CFLAGS) $(SOC_CFLAGS) $(SOC_DEFS)
LDFLAGS += $(COMMON_LDFLAGS) $(SOC_LDFLAGS)

.PHONY: help all build lint stage clean toolchain toolchain-maruko remote-test verify pre-pr \
        check check-soc-stamp print-config test test-werror test-asan test-tsan test-ci

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
	@echo "  make remote-test Run remote tester (pass ARGS='...')"
	@echo "  make verify      Build both backends and verify binaries exist"
	@echo "  make pre-pr      Full pre-PR checklist (version, changelog, build)"

all: build

build: $(TOOLCHAIN_TARGET) check check-soc-stamp | $(OUT_DIR)
build: $(TARGET)
ifeq ($(BUILD_TESTS),1)
build: $(TEST_TARGET) $(PROBE_TARGET)
endif
ifeq ($(BUILD_MARUKO_SHIM),1)
build: $(MARUKO_SHIM_SO)
endif

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

check-soc-stamp:
	@true

check:
	@test -x "$(CC)" || { echo "Compiler missing: $(CC)"; exit 1; }
	@test -d "$(DRV)" || { echo "Library dir missing: $(DRV)"; exit 1; }
	@if [ -n "$(DRV_EXTRA)" ]; then \
		test -d "$(DRV_EXTRA)" || { echo "Extra library dir missing: $(DRV_EXTRA)"; exit 1; }; \
	fi
	@if [ "$(BUILD_MARUKO_SHIM)" = "1" ]; then \
		test -d "$(MARUKO_UCLIBC_DIR)" || { echo "uClibc runtime dir missing: $(MARUKO_UCLIBC_DIR)"; exit 1; }; \
	fi

lint: $(TOOLCHAIN_TARGET) check
	$(CC) $(CFLAGS) -Wall -Wextra -Werror -Wno-unused-parameter -Wno-old-style-declaration -fsyntax-only $(SRC)

$(TARGET): $(SRC) include/backend.h include/codec_config.h include/codec_types.h include/file_util.h include/h26x_param_sets.h include/h26x_util.h include/isp_runtime.h include/maruko_bindings.h include/maruko_config.h include/maruko_controls.h include/maruko_output.h include/maruko_pipeline.h include/maruko_runtime.h include/maruko_video.h include/rtp_packetizer.h include/rtp_session.h include/rtp_sidecar.h include/sdk_quiet.h include/star6e_audio.h include/star6e_controls.h include/star6e_cus3a.h include/star6e_hevc_rtp.h include/star6e_output.h include/star6e_pipeline.h include/star6e_recorder.h include/star6e_ts_recorder.h include/ts_mux.h include/audio_ring.h include/star6e_runtime.h include/star6e_video.h include/stream_metrics.h include/venc_config.h include/venc_httpd.h include/venc_api.h include/sensor_select.h include/venc_ring.h include/star6e.h include/sigmastar_types.h include/ssc338q_compat.h include/imu_bmi270.h include/eis.h include/eis_ring.h include/eis_gyroglide.h include/debug_osd.h $(if $(filter 1,$(BUILD_MARUKO_SHIM)),$(MARUKO_SHIM_SO),)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRC) -L$(DRV) $(if $(DRV_EXTRA),-L$(DRV_EXTRA),) -Ltools $(BASE_LIBS) $(SOC_LIBS) -o $@

$(TEST_TARGET): $(TEST_SRC) include/star6e.h include/sigmastar_types.h include/ssc338q_compat.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(TEST_SRC) -L$(DRV) $(if $(DRV_EXTRA),-L$(DRV_EXTRA),) $(BASE_LIBS) $(SOC_LIBS) -o $@

$(PROBE_TARGET): $(PROBE_SRC) include/star6e.h include/sigmastar_types.h include/ssc338q_compat.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(PROBE_SRC) -L$(DRV) $(if $(DRV_EXTRA),-L$(DRV_EXTRA),) $(BASE_LIBS) $(SOC_LIBS) -o $@

# Host-native timing probe (no cross-compiler or SDK libs needed)
$(TIMING_PROBE_TARGET): $(TIMING_PROBE_SRC) include/rtp_sidecar.h
	$(HOST_CC) -std=c99 -Wall -Wextra -O2 -D_GNU_SOURCE -Iinclude $(TIMING_PROBE_SRC) -lm -o $@

$(MARUKO_SHIM_SO): $(MARUKO_SHIM_SRC)
	$(CC) -shared -fPIC -O2 $(MARUKO_SHIM_SRC) -o $(MARUKO_SHIM_SO)

stage: build
	mkdir -p $(OUT_DIR)/lib
	cp -f $(DRV)/*.so $(OUT_DIR)/lib/
	@if [ -n "$(DRV_EXTRA)" ]; then cp -f "$(DRV_EXTRA)"/*.so $(OUT_DIR)/lib/; fi
ifeq ($(BUILD_MARUKO_SHIM),1)
	cp -f $(MARUKO_SHIM_SO) $(OUT_DIR)/lib/libmaruko_uclibc_shim.so
	cp -f $(MARUKO_UCLIBC_DIR)/* $(OUT_DIR)/lib/
endif

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
                tests/test_isp_runtime.c tests/test_rtp_session.c \
                tests/test_stream_metrics.c \
                tests/test_star6e_hevc_rtp.c tests/test_star6e_output.c \
                tests/test_star6e_audio.c tests/test_star6e_video.c \
                tests/test_star6e_recorder.c \
                tests/test_ts_mux.c tests/test_audio_ring.c \
                tests/test_star6e_ts_recorder.c \
                tests/test_backend.c
# Production sources compiled into the test binary (pure-logic modules only).
# sensor_select.c is included here; its MI_SNR_* deps are stubbed in test_sensor_select.c.
TEST_LIB_SRCS := src/backend.c src/venc_config.c src/venc_api.c src/venc_httpd.c src/venc_webui.c src/sensor_select.c src/venc_ring.c src/file_util.c src/h26x_util.c src/h26x_param_sets.c src/isp_runtime.c src/maruko_config.c src/codec_config.c src/pipeline_common.c src/rtp_session.c src/sdk_quiet.c src/rtp_packetizer.c src/star6e_hevc_rtp.c src/star6e_output.c src/star6e_audio.c src/star6e_video.c src/star6e_recorder.c src/star6e_ts_recorder.c src/ts_mux.c src/rtp_sidecar.c src/stream_metrics.c lib/cJSON.c

$(TEST_RUNNER): $(TEST_SRCS) $(TEST_LIB_SRCS) tests/test_helpers.h include/backend.h include/h26x_param_sets.h include/isp_runtime.h include/maruko_config.h include/pipeline_common.h include/rtp_packetizer.h include/rtp_session.h include/rtp_sidecar.h include/star6e_audio.h include/star6e_hevc_rtp.h include/star6e_output.h include/star6e_recorder.h include/star6e_ts_recorder.h include/ts_mux.h include/audio_ring.h include/star6e_video.h include/stream_metrics.h
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_TEST_EXTRA) $(TEST_SRCS) $(TEST_LIB_SRCS) -lpthread -ldl -o $@

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

TEST_EIS_GG := tests/test_eis_gyroglide
$(TEST_EIS_GG): tests/test_eis_gyroglide.c src/eis_gyroglide.c src/eis.c include/eis.h include/eis_gyroglide.h include/eis_ring.h
	$(HOST_CC) $(HOST_CFLAGS) -DEIS_GYROGLIDE_TEST tests/test_eis_gyroglide.c src/eis_gyroglide.c src/eis.c -lpthread -lm -o $@

test-eis: $(TEST_EIS_GG)
	./$(TEST_EIS_GG)

test-ci: test test-eis test-asan test-tsan

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

remote-test:
	SOC_BUILD=$(SOC_BUILD) ./scripts/remote_test.sh $(ARGS)

# ── Verification targets ──────────────────────────────────────────────

STAR6E_BINS := out/star6e/venc out/star6e/snr_toggle_test out/star6e/snr_sequence_probe
MARUKO_BINS := out/maruko/venc

verify:
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
	rm -f $(MARUKO_SHIM_SO)
	rm -f $(TEST_RUNNER)
	rm -f .build_soc
