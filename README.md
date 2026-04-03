![OpenIPC logo][logo]

# venc — Standalone Video Encoder & Streamer

Standalone H.265/H.264 video encoder and RTP streamer for SigmaStar
Infinity6E (Star6E) and Infinity6C (Maruko) camera SoCs. Designed for
low-latency FPV and IP camera applications with full runtime control
via HTTP API.

## Features

- H.265 (HEVC) and H.264 encoding with CBR/VBR/AVBR/FIXQP rate control
- RTP packetization with adaptive payload sizing
- Compact UDP streaming mode (raw NAL units)
- Built-in web dashboard at `/` for configuration, API docs, and IQ tuning
- HTTP API for live parameter tuning without pipeline restart
- ISP IQ parameter system: 62 params with multi-field structs, export/import
- Custom 3A: built-in AE and AWB with configurable gain limits and convergence
- ISP control: exposure, AWB mode, color temperature
- ROI-based QP gradient for FPV center-priority encoding
- Sensor FPS unlock for IMX415/IMX335 (up to 120fps)
- Optional audio capture with G.711/PCM encoding
- SD card recording: MPEG-TS mux (HEVC + PCM audio), power-loss safe
- Gemini mode: dual VENC for concurrent stream + high-quality record
- Adaptive recording bitrate: auto-reduces if SD card can't keep up
- Dual-backend: Star6E and Maruko from shared codebase
- BMI270 IMU driver with frame-synced FIFO (Star6E only)
- GyroGlide-Lite: gyro-based electronic image stabilization (Star6E only)

## Build

From repository root:

```sh
# Star6E (Infinity6E)
make build SOC_BUILD=star6e

# Maruko (Infinity6C)
make build SOC_BUILD=maruko
```

The toolchain is auto-downloaded on first build. Each backend builds to
its own output directory:

```
out/star6e/venc    # Star6E binary
out/maruko/venc    # Maruko binary
```

Both backends can coexist — no clean needed between them.

Stage a deployable bundle with shared libraries:

```sh
make stage SOC_BUILD=star6e
# Output: out/star6e/venc + out/star6e/lib/*.so
```

Run host tests:

```sh
make test-ci
```

## Deployment

Copy the binary to the target device:

```sh
scp out/star6e/venc root@192.168.2.13:/usr/bin/venc
```

The binary resolves shared libraries from `/usr/lib`. For staged bundles,
set `LD_LIBRARY_PATH` to the lib directory.

## Configuration

venc loads configuration from `/etc/venc.json` on startup. A default
template is provided at `config/venc.default.json`.

```json
{
  "system": { "webPort": 8888, "overclockLevel": 0, "verbose": false },
  "sensor": {
    "index": -1, "mode": -1,
    "unlockEnabled": true, "unlockCmd": 35,
    "unlockReg": 12298, "unlockValue": 128, "unlockDir": 0
  },
  "isp": {
    "sensorBin": "", "exposure": 0,
    "legacyAe": true, "aeFps": 15,
    "awbMode": "auto", "awbCt": 5500
  },
  "image": { "mirror": false, "flip": false, "rotate": 0 },
  "video0": {
    "codec": "h265", "rcMode": "cbr", "fps": 30, "size": "1920x1080",
    "bitrate": 8192, "gopSize": 1.0,
    "qpDelta": -4
  },
  "outgoing": {
    "enabled": false, "server": "", "streamMode": "rtp",
    "maxPayloadSize": 1400,
    "connectedUdp": true, "audioPort": 5601, "sidecarPort": 0
  },
  "fpv": {
    "roiEnabled": true, "roiQp": 0, "roiSteps": 2,
    "roiCenter": 0.25, "noiseLevel": 0
  },
  "audio": {
    "enabled": false, "sampleRate": 16000, "channels": 1,
    "codec": "g711a", "volume": 80, "mute": false
  },
  "imu": {
    "enabled": false, "i2cDevice": "/dev/i2c-1", "i2cAddr": "0x68",
    "sampleRate": 200, "gyroRange": 1000,
    "calFile": "/etc/imu.cal", "calSamples": 400
  },
  "eis": {
    "enabled": false, "marginPercent": 30,
    "gain": 1.0, "deadbandRad": 0.0, "recenterRate": 0.5,
    "testMode": false, "swapXY": false, "invertX": false, "invertY": false
  },
  "record": {
    "enabled": false, "mode": "mirror", "dir": "/mnt/mmcblk0p1",
    "format": "ts", "maxSeconds": 300, "maxMB": 500,
    "bitrate": 0, "fps": 0, "gopSize": 0, "server": ""
  }
}
```

