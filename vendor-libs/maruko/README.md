# Maruko (Infinity6C) MI vendor libs

Runtime bundle for the Maruko backend. These ten `.so` files are `dlopen()`ed
by the `venc-maruko` binary and are **not** present in stock OpenIPC firmware
for Infinity6C (verified on SSC378QE, kernel 5.10.61, firmware built 2026-02-22).

## What's here

| File | Size | Purpose |
|---|---|---|
| libmi_sys.so | 81832 | MI system base |
| libmi_isp.so | 549068 | ISP pipeline |
| libmi_venc.so | 183912 | Video encoder |
| libmi_scl.so | 39588 | Scaler (Maruko uses SCL, Star6E uses VPE) |
| libmi_sensor.so | 47972 | Sensor driver interface |
| libmi_vif.so | 38368 | Video input interface |
| libmi_common.so | 11992 | Common MI helpers |
| libispalgo.so | 934648 | ISP algorithms |
| libcam_os_wrapper.so | 69340 | OS wrapper (uClibc→musl shim replacement) |
| libcus3a.so | 128212 | Custom 3A (AE/AWB/AF) |

Total: ~2.1 MB.

## Provenance (2026-04-22)

Pulled from a known-good Maruko test device (SSC378QE @ 192.168.2.12) overlay
partition (`/overlay/root/usr/lib/`). MD5s are recorded in `MD5SUMS` alongside
this README. These files were originally staged there during the pre-v0.7.0
uClibc-shim era and have been hardware-verified across multiple venc releases
(IMX415 @ 1920×1080 @ 120 fps H.265).

## Not included (intentionally)

- `libmi_rgn.so` — Maruko binary does not compile `debug_osd.c`, so RGN overlays
  are unused. Star6E-only.
- `libmi_vpe.so` — Maruko uses SCL for scaling; VPE is Star6E-only.
- `libmi_ai.so` / `libopus.so` — audio capture is not compiled into the Maruko
  backend (`maruko_runtime.c` only warns if `audio.enabled` is set).
- `libmaruko_uclibc_shim.so` / `ld-uClibc.so.1` — dead since v0.7.0. `venc`,
  `waybeam_hub`, and `majestic` are all musl-linked now.

## Star6E does not use this directory

Stock OpenIPC Infinity6E firmware ships all required MI libs in `/rom/usr/lib/`
(verified on SSC30KQ @ 192.168.2.13). The Star6E release tarball is intentionally
library-free. See `.github/workflows/release.yml` release body for runtime
requirements per backend.

## Updating

When the vendor SDK is refreshed, replace each `.so` in this directory with the
new version from the matching OpenIPC firmware or vendor drop, then regenerate
`MD5SUMS` with `md5sum *.so > MD5SUMS` and verify `/api/v1/restart` + IDR flow
still works on a live Maruko device before committing.
