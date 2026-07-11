# Maruko / IMX335 — full-res 2592×1944 @49 fps via FRAME_BASE

Result of the 2026-07-05 investigation into the IMX335 full-res high-fps gap on the
Infinity6C (I6C / Maruko). Companion to `MAJESTIC_MI_IOCTL_MAP.md` (the ioctl-level
majestic-vs-waybeam comparison) and memory
`maruko_imx335_majestic_isp_wall_crossvalidation`.

## The finding

On the **PR#83 IMX335 driver** (tipoman9, which sustains full-res 2592×1944@59 under
majestic — the "274 MPix/s ISP wall" was a driver-timing artifact, not hardware), waybeam
at 2592×1944@59 behaves differently by **bind link type**:

| waybeam bind (`maruko_pipeline.c:2265`) | Result at 2592×1944@59 | Notes |
|---|---|---|
| **REALTIME** (default full-res path) | **29.6 fps + ISP P0 FIFO storm** (~122× `[0]ISP P0 FIFO FULL`, `[Skip IQ]`) | on-chip line-buffered path can't feed the ISP fast enough at full res → backpressure collapse to half-rate |
| **FRAME_BASE** | **~49 fps clean** (0 FIFO-FULL, 0 rewind) | m2m path; ceilinged by the ISP's per-frame m2m cost (~20  ms/frame), not by FIFO backpressure |

So on I6C at full 5 MP, **FRAME_BASE is the only bind that gives waybeam a clean, sustained
high-fps full-res stream** — and it lands at **49 fps, a 63 % lift over the current shipping
full-res mode (2592×1944@30)**.

REALTIME @59 could not be recovered by any config lever (buffer depths, 3A/3DNR off, ISP
format, IFC compress, or a runtime ISP port kick — all live-tested negative; see the ioctl
map). The majestic-vs-waybeam config is provably byte-identical, so the REALTIME half-rate is
a dynamic-state / workload-contention effect we can't reach through the SDK. **FRAME_BASE @49
is the shippable win; REALTIME @59 is not reachable today.**

## Why FRAME_BASE works here (and what it costs)

- **Path.** REALTIME streams VIF→ISP through on-chip line buffers (lowest latency, but the ISP
  must keep up with the sensor line rate in real time). FRAME_BASE routes full RAW frames
  VIF→DRAM→ISP as a memory-to-memory (m2m) job — the ISP consumes each frame from DRAM at its
  own pace, decoupled from the sensor readout. That decoupling is exactly what removes the P0
  FIFO backpressure.
- **Latency cost: ~+1 frame** (~16.7 ms @60, up to ~20 ms) from the extra VIF→DRAM→ISP hop.
  This is the same trade the IMX415 `_1485` modes already document at
  `maruko_pipeline.c:2259-2263`. It is why waybeam's low-latency hero modes
  (1536×864@144, 1920×1080@100) deliberately stay REALTIME.
- **MMA / bandwidth cost.** FRAME_BASE stages full-width RAW frames in DRAM (~large at 2592
  wide), raising reserved MMA and DRAM bandwidth vs the on-chip REALTIME path — a real
  constraint on the bandwidth-limited I6C, to be validated against the MMA budget before
  committing a mode.
- **Ceiling.** FRAME_BASE beats the FIFO/bandwidth wall, **not** the per-frame-time wall, so it
  does not scale unbounded: ~49–50 fps is the full-res m2m ceiling. Above that needs a lighter
  ISP path or a smaller readout.

## How this can improve our mode lineup

The pipeline **already supports the FRAME_BASE path** — the IMX415 lineup keys it off a `_1485`
mode-name suffix (`maruko_pipeline.c:2265` matches `strstr(desc, "_1485")`). Adding an IMX335
full-res FRAME_BASE mode is therefore a **driver mode-table + suffix change, not new pipeline
code**:

1. **Add a full-res FRAME_BASE mode** to `drivers/sensor_imx335_maruko.c` — e.g.
   `2592x1944@49` with a suffix that keys FRAME_BASE (mirror the `_1485` convention; extend the
   `maruko_pipeline.c:2265` matcher to the new suffix). This gives the lineup a **clean full-res
   ~49 fps mode it currently lacks** (today full-res tops out at 30 under REALTIME).
2. **Position it as a quality/record mode, not a low-latency FPV mode.** Because of the +1-frame
   latency, keep it explicitly separate from the REALTIME low-latency hero modes and label the
   trade-off in the mode description / OSD, so mode selection is informed (max detail & frame
   rate for recording/quality vs minimum latency for flying).
3. **Validate MMA headroom** for the extra full-width RAW DRAM staging before shipping, and
   sweep the exact stable fps (49 measured; a small margin, e.g. @45, may be the safer shipped
   value if the m2m ceiling proves fps-sensitive across units/temperature).
4. **Re-examine IMX415** the same way — its "1485 ISP wall" (`maruko_imx415_1485_lock_isp_wall`)
   may likewise be driver-timing, and the FRAME_BASE @m2m-ceiling pattern may unlock higher
   full-res fps there too.

## Correcting the record

The earlier committed conclusion that full-res tops out at 30 under a hard REALTIME/bandwidth
wall is **wrong**: full-res runs clean at **49 fps under FRAME_BASE**. The
`documentation/MARUKO_IMX335_MODES.md` ISP-ceiling rationale and the
`drivers/sensor_imx335_maruko.c` mode-lineup comments should be updated when the mode is added.

## Reproduce

On .12 with the PR#83 IMX335 driver, at 2592×1944@59:

- Force the bind with the bench env override on the experiment branch:
  `WAYBEAM_VIF_LINK=realtime` → 29.6 fps + FIFO storm; `WAYBEAM_VIF_LINK=framebase` → ~49 fps
  clean. (This env knob is bench-only and is **not** part of this docs PR.)
- Measure: `/api/v1/fps/live`, the `[verbose] fps` line in `/tmp/waybeam.log`, ISP/SCL Ring fps
  in `/proc/mi_modules/mi_scl/mi_scl0`, and `dmesg | grep -i "fifo full"` (distinguish real
  `FIFO-FULL` from the benign `Fifo=0 Connect DevID=0` VIF AFIFO announcements).
