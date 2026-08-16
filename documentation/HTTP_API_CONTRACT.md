# HTTP API Contract

## Purpose
- This document is the source of truth for the runtime HTTP API.
- Any added, changed, or removed HTTP endpoint behavior must be reflected here.

## Design Principles
- Keep endpoints lean and focused on direct operational value.
- Accepted `/api/v1/set` and `/api/v1/defaults` changes are persisted to the
  registered config path before the response returns. Manual `/api/v1/restart`
  still reloads exactly what is already on disk. `/api/v1/live/set` is the
  deliberate exception: it applies live fields to the running config without
  touching disk (for high-cadence automated writers).
- Keep JSON payloads simple and descriptive.
- Keep mutability semantics explicit:
  - `live` — applied immediately without pipeline restart.
  - `restart_required` — triggers automatic pipeline reinit (teardown + rebuild).
  - `read_only` — cannot be changed via API.

## Contract Version
- `contract_version`: `0.18.3`
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
| `record_active` | 409 | Action blocked while recording is in progress |
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
    "contract_version": "0.12.0",
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
      "sensor": { "index": -1, "mode": -1 },
      "isp": { "sensorBin": "/etc/sensors/imx415_greg_fpvXVIII-gpt200.bin", "aeEngine": "sdk", "aeFps": 15, "gainMax": 0, "awbMode": "auto", "awbCt": 5500, "keepAspect": true },
      "image": { "mirror": false, "flip": false, "rotate": 0 },
      "video0": { "rcMode": "cbr", "fps": 90, "size": "auto", "bitrate": 8192, "gopSize": 1.0, "qpDelta": 0, "sceneThreshold": 0, "sceneHoldoff": 2, "resilience": "off", "zoomX": 0.5, "zoomY": 0.5, "framing": "off" },
      "outgoing": { "enabled": true, "server": "udp://192.168.2.20:5600", "streamMode": "rtp", "maxPayloadSize": 1400, "connectedUdp": false, "allowUnixEncoderStall": false },
      "fpv": { "roiEnabled": true, "roiQp": 0, "roiSteps": 2, "roiCenter": 0.25, "noiseLevel": 0 },
      "record": { "enabled": false, "mode": "off", "dir": "/tmp/sdcard", "format": "ts", "maxSeconds": 300, "maxMB": 500 },
      "debug": { "showOsd": false }
    },
    "runtime": {
      "active_precrop": { "x": 0, "y": 240, "w": 2560, "h": 1440 },
      "vpe_taps": { "port0": ["main", "jpeg"], "port1": "detect" }
    }
  }
}
```

The `runtime` block is read-only and reports pipeline state that is not
part of the editable config:

- `active_precrop` — VIF crop rectangle currently programmed (includes
  any sensor overscan offsets or SCL crop origin). Present whenever a
  Star6E or Maruko pipeline has been started; absent before pipeline start
  or after pipeline stop.
- `vpe_taps` — VPE scaler-output ownership (Star6E only; absent on Maruko
  and before pipeline start). `port0` is the main SCL output, a 1:N-shareable
  buffer listing its consumers (`main` — the H.265 encoder, always present;
  plus `jpeg` when the snapshot channel is up and `record` when a dual/record
  channel is bound — these bind alongside on the same buffer, not a second
  scaler). `port1` is the **single** second scaler output: a string naming its
  sole owner (`"stab"` or `"detect"`), or `null` when free. The arbiter refuses
  a second `port1` claim, so `stab` and `detect` are mutually exclusive on the
  hardware; `stab-fill` rides `port0` only (no `port1` tap) but stays mutually
  exclusive with `detect` by resource policy.

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
      "video0.size": { "mutability": "restart_required", "supported": true },
      "video0.scene_threshold": { "mutability": "restart_required", "supported": true },
      "video0.scene_holdoff": { "mutability": "restart_required", "supported": true },
      "video0.resilience": { "mutability": "restart_required", "supported": true },
      "video0.zoom_x": { "mutability": "live", "supported": true },
      "video0.zoom_y": { "mutability": "live", "supported": true },
      "video0.framing": { "mutability": "restart_required", "supported": true },
      "system.verbose": { "mutability": "live", "supported": true },
      "outgoing.enabled": { "mutability": "live", "supported": true },
      "outgoing.server": { "mutability": "live", "supported": true },
      "outgoing.stream_mode": { "mutability": "restart_required", "supported": true },
      "outgoing.connected_udp": { "mutability": "restart_required", "supported": true },
      "outgoing.allow_unix_encoder_stall": { "mutability": "restart_required", "supported": true },
      "discovery.enabled": { "mutability": "restart_required", "supported": true },
      "discovery.service_type": { "mutability": "restart_required", "supported": true },
      "discovery.name": { "mutability": "restart_required", "supported": true },
      "discovery.bare_alias": { "mutability": "restart_required", "supported": true },
      "fpv.roi_qp": { "mutability": "live", "supported": true },
      "video0.stab_crop_pct": {
        "mutability": "restart_required", "supported": true,
        "ui": {
          "group": "Stabilization", "label": "Stab crop %", "control": "number",
          "min": 60, "max": 100, "step": 1, "tooltip": "Kept-frame percentage ..."
        }
      },
      "video0.pause_stab": {
        "mutability": "live", "supported": true,
        "ui": {
          "group": "Stabilization", "label": "Pause stab", "control": "toggle",
          "tooltip": "Live pause for framing=stab and stab-fill ..."
        }
      }
    }
  }
}
```
(truncated — all fields listed in actual response)

`supported` is backend-specific. Current Star6E and Maruko builds both expose
scene detection, intra refresh, and digital zoom fields.

A field MAY carry an optional `ui` object (data-driven field schema): when
present the dashboard renders a control for it generically — no `dashboard.html`
edit or webui-blob rebuild is needed to surface a new module field.  Keys:
`group` (collapsible section title), `label`, `control`
(`toggle`|`number`|`select`|`text`), `min`/`max`/`step` (for `number`),
`options` (array, for `select`), `tooltip`.  Fields without `ui` use the
dashboard's static schema.  The entire **Stabilization** section is data-driven:
the four persisted `video0.stab_*` knobs (`stab_crop_pct`, `stab_kalman_q`,
`stab_kalman_r`, `stab_recenter_speed`) plus the runtime-only `video0.pause_stab`
(the live stab pause — not in `/api/v1/config`) are all surfaced this way.  So
is the **Snapshot** section (`snapshot.enabled`, `snapshot.quality`,
`snapshot.width`, `snapshot.height`) — the switch for both snapshot endpoints,
which was API-only before.

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

# Read current qpDelta
curl "http://<device-ip>/api/v1/get?video0.qp_delta"

# Read current resilience preset
curl "http://<device-ip>/api/v1/get?video0.resilience"

# Read a string field
curl "http://<device-ip>/api/v1/get?isp.sensor_bin"
```

Response `200`:
```json
{"ok":true,"data":{"field":"video0.bitrate","value":8192}}
```
```json
{"ok":true,"data":{"field":"video0.resilience","value":"off"}}
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
`isp.keepAspect`, `isp.shutterRule180`,
`video0.rcMode`, `video0.gopSize`, `video0.qpDelta`,
`video0.sceneThreshold`, `video0.sceneHoldoff`,
`video0.intraRefreshMode`, `video0.intraRefreshLines`,
`video0.intraRefreshQp`, `video0.zoomX`, `video0.zoomY`, `video0.framing`,
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

# Swap ISP tuning bin (empty = auto-detect /etc/sensors/<sensor>.bin)
curl "http://<device-ip>/api/v1/set?isp.sensorBin=/etc/sensors/imx415_fpv.bin"

# Change GOP interval (seconds between keyframes; 0 = all-intra)
curl "http://<device-ip>/api/v1/set?video0.gop_size=0.5"

# Bias relative I-frame QP (Majestic-compatible range: -12..12)
curl "http://<device-ip>/api/v1/set?video0.qp_delta=-4"

# Pan within the active digital zoom crop
curl "http://<device-ip>/api/v1/set?video0.zoomX=0.25&video0.zoomY=0.75"

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

# Use sensor native resolution (default — no downscaling)
curl "http://<device-ip>/api/v1/set?video0.size=auto"

# Preset shortcuts also work
curl "http://<device-ip>/api/v1/set?video0.size=720p"
curl "http://<device-ip>/api/v1/set?video0.size=1080p"

# Enable scene-change IDR control
curl "http://<device-ip>/api/v1/set?video0.scene_threshold=150"

