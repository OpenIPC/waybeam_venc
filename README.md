<p align="center">
  <img src="docs/assets/waybeam_logo.png" alt="Waybeam" width="420">
</p>

<h1 align="center">Waybeam — Vehicle Video Encoder</h1>

<p align="center">
  <em>Standalone H.265 (HEVC) encoder &amp; RTP streamer for SigmaStar Infinity6E and Infinity6C camera SoCs.</em>
</p>

---

Waybeam is the camera-side daemon for the Waybeam FPV ecosystem. It owns
the ISP, sensor, and VENC channel on the vehicle, captures audio, streams
RTP / compact UDP / Unix / SHM video to a ground station, optionally
records to SD card, and exposes the whole pipeline through a single
zero-restart HTTP API and a built-in web dashboard.

Two SoC backends share one source tree:

- **Star6E** — SigmaStar Infinity6E (SSC30KQ, SSC338Q).
- **Maruko** — SigmaStar Infinity6C (SSC378QE).

Both binaries are produced from the same `make build` invocation with
different `SOC_BUILD=` flags. All MI vendor libraries are loaded via
`dlopen` so the binary stays small and the Maruko bundle can ship its
own copies of libs that stock OpenIPC Infinity6C firmware does not.

> **Note on naming.** The product, binary, config file, init script,
> and release tarball are all named `waybeam`. The GitHub repository
> is still `waybeam_venc` for historical URL stability — that is the
> only place the old name survives.

## Features

- H.265 (HEVC) encoding with CBR / VBR / AVBR / FIXQP rate control
- RTP packetization with adaptive payload sizing; compact UDP raw-NAL mode
- Built-in web dashboard at `/` for configuration, API docs, and IQ tuning
- HTTP API for live parameter tuning without pipeline restart
- ISP IQ parameter system: 60+ params, multi-field structs, JSON export/import (both backends)
- Custom 3A: built-in AE and AWB with configurable gain limits and convergence
- ROI-based QP gradient for FPV center-priority encoding
- Sensor FPS unlock for IMX415 / IMX335 (up to 120 fps)
- Optional audio capture (Opus / G.711a / G.711µ / raw PCM) on both
  backends, RTP or compact UDP output, mute via live API
- SD card recording: MPEG-TS mux (HEVC + audio in TS, PCM / A-law / µ-law / Opus
  alongside video), power-loss safe; raw `.hevc` available on Star6E
- Gemini / dual-VENC: concurrent stream + high-quality record (both backends)
- Adaptive recording bitrate: auto-reduces if SD card can't keep up
- Maruko-specific opt-in 3A throttle (`isp.aeEngine="custom"`) — saves
  ~24 % sys CPU at 120 fps with no visible AE quality loss
- BMI270 IMU driver with frame-synced FIFO (both backends) — compiled in,
  disabled by default, ready for telemetry/sidecar consumers
- Intra-refresh (GDR-style rolling stripe) for fast loss recovery on FPV links
- Scene-change-triggered IDR (Star6E) for clean stream join under packet loss

## Build

From the repo root:

```sh
# Star6E (Infinity6E)
make build SOC_BUILD=star6e

# Maruko (Infinity6C)
make build SOC_BUILD=maruko
```

The toolchain is auto-downloaded on first build. Each backend builds to
its own output directory:

```
out/star6e/waybeam   # Star6E binary
out/maruko/waybeam   # Maruko binary
```

Both backends can coexist; no clean is needed when switching.

Stage a deployable bundle with vendored libraries:

```sh
make stage SOC_BUILD=star6e
# Output: out/star6e/waybeam + out/star6e/lib/*.so (Maruko also stages drivers/ + isp-bins/)
```

Run host tests:

```sh
make test-ci
```

## Deployment

### Star6E (Infinity6E)

Copy the binary to the target device:

```sh
scp out/star6e/waybeam root@<device-ip>:/usr/bin/waybeam
```

For the current Star6E bench workflow, prefer the helper — it stops
any running daemon, deploys `/usr/bin/waybeam` and
`/etc/init.d/S95waybeam`, backs up `/etc/waybeam.json`, then starts
the daemon:

```sh
scripts/star6e_direct_deploy.sh cycle
```

### Maruko (Infinity6C)

Maruko devices need more than just the binary because stock OpenIPC
Infinity6C firmware does **not** ship MI vendor libraries, and bench
devices also need matching sensor `.ko` modules and ISP `.bin` tuning
blobs.

The repo carries everything needed for a fresh deployment once a known-good
device has been mirrored locally. Pre-verified copies of the sensor `.ko`
modules and ISP `.bin` blobs are vendored under `sensors/maruko/` and
`iq-profiles/maruko-bin/`:

| Repo location | Target path | Source |
|---|---|---|
| `vendor-libs/maruko/*.so`     | `/usr/lib/`                           | pulled from device, vendored |
| `sensors/maruko/sensor_imx*_maruko.ko` | `/lib/modules/5.10.61/sigmastar/sensor_imx*_mipi.ko` | source-built via `make drivers-maruko`, vendored (staged → `_mipi.ko`) |
| `iq-profiles/maruko-bin/*.bin`| `/etc/sensors/`                       | pulled from device |
| `out/maruko/waybeam`          | `/usr/bin/waybeam`                    | `make build SOC_BUILD=maruko` |
| `out/maruko/json_cli`         | `/usr/bin/json_cli`                   | `make json_cli SOC_BUILD=maruko` (vendored from `waybeam-hub/tools/`) |

`push-libs` also creates two uClibc compat symlinks on the target —
`/lib/ld-uClibc.so.1` and `/lib/libc.so.0`, both pointing to
`/lib/libc.so`. The vendor blob `libcam_os_wrapper.so` has hardcoded
NEEDED tags for these two names; stock OpenIPC musl firmware only
ships `libc.so`, so a fresh firstboot device would otherwise segfault
on first start.

If you provision a device by hand instead of through `push-libs`, run
this on the target once (idempotent):

```sh
ssh root@<device-ip> '
    ln -sf libc.so /lib/ld-uClibc.so.1
    ln -sf libc.so /lib/libc.so.0
'
```

`json_cli` is required by `config-get` / `config-set` / `status` in the
deploy script — `maruko-full` (and `cycle --with-json-cli`) installs it
automatically.

One-time: mirror the working bench (`192.168.2.12` by default) into the repo:

