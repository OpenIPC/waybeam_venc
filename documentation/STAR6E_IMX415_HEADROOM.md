# Star6E IMX415 — Timing Model, Correctness Review & Headroom

Companion to `STAR6E_IMX415_MODES.md`. Captures the *device-established* timing
model for the in-tree driver (`drivers/sensor_imx415_star6e.c`), a correctness
review of the shipped lineup, and grounded speculation on what more is possible.
All numbers device-measured on SSC338Q (Infinity6E, 4.9.84) + IMX415, 4-lane
MIPI, bench box `.13` (2026-07-05).

> **Note on indices:** this document predates the strict fps re-sort in v0.32.0
> and refers to modes by their earlier append-order idx. Modes are unambiguous by
> resolution@fps; see `STAR6E_IMX415_MODES.md` for current indices. The timing
> model and physics here are index-independent.

## 1. The timing model (unified, both links)

Every mode obeys one frame-rate law:

```
fps  =  74.25e6 / (VMAX_eff × HMAX)
```

- **`HMAX`** is the per-line counter from the sensor's init table (`0x3028/29`).
- **`VMAX_eff`** is the *effective* vertical total = **`vts_30fps`** from the
  dispatch, **not** the table's `0x3024/25`. venc calls `SetFPS()` at init, which
  overwrites the table VMAX with `vts_30fps × max_fps / requested_fps`; at
  `requested_fps == max_fps` that is exactly `vts_30fps`. (idx1 makes this
  visible: table VMAX=1720 but `vts_30fps`=1900 rules → the mode runs 1900.)
- **`74.25e6`** is the INCK-derived line clock. Critically it is the **same for
  the 891 and 1485 Mbps links** — HMAX counts the fixed input clock, so the MIPI
  data rate does *not* change the frame timing. The link only sets the
  MIPI-per-line floor (§2.3).

Verified across the whole lineup (predicted vs measured, <1% error):

| Mode | link | VMAX_eff | HMAX | predicted | measured |
|---|---|---|---|---|---|
| 4K@30 (idx0) | 891 | 2250 | 1022 | 32.1 | 32.16 |
| 2816×1584@60 (idx1) | 1485 | 1900 | 652 | 59.9 | 59.52 |
| 1920×1080@90 (idx2) | 891 | 2250 | 365 | 89.9 | 89.92 |
| 2304×1296@100 (idx3) | 1485 | 1360 | 548 | 99.7 | 99.0 |
| 1920×1080@100 (idx4) | 891 | 2250 | 328 | 100.6 | 100.00 |
| 1472×816@120 (idx5) | 891 | 1700 | 365 | 119.7 | 118.64 |

## 2. The four walls

A mode is achievable only if it clears **all** of these. Any breach shows the
same symptom — the VIF delivers **exactly half** the sensor rate with
`DropCnt=0` (silent halve) — which is what made diagnosis hard (see §4).

### 2.1 Vertical-timing wall (VMAX too *small*)
`VMAX_eff ≥ physical_lines + vblank_min`, where
`physical_lines = H` (non-binned) or `2·H` (2×2 binned), `vblank_min ≈ 50–90`.
Because `VMAX_eff = 74.25e6/(fps·HMAX)`, raising fps shrinks the VMAX budget —
so a mode can breach this from *above*. **Fix: lower HMAX** to buy VMAX back.
This is why idx4 full-FOV binned needs HMAX=328 (not the stock 365): at 365,
100fps caps VMAX at 2023 < 2160 physical → halve.

### 2.2 Sensor analog HMAX floor (HMAX too *small*)
The line must be long enough to physically read its columns. Device-bounded for
a **full-width binned** line (1920 out / 3840 phys): floor is **between 229 and
275** (line 3092–3712 ns) — HMAX=275 reads clean at 120fps, HMAX=229 halves.
Wider readouts need a larger floor (more columns); narrower crops can go lower.
This is the ceiling on full-FOV binned fps (§3), independent of the ISP.

### 2.3 MIPI-per-line floor (rarely binding)
`HMAX ≥ 74.25e6 · W · bpp / (m · lanes · link)`, with `m=2` for 2×2 binned
(two physical line-times per output line), `m=1` non-binned. For full-width
binned 1920×10bit on 891: floor ≈ **200** — below the analog floor (§2.2), so
the analog floor dominates. Becomes relevant only for very wide non-binned lines
on the 891 link (→ use 1485).

### 2.4 ISP throughput wall
`W · H · fps ≤ ~300 MPix/s`. Device-mapped non-binned: 2304×1296@100 = 299
**clean**, 2432×1368@100 = 333 **drops**, 2560×1440@100 = 369 **halves**. The
binned path is **not** cheaper per pixel as once suspected — full-FOV binned
1920×1080@120 = 249 MPix/s runs clean, so the binned ISP ceiling is ≥249 and
almost certainly the same ~300.

## 3. Correctness review of the shipped lineup

All six modes recomputed against §1–§2 and re-measured; **all correct**:

