<p align="center">
  <img src="docs/assets/waybeam_logo.png" alt="Waybeam" width="420">
</p>

<h1 align="center">Waybeam — Vehicle Video Encoder</h1>

<p align="center">
  <em>Standalone H.265 (HEVC) encoder &amp; RTP streamer for embedded camera SoCs.</em>
</p>

---

Waybeam is the camera-side daemon for the Waybeam FPV ecosystem. It owns
the ISP, sensor, and VENC channel on the vehicle, captures audio, streams
RTP / compact UDP / Unix / SHM video to a ground station, optionally
records to SD card, and exposes the whole pipeline through a single
zero-restart HTTP API and a built-in web dashboard.

Three build-time SoC backends share one source tree:

- **Star6E** — SigmaStar Infinity6E (SSC30KQ, SSC338Q).
- **Maruko** — SigmaStar Infinity6C (SSC378QE).
- **CV610** — HiSilicon Hi3516CV610 with Sony IMX662 (initial streaming
  backend; advanced controls are being added in phases under #220).

Each binary is produced from the same `make build` invocation with
different `SOC_BUILD=` flags. SigmaStar MI vendor libraries are loaded via
`dlopen` so the binary stays small and the Maruko bundle can ship its
own copies of libs that stock OpenIPC Infinity6C firmware does not.

> **Note on naming.** The product, binary, config file, init script,
> and release tarball are all named `waybeam`. The GitHub repository
> is still `waybeam_venc` for historical URL stability — that is the
> only place the old name survives.

## Features

- H.265 (HEVC) encoding with CBR / VBR / AVBR / FIXQP rate control
- RTP packetization (single-NAL + FU-A, fixed `maxPayloadSize`); compact UDP raw-NAL mode
- Built-in web dashboard at `/` for configuration, API docs, and IQ tuning
- HTTP API for live parameter tuning without pipeline restart
- ISP IQ parameter system: 60+ params, multi-field structs, JSON export/import
  (Star6E and Maruko)
- Custom 3A: built-in AE and AWB with configurable gain limits and convergence
- ROI-based QP gradient for FPV center-priority encoding
- Sensor FPS unlock for IMX415 / IMX335 (in-tree drivers; up to 144 fps
  on Star6E IMX335, 100 fps on IMX415)
- Optional audio capture (Opus / G.711a / G.711µ / raw PCM) on Star6E and
  Maruko, RTP or compact UDP output, mute via live API
- SD card recording: MPEG-TS mux (HEVC + audio in TS, PCM / A-law / µ-law / Opus
  alongside video), power-loss safe; raw `.hevc` available on Star6E
- Gemini / dual-VENC: concurrent stream + high-quality record (Star6E and Maruko)
- Adaptive recording bitrate: auto-reduces if SD card can't keep up
- Maruko-specific opt-in 3A throttle (`isp.aeEngine="custom"`) — saves
  ~24 % sys CPU at 120 fps with no visible AE quality loss
- BMI270 IMU driver with frame-synced FIFO (Star6E and Maruko) — compiled in,
  disabled by default, ready for telemetry/sidecar consumers
- Intra-refresh (GDR-style rolling stripe) for fast loss recovery on FPV links
- Scene-change-triggered IDR (Star6E) for clean stream join under packet loss
- Inline QR scanning (Star6E): overlay-free VPE port1 luma tap + isolated
  `/usr/bin/qr_decode` helper, so a craft reads a pairing marker itself
  without a workstation in the loop

## Build

From the repo root:

```sh
# Star6E (Infinity6E)
make build SOC_BUILD=star6e

# Maruko (Infinity6C)
make build SOC_BUILD=maruko

# HiSilicon CV610 (external public headers + OpenIPC sysroot required)
make build SOC_BUILD=cv610 \
  CV610_CC=/path/to/arm-openipc-linux-musleabi-gcc \
  CV610_SDK_INC=/path/to/openhisilicon \
  CV610_SDK_LIB=/path/to/cv610/rootfs/usr/lib
```

The toolchain is auto-downloaded on first build. Each backend builds to
its own output directory:

```
out/star6e/waybeam   # Star6E binary
out/maruko/waybeam   # Maruko binary
out/cv610/waybeam    # CV610 binary
```

All backend outputs can coexist; no clean is needed when switching.

Stage a deployable bundle with vendored libraries:

```sh
make stage SOC_BUILD=star6e
# Output: out/star6e/{waybeam,qr_decode} + out/star6e/lib/*.so
# (Maruko also stages drivers/ + isp-bins/)
# (CV610 also stages sensors/, S95waybeam, waybeam-cv610.conf, and waybeam.json)
```

Run host tests:

```sh
make test-ci
```

## Deployment

### Star6E (Infinity6E)

Copy the daemon and its isolated QR helper to the target device:

```sh
scp out/star6e/waybeam root@<device-ip>:/usr/bin/waybeam
scp out/star6e/qr_decode root@<device-ip>:/usr/bin/qr_decode
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
    "sliceCount": 1,
    "resilience": "off",
    "framing": "off", "zoomX": 0.5, "zoomY": 0.5
  },
  "outgoing": {
    "enabled": false, "server": "", "streamMode": "rtp",
    "maxPayloadSize": 1400,
    "connectedUdp": true, "allowUnixEncoderStall": false,
    "audioPort": 5601, "sidecarPort": 5602
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
  `sceneHoldoff`) is on Star6E and Maruko. Intra-refresh and whole-access-unit H.265
  slicing are on Star6E, Maruko and CV610. The
  `framing` knob expands to either digital zoom (Star6E and Maruko) or image
  stabilization (`stab` HW-crop / `stab-fill` floating-image, Star6E only;
  live `pauseStab`).
- **`outgoing`** — destination URI (`udp://`, `unix://`, `shm://`,
  `frame-shm://`), stream mode (`rtp` / `compact`), payload sizing,
  optional dedicated audio + sidecar UDP ports.
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
`udp://<receiver_ip>:5600`, `unix://<abstract_name>`,
`shm://<ring_name>` (RTP-packet ring), or `frame-shm://<ring_name>`
(whole-frame ring; see [Frame-SHM output](#frame-shm-output)).

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
on both SigmaStar backends — `curl "http://<dev>/api/v1/set?snapshot.quality=40"`
applies instantly with no pipeline reinit. The remaining snapshot
fields are restart-required (channel-attribute baked at
`MI_VENC_CreateChn` time).

#### GET /api/v1/version

Returns version info.

```sh
curl http://<device-ip>:<port>/api/v1/version
```

```json
{"ok":true,"data":{"app_version":"0.71.0","backend":"star6e","contract_version":"0.20.0","config_schema_version":"1.0.0"}}
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

A field MAY also carry an optional `ui` object (data-driven field schema):
`group` (collapsible section title), `label`, `control`
(`toggle`/`number`/`select`/`text`), `min`/`max`/`step` (for `number`),
`options` (for `select`), and `tooltip`. The dashboard renders a control
from this generically, so a module field reaches the WebUI with no
`dashboard.html` edit or webui-blob rebuild. The entire **Stabilization**
section is built this way (the six `video0.stab_*` knobs plus the
runtime-only `video0.pause_stab`).

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
running, codec, rate, channels, Opus initialization). Both SigmaStar backends.
See [HTTP_API_CONTRACT.md](documentation/HTTP_API_CONTRACT.md) for full
field reference.

```sh
curl http://<device-ip>:<port>/api/v1/audio/status
```

#### GET /api/v1/transport/status

Live observability for the active video transport (UDP / Unix / SHM):
fill percentage, backpressure flag, lifetime drop counters, and on
`frame-shm://` the ring low-water gauge. Consumed by external link
controllers; the WebUI does not call it.

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
| `isp.ae_engine` | string | restart | `"sdk"` (default) lets the SDK firmware run AE on Star6E and Maruko.  `"custom"` runs userspace cus3a — on Star6E it spins the supervisory AE thread; on Maruko it installs the no-op adaptor + 15 Hz `SetAeParam` thread (~24% sys CPU saving at 120 fps).  Alias: `isp.aeEngine`. |
| `isp.ae_fps` | uint | restart | Custom 3A processing rate in Hz (default 15) |
| `isp.gain_max` | uint | live | AE max ISP gain ceiling (0 = use ISP bin default) |
| `isp.awb_mode` | string | live | `"auto"` or `"ct_manual"` |
| `isp.awb_ct` | uint | live | Color temperature in K (for ct_manual) |
| `isp.keep_aspect` | bool | restart | When `true` (default), VIF/SCL crop preserves sensor AR; `false` lets downstream stretch. Star6E + Maruko (Phase 1, v0.9.9). |

#### Image

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `image.mirror` | bool | restart | Horizontal mirror. Applied at the sensor (`MI_SNR_SetOrien`) once at bring-up. Star6E + Maruko. |
| `image.flip` | bool | restart | Vertical flip. Applied at the sensor (`MI_SNR_SetOrien`) once at bring-up. Star6E + Maruko. |
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
| `video0.framing` | string | restart | VPE crop mode: `off`, `stab`, `stab-fill`, `zoom-1.25x`, `zoom-1.50x`, `zoom-1.75x`, `zoom-2x`, `zoom-3x`, `zoom-4x` (see Framing below) |
| `video0.zoom_x` | double | live | Pan crop center X (`0.0` left to `1.0` right) — applies to `zoom-*` modes only |
| `video0.zoom_y` | double | live | Pan crop center Y (`0.0` top to `1.0` bottom) — applies to `zoom-*` modes only |
| `video0.stab_crop_pct` | uint | restart | Override `stab`/`stab-fill` kept-frame / border budget (`0` = preset default 80, else `60..100`) |
| `video0.stab_kalman_q` | double | restart | Pan response (Kalman process noise), shared by `stab`/`stab-fill` (`0.001..1.0`; higher = follows pans sooner / weaker hold; preset default 0.03) |
| `video0.stab_kalman_r` | double | restart | Smoothness (Kalman measurement noise), shared by `stab`/`stab-fill` (`0.1..50.0`; higher = smoother but laggier; preset default 2.0) |
| `video0.stab_recenter_speed` | uint | restart | `pauseStab` glide-home rate in frames (`0..3600`, `0` = default ramp). Inert during normal stabilization — the Kalman recentres |
| `video0.pause_stab` | bool | live | Live pause for `stab`/`stab-fill` — glides the stabilized window / floating image back to centre (software ramp, no rebind). Runtime-only (not persisted); boots `false`. No effect under `off`/`zoom-*` |

#### Framing: Stabilization & Digital Zoom

`video0.framing` is the **single user-facing knob** for the VPE crop. It
is a named preset (restart-required); the underlying crop fraction is
*derived* from the preset and is not separately settable — there is no
`zoom_pct`/`zoomPct` API field.

| `framing` | Effect | Encode dim @1080p | Backends |
|-----------|--------|-------------------|----------|
| `off` | Full image | 1920×1080 | both |
| `stab` | Image stabilization (centered 80% crop) | 1536×864 | Star6E only |
| `stab-fill` | Image stabilization (floating image on a black border) | 1920×1080 | Star6E only |
| `zoom-1.25x` | 1.25× digital zoom | 1536×864 | both |
| `zoom-1.50x` | 1.50× digital zoom | 1280×720 | both |
| `zoom-1.75x` | 1.75× digital zoom | 1088×608 | both |
| `zoom-2x` | 2× digital zoom | 960×528 | both |
| `zoom-3x` | 3× digital zoom | 640×352 | both |
| `zoom-4x` | 4× digital zoom | 480×256 | both |

**Digital zoom** uses Approach-C: it shrinks *both* the crop window and the
encoded output resolution. The SCL path reads the crop at 1:1 and emits it
unchanged — no upscale pass, no extra bandwidth pressure. Receivers see the
smaller resolution in SPS/PPS (the encode dims above are 16-px aligned with a
256-px floor, so the tightest 4× still emits a valid 480×256 frame). Because
there is no upscale, the deep 3×/4× crops are **not** bound by the SCL ~2×
upscale ceiling.

Both stabilization presets run the **same control law** — a Kalman trajectory
smoother — so identical settings give identical feel. The only difference is how
the stabilized offset is applied:

**Stabilization** (`stab`, Star6E only) holds a centered 80% crop and shifts the
**hardware crop window** per frame to cancel motion. It is always centered —
`zoom_x`/`zoom_y` are ignored. Encode is HW-cropped, so the stream resolution
shrinks (1536×864 @1080p) — a fps cost applies (~60→40 on imx335).

**Stabilization, fill variant** (`stab-fill`, Star6E only) keeps the **full
encode resolution** (1920×1080) and composes a *floating* stabilized image on a
black border: it SCL-downscales the full sensor frame and shifts a window inside
it, filling the exposed edges with black. The trade is fps for full resolution.

**Live pause** (`pauseStab`, `stab` *and* `stab-fill`) freezes stabilization
without a pipeline restart: it glides the stabilized window (`stab`) / floating
image (`stab-fill`) back to centre via a software ramp — no HW rebind. It is
runtime-only (not persisted, always boots `false`) and a no-op under `off`/`zoom-*`:

```bash
curl "http://<device>/api/v1/set?video0.pauseStab=1"   # freeze (glide to centre)
curl "http://<device>/api/v1/set?video0.pauseStab=0"   # resume
```

Four **tuning** knobs shape the stabilization (all restart-required; all inert
under `off`/`zoom-*`; shared identically by `stab` and `stab-fill`; re-selecting
the preset resets them to the defaults, so **set `framing` first, then the
overrides**):

- `stab_crop_pct` — **headroom**. Kept-frame % (`stab`) / shift+border budget
  (`stab-fill`). `0` keeps the preset default (80). Smaller (e.g. `60`) = bigger
  dead border = more motion absorbed, at the cost of a tighter / more-bordered
  frame.
- `stab_kalman_q` — **pan response** (Kalman process noise, default `0.03`,
  range `0.001..1.0`). Higher = the view follows slow pans sooner (weaker hold);
  lower = holds tighter and more locked. The estimate eases the offset back to
  centre on its own — there is no separate recenter knob.
- `stab_kalman_r` — **smoothness** (Kalman measurement noise, default `2.0`,
  range `0.1..50.0`). **The primary feel knob.** Higher = smoother but laggier
  (the frame trails your real motion); lower = snappier but more jitter passes
  through.
- `stab_accuracy` — **motion-estimator effort** (`auto` / `high` / `medium` /
  `low`, default `auto`). Sets the IVE block-matching detector level: higher =
  finer motion tracking at more CPU; `auto` picks a level from the mode. Shared
  by `stab` and `stab-fill`.

(`stab_recenter_speed` only sets the `pauseStab` glide-home rate; during normal
stabilization the Kalman handles recentering.)

##### Calibrating to taste

Watch the `stab tick` line in the log (printed every 120 frames):

```
stab tick 600: meas=(82,83) acc=(-206,31) max=(288,216) pan=(500,500) kalman(q=0.0300,r=2.00) paused=0 ...
```
- `meas` = motion estimate this detect · `acc` = the offset being applied ·
  `max` = the border budget (= half the cropped-away pixels) · `kalman` = the
  active q/r.

Recommended order:

1. **Headroom first.** Shake the camera the way it will really move. If `acc`
   sits pinned near `±max` and clips, you're saturating — lower `stab_crop_pct`
   (80 → 70 → 60) until `acc` has room. If motion is gentle and you want max
   FOV/sharpness, keep 80.
2. **Then smoothness (R).** If the output looks jittery/shaky, *raise*
   `stab_kalman_r` (2 → 4 → 8). If it feels floaty and lags your real movement,
   *lower* it (2 → 1 → 0.5).
3. **Then hold vs pan (Q).** If a deliberate pan feels sticky/rubber-banded,
   *raise* `stab_kalman_q` (0.03 → 0.06 → 0.1) so the view follows sooner. If the
   view drifts during slow movement, *lower* it (0.03 → 0.015) to hold tighter.

The active values are echoed once at start so you can confirm each change:
```
[waybeam] stab: src=1440x1080 out=864x648 crop=60% kalman(q=0.0300,r=2.00) pauseGlide=180
```

Examples (each `set` is restart-required; the daemon respawns to apply):

```bash
# Production stab (all knobs at preset defaults).
curl "http://<device>/api/v1/set?video0.framing=stab"

# Smoother, more locked feel: heavier measurement filtering + tighter hold.
# (Restart-required fields must be set ONE per request — the daemon respawns
# between them, so wait for each to come back before the next.)
curl "http://<device>/api/v1/set?video0.stabKalmanR=6"
curl "http://<device>/api/v1/set?video0.stabKalmanQ=0.02"

# Looser, follows pans sooner; more motion headroom (tighter crop).
curl "http://<device>/api/v1/set?video0.stabKalmanQ=0.08"
curl "http://<device>/api/v1/set?video0.stabCropPct=60"
```

**Panning** applies to the `zoom-*` modes only and is live (`zoom_x`,
`zoom_y` ∈ [0,1], default 0.5/0.5 = centered):

```bash
# Restart-required: enable a 3x crop (centered).
curl "http://<device>/api/v1/set?video0.framing=zoom-3x"

# Live pan inside the current zoom crop — top-left corner.
curl "http://<device>/api/v1/set?video0.zoomX=0.0&video0.zoomY=0.0"

# Live pan — bottom-right corner.
curl "http://<device>/api/v1/set?video0.zoomX=1.0&video0.zoomY=1.0"

# Re-center the zoom crop.
curl "http://<device>/api/v1/set?video0.zoomX=0.5&video0.zoomY=0.5"

# Switch to stabilization (centered; pan ignored).
curl "http://<device>/api/v1/set?video0.framing=stab"

# Back to full frame.
curl "http://<device>/api/v1/set?video0.framing=off"
```

CamelCase aliases: `video0.zoomX`, `video0.zoomY`. `zoom_x`/`zoom_y` SETs
return `service paused for pipeline reinit, retry` while a `framing` change
is still reinitializing — retry once it settles.

When `debug.showOsd=true` and a zoom preset is active, the overlay adds rows
after existing OSD stats:

```
zoom  3.00x 640x352
crop  640x352+640+364
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

#### Encoder thread priority (Star6E)

The Star6E encode path — `MI_VENC_GetStream` → RTP packetize → `sendto`, plus
the `stab-fill` blit/compose thread — runs on `SCHED_FIFO` priority **50**,
pinned to CPU 0. At the previous minimum RT priority (`1`) these threads were
occasionally preempted mid-frame by other userspace RT threads and
`SCHED_OTHER` work, producing a periodic ~one-frame RTP delivery stall (a
single idle gap on the wire — no frame loss, FPS unaffected). Priority 50 sits
above those peers but well below the SDK pipeline kernel threads
(`SCHED_RR/98`), which must keep producing the frames we consume.

Override without a rebuild via the `VENC_RT_PRIO` environment variable
(clamped `1..80`); `VENC_RT_PRIO=1` restores the old minimum-priority
behaviour. Requires root (silent fallback otherwise).

A *separate* jitter source — a large I-frame whose serialization exceeds one
frame interval on a constrained uplink (e.g. 50 Mbps on a 100 Mbps link) — is
not a scheduling issue and is unaffected by this priority; mitigate it with a
[resilience preset](#resilience-preset-star6e--maruko--cv610) (intra-refresh) or a
lower bitrate.

#### Resilience preset (Star6E + Maruko + CV610)

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

##### `ltr` — maximum non-reference density with a long GOP

`ltr` trades differently from the other presets: instead of shortening the
GOP so damage is repaired sooner, it makes **half the stream disposable**
and lets you keep a long GOP.

**What `u32Enhance` actually means** (device-measured on Star6E,
2026-08-06, by NAL census of the raw elementary stream — the SDK documents
nothing): it is a *period*, not a count.  The encoder emits exactly one
non-referenced frame in every `enhance + 1`:

| `enhance` | frame pattern | non-referenced (steady state) | measured |
|-----------|---------------|-------------------------------|----------|
| 1         | `InRnRnRn…`   | **50 %** (max)                | 50.0 %   |
| 4         | `IRRRnRRRRn…` | 20 %                          | 17.6 %   |
| 299       | `IRRRR…`      | 0.33 %                        | 0.3 %    |

A capture reads slightly under the steady-state figure because the census
includes the IDR and the partial groups at each end; the shorter the group,
the more that dilutes.  The rule itself is exact.

So **smaller is more resilient**, and `enhance=1` is the ceiling this SoC
can express.  Bare `ltr` selects it; `ltr:<N>` pins the period for sweeps
and is strictly less resilient as N grows.

A lost non-referenced frame costs exactly one frame — the next frame is
already clean.  The other half of the stream is an ordinary P-chain, so
losing one of *those* still cascades to the next IDR.  There is no way to
do better here: the full SigmaStar SDK's `MI_VENC_Set*` surface has **no
long-term-reference, SmartP, or GOP-mode API** — `MI_VENC_SetRefParam` is
the only reference-structure control, and P-frames always predict from the
previous frame, never from the IDR.

Unlike `rally` (the same 1:1 ratio) `ltr` preserves your `gopSize` and
forces intra-refresh off, so it can be paired with a long GOP and
**asymmetric FEC** on the transport: heavy protection on the IDR, light on
the rest.  In waybeam-link that is `fec.i_rate_permille` high with
`fec.p_rate_permille` low.

**Measured cost: essentially none.**  At pinned QP 30 on a moving scene,
`ltr:1` and `off` differ by under 1 % in bitrate (5.62/5.64 vs 5.65/5.68
Mbps over alternating runs).  P-frame size is flat across the GOP, since
prediction is from the previous frame either way.

> ⚠️  **`ltr` is OSD-unsafe.**  It maximises the `ref_enhance > 0`
> condition described above, so expect persistent chroma "green smear"
> over static OSD text, clearing only on the IDR — and with a long GOP it
> persists correspondingly longer.  Verify what the encoder applied with
> `GET /api/v1/resilience/status`.
>
> `bEnablePred` is a **no-op** for this marking on Star6E: `rally`
> (`pred=true`) and `ltr:1` (`pred=false`) produce byte-identical
> `InRnRn…` patterns.

| Field | Type | Mutability | Description |
|-------|------|------------|-------------|
| `video0.resilience` | string | restart | `off` \| `rescue` \| `quality` \| `sprint` \| `racing` \| `endurance` \| `patrol` \| `rally` \| `range` \| `fpv` \| `ltr` \| `ltr:<N>` (default `off`) |
| `video0.gopSize`    | double | restart | Seconds between IDRs.  Honoured **only** when `resilience` is `"off"` or an `ltr` form; the other named presets override it.  Live-reinit applies (no reboot). |

Resilience and slice-count changes are restart-required on all three encoder
backends. The API persists the value and uses the backend's process-respawn
handoff to build a fresh encoder graph; a hardware reboot is not part of the
normal apply contract. SigmaStar deliberately avoids in-process MI teardown
and reinitialization because vendor-kernel teardown has proven unsafe. Verify
the running result through `/api/v1/resilience/status` and startup slice
readback logs.

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

# Max non-reference density (50 % droppable) with a long GOP; OSD-unsafe.
# Pair with asymmetric FEC (protect the IDR, barely protect the rest).
curl "http://<device>/api/v1/set?video0.gopSize=5.0"
curl "http://<device>/api/v1/set?video0.resilience=ltr"

# Sweep the period — larger N is LESS resilient (1 droppable frame per N+1)
curl "http://<device>/api/v1/set?video0.resilience=ltr:4"

# Confirm what the encoder actually applied (resolved ratio, not AUTO)
curl "http://<device>/api/v1/resilience/status"
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
  issue and we'll add one.  `ltr:<N>` is the one parameterised form,
  and it exists so the reference ratio can be swept without a rebuild.
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
| `outgoing.server` | string | live | Destination URI (`udp://ip:port`, `unix://name`, `shm://name`, or `frame-shm://name`) |
| `outgoing.stream_mode` | string | restart | `"rtp"` or `"compact"` |
| `outgoing.max_payload_size` | uint16 | restart | Max UDP payload bytes |
| `outgoing.connected_udp` | bool | restart | Connect UDP socket (applies only to `udp://`) |
| `outgoing.allow_unix_encoder_stall` | bool | restart | Preserve blocking `unix://` behavior when the consumer queue fills. Default `false`: bounded wait then drop the rest of the frame |
| `outgoing.audio_port` | int32 | restart | `>0` = dedicated audio port; `0` = shared video destination; `<0` (e.g. `-1`) = record-only (audio captured + recorded but never streamed). With `unix://`, dedicated audio is sent to `127.0.0.1:<audioPort>` |
| `outgoing.sidecar_port` | uint16 | restart | RTP timing sidecar port (0 = disabled) |

`unix://` uses Linux abstract Unix datagram sockets and is available in
both `rtp` and `compact` mode. On Star6E, `audioPort=0` piggybacks on the
same active video destination for both `udp://` and `unix://`. `shm://`
remains RTP-only; it cannot share audio, but a nonzero `audioPort` still
uses a dedicated local UDP audio destination.

> **`unix://` requires a deep datagram queue.** Unlike UDP, an AF_UNIX
> datagram sender blocks on the *receiver's* queue depth. The kernel
> snapshots that depth from `net.unix.max_dgram_qlen` when the receiving
> socket is created, and the default of **10 datagrams** is only ~7 ms of
> buffer at 15 Mbps with 1400-byte RTP payloads — less than one 60 fps
> frame (~23 packets). Every frame then overruns the queue and stalls the
> encode thread waiting on consumer scheduling, which shows up as timing
> jitter and dropped capture frames rather than as packet loss.
>
> Raise it **before the consumer starts** — raising it afterwards does
> nothing for a socket that already exists:
>
> ```sh
> echo 256 > /proc/sys/net/unix/max_dgram_qlen
> ```
>
> `init.d/S95waybeam` does this at boot. venc warns on stderr at startup
> when it finds a shallower value. Sends are additionally bounded by
> `SO_SNDTIMEO` and a 4 ms per-frame flush deadline, so a wedged consumer
> costs bounded packet drops (counted as `transportDrops` in
> `GET /api/v1/transport/status`) instead of stalling the encoder.

<a id="frame-shm-output"></a>
`frame-shm://` publishes **whole encoded frames** (Annex-B, start codes
preserved) into a POSIX shared-memory ring, bypassing RTP packetization
entirely — no RTP state, no `sendmmsg`, no sidecar. It exists so a
same-host consumer (waybeam-link) can apply per-frame FEC at frame
boundaries instead of re-fragmenting pre-built RTP packets. The ring is an
8-slot SPSC region — 384 KB per slot on Star6E and Maruko, 512 KB on CV610;
each slot carries an 8-byte `VencFrameMeta` header (`pts`, `codec`, `flags`
— `flags` bit 0 marks an IDR, bit 3 is reserved for the receiver-set
SALVAGED flag) followed by the raw frame. On a full ring (consumer stalled or gone)
the encoder drops the frame and keeps running — it never blocks. Like
`shm://` it is video-only (a nonzero `audioPort` uses a dedicated local
UDP audio destination) and cannot be switched to/from live (restart
required). Wire format is specified in the coordination repo at
`protocols/frame-shm.md`; validate a live ring with
`tools/frame_shm_consumer_test.c`.

A negative `audioPort` (e.g. `-1`) selects **record-only** mode: the audio
capture/encode thread still runs and feeds the recording, but no output
socket is created and no audio packets are ever sent. Use this to keep audio
off the air link while still capturing it into SD recordings. It requires
`audio.enabled` and a TS-family `record.format` (HEVC recordings carry no
audio track). Settable via the config file or the runtime API
(`/api/v1/set?outgoing.audioPort=-1`).

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

#### IMU (Star6E and Maruko, POC consumer)

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

## QR Scanning (Star6E)

A craft can read a QR marker itself. A scan window opens an **overlay-free**
capture tap on VPE port1, decodes each frame in the isolated `qr_decode`
helper, and closes when it finds a code or its budget runs out. `make stage`
and `scripts/star6e_direct_deploy.sh` install both executables.

Overlay-free is the point. MI_RGN composites **per scaler output port**, and
every overlay producer targets port0 — `debug_osd` here, `osd_render` in
waybeam-hub. The MJPEG snapshot channel is a port0 1:N consumer, so
`/api/v1/snapshot.jpg` carries whatever HUD is running, right over the middle of
the frame where a marker sits. port1 is a separate scaler output and is clean.

### Enable it

```bash
# Off by default. Restart-required (the geometry is captured at graph build).
curl "http://<device-ip>/api/v1/set?qr.tapEnabled=true"
```

| Field | Default | Mut. | Meaning |
|---|---|---|---|
| `qr.tapEnabled` | `false` | restart | arm the tap |
| `qr.tapWidth` / `qr.tapHeight` | `0` | restart | tap geometry; `0` inherits the main stream |
| `qr.windowMs` | `15000` | live | default scan budget, clamped 1000–60000 |

The capture is the **centre square** of the tap output — 1920×1080 gives a
1080×1080 scan area. Cropping in software rather than asking the port for a
square is deliberate: the SCL scales but does not crop, so a square port would
squash the aspect, and `MI_VPE_SetPortCrop` is sticky on i6e and would poison a
later detect run.

### A marker to test with

<img src="tools/qr/test-images/bounded-P23456789ABCDEFG.png" alt="Waybeam test marker, payload P23456789ABCDEFG" width="260">

Point the camera at this and a scan returns `P23456789ABCDEFG`. The file is
`tools/qr/test-images/bounded-P23456789ABCDEFG.png` (1230×1230, binary-clean);
an SVG vector master and a compact `.pgm` regression fixture sit beside it.

**This is not a plain QR code, and a plain QR code will not decode.** The
scanner requires a continuous **33×33 Waybeam outer-frame profile** wrapped
around a Version-1/Q symbol, and it uses that frame's geometry to derive the
projective transform directly — which is what lets it find a small marker in a
large frame without an unbounded finder scan. Standards-only finder discovery is
never entered without an accepted outer frame. Payloads are further restricted
to exactly 16 characters from the QR alphanumeric alphabet, starting `P` or `C`.

For a hand-held optical test, open **`tools/qr/test-images/phone.html`** on a
phone. It is self-contained — no network, no sibling assets — and **generates
markers itself**: type a payload and it renders live, or hit **Random**. Presets
cover large / medium / small / tiny and 35 degrees, with an **Invert** toggle for
the light-on-dark case. Physically tilting the phone is a better test than a
pre-warped image, because it exercises the real camera projective transform.

Or generate a file:

```bash
python3 -m pip install -r tools/qr/requirements-generator.txt
python3 tools/qr/generate_qr.py C0FFEE1234567890 mymarker.png --scale 30
```

The page's encoder is checked against that generator and against the real decode
cascade by `make qr-test-phone`, so what a phone shows is what the craft reads.

### Scan

```bash
# Open a window (or extend the one already running)
curl "http://<device-ip>/api/v1/qr/scan?ms=10000"

# Poll it
curl "http://<device-ip>/api/v1/qr/status"

# End it early
curl "http://<device-ip>/api/v1/qr/stop"
```

`/qr/status` carries the result:

```json
{"armed": true, "scanning": false, "window_ms": 10000, "remaining_ms": 0,
 "capture": "1080x1080", "frames": 3, "grabs": 1, "port1_owner": "",
 "decode": {"attempts": 1, "decoded": true, "payload": "P23456789ABCDEFG",
            "stage": "qr_decode", "decode_ms": 84, "last_ms": 84}}
```

The `decode` block survives the window closing and is cleared only by the next
`/qr/scan`, so a client polling at 1 Hz still sees a payload from a window that
found its code and shut down between two polls.

`decode.stage` is `qr_decode`; detailed cascade-stage diagnostics remain
available from the standalone helper's `--stats` mode.

### What it costs

Measured on a Star6E bench (imx335, 1080×1080 scan area) before the decoder was
moved behind the helper process boundary. These figures validate the tap and
cascade; end-to-end helper overhead still needs device revalidation:

| | |
|---|---|
| marker in view | decodes on the **first frame of the first attempt, 73–88 ms** |
| nothing to find | full cascade 431 ms @1080², 238 ms @720², 117 ms @540² |
| encoder impact during a full-budget window | **none — 60 fps held**, CPU 25% → 55% |

Scanning runs at `nice 10` and holds a duty cycle: after each attempt it idles
for as long as that attempt took. Without that, back-to-back cascades pegged
both cores and dragged the encoder from 60 fps to 23 — only the encoder thread
is `SCHED_FIFO`, so the ISP, AWB and frame-shm threads lose to a decoder that
never yields.

### Port contention

port1 is single-owner and shared with framing-stab and NPU detect, so a scan is
**mutually exclusive** with both:

```bash
curl "http://<device-ip>/api/v1/qr/scan"
# {"ok":false,"error":{"code":"port1_busy","message":"VPE port1 is held by stab or detect"}}
```

QR is the lowest-priority claimant and never evicts them — turn `framing` or
`detect.enabled` off to free the port. `port1_owner` in `/qr/status` tells you
who holds it.

### Debug capture

```bash
# One frame of the tap as a P5 PGM — geometry, exposure, OSD-freedom checks
curl "http://<device-ip>/api/v1/qr/tap.pgm" -o tap.pgm

# Decode it on a workstation with the same cascade the daemon runs
make qr-decode SOC_BUILD=star6e     # or build for host: tests/qr_decode_host
./out/star6e/qr_decode --stats tap.pgm
```

Only valid while a window is open (`503` otherwise), and `409` while the helper
owns the latch.

### Notes for integrators

- **Scan rate is floored by the daemon, not the client.** Cycling VPE port1 too
  fast wedges the kernel — `MI_VPE_DisablePort` racing an in-flight mhal buffer
  jams the VPE input FIFO. So there is a 500 ms minimum between opens and a
  750 ms minimum time the port stays up. A window that decodes in 85 ms still
  holds port1 for 750 ms before handing it back. Do not try to beat this with a
  tighter poll loop; you will just block.
- **A window self-closes.** A client that dies mid-scan cannot strand port1.
- **Re-scanning while a window is open only extends the deadline** and never
  touches port state.
- Payloads are Waybeam transport envelopes: exactly 16 characters from the QR
  alphanumeric alphabet. `--raw` on the CLI relaxes that for bench work; the
  daemon always enforces it.

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

The sender supports up to 4 concurrent subscribers (keyed by
addr:port, per-slot 5 s TTL) so a vehicle-local hub consumer, the wfb
link_controller and a debug probe can coexist on one port.

With `attitude.enabled=true` (requires `imu.enabled`, Star6E only) each
FRAME also carries a 12-byte ATTITUDE trailer — roll/pitch/yaw from an
on-device complementary filter, int16 0.1° units — consumed by the
vehicle hub for the HUD artificial horizon.

**Mounting correction** is two-stage, all restart-required:

- `attitude.axisFwd` / `attitude.axisDown` (`+x`..`-z`, defaults `+x`/`+z`)
  — a signed axis remap applied to gyro+accel *before* the estimator, so a
  board mounted in any of the 24 axis-aligned orientations reads correctly.
  Identified once per hardware design with the two-pose procedure (camera
  level → gravity axis is `axisDown`; lens straight down → gravity axis is
  `axisFwd`).
- `attitude.trimRollDeg` / `attitude.trimPitchDeg` — boresight trims: the
  residual roll/pitch the estimator reads while the camera is held level,
  undone input-side. Capture them automatically by holding the camera level
  and calling `GET /api/v1/attitude/calibrate_level` (or the WebUI Attitude
  section's "Capture level trims" button), which averages the level-pose
  accel, solves the trims, and persists them via the restart path.
- `attitude.mountDeg` (0/90/180/270) + `attitude.invertRoll` /
  `attitude.invertPitch` remain as a coarse legacy alternative to the axis
  remap.

`GET /api/v1/attitude` returns the live fused snapshot (works standalone —
no sidecar subscriber needed) for the WebUI readout and field setup.

See `include/rtp_sidecar.h`, the canonical cross-repo spec
`protocols/rtp-sidecar.md` (waybeam-coordination), and
`tools/rtp_timing_probe.c` for the full wire protocol and reference
probe.

## Sensor Driver Sources

This repo ships **in-tree, mode-unlocked** IMX335 / IMX415 driver sources
for Star6E and Maruko under `drivers/` — `sensor_imx{335,415}_star6e.c` and
`sensor_imx{335,415}_maruko.c` — with the mode lineups documented in
`drivers/SENSOR_MODE_GUIDE.md` and the per-SoC deep-dives in
`documentation/STAR6E_IMX335_MODES.md`, `STAR6E_IMX415_MODES.md`, and
`MARUKO_IMX335_MODES.md`. Pre-built `.ko` for each backend live in
`sensors/star6e/` and `sensors/maruko/`.

The full upstream sensor catalogue (GC4653 and other SigmaStar Infinity6E
sensors) is available in the `sensors-src/` submodule (from
[OpenIPC/sensors](https://github.com/OpenIPC/sensors)):

```sh
# Fetch the upstream sensor sources (not cloned by default)
git submodule update --init sensors-src
ls sensors-src/sigmastar/infinity6e/sensor/
```

### Building Star6E sensor drivers from source

`drivers/sensor_imx{335,415}_star6e.c` needs the Infinity6E kernel source
tree (SigmaStar BSP, not hosted here). Supply it on the command line:

```sh
make drivers-star6e KSRC_STAR6E=/path/to/infinity6e-kernel
# → sensors/star6e/sensor_imx{335,415}_star6e.ko
```

The Star6E IMX335 lineup adds window-crop tiers up to **144 fps**
(1600×900) and IMX415 up to **100 fps**, over the stock linear modes.
Both drivers apply a **fixed-framerate exposure policy** — the AE shutter
is pinned at the 1/fps ceiling and VMAX is held (nominal-fps vts trims) so
frame rate no longer sags with scene brightness. See the mode docs above
for the full geometry / VMAX tables.

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

The BMI270 driver is compiled into the Star6E and Maruko binaries but
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
