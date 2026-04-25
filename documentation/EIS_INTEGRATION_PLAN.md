# EIS Integration Plan — Star6E (SSC338Q / Infinity6E)

> **Status (0.8.0): historical reference.** The EIS module described in
> "Phase B: Crop-Based EIS" was removed in 0.8.0 — see HISTORY for
> the rationale (worked in only one validated config; not worth the
> shipped maintenance surface). This document is retained for the
> Phase C LDC-warp design notes, which remain valid and would be the
> starting point for any future stabilization work. The IMU module
> (Phase A) is also retained, compiled but disabled by default.

## Investigation Findings

### Available Hardware APIs (confirmed via libmi_vpe.so symbol export)

The Star6E VPE library exports a rich set of EIS-relevant APIs that are **not
currently used** by the venc encoder:

| Symbol | Purpose | EIS Relevance |
|--------|---------|---------------|
| `MI_VPE_SetPortCrop` | Per-frame dynamic crop on VPE output port | **Primary crop-based EIS** — translation compensation |
| `MI_VPE_SetChannelCrop` | Input-level crop before ISP processing | Global stabilization margin |
| `MI_VPE_LoadPortZoomTable` | Load batch of crop windows (N entries) | Batch EIS: pre-compute crop sequence |
| `MI_VPE_StartPortZoom` / `StopPortZoom` | Auto-step one crop entry per frame | Hardware-driven EIS playback |
| `MI_VPE_LDCSetViewConfig` | LDC warp configuration (V2 embedded in VPE) | **Full 3×3 matrix warp EIS** |
| `MI_VPE_LDCBegViewConfig` / `LDCEndViewConfig` | Batch LDC config updates | Atomic LDC parameter changes |
| `MI_VPE_LDCSetBatchViewConfig` | Multi-view LDC batch | Rolling-shutter slice support |
| `MI_VPE_CallBackTask_Register` | ISP frame-start/end IRQ callbacks | Frame-precise IMU synchronization |
| `MI_VPE_SetPortShowPosition` | Output positioning with black fill | Letterbox stabilization output |

### Available Timestamp/Sync APIs (confirmed via libmi_sys.so)

| Symbol | Purpose |
|--------|---------|
| `MI_SYS_GetCurPts` | Read current system PTS (microseconds) |
| `MI_SYS_InitPtsBase` | Set PTS clock baseline |
| `MI_SYS_SyncPts` | Sync PTS to external clock |

### Current Pipeline Architecture

```
VIF ──REALTIME──> VPE ──FRAMEBASE──> VENC ──UDP──> ground station
```

- VIF→VPE: `I6_SYS_LINK_REALTIME` (zero-copy, no frame interception)
- VPE→VENC: `I6_SYS_LINK_FRAMEBASE` (frames in DRAM, timestamps accessible)
- VPE channel: `lensAdjOn = 0` (LDC disabled), no crop configured
- VPE port: output scaling only (width/height), no crop set

### IMU Hardware (from wfb_openipc repository)

- **Sensor**: BMI270 (Bosch) on I2C bus 1, address 0x68
- **Driver**: Userspace C driver via `bmi270.h` (from CoRoLab-Berlin/bmi270_c)
- **Sample rate**: 200 Hz (configurable up to ~1000 Hz)
- **Gyro range**: ±1000 deg/s, 16-bit raw
- **Accel range**: ±2g, 16-bit raw
- **Output modes**: Debug (stdout), UDP (CSV/binary to ground station), GCSV file logging
- **Calibration**: 2-step flat/tilt routine, gyro bias removal, rotation matrix alignment
- **Existing code**: `openipc_imu.c` (basic), `openipc_imu_gcsv.c` (calibrated + GCSV logging),
  `openipc_imu_rad_calibration.c` (rad/s output with calibration)

### SDK Version

This is V2 API — LDC is embedded inside VPE (the `MI_VPE_LDC*` functions), not
a standalone MI_LDC module. No `libmi_ldc.so` required; all LDC APIs are in
`libmi_vpe.so`.

---

## EIS Implementation Approaches (Ranked)

### Approach 1: VPE Port Crop EIS (translation-only, simplest)

**How it works:** Oversample the sensor by 10-20% beyond output resolution.
Each frame, read gyro data, compute X/Y displacement, call
`MI_VPE_SetPortCrop()` to shift the crop window, compensating for motion.

**Pros:**
- Simplest implementation — single API call per frame
- No LDC configuration complexity
- Can be called while channel is running (no stop/restart)
- Works with existing FRAMEBASE binding

**Cons:**
- Translation only (2-DOF) — no rotation compensation
- Requires overscan margin (wastes 10-20% sensor resolution)
- No rolling-shutter correction