```sh
make maruko-pull HOST=root@192.168.2.12
# or with finer control:
scripts/maruko_pull_artifacts.sh libs drivers isp-bins info
git status   # review and commit the cache that landed
```

Routine iteration (binary only):

```sh
make maruko-deploy HOST=root@<device-ip>
# = scripts/maruko_direct_deploy.sh cycle
```

Fresh-device bring-up (binary + libs + uClibc symlinks + json_cli +
drivers + ISP bins, drivers reboot):

```sh
make maruko-full HOST=root@<device-ip>
# = scripts/maruko_direct_deploy.sh full
```

Selective pushes during debugging:

```sh
scripts/maruko_direct_deploy.sh push-libs           # libs + uClibc symlinks
scripts/maruko_direct_deploy.sh push-json-cli       # /usr/bin/json_cli
scripts/maruko_direct_deploy.sh push-drivers --reboot-after
scripts/maruko_direct_deploy.sh push-isp-bin imx415
```

### Building Maruko sensor drivers from source

`drivers/sensor_imx{335,415}_maruko.c` needs the Infinity6C 5.10.61 kernel
source tree. The tree is part of the SigmaStar BSP and is not hosted by
this repo, so you must supply it on the command line:

```sh
make drivers-maruko KSRC_MARUKO=/path/to/infinity6c-kernel
```

`make drivers-maruko` without `KSRC_MARUKO` fails with a clear error — it
does not auto-download the kernel. If you do not have the kernel source,
fall back to the prebuilt `.ko` pulled by `make maruko-pull` from a
known-good device.

## Configuration

`waybeam` loads its configuration from a single fixed path on startup:

```
/etc/waybeam.json
```

There is no `-c` flag and no command-line override. If the file is
absent the binary boots with compiled-in defaults and prints a notice
to stderr; the HTTP API is still available and `/api/v1/restart`
re-reads the file once it has been written.

Default templates live in the repo:

| Backend | Template path |
|---|---|
| Star6E (Infinity6E) | `config/waybeam.default.json` |
| Maruko (Infinity6C) | `config/waybeam.default.maruko.json` |

The release tarballs ship the matching template as `waybeam.json`
inside `waybeam-<backend>.tar.gz`; copy it to `/etc/waybeam.json` on
first install.

### Schema

Every section in the template is shown below. All fields are optional —
omitted fields keep their compiled-in defaults.

```json
{
  "system":   { "webPort": 80, "overclockLevel": 0, "verbose": false },
  "sensor":   { "index": -1, "mode": -1 },
  "isp":      {
    "sensorBin": "",
    "aeEngine": "sdk", "aeFps": 15,
    "gainMax": 0,
    "awbMode": "auto", "awbCt": 5500,
    "keepAspect": true
  },
  "image":    { "mirror": false, "flip": false, "rotate": 0 },
  "video0":   {
    "rcMode": "cbr", "fps": 30,
    "bitrate": 8192, "gopSize": 1.0,
    "qpDelta": -4,
    "sceneThreshold": 0, "sceneHoldoff": 2,
    "resilience": "off",
    "zoomPct": 0.0, "zoomX": 0.5, "zoomY": 0.5
  },
  "outgoing": {
    "enabled": false, "server": "", "streamMode": "rtp",
    "maxPayloadSize": 1400,
    "connectedUdp": true, "audioPort": 5601, "sidecarPort": 5602
  },
  "fpv":      {
    "roiEnabled": true, "roiQp": 0, "roiSteps": 2,
    "roiCenter": 0.25, "noiseLevel": 0
  },
  "audio":    {
    "enabled": false, "sampleRate": 16000, "channels": 1,
    "codec": "g711a", "volume": 80, "mute": false
  },
  "imu":      {
    "enabled": false, "i2cDevice": "/dev/i2c-1", "i2cAddr": "0x68",
    "sampleRateHz": 200, "gyroRangeDps": 1000,
    "calFile": "/etc/imu.cal", "calSamples": 400
  },
  "record":   {
    "enabled": false, "mode": "mirror", "dir": "/mnt/mmcblk0p1",
    "format": "ts", "maxSeconds": 300, "maxMB": 500,
    "bitrate": 0, "fps": 0, "gopSize": 0, "server": ""
  },
  "snapshot": {
    "enabled": true, "quality": 80, "channel": 7,
    "width": 0, "height": 0
  },
  "debug":    { "showOsd": false }
}
```

### Section reference

- **`system`** — HTTP API port, CPU overclock level, verbose logging
  toggle.
- **`sensor`** — pad/mode selection (-1 = auto).
- **`isp`** — ISP tuning bin path, AE engine selector
  (`aeEngine="sdk"` lets the SDK firmware run AE, `"custom"` runs
  userspace cus3a; on Maruko `custom` additionally installs the no-op
  adaptor + supervisory thread for the CPU win), AE rate, gain
  ceiling, AWB mode, aspect-preserving crop.
- **`image`** — mirror / flip / rotate.
- **`video0`** — rate control, fps, resolution, bitrate, GOP,
  per-section QP delta. Video codec is hardcoded H.265 (HEVC).
  Scene-change-triggered IDR (`sceneThreshold`,
  `sceneHoldoff`) is Star6E-only. Intra-refresh and digital zoom are
  both backends.
- **`outgoing`** — destination URI (`udp://`, `unix://`, `shm://`),
  stream mode (`rtp` / `compact`), payload sizing, optional dedicated
  audio + sidecar UDP ports.
- **`fpv`** — center-priority ROI bands + 3DNR level.
- **`audio`** — `enabled`, sample rate, channels, codec, software
  volume, live-mutable mute. Supports `pcm`, `g711a`, `g711u`, `opus`.
- **`imu`** — BMI270 driver (disabled by default).
- **`record`** — SD card recorder. `mode` is `off` / `mirror` /
  `dual` / `dual-stream`; format is `ts` or `hevc` (Star6E only).
- **`snapshot`** — JPEG snapshot channel served at
  `/api/v1/snapshot.jpg`. `quality` is live-mutable; `channel`,
  `width`, `height` are restart-required because they are baked at
  `MI_VENC_CreateChn` time. `width=0` and `height=0` mean "match the
  active main stream".
- **`debug`** — overlay extra OSD rows (zoom, intra-refresh state,
  recording status) on the encoded video.

### Starting a stream

Set `outgoing.enabled` to `true` and `outgoing.server` to
`udp://<receiver_ip>:5600`, `unix://<abstract_name>`, or
`shm://<ring_name>`.

## HTTP API

