# Automatic Aspect-Ratio Precrop

## Problem

When the requested encode resolution has a different aspect ratio than the
sensor mode, the VPE/SCL scaler must map a wider (or taller) input rectangle
onto the output dimensions. Without intervention this produces **non-uniform
scaling** — the image is stretched or squished along one axis.

Example: sensor delivers 1920x1080 (16:9), encode is 1440x1080 (4:3).
A naive VPE output-only resize would compress 1920 horizontal pixels into
1440, distorting the image.

## Solution: Center Precrop

Before scaling, crop the sensor frame to a rectangle whose aspect ratio
matches the target encode resolution, then let the scaler handle any
remaining uniform downscale.

"Precrop" means the crop is applied **early in the pipeline** — before ISP
processing and scaling — so downstream stages operate on native sensor pixels
rather than interpolated data.

### Quality Benefit

- Each output pixel maps to the fewest possible sensor pixels.
- For 1440x1080 from a 1920x1080 sensor: **zero scaling** — every output
  pixel is exactly one sensor pixel (just a spatial crop).
- For 960x720 from a 1920x1080 sensor: crop to 1440x1080 (AR correction),
  then uniform 1.5x downscale to 960x720.
- No aspect-ratio distortion in either case.

## Pipeline Integration

### Star6E  (Sensor -> VIF -> VPE -> VENC)

Precrop is applied at the **VIF capture rectangle** (`port.capt`).

The `i6_vif_port` struct has:
- `capt` (`i6_common_rect`): x, y, width, height — defines which sensor
  pixels to capture.
- `dest` (`i6_common_dim`): width, height — VIF output dimensions.

By setting `capt` to the AR-corrected center crop and `dest` to match,
VIF outputs a correctly-proportioned frame. VPE channel input (`ch.capt`)
is set to the cropped dimensions; the VPE output port scales to the final
encode resolution.

```
  Sensor 1920x1080
       |
  VIF capt: {x=240, y=0, w=1440, h=1080}   <-- precrop
  VIF dest: 1440x1080
       |
  VPE ch.capt: 1440x1080
  VPE port.output: 1440x1080 (or smaller if further downscale needed)
       |
  VENC
```

### Maruko  (Sensor -> VIF -> ISP -> SCL -> VENC)

Precrop is applied at the **SCL port crop field** (`scl_port.crop`).

The `i6c_scl_port` struct has:
- `crop` (`i6c_common_rect`): x, y, width, height — crop before scaling.
- `output` (`i6c_common_dim`): width, height — scaling target.

ISP port crop stays at full sensor dimensions so that 3A (auto-exposure,
auto-white-balance, auto-focus) algorithms see the full frame.

```
  Sensor 1920x1080
       |
  VIF (full frame)
       |
  ISP (full frame, 3A runs on full image)
       |
  SCL crop: {x=240, y=0, w=1440, h=1080}   <-- precrop
  SCL output: 1440x1080 (or smaller)
       |
  VENC
```

## Algorithm

```c
sensor_ar = sensor_w * image_h;
image_ar  = image_w * sensor_h;

if (sensor_ar == image_ar) {
    // Same aspect ratio — no crop, just scale
} else if (sensor_ar > image_ar) {
    // Sensor is wider — crop width, keep full height
    crop_h = sensor_h;
    crop_w = round_up(sensor_h * image_w / image_h);
    crop_w = align_down_2(crop_w);
    crop_x = align_down_2((sensor_w - crop_w) / 2);
    crop_y = 0;
} else {
    // Sensor is taller — crop height, keep full width
    crop_w = sensor_w;
    crop_h = round_up(sensor_w * image_h / image_w);
    crop_h = align_down_2(crop_h);
    crop_x = 0;
    crop_y = align_down_2((sensor_h - crop_h) / 2);
}
```

All dimensions are 2-pixel aligned (SigmaStar hardware requirement).
Cross-multiplication avoids floating-point and preserves precision.

## Proc Interface (Debug / Manual Override)

VPE precrop can also be set at runtime via `/proc`:

```
echo setprecrop <chn> <port> <x> <y> <w> <h> > /proc/mi_modules/mi_vpe/mi_vpe0
```

Example for 1440x1080 center crop on a 1920x1080 sensor:
```
echo setprecrop 0 0 240 0 1440 1080 > /proc/mi_modules/mi_vpe/mi_vpe0
```

Parameters: channel, port, x-offset, y-offset, width, height.

