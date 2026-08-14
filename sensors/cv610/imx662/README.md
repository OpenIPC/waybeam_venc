# Sony IMX662 plugin for Hi3516CV610

This is the source-built HiSilicon V5 sensor plugin used by the CV610
backend. It was developed and verified on the SIP-K662C6S board with four
MIPI lanes.

Verified full-frame modes are 1920x1080 at 30 and 60 fps in RAW12, and 90
and 100 fps in RAW10. The 100 fps mode requires the board loader's 27 MHz
sensor-clock profile; the other modes use the IMX662 37.125 MHz profile.

Build it with the same public OpenHisilicon headers and OpenIPC ARMv7 musl
toolchain as the daemon:

```sh
make sensor-cv610 SOC_BUILD=cv610 \
  CV610_CC=/path/to/arm-openipc-linux-musleabi-gcc \
  CV610_SDK_INC=/path/to/openhisilicon
```

Install the result as `/usr/lib/sensors/libsns_imx662.so`. No proprietary
SDK source or binary is stored here; the build consumes the public sensor
interface and `sensor_common.c` from the external OpenHisilicon checkout.

The initial backend supports linear mode only. HDR and broad ISP tuning are
deliberately deferred by issue #220.
