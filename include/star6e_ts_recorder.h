#ifndef STAR6E_TS_RECORDER_H
#define STAR6E_TS_RECORDER_H

#include "audio_ring.h"
#include "star6e.h"
#include "star6e_recorder.h"
#include "ts_mux.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Default rotation thresholds */
#define TS_RECORDER_DEFAULT_MAX_SECONDS  300
#define TS_RECORDER_DEFAULT_MAX_BYTES    (500ULL * 1024 * 1024)

typedef struct {
	int fd;
	uint64_t bytes_written;
	uint32_t frames_written;
	uint32_t segments;            /* number of .ts files produced */
	uint32_t sync_interval_frames;
	uint32_t frames_since_sync;
	uint32_t space_check_countdown;
	Star6eRecorderStopReason last_stop_reason;
	struct timespec start_time;
	struct timespec segment_start_time;
	char dir[RECORDER_PATH_MAX];
	char path[RECORDER_PATH_MAX];

	/* TS mux state */
	TsMuxState mux;

	/* Audio ring (owned by caller, may be NULL) */
	AudioRing *audio_ring;

	/* Rotation config */
	uint32_t max_seconds;
	uint64_t max_bytes;

	/* Per-segment counters */
	uint64_t segment_bytes;

	/* IDR request callback for segment rotation (NULL if not wired) */
	int (*request_idr)(void);
} Star6eTsRecorderState;

/** Zero-initialize TS recorder state.
 *  audio_codec selects how audio is muxed in TS: TS_AUDIO_CODEC_PCM_S302M
 *  packs raw s16le samples; TS_AUDIO_CODEC_OPUS expects pre-encoded Opus
 *  access units pushed into the audio ring. */
void star6e_ts_recorder_init(Star6eTsRecorderState *state,
	uint32_t audio_rate, uint8_t audio_channels, uint8_t audio_codec);

/** Begin recording to a timestamped .ts file.  Returns 0 on success. */
int star6e_ts_recorder_start(Star6eTsRecorderState *state, const char *dir,
	AudioRing *audio_ring);

/** Write one video frame (with interleaved audio from ring).
 *  is_idr: 1 if this frame is a keyframe.
 *  Returns bytes written, 0 if inactive, -1 on error. */
int star6e_ts_recorder_write_video(Star6eTsRecorderState *state,
	const uint8_t *video_data, size_t video_len,
	uint64_t pts_90khz, int is_idr);

/** Stop recording: fsync and close. */
void star6e_ts_recorder_stop(Star6eTsRecorderState *state);

/** Return 1 if actively recording. */
int star6e_ts_recorder_is_active(const Star6eTsRecorderState *state);

/** Get recording status. Any output pointer may be NULL. */
void star6e_ts_recorder_status(const Star6eTsRecorderState *state,
	uint64_t *bytes_written, uint32_t *frames_written,
	uint32_t *segments, const char **path,
	Star6eRecorderStopReason *last_stop_reason);

/** Convenience: extract NAL data from MI_VENC_Stream_t and write as TS.
 *  Handles IDR detection and PTS from CLOCK_MONOTONIC.
 *  No-op if not active. */
int star6e_ts_recorder_write_stream(Star6eTsRecorderState *state,
	const MI_VENC_Stream_t *stream);

#endif /* STAR6E_TS_RECORDER_H */