All endpoints use **HTTP GET** (BusyBox `wget` compatible). The default
port is 80 (configurable via `system.webPort`). Responses are JSON with
an `{"ok": true/false, ...}` envelope.

### Endpoints

#### GET /api/v1/snapshot.jpg

Returns one JPEG frame from a dedicated MJPEG VENC channel tapped off
the same VPE/SCL output port the main H.265 stream uses. No
parameters; quality defaults to 80, resolution matches the main stream.
Captures are serialized through a module mutex (concurrent clients
queue rather than collide), and the channel is created at pipeline
start so each request only pays the StartRecvPic → GetStream round
trip (~50–150 ms typical).

```sh
curl -o snapshot.jpg http://<device-ip>:<port>/api/v1/snapshot.jpg
```

Response is `Content-Type: image/jpeg`. Failure modes:

- **503 snapshot_disabled** — subsystem not initialised (pipeline not
  up yet, or backend MJPEG channel-create failed during init)
- **504 snapshot_timeout** — channel ran but no frame landed within
  1500 ms (upstream stalled)
- **500 snapshot_failed** — SDK GetStream or memory allocation error

Defaults live in `waybeam.json` under `snapshot` (`enabled`, `quality`,
`channel`, `width`, `height`). `snapshot.quality` is **live-mutable**
on both backends — `curl "http://<dev>/api/v1/set?snapshot.quality=40"`
applies instantly with no pipeline reinit. The remaining snapshot
fields are restart-required (channel-attribute baked at
`MI_VENC_CreateChn` time).

#### GET /api/v1/version

Returns version info.

```sh
curl http://<device-ip>:<port>/api/v1/version
```

```json
{"ok":true,"data":{"app_version":"...","backend":"star6e","contract_version":"0.2.0","config_schema_version":"0.2.0"}}
```

#### GET /api/v1/config

Returns the full active configuration as JSON.

```sh
curl http://<device-ip>:<port>/api/v1/config
```

#### GET /api/v1/capabilities

Returns every field with its mutability (`live` or `restart_required`)
and support status. Support is backend-specific; for example, Star6E
reports `video0.scene_threshold` / `video0.scene_holdoff` as supported,
while Maruko reports them as unsupported. Use this to discover which
fields can be changed at runtime.

```sh
curl http://<device-ip>:<port>/api/v1/capabilities
```

#### GET /api/v1/get?field_name

Read a single configuration field.

```sh
curl "http://<device-ip>:<port>/api/v1/get?video0.bitrate"
```

```json
{"ok":true,"data":{"field":"video0.bitrate","value":8192}}
```

#### GET /api/v1/set?field_name=value

Write a field. Live fields take effect immediately. Restart-required
fields trigger an automatic pipeline reinit.

```sh
# Live change — immediate
curl "http://<device-ip>:<port>/api/v1/set?video0.bitrate=4096"

# Live multi-set — all fields must be live
curl "http://<device-ip>:<port>/api/v1/set?video0.bitrate=4096&system.verbose=true"

# Restart-required — triggers pipeline reinit
curl "http://<device-ip>:<port>/api/v1/set?video0.size=1280x720"
```

```json
{"ok":true,"data":{"field":"video0.bitrate","value":4096}}
{"ok":true,"data":{"applied":[{"field":"video0.bitrate","value":4096},{"field":"system.verbose","value":true}]}}
{"ok":true,"data":{"field":"video0.size","value":"1280x720","reinit_pending":true}}
```

Multi-set is supported only for live fields. If any restart-required field
is present, the full request is rejected and restart/reinit changes must be
sent one at a time.

Returns HTTP 409 on validation failure (e.g., invalid AWB mode).

#### GET /api/v1/restart

Trigger a full pipeline reinit. Reloads `/etc/waybeam.json` and
restarts the camera pipeline without exiting the process.

```sh
curl http://<device-ip>:<port>/api/v1/restart
```

#### GET /api/v1/awb

Query current AWB (auto white balance) state from the ISP.

```sh
curl http://<device-ip>:<port>/api/v1/awb
```

#### GET /request/idr

Request an IDR keyframe from the encoder.

```sh
curl http://<device-ip>:<port>/request/idr
```

#### GET /api/v1/record/start

Start SD card recording. Uses the configured `record.dir`, or override
with a `?dir=` query parameter.

```sh
curl "http://<device-ip>:<port>/api/v1/record/start"
curl "http://<device-ip>:<port>/api/v1/record/start?dir=/mnt/mmcblk0p1"
```

#### GET /api/v1/record/stop

Stop SD card recording.

```sh
curl "http://<device-ip>:<port>/api/v1/record/stop"
```

#### GET /api/v1/record/status

Query recording status.

```sh
curl "http://<device-ip>:<port>/api/v1/record/status"
```

```json
{"ok":true,"data":{"active":true,"format":"ts","path":"/mnt/mmcblk0p1/rec_01h23m45s_abcd.ts","frames":1500,"bytes":12345678,"segments":1,"stop_reason":"none"}}
```

#### GET /api/v1/dual/status

Query the secondary VENC channel status (dual/dual-stream modes only).

```sh
curl "http://<device-ip>:<port>/api/v1/dual/status"
```

```json
{"ok":true,"data":{"active":true,"channel":1,"bitrate":20000,"fps":120,"gop":240}}
```

Returns 404 when dual VENC is not active.

#### GET /api/v1/dual/set?param=value

Live-change secondary VENC channel parameters.

```sh
# Change ch1 bitrate
curl "http://<device-ip>:<port>/api/v1/dual/set?bitrate=10000"

# Change ch1 GOP (in seconds)
curl "http://<device-ip>:<port>/api/v1/dual/set?gop=1.0"
```

#### GET /api/v1/dual/idr

Request an IDR keyframe on the secondary VENC channel.

```sh
curl "http://<device-ip>:<port>/api/v1/dual/idr"
```

#### GET /api/v1/audio/status

Live snapshot of the audio capture/encode pipeline (lib loaded, capture
running, codec, rate, channels, Opus initialization). Both backends.
See [HTTP_API_CONTRACT.md](documentation/HTTP_API_CONTRACT.md) for full
field reference.

```sh
curl http://<device-ip>:<port>/api/v1/audio/status
```

#### GET /api/v1/transport/status

Live observability for the active video transport (UDP / Unix / SHM):
fill percentage, backpressure flag, lifetime drop counters. Used by the
WebUI status bar and external link controllers.