# Enable 2x digital zoom (encoded resolution becomes half width/height)
curl "http://<device-ip>/api/v1/set?video0.framing=zoom-2x"
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
# Attempt to set the retired video0.codec field
curl "http://<device-ip>/api/v1/set?video0.codec=h264"
```

Error `404`:
```json
{"ok":false,"error":{"code":"not_found","message":"unknown config field"}}
```

Video codec is hardcoded H.265; the field was retired with the
resilience-preset consolidation (see HISTORY 0.10.12).

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

### `GET /api/v1/live/set?<field_name>=<value>`

`/api/v1/set`'s field surface, applied to the **running config only — no
write to `/etc/waybeam.json`**. Built for high-cadence automated writers
(waybeam-link adaptive bitrate/caps/fps actuation), where persist-on-set
would wear flash and boot into the last adaptive transient.

```bash
# Volatile bitrate change: applied live, gone after restart
curl "http://<device-ip>/api/v1/live/set?video0.bitrate=4096"

# Multi-set works identically (all fields must be live)
curl "http://<device-ip>/api/v1/live/set?video0.maxIBytes=60000&video0.maxPBytes=12000"
```

Semantics:
- **Live fields only.** Restart-required fields are rejected with `400`
  (`"restart-class field requires persistence; use /api/v1/set"`) — a
  pipeline reinit reloads from disk, which would silently discard a volatile
  value.
- Responses (success and error) are byte-identical in shape to
  `/api/v1/set`.
- A later persisting `/api/v1/set` or `/api/v1/defaults` snapshots the
  **whole running config, earlier volatile changes included** — one config
  struct, by design. Deployments that must keep a field volatile should
  route all writers of that field through `/live/set` (waybeam-link's
  single-bitrate-authority rule).
- Detection: builds without this endpoint answer `404 no matching route`,
  so clients can probe once and fall back to `/api/v1/set`.

### `video0.qp_delta`

- Type: signed integer
- Range: `-12..12`
- Mutability: `live`
- Alias: `video0.qpDelta`
- Semantics: adjusts I-frame QP relative to P-frame; negative values lower I-frame QP (higher quality keyframes), positive values raise it.

### `video0.framing`, `video0.zoom_x`, `video0.zoom_y`, `video0.stab_crop_pct`, `video0.stab_kalman_q`, `video0.stab_kalman_r`, `video0.stab_recenter_speed`

- `video0.framing`: string preset — the single knob for what the VPE crop does.
  - Values: `off` | `stab` (image stabilization, crop+shrink, Star6E only) |
    `stab-fill` (image stabilization, floating image on a black border, Star6E
    only; encode stays full-res, `stabCropPct` sets the shift/border budget) |
    `zoom-1.25x` | `zoom-1.50x` | `zoom-1.75x` | `zoom-2x` | `zoom-3x` |
    `zoom-4x` (digital zoom, both backends).
  - Mutability: `restart_required` (changes encoded resolution / pipeline).
- `video0.pauseStab`: bool, live (`stab` **and** `stab-fill`) — glide the
  stabilized window (`stab`: HW crop) / floating image (`stab-fill`) back to
  centre via a software ramp, no rebind.  Runtime-only: not persisted, always
  boots `false`.  No effect under `framing=off` or zoom.
- `video0.stabCropPct`: the stab crop / shift budget; clamped to **[60, 100]**
  for a stab preset (a smaller value is rejected by the API and floored on
  load).
  - The preset expands internally into the zoom crop fraction (zoom presets) or
    the stabilization crop/recenter (`stab`); the two are mutually exclusive.
    There is no settable continuous `zoom_pct` — use a zoom preset.
- `video0.stab_crop_pct`, `video0.stab_kalman_q`, `video0.stab_kalman_r`,
  `video0.stab_recenter_speed`: mutability `restart_required`. Aliases
  `video0.stabCropPct`, `video0.stabKalmanQ`, `video0.stabKalmanR`,
  `video0.stabRecenterSpeed`. Tuning for the shared Kalman stabilization control
  law — read *after* preset expansion so an explicit value wins, while a plain
  `framing=stab`/`stab-fill` keeps the preset defaults. Re-selecting the preset
  resets them, so set framing first. Inert under `off`/`zoom-*`. **Both presets
  use the same law, so identical values give identical behaviour.**
  - `stab_crop_pct`: uint. `0` = preset default (80); else `60..100` kept-frame %
    (`stab`) / shift+border budget (`stab-fill`). Clamped to **[60, 100]** for an
    active stab preset (smaller is rejected by the API and floored on load).
    Smaller % = larger dead border = more motion headroom, tighter/more-bordered
    frame.
  - `stab_kalman_q`: double, `0.001..1.0` (default 0.03). Process noise / **pan
    response** — higher = the view follows slow pans sooner (weaker hold); lower
    = holds tighter and more locked. The estimate eases the offset back to centre
    on its own (no separate recenter).
  - `stab_kalman_r`: double, `0.1..50.0` (default 2.0). Measurement noise /
    **smoothness** — higher = smoother but laggier; lower = snappier, more jitter
    passes through. Primary feel knob.
  - `stab_recenter_speed`: uint, `0..3600`. Only the `pauseStab` glide-home rate
    (`0` = default ramp); inert during normal stabilization.
- `video0.zoom_x`, `video0.zoom_y`: double, `0.0..1.0`, mutability `live`.
  Aliases `video0.zoomX`, `video0.zoomY`.
- Semantics: digital zoom uses a 1:1 crop — the crop window and encoded output
  resolution shrink together (1920×1080 → `zoom-2x` 960×528, `zoom-3x` 640×352,
  `zoom-4x` 480×256); no SCL upscale, no extra output bandwidth, so the deep
  3×/4× crops are not bound by the SCL ~2× upscale ceiling. `zoom_x`/`zoom_y`
  move the crop center
  live inside the active aspect-ratio-corrected source surface. Under `stab`
  the crop is always centered (`zoom_x`/`zoom_y` are ignored).

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
  - `shm://NAME` — shared-memory RTP-packet ring buffer
  - `frame-shm://NAME` — shared-memory **whole-frame** ring buffer (Annex-B
    frames, no RTP); for a same-host FEC consumer. Wire format:
    `protocols/frame-shm.md` in the coordination repo.
- No pipeline restart required.
- An IDR keyframe is issued after the change for stream continuity.
- If `connectedUdp` is enabled, the UDP socket is re-connected to the new destination.
- Live redirects support `udp://` and `unix://`. Live switch to/from `shm://` or `frame-shm://` is not supported (restart required).
- `connectedUdp` applies only to `udp://`.
- `shm://` and `frame-shm://` are video-only. They cannot share audio; use a nonzero `audioPort` for separate UDP audio.
- On Star6E, `audioPort=0` piggybacks on the active video destination for both `udp://` and `unix://`.
- On Star6E, a nonzero `audioPort` keeps audio on a dedicated UDP port. With `unix://`, `shm://`, or `frame-shm://` video output, that dedicated audio port is sent to `127.0.0.1:<audioPort>`.
- `frame-shm://` publishes whole Annex-B frames into a 16-slot × 512 KB SPSC ring; each slot is prefixed with an 8-byte `VencFrameMeta` (`pts`, `codec`, `flags`; `flags` bit 0 = IDR). On a full ring the encoder drops the frame and keeps running (never blocks). `outgoing.maxPayloadSize` does not apply (no packetization). `GET /api/v1/transport/status` reports `"transport":"frame-shm"` with `framesSent`/`fillPct`/`transportDrops`/`oversizeDrops`.

### Stream Mode and Send Feedback

```bash
# Live: change max payload size on the fly (576..4000)
curl "http://<device-ip>/api/v1/set?outgoing.maxPayloadSize=4000"

# Restart-only
curl "http://<device-ip>/api/v1/set?outgoing.stream_mode=compact"
curl "http://<device-ip>/api/v1/set?outgoing.connected_udp=true"
curl "http://<device-ip>/api/v1/set?outgoing.allowUnixEncoderStall=true"
```

