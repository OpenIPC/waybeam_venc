# HTTP API Contract

## Purpose
- This document is the source of truth for the runtime HTTP API.
- Any added, changed, or removed HTTP endpoint behavior must be reflected here.

## Design Principles
- Keep endpoints lean and focused on direct operational value.
- All API changes are **in-memory only** — never saved to disk. This prevents a
  bad config from bricking the device on reboot.
- Keep JSON payloads simple and descriptive.
- Keep mutability semantics explicit:
  - `live` — applied immediately without pipeline restart.
  - `restart_required` — triggers automatic pipeline reinit (teardown + rebuild).
  - `read_only` — cannot be changed via API.

## Contract Version
- `contract_version`: `0.6.0`
- `status`: `active`

## Governance Rules
- Non-breaking changes: add optional fields, add new endpoints, extend enum values.
- Breaking changes: remove endpoints, rename fields, change required field semantics.
- For every breaking change: increment contract major version, add migration note, update `HISTORY.md`.
- For every non-breaking change: increment contract minor/patch version, update this file.

## Transport And Format
- HTTP/1.0, all methods use `GET` (compatible with BusyBox wget)
- Default port: 80 (configurable via `system.web_port` in config)
- Response content type: `application/json; charset=UTF-8`
- Query parameters: field name is the key, value (if any) follows `=`

## Standard Response Envelope

### Success
```json
{
  "ok": true,
  "data": {}
}
```

### Error
```json
{
  "ok": false,
  "error": {
    "code": "string_code",
    "message": "human readable message"
  }
}
```

## Error Codes
| Code | HTTP Status | Meaning |
|------|-------------|---------|
| `invalid_request` | 400 | Missing or malformed parameters |
| `validation_failed` | 400/409 | Value rejected by field or config validation |
| `not_found` | 404 | Unknown field or route |
| `not_implemented` | 501 | Apply callback not available for this field |
| `internal_error` | 500 | Server-side failure |

## Endpoints

### `GET /api/v1/version`

Return app, backend, schema, and contract version information.

```bash
curl http://<device-ip>/api/v1/version
```

Response `200`:
```json
{
  "ok": true,
  "data": {
    "app_version": "0.1.7",
    "contract_version": "0.1.2",
    "config_schema_version": "1.0.0",
    "backend": "star6e"
  }
}
```

### `GET /api/v1/config`

Return the full active runtime config.

```bash
curl http://<device-ip>/api/v1/config
```

Response `200`:
```json
{
  "ok": true,
  "data": {
    "config": {
      "system": { "webPort": 80, "overclockLevel": 2, "verbose": false },
      "sensor": { "index": -1, "mode": -1, "unlockEnabled": true, "..." : "..." },
      "isp": { "sensorBin": "/etc/sensors/imx415_greg_fpvXVIII-gpt200.bin", "exposure": 9, "legacyAe": false, "aeFps": 15, "gainMax": 0, "awbMode": "auto", "awbCt": 5500 },
      "image": { "mirror": false, "flip": false, "rotate": 0 },
      "video0": { "codec": "h265", "rcMode": "cbr", "fps": 90, "size": "1920x1080", "bitrate": 8192, "gopSize": 1.0, "qpDelta": 0, "sceneThreshold": 0, "sceneHoldoff": 2 },
      "outgoing": { "enabled": true, "server": "udp://192.168.2.20:5600", "streamMode": "rtp", "maxPayloadSize": 1400, "connectedUdp": false },
      "fpv": { "roiEnabled": true, "roiQp": 0, "roiSteps": 2, "roiCenter": 0.25, "noiseLevel": 0 },
      "record": { "enabled": false, "mode": "off", "dir": "/tmp/sdcard", "format": "ts", "maxSeconds": 300, "maxMB": 500 },
      "debug": { "showOsd": false }
    }
  }
}
```

### `GET /api/v1/capabilities`

Return per-field mutability and backend support.

```bash
curl http://<device-ip>/api/v1/capabilities
```

