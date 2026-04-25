# Optical Flow + IMU Fusion Roadmap

> **Status (0.7.14): on hold.** This roadmap assumed the EIS framework
> (`src/eis.c`, `src/eis_gyroglide.c`, `EisMotionRing`) as an
> integration target.  The EIS module was removed in 0.7.14 (see
> HISTORY).  The core ideas — visual-inertial motion estimation,
> ROI-driven encoder feedback, IMU dead-reckoning — remain valid
> targets, but the integration sections below referencing
> `eis.flow.*` config keys, `eis.mode = "fused"`, and the
> `EisMotionRing` structure are now stale.  When this work is
> resumed, the consumer side will need to be redesigned around
> whatever stabilization framework replaces EIS (likely Phase C
> LDC-warp; see EIS_INTEGRATION_PLAN.md).  The IMU module itself
> is retained, compiled but disabled by default.

## Goal

Add visual-inertial motion estimation to waybeam_venc that produces
flight-controller-grade velocity estimates for GPS-denied position hold,
follow-me, and visual EIS. The system must work with **optical flow only**,
**IMU only**, or **both fused** — degrading gracefully when either source
is unavailable.

## Background

### What each sensor provides

| Capability | BMI270 IMU (6-DOF) | Optical Flow (LK) |
|---|---|---|
| Rotation measurement | Excellent (direct gyro) | Poor (crude rz proxy) |
| Translation measurement | Unusable (double-integration drift) | Good (direct pixel displacement) |
| Scale / altitude proxy | None | Not provided (use baro) |
| Latency | ~2.5 ms (400 Hz I2C) | ~100-200 ms (5-30 Hz, frame-dependent) |
| Max update rate | 1600 Hz | ~30 Hz practical |
| Drift | Gyro bias drifts over seconds | Zero (each frame pair is independent) |
| Works in darkness | Yes | No |
| Works on featureless surface | Yes | No |
| Vibration immunity | Needs filtering | Naturally filtered by exposure |

### Why fusion matters

**IMU alone** holds attitude (keeps drone level) but **cannot hold position** —
accelerometer drift means no awareness of lateral movement after seconds.

**Optical flow alone** detects ground-relative translation but confuses
rotation with translation — a yaw spin looks like sideways drift.

**Fused**: The IMU provides high-rate rotation that derotates the flow vectors.
The flow provides absolute velocity that kills IMU drift. Together they produce
~10-30 Hz position-hold-grade velocity estimates with IMU-bridged gaps between
flow updates. This is how PX4 and ArduPilot implement optical flow position hold.

### Derotation — the critical transform

Before optical flow translation is usable, camera rotation must be subtracted:

```
translational_flow = raw_pixel_flow − (gyro_integral × focal_length)
```

Without derotation, any camera rotation is misinterpreted as ground movement.
The flight controller would "correct" phantom drift during every turn —
potentially catastrophic in flight.

### Flight controller integration

The output targets standard protocols that flight controllers already consume:

**MAVLink OPTICAL_FLOW_RAD (msg #106):**
```
integrated_x:       rad   — rotation from IMU-compensated flow (body X)
integrated_y:       rad   — rotation from IMU-compensated flow (body Y)
integrated_xgyro:   rad   — gyro X during integration period
integrated_ygyro:   rad   — gyro Y during integration period
integrated_zgyro:   rad   — gyro Z during integration period
distance:           m     — ground distance (-1 if no rangefinder)
quality:            0-255 — tracking confidence (feature count × match score)
time_delta_distance_us:   — integration period
```

**MSP_OPTICAL_FLOW (iNav):**
```
motion_x:  int16   — flow in body-X (scaled)
motion_y:  int16   — flow in body-Y (scaled)
quality:   uint8   — tracking quality
```

The transport path:
```
venc (fusion output) → UDP:5602 sidecar → waybeam-hub (mod_telemetry) → UART → FC
```

The existing RTP sidecar channel (port 5602) can carry the fused packets
alongside frame metadata. Alternatively, a dedicated UDP port for flow data
allows direct routing to the FC via waybeam-hub.

## LK-Only Recommendation

PR #23 implements two optical flow methods: LK (Lucas-Kanade) and SAD (Sum of
Absolute Differences). **Only LK should be integrated.** SAD should be deferred.

### Why LK is sufficient