Set `outgoing.enabled` to `true` and `outgoing.server` to
`udp://<receiver_ip>:5600`, `unix://<abstract_name>`, or `shm://<ring_name>`
to start streaming.

## HTTP API

All endpoints use **HTTP GET** (BusyBox wget compatible). The default
port is 8888 (configurable via `system.webPort`). Responses are JSON
with an `{"ok": true/false, ...}` envelope.

### Endpoints

#### GET /api/v1/version

Returns version info.

```sh
curl http://192.168.2.13:8888/api/v1/version
```

```json
{"ok":true,"data":{"app_version":"...","backend":"star6e","contract_version":"0.2.0","config_schema_version":"0.2.0"}}
```

#### GET /api/v1/config

Returns the full active configuration as JSON.

```sh
curl http://192.168.2.13:8888/api/v1/config
```

#### GET /api/v1/capabilities

Returns every field with its mutability (`live` or `restart_required`)
and support status. Use this to discover which fields can be changed
at runtime.

```sh
curl http://192.168.2.13:8888/api/v1/capabilities
```

#### GET /api/v1/get?field_name

Read a single configuration field.

```sh
curl "http://192.168.2.13:8888/api/v1/get?video0.bitrate"
```

```json
{"ok":true,"data":{"field":"video0.bitrate","value":8192}}
```

#### GET /api/v1/set?field_name=value

Write a field. Live fields take effect immediately. Restart-required
fields trigger an automatic pipeline reinit.

```sh
# Live change — immediate
curl "http://192.168.2.13:8888/api/v1/set?video0.bitrate=4096"

# Restart-required — triggers pipeline reinit
curl "http://192.168.2.13:8888/api/v1/set?video0.size=1280x720"
```

```json
{"ok":true,"data":{"field":"video0.bitrate","applied":"live"}}
{"ok":true,"data":{"field":"video0.size","applied":"restart_required"}}
```

Returns HTTP 409 on validation failure (e.g., invalid AWB mode).

#### GET /api/v1/restart

Trigger a full pipeline reinit. Reloads `/etc/venc.json` and restarts
the camera pipeline without exiting the process.

```sh
curl http://192.168.2.13:8888/api/v1/restart
```

#### GET /api/v1/awb

Query current AWB (auto white balance) state from the ISP.

```sh
curl http://192.168.2.13:8888/api/v1/awb
```

#### GET /request/idr

Request an immediate IDR keyframe from the encoder.

```sh
curl http://192.168.2.13:8888/request/idr
```

#### GET /api/v1/record/start

Start SD card recording. Uses the configured `record.dir`, or override
with a `?dir=` query parameter.

```sh
curl "http://192.168.2.13:8888/api/v1/record/start"
curl "http://192.168.2.13:8888/api/v1/record/start?dir=/mnt/mmcblk0p1"
```

#### GET /api/v1/record/stop

Stop SD card recording.

```sh
curl "http://192.168.2.13:8888/api/v1/record/stop"
```

#### GET /api/v1/record/status

Query recording status.

```sh
curl "http://192.168.2.13:8888/api/v1/record/status"
```

