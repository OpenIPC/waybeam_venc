# Audio UDP Output

## Overview

venc supports optional audio capture via the SigmaStar MI_AI hardware audio
input interface. Audio is captured in a dedicated thread, optionally encoded
in software, and sent as UDP/RTP packets alongside (or separate from) the
video stream. The MI_AI library (`libmi_ai.so`) is loaded at runtime via
`dlopen`; if the library is absent, audio is silently disabled.

## Configuration

Audio is configured via the `audio` section in `venc.json`, plus the
`audioPort` field in the `outgoing` section.

### Example

```json
{
  "audio": {
    "enabled": true,
    "sampleRate": 48000,
    "channels": 1,
    "codec": "opus",
    "volume": 80
  },
  "outgoing": {
    "server": "udp://192.168.1.2:5600",
    "streamMode": "rtp",
    "maxPayloadSize": 1400,
    "audioPort": 5601
  }
}
```

### Audio settings

| Field | Type | Default | Valid values | Description |
|-------|------|---------|-------------|-------------|
| `enabled` | bool | `false` | `true`, `false` | Master enable for audio capture |
| `sampleRate` | int | `16000` | `8000`, `16000`, `32000`, `48000` | Sample rate in Hz. Hardware-validated on SSC338Q at 8/16/48 kHz. |
| `channels` | int | `1` | `1`, `2` | 1 = mono, 2 = stereo. |
| `codec` | string | `"pcm"` | `"pcm"`, `"g711a"`, `"g711u"`, `"opus"` | Audio codec (see below). Unknown values fall back to PCM with a warning. |
| `volume` | int | `80` | `0`-`100` | Capture volume. Mapped to MI_AI dB scale (-60 to +30 dB). |
| `mute` | bool | `false` | `true`, `false` | Mute audio output (capture still runs). |

### Outgoing settings (audio-related)

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `audioPort` | int | `0` | UDP port for audio. `0` = same destination as video (demux by RTP payload type). Any non-zero value = dedicated audio port. |

## Codecs

All encoding is done in software. The SigmaStar MI_AI hardware encoder
(`MI_AI_EnableAenc`) is not used due to a confirmed segfault inside the
vendor library when encoding is enabled.

### pcm — Raw PCM

- No encoding. 16-bit signed little-endian samples sent directly.
- Bandwidth: `sampleRate * channels * 2` bytes/sec
  - 8 kHz mono: 128 kbps
  - 16 kHz mono: 256 kbps
  - 48 kHz mono: 768 kbps
- CPU overhead: none (memcpy only)

### g711a — G.711 A-law

- ITU-T G.711 A-law companding. 16-bit PCM → 8-bit.
- Bandwidth: `sampleRate * channels` bytes/sec (8 kHz mono: 64 kbps)
- CPU overhead: ~0%
- RTP payload type: PT=8 at 8kHz (RFC 3551), PT=113 at other rates

### g711u — G.711 μ-law

- ITU-T G.711 μ-law companding. Slightly better SNR than A-law at low signal levels.
- Bandwidth: same as g711a
- CPU overhead: ~0%
- RTP payload type: PT=0 at 8kHz (RFC 3551), PT=112 at other rates

### opus — Opus

- Opus codec via `libopus.so` (loaded at runtime via `dlopen`).
- If `libopus.so` is unavailable, falls back to raw PCM with a warning.
- Bandwidth: ~16–64 kbps depending on encoder bitrate (Opus default for VoIP quality)
- CPU overhead: small (~1–2% on SSC338Q at 48kHz mono)
- RTP payload type: PT=120 (dynamic, per RFC 7587)
- RTP clock: **always 48kHz** per RFC 7587 §4.2, regardless of encoder sample rate
  - Timestamp increments by 960 per 20ms frame (48000/50 = 960)
- Recommended: `sampleRate: 48000` — native Opus rate, no resampling

### Codec comparison

| Codec | Bandwidth (48kHz mono) | CPU | Quality | Library required |
|-------|----------------------|-----|---------|-----------------|
| pcm | 768 kbps | 0% | Lossless | none |
| g711a | 48 kbps | ~0% | Good (telephony) | none |
| g711u | 48 kbps | ~0% | Good (μ-law) | none |
| opus | ~32 kbps | ~1% | Excellent | `libopus.so` |

## Transport modes

Audio follows the video stream mode set in `outgoing.streamMode`.

### RTP mode (`"streamMode": "rtp"`)

Standard RTP packetization with 12-byte header:

| Field | Value |
|-------|-------|
| Version | 2 |
| Payload type | Per codec/rate (see table above) |
| SSRC | Random, distinct from video SSRC |
| Timestamp | +960 per frame for Opus (48kHz nominal); +`sampleRate/50` for others |
| Clock rate | 48kHz for Opus (RFC 7587); `sampleRate` for PCM/G.711 |

### Compact mode (`"streamMode": "compact"`)

4-byte header followed by audio payload:

```
[0] = 0xAA (magic byte — video NALUs never start with 0xAA)
[1] = 0x01 (audio packet type)
[2] = length high byte
[3] = length low byte
[4..] = audio payload
```

## Port behavior

- **`audioPort: 0`** (default): Audio packets share the video UDP socket and
  destination. Receivers demux by RTP payload type (RTP mode) or magic byte
  (compact mode).
