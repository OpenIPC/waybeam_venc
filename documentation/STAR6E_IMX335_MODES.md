# Star6E IMX335 Mode Lineup

Status: **device-verified 2026-07-05** on SSC338Q (Infinity6E, kernel 4.9.84)
+ IMX335, 4-lane MIPI (bench box 192.168.1.13). Covers the in-tree driver
`drivers/sensor_imx335_star6e.c`, built via `make drivers-star6e` and staged
to `sensors/star6e/sensor_imx335_star6e.ko`.

This is the Star6E counterpart to the Maruko lineup
(`MARUKO_IMX335_MODES.md`): same "one best mode per fps tier, window-crop,
explicit-geometry" philosophy, seeded from the **stock OpenIPC infinity6e
blueprint** (which ships on the .13 box). The four stock linear tables are
preserved verbatim; two Maruko-style window-crop tiers (100 / 144 fps) are
added and the whole set is presented **in ascending fps order**. HDR/DOL is
**not exposed** (the two HDR handles are `NULL`; the compiler dead-code-
eliminates the HDR init/AE subtree), and there is no 50fps mode.

## Why an in-tree Star6E driver

For Maruko the repo already owns a customizable sensor driver
(`drivers/sensor_imx335_maruko.c`). Star6E previously had only prebuilt stock
`.ko` in `sensors/star6e/`. This adds the same buildable, extendable driver
for Star6E — `make drivers-star6e KSRC_STAR6E=<i6e-kernel>` — so the Star6E
mode lineup is owned here and can be tuned like Maruko's.

## Mode table (final lineup — fps-ordered)

Modes are ordered by ascending fps. idx 0/1/2/4 are the stock infinity6e
linear tables (verbatim); idx 3 (100fps) and idx 5 (144fps) are the new
window-crop tiers, pushed to the **highest FOV the I6E ISP sustains** — the
Maruko I6C ceiling model does **not** apply to I6E (see below).

| Idx | Resolution | fps | Aspect | MPix/s | Init function | Origin |
|---|---|---|---|---|---|---|
| 0 | 2560×1920 | 30  | 4:3  | 147 | `pCus_init_mipi4lane_5m30fps_linear`  | stock (full readout) |
| 1 | 2560×1920 | 60  | 4:3  | 295 | `pCus_init_mipi4lane_5m60fps_linear`  | stock |
| 2 | 2560×1440 | 90  | 16:9 | 332 | `pCus_init_mipi4lane_5m90fps_linear`  | stock |
| 3 | 2176×1224 | 100 | 16:9 | 266 | `pCus_init_window_2176x1224`          | **NEW** window crop |
| 4 | 1920×1080 | 120 | 16:9 | 249 | `pCus_init_mipi4lane_5m120fps_linear` | stock |
| 5 | 1600×900  | 144 | 16:9 | 207 | `pCus_init_window_1600x900`           | **NEW** window crop |

## Device verification (2026-07-05, .13)

Test method: pin `video0.fps=144` and `video0.size=auto`, then switch only
`sensor.mode` — each mode clamps to its own max fps. The **true sensor+ISP
capture rate** is the VIF FPS column in `/proc/mi_modules/mi_vif/mi_vif0`
(the `Src/Dst`-ratio row), **not** the AE `sensor_fps` (which is the *set*
value). `dmesg` watched for sustained FIFO-FULL / Skip-IQ / FrmLost.

| Idx | Target fps | Measured VIF FPS | Sustained drops |
|---|---|---|---|
| 0 | 30  | 29.95  | 0 |
| 1 | 60  | 60.03  | 0 |
| 2 | 90  | 89.56  | 0 (one-shot FIFO-FULL on reinit only) |
| 3 | 100 | 99.80  | 0 |
| 4 | 120 | 119.40 | 0 |
| 5 | 144 | 143.28 | 0 |

Re-verified 2026-07-09 on `.201` after the fixed-framerate exposure clamp +
vts trims (v0.38.0): mode 2 = 90.00±0.09, mode 3 = 100.00, mode 4 =
120.0±0.2, mode 5 = **143.99 steady** (encoder `Fps_1s` 144.00). The
pre-trim shortfalls in the table above were the stock VMAX-extension
behavior + the ~0.2–0.5% real-clock deficit, both now corrected.

Every mode came up clean on the first try — no per-mode tuning. Mode
switching triggers its own pipeline reinit (`reinit_pending`), so setting
`sensor.mode` alone is sufficient; do **not** also issue `/restart` (it
collides with the pending reinit and returns `paused`).

## I6E ISP throughput ceiling (key finding)

The I6E IMX335 sustained-fps ISP ceiling is **≈ 2.66 MPix @100fps and
≈ 1.44–1.55 MPix @144fps** — it has *more* headroom at 100 than the Maruko
I6C model predicts, and little at 144. Over budget, the ISP silently **halves**
(delivers every other frame) with **0** fifo/skip/lost logged — so the VIF FPS
column is the only reliable signal.

Probe results that set idx 3 and idx 5 (measured VIF on .13):

| Candidate | fps | MPix | VIF | Verdict |
|---|---|---|---|---|
| 2560×1440 | 100 | 3.69 | 49.6 | halved — over ceiling |
| **2176×1224** | **100** | **2.66** | **99.5** | **chosen (100 tier)** |
| 2048×1152 | 100 | 2.36 | 64–94 | unstable |
| 1792×1008 | 144 | 1.81 | 73 | halved |
| 1664×936  | 144 | 1.56 | 71.6 | halved |
| **1600×900** | **144** | **1.44** | **143** | **chosen (144 tier)** |

Stock references: 2560×1440@90 = 89.3, 1920×1080@120 = 119.6. (The mode-2
entry was long mislabeled 2400×1350 — the stock 90fps table reads out
2560×1440, Y_OUT=1460.)