```json
{"ok":true,"data":{"active":true,"format":"ts","path":"/mnt/mmcblk0p1/rec_01h23m45s_abcd.ts","frames":1500,"bytes":12345678,"segments":1,"stop_reason":"none"}}
```

#### GET /api/v1/dual/status

Query the secondary VENC channel status (dual/dual-stream modes only).

```sh
curl "http://192.168.2.13:8888/api/v1/dual/status"
```

```json
{"ok":true,"data":{"active":true,"channel":1,"bitrate":20000,"fps":120,"gop":240}}
```

Returns 404 when dual VENC is not active.

#### GET /api/v1/dual/set?param=value

Live-change secondary VENC channel parameters.

```sh
# Change ch1 bitrate
curl "http://192.168.2.13:8888/api/v1/dual/set?bitrate=10000"

# Change ch1 GOP (in seconds)
curl "http://192.168.2.13:8888/api/v1/dual/set?gop=1.0"
```

#### GET /api/v1/dual/idr

Request an IDR keyframe on the secondary VENC channel.

```sh
curl "http://192.168.2.13:8888/api/v1/dual/idr"
```

### Field Reference

Fields marked **live** can be changed at runtime without interrupting
the video stream. Fields marked **restart** trigger a pipeline reinit.

#### System

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `system.web_port` | uint16 | restart | HTTP API port |
| `system.overclock_level` | int | restart | CPU overclock level |
| `system.verbose` | bool | live | Enable verbose logging |

#### Sensor

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `sensor.index` | int | restart | Sensor pad index (-1 = auto) |
| `sensor.mode` | int | restart | Sensor mode (-1 = auto) |
| `sensor.unlock_enabled` | bool | restart | Enable high-FPS sensor unlock |
| `sensor.unlock_cmd` | uint | restart | I2C register write command |
| `sensor.unlock_reg` | uint16 | restart | Unlock register address |
| `sensor.unlock_value` | uint16 | restart | Unlock register value |
| `sensor.unlock_dir` | int | restart | I2C direction flag |

#### ISP

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `isp.sensor_bin` | string | restart | ISP tuning binary path |
| `isp.exposure` | uint | live | Exposure time in ms (0 = auto) |
| `isp.legacy_ae` | bool | restart | Use ISP internal AE instead of custom 3A |
| `isp.ae_fps` | uint | restart | Custom 3A processing rate in Hz (default 15) |
| `isp.awb_mode` | string | live | `"auto"` or `"ct_manual"` |
| `isp.awb_ct` | uint | live | Color temperature in K (for ct_manual) |

#### Image

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `image.mirror` | bool | restart | Horizontal mirror |
| `image.flip` | bool | restart | Vertical flip |
| `image.rotate` | int | restart | Rotation (0, 90, 180, 270) |

#### Video

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `video0.codec` | string | restart | `"h265"` (h264 on Maruko) |
| `video0.rc_mode` | string | restart | `"cbr"`, `"vbr"`, `"avbr"`, `"fixqp"` |
| `video0.fps` | uint | live | Output frame rate |
| `video0.size` | WxH | restart | Encode resolution (e.g., `"1920x1080"`) |
| `video0.bitrate` | uint | live | Target bitrate in kbps |
| `video0.gop_size` | double | live | GOP interval in seconds (0 = all-intra) |
#### Outgoing (Streaming)

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `outgoing.enabled` | bool | live | Enable/disable streaming output |
| `outgoing.server` | string | live | Destination URI (`udp://ip:port`, `unix://name`, or `shm://name`) |
| `outgoing.stream_mode` | string | restart | `"rtp"` or `"compact"` |
| `outgoing.max_payload_size` | uint16 | restart | Max UDP payload bytes |
| `outgoing.connected_udp` | bool | restart | Connect UDP socket (applies only to `udp://`) |
| `outgoing.audio_port` | uint16 | restart | `0` = shared video destination; nonzero = dedicated audio port. With `unix://`, dedicated audio is sent to `127.0.0.1:<audioPort>` |
| `outgoing.sidecar_port` | uint16 | restart | RTP timing sidecar port (0 = disabled) |

