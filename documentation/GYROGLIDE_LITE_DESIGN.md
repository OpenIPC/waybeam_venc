# GyroGlide-Lite Design

**A low-latency frame-synchronous 2-axis translational EIS using batched gyro
integration and crop-window control.**

---

## Plain-language summary

A camera on a drone vibrates and tilts. A gyroscope chip (BMI270) measures
rotation speed 200 times per second. Every time the video encoder finishes a
frame (~30-60 fps), we collect all the gyro samples that accumulated since
the last frame, multiply each sample's rotation rate by its time slice, and
sum them to get "how much did the camera rotate during this frame interval."
We convert that angle into pixels, then shift a crop window inside an
oversized sensor capture area to cancel the motion. The visible image slides
opposite to the shake. A spring pulls the crop window slowly back toward
center so we don't run out of travel. The whole thing costs one
`MI_VPE_SetPortCrop()` call per frame and a few microseconds of arithmetic.

---

## 1. Why gyro-only is the right fast-path

**Accelerometers measure linear acceleration, not rotation.** To get position
from an accelerometer you must double-integrate, which drifts unboundedly
within seconds — unusable for real-time stabilization without GPS or vision
correction. For crop-window EIS on an FPV camera:

- **Gyro directly measures what we correct.** Camera shake is rotational
  (yaw/pitch). Gyro outputs angular velocity in rad/s. One integration gives
  angle. Angle maps to pixel displacement through the lens focal length. One
  integration is stable over frame-sized intervals (16-33 ms).

- **Accelerometer would add complexity for marginal gain.** It could provide
  a gravity reference for horizon leveling, but that requires an AHRS filter
  (complementary or Kalman), adds latency, and the crop-only system can't
  correct rotation anyway. Not worth it for the fast path.

- **Gyro bandwidth matches the problem.** At 200 Hz, we get 3-7 samples per
  60fps frame — enough to capture vibration content up to ~100 Hz (Nyquist).
  Higher frequencies are beyond the crop system's correction bandwidth anyway
  (pixel-level displacement at high freq is sub-pixel).

- **Latency is minimal.** Gyro sample → integrate → crop offset. No filter
  convergence, no model fitting. First frame after startup already has valid
  data.

The existing POC in `eis_crop.c` validates this: the entire signal chain is
`gyro_rad_s × dt → angle → pixels → crop`, with one LPF to extract the
smooth path. GyroGlide-Lite refines this with proper timestamp-based
integration, a spring-damper recenter, deadband, and slew limiting.

**Accelerometer role:** reserved for optional future horizon lock (static
tilt correction). Not in the hot path.

---

## 2. Minimal architecture

### Block diagram

```
 IMU SAMPLING PATH (imu_bmi270.c — existing)
 ────────────────────────────────────────────
 BMI270 FIFO ──I2C burst──> imu_drain()
                               │
                               ▼
                         push_fn callback
                               │
 TIMESTAMPED RING BUFFER       │
 ───────────────────────       │
                               ▼
                    eis_push_sample()   [via EIS dispatch]
                         MotionRing
                    [ts, gx, gy, gz] × 2048
                               │
 PER-FRAME BATCH EXTRACTION    │
 ──────────────────────────    │
                               │◄── frame event (MI_VENC_GetStream returns)
                               ▼
                    ring_read_range(t_prev, t_now)
                         MotionSample[N]
                               │
 PER-FRAME MOTION INTEGRATION  │
 ─────────────────────────     │
                               ▼
                    for each consecutive pair:
                      dt_i = ts[i] - ts[i-1]
                      Δyaw   += gyro_y[i] * dt_i
                      Δpitch += gyro_x[i] * dt_i
                    apply deadband
                               │
 PER-FRAME CROP DECISION       │
 ────────────────────────      │
                               ▼
                    position += Δ × gain
                    if motion < threshold:
                      position -= position × recenter_rate × frame_dt
                    position = slew_limit(position, prev, max_step)
                    position = clamp(position, -margin, +margin)
                               │
 HARDWARE CROP UPDATE          │
 ─────────────────────         │
                               ▼
                    crop_x = center_x + (int)position_x
                    crop_y = center_y + (int)position_y
                    align to even pixel
                    MI_VPE_SetPortCrop(ch, port, &rect)
```