Note: The SDK documentation states `MI_VPE_SetInputportCrop` is **not
supported in realtime mode** (`E_MI_VPE_RUN_REALTIME_MODE`). The proc
interface may bypass this restriction on some SoCs. The application uses
VIF-level crop (Star6E) or SCL-level crop (Maruko) instead, which work
in all modes.

## Configuration

Precrop behaviour is controlled by the `isp.keepAspect` JSON config field
on both Star6E (VIF capture rect) and Maruko (SCL port crop).

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `keepAspect` | boolean | `true` | `true` — center-crop sensor to encode AR before scaling. `false` — pass full sensor area to VIF; VPE scales the entire frame to encode dimensions (image is stretched if ARs differ). |

**Important:** the value must be a JSON boolean. `"keepAspect": 0` is
treated as the default (`true`) by `json_get_bool`.

Mutability is `MUT_RESTART`: changes apply on SIGHUP / `Save & Restart`.
The reinit path detects a precrop delta and reconfigures VIF + VPE without
disturbing the sensor or MIPI PHY.

Example `isp` section with aspect-ratio cropping disabled:
```json
"isp": {
  "sensorBin": "/etc/sensors/imx335_fpv.bin",
  "keepAspect": false
}
```

## Behavior

- **Conditional on `keepAspect`**: precrop is computed and applied only
  when `isp.keepAspect` is `true` (default). When `false`, the full sensor
  area is captured and VPE stretches the entire frame to the encode
  dimensions.
- **Informational output**: when precrop is active, the startup banner
  prints the crop rectangle (requires `system.verbose: true`):
  ```
  - Precrop: 1920x1080 -> 1440x1080 (offset 240,0)
  ```
- **No-op when same AR**: e.g. 1280x720 on a 1920x1080 sensor (both 16:9)
  skips precrop entirely — only VPE/SCL scaling is used.

## Common Scenarios

| Sensor       | Encode      | AR Match? | Precrop Region         | Then Scale       |
|-------------|-------------|-----------|------------------------|------------------|
| 1920x1080   | 1440x1080   | No (4:3)  | 1440x1080 @ (240,0)   | None             |
| 1920x1080   | 960x720     | No (4:3)  | 1440x1080 @ (240,0)   | 1440x1080->960x720 |
| 1920x1080   | 1280x720    | Yes (16:9)| None                   | 1920x1080->1280x720 |
| 1920x1080   | 1920x1080   | Yes       | None                   | None             |
| 2592x1944   | 1440x1080   | No (4:3)  | 2592x1944 @ (0,0)*    | 2592x1944->1440x1080 |

*2592x1944 is already 4:3, same as 1440x1080 — no precrop needed, just scale.

## Implementation Status

- **Star6E**: Implemented. VIF-level precrop via
  `pipeline_common_compute_precrop()` (with `keep_aspect` parameter, v0.7.9),
  `star6e_pipeline_start_vif()`, and `star6e_pipeline_start_vpe()` in
  `star6e_pipeline.c`. Reinit path tracks `state->active_precrop` so a
  `keepAspect` toggle without dimension change still triggers VIF+VPE
  reconfigure.
- **Maruko**: Implemented. SCL-level precrop via `pipeline_common_compute_precrop()`
  applied in `configure_maruko_scl()` (`scl_port.crop`).  Computed against
  the post-binning effective input (sensor capt clamped by `mode.output`),
  so the rect always matches the surface that actually feeds the SCL stage.
  Falls back to zero crop (full source area, downstream stretch) when
  `isp.keepAspect=false`.

## Source Files

- `src/pipeline_common.c` — `pipeline_common_compute_precrop()`
- `src/star6e_pipeline.c` — `select_and_configure_sensor()`,
  `star6e_pipeline_start()`, `star6e_pipeline_reinit()`,
  `star6e_pipeline_start_vif()`, `star6e_pipeline_start_vpe()`
- `include/star6e_pipeline.h` — `Star6ePrecropRect`, `active_precrop`
- `src/maruko_pipeline.c` — `maruko_pipeline_configure_graph()`
  (computes the rect), `configure_maruko_scl()` (writes
  `scl_port.crop`)

## SDK References

- [SigmaStar Pudding SDK — VPE Module](https://wx.comake.online/doc/doc/SigmaStarDocs-Pudding-0120/)
- VIF port struct: `sdk/ssc338q/hal/star/i6_vif.h` (`i6_vif_port.capt`)
- SCL port struct: `sdk/ssc338q/hal/star/i6c_scl.h` (`i6c_scl_port.crop`)
- VPE channel struct: `sdk/ssc338q/hal/star/i6_vpe.h` (`i6_vpe_chn.capt`)
