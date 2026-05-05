#ifndef TS_MUX_H
#define TS_MUX_H

/*
 * MPEG-TS muxer — stateless 188-byte packet emitter.
 * Builds TS packets into caller-provided buffers.
 * No I/O, no allocations — fully unit-testable on host.
 */

#include <stddef.h>
#include <stdint.h>

#define TS_PACKET_SIZE    188
#define TS_SYNC_BYTE      0x47

#define TS_PID_PAT        0x0000
#define TS_PID_PMT        0x1000
#define TS_PID_VIDEO      0x0100
#define TS_PID_AUDIO      0x0101

/* HEVC stream type per ISO/IEC 13818-1 */
#define TS_STREAM_TYPE_HEVC      0x24
/* Private data — used for both SMPTE 302M and Opus audio */
#define TS_STREAM_TYPE_PRIVATE   0x06

/* PAT/PMT re-emission interval in video frames */
#define TS_PAT_PMT_INTERVAL  15

/* Audio codec selection.
 *   PCM_S302M — 16-bit PCM packed per SMPTE 302M (BSSD descriptor).  Default
 *               and only universally-decodable PCM-in-TS form.
 *   OPUS      — Opus packets per the Opus-in-MPEG-TS mapping.  Requires the
 *               caller to feed already-encoded Opus access units to
 *               ts_mux_write_audio (one packet per call).
 * G.711 is intentionally not supported as a recorded codec — there is no
 * in-band TS framing that VLC/ffmpeg/mpv decode without external hints. */
#define TS_AUDIO_CODEC_PCM_S302M  0
#define TS_AUDIO_CODEC_OPUS       1

typedef struct {
	uint8_t cc_pat;          /* continuity counter for PAT */
	uint8_t cc_pmt;          /* continuity counter for PMT */
	uint8_t cc_video;        /* continuity counter for video PID */
	uint8_t cc_audio;        /* continuity counter for audio PID */
	uint32_t video_frames;   /* frames since last PAT/PMT emission */
	uint32_t audio_rate;     /* audio sample rate */
	uint8_t audio_channels;  /* audio channel count */
	uint8_t audio_codec;     /* TS_AUDIO_CODEC_* */
	uint8_t discontinuity;   /* set 1 to emit discontinuity_indicator on next video */
	uint32_t pcr_offset;     /* PCR leads PTS by this many 90kHz ticks */
} TsMuxState;

/** Zero-initialize mux state.  audio_codec selects how PMT and PES frames
 *  are emitted; for SMPTE 302M the muxer packs raw s16le samples itself,
 *  for Opus the caller passes already-encoded Opus packets. */
void ts_mux_init(TsMuxState *s, uint32_t audio_rate, uint8_t audio_channels,
	uint8_t audio_codec);

/** Reset continuity counters for a new segment. */
void ts_mux_reset_cc(TsMuxState *s);

/** Write PAT + PMT packets into buf.  Returns bytes written (2 * 188). */
size_t ts_mux_write_pat_pmt(TsMuxState *s, uint8_t *buf, size_t buf_size);

/** Write video PES as TS packets.  Embeds PCR in first packet's adaptation
 *  field.  Emits PAT/PMT if interval reached.
 *  pts_90khz: presentation timestamp in 90kHz timebase.
 *  is_idr: 1 if this is a random access point (sets RAI flag).
 *  Returns bytes written (multiple of 188). */
size_t ts_mux_write_video(TsMuxState *s, uint8_t *buf, size_t buf_size,
	const uint8_t *data, size_t data_len,
	uint64_t pts_90khz, int is_idr);

/** Write audio PES as TS packets.
 *  pts_90khz: presentation timestamp in 90kHz timebase.
 *  Returns bytes written (multiple of 188). */
size_t ts_mux_write_audio(TsMuxState *s, uint8_t *buf, size_t buf_size,
	const uint8_t *data, size_t data_len,
	uint64_t pts_90khz);

/** Convert CLOCK_MONOTONIC timespec to 90kHz PTS value. */
static inline uint64_t ts_mux_timespec_to_pts(uint32_t sec, uint32_t nsec)
{
	return (uint64_t)sec * 90000ULL + (uint64_t)nsec / 11111ULL;
}

#endif /* TS_MUX_H */