- `outgoing.stream_mode`: `"rtp"` (default) or `"compact"`. Determines packetization format.
- `outgoing.max_payload_size`: Maximum RTP/compact packet payload in bytes. Default `1400`.
  `MUT_LIVE` — applies on the next encoded frame; in-flight packetization for the
  current frame keeps the old size. Range `[576, 4000]`. Values above ~1472 require
  end-to-end MTU support (e.g. Realtek's 3993-byte jumbo-frame links); on a
  standard 1500-MTU path the kernel will IP-fragment, defeating the point.
  Composes with other live fields in a single multi-set request — for example
  `?video0.bitrate=8000&outgoing.maxPayloadSize=4000` applies both atomically.
  Live updates are accepted across all transports (`udp://`, `unix://`, `shm://`):
  the SHM ring slot is sized at startup to fit the validated ceiling so any value
  in range applies live without restart, just like UDP/Unix.
- `outgoing.connected_udp`: When `true`, calls `connect()` on the UDP socket so the kernel
  returns ICMP port-unreachable errors via `sendmsg()`. Useful for detecting that a receiver
  is down. Default `false` (fire-and-forget).
- `outgoing.allow_unix_encoder_stall`: Restart-required boolean, default `false`.
  When `false`, `unix://` sends use the 2 ms socket timeout and cumulative
  approximately 4 ms RTP frame budget; sustained pressure drops the unsent
  remainder of the frame so the encoder stays live. When `true`, those two
  bounds are disabled for `unix://` only: a full consumer queue blocks the
  encoder output thread until the consumer resumes, preserving the legacy
  behavior. UDP, `shm://`, and `frame-shm://` are unaffected. The 256-datagram
  queue recommendation remains active in both modes.

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

Reload `/etc/venc.json` from disk and rebuild the pipeline. Equivalent to sending
`SIGHUP`. This endpoint does NOT write the in-memory config back to disk, so a manual
file swap (editor, scp, json_cli) followed by `/api/v1/restart` reloads exactly what
was placed on disk.

In v0.7.8 persistence moved into the `/api/v1/set` layer — every set (LIVE or RESTART)
now saves to disk before returning, so the WebUI "Save & Restart" flow (applyChanges
→ /api/v1/restart) ends with the on-disk copy already matching memory before the
reload runs.

```bash
curl http://<device-ip>/api/v1/restart
```

Response `200`:
```json
{"ok":true,"data":{"reinit":true}}
```

### `GET /api/v1/attitude`

Live fused attitude from the on-board IMU (complementary filter feeding the
RTP sidecar ATTITUDE trailer). Star6E only; requires `attitude.enabled` and
`imu.enabled`. Angles in degrees, camera frame after the `attitude.axisFwd`/
`axisDown` remap and boresight trims.

```bash
curl http://<device-ip>/api/v1/attitude
```

Response `200`:
```json
{"ok":true,"data":{"valid":true,"settled":true,"rollDeg":0.5,"pitchDeg":-0.2,"yawDeg":1.9}}
```

`{"valid":false}` until the estimator has a gravity reference. `501` on
backends without an attitude path (Maruko).

### `GET /api/v1/attitude/calibrate_level`

The "it's laying flat now" calibration. Hold the camera level and still;
venc averages the level-pose accelerometer samples — ODR-independent: it
completes early once it has the sample target, otherwise it takes whatever
the ≤2 s window gathered (as long as ≥32 samples arrived). It then solves
the boresight trims exactly (input-side rotation, not an output Euler
subtraction) and persists `attitude.trimRollDeg`/`trimPitchDeg` through the
standard restart-set path. Call `/api/v1/restart` afterwards to apply.

```bash
curl http://<device-ip>/api/v1/attitude/calibrate_level
```

Response `200`:
```json
{"ok":true,"data":{"trimRollDeg":-2.60,"trimPitchDeg":20.43,"restartRequired":true}}
```

`409 calibration_failed` when fewer than 32 IMU samples arrive in the ≤2 s
window (`attitude.enabled`/`imu.enabled` off) or the averaged gravity
magnitude is implausible (<0.5 g / >1.5 g — device moving). `501` on Maruko.

### `GET /api/v1/defaults`

Overwrite the in-memory config with compiled-in defaults, persist to `/etc/venc.json`,
then trigger a full pipeline reinit. Drives the "Restore Defaults" button in the WebUI.

Added in v0.7.8. The `saved` field in the response reflects whether persistence
actually succeeded — `false` means the runtime is at defaults but the on-disk copy is
stale (e.g. disk full, readonly FS, permission error); check the venc log for the
`[venc_config] ERROR:` line.

```bash
curl http://<device-ip>/api/v1/defaults
```

Response `200`:
```json
{"ok":true,"data":{"defaults":true,"reinit":true,"saved":true}}
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
    "metrics": { "exposure_us": 9999, "sensor_gain_x1024": 1673, "isp_gain_x1024": 1024, "fps": 90 },
    "runtime": { "sensor_fps": 90, "active_precrop": { "x": 0, "y": 240, "w": 2560, "h": 1440 } }
  }
}
```

`runtime.active_precrop` is included on both backends whenever the
pipeline has been started; it is omitted before the first start and
after a stop.

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

### `GET /api/v1/snapshot.jpg`

Capture one JPEG frame from the snapshot subsystem and return it as
`image/jpeg`.  Gated by `snapshot.enabled`; returns `503`
(`snapshot_disabled`) when the subsystem is off or the pipeline is not
running, `504` (`snapshot_timeout`) if no frame arrives, `500`
(`snapshot_failed`) on backend error.  Not a JSON endpoint on success.

This is also the **QR-scanning source**: `tools/qr/qr_decode` reads JPEG
directly (vendored stb_image, luma-only), so the boot-pairing flow is
`curl … /snapshot.jpg | qr_decode`.  The MJPEG channel is created once at
pipeline start and pulse-encoded per capture (`StartRecvPic → GetStream →
StopRecvPic`) — rapid back-to-back captures are safe.  Frame geometry follows
`snapshot.width`/`snapshot.height` (0 = inherit the main stream); QR range
scales with pixels per module, so size the channel up for longer-distance
markers.  Pairing, commands, boot scheduling, and action dispatch are
deliberately outside the waybeam binary and this endpoint.

This route is registered by the Star6E and Maruko backends. The initial CV610
backend does not advertise snapshot capability and does not register the
route, so requests receive the normal HTTP `404` route response.

> **Retired:** `GET /api/v1/snapshot.pgm` (grayscale P5 PGM, added 0.59.0)
> was removed in 0.60.0 and now answers `404`.  It captured through a
> short-lived per-request VPE/SCL tap, and device stress-testing showed the
> tap's enable/disable cycle can race an in-flight MHAL buffer and wedge the
> whole VPE — up to a kernel panic or hard hang.  The MJPEG channel above has
> no such cycle; consumers that want grayscale decode the JPEG's luma plane
> (`qr_decode` does this natively).

```bash
# QR scanning (boot pairing, bench)
curl -s http://<device-ip>/api/v1/snapshot.jpg | qr_decode
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

### `GET /api/v1/audio/status`

Return live observability for the audio capture/encode pipeline.  Useful for
diagnosing silent audio failures (missing `libmi_ai.so` on Maruko, missing
`libopus.so`, capture thread not running, codec mismatch).

```bash
curl http://<device-ip>/api/v1/audio/status
```

Response `200` (Star6E with audio enabled):
```json
{
  "ok": true,
  "data": {
    "enabled": true,
    "backend": "star6e",
    "lib_loaded": true,
    "device_enabled": true,
    "channel_enabled": true,
    "running": true,
    "codec": "opus",
    "sample_rate": 48000,
    "channels": 1,
    "opus_loaded": true
  }
}
```

Response `200` (Maruko with audio enabled):
```json
{
  "ok": true,
  "data": {
    "enabled": true,
    "backend": "maruko",
    "lib_loaded": true,
    "device_opened": true,
    "group_enabled": true,
    "running": true,
    "codec": "opus",
    "sample_rate": 48000,
    "channels": 1,
    "opus_loaded": true
  }
}
```

Response `200` (CV610 with audio enabled):
```json
{
  "ok": true,
  "data": {
    "enabled": true,
    "backend": "cv610",
    "running": true,
    "codec": "opus",
    "sample_rate": 48000,
    "channels": 1,
    "muted": false,
    "frames": 1486,
    "bytes": 60966,
    "packets": 1486,
    "drops": 0
  }
}
```

CV610 reports a different field set on purpose. It has no dlopened audio
library to report on (the MPI is linked), and its 48 kHz mono Opus
configuration is compiled in rather than read from config, so the useful
observability there is throughput: `frames` / `bytes` / `packets` / `drops`
are lifetime counters from the audio thread. A client must treat the field
set as backend-specific and key off `backend`.

Response `200` when `audio.enabled=false`:
```json
{"ok":true,"data":{"enabled":false,"backend":"maruko"}}
```

Field reference:

| Field | Meaning |
|---|---|
| `enabled` | `audio.enabled=true` and `*_audio_init` reached the run state |
| `lib_loaded` | The MI audio shared library (`libmi_audio.so` Star6E / `libmi_ai.so` Maruko) was found and dlopened |
| `device_enabled` / `device_opened` | The capture device handle is open |
| `channel_enabled` / `group_enabled` | The capture channel / group is enabled |
| `running` | Capture and encode threads are alive |
| `codec` | `"g711a"`, `"g711u"`, `"opus"`, `"pcm"`, or `"unknown"` |
| `sample_rate` | Configured audio sample rate (Hz) |
| `channels` | 1 (mono) or 2 (stereo) |
| `opus_loaded` | When `codec="opus"`, the Opus encoder was successfully initialized.  `false` here while `codec="opus"` means audio falls back to raw PCM with a startup warning. |

Error `501` — backend has no audio observability hook (`query_audio_status`
not registered):
```json
{"ok":false,"error":{"code":"not_implemented","message":"audio status not available on this backend"}}
```

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

Backend gating: only the Star6E backend currently runs the runtime poll that
honors HTTP-driven start/stop.  On Maruko, recording is config-driven only
(set `record.enabled=true` + `record.mode="mirror"|"dual"` in `/etc/venc.json`)
and `/api/v1/record/start` returns:

```json
{"ok":false,"error":{"code":"not_implemented","message":"HTTP record control not available on this backend"}}
```

with HTTP `501`.  This avoids the prior behaviour where the request returned
`{"ok":true}` but no recording started.  Tracked as Phase 6.5 in
`MARUKO_PARITY_PLAN.md`.

### `GET /api/v1/record/stop`

Stop SD card recording.

```bash
wget -q -O- "http://<device-ip>/api/v1/record/stop"
```

Response `200`:
```json
{"ok":true,"data":{"action":"stop"}}
```

Same backend gating applies — Maruko returns `501 not_implemented`.

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
	"elapsed_ms": 25000,
    "segments": 1,
    "stop_reason": "none"
  }
}
```

`elapsed_ms` is the elapsed duration of the active or most recently stopped
recording. `stop_reason` values: `"none"` (currently recording), `"manual"`,
`"disk_full"`, `"write_error"`.

### `GET /api/v1/recordings`

List `.ts` and `.hevc` files in the configured `record.dir` along with
disk-usage totals.

```bash
wget -q -O- "http://<device-ip>/api/v1/recordings"
```

Response `200`:
```json
{
  "ok": true,
  "data": {
    "dir": "/mnt/mmcblk0p1",
    "free_bytes": 1234567890,
    "total_bytes": 15000000000,
    "files": [
      { "name": "rec_01h00m00s_abcd.ts", "size": 12345678, "mtime": 1713600000 }
    ],
    "truncated": false
  }
}
```

Error `503 not_available` — directory not mounted.
Error `500 internal_error` — cannot read directory or out of memory.

The listing is capped at 512 entries; `truncated` is `true` when the
cap was hit and older recordings were not included.  `free_bytes` /
`total_bytes` are `-1` when `statvfs` is unavailable.

### `GET /api/v1/recordings/download?file=<name>`

Stream a single recording as an `attachment` download.  `file` must be
a plain name from the listing — leading `.`, path separators and
control bytes are rejected.

```bash
wget "http://<device-ip>/api/v1/recordings/download?file=rec_01h00m00s_abcd.ts"
```

`Content-Type` is `video/mp2t` for `.ts` files and
`application/octet-stream` for `.hevc`.

Error `400 invalid_request` — missing or unsafe `file` parameter.
Error `404 not_found` — file does not exist in `record.dir`.

### `GET /api/v1/recordings/delete?file=<name>`

Remove a recording from `record.dir`.

```bash
wget -q -O- "http://<device-ip>/api/v1/recordings/delete?file=rec_01h00m00s_abcd.ts"
```

Response `200`:
```json
{"ok":true}
```

Error `400 invalid_request` — missing or unsafe `file` parameter.
Error `404 not_found` — file already gone.
Error `409 record_active` — file is currently being written; stop
recording first.
Error `500 delete_failed` — filesystem error.

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

Query the secondary VENC channel status. Always returns 200; the `active`
field tells you whether dual or dual-stream mode is currently running.

```bash
wget -q -O- "http://<device-ip>/api/v1/dual/status"
```

Response `200` — dual VENC active:
```json
{"ok":true,"data":{"active":true,"channel":1,"bitrate":20000,"fps":120,"gop":240}}
```

Response `200` — dual VENC not active (off, mirror, or any non-dual mode):
```json
{"ok":true,"data":{"active":false}}
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

Error `501` — backend does not support live dual/set (Maruko).  The Star6E
binding owns the low-level `MI_VENC_*ChnAttr` write path; a Maruko port has
not landed yet.

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

### `GET /api/v1/idr/stats`

Return per-channel IDR-rate-limit counters.  The encoder enforces a minimum
spacing between honored IDRs to keep bitrate predictable when many sources
(scene detector, HTTP `/request/idr` and `/api/v1/dual/idr`, recorder
segment rotation) ask for keyframes simultaneously.  This endpoint reports
how many requests were honored vs. coalesced (dropped) per channel.

```bash
curl http://<device-ip>/api/v1/idr/stats
```

Response `200`:
```json
{
  "ok": true,
  "data": {
    "min_spacing_us": 250000,
    "channels": [
      {"idx": 0, "honored": 47, "dropped": 3},
      {"idx": 1, "honored": 12, "dropped": 0}
    ]
  }
}
```

`min_spacing_us` is the compile-time minimum spacing in microseconds.
Channels with both counters at zero are omitted.  Available on both
backends; always returns a valid response (even when no IDR has been
requested yet — `channels` is then an empty array).

### `GET /api/v1/transport/status`

Return live observability for the active video transport (UDP / Unix /
SHM).  Used by the WebUI status bar and by external link controllers
that need to detect output backpressure.

```bash
curl http://<device-ip>/api/v1/transport/status
```

Response `200` (SHM ring transport, common for `outgoing.server=shm://...`):
```json
{
  "ok": true,
  "data": {
    "active": true,
    "transport": "shm",
    "fillPct": 12,
    "inPressure": false,
    "transportDrops": 0,
    "pressureDrops": 0,
    "packetsSent": 184523,
    "oversizeDrops": 0,
    "slotCount": 1024,
    "usedSlots": 122
  }
}
```

Response `200` (frame-shm ring, with the ring-fill bitrate clamp engaged):
```json
{
  "ok": true,
  "data": {
    "active": true,
    "transport": "frame-shm",
    "fillPct": 25,
    "inPressure": false,
    "transportDrops": 0,
    "pressureDrops": 0,
    "framesSent": 41207,
    "oversizeDrops": 0,
    "slotCount": 8,
    "usedSlots": 2,
    "throttlePermille": 640,
    "effectiveBitrateKbps": 6400
  }
}
```

Response `200` (UDP/Unix kernel-buffer fill_pct):
```json
{
  "ok": true,
  "data": {
    "active": true,
    "transport": "udp",
    "fillPct": 4,
    "inPressure": false,
    "pressureDrops": 0,
    "transportDrops": 0,
    "packetsSent": 184523
  }
}
```

Response `200` (output disabled or no socket open):
```json
{"ok":true,"data":{"active":false,"transport":"none"}}
```

Field reference:

| Field | Meaning |
|---|---|
| `transport` | `"shm"`, `"frame-shm"`, `"udp"`, `"unix"`, or `"none"` |
| `fillPct` | Current fill ratio `0..100`.  For SHM, ring fill.  For UDP, kernel send-buffer fill.  For Unix, fill against the peer's datagram queue — see the note below |
| `inPressure` | Point-in-time HTTP snapshot: true when the current `fillPct >= 75`, false otherwise. The RTP sidecar field uses 75/50 hysteresis |
| `transportDrops` | Lifetime drops: ring-full for SHM; unsent datagrams after `EAGAIN`/`ENOBUFS` or frame-budget exhaustion for UDP/Unix |
| `pressureDrops` | Frames dropped by the in-process backpressure path while a sidecar probe was subscribed |
| `packetsSent` | Lifetime sends accepted: ring writes for SHM, datagrams for UDP/Unix |
| `oversizeDrops` | (SHM only) Frames rejected for exceeding slot capacity |
| `slotCount` / `usedSlots` | (SHM only) Ring sizing; `usedSlots` is a snapshot |
| `throttlePermille` | (frame-shm only) Ring-fill bitrate clamp, `1000` = unclamped, `250` = floor.  A **clamp, not a veto** — `video0.bitrate` in `/api/v1/config` is never modified, so an external rate controller's writes all still succeed |
| `effectiveBitrateKbps` | (frame-shm only) `video0.bitrate` scaled by `throttlePermille`; what the encoder is actually programmed to |

On `unix://`, `fillPct` is measured against the *peer's* datagram queue,
which is the limit that actually blocks a sender — not the local
`SO_SNDBUF`.  The producer cannot read the peer's queue depth, so the
denominator is calibrated from the first send that saturates the queue and
is an estimate from `net.unix.max_dgram_qlen` before that.  `fillPct` may
therefore read low until the transport has been pushed once.

With `outgoing.allowUnixEncoderStall=false`, `transportDrops` rising on
`unix://` means the consumer is not keeping up: a frame's packets exhausted
the cumulative flush budget and the remainder was dropped rather than
stalling the encoder. With the compatibility option enabled, ordinary queue
pressure blocks instead and does not increment `transportDrops`; status reads
may show a full queue while the consumer is paused. The usual cause of
unexpected pressure is a shallow `net.unix.max_dgram_qlen` — see the
`unix://` notes in the README.

A `throttlePermille` below `1000` means the consumer is not draining the ring
fast enough for the configured bitrate.  Pinned at `250` the clamp has spent
all its authority and drops may resume — that case is also logged once on
entry and once on exit.  Disable with `outgoing.shmThrottle=false` (live).

Error `501` — backend has no transport observability hook.

### `GET /api/v1/modes`

Return the table of sensor pads and resolution modes the underlying SDK
reports for the currently-loaded sensor driver.  Used to populate the
WebUI sensor-mode dropdown and to validate `sensor.mode` writes.

```bash
curl http://<device-ip>/api/v1/modes
```

Response `200`:
```json
{
  "ok": true,
  "data": {
    "selected_pad": 0,
    "selected_mode": 1,
    "pads": [
      {
        "pad": 0,
        "modes": [
          {"index": 0, "width": 1920, "height": 1080, "min_fps": 1, "max_fps": 60,  "desc": "1080p60",  "selected": false},
          {"index": 1, "width": 1920, "height": 1080, "min_fps": 1, "max_fps": 90,  "desc": "1080p90",  "selected": true},
          {"index": 2, "width": 1472, "height": 816,  "min_fps": 1, "max_fps": 120, "desc": "1472x816@120", "selected": false}
        ]
      }
    ]
  }
}
```

`selected_pad` / `selected_mode` reflect the currently-active pipeline
selection.  The full `pads[].modes[]` list always shows every mode the
driver enumerates so callers can show an "available modes" UI.

**CV610 note — every advertised mode is selectable, but only against a
sys_config that exposes the runtime sensor clock.** The IMX662 runs 30/60/90
fps on a 37.125 MHz input clock and 100 fps on 27 MHz (27 MHz fed while the
sensor selects its 24 MHz INCK profile). The daemon sets the SoC-side clock
to match the requested mode during MIPI bring-up, by writing
`/sys/module/open_sys_config/parameters/sns0_clk_hz`.

Against an older `open_sys_config` without that parameter the daemon logs a
warning and continues, and the clock stays at whatever
`CV610_SENSOR_PROFILE` selected at insmod — correct for exactly one mode.
Every other mode then runs at the wrong rate, scaled by the clock ratio
(measured: `video0.fps=60` on the 27 MHz profile delivers 43.6 fps =
60 x 27/37.125) while `/api/v1/modes` and `/api/v1/fps/live` still report
60. Neither endpoint measures anything, so a client must not treat a
`selected` mode as proof of the delivered rate — measure it from
`framesSent` in `/api/v1/transport/status`.

CV610 preserves the same envelope and `selected_pad` / `selected_mode` /
`pads[].modes[]` shape. Its initial IMX662 backend reports one synthetic pad
containing the four supported fixed-rate modes (1080p30/60 RAW12 and
1080p90/100 RAW10); each entry has equal `min_fps` and `max_fps`.

`selected_pad` / `selected_mode` are what bring-up **actually selected**, not
what the config asks for — the backend publishes them once the pipeline is
resolved, so a forced `sensor.mode`, a substituted rate and a failed bring-up
are all reported honestly. Both read `-1` until bring-up completes, which in a
healthy daemon is never observable: the HTTP server starts after the backend
prepares.

The `width` / `height` of a CV610 entry is what the **sensor captures**, not
what is encoded. Every entry captures 1920x1080, and `video0.size` is the
encoded geometry VPSS scales that capture down to.

**Selecting a CV610 mode uses the same two knobs as SigmaStar.** An explicit
`sensor.mode` is an index into this list and wins outright; the mode's rate
becomes the pipeline rate and `video0.fps` is left as written. With
`sensor.mode` at `-1` (or any negative value, meaning auto) `video0.fps` is a
**target** rather than a command: an exact match is used, otherwise the
slowest mode still faster than the target, and a target above every mode
clamps to the fastest. `sensor.index` accepts only `-1` or `0` — one synthetic
pad — and anything else is `409`, as is a `sensor.mode` past the end of the
list.

A substituted rate is announced on the daemon's log
(`Requested 45 fps, using 60 fps (sensor mode 1: 1080p60 RAW12)`) and is
visible over HTTP as `/api/v1/fps/live` disagreeing with
`/api/v1/fps/config`. Neither endpoint measures anything, so that pair states
the *intended* rate, not the delivered one. A client must not read a mode's `width` / `height` as the stream
resolution — `video0.size` in `/api/v1/config` is the encoded size, and
`auto` there means the mode's capture geometry.

Error `500 modes_failed` — `MI_SNR_QueryResCount` failed (e.g. sensor
driver not loaded yet during a brief startup window). This SigmaStar query
failure does not apply to CV610's static IMX662 table.

## QR Scanning (Star6E only)

An overlay-free NV12 luma tap on VPE **port1** feeds the isolated
`/usr/bin/qr_decode` helper. port1 exists because MI_RGN composites **per scaler output port** and
every overlay producer targets port0 — `debug_osd` here, `osd_render` in
waybeam-hub — so the MJPEG snapshot channel (a port0 1:N consumer) carries
whatever HUD is running over anything a marker might occupy.

Gated on `qr.tapEnabled`.  port1 is single-owner and shared with framing-stab
and NPU detect, so a scan is **mutually exclusive** with both: whoever holds it
wins, and QR is the lowest-priority claimant.

### `GET /api/v1/qr/scan[?ms=N]`

Open a scan window, or **extend** the one already running.  `ms` defaults to
`qr.windowMs`, clamped to 1000–60000.

A supervisor thread owns the window: it opens port1, sends each fresh frame to
the helper until the deadline, and closes the port on the way out — so a client that dies
mid-scan cannot strand port1.  **A successful decode ends the window early**;
the point of a window is to find one code.

Extending never touches port state.  That matters: re-opening port1 per request
is what wedged the retired `/api/v1/snapshot.pgm`.

```bash
curl "http://<device-ip>/api/v1/qr/scan?ms=10000"
```

Response `200`:
```json
{"ok": true, "data": {"scanning": true, "window_ms": 10000,
                      "remaining_ms": 9999, "capture": "1080x1080"}}
```

- `409 port1_busy` — framing-stab or NPU detect holds port1.  Reported
  immediately, never queued.
- `503 tap_disabled` — `qr.tapEnabled` is off, or the pipeline is not running.
- `500 scan_failed` — the SCL would not drive the configured geometry.  The
  port is enabled and then verified to deliver a frame within 1 s; if it does
  not, port1 is released rather than held for a window that can never capture.

### `GET /api/v1/qr/stop`

End the current window and hand port1 back.  Blocks until the port is actually
released, so port1 is free on return.  Idempotent.

This *requests* a close rather than forcing one.  The port is held for a minimum
of 750 ms after it comes up, because disabling one that only just opened —
while the SCL still has buffers in flight — panics the kernel (the same failure
that retired `/api/v1/snapshot.pgm`).  A stop issued against a window older than
that returns immediately; against a brand-new one it waits out the remainder.
A further 500 ms floor applies between two opens.  Together these cap the
port-cycle rate no matter how fast a client loops.

Note the path: **not** `/api/v1/qr/scan/stop`.  The router matches on prefix and
accepts a `/` continuation, so a nested path would be swallowed by the
`/qr/scan` route.

### `GET /api/v1/qr/status`

Poll a window without disturbing it.

```json
{
  "ok": true,
  "data": {
    "armed": true,
    "scanning": false,
    "window_ms": 15000,
    "remaining_ms": 0,
    "capture": "1080x1080",
    "frames": 3,
    "grabs": 1,
    "port1_owner": "",
    "decode": {
      "attempts": 1,
      "decoded": true,
      "payload": "P23456789ABCDEFG",
      "stage": "qr_decode",
      "decode_ms": 84,
      "last_ms": 84
    }
  }
}
```

- `frames` / `grabs` — buffers drained, and buffers actually copied out, this
  window.  Reset when a window opens, not when one is extended.
- `port1_owner` — `""`, `"qr"`, `"stab"` or `"detect"`.
- `decode.stage` — `qr_decode` when the isolated decoder helper succeeds.
  Detailed cascade-stage diagnostics are available from `qr_decode --stats`.
- The `decode` block **survives the window closing** and is cleared only by the
  next `/qr/scan`, so a client polling at 1 Hz still sees the payload from a
  window that found its code and shut down between two polls.

`payload` is a Waybeam transport envelope: exactly 16 characters from the QR
alphanumeric alphabet (`0-9 A-Z` and `` $%*+-./:``).  Nothing in that set needs
JSON escaping; anything outside it is scrubbed to `?` before serialization.

### `GET /api/v1/qr/tap.pgm`

One frame of the tap as a binary P5 PGM (self-describing dimensions, stride
removed).  Debug instrumentation for validating capture — geometry,
OSD-freedom, exposure — not part of the scanning flow.

- `503 tap_disabled` — no window is open.
- `504 tap_timeout` — no frame arrived.
- `409 scan_decoding` — a cascade currently owns the latch.  The latch is
  single-buffered; refusing beats blocking an httpd worker or returning a frame
  that is being overwritten.

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

1. **Accepted config writes are persistent.** `/api/v1/set` persists accepted
   live and restart-required field changes to the registered config path before
   returning. `/api/v1/defaults` persists compiled defaults. `/api/v1/restart`
   reloads the on-disk config and does not synthesize new changes by itself.

2. **Video codec is hardcoded H.265.** The `video0.codec` field was
   retired in 0.10.12. Setting it via `/api/v1/set` returns `404`
   `unknown config field`. Legacy configs containing
   `"codec": "h264"` or `"h265"` load cleanly — the key is ignored
   and HEVC is used unconditionally.

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
- Shared `GET` endpoints remain consistent across backends. Platform-specific
  routes are listed explicitly in the matrix below.

### Backend Support Matrix

Endpoints that behave the same on both backends are omitted.  Only feature
divergence is listed.  As of `contract_version: 0.12.1`:

| Feature / Endpoint | Star6E | Maruko | Notes |
|---|---|---|---|
| `/api/v1/record/{start,stop}` | yes | **501** | Maruko has no runtime poll loop yet (Phase 6.5).  Config-driven recording (`record.enabled=true` + `record.mode="mirror"\|"dual"`) works. |
| `/api/v1/record/status` | live counters | live counters | Both backends register a status callback against the live `Star6eTsRecorderState`; Maruko reflects daemon-config-driven recording (mirror/dual). |
| `/api/v1/qr/*` | yes | **404** | Star6E-only VPE port1 luma tap. QR capability fields remain in the shared schema but report `supported:false` on Maruko. |
| `/api/v1/recordings*` | yes | yes | File listing/download/delete works against `record.dir` regardless of which backend wrote the file. |
| `/api/v1/audio/status` | yes | yes | Both backends register `query_audio_status`. |
| `/api/v1/dual/status`, `/dual/idr` | yes | yes | `/dual/status` always 200 (`active:false` when off, `active:true,channel,bitrate,fps,gop` when on).  `/dual/idr` returns 200 when active, 404 when not. Maruko HTTP registration landed in 0.10.4 — earlier Maruko builds returned 404 from these even when `record.mode=dual` was running. |
| `/api/v1/dual/set` | yes | **501** | Star6E-only: the underlying `MI_VENC_*ChnAttr` write path binds to `i6_venc_chn`, but Maruko's venc library expects `i6c_venc_chn` (different layout). Maruko returns 501 until the call path is ported. |
| `/api/v1/iq` and `/api/v1/iq/set` | full (≈45 params) | full (parity in `maruko_iq.c`) | Both backends use the same IQ table schema. |
| `/api/v1/awb` | live | live | Both backends register `query_awb_info`. |
| `/api/v1/ae` | live + `runtime.active_precrop` | live + `runtime.active_precrop` | Both backends now include `runtime.active_precrop` in the AE response (Maruko parity landed in `0.8.4`). |
| `/api/v1/transport/status` | yes | yes | SHM-ring fields are shown when `outgoing.server=shm://`; otherwise the UDP/Unix subset. |
| `/api/v1/idr/stats` | yes | yes | Identical schema; values reflect each backend's IDR rate-limit. |
| `video0.codec=h264` | 404 unknown_field | 404 unknown_field | Field retired in 0.10.12; codec is hardcoded H.265 on both backends. |
| `video0.scene_threshold` / `scene_holdoff` | yes | yes | Restart-required fields; both backends run the shared scene detector. |
| `video0.framing` / `zoom_x` / `zoom_y` | yes | partial | `framing` requires reinit; zoom presets work on both backends, the `stab` preset is Star6E-only (no-op on Maruko); `zoom_x/y` are live pan controls (ignored under `stab`). |
| `detect.model_path` / `model_id` / `conf_thresh` / `nms_iou` | **live** | **live** | Both backends hot-swap the NPU detector on the pipeline thread without respawning video. Star6E uses VPE port 1; Maruko uses SCL port 3 and its drain-while-disable teardown. A model whose reported input geometry disagrees with the configured tap is refused and leaves detection off. |
| `detect.net_width` / `net_height` | restart | restart | Tap geometry is fixed when the VPE/SCL detector port is created. |
| `video0.min_qp` / `max_qp` | live | **501** | Star6E-only RC QP bounds; Maruko capabilities report these fields unsupported. |
| `isp.aeEngine` ("sdk" only) | applied | applied | Unified AE selector landed in 0.10.13.  `custom` (userspace AE governor) is RETIRED — Maruko in 0.22.0, Star6E in 0.47.0 — and the value was **removed** in 0.47.0.  `sdk` is the only accepted value; any other (e.g. a stale `custom`) warns and falls back to `sdk`.  Both backends run the SDK firmware/bin AE for convergence plus a supervisory thread that enforces the `isp.gain*`/`isp.shutter*` limits. |

## Change Log (Contract)
- `0.18.3` (additive — CV610 sensor-mode selection reaches SigmaStar parity):
  - **`sensor.index` and `sensor.mode` are now supported on CV610**, both
    `restart_required`, the same `mutability` the shared table gives them on
    Star6E and Maruko. They parsed before but were read by nothing, so
    `/api/v1/set` answered `409` for a field the config file carried. A
    client that renders sensor controls from `/api/v1/capabilities` now gets
    the same surface on all three backends.
  - **`video0.fps` is a target on CV610, not a command.** With `sensor.mode`
    auto, an exact match is used, otherwise the slowest mode still faster
    than the target, and a target above every mode clamps to the fastest.
    Rates that are not in the table were `409` before and are now honoured
    with a substitution — see `/api/v1/modes`. `video0.fps=0` remains `409`.
  - **`/api/v1/modes` reports the achieved selection.** `selected_pad` /
    `selected_mode` were recomputed from `video0.fps` per request, so they
    described the configured mode even when another one was running. They are
    now published by the backend at bring-up, and read `-1` before it
    completes. Shape unchanged.
  - `video0.gopSize` is validated against the **selected** mode's rate. The
    encoder has always derived its GOP length from that rate; the check used
    `video0.fps`, which the two knobs above can now separate from it. A
    `gopSize` that was accepted and then exceeded the encoder's 65536-frame
    limit is now rejected at `409`.
- `0.18.2` (additive — CV610 `video0.size` becomes a real control):
  - **`video0.size` on CV610 now accepts any geometry the mode can be scaled
    down to**, not just the capture size. VI has no scaler, so the backend
    previously bound VI straight to VENC and `1920x1080` was the only value
    that validated. A VPSS group now sits between them (VI chn → VPSS grp →
    VPSS chn → VENC) and does the scaling, so `video0.size` is the encoded
    geometry and `video0.fps` alone selects the sensor mode — the same split
    Star6E has. Rejections are `409`: not a multiple of 8, below 128x128, or
    larger than the mode's capture size (VPSS does not invent detail, and
    upscaling would spend encoder bandwidth for no information).
  - `/api/v1/modes` keeps its shape, but a CV610 entry's `width` / `height`
    is now explicitly the **capture** geometry rather than the stream
    resolution, and `selected` follows `video0.fps` alone. Previously a
    client could infer the two were the same, because they were.
  - **A CV610 sensor clock that cannot be set is now fatal to bring-up**, so
    a mode either delivers its nominal rate or the daemon does not start. The
    two failures are answered differently: a `sys_config` with no
    `sns0_clk_hz` parameter predates it, and that stays a warning with the
    clock left as loaded (one mode then runs correctly, the rest do not).
    A parameter that *is* present and rejects the write means the line timing
    is known to be running against the wrong MCLK, and the delivered rate
    would be wrong while `/api/v1/modes` and `/api/v1/fps/live` both report
    nominal — so bring-up aborts rather than serve a rate nothing reports.
  - **Correction to `0.18.1`**, which stated that `video0.fps` on CV610 is
    "honoured only within the sensor clock profile the kernel modules were
    loaded with". That shipped alongside the runtime sensor clock in the same
    release and was stale on arrival: the daemon sets the clock for the
    selected mode at bring-up. The boot-time profile matters only against a
    `sys_config` too old to expose `sns0_clk_hz`, which is what the caveat
    under `/api/v1/modes` describes.