```sh
curl http://<device-ip>:<port>/api/v1/transport/status
```

#### GET /api/v1/idr/stats

Per-channel IDR-rate-limit counters: how many requests were honored vs.
coalesced.

```sh
curl http://<device-ip>:<port>/api/v1/idr/stats
```

#### GET /api/v1/modes

Sensor pad and resolution mode introspection — populates the WebUI
sensor-mode dropdown. Reports the currently-active selection plus every
mode the SDK enumerates.

```sh
curl http://<device-ip>:<port>/api/v1/modes
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

The legacy `sensor.unlock_*` register-hook fields were retired in
0.10.13 — the OpenIPC kernel sensor drivers and the per-mode ISP
binaries now write the high-FPS unlock registers themselves, so the
userspace pre-hook is redundant.  Existing configs containing
`unlockEnabled`/`unlockCmd`/`unlockReg`/`unlockValue`/`unlockDir`
load cleanly; the keys are silently ignored.

#### ISP

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `isp.sensor_bin` | string | live | ISP tuning binary path (empty = auto-detect /etc/sensors/&lt;sensor&gt;.bin) |
| `isp.ae_engine` | string | restart | `"sdk"` (default) lets the SDK firmware run AE on both backends.  `"custom"` runs userspace cus3a — on Star6E it spins the supervisory AE thread; on Maruko it installs the no-op adaptor + 15 Hz `SetAeParam` thread (~24% sys CPU saving at 120 fps).  Alias: `isp.aeEngine`. |
| `isp.ae_fps` | uint | restart | Custom 3A processing rate in Hz (default 15) |
| `isp.gain_max` | uint | live | AE max ISP gain ceiling (0 = use ISP bin default) |
| `isp.awb_mode` | string | live | `"auto"` or `"ct_manual"` |
| `isp.awb_ct` | uint | live | Color temperature in K (for ct_manual) |
| `isp.keep_aspect` | bool | restart | When `true` (default), VIF/SCL crop preserves sensor AR; `false` lets downstream stretch. Star6E + Maruko (Phase 1, v0.9.9). |

#### Image

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `image.mirror` | bool | restart | Horizontal mirror |
| `image.flip` | bool | restart | Vertical flip |
| `image.rotate` | int | restart | Rotation (0, 90, 180, 270) |

#### Video

Video codec is hardcoded H.265 (HEVC) — there is no `video0.codec`
field. Existing configs containing `"codec": "h264"` or `"h265"` load
cleanly; the key is silently ignored.

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `video0.rc_mode` | string | restart | `"cbr"`, `"vbr"`, `"avbr"`, `"fixqp"` |
| `video0.fps` | uint | live | Output frame rate |
| `video0.size` | string | restart | Encode resolution: `"auto"` (default, uses sensor native), `"1920x1080"`, `"720p"`, `"1080p"` |
| `video0.bitrate` | uint | live | Target bitrate in kbps |
| `video0.gop_size` | double | live | GOP interval in seconds (0 = all-intra) |
| `video0.qp_delta` | int | live | Relative I/P QP delta (-12..12) |
| `video0.frame_lost` | bool | restart | Enable frame-lost safety net |
| `video0.zoom_pct` | double | restart | Digital zoom crop fraction (`0.0` = off, `0.25..1.0` = crop fraction) |
| `video0.zoom_x` | double | live | Zoom crop center X (`0.0` left to `1.0` right) |
| `video0.zoom_y` | double | live | Zoom crop center Y (`0.0` top to `1.0` bottom) |

#### Digital Zoom (Star6E + Maruko)

Approach-C digital zoom shrinks both the crop window and encoded output
resolution. The SCL path reads the crop at 1:1 and emits it unchanged, so
there is no upscale pass and no extra bandwidth pressure. Receivers see the
smaller resolution in SPS/PPS.

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `video0.zoom_pct` | double | restart | `0.0` = off/full frame; `0.25..1.0` = crop fraction (smaller = deeper zoom) |
| `video0.zoom_x` | double | live | Crop center X, `0.0` = left, `1.0` = right |
| `video0.zoom_y` | double | live | Crop center Y, `0.0` = top, `1.0` = bottom |

CamelCase aliases: `video0.zoomPct`, `video0.zoomX`, `video0.zoomY`.

Examples:

```bash
# Restart-required: enable a 2x crop.
curl "http://<device>/api/v1/set?video0.zoomPct=0.5"

# Live pan inside the current crop size.
curl "http://<device>/api/v1/set?video0.zoomX=0.25&video0.zoomY=0.75"