Response `200`:
```json
{
  "ok": true,
  "data": {
    "fields": {
      "video0.bitrate": { "mutability": "live", "supported": true },
      "video0.fps": { "mutability": "live", "supported": true },
      "video0.gop_size": { "mutability": "live", "supported": true },
      "video0.qp_delta": { "mutability": "live", "supported": true },
      "video0.codec": { "mutability": "restart_required", "supported": true },
      "video0.size": { "mutability": "restart_required", "supported": true },
      "video0.scene_threshold": { "mutability": "restart_required", "supported": true },
      "video0.scene_holdoff": { "mutability": "restart_required", "supported": true },
      "system.verbose": { "mutability": "live", "supported": true },
      "isp.exposure": { "mutability": "live", "supported": true },
      "outgoing.enabled": { "mutability": "live", "supported": true },
      "outgoing.server": { "mutability": "live", "supported": true },
      "outgoing.stream_mode": { "mutability": "restart_required", "supported": true },
      "outgoing.connected_udp": { "mutability": "restart_required", "supported": true },
      "fpv.roi_qp": { "mutability": "live", "supported": true }
    }
  }
}
```
(truncated — all fields listed in actual response)

`supported` is backend-specific. On Star6E, `video0.scene_threshold` / `video0.scene_holdoff` report
`supported: true`; on Maruko they report `supported: false` and writes are
rejected with `501 not_implemented`.

### `GET /api/v1/config.json`

Majestic-compatible alias of `/api/v1/config`.

```bash
curl http://<device-ip>/api/v1/config.json
```

### `GET /api/v1/get?<field_name>`

Read a single config field. The field name is the query parameter key (no value needed).

```bash
# Read current bitrate
curl "http://<device-ip>/api/v1/get?video0.bitrate"

# Read current codec
curl "http://<device-ip>/api/v1/get?video0.codec"

# Read current qpDelta
curl "http://<device-ip>/api/v1/get?video0.qp_delta"

# Read a string field
curl "http://<device-ip>/api/v1/get?isp.sensor_bin"
```

Response `200`:
```json
{"ok":true,"data":{"field":"video0.bitrate","value":8192}}
```
```json
{"ok":true,"data":{"field":"video0.codec","value":"h265"}}
```
```json
{"ok":true,"data":{"field":"video0.qp_delta","value":0}}
```
```json
{"ok":true,"data":{"field":"isp.sensor_bin","value":"/etc/sensors/imx415_greg_fpvXVIII-gpt200.bin"}}
```

Error `400` — missing field name:
```json
{"ok":false,"error":{"code":"invalid_request","message":"missing query parameter (field name)"}}
```

Error `404` — unknown field:
```json
{"ok":false,"error":{"code":"not_found","message":"unknown config field"}}
```

Majestic-style camelCase aliases are also accepted for selected fields,
including `fpv.roiQp`, `fpv.roiEnabled`, `fpv.roiSteps`, `fpv.roiCenter`,
`fpv.noiseLevel`, `isp.sensorBin`, `isp.awbMode`, `isp.awbCt`,
`video0.rcMode`, `video0.gopSize`, `video0.qpDelta`,
`video0.sceneThreshold`, `video0.sceneHoldoff`,
`outgoing.maxPayloadSize`,
`outgoing.audioPort`, `system.webPort`, and `system.overclockLevel`.

### `GET /api/v1/set?<field_name>=<value>`

Write a config field. The field name is the query key, the new value follows `=`.

**Live fields** (`mutability: "live"`) are applied immediately without pipeline restart:

```bash
# Change bitrate to 4096 kbps
curl "http://<device-ip>/api/v1/set?video0.bitrate=4096"

# Change FPS
curl "http://<device-ip>/api/v1/set?video0.fps=60"

# Change GOP interval (seconds between keyframes; 0 = all-intra)
curl "http://<device-ip>/api/v1/set?video0.gop_size=0.5"

# Bias relative I-frame QP (Majestic-compatible range: -12..12)
curl "http://<device-ip>/api/v1/set?video0.qp_delta=-4"

# Apply multiple live fields atomically in one request
curl "http://<device-ip>/api/v1/set?video0.bitrate=4096&system.verbose=true"

# Coupled live timing changes can be sent together
curl "http://<device-ip>/api/v1/set?video0.fps=30&video0.gopSize=1.0"
```

When `video0.scene_threshold` is non-zero, the inline scene detector tracks
frame size EMA and requests IDR after scene change spikes settle.

If a `GET /api/v1/set` request contains multiple `key=value` pairs joined by
`&`, every field must be live. Mixed live + restart requests are rejected.
Duplicate fields are also rejected after alias canonicalization, so
`video0.qp_delta` and `video0.qpDelta` cannot appear in the same batch.

Response `200`:
```json
{"ok":true,"data":{"field":"video0.bitrate","value":4096}}
```

Response `200` for multi-set:
```json
{"ok":true,"data":{"applied":[{"field":"video0.bitrate","value":4096},{"field":"system.verbose","value":true}]}}
```

**Restart-required fields** (`mutability: "restart_required"`) trigger an automatic
pipeline reinit (sensor→VIF→VPE→VENC teardown and rebuild):