**Required changes:**
1. Add IMU reader thread (BMI270 via I2C, from existing wfb_openipc code)
2. Oversample: set VPE capture larger than output (e.g., 2048×1536 capture → 1920×1080 output)
3. Per-frame loop: read gyro → integrate → compute crop offset → `MI_VPE_SetPortCrop()`
4. Timestamp sync: `MI_SYS_GetCurPts()` at IMU sample time, match to frame `u64Pts`

### Approach 2: VPE Zoom Table EIS (batch crop, hardware-driven)

**How it works:** Pre-compute a batch of N crop windows from accumulated gyro
data, load via `MI_VPE_LoadPortZoomTable()`, then `MI_VPE_StartPortZoom()`
auto-applies one entry per frame.

**Pros:**
- Hardware-driven frame stepping — no per-frame API call jitter
- Good for predictable/smooth motion compensation

**Cons:**
- Batch latency: must pre-compute before loading
- `MI_VPE_SetPortCrop` disabled while zoom active
- Reload requires stop-load-start cycle
- Still translation-only

### Approach 3: VPE LDC Warp EIS (full 3×3 homography)

**How it works:** Enable LDC in VPE (`lensAdjOn = 1`), configure
`i6e_vpe_ildc` with DIS mode, per-frame update `i6e_vpe_ldc.proj3x3` via
`MI_VPE_LDCSetViewConfig()` with gyro-derived 3×3 rotation matrix.

**Pros:**
- Full rotation compensation (3-DOF or more)
- Rolling-shutter correction via `userSliceNum`
- Hardware-accelerated warp engine
- Can combine with lens distortion correction

**Cons:**
- Most complex to implement
- LDC struct fields (`i6e_vpe_ildc`) need careful initialization
- Underdocumented — struct field semantics inferred from headers
- May require `configAddr` blob from vendor tooling
- Risk of VPE pipeline stall if misconfigured

**Required changes:**
1. Everything from Approach 1 (IMU thread, timestamp sync)
2. Set `lensAdjOn = 1` in both `i6e_vpe_chn.lensInit` and `i6e_vpe_para`
3. Configure `proj3x3On = 1`, `userSliceNum`, `focalLengthX/Y`
4. Per-frame: compute 3×3 homography from gyro → `MI_VPE_LDCSetViewConfig()`

---

## Recommended Phased Implementation

### Phase A: IMU Integration — COMPLETE

BMI270 driver integrated as `src/imu_bmi270.c`. FIFO mode (frame-synced,
no thread) is the primary path; polling mode as fallback. Validated on
SSC30KQ hardware with `/dev/i2c-1` at 200 Hz.

### Phase B: Crop-Based EIS — COMPLETE

Implemented as modular EIS framework with two backends:
- `eis_legacy.c` — original LPF-based approach (Approach 1 as described above)
- `eis_gyroglide.c` — GyroGlide-Lite: timestamp-based integration, motion-gated
  recenter, edge-aware recentering, slew limiting, runtime bias adaptation

Hardware-validated on SSC30KQ at 60fps with BMI270 IMU. VPE crop margin
capped at 30% (VPE stalls above this when scaling is active). See
`documentation/GYROGLIDE_LITE_DESIGN.md` for the full design.

### Phase C: LDC Warp EIS (Approach 3) — FUTURE

Upgrade to full rotation compensation if crop-based EIS proves insufficient.

1. Enable VPE LDC, configure projection matrix pipeline
2. Compute per-frame 3×3 rotation matrix from integrated gyro quaternion
3. Feed via `MI_VPE_LDCSetViewConfig()`
4. Add rolling-shutter compensation via slice-based matrices

---

## Key Unknowns / Risks

1. **LDC config blob**: `i6e_vpe_ildc.configAddr` — unclear if this is required
   for DIS_GYRO mode or only for lens distortion correction. May need a dummy
   config or vendor-generated calibration file.

2. **MI_VPE_SetPortCrop struct**: The HAL header `i6_vpe_port` has no crop field,
   but `MI_VPE_SetPortCrop` is exported. Need to determine the correct struct
   (likely `MI_SYS_WindowRect_t` / `i6_common_rect`).

3. **Frame timing precision**: The VPE→VENC FRAMEBASE link gives us frame PTS,
   but we need to verify that `MI_SYS_GetCurPts()` and frame `u64Pts` share the
   same timebase for IMU correlation.

4. **IMU-camera time alignment**: BMI270 is sampled via I2C polling (not
   hardware-triggered). Jitter in I2C reads may limit sync precision.
   VPE callback (`MI_VPE_CallBackTask_Register`) could provide frame-start
   IRQ for tighter sync, but requires investigation.

5. **LDC + REALTIME binding**: VIF→VPE uses REALTIME link. Enabling LDC may
   change VPE processing latency. Need to verify pipeline doesn't stall.