- `0.18.1` (additive — CV610 control surface reaches what the backend reads):
  - CV610 now advertises `video0.fps`, `video0.size`, `outgoing.enabled`,
    `outgoing.server`, `outgoing.connected_udp`,
    `outgoing.allow_unix_encoder_stall`, `audio.enabled` and `audio.mute` as
    supported. All were already read by the backend; the capability set
    under-reported them, so `/api/v1/set` answered `501` for a value the
    daemon would have honoured on the next start.
  - **`mutability` is now per backend.** It may only be downgraded from the
    shared table, never widened. CV610 reports `video0.fps`,
    `outgoing.enabled`, `outgoing.server` and `audio.mute` as
    `restart_required` where Star6E and Maruko report `live`, because the
    CV610 slice reads them once at start. `/api/v1/live/set` rejects them
    with `400 invalid_request` on CV610 and still applies them on Star6E.
    A client must therefore read `mutability` from the target device rather
    than assuming it per field.
  - `/api/v1/audio/status` is implemented on CV610, reporting the fixed
    48 kHz mono Opus configuration plus live `frames` / `bytes` / `packets` /
    `drops` counters. It answered `501 not_implemented` before while audio
    was in fact streaming.
  - `video0.fps` on CV610 is honoured only within the sensor clock profile
    the kernel modules were loaded with — see the CV610 caveat under
    `/api/v1/modes`. No contract shape changed; the constraint is documented
    because the mode list cannot express it.
  - Still unsupported on CV610, and deliberately so: `audio.sample_rate`,
    `audio.channels`, `audio.codec`, `audio.volume` (hardcoded in
    `src/cv610_audio.c`) and `video0.rc_mode` (hardcoded H.265 CBR). The
    shipped defaults coincide with the hardcoded values; that is not support.