| Factor | LK | SAD |
|---|---|---|
| Hardware acceleration | Full — `MI_IVE_LkOpticalFlow` runs on ISP silicon | Partial — `MI_IVE_Sad` for detection, CPU for patch matching |
| CPU cost | Low (hardware-offloaded) | Higher (CPU planar reduction competes with encoder) |
| Rotation output | Yes (`rz` proxy — useful for derotation validation) | No |
| Translation output | Yes (`tx`, `ty`) | Yes (`tx`, `ty`, `tz`) |
| Scale output | No | Yes (`tz`) — but FC doesn't use it (baro handles altitude) |

**LK wins on all axes that matter for position hold:**

1. **FC only needs tx/ty/quality** — the MAVLink `OPTICAL_FLOW_RAD` message
   does not carry a scale field. Altitude comes from barometer or rangefinder.
   SAD's `tz` has no standard consumer.

2. **LK's `rz` rotation proxy aids development** — during IMU-flow fusion
   bring-up, comparing LK's `rz` against gyro integral validates time sync
   correctness. SAD provides no rotation reference.

3. **Hardware offload = predictable CPU budget** — LK runs entirely on IVE
   silicon. SAD's CPU patch matcher (even with NEON inner loop) steals
   Cortex-A7 cycles from the encoder. At 10-30 Hz flow rate, this matters.

4. **Without IMU, LK still works** — the FC has its own onboard gyro and
   can partially compensate for rotation. LK's raw `tx`/`ty` plus the FC's
   gyro gives a usable (if degraded) position hold. SAD adds nothing in this
   scenario that LK doesn't already provide.

### When SAD might be reconsidered

- Pure visual odometry without any FC gyro (not our use case)
- Extremely low-texture scenes where LK's corner detector finds no features
  but SAD's dense grid finds weak texture (marginal edge case)
- Scale estimation becomes critical (e.g., no barometer on the FC)

**Decision**: Implement LK only. Remove `opt_flow_sad.c` and
`star6e_osd_simple.c` from the integration. This eliminates ~1,700 lines
and the CPU-heavy patch matcher. The `eis.flow.method` config field can
remain as a placeholder for future backends.

## Known Vendor Constraints