- **idx4 (1920×1080@100 binned, HMAX=328)** — VMAX_eff=2250 ≥ 2160+90 ✓;
  HMAX=328 > analog floor ✓; 207 MPix/s < 300 ✓; MIPI floor 200 < 328 ✓.
  Robust to `video0.fps` requests: SetFPS clamps to max_fps=100, and lower
  requests only *raise* VMAX (safer). Verified 100.00fps, 0-drop/30s,
  warm-switch clean both directions (i2c-confirmed binning latch clears).
- **idx1 (2816×1584@60 widened)** — sensor table unchanged (still scans
  2952×1656); only `senout` widened so venc crops less. 267 MPix/s < 300 ✓,
  0 steady FIFO-FULL.
- **idx0/2/3/5** — stock/prior, unchanged, re-verified in the table above.

**Caveats / not-yet-checked:**
- **Image quality of idx4 (HMAX=328)** is inferred, not eyeballed. Counters
  (IsrCnt=enqueue=delivered=100) + clean dmesg (no line-count/CRC errors) mean
  frames are *structurally* complete, and the MIPI-per-line budget has margin,
  but a visual HDMI check for right-edge shading is still worth doing once.
- idx4 ships **12-bit** `data_prec` while the binned ADC is 10-bit (`0x3031=0`).
  This matches the stock 90/120 binned modes (same nominal-container quirk) and
  was proven irrelevant to the halve — left as-is for stock parity.

## 4. Process review (what to trust, what misled)

**Sound:** systematic on-device measurement; i2c register readback for ground
truth; and the calibration that unlocked everything — the stock 1472×816@120
mode shows `IsrCnt = enqueue = delivered = 119`, proving `IsrCnt` is **1
ISR/frame**, so "sensor 100 + enqueue 50" is a *real* halve, not double-counting.

**Red herrings chased (all device-disproven before the answer):** a "binned ISP
ceiling ~190 MPix/s" (it was the vertical wall), bit depth (10 and 12 both
halve), and link speed (891 and 1485 both halve). The trap: **two independent
walls (§2.1 vertical, §2.2 HMAX floor) produce the identical silent-halve
symptom**, so the symptom alone is not diagnostic — you must check `VMAX_eff` vs
physical lines *and* `HMAX` vs floor.

**Gotchas logged:** (a) `sed 's/"mode": *[0-9]*/.../' waybeam.json` also matches
the orientation `"mode": "mirror"` (zero-digit match) and corrupts the JSON →
anchor to the sensor-mode line. (b) A high-fps probe is silently capped by a
lower persisted `video0.fps` (the 144 probe actually ran 120, VMAX=2700) — set
`video0.fps ≥ target` before probing. (c) Live i2c VMAX writes are overwritten
by venc's AE within ~1s; live `video0.fps` persists to config.

## 5. Headroom — what more is possible

### 5.1 Proven & shipped
- **3840×2160@40 full 4K** (idx8, HMAX=825, VMAX=2250) — native 4K at 40fps,
  +33% over the stock idx0 4K@30. 40.7fps sensor=enqueue=delivered, 0-drop,
  0 steady FIFO-FULL (332 MPix/s). The clean full-4K ceiling (§5.3).
- **1728×972@100 binned wide crop** (idx9, HMAX=365, VMAX=2034) — the widest
  2×2-binned FOV that renders *clean* at 100fps (~80% of full-4K linear),
  **image-verified** on the display. Clones the idx5 1472×816 crop
  (`WINMODE=0x04` + explicit centered window) widened to 1728. See §5.6.
- **1728×816@120 binned wide crop** (idx10, HMAX=365, VMAX=1700) — same 1728
  width at 120fps; height capped at 816 (120fps forces VMAX≤1700 at the
  floor-clearing HMAX=365). 17% wider than idx5. Image-verified.
- **3840×1152@60 ultrawide** (idx7) — full sensor *width*, letterbox 3.33:1,
  59.98fps 0-drop. Built on idx1's 1485 base, window widened to 3840 + height
  cropped to 1152, HMAX=1022 (idx0's proven full-width line → no analog-floor
  risk), VMAX=1211. 265 MPix/s. Height is HMAX-bounded to ~1160; a taller
  3840×1296 (2.96:1) needs HMAX≈917 (below idx0, ISP at ~299) — a riskier
  stretch, not shipped.

### 5.2 Bounded / characterized
- **Full-FOV binned ceiling ≈ 120–130fps.** 144 (HMAX=229) breaches the analog
  HMAX floor. So 120 is the practical full-FOV binned max; ~128fps might squeak
  in (HMAX≈258) but is near the floor and risky.

### 5.3 Full-4K high-fps — MEASURED (2026-07-05, bench probes on .13)
Probed full 3840×2160 non-binned on the 1485 link (VMAX=2250, varying HMAX),
delta-sampling VIF `IsrCnt` vs `EnqCnt` over 5 s in **steady state** (cold-boot
FIFO-FULL is AE-settling noise — discard the first ~15 s):

