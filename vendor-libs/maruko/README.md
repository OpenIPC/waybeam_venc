# Maruko (Infinity6C) MI vendor libs

Runtime bundle for the Maruko backend. These twelve `.so` files are `dlopen()`ed
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
| libmi_ai.so | 110660 | Audio input (Phase 5, v0.9.15+) |
| libmi_ao.so | 85232 | Audio output (reserved for Phase 5b — playback path) |

Total: ~2.3 MB.

## Provenance

Original ten libs (2026-04-22): pulled from a known-good Maruko test device
(SSC378QE @ 192.168.2.12) overlay partition (`/overlay/root/usr/lib/`).

Audio additions (2026-05-02, v0.9.15): `libmi_ai.so` and `libmi_ao.so` pulled
from the SDK source tree
(`Maruko_work_dir/SourceCode/.../i6c/ipc/common/uclibc/9.1.0/mi_libs/dynamic/`).
Vermagic / runtime-verified against the same SSC378QE bench: kernel-side audio
support is already compiled into the unified `mi.ko` (2.1 MB module) on the
firmware in use, so no kmod insmod is required — only the userspace lib is
missing in stock OpenIPC.

MD5s are recorded in `MD5SUMS` alongside this README.

## Not included (intentionally)

- `libmi_rgn.so` — Maruko binary does not compile `debug_osd.c`, so RGN overlays
  are unused. Star6E-only.
- `libmi_vpe.so` — Maruko uses SCL for scaling; VPE is Star6E-only.
- `libopus.so` — bench already ships it at `/usr/lib/libopus.so.0.8.0`; the
  audio backend `dlopen`s the system copy at runtime.
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
