# Maruko IMX335 Best-Per-FPS Mode Lineup

Status: device-verified July 2026 on SSC378QE (Infinity6C) + IMX335, 4-lane
MIPI. Covers the 6-mode lineup in `drivers/sensor_imx335_maruko.c` and the
REALTIME bind policy in `src/maruko_pipeline.c`. Ported from the IMX415
method (`MARUKO_IMX415_1485_MODES.md`) — same "one best mode per fps tier,
native aspect, ~5% ISP headroom" philosophy, adapted to the IMX335's 4:3
sensor.

## Sensor characteristics

IMX335 is a 5MP **2592x1944 4:3** sensor with **no binning** (unlike the
IMX415's HADD/VADD/ADDMODE). The only way to trade resolution for frame
rate is a **window crop** of the pixel array — read fewer lines/columns,
shorten the line time (HMAX), raise the fps. All modes run the vendor 891
Mbps analog/PLL config; there is no 1485 Mbps double-link step (the IMX335
never needs it — full readout is only 5 MPix and its highest-fps modes are
sub-2.1 MPix crops).

Line clock is **74.25 MHz**, so `line_period_ns = HMAX / 74.25e6` and
`fps = 74.25e6 / (VMAX × HMAX)`. VMAX/fps is applied at runtime through
`vts_30fps` in `pCus_SetVideoRes` (not baked into the geometry tables), so
one geometry table serves all pacings.

## Window mode works on I6C

The driver's historical comment claimed window mode (`0x3018=0x04`) "hangs
the ISP" on I6C and only full all-pixel readout produces frames. **That was
false** — a stale-register artifact, not a hardware limit. Every crop mode
below drives windowed readout and is device-verified. The earlier hangs
came from switching into a windowed table that relied on power-on defaults
for the geometry registers while the sensor still held state from a prior
mode. The fix is the same PR#156 discipline used on IMX415: **write every
geometry register explicitly, in standby, before streaming.**

## The ISP throughput ceiling

The Maruko ISP has a per-frame processing cost that bounds fps as a function
of frame pixels. The model fit against IMX415's proven modes:

```
fps_max(N_pixels) ≈ 1 / (0.0017 + 3.65e-9 × N_pixels)     (~245–274 MPix/s)
```

Each IMX335 mode is sized to sit **~5–8% under** its predicted ceiling so the
pipeline never runs at the ragged edge. Device-measured fps matches the
model:

| Idx | Crop | MPix | Target fps | Model max | Headroom |
|---|---|---|---|---|---|
| 0 | 2592x1944 | 5.04 | 30 | 49.7 | vendor-paced (not ISP-bound) |
| 1 | 2496x1872 | 4.67 | 50 | 53.3 | 6.6% |
| 2 | 2272x1704 | 3.87 | 60 | 63.2 | 5.3% |
| 3 | 1792x1344 | 2.41 | 90 | 95.3 | 5.9% |
| 4 | 1920x1080 | 2.07 | 100 | 108 | 8.0% |
| 5 | 1536x864 | 1.33 | 144 | 153 | 6.1% |

All modes bind **REALTIME** (`VIF->ISP bind: REALTIME` in the log) for
minimum glass-to-glass latency — none need the FRAME_BASE buffering the
IMX415 1485 modes require, because their line bursts stay within the ISP
drain.

### Two walls at 144fps (device-proven, 2026-07-04)

The 144fps mode is bounded by **two independent** ISP limits — the per-frame
*time* model above is only one of them:

1. **Per-frame time** — the model. `T ≈ 1.7 ms + 3.65 ns/px`. At 144fps the
   frame period is 6.94 ms, so the window must satisfy `T < 6.94 ms`, i.e.
   ≤ ~1.42 MPix. **1536x864 (1.33 MP → 6.54 ms)** clears it; **1600x900
   (1.44 MP → 6.96 ms)** misses by ~1% and the ISP Skip-IQs and stalls to 0.
2. **Bandwidth** — ~274 MPix/s. **1920x1080@144 = 298 MPix/s** overflows the
   ISP P0 input FIFO (`FIFO FULL` in dmesg) and collapses to ~26 fps, even
   though its per-frame *time* would be fine.

Both limits are at the ISP **input** (straight off the sensor), so reducing
the SCL/encode output size rescues neither — the **sensor readout window**
itself must shrink. That per-frame cost is **not** 3A: with `WAYBEAM_NO_3A=1`
(3A frozen, `CUS3A_SetRunMode(OFF)`) the 1600x900 window still stalls, so the
fixed ~1.7 ms is vendor ISP pixel processing (demosaic/NR/frame setup), not
the AE/AWB loop. 1536x864 is the largest 16:9 window that clears both walls,
measured **144.0 fps SCL / 143.4 fps VENC, 0 DropCnt / 0 FIFO-FULL / 0
Skip-IQ**.

## Mode table (driver indexes — final lineup)

Modes 0–3 keep the native 4:3 aspect; modes 4–5 are 16:9 low-latency.

| Idx | Resolution | fps | HMAX | Aspect | Init function | Role |
|---|---|---|---|---|---|---|
| 0 | 2592x1944 | 30 | 600 | 4:3 | `pCus_init_mipi4lane_5m30fps_linear` | full-res all-pixel, best IQ / full FOV |
| 1 | 2496x1872 | 50 | 375 | 4:3 | `pCus_init_window_2496x1872` | center crop |
| 2 | 2272x1704 | 60 | 340 | 4:3 | `pCus_init_window_2272x1704` | center crop |
| 3 | 1792x1344 | 90 | 275 | 4:3 | `pCus_init_window_1792x1344` | center crop |
| 4 | 1920x1080 | 100 | 274 | 16:9 | `pCus_init_mipi4lane_5m120fps_linear` | low-latency hero (paced 120fps table) |
| 5 | 1536x864 | 144 | 275 | 16:9 | `pCus_init_window_1536x864` | ultra-low-latency (see "Two walls at 144fps") |

**Breaking index remap.** This lineup dropped two redundant 16:9 modes
(1920x1080@60 and @90 — now served by the higher-res 4:3 crops) and renumbers
everything. Configs with a pinned `sensor.mode` must be updated;
`sensor.mode: -1` (auto) resolves by target width/height/fps and needs no
change.

**Pre-staging 144fps for a mode switch.** A live `video0.fps` request above
the current mode's max is no longer rejected (ceiling `PIPELINE_LIVE_FPS_MAX`
= 144). Set `video0.fps=144` first *while parked in a lower-fps mode*: the
value is clamped to the current mode's max for the immediate rebind (so mode 4
keeps running at 100), but the config now carries 144, so switching to
`sensor.mode: -1` (or mode 5) resolves to the 144fps window instead of the
auto-select clamping the target back down. Without this pre-stage, selecting a
144 mode with the config still at 100fps would enter it at 100.