- `0.18.0` (additive — Unix encoder-stall compatibility):
  - Added restart-required `outgoing.allow_unix_encoder_stall` with camelCase
    alias `outgoing.allowUnixEncoderStall`. Default `false` retains bounded
    send/drop behavior; `true` restores blocking `unix://` sends.
  - Added Star6E-only inline QR scan routes and the `qr.*` configuration
    surface. `qr.window_ms` applies live to the next scan; Maruko advertises
    all QR fields unsupported and does not register the routes.
  - Added live Star6E `video0.min_qp` / `video0.max_qp` RC bounds. Maruko
    advertises these fields unsupported.
  - `/api/v1/record/status` now includes `elapsed_ms` on both backends.
- `0.17.0` (additive — socket transport telemetry):
  - The UDP/Unix response from `/api/v1/transport/status` now reports
    `transportDrops` and `packetsSent`, matching the existing SHM field names.
  - Unix `fillPct` is calibrated against the peer's observed full datagram
    queue rather than the sender's `SO_SNDBUF`.
- `0.16.1` (non-breaking):
  - `isp.keepAspect` is now **supported on Maruko** — capabilities report
    `supported:true` and `/api/v1/set` accepts it (previously rejected with
    `not_implemented`). `false` passes the full sensor frame through and the
    I6C SCL scales both axes non-uniformly (stretch-to-fill). Exception: a
    single-axis squeeze (one axis already matching the output) stalls the
    SCL, so that geometry is centre-cropped regardless, with a startup note.