### Component mapping

| Block | File |
|---|---|
| IMU sampling | `imu_bmi270.c` — FIFO + polling, push callback |
| Ring buffer | `eis_ring.h` — shared header-only, mutex-protected |
| Batch extraction | `eis_ring.h:eis_ring_read_range()` |
| Motion integration | `eis_gyroglide.c:gyroglide_compute()` — timestamp-based |
| Crop decision | `eis_gyroglide.c:gyroglide_compute()` — recenter + edge + slew |
| HW crop write | `eis_gyroglide.c:apply_crop()` → `MI_VPE_SetPortCrop()` |
| Frame trigger | `star6e_runtime.c` — `imu_drain()` then `eis_update()` before GetStream |

---

## 3. Control law — minimal state variables

### State (4 floats + 1 timestamp)

```c
float pos_x, pos_y;          /* crop offset in pixels, relative to center */
float bias_x, bias_y;        /* slow-adapting gyro bias estimate (rad/s) */
struct timespec t_prev;       /* timestamp of last frame update */
```

The simplified exponential recenter needs only position — no separate
velocity state. Compare to current POC which tracks `raw_angle_x/y` and
`smooth_angle_x/y` (4 floats).

### Control law per frame

```
# Inputs: Δangle_x, Δangle_y (integrated gyro over frame interval, radians)
#         frame_dt (seconds since last update)

# 1. Convert angular displacement to pixel displacement
Δpx_x = Δangle_x × pixels_per_radian × gain

# 2. Update position
pos_x = pos_x + Δpx_x

# 3. Motion-gated recenter (only when camera is still)
motion_px = |Δangle_x + Δangle_y| × pixels_per_radian
if motion_px < 0.5:
    pos_x = pos_x × (1.0 - recenter_rate × frame_dt)

# 4. Edge-aware recenter (always active — prevents margin saturation)
if |pos_x| / margin_x > 0.8:
    extra_decay ∝ (edge_fraction - 0.8)
    pos_x *= (1 - extra_decay)

# 5. Slew limit
pos_x = clamp(pos_x - prev_pos_x, -max_slew, +max_slew) + prev_pos_x

# 6. Clamp to margin
pos_x = clamp(pos_x, -margin_x, +margin_x)
```

**Important:** the recenter must NOT run during active motion — it would
fight the correction and make stabilization feel sluggish. Recenter only
activates when per-frame motion is below ~0.5px, allowing the crop to
smoothly return to center during stationary periods.

### Why this over the existing POC's LPF approach

The existing POC uses `smooth = smooth + alpha × (raw - smooth)`, then
`correction = smooth - raw`. This is mathematically a high-pass filter on
the gyro integral. The spring-recenter formulation is equivalent when
`recenter_rate ≈ 1/filter_tau`, but gives:

- **More intuitive tuning**: recenter_rate directly controls return speed
- **Better edge behavior**: edge-aware recenter prevents sticky saturation
- **Direct crop semantics**: pos_x IS the crop offset — no indirection
  through two diverging angle accumulators that can overflow over time

---

## 4. Processing the IMU batch between frames

### Defining the time interval

```
t0 = timestamp of previous frame event (stored as t_prev)
t1 = timestamp of current frame event (clock_gettime(CLOCK_MONOTONIC))
frame_dt = t1 - t0   (seconds, typically 16.6ms @ 60fps)
```

### Integrating gyro over dt — timestamp-based

The current POC divides frame_dt evenly across N samples:

```c
/* Current (approximate): */
double sample_dt = dt / n;
for (i = 0; i < n; i++)
    raw_angle_x += samples[i].gyro_x * sample_dt;
```

