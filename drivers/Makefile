# Sensor driver build — Maruko (Infinity6C) and Star6E (Infinity6E)
#
# Build via the top-level wrappers (they supply KSRC + CROSS):
#   make drivers-maruko KSRC_MARUKO=/path/to/infinity6c-kernel
#   make drivers-star6e KSRC_STAR6E=/path/to/infinity6e-kernel
#
# Or directly:
#   make -C drivers sensor SOC=maruko KSRC=<i6c-kernel> CROSS=<i6c-prefix->
#   make -C drivers sensor SOC=star6e KSRC=<i6e-kernel> CROSS=<i6e-prefix->
#
# Requires the matching kernel source tree (arch/arm + top-level Makefile)
# and cross-toolchain. Neither kernel tree is hosted in this repo; supply
# them via KSRC*/CROSS or the top-level wrappers.
#
# Maruko  produces sensor_imx415_maruko.ko / sensor_imx335_maruko.ko
#         (5.10.61); deploy as sensor_imx*_mipi.ko.
# Star6E  produces sensor_imx335_star6e.ko (4.9.84); deploy as
#         sensor_imx335_mipi.ko. A reboot is required after replacing.

THIS_DIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))

SOC      ?= maruko
KSRC     ?=
I6C_INC  := $(realpath $(THIS_DIR)/../sensors-src/sigmastar/infinity6c/include)
I6E_INC  := $(realpath $(THIS_DIR)/../sensors-src/sigmastar/infinity6e/include)

ifeq ($(SOC),maruko)
CROSS    ?= $(abspath $(THIS_DIR)/../toolchain/toolchain.sigmastar-infinity6c/bin/arm-openipc-linux-musleabihf-)
EXTRA_CFLAGS += -DSIGMASTAR_MARUKO
EXTRA_CFLAGS += -I$(I6C_INC) -I$(KSRC)/drivers/sstar/include
obj-m := sensor_config.o sensor_imx415_maruko.o sensor_imx335_maruko.o
else ifeq ($(SOC),star6e)
CROSS    ?= $(abspath $(THIS_DIR)/../toolchain/toolchain.sigmastar-infinity6e/bin/arm-openipc-linux-gnueabihf-)
EXTRA_CFLAGS += -I$(I6E_INC) -I$(KSRC)/drivers/sstar/include
obj-m := sensor_config.o sensor_imx335_star6e.o sensor_imx415_star6e.o
else
$(error Unsupported SOC '$(SOC)'; expected 'maruko' or 'star6e')
endif

EXTRA_CFLAGS += -Wno-unused-function

sensor: check-ksrc
	$(MAKE) ARCH=arm CROSS_COMPILE=$(CROSS) -C $(KSRC) M=$(CURDIR) modules

clean: check-ksrc
	$(MAKE) ARCH=arm CROSS_COMPILE=$(CROSS) -C $(KSRC) M=$(CURDIR) clean

# Gate the build on a usable kernel tree. Run from `sensor`/`clean` only
# so plain `make -C drivers` (no target) still tab-completes / lists the
# available recipes without erroring.
check-ksrc:
	@if [ -z "$(KSRC)" ]; then \
		echo "ERROR: KSRC is not set." >&2; \
		echo "       Pass KSRC=/path/to/kernel on the command line," >&2; \
		echo "       or invoke from the top level as 'make drivers-maruko KSRC_MARUKO=...'" >&2; \
		echo "       / 'make drivers-star6e KSRC_STAR6E=...'." >&2; \
		exit 1; \
	fi
	@if [ ! -f "$(KSRC)/Makefile" ] || [ ! -d "$(KSRC)/arch/arm" ]; then \
		echo "ERROR: KSRC=$(KSRC) does not look like a kernel source tree" >&2; \
		echo "       (missing Makefile or arch/arm)." >&2; \
		exit 1; \
	fi

.PHONY: sensor clean check-ksrc
