# Star6E (Infinity6E) MI vendor libs

Runtime reference set for the Star6E backend. These thirteen `.so` files are the
libraries the `venc-star6e` binary `dlopen()`s at runtime. Stock OpenIPC
Infinity6E firmware normally ships them in `/rom/usr/lib/`, so the Star6E build
links none of them (`DRV :=` in the `Makefile`; everything is resolved via
`dlopen` against the firmware copies). This directory is kept so we can:

1. verify the binary's full runtime dependency set in one place, and
2. push an individual lib to a device whose firmware is missing it — most
   importantly `libmi_ive.so`, required by image stabilization but not present
   in every Infinity6E firmware drop.

Use `scripts/star6e_direct_deploy.sh push-libs` to copy this set to a device's
`/usr/lib/` (overlay), which shadows `/rom/usr/lib/`.

## What's here

| File | dlopen'd by | Purpose |
|---|---|---|
| libmi_sys.so | star6e_mi.c | MI system base |
| libmi_vif.so | star6e_mi.c | Video input interface |
| libmi_vpe.so | star6e_mi.c | Video process / scaler (Star6E uses VPE) |
| libmi_sensor.so | star6e_mi.c | Sensor driver interface |
| libmi_venc.so | star6e_mi.c | Video encoder |
| libcam_os_wrapper.so | star6e_mi.c | OS wrapper |
| libispalgo.so | star6e_mi.c | ISP algorithms |
| libcus3a.so | star6e_mi.c / star6e_cus3a.c | Custom 3A (AE/AWB/AF) |
| libmi_isp.so | star6e_mi.c / isp_runtime.c / star6e_controls.c | ISP pipeline |
| libmi_ai.so | star6e_audio.c | Audio input (Opus/G.711/PCM capture) |
| libmi_iqserver.so | (IQ tooling) | IQ server |
| **libmi_ive.so** | star6e_pipeline.c | **Software IVE motion detector — image stabilization** |
| **libmi_rgn.so** | debug_osd.c | **RGN region overlay — debug OSD** |

`libmi_ive.so` only `NEEDED`s `libc`/`libgcc_s`; `libmi_rgn.so` only `NEEDED`s
`libc`. Both are self-contained relative to the rest of this set.

## Not bundled here (resolved elsewhere at runtime)

- `libopus.so` — third-party codec; the audio backend `dlopen`s the system copy
  shipped by firmware. Not an MI vendor lib.
- `libive.so` — a fallback name `star6e_pipeline.c` tries after `libmi_ive.so`;
  does not exist on Infinity6E firmware (the real name is `libmi_ive.so`).

## Provenance

The original eleven libs predate this README (promoted to this location in the
standalone-repo flatten; exact SDK vintage not recorded).

`libmi_ive.so` and `libmi_rgn.so` were pulled 2026-05-23 from the Star6E test
device (ssc338q / imx335 @ 192.168.1.13) `/rom/usr/lib/`, the device the
image-stabilization work was verified on. See
`documentation/STABILIZATION_TEST_PLAN.md`.

> Caveat — mixed vintage: three of the original libs
> (`libcam_os_wrapper.so`, `libmi_ai.so`, `libmi_sys.so`) have different md5s
> than the same files in that device's `/rom/usr/lib/`, so this set is not a
> single coherent SDK snapshot. The two newly added libs are self-contained
> (`libc`/`libgcc_s` only), so this does not affect them, but refresh the whole
> set from one firmware drop before relying on `push-libs` for the core MI libs.

MD5s are recorded in `MD5SUMS` alongside this README.

## Updating

When the vendor SDK is refreshed, replace each `.so` with the new version from
the matching OpenIPC Infinity6E firmware, regenerate `MD5SUMS` with
`md5sum *.so > MD5SUMS`, and verify stabilization (`framing=stab`) + OSD +
`/api/v1/restart` still work on a live Star6E device before committing.