This is wrong when samples have non-uniform spacing. The correct approach
uses per-sample timestamps:

```c
/* GyroGlide-Lite (timestamp-based): */
float delta_x = 0.0f, delta_y = 0.0f;

for (uint32_t i = 0; i < n; i++) {
    float dt_i;
    if (i == 0)
        dt_i = ts_to_sec(t_prev, samples[0].ts);
    else
        dt_i = ts_to_sec(samples[i-1].ts, samples[i].ts);

    dt_i = clamp(dt_i, 0.0f, 0.05f);  /* sanity cap */

    float gx = samples[i].gyro_x - bias_x;
    float gy = samples[i].gyro_y - bias_y;

    delta_x += gx * dt_i;
    delta_y += gy * dt_i;
}

/* Tail extrapolation: last sample → frame time */
if (n > 0) {
    float dt_tail = ts_to_sec(samples[n-1].ts, t_now);
    if (dt_tail > 0.0f && dt_tail < 0.02f) {
        delta_x += (samples[n-1].gx - bias_x) * dt_tail;
        delta_y += (samples[n-1].gy - bias_y) * dt_tail;
    }
}
```

### Deadband

After integration, before applying to position:

```c
if (fabsf(delta_x) < deadband_rad) delta_x = 0.0f;
if (fabsf(delta_y) < deadband_rad) delta_y = 0.0f;
```

Suppresses sensor noise when stationary. ~0.001 rad/frame (≈0.06°/frame).

### Optional 1-pole smoothing

On the per-frame delta (not raw gyro — that would add lag):

```c
delta_x = alpha * delta_x + (1 - alpha) * prev_delta_x;  /* alpha ≈ 0.7-1.0 */
```

Start with alpha = 1.0 (disabled). Reduce only if frame-to-frame jitter is visible.

---

## 5. Float vs. fixed-point

**Recommendation: use `float` (IEEE 754 single-precision).**

- ARM Cortex-A7 (SSC30KQ/SSC338Q) has a VFPv4 FPU. Single-precision float
  operations are 1 cycle throughput. No penalty vs. integer math.
- The entire per-frame computation is ~30-50 float ops. At 500 MHz: ~100 ns.
- The existing codebase (`imu_bmi270.c`, `eis_crop.c`) is all `float`.
- Gyro values span many orders of magnitude (bias ~0.001, signal ~5 rad/s).
  Float handles this naturally; fixed-point needs careful Q format selection.

**Exception:** final crop coordinates are `uint16_t` — float→int at the end.

---

## 6. Frame timing and crop-application timing

### The core question

When `MI_VPE_SetPortCrop()` is called at the moment frame N exits the
encoder, does the new crop position affect frame N+1 or frame N+2?

### Expected behavior

The VPE pipeline is VIF→VPE (REALTIME) → VENC (FRAMEBASE). SetPortCrop()
is likely latched at next VPE VSYNC → **one-frame delay** (most likely).

### How to confirm experimentally

1. Enable test_mode sine wobble at known frequency
2. Record encoded video + log crop_x per frame
3. Compare phase of visible motion vs. logged crop values
4. Phase match → zero delay. One-frame shift → one-frame delay.

### Compensation

If confirmed one-frame late, apply predictive extrapolation:

```c
float predicted_x = pos_x + delta_x * px_per_rad * gain;
/* Use predicted_x for crop write */
```

---

## 7. Tuning guidance

| Parameter | Default | Range | Effect |
|---|---|---|---|
| `gain` | 1.0 | 0.0–1.0 | Fraction of detected motion to correct |
| `deadband_rad` | 0.0 | 0.0–0.01 | Per-frame angle below which correction zeroed |
| `recenter_rate` | 0.5 | 0.1–5.0 | Return-to-center speed when idle (1/s) |
| `margin_percent` | 30 | 5–30 | Overscan reserved for stabilization |
| `max_slew_px` | 0 | 0–20.0 | Max crop change per frame (0 = disabled) |
| `px_per_rad` | W/2 | auto | Lens FOV dependent (554 for 120° on 1920px) |
| `bias_alpha` | 0.001 | 0.0001–0.01 | Runtime bias adaptation rate |

