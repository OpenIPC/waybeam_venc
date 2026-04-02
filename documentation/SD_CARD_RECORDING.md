# SD Card Recording

## Overview

venc can record HEVC video with PCM audio to SD card while simultaneously
streaming via RTP. The recording uses MPEG-TS (.ts) as the container format.

MPEG-TS was chosen because:
- **Power-loss safe** — no finalization step required. If power is cut
  mid-recording, everything up to the last write is playable.
- **CPU-efficient** — pure memory operations, no compression beyond what the
  HW encoder already does.
- **Universally playable** — ffplay, VLC, mpv, and ffprobe all handle it.

## Quick Start

### 1. Ensure SD card is mounted

OpenIPC auto-mounts the first SD card partition at `/mnt/mmcblk0p1`.
Verify with:

```bash
df -h /mnt/mmcblk0p1
```

If not mounted, the `scripts/S99mountSD` init script can be installed to
`/etc/init.d/` on the device for automatic mounting on boot.

### 2. Enable recording in config

Edit `/etc/venc.json` on the device:

```json
{
  "record": {
    "enabled": true,
    "dir": "/mnt/mmcblk0p1",
    "format": "ts",
    "maxSeconds": 300,
    "maxMB": 500
  }
}
```

Recording starts automatically when venc launches.

### 3. HTTP API control

Recording can also be started and stopped at runtime without editing the config:

```bash
# Start recording (uses config dir, or override with ?dir=)
wget -q -O- "http://<device>:<port>/api/v1/record/start"
wget -q -O- "http://<device>:<port>/api/v1/record/start?dir=/mnt/mmcblk0p1"

# Stop recording
wget -q -O- "http://<device>:<port>/api/v1/record/stop"

# Check status
wget -q -O- "http://<device>:<port>/api/v1/record/status"
```

Status response:
```json
{
  "ok": true,
  "data": {
    "active": true,
    "format": "ts",
    "path": "/mnt/mmcblk0p1/rec_01h23m45s_abcd.ts",
    "frames": 1500,
    "bytes": 12345678,
    "segments": 1,
    "stop_reason": "none"
  }
}
```

## Config Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | bool | `false` | Start recording on launch |
| `mode` | string | `"mirror"` | Recording mode (see Gemini Mode below) |
| `dir` | string | `"/mnt/mmcblk0p1"` | Output directory (must exist and be writable) |
| `format` | string | `"ts"` | `"ts"` (MPEG-TS with audio) or `"hevc"` (raw NAL stream) |
| `maxSeconds` | uint | `300` | Rotate to new file after this many seconds (0 = no time limit) |
| `maxMB` | uint | `500` | Rotate to new file after this many MB (0 = no size limit) |
| `bitrate` | uint | `0` | Dual mode: ch1 bitrate in kbps (0 = same as video0) |
| `fps` | uint | `0` | Dual mode: ch1 fps (0 = sensor max) |
| `gopSize` | double | `0` | Dual mode: ch1 GOP in seconds (0 = same as video0) |
| `server` | string | `""` | Dual-stream mode: second RTP destination URI |

## Gemini Mode (Dual VENC)

The `record.mode` field selects between four operating modes that use one
or two hardware VENC encoder channels:

| Mode | ch0 (primary) | ch1 (secondary) | Use case |
|------|--------------|-----------------|----------|
| `"off"` | Stream via RTP | — | Stream only, no recording |
| `"mirror"` | Stream + record | — | Same frames to both (default) |
| `"dual"` | Stream (video0 config) | Record (record config) | Different quality per destination |
| `"dual-stream"` | Stream to server #1 | Stream to server #2 | Two outgoing RTP streams |

In **mirror** mode (default), a single VENC channel encodes frames that are
sent via RTP and simultaneously written to the TS recorder. This is the
simplest mode and has the lowest overhead.

In **dual** mode, two independent VENC channels encode the same VPE output
at different bitrates/fps/GOP settings. The primary channel (ch0) uses
`video0` config for low-latency streaming. The secondary channel (ch1) uses
`record.bitrate`/`record.fps`/`record.gopSize` for high-quality recording.
This enables streaming at 30fps 4 Mbps over a constrained WiFi link while
simultaneously recording at 120fps 20 Mbps to SD card.

In **dual-stream** mode, both channels stream via RTP to different
destinations. Ch0 uses `outgoing.server`, ch1 uses `record.server`.
No recording to SD card.

### Dual mode config example

```json
{
  "video0": {
    "fps": 30, "bitrate": 4000, "gopSize": 0.5
  },
  "outgoing": {
    "enabled": true, "server": "udp://192.168.1.6:5600"
  },
  "record": {
    "enabled": true,
    "mode": "dual",
    "dir": "/mnt/mmcblk0p1",
    "format": "ts",
    "bitrate": 20000,
    "fps": 0,
    "gopSize": 2.0
  }
}
```

This streams 30fps 4 Mbps (fast GOP for WiFi) while recording at sensor-max
fps (120fps on imx335 mode 3) at 20 Mbps (quality).

### Hardware encoder limits