```bash
# Change resolution (single call, triggers one pipeline reinit)
curl "http://<device-ip>/api/v1/set?video0.size=1280x720"

# Preset shortcuts also work
curl "http://<device-ip>/api/v1/set?video0.size=720p"
curl "http://<device-ip>/api/v1/set?video0.size=1080p"
curl "http://<device-ip>/api/v1/set?video0.size=4MP"

# Enable Star6E scene-change IDR control
curl "http://<device-ip>/api/v1/set?video0.scene_threshold=150"
```

Response `200` (includes `"reinit_pending": true`):
```json
{"ok":true,"data":{"field":"video0.size","value":"1280x720","reinit_pending":true}}
```

Restart/reinit writes stay single-field by design. Even though the main loop
debounces reinit requests, clients should send restart-required changes one at
a time and let each accepted write schedule the pipeline rebuild.

Adaptive control usage notes:
- Keep `video0.scene_threshold=0` for fixed-GOP workflows and drive keyframe
  interval through `video0.gop_size`.
- On the current Star6E IMX335 bench, a practical starting point is:
  `video0.sceneThreshold=150`, `video0.sceneHoldoff=2`.
- Tune threshold first, holdoff second. In practice, threshold changes are
  a safer first response than raising holdoff.

Example Star6E tuning sequence:

```bash
curl "http://<device-ip>/api/v1/set?video0.sceneThreshold=150"
curl "http://<device-ip>/api/v1/set?video0.sceneHoldoff=2"
```

**Validation errors** — some values are rejected before being applied:

```bash
# Attempt to switch Star6E RTP mode to h264
curl "http://<device-ip>/api/v1/set?video0.codec=h264"
```

Error `409`:
```json
{"ok":false,"error":{"code":"validation_failed","message":"star6e RTP mode currently supports h265 only"}}
```

Error `501` — apply callback not available:
```json
{"ok":false,"error":{"code":"not_implemented","message":"apply callback not available"}}
```

Error `400` — multi-set included a restart-required field:
```json
{"ok":false,"error":{"code":"invalid_request","message":"multi-set only supports live fields; restart-required fields must be set one at a time"}}
```

The same camelCase aliases listed above are accepted here for
Majestic-oriented clients.

### `video0.qp_delta`

- Type: signed integer
- Range: `-12..12`
- Mutability: `live`
- Alias: `video0.qpDelta`
- Semantics: adjusts I-frame QP relative to P-frame; negative values lower I-frame QP (higher quality keyframes), positive values raise it.

### `GET /api/v1/fps/config`

Return the configured target FPS from the active runtime config.

```bash
curl http://<device-ip>/api/v1/fps/config
```

Response `200`:
```json
{"ok":true,"data":{"fps":60}}
```

### `GET /api/v1/fps/live`

Return the live/applied FPS reported by the active backend. If a backend does
not expose a distinct live value, this falls back to the configured FPS.

```bash
curl http://<device-ip>/api/v1/fps/live
```

Response `200`:
```json
{"ok":true,"data":{"fps":60}}
```

### Output Enable/Disable

The `outgoing.enabled` field controls whether encoded frames are sent over UDP.

```bash
# Enable output (starts sending, restores FPS, issues IDR)
curl "http://<device-ip>/api/v1/set?outgoing.enabled=true"

# Disable output (stops sending, reduces FPS to 5fps idle)
curl "http://<device-ip>/api/v1/set?outgoing.enabled=false"
```

**Behavior when disabled:**
- FPS is reduced to 5fps (idle rate) to minimize sensor/ISP power draw.
- Encoder keeps running at the reduced rate; frames are encoded and discarded.
- The previous FPS is stored and restored when output is re-enabled.
- An IDR keyframe is issued on re-enable for immediate stream sync.

**Default:** `false` — output must be explicitly enabled. Configure `outgoing.server`
before enabling.

### Live Destination Redirect

The `outgoing.server` field can be changed at runtime to redirect the stream.

```bash
# Redirect stream to a different GCS
curl "http://<device-ip>/api/v1/set?outgoing.server=udp://<receiver-ip>:5600"
```

- Accepted URI schemes:
  - `udp://HOST:PORT` — standard UDP datagram output
  - `unix://NAME` — Linux abstract Unix datagram socket `@NAME`
  - `shm://NAME` — shared-memory RTP ring buffer