- **`audioPort: N`** (N > 0): Audio is sent to a dedicated UDP port on the
  same destination host as video. Clean separation; receivers bind two ports.

## Architecture

```
Microphone/I2S ──► MI_AI device ──► DMA ring (frmNum=20, 400ms)
                                         │
                                  GetFrame (50ms timeout)
                                         │
                                ┌────────▼────────┐
                                │ Capture thread  │  SCHED_FIFO/1
                                │ (Thread A)      │
                                └────────┬────────┘
                                         │  cap_ring (64 entries)
                                ┌────────▼────────┐
                                │ Encode thread   │  SCHED_OTHER
                                │ (Thread B)      │
                                │                 │
                                │ ┌─────────────┐ │
                                │ │ Opus/G.711  │ │
                                │ │ SW encode   │ │
                                │ └──────┬──────┘ │
                                └────────┼────────┘
                                         │
                                  RTP packetize
                                         │
                                    UDP sendto
```

### Threading model

Audio uses two threads:

- **Thread A (capture)**: Calls `MI_AI_GetFrame` in a tight loop with 50ms
  timeout, copies the PCM frame into `cap_ring`, releases the MI frame
  immediately. Runs at `SCHED_FIFO` priority 1 (minimum RT) — sufficient
  since the DMA ring holds 400ms (frmNum=20), and no data loss occurs unless
  the ring fills. Higher RT priority was tested and made timing worse due to
  interference with ISP threads.
- **Thread B (encode+send)**: Pops frames from `cap_ring`, encodes (Opus/G.711
  or passthrough), packetizes, and sends via UDP. Runs at `SCHED_OTHER`.

### MI_AI device configuration

Key parameters:
- `rate`: sample rate (8000, 16000, 48000)
- `i2s.clock = 0`: MCLK **disabled** — I2S master generates its clock from
  the internal PLL based on `rate`. Must be 0; setting to 1 (12.288 MHz MCLK)
  causes the driver to ignore the `rate` field and deliver 16kHz data
  regardless of configuration.
- `i2s.syncRxClkOn = 1`: I2S TX and RX share the same clock source (per SDK
  reference).
- `frmNum = 20`: 400ms DMA ring — protects against ISP/AE preemption bursts.
- `packNumPerFrm = sampleRate/50`: ~20ms per DMA frame at all sample rates.
- Port depth: `user=1, buf=2` — minimum buffering (40ms), down from the
  default (2,4) which added 80ms unnecessarily.

### stdout filter

`libmi_ai.so`'s internal DMA thread (`ai0_P0_MAIN`, SCHED_RR/98) writes
`[MI WRN]: Buffer(s) is lost due to slow fetching` to **stdout** (fd 1)
using ANSI yellow escape codes. The filter interposes fd 1 with a pipe;
a filter thread discards any line beginning with ESC (0x1B) and forwards
everything else to the real stdout. Installed at every audio init (including
reinit), torn down at teardown.

### Library loading

`libmi_ai.so` is loaded via `dlopen("libmi_ai.so", RTLD_NOW | RTLD_GLOBAL)`.
`libopus.so` is loaded on demand when `codec: "opus"` is configured.
Missing libraries cause graceful fallback (warning + continue without audio).

## Receiving audio

### Opus (recommended)

```bash
# ffplay
ffplay -nodisp -f rtp -i rtp://0.0.0.0:5601

# GStreamer
gst-launch-1.0 udpsrc port=5601 caps="application/x-rtp,payload=120,clock-rate=48000,encoding-name=OPUS" ! \
  rtpopusdepay ! opusdec ! autoaudiosink
```

### G.711 A-law

```bash
# GStreamer (8kHz)
gst-launch-1.0 udpsrc port=5601 ! \
  "application/x-rtp,encoding-name=PCMA,clock-rate=8000" ! \
  rtppcmadepay ! alawdec ! autoaudiosink
```

### Raw PCM

```bash
# GStreamer (16kHz mono)
gst-launch-1.0 udpsrc port=5601 ! \
  "application/x-rtp,encoding-name=L16,clock-rate=16000,channels=1" ! \
  rtpL16depay ! autoaudiosink
```

## Hardware requirements

Audio requires an I2S audio source connected to the SoC's I2S pins. The
SSC338Q exposes I2S master pins; board-level routing varies. Common options:

- MEMS microphone breakout (INMP441, ICS-43434) on I2S
- Line-in codec (ES8388, NAU88C22) on I2S

If no audio hardware is connected, `MI_AI_GetFrame` will return silence or
timeout; the audio thread idles without output. The video pipeline is
unaffected.

## Known limitations

- **No hardware encoding**: `MI_AI_EnableAenc` / `MI_AI_SetAencAttr` causes
  a segfault inside `MI_AI_GetFrame`. All encoding is software.

- **No G.726 codec**: G.726 (ADPCM) requires per-sample state tracking
  (~200 LOC). Not implemented; use G.711 instead.

- **Maruko backend**: Audio is not supported on the Maruko backend.

- **Single audio source**: Only device 0, channel 0 is used.

- **32kHz not hardware-validated**: The SDK lists `E_MI_AUDIO_SAMPLE_RATE_32000`
  as valid and the code supports it, but it has not been tested on SSC338Q
  hardware. 8kHz, 16kHz, and 48kHz are confirmed working.
