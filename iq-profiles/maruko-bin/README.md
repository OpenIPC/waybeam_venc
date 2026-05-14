# Maruko (Infinity6C) ISP tuning blobs

Sigma­Star ISP `.bin` files containing pre-tuned image-quality
parameters (AE/AWB curves, gamma, color-correction matrices, denoise
profiles).  Loaded into the ISP at pipeline start via
`MI_ISP_*_CmdLoadBinFile`, and reloadable live via the
`isp.sensor_bin` config field (v0.10.6+).

## What's here

| File | Sensor | Size | Source |
|---|---|---|---|
| `imx335.bin`            | IMX335 5 MP   | 135 KB | pulled from working SSC378QE bench `/etc/sensors/` |
| `imx415.bin`            | IMX415 8 MP   | 141 KB | pulled from working SSC378QE bench `/etc/sensors/` |
| `imx415_fpv_api.bin`    | IMX415 8 MP   | 146 KB | FPV-tuned variant from the same bench |

MD5s are recorded in `MD5SUMS`.

## Provenance

Pulled 2026-05-11 from a known-good Maruko (SSC378QE) bench at
`192.168.2.12`, kernel 5.10.61, OpenIPC 2.6.02.22.  The same blobs
that shipped in stock OpenIPC firmware for this bench at that date —
the `_fpv_api` variant is a derivative used for the FPV pipeline
(`isp.sensorBin=/etc/sensors/imx415_fpv_api.bin`).

## Why these are vendored

The release tarball was previously ISP-bin-free, so a fresh-device
install from the release lacked the IQ tuning files venc loads at
startup.  On a stock OpenIPC firmware the files were already in
`/etc/sensors/` so this was invisible — but a fresh device built
from a sysupgrade flash or with a stripped rootfs would fail to
acquire image quality.  Vendoring them lets `make stage` and the
GitHub release pipeline ship them in `isp-bins/` for a complete
drop-in install.

## Install (manual)

From the release tarball:

```sh
cp /tmp/venc-maruko/isp-bins/*.bin /etc/sensors/
```

## Updating

Pull again from a known-good device whenever vendor SDK is refreshed:

```sh
scripts/maruko_pull_artifacts.sh isp-bins        # default host root@192.168.2.12
# or finer:
scripts/maruko_pull_artifacts.sh --host root@<ip> isp-bins
( cd iq-profiles/maruko-bin && md5sum imx*.bin > MD5SUMS )
```

Verify on bench before committing (`scripts/maruko_direct_deploy.sh
push-isp-bin imx415` + a smoke test of the resulting stream).

## Not the same as iq-profiles/*.json

`iq-profiles/*.json` (e.g. `imx335_greg_fpvVII-gpt200.json`) are
**JSON-formatted** parameter exports from venc's IQ API — they live
beside this directory but are loaded via `/api/v1/iq/import` rather
than `MI_ISP_*_CmdLoadBinFile`.  The `.bin` files here are the raw
binary format the ISP firmware reads directly.