- No pipeline restart required.
- An IDR keyframe is issued after the change for stream continuity.
- If `connectedUdp` is enabled, the UDP socket is re-connected to the new destination.
- Live redirects support `udp://` and `unix://`. Live switch to `shm://` is not supported.
- `connectedUdp` applies only to `udp://`.
- `shm://` remains RTP-only. It cannot share audio; use a nonzero `audioPort` for separate UDP audio.
- On Star6E, `audioPort=0` piggybacks on the active video destination for both `udp://` and `unix://`.
- On Star6E, a nonzero `audioPort` keeps audio on a dedicated UDP port. With `unix://` or `shm://` video output, that dedicated audio port is sent to `127.0.0.1:<audioPort>`.

### Stream Mode and Send Feedback

```bash
# These require pipeline restart
curl "http://<device-ip>/api/v1/set?outgoing.stream_mode=compact"
curl "http://<device-ip>/api/v1/set?outgoing.connected_udp=true"
```

- `outgoing.stream_mode`: `"rtp"` (default) or `"compact"`. Determines packetization format.
- `outgoing.max_payload_size`: Maximum RTP payload size in bytes. Default `1400`.
  Applies to both RTP (FU fragmentation threshold) and compact modes. Values above 1400
  are supported for jumbo-frame networks.
  Default `0` (disabled — uses fixed `maxPayloadSize`). When non-zero, the adaptive
  algorithm adjusts the effective payload size to hit this target, clamped to
  `[1000, maxPayloadSize]`.
- `outgoing.connected_udp`: When `true`, calls `connect()` on the UDP socket so the kernel
  returns ICMP port-unreachable errors via `sendmsg()`. Useful for detecting that a receiver
  is down. Default `false` (fire-and-forget).

### Live FPS Control — Behavior Details

Setting `video0.fps` via the API applies **hardware-level frame decimation** within the
active sensor mode. The sensor continues running at its native `maxFps`; the MI_SYS bind
layer between VPE and VENC drops frames to match the requested rate.

```bash
# On a 90fps sensor mode: set output to 30fps (sensor stays at 90, VENC receives 30)
curl "http://<device-ip>/api/v1/set?video0.fps=30"

# Set output to 60fps
curl "http://<device-ip>/api/v1/set?video0.fps=60"

# Restore full sensor rate
curl "http://<device-ip>/api/v1/set?video0.fps=90"
```

**Clamping:** If the requested FPS exceeds the current sensor mode's `maxFps`, the value
is silently clamped to the mode maximum. For example, requesting 120fps on a 90fps mode
sets the output to 90fps. To access a higher sensor mode, edit `/etc/venc.json` and
restart the process.

**What happens under the hood:**
1. VPE→VENC bind is torn down and re-established with `src_fps:dst_fps` ratio
2. VENC rate control `fpsNum` is updated for correct bitrate allocation
3. No pipeline restart — latency is sub-second

**Mode switching limitation:** Changing sensor modes (e.g. 90fps→120fps) requires a full
process restart. The SigmaStar kernel driver does not reliably reinitialize the MIPI PHY
when switching modes in-process. Use `/api/v1/restart` (reloads `/etc/venc.json`) or
restart the venc process to change sensor modes.

### `GET /api/v1/restart`

Trigger a full pipeline reinit: reload config from disk (`/etc/venc.json`) and rebuild
the pipeline. Equivalent to sending `SIGHUP` to the process.

```bash
curl http://<device-ip>/api/v1/restart
```

Response `200`:
```json
{"ok":true,"data":{"reinit":true}}
```

### `GET /api/v1/ae`

Return live AE diagnostics from the active backend.

```bash
curl http://<device-ip>/api/v1/ae
```

Response `200`:
```json
{
  "ok": true,
  "data": {
    "sensor_plane": { "ret": 0, "pad": 0, "shutter_us": 1112, "sensor_gain_x1024": 10240, "comp_gain_x1024": 1024 },
    "exposure_limit": { "ret": 0, "min_shutter_us": 150, "max_shutter_us": 10000, "min_sensor_gain": 1024, "max_sensor_gain": 30000, "min_isp_gain": 1024, "max_isp_gain": 1024 },
    "exposure_info": { "ret": 0, "stable": true, "reach_boundary": false, "long_us": 9999, "long_sensor_gain_x1024": 1673, "long_isp_gain_x1024": 1024, "luma_y": 236, "avg_y": 247 },
    "state": { "ret": 0, "raw": 0, "name": "normal" },
    "expo_mode": { "ret": 0, "raw": 0, "name": "auto" },
    "metrics": { "exposure_us": 9999, "sensor_gain_x1024": 1673, "isp_gain_x1024": 1024, "fps": 90 }
  }
}
```

