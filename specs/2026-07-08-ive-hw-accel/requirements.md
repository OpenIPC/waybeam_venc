# IVE hardware block — FPV compute accelerator on Maruko (i6c)

Status: **Proven + benchmarked on device. No consumer built yet.**
Date: 2026-07-08
Device: `root@192.168.2.233` (i6c / SSC378QE)

## The discovery

While investigating stab (see `../2026-07-08-maruko-stab/`), we established that
the SoC has a **real, working IVE hardware block** that is completely idle on our
vehicles today. It is distinct from `MI_IVE_Shift_Detector`, which is a NEON
*software* path.

Naming trap that misled earlier analysis: `libmi_ive.so` links **two** modules —
`#ive#` (the thin HW shim) and `#MVE#` (SigmaStar's Simd/NEON software vision lib).
**"MVE" is a module name, not a Motion Vector Engine.** There is no motion-search
hardware (`grep motion|mv_|block_match drivers/sstar/` → 0 hits). But the `ive`
side really does drive silicon: `libmi_ive.so` has 32 call sites issuing the one
IVE ioctl, and ops like `MI_IVE_Sobel/Erode/Thresh/Sad` raise the `ive isr`.

## Kernel ABI (fully open-source, BSP `drivers/sstar/include/`)

One ioctl:
```c
#define IVE_IOC_MAGIC   'I'
#define IVE_IOC_PROCESS _IOW(IVE_IOC_MAGIC, 1, ive_ioc_config*)   // = 0x40044901
```
Structs (`mdrv_ive_io_st.h`):
```c
typedef struct { int format; u16 width; u16 height;
                 u64 address[3]; u16 stride[3]; } ive_ioc_image;   // sizeof 40
typedef struct { int op_type;           // @0
                 ive_ioc_image input;   // @8
                 ive_ioc_image output;  // @48
                 union { ...per-op coeffs... }; } ive_ioc_config;  // union @88
```
Rules learned:
- Driver does `copy_from_user(&cfg, arg, sizeof(kernel ive_ioc_config))`
  (`mdrv_ive.c:437`) → **over-allocate** the userspace coeff area (a `char[256]`
  tail works).
- Buffers must be **MMA-physical**: `MI_SYS_MMA_Alloc(0,NULL,size,&phy)` +
  `MI_SYS_Mmap(phy,size,&vir,0)`. Driver takes raw MIU physicals unchecked
  (`USE_MIU_DIRECT`, `mdrv_ive.h:99`; `hal_ive.c:239-241`).
- Min image **16×5** (`drv_ive.c:42-43`).
- input/output size must match, **except** `SAD`/`NCC`/`MAP`/`HISTOGRAM`/`BAT`
  (`drv_ive_check_config`).
- **One outstanding request per fd**: driver rejects a second concurrent request
  ("One file can request once at the same time only", `mdrv_ive.c:431-435`).
  Serialize or open multiple fds.
- Completion is IRQ + `poll()`-driven (IRQ 78 "ive isr").

## Benchmark (measured, `tools/ive_i6c_hwop.c`, THRESH op, `.233`)

Verified correct: THRESH on a known gradient → 0/147456 mismatches; `ive isr`
increments once per op.

| size | ms/op wall | CPU ms/op | core % | Mpix/s |
|---|---|---|---|---|
| 64×64 | 0.052 | 0.010 | 19% | 78 |
| 128×128 | 0.045 | 0.009 | 19% | 366 |
| 256×256 | 0.199 | 0.011 | 5% | 330 |
| 384×384 | 0.411 | 0.014 | 3% | 359 |
| 640×480 | 0.784 | 0.014 | 2% | 392 |
| 1280×720 | 2.033 | 0.024 | 1% | 453 |
| 1920×1080 | 4.164 | 0.025 | 1% | 498 |

**Two numbers that define what's feasible:**
- **Fixed per-op overhead ≈ 45 µs** (ioctl + IRQ + poll wake).
- **Asymptotic throughput ≈ 500 Mpix/s.**

A full 1080p per-pixel op = **4.2 ms wall but 0.025 ms CPU (1% of one core)** — a
genuine offload. Roughly **3 full-1080p ops fit a 60 fps frame** at ~free CPU.

Caveat: only `THRESH` was benchmarked. `SOBEL`/`SAD`/`NCC`/`BAT`/`HISTOGRAM` have
different coeff structs and output formats; per-op cost is likely bandwidth-bound
and similar, but that is an assumption pending measurement.

## HW op inventory (`mdrv_ive_io_st.h:44-75`)

FILTER, CSC, FILTER_AND_CSC, SOBEL, MAG_AND_ANG, ORD_STA_FILTER, BERNSEN, DILATE,
ERODE, THRESH{,_S16,_U16}, AND, OR, XOR, ADD, SUB, 16BIT_TO_8BIT, MAP, HISTOGRAM,
INTEGRAL, SAD, NCC, LBP, BAT, ADP_THRESH, MATRIX_TRANSFORM, IMAGE_DOT,
ALPHA_BLENDING. (~27 total.)

**`NCC` (0x16) and `BAT` (0x19) are HW ops that `libmi_ive.so` services in
SOFTWARE** — driving the ioctl directly is the only way to get them on hardware.

## FPV opportunities (ranked)

1. **ROI / QP map for the encoder** — `BAT` + `SAD` → per-block activity map →
   VENC ROI QP. Spend bits on motion/texture, starve sky/static ground. Feeds
   straight into the wfb-ng link budget (quality per bit). **Highest value.**
2. **Scene-change → IDR governor + link pre-warn** — `HISTOGRAM`/`SAD` → force IDR
   only on real cuts; pre-warn `link_controller` of I-frame spikes (attacks the
   known `wfb_ng_peek_idr_profile_airtime_spike` fec_skip/MCS-flap failure).
3. **Preflight lens check** — `SOBEL` + `INTEGRAL`/`BAT` → per-tile sharpness →
   detect obstruction/defocus/droplet before takeoff, OSD warning. Same primitives
   give focus-peaking (`SOBEL`+`THRESH`→RGN).
4. **AE assist / blackout detection** — `HISTOGRAM` in HW; move some 3A stats off
   CPU (already cut 60% via AE throttle); detect all-black/all-white failure frames.
5. **Coarse optical flow / velocity** — `SAD` with pre-shifted `src2` physical
   address (driver passes physicals through unchecked). At **128×128 = 45 µs/op**,
   a 7×7 search = 49 ops ≈ **2.2 ms wall, ~0.4 ms CPU**; at 10–20 Hz nearly free.
   This is the "Optical flow + IMU fusion" roadmap item, HW-backed. (NB: full-res
   pyramid search is NOT worth it — see cost note below.)
6. **HW `NCC` template tracking** — the free lunch (blob does it in SW). POI/gimbal
   lock, follow-me, landing-pad acquisition.
7. **Precision-landing marker detection** — `ADP_THRESH`/`BERNSEN` (lighting-robust
   binarize) + `ERODE`/`DILATE`. (Connected-components stays on CPU.)
8. **Motion-triggered recording** — `SUB`→`THRESH`→`ERODE`/`DILATE` frame-diff,
   entirely in HW; wake-on-motion DVR.
9. **`MAP` LUT** (256-entry, 45 µs low-res) — gamma, false-colour, thermal palette,
   high-contrast FPV modes.

Lower confidence / needs a bench: **`MATRIX_TRANSFORM` (3×3)** for IMU-leveled
horizon (unknown if geometric warp vs colour matrix — the name is ambiguous);
**`ALPHA_BLENDING`** for OSD (probably no win — `MI_RGN` already HW-composites).

Why full-res motion search is NOT worth HW: `SAD` has no offset param, so each
candidate is one ioctl. Hundreds of candidates at full res (pyramid-3/search-96) ≈
50–100 ms ≫ the 16.8 ms NEON path. HW wins only at *low res / few candidates*
(item 5).

## The one piece of enabling code

A ~150-line `src/ive_hw.c` helper: `open("/dev/mstar_ive0")`, MMA alloc/free,
`ive_hw_submit(op, in, out, coeff)` (ioctl + poll), teardown. That single file
unlocks every item above. It needs a raw image source — conveniently the **same
free SCL port 2** the stab detector tap wants (port0 is IFC-compressed).

## Scope for THIS spec

- Land `src/ive_hw.c` + a self-test that reproduces the benchmark table in-tree.
- Prototype **item 1 (ROI/QP map)** end-to-end as the first real consumer.
- Everything else stays a documented backlog until item 1 proves the pattern.

## Acceptance criteria (item 1 prototype)

1. `ive_hw.c` self-test reproduces the THRESH benchmark within ±20% on device.
2. A raw 384×384 (or larger) tap feeds `BAT`/`SAD` at sensor fps at <5% CPU.
3. An activity map measurably shifts VENC QP toward high-motion regions
   (verified by inspecting per-region QP or output bitrate distribution).
4. No `ive isr` starvation / no interference with the main encode path.
