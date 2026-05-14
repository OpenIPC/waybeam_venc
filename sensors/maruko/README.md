# Maruko (Infinity6C) sensor kernel modules

Source-built `sensor_imx*_maruko.ko` files for the Maruko (SigmaStar
Infinity6C, SSC378QE) platform.  Verified on the bench at
`192.168.2.12` (kernel 5.10.61, OpenIPC 2.6.02.22).

## What's here

| File | Size | Source |
|---|---|---|
| `sensor_imx335_maruko.ko` | 157 KB | `make drivers-maruko KSRC_MARUKO=...` from `drivers/sensor_imx335_maruko.c` |
| `sensor_imx415_maruko.ko` | 163 KB | `make drivers-maruko KSRC_MARUKO=...` from `drivers/sensor_imx415_maruko.c` |

MD5s are recorded in `MD5SUMS`.

## Why these are vendored

Stock OpenIPC Infinity6C firmware ships `sensor_imx*_mipi.ko` in
`/lib/modules/5.10.61/sigmastar/`, but those are upstream OpenIPC
drivers — not the *modified* drivers built from the C sources in
`drivers/`.  The release tarball was previously sensor-driver-free,
so any device installed from the release ended up running stock
OpenIPC drivers regardless of the work in `drivers/sensor_*.c`.

Vendoring the source-built `.ko` here lets `make stage` and the
GitHub release pipeline ship the modified driver as part of the
tarball — drop-in replacement for the stock `_mipi.ko` on install.

## Install (manual)

The deploy script (`scripts/maruko_direct_deploy.sh push-drivers`)
renames `_maruko.ko` → `_mipi.ko` on the target so the kernel module
loader can find them by their canonical name.  For manual install
from the release tarball, the staged files are already renamed to
`_mipi.ko` for drop-in compatibility:

```sh
cp /tmp/venc-maruko/drivers/*.ko /lib/modules/$(uname -r)/sigmastar/
depmod -a
reboot
```

## Updating

Rebuild from source and re-vendor when `drivers/sensor_*.c` changes:

```sh
# Build .ko against the Infinity6C kernel source
make drivers-maruko KSRC_MARUKO=/path/to/i6c-kernel

# Copy the freshly-built modules in over the vendored ones
cp drivers/sensor_imx335_maruko.ko sensors/maruko/
cp drivers/sensor_imx415_maruko.ko sensors/maruko/

# Refresh checksums
( cd sensors/maruko && md5sum sensor_imx*_maruko.ko > MD5SUMS )

# Verify on bench before committing
scripts/maruko_direct_deploy.sh push-drivers --reboot-after
```

## Not vendored (intentionally)

- `sensor_*_mipi.ko` — stock OpenIPC drivers; users keep whatever
  shipped with their firmware unless our `_maruko.ko` replaces them.
- `sensor_config.ko`, `sensor_gc4653_mipi.ko`, `sensor_os04a10_mipi.ko`,
  `sensor_sc*_mipi.ko` — non-IMX sensors; not currently supported by
  `waybeam_venc` and not relevant to the FPV bench setup.