# Disable zoom on the next reinit.
curl "http://<device>/api/v1/set?video0.zoomPct=0.0"
```

When `debug.showOsd=true` and zoom is active, the overlay adds rows
after existing OSD stats:

```
zoom  2.00x 960x540
crop  960x540+480+270
```

#### Adaptive Encoder Control (Star6E + Maruko)

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `video0.scene_threshold` | uint16 | restart | Scene spike threshold ratio x100 (0=off, 150=1.5x EMA spike detection) |
| `video0.scene_holdoff` | uint8 | restart | Consecutive spike frames required (default 2) |

CamelCase aliases: `video0.sceneThreshold`, `video0.sceneHoldoff`.

When `scene_threshold` is non-zero, the inline scene detector tracks frame
size EMA, computes complexity, and requests an IDR after a spike above the
threshold settles. Use `/api/v1/capabilities` to check backend support
before writing these fields.

Typical usage:
- Leave `video0.scene_threshold=0` for fixed-GOP behavior controlled by
  `video0.gop_size`.
- Set `video0.scene_threshold=150` for FPV/live links where
  scene-change-triggered IDRs improve stream recovery.
- Pair scene detection with `outgoing.sidecar_port>0` when an external
  controller needs per-frame `frame_type`, `complexity`, `scene_change`,
  `idr_inserted`, and `frames_since_idr` telemetry on the sidecar.

#### Resilience preset (Star6E + Maruko)

A single field picks an error-resilience profile.  Intra-refresh
(rolling GDR stripe), the SVC-T reference pyramid (refPred), and the GOP
length are all derived from the preset — no per-feature knobs.

**The two axes that matter:**

1. **Stripe-only recovery** — can a damaged frame buffer be cleaned up
   by intra-refresh stripes alone, without waiting for an IDR?
2. **OSD-safe** — does the preset leave persistent chroma artefacts
   ("green smear") over static high-contrast overlays like an OSD
   panel?  The two are linked: any preset with `ref_enhance > 0`
   (SVC-T temporal hierarchy) marks enhancement frames as TRAIL_N, so
   their intra-refresh stripes are display-only and never propagate
   into the decoder's reference state.  For motion-rich pixels this
   doesn't matter — opportunistic intra coding scrubs the DPB
   anyway.  For static OSD content the chroma plane stays in
   skip-mode-from-stale-reference and you get the green smear until
   the next IDR.

|                            | **OSD-safe** (no SVC-T)                      | **OSD-unsafe** (uses SVC-T → refPred)         |
|----------------------------|----------------------------------------------|-----------------------------------------------|
| **Ultra-low recovery**     | `rescue` — IDR-spam, no intra-refresh       | —                                             |
| **Very fast recovery**     | `sprint` — close-range + plenty of bitrate  | —                                             |
| **Fast recovery needed**   | `racing` — close-range LOS                   | `rally` — light refPred, motion-heavy scenes  |
| **Recovery time tradable** | `endurance` — balanced wavefront, less bitrate | `range` — long-range FPV (heavy refPred)    |
| **Long stable flight**     | `patrol` — balanced + 4 s GOP                | `fpv` — drone FPV (heaviest refPred)          |
| **Slow recovery OK**       | `quality` — plane / cruiser (IDR-based)      | —                                             |

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `video0.resilience` | string | **reboot** | `off` \| `rescue` \| `quality` \| `sprint` \| `racing` \| `endurance` \| `patrol` \| `rally` \| `range` \| `fpv` (default `off`) |
| `video0.gopSize`    | double | restart | Seconds between IDRs.  Honoured **only** when `resilience: "off"`; named presets override it.  Live-reinit applies (no reboot). |

> ⚠️  **Resilience changes require a reboot on both Star6E and Maruko.**
> Setting `video0.resilience` (or any of the derived fields
> `intra_refresh_*` or `ref_*`) persists the new value to
> `/etc/waybeam.json` and returns `{"reboot_required": true}`.  The
> live encoder pipeline keeps running the previous preset until the
> next daemon start.
>
> The SigmaStar MI SDK kernel module does not survive a live
> re-configure of these fields.  Empirically confirmed on both
> backends (2026-05-15 bench testing):
>
> - **Star6E (Infinity6E)** — fork+exec respawn for the new config
>   triggers an SoC kernel panic within 1–2 transitions; ICMP dies,
>   requires a power-cycle.
> - **Maruko (Infinity6C)** — in-process pipeline reinit completes
>   cleanly for most transitions, but one in a sweep of 7 zombied
>   the daemon via a page fault in `MI_SYS_IMPL_FlushInputPortTasks`
>   inside the `mi` kernel module.  System stays up but waybeam dies
>   and does not respawn — reboot is required to restart it.
>
> Different failure modes, same root cause.  Cold-boot into any
> preset is 100 % reliable on both backends, so the reboot model is
> what we ship.

Expansion table:

| Preset      | intra-refresh    | refPred (base/enhance) | gopSize override | OSD-safe?         |
|-------------|------------------|------------------------|------------------|-------------------|
| `off`       | off              | off                    | user-set         | yes (no refresh)  |
| `rescue`    | off              | off                    | **0.25 s**       | yes (IDR-spam)    |
| `quality`   | off              | off                    | 4.0 s            | yes (IDR-based)   |
| `sprint`    | fast (150 ms)    | off                    | **0.5 s**        | yes               |
| `racing`    | fast (150 ms)    | off                    | 2.0 s            | yes               |
| `endurance` | balanced (500 ms)| off                    | 2.0 s            | yes               |
| `patrol`    | balanced (500 ms)| off                    | 4.0 s            | yes               |
| `rally`     | fast (150 ms)    | base=1, enhance=1      | 2.0 s            | no — green smear  |
| `range`     | balanced (500 ms)| base=1, enhance=4      | 2.0 s            | no — green smear  |
| `fpv`       | robust (1000 ms) | base=1, enhance=4      | 2.0 s            | no — green smear  |

**Latency vs bitrate cost of short-GOP presets.**  Short GOPs reduce
worst-case recovery latency (next IDR is closer) but cost bitrate
because IDRs are 10–20× the size of P-frames.  At 1080p60 / 13 Mbps:

| GOP    | IDRs per 120 frames | IDR share of bitstream |
|--------|---------------------|------------------------|
| 4.0 s  | 0.5 (one every 240 fr) | ~3 %               |
| 2.0 s  | 1                       | ~5 %               |
| 0.5 s  | 4                       | ~20–25 %           |
| 0.25 s | 8                       | ~35–40 %           |

Pick `sprint` over `racing` when you have headroom and want a
guaranteed IDR floor on top of intra-refresh stripes.  Pick `rescue`
when you specifically want spec-compliant pure-IDR recovery (e.g. for
A/B-debugging whether an intra-refresh preset is misbehaving in the
field).  Both are OSD-safe.

Quick start:

```bash
# Default for FPV with OSD overlay — fast stripe recovery, no SVC-T
curl "http://<device>/api/v1/set?video0.resilience=racing"

# Long stable flight with OSD — balanced wavefront + 4 s GOP for bitrate
curl "http://<device>/api/v1/set?video0.resilience=patrol"

