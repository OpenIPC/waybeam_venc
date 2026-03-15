# Audio UDP Output

## Overview

venc supports optional audio capture via the SigmaStar MI_AI hardware audio
input interface. Audio is captured in a dedicated thread, optionally encoded
in software, and sent as UDP packets alongside (or separate from) the video
stream. The MI_AI library (`libmi_ai.so`) is loaded at runtime via `dlopen`;
if the library is absent, audio is silently disabled.

## Configuration

Audio is configured via the `audio` section in `venc.json`, plus the
`audioPort` field in the `outgoing` section.

### Example

```json
{
  "audio": {
    "enabled": true,
    "sampleRate": 8000,
    "channels": 1,
    "codec": "g711a",
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
| `sampleRate` | int | `16000` | `8000`, `16000`, `32000`, `48000` | Sample rate in Hz. Lower rates use less bandwidth. 8000 is typical for voice. |
| `channels` | int | `1` | `1`, `2` | 1 = mono, 2 = stereo. Clamped to 1-2. |
| `codec` | string | `"pcm"` | `"pcm"`, `"g711a"`, `"g711u"` | Audio codec (see below). Unknown values fall back to PCM with a warning. |
| `volume` | int | `80` | `0`-`100` | Capture volume. Mapped to MI_AI dB scale (-60 to +30 dB). |

### Outgoing settings (audio-related)

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `audioPort` | int | `5601` | UDP port for audio. `0` = same destination as video (demux by RTP payload type or packet header). Any other value = dedicated audio port. |

## Codecs

All encoding is done in software. The SigmaStar MI_AI hardware encoder
(`MI_AI_EnableAenc`) is not used due to a confirmed segfault in the vendor
library's `MI_AI_GetFrame` when encoding is enabled.

### pcm — Raw PCM

- No encoding. 16-bit signed little-endian samples sent directly.
- Bandwidth: `sampleRate * channels * 2` bytes/sec
  - 8 kHz mono: 128 kbps
  - 16 kHz mono: 256 kbps
  - 48 kHz mono: 768 kbps
- CPU overhead: none (memcpy only)
- Use when bandwidth is not a concern or for maximum quality.

### g711a — G.711 A-law

- ITU-T G.711 A-law companding. 16-bit PCM compressed to 8-bit.
- 2:1 compression ratio.
- Bandwidth: `sampleRate * channels` bytes/sec
  - 8 kHz mono: 64 kbps
- CPU overhead: ~0% (measured on SSC30KQ at 8 kHz mono)
- Standard telephony codec. Good voice quality at low bandwidth.
- RTP payload type: 110 (dynamic)

### g711u — G.711 μ-law

- ITU-T G.711 μ-law companding. 16-bit PCM compressed to 8-bit.
- 2:1 compression ratio. Slightly better SNR than A-law at low signal levels.
- Bandwidth: same as g711a
- CPU overhead: ~0% (measured on SSC30KQ at 8 kHz mono)
- RTP payload type: 110 (dynamic)

### Codec comparison

| Codec | Bandwidth (8kHz mono) | CPU | Quality |
|-------|-----------------------|-----|---------|
| pcm | 128 kbps | 0% | Lossless |
| g711a | 64 kbps | 0% | Good (telephony standard) |
| g711u | 64 kbps | 0% | Good (slightly better low-level SNR) |

## Transport modes

Audio follows the video stream mode set in `outgoing.streamMode`.

### RTP mode (`"streamMode": "rtp"`)

Standard RTP packetization with 12-byte header:

| Field | Value |
|-------|-------|
| Version | 2 |
| Payload type | 110 (dynamic, all codecs) |
| SSRC | Random, distinct from video SSRC |
| Timestamp | Increments by samples per frame (320 at default config) |
| Clock rate | Matches sample rate |

Audio uses a separate SSRC from video, allowing receivers to demux streams
on the same port by SSRC or payload type.

### Compact mode (`"streamMode": "compact"`)

4-byte header followed by audio payload:

```
[0] = 0xAA (magic byte — video NALUs never start with 0xAA)
[1] = 0x01 (audio packet type)
[2] = length high byte
[3] = length low byte
[4..] = audio payload
```

Payload is fragmented into chunks of `maxPayloadSize - 4` bytes if needed.

## Port behavior

- **`audioPort: 0`** (default): Audio packets share the video UDP socket and
  destination. Receivers demux by RTP payload type (RTP mode) or magic byte
  (compact mode).
- **`audioPort: N`** (N > 0): Audio is sent to a dedicated UDP port on the
  same destination host. A separate UDP socket is created. Clean separation;
  receivers bind two ports independently.

Audio destination IP always follows the video destination. When the video
server is changed at runtime (via HTTP API `apply_server`), audio
automatically follows because the audio thread reads the video destination
via a shared pointer.

## Architecture

```
Microphone/I2S ─► MI_AI device ─► MI_AI channel ─► GetFrame loop
                                                        │
                                              ┌─────────┴─────────┐
                                              │ SW G.711 encode   │
                                              │ (or pass-through  │
                                              │  for PCM)         │
                                              └─────────┬─────────┘
                                                        │
                                              ┌─────────┴─────────┐
                                              │ RTP packetize or  │
                                              │ compact header    │
                                              └─────────┬─────────┘
                                                        │
                                                   UDP sendto