Error `501`:
```json
{"ok":false,"error":{"code":"not_implemented","message":"AE query not available"}}
```

### `GET /api/v1/awb`

Return live AWB diagnostics from the active backend.

```bash
curl http://<device-ip>/api/v1/awb
```

Error `501`:
```json
{"ok":false,"error":{"code":"not_implemented","message":"AWB query not available"}}
```

### `GET /api/v1/iq`

Query all ISP IQ parameter values. Always available on Star6E backend.

```bash
curl http://<device-ip>/api/v1/iq
```

Response `200`:
```json
{
  "ok": true,
  "data": {
    "lightness": {"ret": 0, "enabled": true, "op_type": "auto", "value": 50},
    "contrast": {"ret": 0, "enabled": true, "op_type": "manual", "value": 70},
    "color_to_gray": {"ret": 0, "value": false},
    "demosaic": {"ret": 0, "enabled": true, "value": 45}
  }
}
```

Each parameter reports:
- `ret`: MI_ISP return code (0 = success)
- `enabled`: bEnable flag
- `op_type`: `"auto"` or `"manual"` (omitted for bool-only and manual-only params)
- `value`: current primary value (backward-compat scalar)
- `fields`: (multi-field params only) object with all named sub-fields and arrays
- `available`: `false` if the dlsym symbol was not found

Multi-field example (colortrans):
```json
"colortrans": {
  "ret": 0, "enabled": true, "value": 200,
  "fields": {
    "y_ofst": 200, "u_ofst": 0, "v_ofst": 0,
    "matrix": [23, 45, 9, 1005, 987, 56, 56, 977, 1015]
  }
}
```

Error `501` if backend doesn't support IQ (Maruko):
```json
{"ok":false,"error":{"code":"not_implemented","message":"IQ query not available"}}
```

### `GET /api/v1/iq/set?<param>=<value>`

Set a single IQ parameter. The parameter is switched to manual mode (for
auto/manual params) and the value is written to the primary manual field.

Supports dot-notation for multi-field params, comma-separated arrays, and
enable/disable toggling via the `.enabled` virtual field:

```bash
# Simple scalar
curl "http://<device-ip>/api/v1/iq/set?contrast=70"

# Dot-notation for sub-field
curl "http://<device-ip>/api/v1/iq/set?colortrans.y_ofst=200"

# Array value (comma-separated)
curl "http://<device-ip>/api/v1/iq/set?colortrans.matrix=23,45,9,1005,987,56,56,977,1015"

# Enable/disable toggle (non-bool params only)
curl "http://<device-ip>/api/v1/iq/set?colortrans.enabled=0"
curl "http://<device-ip>/api/v1/iq/set?crosstalk.enabled=1"

# Bool toggle
curl "http://<device-ip>/api/v1/iq/set?color_to_gray=1"
```

Response `200`:
```json
{"ok":true,"data":{"param":"colortrans.y_ofst","value":200}}
{"ok":true,"data":{"param":"colortrans.matrix","value":[23,45,9,1005,987,56,56,977,1015]}}
```

### `POST /api/v1/iq/import`

Import IQ parameters from a JSON body (output of `GET /api/v1/iq`).
Partial imports are supported — only parameters present in the JSON are applied.
The `enabled` field is respected during import — parameters with `"enabled":false`
will be disabled on the ISP.

```bash
# Full import from exported file
curl -X POST -H "Content-Type: application/json" \
  -d @my_tuning.json http://<device-ip>/api/v1/iq/import

# Partial import — only specific params
echo '{"lightness":{"value":75},"demosaic":{"fields":{"dir_thrd":30}}}' | \
  curl -X POST -H "Content-Type: application/json" -d @- http://<device-ip>/api/v1/iq/import
```

Response `200`:
```json
{"ok":true,"data":{"imported":true}}
```

### `GET /` (Web Dashboard)

Serves a self-contained HTML dashboard (gzip-compressed, ~14KB). The dashboard
provides Settings, API Reference, and Image Quality tabs. All modern browsers
decompress the gzip response automatically.

**Available parameters (62 total, Star6E):**