## Window-crop geometry

Every crop reuses the proven 120fps analog/PLL config
(`Sensor_init_table_4lane_5m120fps`) and overrides only the readout window +
HMAX. All geometry is derived from the working 1920x1080 window mode
(formulas verified exact against it):

```
HTRIM_start = 48 + (2592 - W) / 2      0x302C/2D
HNUM        = W + 24                    0x302E/2F
Y_OUT_SIZE  = H                         0x3056/57
AREA3_start = 176 + (1944 - H)          0x3074/75
AREA3_width = 2 × H                     0x3076/77
HMAX        = empirical (line time)     0x3034/35
```

`imx335_init_window_crop()` writes the base 120fps table **minus its final
standby-exit** (`0x3000=0x00`, `0x3002=0x00`), then the geometry override
**while still in standby**, then re-issues the standby exit. This latches the
crop window before streaming and guarantees a warm mode switch never inherits
stale window state.

HMAX is the one empirically-tuned value — line time grows with readout width,
so wider crops need a larger HMAX. Device-tuned values: 375 (2496 wide),
340 (2272), 275 (1792 and 1080). All three crops came up first-try at these
values; the widest (2496@50, HMAX=375) is the tightest but held 50.0 fps
solid with no encoder starvation.

## Full-res mode 0 and the pre-transition

`sensor_select` primes `MI_SNR_SetRes(pad, 1)` before `SetRes(pad, 0)`
whenever mode 0 is selected (`find_best_mode`/`select_and_enable` in
`src/sensor_select.c`), because the MI framework skips `pCus_sensor_init` when
`SetRes` lands on the boot-default index 0. The priming SetRes to index 1
(a crop mode) sets the crop init pointer, but `SetRes(0)` immediately
overwrites it with the full-res init pointer, so only the full-res init runs
on `MI_SNR_Enable`. The full-res 30fps table writes the **all-pixel geometry
reset explicitly** (`0x3018=0x00`, HTRIM/HNUM/Y_OUT at full width), so it
cleanly overrides any window regs the priming SetRes touched. Device-verified:
mode 0 comes up at 2592x1944@30, 11.2 Mbps, AE stable, through this path.

## Cold-boot enumeration wedge (operational)

Repeated **`reboot -f`** after a failed/hung teardown can leave the IMX335
i2c enumeration wedged: `MI_SNR_QueryResCount` returns 0 for **all** pads, so
`sensor_select` fails every mode with `--sensor-mode N is not available on
selected pad(s)` — regardless of which mode is requested (enumeration happens
before mode selection). This looks mode-specific if you only try one mode, but
it is not: in the wedged state every mode fails identically.

- **Trigger:** `reboot -f` skips clean sensor teardown; the sensor i2c/pad
  binding can carry a bad state across the warm reset.
- **Recovery:** a full **power-cycle** (not a warm reboot) — only a
  power-cycle resets the sensor. A graceful `reboot` (runs init.d shutdown,
  unloads modules) is much less likely to wedge than `reboot -f`.
- Once enumerated, `Pad 0: 6 mode(s)` prints and all modes select normally.

Live (warm, API-driven) mode switching across all five modes was verified
working once the explicit-geometry discipline was in place — the earlier
"live switch wedges to zombie" behavior does not reproduce with these tables.

## Deploying a driver change

The `.ko` is loaded at boot by `load_maruko -i` (S70vendor), which binds pads
and i2c — an `rmmod`/`insmod` does **not** re-run that binding, so a driver
swap needs a file-replace + reboot:

```
# from repo root, cross-built .ko at sensors/maruko/sensor_imx335_maruko.ko
cat sensors/maruko/sensor_imx335_maruko.ko | \
  ssh root@<maruko> 'cat > /lib/modules/5.10.61/sigmastar/sensor_imx335_mipi.ko'
# also update the persistent overlay copy so it survives reboot:
cat sensors/maruko/sensor_imx335_maruko.ko | \
  ssh root@<maruko> 'cat > /overlay/root/lib/modules/5.10.61/sigmastar/sensor_imx335_mipi.ko'
# then reboot (prefer graceful `reboot`; power-cycle if enumeration wedges)
```

Build: `make drivers-maruko KSRC_MARUKO=<maruko-kernel-src>`. The modpost
`undefined!` warnings for `DrvSensor*`/`intlog10` are expected — those symbols
resolve from the proprietary `mi` module at load time.