## Window-crop geometry

Crops reuse the proven `Sensor_init_table_4lane_5m120fps` analog/PLL base and
override only the readout window + HMAX. The stock 120fps table **is** the
Maruko window formula for W=1920 (verified byte-exact), so the same formula
generates every crop:

```
HTRIM_start = 48 + (2592 - W) / 2      0x302C/2D
HNUM        = W + 24                    0x302E/2F
Y_OUT_SIZE  = H                         0x3056/57
AREA3_start = 176 + (1944 - H)          0x3074/75
AREA3_width = 2 × H                     0x3076/77
HMAX        = 275 (all crops)           0x3034/35
```

`imx335_init_window_crop()` writes the base 120fps table minus its final
standby-exit (`0x3000=0x00`, `0x3002=0x00`), then the geometry override **in
standby**, then re-issues the standby-exit — latching the window before
streaming so a warm mode switch never inherits stale geometry (PR#156
discipline).

## Runtime pacing (VMAX / vts_30fps)

VMAX is **not** baked into the crop geometry tables; it is applied at runtime
via `vts_30fps` in `pCus_SetVideoRes`, so one geometry table serves all
pacings. The per-mode `vts_30fps` seed derives from the 120fps line constant:

```
K = HMAX × VMAX × max_fps = 275 × 2256 × 120 = 74,448,000   (shared i6c↔i6e)
vts_30fps(mode) = K / (HMAX × max_fps)
Preview_line_period ∝ HMAX   (3694 ns at HMAX=275)
```

Crop seeds in the driver: idx 3 (100fps) `vts_30fps=2701`, idx 5 (144fps)
`vts_30fps=1875`; both `Preview_line_period=3694`.

### Empirical vts trim (nominal-fps landing)

The real pixel clock runs ~0.2–0.5% below the K constant, so nominal vts
seeds deliver slightly under the labeled fps (e.g. 143.7 at vts 1880, 119.7
at 2256). The seeds for modes 2/3/4/5 are trimmed a few lines below nominal
(3016→3001, 2707→2701, 2256→2250, 1880→1875) so the **delivered** rate lands
at/just above the label. Modes 0/1 measured at/over nominal and keep stock
seeds.

## Fixed-framerate exposure policy

The stock driver extends VMAX whenever AE requests an exposure longer than
the frame budget (`SetAEUSecs`: `vts = lines + 1`; `SetFPS`:
`vts = expo_lines + 8`). With AE pinned at the 1/fps shutter ceiling (any
indoor scene at 144fps) this held VMAX at 1887 instead of 1880 — delivered
fps sagged to ~143.4 and **wandered with scene brightness** (142–143.5
observed). Both sites now clamp exposure to `vts - 9` (the SHR floor) and
never touch VMAX: frame rate is constant by construction, and AE compensates
the ~0.5% exposure loss with gain. Device-verified: VMAX register pinned at
the seed value, encoder `Fps_1s` = 144.00 sustained.

## Orientation (flip) per-mode OB rewrites

The stock M0F1/M1F1 orientation path rewrites `AREA3_ST_ADR` (0x3074/75) and
the OB block (0x30C6/0x30CE/0x30D8) per mode, following
`AREA3_ST_flipped = 4288 − AREA3_ST_normal`. The inherited block only covered
stock indices 0–3 with stock geometry: our idx 3 (2176×1224) was getting the
stock 1080p value (3248 instead of 3392) and idx 4/5 got **no flip writes at
all** (not even the mirror/flip base registers). Fixed: idx 3 → 3392,
idx 4 → 3248, idx 5 → 3068, all with the cropped-mode OB values
(0x30C6=0x12, 0x30CE=0x64). Flip in modes 3–5 is code-verified against the
formula but not yet camera-verified (bench runs flip=off).

## Full-res mode 0 all-pixel reset

Because the crops set window mode (`0x3018=0x04`), the full-res mode 0 table
writes an **explicit all-pixel reset** (`0x3018=0x00` + full-frame
HTRIM/HNUM/Y_OUT) so a warm switch from a crop mode back to mode 0 restores the
full readout. On cold boot this block is byte-equivalent to the stock table
(`0x3018=0x00` is the power-on default).

## Deploying a driver change

The `.ko` is selected at boot by `S94sensor-detect` (`lsmod | grep
sensor_imx335`) and loaded by the MI framework, which binds pads — an
`rmmod`/`insmod` does not re-run that binding, so a driver swap needs a
file-replace + reboot. The .13 rootfs is squashfs + overlay, so a single write
copies-up and persists:

```
# cross-built .ko from sensors/star6e/sensor_imx335_star6e.ko, then:
arm-linux-strip --strip-debug sensor_imx335_star6e.ko          # ~15 KB
cat sensor_imx335_star6e.ko | \
  ssh root@<star6e> 'cat > /lib/modules/4.9.84/sigmastar/sensor_imx335_mipi.ko'
# then reboot (prefer graceful `reboot`; power-cycle if enumeration wedges)
```

Build: `make drivers-star6e KSRC_STAR6E=<i6e-4.9.84-kernel-src>`. Use the
OpenIPC builder output (`builder/openipc/output/build/linux-custom`) that
produced the device firmware so module **vermagic matches** the target
(`4.9.84 SMP preempt mod_unload ARMv7 thumb2 p2v8`). The modpost `undefined!`
warnings for `DrvSensor*`/`DrvVif_*`/`intlog10` are expected — those resolve
from the proprietary `mhal`/`mi` modules at load time.

A stock-driver backup pulled from .13 is kept at
`sensors/star6e/BACKUP_13_sensor_imx335_mipi.ko` (21392 B, md5
`fd52af6111b28241c0a870b828f24081`) for rollback.
