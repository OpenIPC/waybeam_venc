ROOT ?= ..

SOC_BUILD ?= star6e

STAR6E_CC ?= $(ROOT)/toolchain/toolchain.sigmastar-infinity6e/bin/arm-openipc-linux-gnueabihf-gcc
MARUKO_CC ?= $(ROOT)/toolchain/toolchain.sigmastar-infinity6c/bin/arm-openipc-linux-musleabihf-gcc

STAR6E_DRV ?= $(ROOT)/star6e-standalone/libs/star6e
MARUKO_DRV ?= $(ROOT)/star6e-standalone/libs/maruko
MARUKO_UCLIBC_DIR ?= $(ROOT)/star6e-standalone/libs/maruko/uclibc
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

VENC_VERSION := $(shell cat $(ROOT)/VERSION 2>/dev/null || echo unknown)
COMMON_CFLAGS := -Os -s -Iinclude -Ilib -include include/ssc338q_compat.h -DVENC_VERSION=\"$(VENC_VERSION)\"
CONFIG_SRC := src/venc_config.c src/venc_httpd.c src/venc_api.c src/sensor_select.c src/venc_ring.c lib/cJSON.c
HELPER_SRC := src/backend.c src/file_util.c src/h26x_util.c src/h26x_param_sets.c src/codec_config.c src/pipeline_common.c src/sdk_quiet.c src/rtp_adapt.c src/rtp_packetizer.c src/isp_runtime.c src/rtp_session.c src/stream_metrics.c src/rtp_sidecar.c
MARUKO_ONLY_SRC := src/maruko_config.c src/maruko_video.c src/maruko_controls.c src/maruko_output.c src/maruko_pipeline.c src/maruko_runtime.c
STAR6E_ONLY_SRC := src/star6e_output.c src/star6e_audio.c src/star6e_hevc_rtp.c src/star6e_video.c src/star6e_pipeline.c src/star6e_controls.c src/star6e_runtime.c src/star6e_cus3a.c src/star6e_recorder.c src/star6e_ts_recorder.c src/ts_mux.c src/imu_bmi270.c src/eis_crop.c
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
SOC_DEFS := -DPLATFORM_STAR6E -DPLATFORM_MARUKO -DHAVE_BACKEND_MARUKO=1
SOC_LDFLAGS := -Wl,-rpath-link,$(DRV) -Wl,--unresolved-symbols=ignore-in-shared-libs -Wl,-rpath-link,tools
ifneq ($(strip $(DRV_EXTRA)),)
SOC_LDFLAGS += -Wl,-rpath-link,$(DRV_EXTRA)
endif
SOC_LIBS := -lmi_common -lmaruko_uclibc_shim -lm
BUILD_TESTS := 0
BUILD_MARUKO_SHIM := 1
else ifeq ($(SOC_BUILD),star6e)
CC := $(STAR6E_CC)
SRC := src/main.c src/backend_star6e.c $(STAR6E_ONLY_SRC) $(HELPER_SRC) $(CONFIG_SRC)
DRV := $(STAR6E_DRV)
DRV_EXTRA :=
SOC_DEFS := -DPLATFORM_STAR6E -DHAVE_BACKEND_STAR6E=1
SOC_LDFLAGS := -Wl,-rpath-link,$(DRV)
SOC_LIBS := -lm
BUILD_TESTS := 1
BUILD_MARUKO_SHIM := 0
else
$(error Unsupported SOC_BUILD '$(SOC_BUILD)'; expected 'star6e' or 'maruko')
endif

CFLAGS += $(COMMON_CFLAGS) $(SOC_DEFS)
LDFLAGS += $(COMMON_LDFLAGS) $(SOC_LDFLAGS)

.PHONY: all clean check lint stage print-config test test-werror test-asan test-tsan test-ci

all: check check-soc-stamp | $(OUT_DIR)
all: $(TARGET)
ifeq ($(BUILD_TESTS),1)
all: $(TEST_TARGET) $(PROBE_TARGET)
endif
ifeq ($(BUILD_MARUKO_SHIM),1)
all: $(MARUKO_SHIM_SO)
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

lint: check
	$(CC) $(CFLAGS) -Wall -Wextra -Werror -Wno-unused-parameter -Wno-old-style-declaration -fsyntax-only $(SRC)

$(TARGET): $(SRC) include/backend.h include/codec_config.h include/codec_types.h include/file_util.h include/h26x_param_sets.h include/h26x_util.h include/isp_runtime.h include/maruko_bindings.h include/maruko_config.h include/maruko_controls.h include/maruko_output.h include/maruko_pipeline.h include/maruko_runtime.h include/maruko_video.h include/rtp_adapt.h include/rtp_packetizer.h include/rtp_session.h include/rtp_sidecar.h include/sdk_quiet.h include/star6e_audio.h include/star6e_controls.h include/star6e_cus3a.h include/star6e_hevc_rtp.h include/star6e_output.h include/star6e_pipeline.h include/star6e_recorder.h include/star6e_ts_recorder.h include/ts_mux.h include/audio_ring.h include/star6e_runtime.h include/star6e_video.h include/stream_metrics.h include/venc_config.h include/venc_httpd.h include/venc_api.h include/sensor_select.h include/venc_ring.h include/star6e.h include/sigmastar_types.h include/ssc338q_compat.h include/imu_bmi270.h include/eis_crop.h $(if $(filter 1,$(BUILD_MARUKO_SHIM)),$(MARUKO_SHIM_SO),)
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

stage: all
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
                tests/test_rtp_adapt.c tests/test_rtp_packetizer.c \
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
TEST_LIB_SRCS := src/backend.c src/venc_config.c src/venc_api.c src/venc_httpd.c src/sensor_select.c src/venc_ring.c src/file_util.c src/h26x_util.c src/h26x_param_sets.c src/isp_runtime.c src/maruko_config.c src/codec_config.c src/pipeline_common.c src/rtp_session.c src/sdk_quiet.c src/rtp_adapt.c src/rtp_packetizer.c src/star6e_hevc_rtp.c src/star6e_output.c src/star6e_audio.c src/star6e_video.c src/star6e_recorder.c src/star6e_ts_recorder.c src/ts_mux.c src/rtp_sidecar.c src/stream_metrics.c lib/cJSON.c

$(TEST_RUNNER): $(TEST_SRCS) $(TEST_LIB_SRCS) tests/test_helpers.h include/backend.h include/h26x_param_sets.h include/isp_runtime.h include/maruko_config.h include/pipeline_common.h include/rtp_adapt.h include/rtp_packetizer.h include/rtp_session.h include/rtp_sidecar.h include/star6e_audio.h include/star6e_hevc_rtp.h include/star6e_output.h include/star6e_recorder.h include/star6e_ts_recorder.h include/ts_mux.h include/audio_ring.h include/star6e_video.h include/stream_metrics.h
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

test-ci: test test-asan test-tsan

clean:
	rm -rf out/star6e out/maruko
	rm -f $(TIMING_PROBE_TARGET)
	rm -f $(MARUKO_SHIM_SO)
	rm -f $(TEST_RUNNER)
	rm -f .build_soc
