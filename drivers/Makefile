# Maruko sensor driver build (IMX415, IMX335)
#
# Build: make -C drivers sensor KSRC=/path/to/infinity6c-kernel
#                               [CROSS=/path/to/cross-prefix-]
# Requires: an Infinity6C 5.10.61 kernel source tree and the I6C cross-
# toolchain. Neither is hosted in this repo; supply them via KSRC/CROSS
# or via the top-level `make drivers-maruko KSRC_MARUKO=...` wrapper.
#
# Produces sensor_imx415_maruko.ko and sensor_imx335_maruko.ko.
# Deploy the one matching your sensor to /lib/modules/5.10.61/sigmastar/
# as sensor_imx415_mipi.ko or sensor_imx335_mipi.ko.
# A reboot is required after replacing the driver.

THIS_DIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))

KSRC     ?=
CROSS    ?= $(abspath $(THIS_DIR)/../toolchain/toolchain.sigmastar-infinity6c/bin/arm-openipc-linux-musleabihf-)
I6C_INC  := $(realpath $(THIS_DIR)/../sensors-src/sigmastar/infinity6c/include)

EXTRA_CFLAGS += -DSIGMASTAR_MARUKO
EXTRA_CFLAGS += -I$(I6C_INC) -I$(KSRC)/drivers/sstar/include
EXTRA_CFLAGS += -Wno-unused-function

obj-m := sensor_config.o sensor_imx415_maruko.o sensor_imx335_maruko.o

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
		echo "       Pass KSRC=/path/to/infinity6c-kernel on the command line," >&2; \
		echo "       or invoke from the top level as 'make drivers-maruko KSRC_MARUKO=...'." >&2; \
		exit 1; \
	fi
	@if [ ! -f "$(KSRC)/Makefile" ] || [ ! -d "$(KSRC)/arch/arm" ]; then \
		echo "ERROR: KSRC=$(KSRC) does not look like a kernel source tree" >&2; \
		echo "       (missing Makefile or arch/arm)." >&2; \
		exit 1; \
	fi

.PHONY: sensor clean check-ksrc