`unix://` uses Linux abstract Unix datagram sockets and is available in both
`rtp` and `compact` mode. On Star6E, `audioPort=0` piggybacks on the same
active video destination for both `udp://` and `unix://`. `shm://` remains
RTP-only; it cannot share audio, but a nonzero `audioPort` still uses a
dedicated local UDP audio destination.

#### FPV

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `fpv.roi_enabled` | bool | live | Enable horizontal ROI bands |
| `fpv.roi_qp` | int | live | Signed ROI delta QP (-30..30, negative = sharper center) |
| `fpv.roi_steps` | uint16 | live | Number of horizontal bands (1-4) |
| `fpv.roi_center` | double | live | Center band width ratio (0.1-0.9) |
| `fpv.noise_level` | int | restart | 3DNR noise reduction level |

#### Audio

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `audio.mute` | bool | live | Mute/unmute audio output |

Audio configuration (enabled, sample rate, channels, codec, volume) is
set in `/etc/venc.json` only and requires a process restart to change.

Supported codecs: `"pcm"` (raw 16-bit), `"g711a"` (A-law), `"g711u"` (µ-law).

**RTP payload types:** When streaming in RTP mode, venc uses standard static
payload types when the sample rate matches the RFC 3551 standard:

| Codec | Sample rate | RTP PT | Notes |
|-------|-------------|--------|-------|
| `g711u` | 8000 | 0 (PCMU) | RFC 3551 standard |
| `g711a` | 8000 | 8 (PCMA) | RFC 3551 standard |
| `g711u` | non-8kHz | 112 | Dynamic, Waybeam convention |
| `g711a` | non-8kHz | 113 | Dynamic, Waybeam convention |
| `pcm` | 44100 | 11 (L16 mono) | RFC 3551 standard |
| `pcm` | other | 110 | Dynamic PCM |

Sample rate range: 8000–48000 Hz (clamped by config parser). The
recommended default is 16kHz G.711a for low-latency FPV audio.

**Frame timing:** Each RTP packet contains `sample_rate / 50` samples
(~20ms of audio). The RTP timestamp increments by this value per packet.

#### Recording (Star6E only)

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `record.enabled` | bool | restart | Start recording on launch |
| `record.mode` | string | restart | `"off"`, `"mirror"`, `"dual"`, `"dual-stream"` |
| `record.dir` | string | restart | Output directory (must be mounted) |
| `record.format` | string | restart | `"ts"` (MPEG-TS + audio) or `"hevc"` (raw) |
| `record.max_seconds` | uint | restart | Rotate file after N seconds (0 = off) |
| `record.max_mb` | uint | restart | Rotate file after N MB (0 = off) |
| `record.bitrate` | uint | restart | Dual mode: ch1 bitrate in kbps (0 = same as video0) |
| `record.fps` | uint | restart | Dual mode: ch1 fps (0 = sensor max) |
| `record.gop_size` | double | restart | Dual mode: ch1 GOP in seconds (0 = same as video0) |
| `record.server` | string | restart | Dual-stream: second RTP destination URI |

Recording can also be controlled at runtime via the HTTP API. In dual/dual-stream
modes, the secondary channel parameters can be adjusted live via `/api/v1/dual/set`.

#### IMU (Star6E only, POC)

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `imu.enabled` | bool | restart | Enable BMI270 IMU driver |
| `imu.i2c_device` | string | restart | I2C device path |
| `imu.i2c_addr` | string | restart | I2C address (hex, e.g. `"0x68"`) |
| `imu.sample_rate` | int | restart | ODR in Hz (25-1600) |
| `imu.gyro_range` | int | restart | Gyro range in ±dps |
| `imu.cal_file` | string | restart | Calibration file path |
| `imu.cal_samples` | int | restart | Auto-bias samples at startup |

