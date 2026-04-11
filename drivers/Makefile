# Maruko IMX415 sensor driver build
#
# Build: make -C drivers sensor
# Requires: kernel source, I6C toolchain
#
# The sensor driver is a kernel module (.ko) that must be deployed
# to /lib/modules/5.10.61/sigmastar/ on the Maruko device.
# A reboot is required after replacing the driver.

KSRC     ?= $(HOME)/dev/waybeam-coordination/Maruko_work_dir/SourceCode/kernel
CROSS    ?= $(HOME)/dev/waybeam-coordination/waybeam_venc/toolchain/toolchain.sigmastar-infinity6c/bin/arm-openipc-linux-musleabihf-
I6C_INC  := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../sensors-src/sigmastar/infinity6c/include)

EXTRA_CFLAGS += -DSIGMASTAR_MARUKO
EXTRA_CFLAGS += -I$(I6C_INC) -I$(KSRC)/drivers/sstar/include
EXTRA_CFLAGS += -Wno-unused-function

obj-m := sensor_config.o sensor_imx415_maruko.o

sensor:
	$(MAKE) ARCH=arm CROSS_COMPILE=$(CROSS) -C $(KSRC) M=$(CURDIR) modules

clean:
	$(MAKE) ARCH=arm CROSS_COMPILE=$(CROSS) -C $(KSRC) M=$(CURDIR) clean

.PHONY: sensor clean
