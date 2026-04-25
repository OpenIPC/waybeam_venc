# SIGHUP Reinit and Live Resolution Switching

## Overview

`venc` supports two reload mechanisms that do not require a process restart:

| Trigger | Config reload | Behaviour |
|---------|--------------|-----------|
| `killall -1 venc` (SIGHUP) | Disk (`venc.json`) | Full pipeline reinit |
| `GET /api/v1/restart` | Disk (`venc.json`) | Full pipeline reinit |
| `GET /api/v1/set?video0.size=WxH` | In-memory only | Pipeline reinit |

All three rebuild VENC, output, audio, and IMU from the new config.  The
sensor, VIF, and VPE stages are preserved to the extent the new resolution
allows (see below).

---

## Why the MIPI PHY Must Not Be Cycled

The SigmaStar Infinity6E MIPI D-PHY receiver does not survive an
`MI_SNR_Disable` / `MI_SNR_Enable` cycle at runtime.  After `MI_SNR_Enable`
returns, the PHY clock lane enters a fault state.  Subsequent VIF→VPE frame
transfers stall, `MI_VENC_StopRecvPic` never completes, and the process
enters D-state (uninterruptible sleep).

The prior reinit path called `star6e_pipeline_stop()` (which includes
`MI_SNR_Disable`) followed by `star6e_pipeline_start()`.  This reliably
produced the D-state hang.

**The fix:** partial teardown.  `star6e_pipeline_stop_venc_level()` tears
down only the layers above VPE:

```
Sensor  ─ VIF ─ VPE   ← preserved across reinit
                  │
               VENC    ← torn down and rebuilt
                  │
              Output   ← torn down and rebuilt
```

The VIF→VPE REALTIME bind remains active throughout.  `MI_SNR_Disable` is
never called.

---

## Reinit Levels for Resolution Changes

When `video0.size` changes, `star6e_pipeline_reinit()` checks whether the
precrop rectangle changes.  Precrop is a center-crop of the sensor frame to
match the target aspect ratio (see `documentation/PRECROP_ASPECT_RATIO.md`).

### Same aspect ratio — VPE port resize only

Example: `1920x1080` → `1280x720` (both 16:9).

The precrop rectangle is identical for both resolutions on a 16:9 sensor.
Only the VPE output port dimensions change:

```
MI_VPE_DisablePort(0, 0)
MI_VPE_SetPortMode(0, 0, &new_dims)
MI_VPE_EnablePort(0, 0)
```

VIF, the VIF→VPE bind, and the VPE channel itself are untouched.

### Different aspect ratio — VIF crop + VPE recreate

Example: `1920x1080` (16:9) → `1920x1440` (4:3).

The precrop rectangle shifts (different width or height), so VIF must
capture a different region and VPE must be recreated with new `capt`
dimensions:

```
MI_SYS_UnBindChnPort(VIF → VPE)        ← unbind
MI_VPE_DisablePort / StopChannel / DestroyChannel
MI_VIF_DisableChnPort(0, 0)
MI_VIF_SetChnPortAttr(0, 0, &new_crop)  ← new precrop region
MI_VIF_EnableChnPort(0, 0)
MI_VPE_CreateChannel / SetChannelParam / StartChannel / SetPortMode / EnablePort
```

The VIF **device** (`MI_VIF_EnableDev`) stays enabled.  Only the port crop
is updated.  The MIPI PHY is never touched.

After VPE is recreated, `bind_and_finalize_pipeline()` re-establishes the
VIF→VPE REALTIME bind and the VPE→VENC FRAMEBASE bind normally.

### Overscan handling

Sensors that report a larger `plane.capt` than the usable `mode.output`
area (MIPI overscan) are handled correctly.  Precrop is computed from the
usable area (`mode.output`) and the overscan half-offset is added to the
VIF crop coordinates — matching the logic in `select_and_configure_sensor()`
used at first start.

---

## ISP Channel Readiness

`MI_VPE_CreateChannel` starts ISP channel initialisation asynchronously.
Any ISP API call issued before the kernel logs `MhalCameraOpen` will receive
an error:

```
[MS_CAM_IspApiGet][ERROR - ISP channel [0] have NOT been created.
```

Two guards prevent this:

1. **`star6e_pipeline_wait_isp_channel()`** — called in
   `bind_and_finalize_pipeline()` inside the `!state->bound_vif_vpe` block
   (i.e., only when a new VIF→VPE bind is being established).  Polls
   `MI_ISP_IQ_GetParaInitStatus` until `bFlag == 1` or 2000 ms elapses.
   Fires on first start and on every AR-change reinit.  Skipped on ordinary
   SIGHUP reinits (VPE kept alive, ISP already ready).

2. **`star6e_pipeline_wait_isp_ready()`** — called inside
   `isp_runtime_load_bin_file()` before loading the ISP bin.  Also polls
   `MI_ISP_IQ_GetParaInitStatus`.  Provides a second gate before the bin
   load writes ISP registers.

---

## GCC `flatten` Attribute

Both `star6e_pipeline_start()` and `star6e_pipeline_reinit()` are annotated
with `__attribute__((flatten))`.

The SigmaStar I6E ISP driver inspects the call-stack layout at the moment
`MI_VPE_CreateChannel` is called.  At `-Os`, GCC may emit `start_vpe()` as
a separate out-of-line function when it has two call sites (start and
AR-change reinit).  The different stack layout causes `MI_ISP_IQ_GetParaInitStatus`
to return error 6, aborting ISP channel creation.  `flatten` forces all
static callees to be inlined, restoring the monolithic stack frame the
driver expects.

---

## Limitations

- **Sensor mode changes require a process restart.**  The sensor mode
  (resolution, FPS tier) is selected once at startup and locked for the
  process lifetime.  `video0.fps` can be adjusted live (hardware bind
  decimation), but switching from e.g. 60fps mode to 120fps mode requires
  restarting `venc`.  The reinit path clamps `video0.fps` to
  `sensor.mode.maxFps` if the new config requests a higher FPS.

- **`video0.codec` and stream mode changes** also require a process restart
  (marked `restart_required` in the API contract but the reinit path does
  not yet handle codec switching).

- **Dual VENC (Gemini mode)** is torn down completely on every reinit and
  must be restarted via the API after the pipeline comes back up.
