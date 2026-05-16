# AE/AWB CPU Tuning (Star6E + Maruko)

This document describes the 3A (AE/AWB) processing strategy in the standalone
`venc` encoder.

## Supervisory AE Thread (opt-in via `isp.aeEngine: "custom"`)

The unified `isp.aeEngine` selector chooses between SDK-managed AE and
userspace cus3a:

| `aeEngine` | Star6E                          | Maruko                                |
|------------|---------------------------------|----------------------------------------|
| `"sdk"` (default) | ISP firmware AE drives the sensor (no supervisory thread) | NATIVE AE algo runs inside `3A_Proc_0` at sensor rate |
| `"custom"` | Lightweight supervisory thread enforces gain/shutter caps; ISP's internal AE still drives convergence | No-op AE adaptor replaces NATIVE algo + supervisory `SetAeParam` thread at `aeFps` Hz (~24% CPU saving at 120fps) |

Enable userspace AE by setting `"aeEngine": "custom"` in the config.

### How It Works

The ISP's internal AE stays in NORMAL state at all times.  The supervisory
thread monitors HW statistics and enforces constraints via
`MI_ISP_AE_SetExposureLimit()`:

1. **Startup**: Reads the ISP bin's default exposure limits as a baseline.
   If `gainMax` or `shutter_max_us` are configured, tightens the limits
   via a single `SetExposureLimit` call.
2. **Loop** (at `aeFps` Hz, default 15): Reads HW stats for avg_y
   monitoring.  If `shutter_max_us` or `gainMax` have changed at runtime
   (e.g. via the HTTP API), reads current limits, updates the changed
   field, and writes back.  Only calls `SetExposureLimit` when a limit
   actually changes — never spams it.
3. **Logging**: Every 5 seconds, logs avg_y, current shutter/gain, and
   ISP AE state (should always be "normal").

The CUS3A handoff (`enable(1,1,1)` -> 1s delay -> `enable(0,0,0)`) fires
normally after ISP bin load.  After handoff, the ISP's internal AE runs
autonomously in firmware at near-zero ARM CPU cost.  The supervisory thread
adds only the overhead of reading stats and (rarely) writing limits.

AWB is handled entirely by the ISP's internal AWB.  The `awbMode` control
(`auto` / `ct_manual`) in `star6e_controls.c` works independently of this
thread via direct ISP AWB API calls.

### Configuration

All settings are in the `isp` section of `/etc/waybeam.json`:

| Field | Default | Description |
|-------|---------|-------------|
| `aeEngine` | `"sdk"` | Set `"custom"` to use the supervisory AE thread |
| `aeFps` | `15` | Monitoring rate in Hz (used when `aeEngine="custom"`) |
| `gainMax` | `0` | Max sensor gain cap (0 = use ISP bin default) |

Shutter cap is auto-derived from sensor FPS (`1000000 / fps`) and can be
overridden at runtime via the `isp.exposure` API control.

Example — enable supervisory AE with gain cap:
```json
{
  "isp": {
    "aeEngine": "custom",
    "aeFps": 15,
    "gainMax": 10000
  }
}
```

### Interaction with Exposure API

Setting `isp.exposure` via the HTTP API updates both the ISP's exposure
limit (via `cap_exposure_for_fps`) and the supervisory thread's shutter cap
in real time.  Setting `isp.exposure=0` resets to the FPS-derived default.

### Interaction with Gain Max API

Setting `isp.gainMax` via the HTTP API updates the supervisory thread's gain
cap.  The thread picks up the change on its next cycle and writes it via
`SetExposureLimit`.  A value of 0 means "use ISP bin default" (no cap).

### Monitoring

The thread logs status every 5 seconds:
```
[cus3a] 300 frames, 2 limit writes, shutter=1306us gain=10240 avgY=132 isp_ae=normal
```

At startup it reads the ISP bin baseline and logs initial constraints:
```
[cus3a] ISP bin limits: gain 1024-32768, isp_gain max 1024, shutter 150-33333us
[cus3a] ISP AE state: NORMAL
[cus3a] initial limits: maxShutter=8333us maxGain=10000
[cus3a] supervisory thread started: 15 Hz, shutter cap 8333us, gain cap 10000
```

### Why Not Custom AE?

The previous implementation (pre-supervisory) used `MI_ISP_CUS3A_SetAeParam()`
to drive exposure directly from the thread.  Live hardware testing proved this
API does NOT write to sensor registers — the sensor stays frozen at whatever
the ISP set before the custom thread took over.  The ISP's internal AE in
NORMAL state is the only thing that actually drives the sensor.

`MI_ISP_AE_SetExposureLimit()` is the only working API for influencing exposure
— it modifies the bounds the ISP's internal AE operates within.

## SDK AE Mode (default)

The default mode (`aeEngine="sdk"`) uses the ISP's internal auto-exposure:

1. CUS3A is enabled (1,1,1) at startup
2. After 1 second, CUS3A is disabled (0,0,0) — the "handoff"
3. The ISP's internal AE continues running autonomously
4. AWB runs via the ISP's internal callbacks at frame rate

In this mode the supervisory thread is not started and has zero overhead.
Set `"aeEngine": "custom"` to switch to the supervisory thread.

## Maruko Backend

Maruko keeps CUS3A enabled (1,1,1) permanently — the ISP pipeline requires
it for frame processing at >=60fps (without it, the ISP FIFO stalls).
The supervisory thread is Star6E-only.  Maruko uses the ISP's internal AE/AWB
at all times.

## Automatic Exposure Cap

Both backends automatically cap `maxShutterUs` to `1000000 / fps` after ISP
bin load.  This prevents the ISP bin's default shutter limit (often 10ms)
from throttling high-fps modes.  The cap applies regardless of AE mode
(supervisory or legacy).

```
> Exposure cap: maxShutter 10000us -> 8333us (for 120 fps)
```

## Notes

- The supervisory thread uses ~90 KB heap (AE stats buffer only — no AWB
  stats needed).
- All ISP symbols are resolved via `dlsym` — no build-time dependency on
  specific SDK versions.
- AE struct layout was verified via hex dump on Star6E imx335.  The
  `CusAEInfo_t` struct has 3 reserved u32s before actual fields (SDK-specific).
