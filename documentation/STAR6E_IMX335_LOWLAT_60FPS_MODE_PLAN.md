# Star6E IMX335 low-latency 60 fps sensor mode — Step 0 verdict

Status: **CLOSED — MEASURED NEGATIVE for the fast-scan mode; verified
alternative recommended.** Step 0 (measure before designing) was executed on
.232 (SSC338Q, IMX335, in-tree `sensor_imx335_star6e.ko`, venc v0.62.0,
2026-08-01) and killed the original proposal before any register table was
written. This document records the measurements, the delivery-timing model
they establish, and the config-only path that actually lowers latency with
retained FOV.

## Original proposal (for the record)

Add a driver mode that scans fast (HMAX=275) and stretches VMAX so 60 fps is
met with long blanking — deliver the finished frame to the ISP earlier
within each 16.7 ms period. Predicted readout: 8.2 ms (5M60) → ~5.3 ms
(2560x1440 fast-scan).

## Step 0 measurements

Per-mode scan timing computed from the driver tables
(line_time = HMAX / 74.448 MHz):

| mode | geometry | VMAX | HMAX | line time | readout (active rows) |
|---|---|---|---|---|---|
| 0 | 2560x1920@30 | 4125 | 600 | 8.06 µs | ~15.7 ms |
| 1 | 2560x1920@60 | 3936 | 314 | 4.218 µs | ~8.2 ms |
| 2 | 2560x1440@90 | 3016 | 275 | 3.694 µs | ~5.0 ms (~5.3 ms paced to 60) |

VENC arrival = `OnPreProcessInputTask` avg from
`/proc/mi_modules/mi_venc/mi_venc0` (delta from the frame's PTS reference to
its arrival at the encoder input), measured in three states:

| state | readout | capture period | measured arrival |
|---|---|---|---|
| mode 1 @60/60 | 8.2 ms | 16.67 ms | **16.72 ms** |
| mode 2 @90/90 | 5.0 ms | 11.11 ms | **11.20 ms** |
| mode 2 paced to 60 (**= the proposed fast-scan mode, exactly**) | 5.3 ms | 16.67 ms | **16.74 ms** |

The third row is the punchline: the driver's `set_fps` already implements
VMAX-stretch pacing (fixed HMAX=275, blanking extended), so "mode 2 at
fps 60" IS the proposed fast-scan-60 configuration — and it delivers
**zero** improvement over mode 1.

Exposure probe: sweeping `isp.shutterMaxUs` 16 666 → 4 000 µs (AE followed,
confirmed via `/api/v1/ae`) moved the arrival numbers not at all;
independently confirmed via `shutterRule180` on/off.

## Delivery-timing model (established)

- **PTS is stamped at capture/frame start**, upstream of exposure — no
  shutter setting can ever appear in `capture → frame_ready` metrics.
  (Shorter exposure still reduces real photon staleness — the image content
  is up to ~½ exposure old at PTS — it is just invisible to the counters.
  At the dim bench scene AE ran the full 16.7 ms frame period.)
- **Frame delivery to the encoder is VSYNC-quantized: arrival ≈ exactly one
  capture period after PTS, regardless of readout time.** The completed
  frame is handed downstream at the next frame boundary, not at readout-end.
  Measured residual over the period: +50–90 µs.
- Encode itself is ~3.7 ms (arrival → `DequeueInputTask`), packetise/ship
  sub-ms. Consequence: **capture→ready has a hard floor of 1/capture_fps.
  The only lever is capture rate.**
- 90-capture → 60-encode hybrid (FRC drop at the VENC input port): mean
  arrival ~14 ms with an alternating 0/5.6 ms pacing beat — a ~2.7 ms mean
  win with added jitter, below this plan's 4 ms bar. Not worth a venc
  feature (sensor-fps decoupling) on its own.

## What actually works — recommendation

**`sensor.mode=2` (2560x1440@90) with `video0.fps=90`.**

- **FOV is retained exactly.** The craft encodes 1280x720 with
  `image.keepAspect=true`, whose precrop of mode 1 is the centered
  full-width 2560x1440 window — the same sensor region mode 2 reads
  natively (`active_precrop {0,240,2560,1440}` observed live). Nothing the
  viewer sees changes except cadence.
- capture→ready drops 16.7 → **11.2 ms flat** (−5.5 ms, no jitter), and the
  60→90 Hz cadence cuts mean display wait by a further ~2.8 ms — roughly
  **−8 ms glass-to-glass**.
- ISP pixel load per frame *drops* vs mode 1 (3.69 vs 4.9 MPix); total
  332 MPix/s at 90 fps was device-verified sustained in bring-up and
  re-verified in this session's soak.
- 3-minute soak at 90/90 on .232: encode 89.95–89.99 fps sustained, zero
  new VENC drops, arrival 11.15–11.26 ms, CBR 100–104 % of the
  10 303 kbps target at `fpv.noiseLevel=0`.

Adopting it is an **operating-mode decision, not a venc change**: on the
craft waybeam-link owns fps/resolution via its mode catalog, so this lands
as a new/edited mode file (and the ground catalog + `catalog_fingerprint`
must follow — waybeam-link §11.7 index-drift trap applies).

Secondary guidance: keep an exposure cap (`isp.shutterMaxUs` or
`shutterRule180`) for photon staleness; do not expect it to move any
pipeline metric.

## Non-goals / closed questions

- Fast-scan + blanking-stretch driver modes: **closed, measured zero-gain**
  (this document). Do not re-propose without evidence the VSYNC
  quantization changed (new SDK/mhal).
- VPE→VENC bind: FRAMEBASE is the only mode on i6e
  (`REALTIME_PIPELINE_INVESTIGATION.md`) — settled separately.
- Delivering at sensor EOF instead of next SOF would recover ~11 ms at
  60 fps, but the hand-off point is VIF/mhal behaviour, not sensor-driver
  behaviour — out of reach without vendor changes.