#### EIS (Star6E only)

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `eis.enabled` | bool | restart | Enable gyro-based image stabilization |
| `eis.mode` | string | restart | EIS backend (default: `"gyroglide"`) |
| `eis.margin_percent` | int | restart | Overscan margin 1-30% (default 30) |
| `eis.gain` | float | restart | Correction gain 0.0-1.0 (default 1.0) |
| `eis.deadband_rad` | float | restart | Per-frame angle threshold in rad (default 0.0) |
| `eis.recenter_rate` | float | restart | Return-to-center speed when idle, 1/s (default 0.5) |
| `eis.max_slew_px` | float | restart | Max crop change per frame in px (0 = off) |
| `eis.bias_alpha` | float | restart | Runtime gyro bias adaptation rate (default 0.001) |
| `eis.test_mode` | bool | restart | Inject sine wobble (no IMU needed) |
| `eis.swap_xy` | bool | restart | Swap gyro X/Y axis mapping |
| `eis.invert_x` | bool | restart | Invert gyro X correction |
| `eis.invert_y` | bool | restart | Invert gyro Y correction |

### Usage Examples

**Start streaming to a receiver:**

```sh
curl "http://192.168.2.13:8888/api/v1/set?outgoing.server=udp://192.168.2.20:5600"
curl "http://192.168.2.13:8888/api/v1/set?outgoing.enabled=true"
```

**Switch to 720p at 90fps with lower bitrate:**

```sh
curl "http://192.168.2.13:8888/api/v1/set?video0.size=1280x720"
curl "http://192.168.2.13:8888/api/v1/set?video0.fps=90"
curl "http://192.168.2.13:8888/api/v1/set?video0.bitrate=4096"
```

**Manual white balance at 6500K:**

```sh
curl "http://192.168.2.13:8888/api/v1/set?isp.awb_mode=ct_manual"
curl "http://192.168.2.13:8888/api/v1/set?isp.awb_ct=6500"
```

**Enable center-priority ROI encoding:**

```sh
curl "http://192.168.2.13:8888/api/v1/set?fpv.roi_enabled=true"
curl "http://192.168.2.13:8888/api/v1/set?fpv.roi_qp=-18"
curl "http://192.168.2.13:8888/api/v1/set?fpv.roi_steps=2"
```

**Request an IDR keyframe (useful after stream start):**

```sh
curl http://192.168.2.13:8888/request/idr
```

**Start/stop SD card recording:**

```sh
# Start recording (MPEG-TS with audio)
curl "http://192.168.2.13:8888/api/v1/record/start"

# Check recording status
curl "http://192.168.2.13:8888/api/v1/record/status"

# Stop recording
curl "http://192.168.2.13:8888/api/v1/record/stop"
```

## SD Card Recording

venc records HEVC video with PCM audio to SD card in MPEG-TS format.
Recording runs concurrently with RTP streaming at minimal CPU overhead
(1-4% additional load measured across 30-120fps at 4-22 Mbps).

Key properties:
- **Power-loss safe** — MPEG-TS requires no finalization; partial files
  are playable up to the last written packet.
- **Gemini mode** — dual VENC channels for independent stream and record
  quality. Stream at 30fps 4 Mbps over WiFi while recording at 120fps
  20 Mbps to SD card. Four modes: off, mirror, dual, dual-stream.
- **Recording thread** — dedicated pthread drains the secondary encoder
  channel at full speed, with adaptive bitrate reduction (10%/s) if the
  SD card can't keep up.
- **File rotation** — splits at IDR keyframe boundaries by time (default
  5 minutes) or size (default 500 MB). Each segment is independently
  playable.
- **Disk safety** — periodic free-space checks with automatic stop when
  below 50 MB. Handles ENOSPC gracefully.
