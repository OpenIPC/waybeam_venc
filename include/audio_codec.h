#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

/*
 * Platform-agnostic audio encoder helpers.
 *
 * Backed by:
 *   - Inline G.711 A-law / µ-law conversion (no external lib).
 *   - Lazy `dlopen("libopus.so")` for Opus — caller manages the encoder
 *     handle but uses these helpers to load symbols and encode.
 *
 * Both Star6E (`src/star6e_audio.c`) and Maruko (`src/maruko_audio.c`) link
 * against this so the codec table stays in one place.
 */

#include <stdint.h>
#include <stddef.h>

#define AUDIO_CODEC_TYPE_RAW    (-1)
#define AUDIO_CODEC_TYPE_G711A  0
#define AUDIO_CODEC_TYPE_G711U  1
#define AUDIO_CODEC_TYPE_OPUS   2

/* OPUS_APPLICATION_AUDIO — non-voice content at medium bitrates. */
#define AUDIO_CODEC_OPUS_APPLICATION_AUDIO  2049
/* RFC 7587: Opus RTP payload uses a 48 kHz nominal clock for timestamps. */
#define AUDIO_CODEC_OPUS_RTP_CLOCK_HZ       48000

/** Map a codec name string to AUDIO_CODEC_TYPE_*.  Returns AUDIO_CODEC_TYPE_RAW
 *  when the string is "pcm" or unrecognized. */
int audio_codec_parse_name(const char *name);

/** G.711 encode one PCM frame.  `pcm` is little-endian S16; `out` must hold
 *  at least `num_samples` bytes.  Returns the number of bytes written
 *  (always == num_samples for G.711 8-bit).  `codec_type` must be
 *  AUDIO_CODEC_TYPE_G711A or AUDIO_CODEC_TYPE_G711U. */
size_t audio_codec_encode_g711(const int16_t *pcm, size_t num_samples,
	uint8_t *out, int codec_type);

/* Opus support */

typedef struct {
	void *lib;            /* dlopen handle for libopus.so (NULL when unavailable) */
	void *encoder;        /* OpusEncoder * */
	int32_t (*encode)(void *enc, const int16_t *pcm, int frame_size,
		uint8_t *out, int32_t max_bytes);
	void    (*destroy)(void *enc);
} AudioCodecOpus;

/** Initialize Opus encoder.  Looks up libopus.so, creates an encoder for the
 *  given sample_rate / channels.  On any failure logs a warning and returns
 *  -1 with `*opus` left zeroed (caller is expected to fall back to raw PCM). */
int audio_codec_opus_init(AudioCodecOpus *opus, uint32_t sample_rate,
	uint32_t channels);

/** Tear down Opus encoder.  Safe to call when init was never run or failed. */
void audio_codec_opus_teardown(AudioCodecOpus *opus);

/* Stdout filter: libmi_ai.so / libmi_isp.so write yellow ANSI noise to stdout
 * from internal threads.  These helpers swap fd 1 with a pipe and drop lines
 * starting with ESC (0x1B).  Use stdout_filter_real_fd() with dprintf() so
 * verbose output never stalls on a full pipe.  Single-instance per process —
 * counted: start+stop pair from each backend; only first start installs and
 * only the last stop tears down. */

void audio_codec_stdout_filter_start(void);
void audio_codec_stdout_filter_stop(void);
int  audio_codec_stdout_filter_real_fd(void);

#endif /* AUDIO_CODEC_H */