| Parameter | Type | Range | Description |
|-----------|------|-------|-------------|
| `lightness` | u32 | 0-100 | Lightness level |
| `contrast` | u32 | 0-100 | Contrast level |
| `brightness` | u32 | 0-100 | Brightness level |
| `saturation` | u8 | 0-127 | Color saturation (32=1X) |
| `sharpness` | u8 | 0-255 | Overshoot gain |
| `hsv` | u8 | 0-64 | Hue LUT first entry |
| `nr3d` | u8 | 0-255 | 3D NR motion threshold |
| `nr3d_ex` | u32 | 0-1 | 3D NR extended AR enable |
| `nr_despike` | u8 | 0-15 | De-spike blend ratio |
| `nr_luma` | u8 | 0-255 | Luma NR strength |
| `nr_luma_adv` | u32 | 0-1 | Advanced luma NR debug enable |
| `nr_chroma` | u8 | 0-127 | Chroma NR match ratio |
| `nr_chroma_adv` | u8 | 0-255 | Advanced chroma NR strength |
| `false_color` | u8 | 0-255 | False color frequency threshold |
| `crosstalk` | u8 | 0-31 | Cross-talk correction strength |
| `demosaic` | u8 | 0-63 | Demosaic direction threshold |
| `obc` | u16 | 0-255 | Optical black correction R value |
| `dynamic_dp` | u8 | 0-1 | Hot pixel detection enable |
| `dp_cluster` | u32 | 0-1 | Cluster dead pixel edge mode |
| `r2y` | u16 | 0-1023 | R2Y matrix first coefficient |
| `colortrans` | u16 | 0-2047 | Color transform Y offset |
| `rgb_matrix` | u16 | 0-8191 | CCM first coefficient |
| `wdr` | u8 | 0-4 | WDR box number |
| `wdr_curve_adv` | u16 | 0-16384 | WDR curve slope |
| `pfc` | u8 | 0-255 | Phase focus correction strength |
| `pfc_ex` | u32 | 0-1 | Extended PFC debug enable |
| `hdr` | u8 | 0-1 | HDR NR enable |
| `hdr_ex` | u16 | 0-65535 | HDR sensor exposure ratio |
| `shp_ex` | u32 | 0-1 | Extended sharpness debug enable |
| `rgbir` | u8 | 0-7 | RGBIR position type |
| `iq_mode` | u32 | 0-1 | IQ mode (0=day, 1=night) |
| `lsc` | u16 | 0-65535 | Lens shading center X |
| `lsc_ctrl` | u8 | 0-255 | LSC R ratio by CCT |
| `alsc` | u8 | 0-255 | Adaptive LSC grid X |
| `alsc_ctrl` | u8 | 0-255 | ALSC R ratio by CCT |
| `obc_p1` | u16 | 0-255 | OBC phase 1 R value |
| `stitch_lpf` | u16 | 0-256 | Stitch LPF first coefficient |
| `rgb_gamma` | bool | 0/1 | RGB gamma enable |
| `yuv_gamma` | bool | 0/1 | YUV gamma enable |
| `wdr_curve_full` | bool | 0/1 | WDR full curve enable |
| `dummy` | bool | 0/1 | Dummy tuning enable |
| `dummy_ex` | bool | 0/1 | Extended dummy enable |
| `defog` | bool | 0/1 | Defogging enable |
| `color_to_gray` | bool | 0/1 | Grayscale mode |
| `nr3d_p1` | bool | 0/1 | 3D NR phase 1 enable |
| `fpn` | bool | 0/1 | Fixed pattern noise enable |

**Hardware test results (SSC30KQ, imx335):**
- 45/46 symbols resolved (`stitch_lpf` not present)
- 40/45 params roundtrip correctly (set → query reads same value)
- 3 offset mismatches: `nr_despike`, `pfc`, `hdr` (set succeeds but readback differs — struct padding)
- 2 ISP-rejected: `nr3d_p1`, `fpn` (set succeeds but ISP ignores on this sensor)

### `GET /metrics/isp`

Return a compact Prometheus-style ISP metrics snapshot.

```bash
curl http://<device-ip>/metrics/isp
```

Response `200`:
```text
# HELP isp_again Analog Gain
# TYPE isp_again gauge
isp_again 1673
# HELP isp_dgain Digital Gain
# TYPE isp_dgain gauge
isp_dgain 1024
# HELP isp_exposure Exposure
# TYPE isp_exposure gauge
isp_exposure 9
# HELP isp_fps Sensor fps
# TYPE isp_fps gauge
isp_fps 90
```

### `GET /api/v1/record/start`

Start SD card recording. Optional `?dir=/path` query parameter overrides the
default recording directory (from config `record.dir`, default `/media`).

```bash
# Start recording with default dir
wget -q -O- "http://<device-ip>/api/v1/record/start"

# Start with custom directory
wget -q -O- "http://<device-ip>/api/v1/record/start?dir=/media/clips"
```

Response `200`:
```json
{"ok":true,"data":{"action":"start","dir":"/media"}}
```

