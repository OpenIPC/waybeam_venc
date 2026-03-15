# AE/AWB CPU Tuning (Star6E + Maruko)

This document describes the 3A (AE/AWB) processing strategy in the standalone
`venc` encoder.

## Custom 3A Thread (Star6E, default since v0.3.0)

Star6E runs a dedicated 15 Hz thread that handles both AE (auto-exposure) and
AWB (auto white balance), replacing the ISP's internal per-frame CUS3A
callbacks.  This is the default behavior — no configuration is needed.

### Why Custom 3A?

The ISP's built-in CUS3A runs AE/AWB callbacks at every sensor frame.  At
120fps this consumes significant CPU (~3.6ms per DoAe call = 30% throughput
loss).  The previous workaround (`--ae-cadence`) toggled CUS3A on/off every
N frames, but this was fragile and still relied on the ISP's opaque AE
algorithm.

The custom 3A thread pauses the ISP's internal AE via `MI_ISP_AE_SetState`
and disables the CUS3A AWB callback via `MI_ISP_CUS3A_Enable(1,0,0)`.  It
then polls HW statistics at a configurable rate (default 15 Hz) and applies
exposure/gain/WB corrections directly.

### AE Algorithm

Proportional controller with dead-band:
- Reads raw luma averages from `MI_ISP_AE_GetAeHwAvgStats` (128x90 grid)
- Target: average Y within configurable range (default 100-140 out of 255)
- Priority: increase shutter first, then gain (dark); decrease gain first,
  then shutter (bright)
- Max shutter auto-derived from sensor FPS (`1000000 / fps`), synced with
  the `isp.exposure` API control
- Max gain configurable (default 20480 = 20x, FPV-optimized for low noise)
- Step size configurable (default 10% per cycle)

### AWB Algorithm

Grey-world with IIR smoothing:
- Reads per-block R/G/B averages from `MI_ISP_AWB_GetAwbHwAvgStats`
- Normalizes R and B gains to match G channel (grey-world assumption)
- IIR filter: 70% old gain + 30% new target, preventing oscillation
- Dead-band: 2% change threshold to avoid churn in stable scenes
- Gain limits: 0.5x to 8x

### Configuration

All settings are in the `isp` section of `/etc/venc.json`:

| Field | Default | Description |
|-------|---------|-------------|
| `legacyAe` | `false` | Set `true` to revert to ISP internal AE + handoff |
| `aeFps` | `15` | 3A processing rate in Hz |
| `aeTargetLow` | `100` | Luma dead-band lower bound (0-255) |
| `aeTargetHigh` | `140` | Luma dead-band upper bound (0-255) |
| `aeChangePct` | `10` | AE step size per cycle (percent) |
| `aeGainMax` | `20480` | Max sensor gain (x1024; 20480 = 20x) |

Example — aggressive convergence with lower gain ceiling:
```json
{
  "isp": {
    "sensorBin": "/etc/sensors/imx335.bin",
    "aeFps": 25,
    "aeChangePct": 20,
    "aeGainMax": 10240
  }
}
```

### Interaction with Exposure API

Setting `isp.exposure` via the HTTP API updates both the ISP's exposure
limit and the custom AE thread's max shutter in real time.  Setting
`isp.exposure=0` resets to the FPS-derived default.

### Interaction with Manual AWB

Setting `isp.awb_mode=ct_manual` pauses the custom AWB loop.  The
user-specified color temperature is applied directly via `MI_ISP_AWB_SetAttr`.
Setting `isp.awb_mode=auto` resumes the custom AWB loop.

### Monitoring

The thread logs status every 5 seconds:
```
[cus3a] 300 frames, ae=23 awb=0, shutter=1306us gain=10240 avgY=132 wb=R2111/G1024/B2250 isp_ae=paused
```

At startup it verifies the ISP AE is paused and logs the configuration:
```
[cus3a] ISP AE state after pause: PAUSED (raw=1, ret=0)
[cus3a] CUS3A AWB/AF disabled (ret=0)
[cus3a] thread started: 15 Hz, target Y 100-140, gain 1024-20480, shutter 150-8333us, step 10%, awb=on
```

If the ISP AE state is unexpectedly re-enabled, the thread re-pauses it
and logs a warning.

### Benchmark Data (imx335, ISP bin loaded)

**120fps (mode 3, 1920x1080):**

| Config | Steady-state FPS | Notes |
|--------|-----------------|-------|
| Custom 3A (default) | 119 | Full rate, 15 Hz AE+AWB |
| Custom 3A, 30 Hz | 119 | Higher 3A rate, no throughput impact |
| Legacy AE | 119 | ISP internal AE (after CUS3A handoff) |

**All sensor modes (custom 3A, default settings):**

| Mode | Resolution | Target FPS | Actual FPS |
|------|-----------|-----------|------------|
| 0 | 2560x1920 | 30 | 30 |
| 1 | 2560x1920 | 60 | 60 |
| 2 | 2400x1350 | 90 | 83-89 |
| 3 | 1920x1080 | 120 | 119 |

## Legacy AE Mode

Set `"legacyAe": true` in the ISP config section to revert to the original
ISP-managed AE behavior:

1. CUS3A is enabled (1,1,1) at startup
2. After 1 second, CUS3A is disabled (0,0,0) — the "handoff"
3. The ISP's internal AE continues running autonomously
4. AWB runs via the ISP's internal callbacks at frame rate

This mode exists for compatibility and debugging.  The custom 3A thread is
not started and has zero overhead.

## Maruko Backend

Maruko keeps CUS3A enabled (1,1,1) permanently — the ISP pipeline requires
it for frame processing at >=60fps (without it, the ISP FIFO stalls).
The custom 3A thread is Star6E-only.  Maruko uses the ISP's internal AE/AWB
at all times.

## Automatic Exposure Cap

Both backends automatically cap `maxShutterUs` to `1000000 / fps` after ISP
bin load.  This prevents the ISP bin's default shutter limit (often 10ms)
from throttling high-fps modes.  The cap applies regardless of AE mode
(custom or legacy).

```
> Exposure cap: maxShutter 10000us -> 8333us (for 120 fps)
```

## Notes

- The custom 3A thread uses ~125 KB heap (90 KB AE stats + 35 KB AWB stats).
- All ISP symbols are resolved via `dlsym` — no build-time dependency on
  specific SDK versions.
- AE struct layout was verified via hex dump on Star6E imx335.  The
  `CusAEInfo_t` struct has 3 reserved u32s before actual fields (SDK-specific).