# OSD off, heavy refPred for long-range lossy link
curl "http://<device>/api/v1/set?video0.resilience=fpv"
```

Notes:
- H.265 only.  The runtime rewrites the NAL header of frames the SDK
  marks `ENHANCE_P_NOTFORREF` from `TRAIL_R` (type 1) to `TRAIL_N`
  (type 0) so a generic HEVC decoder can identify non-reference frames
  and drop them cleanly under loss.
- `video0.resilience` is the **only** user-facing knob for
  intra-refresh and refPred.  The underlying granular fields
  (`intra_refresh_*`, `ref_base`, `ref_enhance`, `ref_pred`) are
  intentionally not part of the JSON schema or HTTP API — the preset
  table fully drives them.  Use a named preset; if none fits, file an
  issue and we'll add one.
- Applied to ch0 only.  The dual-VENC recorder (ch1) is intentionally
  skipped — TS containers expect IDRs at GOP boundaries.
- Budget +20–30 % bitrate when picking a preset that enables
  intra-refresh; intra-coded rows compress worse than inter-coded ones.
- **OSD-unsafe explained.**  SVC-T TRAIL_N frames are dropped from the
  decoder's DPB after display, so their intra-refresh stripes don't
  persist as reference data.  For static high-contrast content (OSD
  text) the chroma plane stays in skip-mode prediction from the
  pre-refresh reference frame.  Once chroma drifts it can only be
  corrected by an IDR — stripe-only recovery doesn't work for those
  MBs.  The SigmaStar VENC SDK exposes no force-intra-MB knob to
  override this (ROI is delta-QP only, and skip-mode bypasses QP for
  zero-residual blocks).  Bench-confirmed: `racing`/`endurance`/`patrol`
  fully clean up the OSD area within ~10 wavefront cycles; `rally`
  and stronger refPred presets leave persistent green smear that only
  an IDR can clear.
- Real-world refPred benefit on a lossy link depends on the sender
  applying per-NAL-type FEC priority (protecting `TRAIL_R` more
  aggressively than `TRAIL_N`).  Without that integration the pyramid
  is roughly neutral on uniform random loss but keeps the bitstream
  spec-correct (no decoder warping).
- Unknown `resilience` values fall back to `off` with a warning at
  load time.

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

`unix://` uses Linux abstract Unix datagram sockets and is available in
both `rtp` and `compact` mode. On Star6E, `audioPort=0` piggybacks on the
same active video destination for both `udp://` and `unix://`. `shm://`
remains RTP-only; it cannot share audio, but a nonzero `audioPort` still
uses a dedicated local UDP audio destination.

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
set in `/etc/waybeam.json` only and requires a process restart to change.

Supported codecs: `"pcm"` (raw 16-bit, big-endian L16 per RFC 3551),
`"g711a"` (A-law), `"g711u"` (µ-law), `"opus"` (requires `libopus.so`
at runtime; falls back to PCM with a warning if the library or encoder
is unavailable).

**RTP payload types:** When streaming in RTP mode, Waybeam uses standard
static payload types when the sample rate matches the RFC 3551 standard:

| Codec | Sample rate | RTP PT | Notes |
|-------|-------------|--------|-------|
| `g711u` | 8000 | 0 (PCMU) | RFC 3551 standard |
| `g711a` | 8000 | 8 (PCMA) | RFC 3551 standard |
| `g711u` | non-8 kHz | 112 | Dynamic, Waybeam convention |
| `g711a` | non-8 kHz | 113 | Dynamic, Waybeam convention |
| `pcm` | 44100 | 11 (L16 mono) | RFC 3551 standard |
| `pcm` | other | 110 | Dynamic PCM |
| `opus` | any | 98 | Dynamic, majestic-compatible (RFC 7587) |

Sample rate range: 8000–48000 Hz (clamped by config parser). For Opus
the recommended sample rate is 48000 Hz (native Opus clock, no
resampling); the RTP clock is fixed at 48 kHz per RFC 7587 regardless
of capture rate. For voice-only FPV audio, 16 kHz G.711a remains a
low-latency choice.

**Frame timing:** Each RTP packet carries one 20 ms frame. The RTP
timestamp advances by `sample_rate / 50` samples for PCM/G.711, and by
960 (the 48 kHz nominal Opus tick) for Opus.

**Receiving Opus with GStreamer:**

The minimal one-liner that the README used to suggest had two recurring
problems on real receivers — out-of-order UDP packets confused
`rtpopusdepay`, and the default sink could not consume the Opus 48 kHz
mono stream directly. Use this expanded pipeline:

```bash
gst-launch-1.0 -v \
  udpsrc port=5601 \
    caps="application/x-rtp,media=audio,clock-rate=48000,encoding-name=OPUS,payload=98,channels=1" \
  ! rtpjitterbuffer latency=40 \
  ! rtpopusdepay \
  ! opusdec plc=true \
  ! audioconvert \
  ! audioresample \
  ! autoaudiosink sync=false
```

Key adjustments versus the older one-liner:
- `rtpjitterbuffer latency=40` is required — `rtpopusdepay` discards
  out-of-order packets on its own, which clicks/drops audio on lossy
  wireless links.
- `channels=1` matches the capture default; add it explicitly so
  versions of GStreamer that do not infer it from the encoded stream
  still negotiate.
- `audioresample` after `audioconvert` lets the chosen audio sink pick
  any rate (PulseAudio on a laptop will not always accept 48 kHz mono).
- `sync=false` on the sink avoids dropped frames at startup before
  the RTP clock has stabilised. Remove it once you have wallclock
  sync wired (`ntp-sync-parameters` / `clock-sync`).

For stereo capture (`audio.channels=2` in config) set `channels=2`
in caps. For PT-mismatched senders, replace `payload=98` with whatever
the sender reports in `/api/v1/audio/status`.

To dump RTP audio to a file instead of a sink:

```bash
gst-launch-1.0 \
  udpsrc port=5601 \
    caps="application/x-rtp,media=audio,clock-rate=48000,encoding-name=OPUS,payload=98,channels=1" \
  ! rtpjitterbuffer latency=40 \
  ! rtpopusdepay \
  ! oggmux \
  ! filesink location=audio.ogg
```

#### Recording

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `record.enabled` | bool | restart | Start recording on launch |
| `record.mode` | string | restart | `"off"`, `"mirror"`, `"dual"`, `"dual-stream"` |
| `record.dir` | string | restart | Output directory (must be mounted) |
| `record.format` | string | restart | `"ts"` (MPEG-TS + audio) or `"hevc"` (raw); on Maruko only `"ts"` is implemented |
| `record.max_seconds` | uint | restart | Rotate file after N seconds (0 = off) |
| `record.max_mb` | uint | restart | Rotate file after N MB (0 = off) |
| `record.bitrate` | uint | restart | Dual mode: ch1 bitrate in kbps (0 = same as video0) |
| `record.fps` | uint | restart | Dual mode: ch1 fps (0 = sensor max) |
| `record.gop_size` | double | restart | Dual mode: ch1 GOP in seconds (0 = same as video0) |
| `record.server` | string | restart | Dual-stream: second RTP destination URI |

Backend support:
- **Star6E** — full feature set: `mirror`/`dual`/`dual-stream` modes,
  both `ts` and `hevc` formats, HTTP-driven start/stop via
  `/api/v1/record/start|stop`, adaptive bitrate while SD-bound.
- **Maruko (Phase 6, v0.9.14)** — `mirror` and `dual` modes wired,
  `ts` format only, **config-driven only**: set `record.enabled=true`
  + `record.mode=...` in `/etc/waybeam.json` and reload. HTTP
  `/api/v1/record/start|stop` returns `501 not_implemented` on Maruko.
  Audio is interleaved into the TS file whenever Phase 5 audio
  capture is active (`audio.enabled=true`).