### Hardware-validated recommendations

**Target frame rate: 60fps or lower.** At 120fps with 200Hz IMU, each frame
gets only ~1.6 gyro samples and an 8ms integration window — the per-frame
correction is too small for visible stabilization. At 60fps, each frame gets
~3.3 samples over 16.6ms, producing 2x larger corrections. The IMX335 sensor
also selects a higher-resolution mode at 60fps (2560x1920 vs 1920x1080 at
120fps), giving the VPE crop more input resolution to work with.

**Margin: 30% recommended (default, max safe).** At 10% the correction range
is only ±48px horizontal — too small for meaningful stabilization. 30%
produces a 1344x756 crop from 1920x1080 capture, giving ±288px horizontal
and ±162px vertical — enough to absorb significant hand shake. The VPE
scaler handles the upscale without measurable fps cost at 60fps.

**VPE hard limit: 30% maximum.** On SSC30KQ, margins above 30% (crop below
~70% of capture dimensions) cause VPE pipeline stalls when the VPE is also
scaling the input (e.g., 2560x1440 sensor → 1920x1080 output at 60fps).
Both backends clamp margin_percent to 30 and enforce a minimum crop of 70%
of capture. The config parser also clamps to this range.

**Deadband: 0.0 recommended.** The original default of 0.001 rad was too
aggressive for short integration windows — it suppressed real motion. With
motion-gated recentering, bias drift is handled by the recenter itself
when the camera is still, making a separate deadband unnecessary.

**Crop update placement:** The `imu_drain()` + `eis_update()` call should
happen BEFORE `MI_VENC_GetStream()` so the crop is latched by VPE for the
frame currently being captured, reducing latency by one frame.

### Tuning procedure

1. Start with defaults. Camera stationary → crop should stay at center ±1px.
2. Slow deliberate pan → should track briefly then recenter. Adjust recenter_rate.
3. Sharp shake → should respond immediately. If overcorrecting, reduce gain.
4. Sustained rotation → should hit margin then smoothly recenter.
5. Enable slew limiting if single-frame jumps are visible.

---

## 8. Failure modes and limitations

| Limitation | Explanation |
|---|---|
| No rotation compensation | Roll shake can't be fixed by crop |
| Resolution loss | 10% overscan = 10% fewer output pixels |
| Limited correction range | ±5% margin ≈ ±5° at typical FOV |
| No rolling-shutter correction | Line-by-line readout skew not addressed |
| Sustained rotation saturates | Deliberate pan fights the recenter spring |
| IMU failure | Graceful: zero correction, spring recenters |
| Gyro bias drift | Runtime adaptation + deadband masks residual |

---

## 9. Future improvements (preserving low complexity)

| Improvement | Complexity | Benefit |
|---|---|---|
| Gravity-referenced tilt | Medium | Horizon leveling via accel complementary filter |
| Exposure-aware scaling | Low | Better correction with fast shutter |
| One-frame prediction | Low | Compensate pipeline delay |
| Adaptive gain | Low | Soft saturation near margins |
| Intent detection | Medium | Reduce correction during deliberate pans |
| LDC warp path | High | Full rotation + rolling-shutter via VPE LDC |
| HTTP API tuning | Low | Live parameter adjustment without restart |

---

## 10. Implementation summary

| Feature | Implementation |
|---|---|
| Integration | Per-sample `ts[i]-ts[i-1]` with tail extrapolation |
| Stabilization model | Direct position + motion-gated recenter |
| Recenter | Exponential decay only when idle + edge-aware boost |
| Bias tracking | Runtime slow-adapt in hot path |
| Deadband | Optional per-frame delta gating |
| Slew limit | Optional per-frame max step |
| Edge behavior | Soft accelerated recenter before hard clamp |
| Modularity | EisOps vtable dispatch for pluggable backends |