- **LK point count max 14**: `MI_IVE_LkOpticalFlow` crashes with 16+ points
  (PR #23 discovered empirically). Code must hard-clamp the point count and
  add a runtime `assert` or guard. This limits tracking density but 10-14
  well-selected feature points is sufficient for planar motion estimation.
- **LK input size**: 64×64 to 720×576 (vendor SDK limit). Source frames must
  be downsampled. Current implementation uses ~160×180 center-cropped target.
- **LK alignment**: Input/output physical addresses must be 16-byte aligned,
  stride must be 16-pixel aligned. The existing code handles this via
  dedicated IVE image buffers.

## Camera Calibration

Derotation requires knowing the camera's focal length in pixels:

```
f_px = image_width / (2 × tan(FOV / 2))
```

| Sensor | Typical FOV | f_px at 1920w | f_px at 160w (tracking) |
|---|---|---|---|
| IMX335 (120°) | 120° | 554 | 46 |
| IMX415 (90°) | 90° | 960 | 80 |
| IMX327 (150°) | 150° | 358 | 30 |

This must be a config parameter (`eis.flow.focalLengthPx`) or derived from
a new `sensor.fovDegrees` field. Without correct focal length, derotation
over- or under-compensates rotation, producing residual drift proportional
to yaw rate.

For flow-only mode (no derotation), focal length is not needed — the raw
pixel flow goes directly to the FC, which handles compensation using its
own gyro.

## Architecture Requirements

### Must integrate with existing EIS framework

The optical flow module **must not** be a standalone parallel subsystem.
It must integrate as a motion source within the existing EIS architecture:

- `src/eis.c` — EIS lifecycle and frame-sync dispatch
- `src/eis_gyroglide.c` — current gyro-only crop-based EIS
- `include/eis_ring.h` — `EisMotionRing` for gyro samples
- `include/imu_ring.h` — `ImuRing` for raw IMU samples
- `src/imu_bmi270.c` — BMI270 driver with FIFO drain, calibration, rotation matrix

The EIS framework already has:
- Frame-synchronized gyro batch drain via FIFO
- Ring buffers with timestamp-based range queries
- Crop-window stabilization via `MI_VPE_SetPortCrop`
- Configurable mode selection (`eis.mode`)

Optical flow should plug in as an additional motion source, not replace or
duplicate this infrastructure.

### Three operating modes

```
eis.mode = "gyroglide"       — IMU-only crop EIS (current, no flow)
eis.mode = "optflow"          — flow-only (no IMU required)
eis.mode = "fused"            — IMU derotation + flow translation
```

When `fused` mode is selected but IMU is disabled, fall back to `optflow`.
When `fused` mode is selected but flow has no features (low light, featureless
ground), fall back to IMU-only and set quality=0.

### Config integration

Optical flow settings belong in the existing `eis` config section, not a
separate `optflow` section:

```json
{
  "eis": {
    "enabled": true,
    "mode": "fused",
    "marginPercent": 30,
    "flow": {
      "fps": 10,
      "method": "lk"
    }
  }
}
```

This follows the 4-layer config sync rules (C struct + parser, API field table,
WebUI dashboard, default config). Any new fields must be added to all four
layers in the same PR.

## Implementation Phases

### Phase 1: Refactor LK optical flow as EIS motion source

**Goal**: Integrate LK-only flow from PR #23 into the EIS framework. Drop SAD.

- [ ] Keep `opt_flow_lk.c`, remove `opt_flow_sad.c` and `star6e_osd_simple.c`
- [ ] Collapse `opt_flow.c` dispatcher — with only LK, the strategy pattern
      wrapper is unnecessary; merge into `opt_flow_lk.c` or rename to `eis_optflow.c`
- [ ] Extract MI_SYS type redefinitions from LK source into a shared header
      (or use the SDK headers added by PR #23)
- [ ] Rename `OptFlowState` to `EisOptflowState` to avoid name collision
- [ ] Remove `--unresolved-symbols=ignore-in-shared-libs` linker flag — use dlsym
      for MI_RGN symbols (same pattern as existing MI_IVE dlopen)
- [ ] Make `__expf_finite` / `__log_finite` shims `static` or `__attribute__((weak))`
- [ ] Move config from `optflow.*` to `eis.flow.*` section
- [ ] Feed LK output into `EisMotionRing` or a new `FlowMotionRing` with timestamps
- [ ] Fix config default mismatch (`enabled` defaults must match code and JSON)
- [ ] No OSD rendering in venc — flow data exposed via HTTP API and UDP only.
      Debug visualization handled externally by waybeam-hub, msposd, or ground tools

### Phase 2: IMU-flow time synchronization

**Goal**: Align gyro samples with flow integration periods.

- [ ] Tag each flow result with the VPE frame PTS (from `MI_SYS_GetCurPts` or
      the buffer's `u64Pts` field)
- [ ] Query IMU ring for gyro samples within the flow integration window
      (`prev_frame_pts` to `curr_frame_pts`)
- [ ] Integrate matched gyro samples to get rotation during flow period
- [ ] Handle clock domain differences (IMU uses `CLOCK_MONOTONIC`, VPE uses SoC PTS)
      — calibrate offset at startup via `MI_SYS_GetCurPts` vs `clock_gettime`

### Phase 3: Derotation and fusion

**Goal**: Produce rotation-compensated translational velocity.

- [ ] Compute `gyro_integral = Σ(gyro_sample × dt)` over the flow integration period
- [ ] Estimate focal length from sensor config: `f_px = width / (2 × tan(FOV/2))`
      — FOV may need to be a config parameter or derived from ISP bin metadata
- [ ] Derotate: `flow_trans = flow_raw − gyro_integral × f_px`
- [ ] Compute quality metric: `quality = clamp(n_features × match_confidence / threshold, 0, 255)`
- [ ] Output struct:
      ```c
      typedef struct {
          struct timespec ts;
          float integrated_x;      /* rad, derotated */
          float integrated_y;      /* rad, derotated */
          float integrated_xgyro;  /* rad, raw gyro */
          float integrated_ygyro;  /* rad, raw gyro */
          float integrated_zgyro;  /* rad, raw gyro */
          float distance;          /* m, -1 if no rangefinder */
          uint8_t quality;         /* 0-255 */
          uint32_t dt_us;          /* integration period */
      } FusedFlowSample;
      ```

### Phase 4: Data output (HTTP API + UDP)

**Goal**: Expose fused data for external consumers. No OSD rendering in venc —
all visualization and FC forwarding is handled by external tools.

- [ ] Add HTTP endpoint `GET /api/v1/flow` returning current fused state as JSON:
      `{ "integrated_x", "integrated_y", "gyro_x/y/z", "quality", "dt_us", "mode" }`
- [ ] Send `FusedFlowSample` as compact binary UDP to a configurable port
      (either sidecar extension or dedicated `eis.flow.port`)
- [ ] Document UDP packet format in `HTTP_API_CONTRACT.md` for external consumers
- [ ] waybeam-hub `mod_telemetry` encodes received flow data as MAVLink
      `OPTICAL_FLOW_RAD` (msg #106) or MSP_OPTICAL_FLOW and forwards to FC UART
- [ ] Implement flow-only fallback: when no IMU, send raw flow as integrated_x/y
      with gyro fields zeroed and quality reduced (FC EKF handles the degraded case)
- [ ] Implement IMU-only mode: send gyro-only data with quality=0 and zero flow
      (FC ignores low-quality flow but still gets gyro for attitude)

### Phase 5: Validation and tuning

- [ ] Bench test: camera on motorized gimbal with known motion profile
- [ ] Verify derotation accuracy: pure rotation should produce near-zero flow output
- [ ] Verify translation accuracy: pure lateral slide should produce consistent flow
- [ ] Test degraded modes: cover lens (flow-only fails gracefully), disconnect IMU
      (fused falls back to flow-only)
- [ ] Tune quality thresholds for different lighting conditions
- [ ] Measure end-to-end latency: frame capture → fused output → FC receipt
- [ ] PX4/ArduPilot bench test with `EKF2_AID_MASK` optical flow enabled

## Performance Budget

Target: no more than 5% CPU overhead on the encoder thread at 120 fps.

| Component | Budget | Rate | Notes |
|---|---|---|---|
| VPE buffer borrow | 1 ms | 10 Hz | `MI_SYS_ChnOutputPortGetBuf` + `PutBuf` |
| Downsample to tracking size | 0.5 ms | 10 Hz | ~160×180 target |
| LK matching | 2-5 ms | 10 Hz | Hardware-accelerated via `MI_IVE_LkOpticalFlow` |
| Derotation + fusion | <0.1 ms | 10 Hz | Scalar arithmetic |
| MAVLink encoding + UDP send | <0.1 ms | 10 Hz | Small packet |
| IMU ring query | <0.01 ms | 10 Hz | Timestamp range scan |

At 10 Hz processing and 120 fps encoding, flow runs once every 12 frames.
Per-flow-frame CPU cost ~3-6 ms. Amortized: ~0.3-0.5 ms per encoded frame
(~3-6% of the 8.3 ms frame budget).

## NEON Optimization Notes

Benchmarking on the target Cortex-A7 (see PR #25) showed:

- **NEON loses** on small vectors (3×3 rotation: 0.8×), gather loads from
  non-contiguous structs (EIS integration: 0.5×), and small batch sizes
- **NEON wins** on wide contiguous data (CRC32 table: 6.6×)
- **Auto-vectorization** via `-ftree-vectorize` provides free gains on simple loops

With SAD removed, the remaining NEON candidates in the flow path are limited.
The LK matching is hardware-accelerated (no CPU NEON needed). The derotation
arithmetic (small vectors) should stay scalar. The downsample (per-pixel row
copy) may benefit from NEON `vld1`/`vst1` bulk transfers but is only ~0.5 ms
at 10 Hz — not worth optimizing.

## Key Files

| File | Role |
|---|---|
| `src/eis.c` | EIS lifecycle, mode dispatch, frame-sync hook |
| `src/eis_gyroglide.c` | Gyro-only crop EIS (reference for integration pattern) |
| `include/eis_ring.h` | Motion sample ring buffer with timestamp queries |
| `src/imu_bmi270.c` | BMI270 driver, FIFO drain, calibration |
| `include/imu_ring.h` | Raw IMU sample ring |
| `src/star6e_runtime.c` | Encoder main loop (optflow hook point) |
| `src/star6e_pipeline.c` | Pipeline init/teardown (optflow create/destroy) |
| `src/rtp_sidecar.c` | Frame metadata UDP channel (transport candidate) |
| `documentation/EIS_INTEGRATION_PLAN.md` | VPE crop/LDC API reference |
| `documentation/GYROGLIDE_LITE_DESIGN.md` | Gyro-only EIS design rationale |

## References

- PX4 optical flow EKF: `EKF2_AID_MASK` bit 1 (optical flow)
- ArduPilot optical flow: `FLOW_TYPE` parameter
- MAVLink OPTICAL_FLOW_RAD: message ID 106
- SigmaStar IVE API: `MI_IVE_LkOpticalFlow`
- DJI vision positioning: dual-camera downward + ultrasonic rangefinder