```

### Threading model

Audio capture runs in a **separate thread** from the video streaming loop.
`MI_AI_GetFrame` blocks with a 100ms timeout per call. The audio thread is
created during `pipeline_start` and joined during `pipeline_stop`.

Thread safety: the audio thread shares the video UDP socket (when
`audioPort == 0`). `sendto`/`sendmsg` is thread-safe on Linux.

### Library loading

`libmi_ai.so` is loaded via `dlopen("libmi_ai.so", RTLD_NOW | RTLD_GLOBAL)`.
`RTLD_NOW` ensures all symbols are resolved at load time — if any are
missing, `dlopen` fails cleanly with a `dlerror()` message instead of
crashing at runtime. Nine MI_AI functions are resolved via `dlsym`.

If the library is not present on the target filesystem, audio init prints
a warning and continues without audio. The video pipeline is unaffected.

### MMA pool allocation

The SigmaStar MI_AI module requires private DMA buffer pools allocated
before device configuration. Two pools are created:

1. **Device pool** (64 KB): DMA buffers for the audio input hardware.
2. **Channel output pool** (variable): Ring buffer for captured frames.
   Size = `packNumPerFrm * bytesPerSample * channels * 2 * 8` (aligned to 4K).

Without these pools, `MI_AI_GetFrame` returns `MI_AI_ERR_NOBUF` (0xA004200D).

## HTTP API

All audio settings are exposed via the existing HTTP API with `MUT_RESTART`
mutability (changes require pipeline restart):

```
GET  /api/v1/get?audio.enabled
POST /api/v1/set?audio.enabled=true
POST /api/v1/set?audio.codec=g711a
POST /api/v1/set?audio.sampleRate=8000
POST /api/v1/set?audio.volume=80
POST /api/v1/set?outgoing.audioPort=5601
```

## Receiving audio

### With ffplay (RTP mode, separate audio port)

```bash
ffplay -nodisp -f rtp -i rtp://0.0.0.0:5601
```

### With GStreamer (RTP mode, separate audio port)

```bash
# G.711 A-law
gst-launch-1.0 udpsrc port=5601 ! \
  "application/x-rtp,encoding-name=PCMA,clock-rate=8000" ! \
  rtppcmadepay ! alawdec ! autoaudiosink

# Raw PCM
gst-launch-1.0 udpsrc port=5601 ! \
  "application/x-rtp,encoding-name=L16,clock-rate=8000,channels=1" ! \
  rtpL16depay ! autoaudiosink
```

## Hardware requirements

Audio requires an I2S audio source connected to the SoC's I2S pins. The
SSC30KQ/SSC338Q exposes I2S master pins, but board-level routing varies.
Common options:

- MEMS microphone breakout (INMP441, ICS-43434) on I2S
- Line-in codec (ES8388, NAU88C22) on I2S
- Digital PDM microphone (some SoC variants)

If no audio hardware is connected, `MI_AI_GetFrame` will return silence or
timeout. The audio thread will idle with no output. The video pipeline is
unaffected.

## Known limitations

- **No hardware encoding**: The SigmaStar MI_AI hardware encoder
  (`MI_AI_EnableAenc` / `MI_AI_SetAencAttr`) causes a segfault inside
  `MI_AI_GetFrame` on the SSC30KQ. This was confirmed with both
  `RTLD_LAZY` and `RTLD_NOW` loading — the crash is inside the vendor
  library, not from missing symbols. All encoding is done in software.

- **No G.726 codec**: G.726 (ADPCM) would require a ~200 LOC software
  encoder with per-sample state tracking. Not implemented. Use G.711
  instead (same bandwidth class for voice).

- **No Opus codec**: Would require cross-compiling `libopus`, violating
  the project's no-external-dependency policy.

- **Maruko backend**: Audio is not supported on the Maruko backend.
  Enabling audio on Maruko prints a warning and continues without audio.

- **Single audio source**: Only device 0, channel 0 is used. No
  multi-channel or multi-device support.