Recording format is determined by `record.format` config: `"ts"` (default, MPEG-TS
with audio) or `"hevc"` (raw HEVC NAL stream). File rotation is controlled by
`record.maxSeconds` and `record.maxMB` config fields.

### `GET /api/v1/record/stop`

Stop SD card recording.

```bash
wget -q -O- "http://<device-ip>/api/v1/record/stop"
```

Response `200`:
```json
{"ok":true,"data":{"action":"stop"}}
```

### `GET /api/v1/record/status`

Query recording status.

```bash
wget -q -O- "http://<device-ip>/api/v1/record/status"
```

Response `200`:
```json
{
  "ok": true,
  "data": {
    "active": true,
    "format": "ts",
    "path": "/media/rec_01h23m45s_abcd.ts",
    "frames": 1500,
    "bytes": 12345678,
    "segments": 1,
    "stop_reason": "none"
  }
}
```

`stop_reason` values: `"none"` (currently recording), `"manual"`, `"disk_full"`,
`"write_error"`.

### `GET /request/idr`

Request an IDR (keyframe) from the encoder.

```bash
curl http://<device-ip>/request/idr
```

Response `200`:
```json
{"ok":true,"data":{"idr":true}}
```

If `outgoing.sidecar_port` is enabled at the same time, Star6E also appends
the scene-detector telemetry trailer to sidecar `FRAME` packets. That
is the intended external interface for per-frame size/type/complexity observations.

### `GET /api/v1/dual/status`

Query the secondary VENC channel status. Only available when dual or dual-stream
mode is active.

```bash
wget -q -O- "http://<device-ip>/api/v1/dual/status"
```

Response `200`:
```json
{"ok":true,"data":{"active":true,"channel":1,"bitrate":20000,"fps":120,"gop":240}}
```

Error `404` — dual VENC not active:
```json
{"ok":false,"error":{"code":"not_active","message":"Dual VENC channel is not active"}}
```

### `GET /api/v1/dual/set?<param>=<value>`

Live-change secondary VENC channel parameters. Supported parameters:

| Parameter | Type | Description |
|-----------|------|-------------|
| `bitrate` | uint | Bitrate in kbps (applied immediately via MI_VENC, IDR issued) |
| `gop` | double | GOP interval in seconds (converted to frames using ch1 fps) |

```bash
# Change ch1 bitrate to 10 Mbps
wget -q -O- "http://<device-ip>/api/v1/dual/set?bitrate=10000"

# Change ch1 GOP to 1 second (120 frames at 120fps)
wget -q -O- "http://<device-ip>/api/v1/dual/set?gop=1.0"
```

Response `200`:
```json
{"ok":true,"data":{"field":"bitrate","value":10000}}
{"ok":true,"data":{"field":"gop","value":1.00,"frames":120}}
```

Error `400` — missing or invalid parameter:
```json
{"ok":false,"error":{"code":"missing_param","message":"Usage: /api/v1/dual/set?bitrate=N or ?gop=N"}}
```

Error `404` — dual VENC not active.

### `GET /api/v1/dual/idr`

Request an IDR keyframe on the secondary VENC channel.

```bash
wget -q -O- "http://<device-ip>/api/v1/dual/idr"
```

Response `200`:
```json
{"ok":true,"data":{"idr":true}}
```

Error `404` — dual VENC not active.

## SIGHUP Pipeline Reinit

In addition to the `/api/v1/restart` endpoint, the pipeline can be reinited by sending
`SIGHUP` to the venc process:

```bash
# From the device shell
killall -HUP venc

# Remotely via SSH
ssh root@<device-ip> "killall -HUP venc"
```

Behavior:
- Tears down the full pipeline (VENC→VPE→VIF→sensor, unbinds, closes socket)
- Reloads `/etc/venc.json` from disk
- Rebuilds the pipeline with the new config
- The HTTP server survives reinit cycles (no port re-bind)
- Stress-tested: 10+ consecutive SIGHUPs without failure

## Important Safety Notes

1. **API changes are in-memory only.** No API call writes to `/etc/venc.json`.
   A reboot always restores the on-disk config. To persist changes, edit
   `/etc/venc.json` directly and then `SIGHUP` or call `/api/v1/restart`.

2. **Codec restriction is backend-specific.** Star6E RTP mode requires
   `video0.codec=h265`, so attempting to set `video0.codec=h264` there returns
   a `409` error. Maruko accepts both `h264` and `h265`.