Recording can also be controlled at runtime via the HTTP API. In
dual/dual-stream modes, the secondary channel parameters can be
adjusted live via `/api/v1/dual/set`.

#### IMU (both backends, POC consumer)

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `imu.enabled` | bool | restart | Enable BMI270 IMU driver |
| `imu.i2c_device` | string | restart | I2C device path |
| `imu.i2c_addr` | uint8 | restart | I2C address (decimal or hex string, e.g. `104` or `"0x68"`) |
| `imu.sample_rate_hz` | int | restart | ODR in Hz (25-1600). Alias: `imu.sampleRateHz`. |
| `imu.gyro_range_dps` | int | restart | Gyro range in ±dps. Alias: `imu.gyroRangeDps`. |
| `imu.cal_file` | string | restart | Calibration file path |
| `imu.cal_samples` | int | restart | Auto-bias samples at startup |

Phase 3 (PR #84, v0.9.13) ported the IMU driver to Maruko with one
caveat: on Maruko, init must run **before** `MI_VENC_StartRecvPic`
because the 2 s auto-bias loop blocking the main thread post-VENC
leaves the encoder fd in a state where `poll()` never returns POLLIN.
This ordering constraint is captured in `maruko_pipeline.c`; do not
re-order without re-running the bench check on `192.168.2.12`.

### Usage Examples

**Start streaming to a receiver:**

```sh
curl "http://<device-ip>:<port>/api/v1/set?outgoing.server=udp://<receiver-ip>:5600"
curl "http://<device-ip>:<port>/api/v1/set?outgoing.enabled=true"
```

**Switch to 720p at 90 fps with lower bitrate:**

```sh
curl "http://<device-ip>:<port>/api/v1/set?video0.size=1280x720"
curl "http://<device-ip>:<port>/api/v1/set?video0.fps=90"
curl "http://<device-ip>:<port>/api/v1/set?video0.bitrate=4096"
```

**Manual white balance at 6500 K:**

```sh
curl "http://<device-ip>:<port>/api/v1/set?isp.awb_mode=ct_manual"
curl "http://<device-ip>:<port>/api/v1/set?isp.awb_ct=6500"
```

**Enable center-priority ROI encoding:**

```sh
curl "http://<device-ip>:<port>/api/v1/set?fpv.roi_enabled=true"
curl "http://<device-ip>:<port>/api/v1/set?fpv.roi_qp=-18"
curl "http://<device-ip>:<port>/api/v1/set?fpv.roi_steps=2"
```

**Request an IDR keyframe (useful after stream start):**

```sh
curl http://<device-ip>:<port>/request/idr
```

**Start/stop SD card recording:**

```sh
# Start recording (MPEG-TS with audio)
curl "http://<device-ip>:<port>/api/v1/record/start"

# Check recording status
curl "http://<device-ip>:<port>/api/v1/record/status"

# Stop recording
curl "http://<device-ip>:<port>/api/v1/record/stop"
```

## SD Card Recording

Waybeam records HEVC video with PCM audio to SD card in MPEG-TS format.
Recording runs concurrently with RTP streaming at minimal CPU overhead
(1–4 % additional load measured across 30–120 fps at 4–22 Mbps).

Key properties:
- **Power-loss safe** — MPEG-TS requires no finalization; partial files
  are playable up to the last written packet.
- **Gemini mode** — dual VENC channels for independent stream and record
  quality. Stream at 30 fps 4 Mbps over WiFi while recording at 120 fps
  20 Mbps to SD card. Four modes: off, mirror, dual, dual-stream.
- **Recording thread** — dedicated pthread drains the secondary encoder
  channel at full speed, with adaptive bitrate reduction (10 %/s) if
  the SD card can't keep up.
- **File rotation** — splits at IDR keyframe boundaries by time
  (default 5 minutes) or size (default 500 MB). Each segment is
  independently playable.
- **Disk safety** — periodic free-space checks with automatic stop
  when below 50 MB. Handles ENOSPC gracefully.
- **Audio interleaving** — raw 16-bit PCM, Opus, A-law, or µ-law from
  the hardware audio input is muxed alongside HEVC video in the TS
  container.
- **Live API control** — `/api/v1/dual/set` for runtime bitrate/GOP
  changes on the secondary channel.

Enable in config or use the HTTP API for runtime control. The SD card
must be pre-mounted at the configured directory (OpenIPC auto-mounts to
`/mnt/mmcblk0p1`).

Verify recordings with:

```sh
ffprobe recording.ts                # check streams and format
ffmpeg -i recording.ts -f null -    # full decode test
ffplay recording.ts                 # play directly
```

See `documentation/SD_CARD_RECORDING.md` for the full guide including
performance benchmarks, limitations, and architecture details.

## RTP Timing Sidecar

An optional out-of-band UDP channel that sends per-frame timing
metadata alongside the RTP video stream. Set `outgoing.sidecarPort=0`
to disable it.

When enabled, the sidecar provides frame-level diagnostics for the
entire sender-side pipeline:

```
capture_us → [encode] → frame_ready_us → [packetise+send] → last_pkt_send_us
                                                              ↕ (network)
                                                        recv_last_us (probe)
```

This enables measurement of encode duration, send spread, one-way
latency, frame interval jitter, RTP packet counts and gaps, and
optionally — when Star6E adaptive encoder control is active — per-frame
size, QP, complexity, scene-change flag, IDR decision, and
frames-since-IDR.

See `include/rtp_sidecar.h` and `tools/rtp_timing_probe.c` for the
full wire protocol and reference probe.

## Sensor Driver Sources

Full sensor driver source code is available in the `sensors-src/`
submodule (from [OpenIPC/sensors](https://github.com/OpenIPC/sensors)).
This includes drivers for IMX335, IMX415, GC4653, and other SigmaStar
Infinity6E sensors.

```sh
# Fetch the sensor sources (not cloned by default)
git submodule update --init sensors-src

# Driver sources for Infinity6E
ls sensors-src/sigmastar/infinity6e/sensor/
```

Pre-built kernel modules (`.ko`) for IMX335 and IMX415 remain in
`sensors/`.

### Maruko IMX335 Sensor Modes

Custom Maruko driver in `drivers/sensor_imx335_maruko.c` (built via
`make -C drivers sensor`):

| Mode | Resolution | Max FPS | Verified | Init table |
|------|-----------|---------|----------|------------|
| 0 | 1920x1080 | 60 | 59 fps | Star6E 120 fps windowed |
| 1 | 1920x1080 | 90 | 89 fps | Star6E 120 fps windowed |

Deploy: `scp sensor_imx335_maruko.ko root@device:/lib/modules/5.10.61/sigmastar/sensor_imx335_mipi.ko`

The driver uses no-op `pCus_poweroff` (sensor stays powered from boot)
and a VTS 120 % cap to prevent AE from dropping FPS in low light.
A delayed `MI_SNR_SetFps` kick after ~1 s fixes cold-boot FPS lock.

## Web Dashboard

Waybeam includes a built-in web dashboard served at the root URL
(`/`). Open `http://<device-ip>/` in any browser to access it.

### Settings Tab

All configuration fields across 12 sections (System, Sensor, ISP, Image,
Video, Outgoing, Audio, FPV, IMU, Recording, Adaptive Encoder Control,
Debug) with:

- **Collapsible sections** — start collapsed for a clean overview
- **Live/Restart badges** — green for immediate changes, orange for restart-required
- **Tooltips** — hover any field label for a description
- **Change tracking** — modified fields highlighted; Apply only sends changes
- **Apply Changes** — applies all modified fields via the API
- **Save & Restart** — applies changes then triggers pipeline reinit
- **Restore Defaults** — reloads on-disk config and resets the form

### API Reference Tab

Documentation for all HTTP endpoints with descriptions and example
responses, grouped by category: Configuration, Encoder Control, ISP &
Image Quality, Recording, and Dual-Stream.

### Image Quality Tab

Direct access to 62 SigmaStar ISP parameters organized by category.
Multi-field parameters render as inline forms; arrays render as
editable grids. Export the full IQ state as a timestamped JSON file
and import it back to restore tuning. Partial imports are supported.

```sh
# Export current IQ state
curl http://<device>/api/v1/iq > my_tuning.json

# Import (full or partial)
curl -X POST -H "Content-Type: application/json" \
  -d @my_tuning.json http://<device>/api/v1/iq/import
```

Multi-field parameters support dot-notation for individual field access:

```sh
curl "http://<device>/api/v1/iq/set?colortrans.y_ofst=200"
curl "http://<device>/api/v1/iq/set?colortrans.matrix=23,45,9,1005,987,56,56,977,1015"
```

Legacy single-value set (`?colortrans=200`) still works for backward
compatibility.

### Status Bar

The top telemetry bar shows version, backend type, live FPS
(auto-refreshes every 2 s), recording status indicator, and an Export
Config button to download the full configuration as JSON.

## IMU (BMI270 gyro module)

The BMI270 driver is compiled into the binary on both backends but
disabled by default (`imu.enabled = false`). When enabled, it samples
gyro+accel via the hardware FIFO at 200 Hz, drains per video frame, and
hands samples to a caller-supplied push callback.

The previous EIS consumer (`gyroglide` crop-based stabilization) was
removed in 0.8.0 — see `HISTORY.md` for the rationale and
`documentation/EIS_INTEGRATION_PLAN.md` for what a future replacement
(LDC-warp Phase C) would look like.

**Maruko ordering caveat.** On Maruko, IMU init must run **before**
`MI_VENC_StartRecvPic` (i.e. before `bind_maruko_pipeline()`) because
the auto-bias loop blocks the main thread for ~2 s. Empirically,
blocking the main thread for 2 s after `StartRecvPic` leaves the VENC
fd in a state where `poll()` never returns POLLIN and the stream loop
never progresses. Star6E does not exhibit this — IMU init can stay
post-VENC there.

To enable the IMU for development:

```json
{
  "imu": {
    "enabled": true,
    "i2cDevice": "/dev/i2c-1",
    "i2cAddr": "0x68",
    "sampleRateHz": 200,
    "gyroRangeDps": 1000,
    "calFile": "/etc/imu.cal",
    "calSamples": 400
  }
}
```

Restart Waybeam. The 2-second auto-calibration runs at startup — hold
the board still during it.

## Inspiration & Credits

Waybeam exists because of prior OpenIPC work. Two upstream projects
made it possible to start from a working baseline instead of a blank
page; we owe them direct credit:

- [**OpenIPC/divinus**](https://github.com/OpenIPC/divinus) — the
  reference reverse-engineered camera firmware for SigmaStar SoCs.
  We borrowed the SigmaStar MI API struct layouts (`MI_SYS`, `MI_SNR`,
  `MI_VIF`, `MI_VPE`, `MI_VENC`, `MI_RGN`) needed to talk to the
  vendor `.so` libraries without an SDK header, plus the
  IMX415/IMX335 sensor-unlock register sequences for high-FPS modes.
- [**OpenIPC/research / venc**](https://github.com/OpenIPC/research/tree/master/venc)
  — early standalone-encoder research, source of the initial `dlopen`
  approach to load MI libs at runtime and the first sketch of the
  VENC channel lifecycle. Both projects are MIT-licensed (as is this
  one), so reuse is explicitly allowed.

### Code provenance — current state

We did a recent line-by-line accounting against the v0.3.0 import and
the two upstream projects. Across the current ~37 kLoC of `src/` +
`include/`:

| Source | Approx share | Where it lives today |
|---|---:|---|
| OpenIPC/divinus | **~3 %** | `include/sigmastar_types.h` MI ABI structs/enums; IMX sensor unlock register tables in `sensor_select.c`. |
| OpenIPC/research/venc | **~5 %** | Initial `dlopen` symbol loader pattern; first sketch of the Star6E VENC channel start/stop loop, now substantially refactored. |
| New to Waybeam | **~92 %** | Maruko backend (entire); dual-backend pipeline architecture; HTTP API + WebUI; ISP/IQ system; custom 3A; recording (MPEG-TS mux, dual VENC, adaptive bitrate); RTP timing sidecar; intra-refresh; scene detection; snapshot channel; IMU driver + ring; SIGHUP-respawn handoff; audio capture (Opus/G.711/PCM RTP + TS). |

Both upstream projects are MIT licensed (so is Waybeam) — reuse is
explicit and welcome.  The numbers above are a transparent inventory,
not a disclaimer.  If you find a line or pattern that traces back
specifically and we missed crediting it, please open an issue and
we'll fix the attribution.

## License

MIT — see [LICENSE](LICENSE).