- `0.16.0` (breaking — snapshot.pgm retired):
  - `GET /api/v1/snapshot.pgm` removed; answers `404`. Its per-request VPE/SCL
    tap could wedge the SoC (device-verified: `DisablePort … mhal not return
    buffer` → `EnsureInputPortFifoEmpty` storm). QR scanning now consumes
    `GET /api/v1/snapshot.jpg`; `qr_decode` reads JPEG natively. Error codes
    `bad_crop`, `bad_max_dim`, `snapshot_gray_busy` and
    `snapshot_gray_unsupported` are gone with it.
  - The `snapshot.*` config section is dashboard-visible (FieldUi group
    "Snapshot", rendered from capabilities).
- `0.15.0` (additive — Maruko detector parity):
  - Maruko now implements the ABI-3 detector host on SCL port 3, including
    live enable/disable and model reload, `detect.osd`, and the unchanged RTP
    sidecar DETECT trailer. Detect fields no longer return 501 on Maruko.
  - Detection is refused while Maruko stabilization or zoom is active until
    the independent SCL-port crop can be mapped to encoded-frame coordinates.
  - The default Maruko deployment uses an 800x448 I6C model. Small-flash
    systems may store it as `/root/models/<name>.img.xz`; the init script
    inflates the configured `/tmp/<name>.img` before startup.