- **Audio interleaving** — raw 16-bit PCM from the hardware audio input
  is muxed alongside HEVC video in the TS container.
- **Live API control** — `/api/v1/dual/set` for runtime bitrate/GOP
  changes on the secondary channel.

Enable in config or use the HTTP API for runtime control. The SD card
must be pre-mounted at the configured directory (OpenIPC auto-mounts to
`/mnt/mmcblk0p1`).

Verify recordings with:

```sh
ffprobe recording.ts          # check streams and format
ffmpeg -i recording.ts -f null -   # full decode test
ffplay recording.ts           # play directly
```

See `documentation/SD_CARD_RECORDING.md` for the full guide including
performance benchmarks, limitations, and architecture details.

## RTP Timing Sidecar

An optional out-of-band UDP channel that sends per-frame timing metadata
alongside the RTP video stream. Disabled by default (`outgoing.sidecarPort=0`).

### Purpose

When enabled, the sidecar provides frame-level diagnostics for the entire
sender-side pipeline:

```
capture_us → [encode] → frame_ready_us → [packetise+send] → last_pkt_send_us
                                                              ↕ (network)
                                                        recv_last_us (probe)
```

This enables measurement of:
- **Encode duration** — time from sensor capture to encoder output
- **Send spread** — time to packetise and hand all RTP packets to the kernel
- **One-way latency** — frame-ready on venc to first-packet-received on ground
  (requires clock synchronisation)
- **Frame intervals** — jitter and regularity of both sender and receiver clocks
- **RTP packet counts and gaps** — per-frame packet accounting

### Enabling

Set the sidecar port in the configuration:

```sh
curl "http://192.168.2.13:8888/api/v1/set?outgoing.sidecar_port=6666"
```

Or in `/etc/venc.json`:

```json
"outgoing": { "sidecarPort": 6666 }
```

A pipeline restart is required after changing this setting. The sidecar
socket is silent until a probe subscribes — zero network overhead when no
probe is connected.

When the sidecar is disabled (port 0), no socket is created and there is
no runtime overhead.

### Wire Protocol

The sidecar uses a simple binary UDP protocol:

| Message | Direction | Size | Purpose |
|---------|-----------|------|---------|
| SUBSCRIBE | probe -> venc | 8 B | Start/refresh metadata subscription |
| FRAME | venc -> probe | 52 B | Per-frame timing + RTP sequence info |
| SYNC_REQ | probe -> venc | 16 B | NTP-style clock offset request |
| SYNC_RESP | venc -> probe | 32 B | Clock offset response (t1, t2, t3) |

All messages share a common 6-byte header: 4-byte magic (`0x52545053` =
"RTPS"), 1-byte version, 1-byte message type. Fields are network byte order.

Subscription expires after 5 seconds without any probe message. Both
SUBSCRIBE and SYNC_REQ refresh the expiry timer.

### Reference Probe

A host-native reference probe is included at `tools/rtp_timing_probe.c`.
It listens for RTP on one port and communicates with the venc sidecar on
another, correlating frames by (SSRC, RTP timestamp).

Build (no cross-compiler needed):

```sh
make rtp_timing_probe
```

Usage:

```sh
./rtp_timing_probe --venc-ip 192.168.2.13 [--rtp-port 5600] [--sidecar-port 6666] [--stats]
```

Without `--stats`, the probe outputs tab-separated frame records to stdout
(one line per frame) suitable for piping to analysis tools. The TSV includes
columns for all timing fields, sequence numbers, gaps, intervals, and
estimated latency.

With `--stats`, a summary is printed to stderr on exit:

```
=== Timing Probe Summary ===

Duration:             20.0 s
Frames:               936 (46.8 fps)
RTP packets:          8484 (9.1 avg/frame)
RTP gaps:             0

--- Send spread (frame_ready -> last_pkt_send) ---
  Mean:    294 us
  P50:     265 us
  P95:     331 us
  P99:     1710 us

--- Encode duration (capture -> frame_ready) ---
  Mean:    4254 us

--- Clock sync ---
  Samples:  8
  Best RTT: 347 us
```