The SigmaStar HW encoder has a total throughput ceiling of approximately
**150 1080p HEVC frames/second** across all channels combined. Measured on
Star6E ssc338q + imx335:

| Sensor FPS | ch0 FPS | ch1 FPS | Total | Status |
|-----------|---------|---------|-------|--------|
| 30 | 30 | 30 | 60 | Full rate, zero loss |
| 60 | 60 | 60 | 120 | Full rate, zero loss |
| 90 | 74 | 74 | 148 | HW limit (~82% of sensor) |
| 120 | 76 | 76 | 152 | HW limit (~63% of sensor) |

For FPV use (stream 30fps + record 120fps), the combined load is ~150 fps
which is within the HW budget with no frame drops over sustained 2-minute
runs.

Bitrate does NOT affect throughput — the encoder bottleneck is pixel
processing, not bitrate. 20 Mbps and 40 Mbps per channel produce identical
frame rates.

## Recording Formats

### MPEG-TS (`"ts"`) — recommended

- Container: `.ts` files with PAT/PMT, HEVC video (PID 0x100), PCM audio (PID 0x101)
- Audio: raw 16-bit PCM from the AI capture device (same source as RTP audio)
- Each segment starts with PAT + PMT + IDR keyframe — playable independently
- PCR embedded in video adaptation field for decoder timing
- HEVC registration descriptor in PMT for player compatibility

### Raw HEVC (`"hevc"`)

- Container: `.hevc` files with raw Annex-B NAL units (start-code delimited)
- No audio, no timestamps, no container overhead
- Playable with `ffplay file.hevc` but no seeking support
- Useful for debugging or minimal-overhead recording

## File Rotation

When `maxSeconds` or `maxMB` thresholds are reached, the recorder:

1. Waits for the next IDR (keyframe) boundary
2. Closes and fsyncs the current segment
3. Opens a new `.ts` file with fresh PAT/PMT
4. Resets continuity counters (each segment is self-contained)

File naming: `rec_<HH>h<MM>m<SS>s_<rand>.ts` based on system uptime.

Both `maxSeconds` and `maxMB` can be active simultaneously. Set either to `0`
to disable that dimension. Setting both to `0` produces a single file until
manually stopped or disk full.

## Disk Safety

- **Space check**: `statvfs` every 300 frames. Recording auto-stops when free
  space drops below 50 MB.
- **ENOSPC handling**: if `write()` returns ENOSPC, recording stops immediately
  with `stop_reason: "disk_full"`.
- **Periodic fsync**: `fdatasync()` every 900 frames (~30s at 30fps) to flush
  kernel buffers to SD card.
- **Short write handling**: the write loop retries on partial writes and EINTR.

## Concurrent Streaming + Recording

RTP streaming and SD card recording operate on the same encoded video frames
independently. The audio thread pushes raw PCM to both the RTP encoder (after
G.711 encoding) and the recording ring buffer (before codec encoding)
simultaneously.

There is no mutex contention between the streaming and recording paths — the
audio ring buffer is the only shared structure, protected by a lightweight
mutex with sub-microsecond hold times.

## Performance (HW Measured)

Star6E ssc338q + imx335, 1280x720 CBR, audio enabled:

| FPS | Bitrate | Stream only | Stream + Record | Overhead |
|-----|---------|-------------|-----------------|----------|
| 30  | 4 Mbps  | 4% CPU      | 5% CPU          | +1%      |
| 30  | 8 Mbps  | 4% CPU      | 4% CPU          | +0%      |
| 30  | 16 Mbps | 3% CPU      | 5% CPU          | +2%      |
| 30  | 22 Mbps | 2% CPU      | 3% CPU          | +1%      |

Star6E ssc338q + imx335, 1920x1080 CBR, audio enabled:

| FPS | Bitrate | Stream only | Stream + Record | Overhead |
|-----|---------|-------------|-----------------|----------|
| 120 | 8 Mbps  | 17% CPU     | 18% CPU         | +1%      |
| 120 | 16 Mbps | 17% CPU     | 21% CPU         | +4%      |
| 120 | 22 Mbps | 20% CPU     | 24% CPU         | +4%      |

Recording overhead is 0-4% CPU across all tested configurations.

## Limitations

- **Audio codec**: only raw PCM (16-bit signed, mono/stereo) is muxed into the
  TS container. G.711 encoded audio is not recorded — the ring buffer captures
  PCM before the software codec. ffmpeg identifies the audio stream as private
  data; custom playback tools may need configuration to decode it as PCM.
- **Audio disabled**: when `audio.enabled` is `false` in config, the TS file
  contains video only (no audio PID in PMT). This is fully supported.
- **Maximum frame size**: the NAL extraction buffer is 512 KB, supporting
  bitrates up to ~50 Mbps. Frames exceeding this are truncated with a warning
  log.
- **No runtime format switching**: the `format` field is read at recording
  start. Changing format requires stopping and restarting recording.
- **SD card must be pre-mounted**: venc does not mount the SD card itself.
  The directory specified in `record.dir` must exist and be writable. If the
  directory does not exist, recording fails with an error log.