- `0.14.0` (additive — detector live model swap):
  - `detect.model_path` changed from `MUT_RESTART` to `MUT_LIVE`, and
    `detect.model_id` / `detect.conf_thresh` / `detect.nms_iou` are now
    settable (`MUT_LIVE`).  Changing any of them re-creates only the NPU
    detector plugin + VPE port1 tap on the pipeline thread — the video0
    encode/RTP path keeps running, so there is no pipeline respawn, keyframe
    reset, reconnect, or transport drop.  A **`model_path` change is not free**,
    though: the NPU graph load runs on the pipeline thread (that is what makes
    the swap atomic against the per-frame `DETECT` snapshot), so frame output
    stalls while it runs.  Measured on Star6E .232 at 100 fps: **~100-450 ms**
    on some runs, **~2.2-2.5 s** on others, with nothing in between and the
    trigger not isolated (not memory pressure, not file I/O, not accumulation
    across reloads).  Budget for **~2.5 s**.
  - `model_id` / `conf_thresh` / `nms_iou` do **not** reload the graph: when the
    requested `model_path` matches what is loaded, the label and thresholds are
    applied in place (thresholds via the plugin's optional `set_thresholds()`,
    falling back to a full reload if the backend lacks it).  Measured cost:
    ~50 ms for a threshold change, ~0 ms for `model_id`, versus ~2300 ms before.
    So live threshold tuning is cheap; only swapping the `.img` is expensive.
    The sidecar `model_id` flips to the new value in lockstep with the first
    new-model `DETECT` trailer.  Star6E only; Maruko returns `501` (no
    `apply_detect_reload`).
  - The host now verifies the loaded model's **real** input geometry (reported
    by the plugin via the new ABI-2 `model_dims()`) against the VPE port1 tap
    it created, and refuses a mismatch — `net_width`/`net_height` are config,
    not evidence of what a `.img` expects.  Because `model_path` is live but
    the dims are restart-scope, pointing `model_path` at a different-geometry
    model used to be accepted silently and left an "active" detector that
    never detected (the backend rejects every frame and `process()` errors are
    not logged).  A refused swap logs both geometries and the exact
    `netWidth`/`netHeight` to set, leaves detection off, and releases the
    port1 claim (`runtime.vpe_taps.port1` reads `null`); the stream is
    unaffected.  Note `/set` still returns `200` — the reload is serviced
    asynchronously on the pipeline thread, so the stored value is accepted
    even when the model is then rejected; check `runtime.vpe_taps` or the log
    for the outcome.
  - `detect.net_width` / `detect.net_height` are now settable as
    `MUT_RESTART` (a tap-geometry change needs the VPE port recreated).
    Both must be `0` (default) or a multiple of 32 (`>=64`).
  - `detect.confThresh` / `nmsIou` accept `[0, 1)` (0 = plugin default);
    `netWidth` / `netHeight` accept `0` or a `>=64` multiple of 32.
- `0.57.0` (additive — new config field + new response fields):
  - Added `outgoing.shm_throttle` (boolean, default `true`, `MUT_LIVE`,
    alias `outgoing.shmThrottle`).  Enables the `frame-shm://` ring-fill
    bitrate clamp; inert on every other transport.  Both backends.
  - `GET /api/v1/transport/status` gains `throttlePermille` and
    `effectiveBitrateKbps` on the `frame-shm` branch only.
  - The clamp never writes `video0.bitrate`, so `GET /api/v1/config` and
    every `set` response are unaffected by it.  Read the effective rate
    from `transport/status`, not from the config.
  - Sidecar `TRANSPORT_INFO` trailer: `_pad[2]` became
    `throttle_permille` (u16, network order).  Trailer stays 16 bytes and
    later trailers keep their offsets; `0` means "not reported".
- `0.46.0` (additive — new config fields):
  - Added `isp.gain_min` (min sensor gain floor) and `isp.shutter_min_us`
    (min exposure floor, µs) to the config schema.  Both default `0` =
    "use the ISP bin's calibrated floor" (no override), symmetric with the
    existing `isp.gain_max` / `isp.shutter_max_us` ceilings.  The
    supervisory cus3a thread writes them into `minSensorGain` /
    `minShutterUs` of the ISP exposure limit; each floor is clamped to not
    exceed its ceiling, and `isp.shutter_rule_180` (min==max pin) overrides
    a manual `shutter_min`.  `MUT_LIVE`, both backends.
  - Added `isp.gainMin` / `isp.shutterMinUs` camelCase aliases.
- `0.12.1` (additive — new config field):
  - Added `isp.shutter_rule_180` (boolean, default `false`) to config
    schema.  When `true`, pins exposure to exactly 1/(2×fps) — sets
    `minShutterUs == maxShutterUs` in the ISP exposure limit so the AE
    shutter is locked while gain still auto-adjusts.  The supervisory
    cus3a thread continuously enforces the pin.  `MUT_RESTART`.  Both
    backends.
  - Added `isp.shutterRule180` camelCase alias (`isp.shutter_rule_180`).