The probe uses burst-then-coast clock synchronisation: 8 fast samples at
200 ms intervals, then one sample every 10 seconds. Only the sample with
the lowest RTT is used for offset estimation.

### Sidecar Overhead

At 90 fps with an active subscriber:
- **venc -> probe**: ~90 frame packets/s (52 B each) + sync responses
- **probe -> venc**: ~0.5 subscribe/s + ~0.1 sync/s
- **Bandwidth**: ~40 kbps total (both directions)
- **CPU**: single `poll()` per frame + one `sendto()` per frame

When no probe is subscribed, the sidecar socket exists but no packets
are sent.

## Sensor Unlock

IMX415 and IMX335 sensors support high-FPS modes (90/120fps) via a
register unlock sequence applied before pipeline initialization. This
is enabled by default (`sensor.unlock_enabled=true`) with preset values
for IMX415.

For different sensors, adjust `sensor.unlock_cmd`, `sensor.unlock_reg`,
and `sensor.unlock_value` in the config file or via the API before a
restart.

See `documentation/SENSOR_UNLOCK_IMX415_IMX335.md` for register details.

## Remote Testing

Build, deploy, and test in one command:

```sh
./scripts/remote_test.sh --help
```

Run the API test suite against a live device:

```sh
./scripts/api_test_suite.sh 192.168.2.13 8888
```

## Web Dashboard

venc includes a built-in web dashboard served at the root URL (`/`). Open
`http://<device-ip>/` in any browser to access it.

### Settings Tab

All 75 configuration fields across 11 sections (System, Sensor, ISP, Image,
Video, Outgoing, Audio, FPV, IMU, EIS, Recording) with:

- **Collapsible sections** — start collapsed for a clean overview
- **Live/Restart badges** — green for immediate changes, orange for restart-required
- **Tooltips** — hover any field label for a description
- **Change tracking** — modified fields highlighted; Apply only sends changes
- **Apply Changes** — applies all modified fields via the API
- **Save & Restart** — applies changes then triggers pipeline reinit
- **Restore Defaults** — reloads on-disk config and resets the form

### API Reference Tab

Documentation for all HTTP endpoints with descriptions and example responses,
grouped by category: Configuration, Encoder Control, ISP & Image Quality,
Recording, and Dual-Stream.

### Image Quality Tab

Direct access to 62 SigmaStar ISP parameters organized by category.

**Parameter Categories** — expandable sections with clickable parameter chips.
Multi-field parameters (colortrans, obc, demosaic, false_color, crosstalk,
r2y, wdr_curve_adv) show sub-field chips for individual field access.

**Expanded Editor** — click a multi-field parameter to open an inline form
with all sub-fields pre-filled from live ISP values. Array fields (e.g.,
colortrans 3x3 matrix) render as editable grids. Changed fields highlight
and Apply All writes only the modified fields.

**Export / Import** — save all IQ parameters as a timestamped JSON file,
or import a previously saved file to restore tuning. Partial imports are
supported — only the parameters present in the JSON are applied, leaving
others untouched.

```sh
# Export current IQ state
curl http://<device>/api/v1/iq > my_tuning.json

# Import (full or partial)
curl -X POST -H "Content-Type: application/json" \
  -d @my_tuning.json http://<device>/api/v1/iq/import

# Partial import example — only set specific params
echo '{"lightness":{"value":75},"demosaic":{"fields":{"dir_thrd":30}}}' | \
  curl -X POST -H "Content-Type: application/json" -d @- http://<device>/api/v1/iq/import
```

### IQ Dot-Notation API

Multi-field parameters support dot-notation for individual field access:

