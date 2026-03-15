# /proc/mi_modules Reference for Star6E Pipeline Debugging

Verified on SSC338Q (Star6E) with IMX415 sensor.
All paths under `/proc/mi_modules/`.

See also: [SigmaStar Pudding SDK API Reference](https://wx.comake.online/doc/doc/SigmaStarDocs-Pudding-0120/) for MI_SNR, MI_VIF, MI_VPE, MI_VENC, MI_SYS, and MI_ISP module APIs.

## Quick FPS Verification

To verify actual pipeline FPS while venc is running:

```sh
# Sensor pad — what the sensor hardware is actually delivering
grep -A1 "PadId.*bEnable.*fps" /proc/mi_modules/mi_sensor/mi_sensor0

# VIF output — frames leaving the video input formatter
grep -A1 "GetFrame/Ms.*FPS.*GetTotalCnt" /proc/mi_modules/mi_vif/mi_vif0

# VPE input — frames entering video processing with configured vs actual rate
grep -A1 "SrcFrmrate.*DstFrmrate.*GetFrame" /proc/mi_modules/mi_vpe/mi_vpe0

# VPE output port — processed frames with actual fps
grep -A1 "PortId.*passid.*bindtype.*fps" /proc/mi_modules/mi_vpe/mi_vpe0

# VENC device — encoder hardware FPS and macroblock rate
grep -A1 "MaxTaskCnt.*FPS.*MbRate" /proc/mi_modules/mi_venc/mi_venc0

# VENC channel — per-second FPS and bitrate
grep -A1 "State.*Fps_1s.*kbps" /proc/mi_modules/mi_venc/mi_venc0
```

---

## Module Details

### mi_sensor (`mi_sensor/mi_sensor0`)

**Pad info** — the authoritative source for what the sensor is actually doing:

| Field | Meaning |
|-------|---------|
| `PadId` | Sensor pad index (0-based) |
| `bEnable` | 1 if pad is active |
| `fps` | **Actual sensor FPS** — this is the real hardware frame rate, not what was requested. If this differs from requested, the sensor driver couldn't achieve it. |
| `ResCnt` | Number of available resolution modes |
| `intfmode` | Interface (MIPI, BT656, etc.) |
| `planecnt` | Number of data planes |

**Resolution table** — lists all available sensor modes:

| Field | Meaning |
|-------|---------|
| `strResDesc` | e.g. `1920x1080@90fps` |
| `CropX/Y/W/H` | Sensor crop window |
| `OutW/OutH` | Output dimensions |
| `MaxFps/MinFps` | Mode FPS range |
| `Cur` | Currently active mode (marked separately) |

**Plane info** — sensor data format:

| Field | Meaning |
|-------|---------|
| `SnrName` | Sensor driver name (e.g. `IMX415_MIPI`) |
| `BayerId` | Bayer pattern (GB, RG, etc.) |
| `ePixPrec` | Pixel precision (e.g. `12BPP`) |

**Key insight**: If sensor pad `fps` field shows 71 but you requested 90, the sensor hardware is not running at the requested rate. On IMX415, loading the ISP bin (`--isp-bin /etc/sensors/imx415_greg_fpvXVIII-gpt200.bin`) is required for >60fps — without it, the sensor maxes out around 71fps regardless of mode setting.

---

### mi_vif (`mi_vif/mi_vif0`)

**Output port stats** — frames captured from sensor:

| Field | Meaning |
|-------|---------|
| `GetFrame/Ms` | Frame interval numerator/denominator in ms (e.g. `91/1005` = ~90fps) |
| `FPS` | Measured output FPS (e.g. `90.54`) |
| `FinishCnt` | Total frames delivered |
| `GetTotalCnt` | Total frame requests |
| `GetOkCnt` | Successful frame gets |

**Dev stats** — hardware-level:

| Field | Meaning |
|-------|---------|
| `Intf` | Interface type (MIPI, RGB_REAL, etc.) |
| `IsrCnt` | Interrupt count (should grow steadily) |
| `infmt` | Input pixel format (e.g. `GB_12BPP`) |
| `DropCnt` | Frames dropped at VIF level — nonzero indicates backpressure |

**Outport stats**:

| Field | Meaning |
|-------|---------|
| `Fps` | Measured FPS at VIF output |
| `OutCount` | Total output frames (hex) |
| `FailCount` | Failed outputs — nonzero indicates problems |

---

### mi_vpe (`mi_vpe/mi_vpe0`)

**Input port** — frames entering video processing:

| Field | Meaning |
|-------|---------|
| `SrcFrmrate` | Configured source frame rate (e.g. `90/1`) |
| `DstFrmrate` | Configured destination frame rate (e.g. `90/1`) |
| `GetFrame/Ms` | Actual measured frame interval |
| `FPS` | Actual measured input FPS |
| `FinishCnt` | Frames processed |
| `RewindCnt` | Frame rewinds (retries) — high count suggests processing can't keep up |
| `BindInQ_cnt` | Frames queued from bound input — persistent nonzero means backpressure |

**Output port** — processed frames:

| Field | Meaning |
|-------|---------|
| `bindtype` | Binding mode (`Frame` = frame-based, used for VPE->VENC) |
| `Pixel` | Output pixel format (e.g. `YUV420SP`) |
| `OutputW/OutputH` | Output resolution |
| `fps` | Actual output FPS |
| `GetCnt` | Total output frame count |
| `FailCnt` | Failed outputs — nonzero is a problem |
| `FinishCnt` | Successfully finished frames |
| `Histogram` | Frame processing time histogram value |

**Channel params**:

| Field | Meaning |
|-------|---------|
| `3DNRLevel` | Active 3DNR level (0-7) |
| `bMirr/flip` | Mirror and flip state |
| `RunMode` | `RealTime` or other VPE modes |
| `SensorId` | Bound sensor ID |

**ISP interrupt stats** (in Dev info):

| Field | Meaning |
|-------|---------|
| `VsyncCnt` | Vertical sync count — should match sensor FPS * uptime |
| `FrameDoneCnt` | ISP frame-done count |
| `DropCnt` | ISP-level drops — indicates ISP can't process fast enough |

**Pipeline delay** (in us):

| Field | Meaning |
|-------|---------|
| `EnqueueInputTask` Avg | Average time to enqueue a frame for processing |
| `DequeueOutputTask` Avg | Average total pipeline latency through VPE |
| `FinDMADispatch` Avg | DMA dispatch latency |

---

### mi_venc (`mi_venc/mi_venc0`)

**Input port** — frames entering encoder:

| Field | Meaning |
|-------|---------|
| `SrcFrmrate` | Configured source frame rate |
| `FPS` | Actual measured input FPS |
| `FinishCnt` | Frames encoded |

**Device stats** — encoder hardware:

| Field | Meaning |
|-------|---------|
| `FPS` | Encoder hardware FPS |
| `MbRate` | Macroblock rate (macroblocks/sec) — useful for checking HW encoder load |
| `%` | Encoder utilization percentage |
| `UtilHw/UtilMi` | Hardware and MI utilization |
| `SupportRing` | Ring buffer support flag |

**Channel stats** — per-channel encoding:

| Field | Meaning |
|-------|---------|
| `Fps_1s` | FPS averaged over last 1 second |
| `kbps` (after Fps_1s) | Bitrate in kbps over last 1 second |
| `Fps10s` | FPS averaged over last 10 seconds |
| `kbps` (after Fps10s) | Bitrate over last 10 seconds |
| `Gardient` | Frame gradient/complexity metric |
| `FrameIdx` | Current frame index |

**Channel config**:

| Field | Meaning |
|-------|---------|
| `CODEC` | H264 or H265 |
| `Profile` | Codec profile |
| `BufSize` | Allocated buffer size |
| `RefNum` | Reference frame count |
| `bByFrame` | Frame-based encoding flag |
| `FrameCnt` | Total encoded frames |
| `DropCnt` | Encoder drops — frames skipped by encoder |
| `ReEncCnt` | Re-encode count — frames that needed re-encoding |

**Rate control**:

| Field | Meaning |
|-------|---------|
| `RateCtl` | Rate control mode (CBR, VBR, AVBR, etc.) |
| `GOP` | GOP size |
| `MaxBitrate` | Configured max bitrate |
| `MaxQp/MinQp` | QP range for P-frames |
| `MaxIQp/MinIQp` | QP range for I-frames |

**Pipeline delay** (in us):

| Field | Meaning |
|-------|---------|
| `GetStream` Avg | Average time from frame input to encoded stream output — this is the encode latency |

**Input port HW stats**:

| Field | Meaning |
|-------|---------|
| `SrcFrmRate` | Configured input frame rate (e.g. `90/1`) |
| `FrameCnt` | Frames received |
| `DropCnt` | Frames dropped at encoder input — indicates encoder can't keep up |
| `BlockCnt` | Blocked frame count — input stalls |

---

## Common Diagnostic Patterns

### Verify actual FPS matches requested
Compare sensor pad `fps` field against what was passed to `-f`. If they differ, the sensor driver or ISP bin is the bottleneck.

### Detect pipeline backpressure
Check `DropCnt` at each stage (VIF, VPE ISP, VENC input). Drops propagate downstream — find the first stage with nonzero drops.

### Measure end-to-end latency
Sum the pipeline delay `Avg` values: VIF `FinDMADispatch` + VPE `DequeueOutputTask` + VENC `GetStream`.

### Check encoder saturation
VENC device `%` (utilization) near 100 means the hardware encoder is at capacity. Options: lower resolution, lower FPS, or use a less complex codec profile.

### Verify bitrate
VENC channel `kbps` (1s average) should be close to the configured `-r` rate. Large deviations indicate rate control issues.

### Detect sensor mode mismatch
Compare sensor pad `Cur` resolution/fps against what was expected. If `MI_SNR_SetFps()` succeeded but pad shows different fps, the sensor hardware couldn't achieve it.

---

## One-Liner: Full Pipeline FPS Snapshot

```sh
echo "=== Sensor ===" && grep -A1 "PadId.*bEnable.*fps" /proc/mi_modules/mi_sensor/mi_sensor0 && \
echo "=== VIF ===" && grep -A1 "GetFrame/Ms.*FPS.*GetTotalCnt" /proc/mi_modules/mi_vif/mi_vif0 && \
echo "=== VPE In ===" && grep -A1 "SrcFrmrate.*DstFrmrate.*GetFrame" /proc/mi_modules/mi_vpe/mi_vpe0 && \
echo "=== VPE Out ===" && grep -A1 "PortId.*passid.*bindtype.*fps" /proc/mi_modules/mi_vpe/mi_vpe0 && \
echo "=== VENC ===" && grep -A1 "State.*Fps_1s.*kbps" /proc/mi_modules/mi_venc/mi_venc0
```