- `0.12.0` (additive — new endpoints):
  - `GET /api/v1/attitude` — live fused attitude snapshot (Star6E only;
    `{"valid":false}` until `attitude.enabled` + `imu.enabled` are on).
  - `GET /api/v1/attitude/calibrate_level` — level-pose boresight
    calibration: averages the level-pose accel (ODR-independent sample
    window), solves and persists `attitude.trimRollDeg`/`trimPitchDeg`,
    returns them with `restartRequired:true`. 409 when the IMU is off or
    gravity is implausible; 501 on Maruko.
  - New `attitude.*` config section (0.39.1/0.40.0): `enabled`,
    `axisFwd`, `axisDown`, `trimRollDeg`, `trimPitchDeg`, `mountDeg`,
    `invertRoll`, `invertPitch` — all restart-required.
- `0.11.0` (breaking — field removed):
  - Removed `video0.frameLost` (and its `frame_lost` canonical / alias). The
    SDK VENC frame-lost strategy it drove is gone: on Star6E (i6e) it never
    fired as a bandwidth throttle — device tests showed a threshold set to ⅛ of
    the CBR target dropped zero frames — and the `pskip` variant returns
    `E_MI_ERR_NOT_SUPPORT`.  It only ever acted as an I-frame overshoot guard
    that the CBR rate controller already covers.  Bandwidth backpressure is
    driven by `video0.bitrate` (smooth) and `video0.fps` (temporal); a client
    that still sends `video0.frameLost`/`frameLostMode`/`frameLostThreshold`/
    `frameLostGap` now gets an unknown-field error instead of a silent accept.
- `0.10.1` (additive, no version bump):
  - Re-exposed `video0.stab_crop_pct` + `video0.stab_recenter_speed`
    (aliases `stabCropPct`/`stabRecenterSpeed`, both `restart_required`) as
    advanced overrides of the `stab` preset.  Read after preset expansion so
    `framing=stab` alone keeps 80/180; explicit values win.  `0`/`0` =
    stick-to-patch (demo).  Inert under `off`/`zoom-*`.
  - `video0.framing` gained `zoom-3x` (1080p → 640×352) and `zoom-4x`
    (480×256) digital-zoom presets.  Additive enum extension; existing
    values unchanged.  Approach-C still shrinks crop+output 1:1, so the
    deep crops are not bound by the SCL ~2× upscale ceiling.
  - `video0.framing` stabilization collapsed to a single `stab` preset
    (was the never-shipped `low`/`medium`/`high`).  Those names are now
    unknown values that fall back to `off` on load; `SET` accepts `stab`.
    There is no settable `zoom_pct`/`zoomPct` — the preset is the only knob.
  - `GET /api/v1/dual/status` always returns `200` now.  When dual VENC
    is not active the body is `{"ok":true,"data":{"active":false}}`
    instead of the previous `404` + `not_active` error envelope.
    `/dual/set` and `/dual/idr` keep the `404` + `not_active` semantics
    — those are write endpoints that need a live ch1 to operate on.
  - Maruko: `/api/v1/dual/{status,idr}` now actually reflect the live
    dual VENC state.  Before this version Maruko started chn 1 when
    `record.mode = "dual"` or `"dual-stream"` but never registered the
    handle with the HTTP API, so all three endpoints returned `404`
    even when dual was running.  Star6E behaviour unchanged.
  - `/api/v1/dual/set` returns `501` on Maruko (was: silent 404).
    Star6E behaviour unchanged.  See "Backend Support Matrix".
- `0.10.0`:
  - Added digital zoom fields: `video0.zoom_pct` (`zoomPct` alias,
    restart-required) plus live pan fields `video0.zoom_x` / `video0.zoom_y`
    (`zoomX` / `zoomY` aliases).
  - Added validation for zoom API writes: `zoom_pct` must be `0.0` or
    `[0.25, 1.0]`; `zoom_x/y` must be finite values in `[0.0, 1.0]`.
  - Updated WebUI-facing field metadata examples for intra refresh and zoom.
  - Corrected the persistence note: accepted `/api/v1/set` writes have been
    persisted since v0.7.8.
- `0.8.4`:
  - `GET /api/v1/record/status` now reflects daemon-config-driven recording
    on Maruko (mirror/dual): previously the response was zero-fill
    (`active:false`, all counters 0) even when a TS file was being written.
    The Maruko runtime now registers a status callback against the same
    `Star6eTsRecorderState` the recorder uses.  No schema change.
  - `GET /api/v1/ae` on Maruko now includes `runtime.active_precrop`,
    matching Star6E.  The precrop was already being reported via
    `/api/v1/config`; only the AE response was missing it.
  - **Internal** (no API surface change): the `/api/v1/record/start|stop`
    501 gate now keys off an explicit
    `venc_api_set_record_http_control_supported(true)` opt-in instead of
    the status-callback presence.  This decoupling is what allowed Maruko
    to add status visibility without accidentally re-enabling the
    HTTP-driven control endpoints (which it still doesn't consume).
- `0.8.3`:
  - Added `GET /api/v1/audio/status` — live observability for the audio
    capture/encode pipeline (lib loaded, capture running, codec, rate,
    channels, Opus encoder available).  Available on both backends; returns
    `501` when the backend has no audio observability hook.
  - `GET /api/v1/record/start` and `GET /api/v1/record/stop` now return
    `501 not_implemented` on backends without a runtime record poll
    (currently only Maruko).  Previously the requests appeared to succeed
    with `{"ok":true}` but did nothing.  Star6E behaviour is unchanged.
  - Documented three pre-existing routes that had landed in code without
    contract entries: `GET /api/v1/modes` (sensor pad/mode introspection),
    `GET /api/v1/transport/status` (output transport observability), and
    `GET /api/v1/idr/stats` (per-channel IDR rate-limit counters).  No
    behavioural change.
  - Added a Backend Support Matrix table covering Star6E vs Maruko
    divergence post-Phase-5 (audio), Phase-6 (recording), Phase-7 (dual
    VENC), and Phase-9 (`isp.aeMode`).
  - In-binary `/api/v1/version` now reports `contract_version=0.8.3`
    (previously the constant was stuck at `0.3.0` while the doc moved
    forward to `0.8.2`).
- `0.8.2`:
  - `outgoing.max_payload_size` is now `MUT_LIVE` (was `MUT_RESTART`) and
    can be batched with other live fields in a single `/api/v1/set` call,
    e.g. `?video0.bitrate=8000&outgoing.maxPayloadSize=4000`.
  - Validation range tightened to `[576, 4000]` (boot will refuse a config
    outside that range).
  - SHM ring slot is sized at startup to fit the validated ceiling
    (4000 + 12 RTP header = 4012 bytes per slot, 8-byte aligned), so
    `shm://` accepts the full live range with no restart-to-grow caveat,
    matching `udp://` and `unix://` behavior. Costs ~1.3 MiB extra SHM
    per ring.
- `0.6.3`:
  - Added `GET /api/v1/recordings` — list files with size/mtime plus
    `free_bytes` / `total_bytes` for the configured `record.dir`.
  - Added `GET /api/v1/recordings/download?file=<name>` — stream a
    recording as an attachment download.
  - Added `GET /api/v1/recordings/delete?file=<name>` — delete a file;
    refuses the currently-active recording with `409 record_active`.
  - New error code `record_active` (409) for actions blocked while
    recording.
  - Browser UI for the above endpoints lives in the `Recordings` tab on
    the dashboard at `/`; there is no separate HTML route.
- `0.6.2`:
  - Added `isp.keepAspect` (boolean, default `true`) to config schema.
    When `false`, VIF captures the full sensor area and VPE scales without
    aspect-ratio cropping (image is stretched if sensor and encode AR
    differ). `MUT_RESTART` — applied on SIGHUP / Save & Restart.
    Star6E only; Maruko reads but ignores the field until SCL crop port
    lands as a follow-up.
  - Added `isp.keepAspect` camelCase alias (`isp.keep_aspect`).
  - `GET /api/v1/config` response gains a `runtime` block with
    `active_precrop` ({x,y,w,h}) — the VIF crop currently programmed
    (includes any sensor overscan offsets).  Omitted when the pipeline
    has not started or after stop.  Available on both backends.
  - `GET /api/v1/ae` Star6E response includes `runtime.active_precrop`
    with the same rectangle.
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
  - Removed `isp.exposure` config field, capability, and Prometheus metric.
    Auto-cap to frame period (1/fps) is now the only exposure mode.
  - Changed `video0.size` default from `"1920x1080"` to `"auto"` (use sensor
    native resolution). Added `"auto"` preset to size parser.
  - Removed `"4MP"` size preset (sensor-specific, not a standard resolution).
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