- **vfat timestamp limitation**: FAT32 filesystems do not store sub-second
  timestamps. File creation times have 2-second resolution.
- **No in-stream seeking**: TS files can be seeked by frame/time in players,
  but the recorder does not write an index. Players seek by scanning PTS values,
  which is fast for files under ~1 GB.
- **Segment rotation latency**: rotation only occurs at IDR boundaries. With
  the default 1-second GOP, the actual segment duration may exceed `maxSeconds`
  by up to one GOP interval.
- **32-bit byte counters in status**: `bytes_written` is `uint64_t` internally
  but the JSON status endpoint formats it as an integer. Values above 2^53
  (~9 PB) lose precision in JSON. Not a practical concern.
- **Star6E only**: the TS recording feature is wired into the Star6E runtime.
  Maruko backend does not currently have recording support.
- **Dual mode encoder limit**: at 1080p, the HW encoder caps at ~150 total
  fps across both channels. At 90/120fps sensor modes with dual full-rate
  encode, each channel gets ~74-76 fps. Use asymmetric fps (e.g., stream
  30fps + record 120fps) to stay within the budget.
- **Dual mode requires restart**: switching between modes (mirror/dual/
  dual-stream) requires a pipeline restart. Mode is not live-changeable.
- **Dual mode teardown**: a dedicated recording thread keeps ch1 frames
  drained during pipeline teardown to prevent VPE backpressure. Intermittent
  D-state hangs are possible if the SD card blocks a write for >500ms during
  shutdown — a 10s SIGALRM safety net writes the VPE SCL preset before
  forced exit.

## Recording Thread (Dual Mode)

In dual and dual-stream modes, a dedicated pthread drains ch1 frames
independently of the main loop. This prevents VPE backpressure caused
by synchronous TS mux + SD writes blocking the encode path at 120fps.

Key properties:
- Thread runs through entire `pipeline_stop()` teardown, keeping ch1
  buffer empty while `StopRecvPic` flushes VENC state
- Checks `g_running` to skip SD writes during shutdown (fast drain)
- ch1 output buffer depth is 64 frames (533ms at 120fps) for SD card
  latency tolerance

### Adaptive Bitrate

If the SD card can't keep up with the configured recording bitrate, the
recording thread automatically reduces ch1 bitrate:

- After each frame write, peeks at queue depth to detect backpressure
- If >80% of frames have another queued for 3 consecutive seconds,
  reduces bitrate by 10%
- Floor at 25% of original bitrate (minimum 1 Mbps)
- Bitrate stays reduced for the rest of the session (no recovery)
- Avoids reacting to transient SD write stalls (single slow write)

## Dual Channel HTTP API

The secondary VENC channel can be controlled at runtime in dual-stream mode:

```bash
# Query ch1 status (bitrate, fps, gop, active state)
wget -q -O- "http://<device>:<port>/api/v1/dual/status"

# Change ch1 bitrate
wget -q -O- "http://<device>:<port>/api/v1/dual/set?bitrate=10000"

# Change ch1 GOP (in seconds)
wget -q -O- "http://<device>:<port>/api/v1/dual/set?gop=1.0"

# Request IDR on ch1
wget -q -O- "http://<device>:<port>/api/v1/dual/idr"
```

All endpoints return 404 when dual VENC is not active. See
`documentation/HTTP_API_CONTRACT.md` for the full specification.

## Verification

Verify a recorded file:

```bash
# Check streams and format
ffprobe -analyzeduration 10000000 -probesize 10000000 recording.ts

# Full decode test (no output, just checks for errors)
ffmpeg -i recording.ts -f null -

# Remux to MP4 for editing
ffmpeg -i recording.ts -c:v copy -t 60 clip.mp4

# Play directly
ffplay recording.ts
```

Expected ffprobe output:
```
Stream #0:0[0x100]: Video: hevc (Main) (HEVC / 0x43564548), yuv420p(tv), 1280x720, 30 fps
Stream #0:1[0x101]: Data: bin_data (LPCM / 0x4D43504C)
```

## Architecture

```
Audio Thread                    Main Loop
    |                               |
    | MI_AI_GetFrame()              | MI_VENC_GetStream()
    |                               |
    v                               v
[raw PCM] --push--> AudioRing   [HEVC NALs]
                       |            |
                       |     ts_mux_write_video()
                       |            |
                 audio_ring_pop()   |
                       |            |
                ts_mux_write_audio()|
                       |            |
                       v            v
                    [TS packets in buffer]
                           |
                      write_all(fd)
                           |
                           v
                      [.ts file on SD]

Concurrent:
  star6e_video_send_frame() ──> RTP stream (UDP)
```

Files:
- `ts_mux.h` / `ts_mux.c` — stateless 188-byte TS packet emitter
- `audio_ring.h` — SPSC mutex ring for PCM frames (header-only)
- `star6e_ts_recorder.h` / `star6e_ts_recorder.c` — orchestrates mux + I/O + rotation
- `star6e_recorder.h` / `star6e_recorder.c` — raw HEVC recorder (original)
