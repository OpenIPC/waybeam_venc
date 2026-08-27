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

	/* Segment rotation can only cut on an IRAP, so on a stream that emits
	 * none of its own (GDR / resilience=racing) it has to ask for one.
	 *
	 * check_rotation() raises a FLAG rather than calling into the SDK
	 * itself, and the backend services it after its ReleaseStream. Calling
	 * from here would be wrong twice over:
	 *   - it would land inside the SDK's GetStream/ReleaseStream window,
	 *     which no existing IDR call site on any backend does;
	 *   - a single shared hook cannot name the right channel. In
	 *     record.mode=dual the recorder is fed by ch1 while the hook
	 *     targets ch0, so the request would keyframe the LIVE stream and
	 *     do nothing for the file. star6e_runtime.c already carries that
	 *     fix for record-start; this avoids reintroducing it.
	 *
	 * The backend must use the COALESCED request path, not the forced one:
	 * a periodic rotation is not a bootstrap event, and forcing would
	 * re-arm the rate limiter and swallow a genuinely requested keyframe
	 * arriving just after.
	 *
	 * Left unserviced, rotation keeps the pre-0.70.0 behaviour: it waits
	 * for a natural keyframe and never fires if none comes. */
	int idr_request_pending;          /* backend polls and clears */
	time_t idr_request_last_sec;      /* pacing anchor */
	uint32_t idr_requests_unanswered; /* 0 = none outstanding */
	/* When the thresholds were FIRST seen crossed.  A keyframing stream
	 * rotates on its own within a GOP, so asking immediately buys nothing
	 * and costs a keyframe: measured on Maruko at resilience=off with a
	 * 1 s GOP, max_seconds=15 over 50 s went from 1 honored IDR to 4 —
	 * three needless keyframes on the live link for three rotations that
	 * would have happened anyway.  Waiting one second before the first ask
	 * lets such a stream rotate naturally and leaves the request for the
	 * streams that genuinely never produce one. */
	time_t rotation_due_since;
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

/** Stop asking after this many unanswered requests and let rotation go back
 *  to waiting for a natural keyframe.  Without a bound, a mis-wired hook or a
 *  keyframe that never reads back as an IRAP would tax the live link with a
 *  forced keyframe every second for the whole recording — and the file would
 *  still grow unbounded, which is the defect this feature exists to fix. */
#define TS_RECORDER_MAX_IDR_REQUESTS 8

/** Take a pending rotation IDR request, if any.
 *
 *  Call AFTER the SDK's ReleaseStream, and issue a COALESCED IDR request on
 *  the channel that feeds THIS recorder — ch1 under record.mode=dual, not the
 *  main channel. Returns 1 when a request is due (and clears it), 0 otherwise.
 *
 *  Atomic because on backends whose recorder runs on the async writer thread
 *  the flag is raised there and consumed on the encode loop. */
static inline int star6e_ts_recorder_take_idr_request(
	Star6eTsRecorderState *state)
{
	if (!state ||
	    !__atomic_load_n(&state->idr_request_pending, __ATOMIC_RELAXED))
		return 0;
	__atomic_store_n(&state->idr_request_pending, 0, __ATOMIC_RELAXED);
	return 1;
}

/** Get recording status. Any output pointer may be NULL. */
void star6e_ts_recorder_status(const Star6eTsRecorderState *state,
	uint64_t *bytes_written, uint32_t *frames_written,
	uint32_t *segments, const char **path,
	Star6eRecorderStopReason *last_stop_reason);

#if !defined(PLATFORM_MARUKO) && !defined(PLATFORM_CV610)
/** Convenience: extract NAL data from MI_VENC_Stream_t and write as TS.
 *  Handles IDR detection and PTS from CLOCK_MONOTONIC.
 *  No-op if not active.
 *
 *  SigmaStar-typed, so it is compiled only where star6e_output.c is linked
 *  (the Star6E target and the host test build).  Maruko has
 *  maruko_ts_recorder_write_stream(); CV610 already holds a contiguous AU and
 *  calls star6e_ts_recorder_write_video() directly. */
int star6e_ts_recorder_write_stream(Star6eTsRecorderState *state,
	const MI_VENC_Stream_t *stream);
#endif

#endif /* STAR6E_TS_RECORDER_H */
