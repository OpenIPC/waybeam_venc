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

Linear mode only — `cmos_set_wdr_mode()` rejects every other WDR mode, and HDR
stays deferred by issue #220.

## What the plugin programs

`imx662_cfg.h` holds Sony's power-on sequence — the reserved analog/ADC trim,
the MIPI TX timing, the readout window and the HDR context that linear mode
needs parked. It runs once, in standby, before the per-mode writes, and the
per-mode writes win where the two overlap. That ordering matters for
`0x3A50/51/52`, which keys off ADBIT rather than output depth: the table holds
the 12-bit form and the RAW10 modes overwrite it.

`cmos_get_isp_default()` and `cmos_get_awb_default()` seed the ISP once at init.
The algorithm tables in `imx662_cmos_param.h` are ported from the in-tree
`smart_sc450ai` driver, which targets this same ISP silicon. The blocks that
describe ISP behaviour (gamma, CLUT, demosaic, CAC, anti-false-colour, LDCI,
dehaze, CA) transfer on principle; the noise-fitted ones (bayer_nr, sharpen,
noise_calibration, DRC) are borrowed from a 4 MP sensor and are kept on a
hardware A/B rather than a calibration — a measured IMX662 fit should replace
them. The AWB saturation curve is scaled up from sc450ai's at the low-ISO end.

These are compile-time seeds, not live controls. CV610 has no runtime IQ layer
yet: `/api/v1/iq/get` and `/api/v1/iq/set` return `not_implemented` because the
backend provides no `apply_iq_param`. Adding one (mirroring `src/star6e_iq.c`)
would turn these defaults into an editable starting point.

Reference material for further tuning lives outside this repo, in
`hisilicon/vendor/imx662-docs/`.