3. **BusyBox compatibility.** All endpoints use `GET` method so they work with
   BusyBox `wget` (which only supports GET):
   ```bash
   # On-device with BusyBox wget
   wget -q -O- "http://127.0.0.1/api/v1/get?video0.fps"
   wget -q -O- "http://127.0.0.1/api/v1/set?video0.bitrate=4096"
   ```

## Backend Compatibility Notes
- Star6E is the reference behavior for API-touching features.
- Maruko may return `not_implemented` for specific apply paths until parity work is complete.
- `GET` endpoints must remain consistent across backends.

## Change Log (Contract)
- `0.5.0`:
  - Added `GET /api/v1/iq` — query all ISP IQ parameter values (46 params).
  - Added `GET /api/v1/iq/set?param=value` — set individual IQ parameters live.
  - Always enabled on Star6E (no config toggle needed — zero runtime overhead).
  - Params cover image quality, noise reduction, corrections, dynamic range,
    lens calibration, LUT enables, and ISP mode controls.
  - Star6E: 45/46 symbols resolved, Maruko returns 501.
- `0.4.0`:
  - Added `GET /api/v1/dual/status` — query secondary VENC channel state.
  - Added `GET /api/v1/dual/set?bitrate=N` — live ch1 bitrate change.
  - Added `GET /api/v1/dual/set?gop=N` — live ch1 GOP change (in seconds).
  - Added `GET /api/v1/dual/idr` — request IDR on secondary channel.
  - All dual endpoints return 404 when dual VENC is not active.
  - Config `record` section expanded: `mode` ("off"/"mirror"/"dual"/"dual-stream"),
    `bitrate`, `fps`, `gopSize` for ch1 config, `server` for dual-stream.
- `0.3.0`:
  - Added `GET /api/v1/record/start` — start SD card recording (optional `?dir=`).
  - Added `GET /api/v1/record/stop` — stop SD card recording.
  - Added `GET /api/v1/record/status` — query recording status (active, format, bytes, segments, stop_reason).
  - Config `record` section expanded: `format` ("hevc" or "ts"), `maxSeconds`, `maxMB`.
  - MPEG-TS muxer: HEVC video + PCM audio in power-loss safe container.
  - File rotation at IDR boundaries by time (default 300s) or size (default 500MB).
  - RTP streaming and recording operate concurrently.
- `0.2.3`:
  - Added `GET /api/v1/ae` for live AE diagnostics.
  - Added `GET /api/v1/awb` for live AWB diagnostics.
  - Added `GET /metrics/isp` for compact ISP metrics export.
  - Added Majestic-compatible `GET /api/v1/config.json` alias.
  - Added `GET /api/v1/fps/config` for configured FPS queries.
  - Added `GET /api/v1/fps/live` for live/applied FPS queries.
  - Added support for selected Majestic-style camelCase field aliases on
    `GET /api/v1/get` and `GET /api/v1/set`.
- `0.2.1`:
  - `outgoing.max_payload_size` now applies to RTP mode (was only used by compact mode).
    Default 850. Set to 0 to disable adaptive sizing.
- `0.2.0`:
  - Added `outgoing.enabled` (MUT_LIVE): enable/disable UDP output with FPS idle.
  - Added `outgoing.server` changed from MUT_RESTART to MUT_LIVE: live destination redirect.
  - Added `outgoing.streamMode` (MUT_RESTART): explicit stream mode selection.
  - Added `outgoing.connectedUdp` (MUT_RESTART): connected UDP error reporting.
  - IDR keyframe issued on output enable, destination change, and bitrate change.
  - Server URIs now accept `udp://`, `unix://`, and `shm://`.
- `0.1.3`:
  - Documented live FPS control behavior (hardware bind decimation, clamping, mode switching limitation).
  - `video0.fps` set via API now uses MI_SYS_BindChnPort2 rebind instead of /proc write.
- `0.1.2`:
  - Updated to reflect actual implemented API (was draft, now active).
  - All endpoints use GET method (BusyBox wget compatibility).
  - Documented query parameter format: `?field_name` for get, `?field_name=value` for set.
  - Added `/api/v1/restart` endpoint (replaces planned `POST /api/v1/actions/restart`).
  - Added `/request/idr` endpoint.
  - Removed unimplemented `PUT /api/v1/config` and `PATCH /api/v1/config` (future work).
  - Added curl examples for all endpoints.
  - Added SIGHUP reinit documentation.
  - Added safety notes (in-memory only, codec restriction).
- `0.1.1`:
  - Updated examples to use `video.capture_resolution` restart semantics.
- `0.1.0`:
  - Initial draft contract and endpoint definitions.
