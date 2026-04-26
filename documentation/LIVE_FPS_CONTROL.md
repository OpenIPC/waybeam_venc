# Live FPS Control

## Overview

Live FPS control allows changing the encoder output frame rate at runtime via the
HTTP API without restarting the pipeline. The sensor continues running at its native
maximum rate; frame decimation is handled in hardware by the MI_SYS bind layer.

## Mechanism: Hardware Bind Decimation

The SigmaStar MI_SYS module supports frame decimation through `MI_SYS_BindChnPort2()`,
which accepts separate `src_fps` and `dst_fps` parameters. When `dst_fps < src_fps`,
the bind layer drops frames at the hardware level to match the ratio.

The VPE→VENC bind is the decimation point:
```
Sensor (90fps) → VIF (90fps) → VPE (90fps) → [bind 90:30] → VENC (30fps)
```

When the user changes FPS via the API, the bind is torn down and re-established
with the new ratio. The VENC rate control `fpsNum` is also updated to ensure
correct bitrate allocation at the new frame rate.

## API Usage

```bash
# Set output to 30fps (sensor stays at its native rate)
curl "http://<device>/api/v1/set?video0.fps=30"

# Set output to 60fps
curl "http://<device>/api/v1/set?video0.fps=60"

# Restore full sensor rate
curl "http://<device>/api/v1/set?video0.fps=90"
```

## Clamping Behavior

If the requested FPS exceeds the active sensor mode's `maxFps`, the value is
silently clamped to the mode maximum. A log message is emitted:

```
> FPS 120 exceeds sensor mode max 90, clamping
> FPS changed to 90 (bind 90:90)
```

To access a higher sensor mode (e.g. switching from 90fps mode to 120fps mode),
edit `/etc/venc.json` and restart the process.

## Why Sensor Always Runs at maxFps

The sensor is always configured to run at its mode's `maxFps` rather than the
requested output FPS. Setting intermediate values (e.g. 100fps on a 120fps mode)
can stall some sensors like the IMX335 — `MI_SNR_SetFps(100)` returns success but
the sensor physically cannot deliver, causing a pipeline stall.

By always running at maxFps and using downstream bind decimation, we avoid this
class of hardware bugs entirely.

## FPS-Aware Mode Selection

When venc starts, `sensor_mode_cost()` considers both resolution distance and FPS
excess when ranking sensor modes. The cost function places FPS excess in the high
32 bits and resolution distance in the low 32 bits:

```c
cost = (fps_excess << 32) | (res_cost & 0xFFFFFFFF)
```

This means a 90fps mode is always preferred over a 120fps mode when 90fps is
requested, even if the 120fps mode has a better resolution match.

## Pipeline Startup

At pipeline startup, the VPE→VENC bind already applies decimation if the config
fps is lower than the sensor mode's maxFps:

```c
uint32_t bind_src_fps = mode.maxFps;       // e.g. 120
uint32_t bind_dst_fps = config.video0.fps; // e.g. 90
MI_SYS_BindChnPort2(&vpe_port, &venc_port, bind_src_fps, bind_dst_fps, ...);
```

The VENC channel is also created with the config fps (not the sensor fps) for
correct rate control from the start.

## Sensor mode switching

`video0.fps` can be adjusted live (hardware bind decimation, see above).

Switching sensor *mode* (e.g. 90 fps mode → 120 fps mode) is also supported
without a process restart since 0.9.0 — `SIGHUP` and `/api/v1/restart`
reload the on-disk config and rebuild the pipeline from sensor up.  See
`documentation/SIGHUP_REINIT.md`.

## Tested Configurations (IMX335)

### 90fps mode (mode index 2, 2560x1440)
| Requested FPS | Bind ratio | VENC actual |
|---------------|-----------|-------------|
| 15 | 90:15 | 14.92 |
| 30 | 90:30 | 29.86 |
| 45 | 90:45 | 44.77 |
| 60 | 90:60 | 60.03 |
| 75 | 90:75 | (tested, working) |
| 90 | 90:90 | 89.56 |
| 100 | clamped→90 | 89.56 |
| 120 | clamped→90 | 89.56 |

### 120fps mode (mode index 3, 1920x1080)
| Requested FPS | Bind ratio | VENC actual |
|---------------|-----------|-------------|
| 30 | 120:30 | 29.92 |
| 60 | 120:60 | 59.80 |
| 90 | 120:90 | 90.00 |
| 120 | 120:120 | 119.80 |

## Implementation Files

- `src/backend_star6e.c` — `apply_fps()`, pipeline bind setup
- `src/sensor_select.c` — `sensor_mode_cost()` FPS-aware scoring,
  always-maxFps sensor configuration
- `include/sensor_select.h` — updated `sensor_mode_cost()` signature