| Mode | HMAX | MPix/s | sensor fps | enqueue fps | FIFO-FULL/5s | verdict |
|---|---|---|---|---|---|---|
| 4K@36 | 917 | 299 | 36.6 | 36.6 | 0 | **clean** |
| 4K@40 | 825 | 332 | 40.8 | 40.8 | 0 | **clean — ceiling** |
| 4K@42 | 779 | 352 | 43.0 | 39.8 | 16 | ISP-wall (~7% loss) |
| 4K@45 | 733 | 373 | 45.6 | 22.8 | 0 | analog-floor **halve** |

**Clean full-4K ceiling = 4K@40** (a native 4K mode, +33% fps over stock 4K@30).
Two *distinct* walls sit just above it, and the halve/FIFO-FULL signatures tell
them apart:
- **4K@42** — FIFO-FULL present, ~7% frame loss → the **ISP throughput wall**
  (~340 MPix/s for wide 4K lines). Note this is *not* pure MPix/s: 2432×1368@**100**
  = 333 dropped, but 4K@**40** = 332 is clean — lower fps gives the ISP more time
  per frame at equal pixel rate, and a 3840-wide line stresses the ISP line-FIFO
  harder than a 2432-wide one.
- **4K@45** — clean halve (sensor 45.6 → enqueue 22.8) with **zero** FIFO-FULL →
  the **sensor analog HMAX floor** (733 < floor ≤ 779 for a 3840-column non-binned
  line). This is the sensor's own horizontal readout time, independent of any SoC
  clock.

Other still-plausible probes (untested): 2560×1440@75 non-binned (277 MPix/s);
1472×816@150 binned crop (180, narrow line → low analog floor).

### 5.4 The ISP/CSI-clock lever — RESOLVED: not available on I6E
The Maruko/I6C playbook raises the wall by bumping the CSI-MAC clock to 288 MHz
(`SetCSI_Clk(CUS_CSI_CLK_288M)`, keyed on a `link_mbps` field). **This does not
port to I6E.** Tested on .13: the I6E vendor CSI driver *rejects* 288 MHz —
`dmesg: [Drv_CSISetClk] Not supported CSI CLK 288000` → the sensor never powers
on → "Sensor is abnormal" → waybeam crash-loops. `CUS_CSI_CLK_288M` is declared
in the I6E header (=5) but unimplemented in the blob; **216 MHz is the hard
CSI-MAC ceiling** here (no step between 216 and the unsupported 288). Every 1485
mode we ship already runs fine at 216M, so the driver pins 216M and keeps
`link_mbps` as metadata only.

Consequently **4K@45 is not reachable on I6E by any available means**: the CSI
clock is capped, the sensor analog floor (§5.3) is a hardware readout limit, and
the ISP throughput isn't tunable through the cus-sensor interface. The only
remaining theoretical lever is the **SCL/ISP core clock** (`DrvSclModuleClkInit`
logs a 345 MHz default) — but that lives in the kernel/DTS, not the sensor
driver, and is a much deeper change. Bit depth is *not* a lever (the wall is
pixel-rate, not bit-rate limited).

### 5.5 Not worth pursuing
- Bit-depth reduction for fps (ISP wall is pixel-limited).
- Pushing HMAX below the analog floor (silent halve / likely image damage).
- HDR/DOL modes (halve fps, add ISP load; deliberately removed).

### 5.6 The binned WINMODE=0x04 crop HMAX floor (a 5th failure mode)
The four walls in §2 all produce a *silent halve*. Device testing of binned
wide-crops surfaced a **fifth, distinct** symptom: **black frames with colored
horizontal lines** while the VIF still reports the full, correct fps (0 drops,
0 FIFO-FULL) and exposure is normal — a *malformed readout*, not a timing halve.

Cause: a `WINMODE=0x04` (windowed-crop) binned readout needs a **larger HMAX
than the full-frame `WINMODE=0x00` analog floor** (§2.2's 229–275). For a
1728-wide binned crop the crop floor sits between **308 and 365**: HMAX=365
renders clean (the wide-crop modes, device-verified image), HMAX=308 corrupts.
This also explains the **full-FOV 1920×1080 binned 100/120fps modes**: to hit
those rates they drop HMAX to ~328/275, below their (wider, 3840-phys-column)
crop floor, so they report correct fps but render black+colored-lines garbage —
they were **removed in v0.33.0**. The full-FOV binned **@90 is clean and kept**
(90fps needs only HMAX≈365-equivalent, which stays above the floor). For
higher-FOV binned at 100/120fps use the wide-crop modes instead.

Practical rule for new binned crops: keep **HMAX ≥ 365**, then the frame rate
caps the height (VMAX = 74.25e6/(fps·365) ≥ 2·H + vblank): ~972 lines at 100fps,
~816 at 120fps. Width is free up to the ~300 MPix/s ISP wall.