```sh
# Set a single field
curl "http://<device>/api/v1/iq/set?colortrans.y_ofst=200"

# Set an array field (comma-separated)
curl "http://<device>/api/v1/iq/set?colortrans.matrix=23,45,9,1005,987,56,56,977,1015"

# Query shows all fields
curl http://<device>/api/v1/iq
# Returns: "colortrans":{"enabled":true,"value":200,"fields":{"y_ofst":200,"u_ofst":0,"v_ofst":0,"matrix":[23,45,...]}}
```

Legacy single-value set (`?colortrans=200`) still works for backward compatibility.

### Status Bar

The top telemetry bar shows version, backend type, live FPS (auto-refreshes
every 2s), recording status indicator, and an Export Config button to
download the full configuration as JSON.

## GyroGlide-Lite: Gyro-Based Image Stabilization

GyroGlide-Lite is a low-latency 2-axis electronic image stabilization
system that uses the BMI270 gyroscope to compensate for camera shake via
per-frame crop-window shifting. It runs entirely on the SigmaStar VPE
hardware scaler with negligible CPU overhead.

### How it works

1. The BMI270 IMU samples gyro data at 200 Hz via hardware FIFO
2. Each video frame, the FIFO is drained and gyro samples are integrated
   over the frame interval using per-sample timestamps
3. The integrated angular displacement is converted to a pixel offset
4. `MI_VPE_SetPortCrop()` shifts the crop window to cancel the motion
5. VPE upscales the cropped region back to the output resolution

The crop window returns to center only when the camera is stationary,
preventing the recenter from fighting active corrections.

### Quick start

Add to `/etc/venc.json`:

```json
{
  "imu": {
    "enabled": true,
    "i2cDevice": "/dev/i2c-1",
    "i2cAddr": "0x68",
    "sampleRate": 200,
    "gyroRange": 1000,
    "calSamples": 400
  },
  "eis": {
    "enabled": true,
    "mode": "gyroglide",
    "marginPercent": 30
  }
}
```

Restart venc. Hold the board still during the 2-second IMU auto-calibration
at startup.

### Recommended settings

- **FPS: 60 or lower.** At 120fps the per-frame integration window is too
  short for effective correction. At 60fps the sensor selects a higher-res
  mode (2560x1920) giving more headroom.
- **Margin: 30% (default, maximum safe).** This gives ±288px horizontal and
  ±162px vertical correction range from a 1344x756 crop within 1920x1080.
  Values above 30% can stall the VPE pipeline and are automatically clamped.
- **Gain: 1.0** for full correction. Reduce to 0.5-0.8 if overcorrecting.
- **Deadband: 0.0** recommended. The motion-gated recenter handles bias
  drift when the camera is stationary.

### Test mode

To verify EIS is working without an IMU connected:

```json
"eis": { "enabled": true, "testMode": true }
```

This injects a visible sine-wave wobble into the crop window.

### Axis calibration

If the stabilization moves the wrong direction, use the axis flags:

```json
"eis": { "swapXY": true, "invertX": false, "invertY": true }
```

The correct mapping depends on how the IMU is mounted relative to the
camera sensor.

### Architecture

The EIS system uses a modular dispatch framework. The **gyroglide** backend
provides timestamp-based integration, motion-gated recenter, edge-aware
recentering, and optional slew limiting. New backends can be added by
implementing the `EisOps` vtable in `eis.h`.

### Limitations

- **Translation only** — cannot correct roll (rotation around optical axis)
- **30% max margin** — VPE hardware limit when scaling is active
- **Resolution loss** — 30% overscan means the effective output is cropped
  from a 1344x756 window (upscaled to output resolution)
- **No rolling-shutter correction** — CMOS line-by-line readout skew is
  not addressed by crop-window shifting

See `documentation/GYROGLIDE_LITE_DESIGN.md` for the full design document
and `documentation/EIS_INTEGRATION_PLAN.md` for the implementation roadmap.

[logo]: https://openipc.org/assets/openipc-logo-black.svg
