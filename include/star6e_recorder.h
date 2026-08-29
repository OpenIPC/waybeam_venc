#ifndef STAR6E_RECORDER_H
#define STAR6E_RECORDER_H

#include "star6e.h"

#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#define RECORDER_PATH_MAX 256
#define RECORDER_DEFAULT_DIR "/mnt/mmcblk0p1"
#define RECORDER_SYNC_DEFAULT_FRAMES 900
/* Stop recording when free space drops below this threshold (bytes). */
#define RECORDER_MIN_FREE_BYTES (50ULL * 1024 * 1024)
/* Check disk space every N frames to avoid syscall overhead. */
#define RECORDER_SPACE_CHECK_INTERVAL 300

typedef enum {
	RECORDER_STOP_MANUAL = 0,
	RECORDER_STOP_DISK_FULL,
	RECORDER_STOP_WRITE_ERROR,
} Star6eRecorderStopReason;

typedef struct {
	int fd;
	/* Set by start(), cleared by every stop.  This recorder does not
	 * rotate, so unlike the TS one it never has a transient fd == -1 — but
	 * it DOES stop itself from the recorder writer thread on ENOSPC or a
	 * write error, while the encode loop and the httpd thread read the
	 * descriptor.  A plain int store racing two unsynchronised loads leaves
	 * the producer pushing into a closed recorder, where the frames are
	 * discarded and counted nowhere.  Accessed atomically for that. */
	int recording;
	uint64_t bytes_written;
	uint32_t frames_written;
	uint32_t sync_interval_frames;
	uint32_t frames_since_sync;
	uint32_t space_check_countdown;
	Star6eRecorderStopReason last_stop_reason;
	struct timespec start_time;
	char dir[RECORDER_PATH_MAX];
	char path[RECORDER_PATH_MAX];

	/* Guards the status-visible fields above (recording, counters,
	 * last_stop_reason, start_time, path) so a poll on the httpd thread
	 * gets ONE coherent instant instead of a mix.  bytes_written is 64-bit
	 * and these targets are ARM32, so an unsynchronised load can also tear
	 * outright; path is rewritten wholesale on a TS rotation.
	 *
	 * Held only across field updates -- NEVER across write(), open(),
	 * fdatasync() or close().  A status poll must not be able to block on
	 * the disk; that is the coupling the async writer exists to remove. */
	pthread_mutex_t status_lock;
	/* The status callback is published before the recorders are
	 * initialised on some backends, so a poll can arrive before
	 * status_lock exists.  Zero from the memset means "not yet": the
	 * snapshot reports inactive rather than locking an uninitialised
	 * mutex. */
	int status_lock_ready;
} Star6eRecorderState;

/* One coherent instant of a recorder's status, for a reader on another
 * thread.  Shared by both recorders; `segments` stays 0 for the raw one. */
typedef struct {
	int      active;
	uint64_t bytes_written;
	uint32_t frames_written;
	uint32_t segments;
	uint64_t elapsed_ms;
	Star6eRecorderStopReason last_stop_reason;
	char     path[RECORDER_PATH_MAX];
} Star6eRecorderSnapshot;

/** Milliseconds since a recorder's start_time on CLOCK_MONOTONIC.
 *  Both recorders stamp start_time at start and neither exposes elapsed
 *  through its status accessor; this keeps the two runtime backends from
 *  each rolling their own subtraction. Meaningless unless the recorder is
 *  active — callers check that first. */
static inline uint64_t star6e_recorder_elapsed_ms(const struct timespec *start)
{
	struct timespec now;
	if (!start || (start->tv_sec == 0 && start->tv_nsec == 0)) return 0;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t ms = (int64_t)(now.tv_sec - start->tv_sec) * 1000 +
	             (now.tv_nsec - start->tv_nsec) / 1000000;
	return ms > 0 ? (uint64_t)ms : 0;
}

/** Zero-initialize recorder state (no file open). */
void star6e_recorder_init(Star6eRecorderState *state);

/** Begin recording to a timestamped .hevc file in dir.
 *  Returns 0 on success, -1 on error.
 *  If already recording, stops the current recording first. */
int star6e_recorder_start(Star6eRecorderState *state, const char *dir);

/** Write one already-assembled Annex-B access unit to the recording file.
 *  The SoC-independent writer: same side-effect order as
 *  star6e_recorder_write_frame() — periodic disk-space check, then the
 *  write, then the counters and the sync_file_range cadence, with a
 *  truncate back to the frame boundary if the write fails part-way so the
 *  file never retains half an AU.
 *
 *  For backends that already hand over one contiguous buffer (CV610 copies
 *  the SDK's pack list into `frame` before it reaches any consumer), this is
 *  the whole recorder interface — no SDK-typed adapter is needed.
 *
 *  No-op if not currently recording.  Returns bytes written, 0 if not
 *  active, or -1 on error.  Automatically stops recording on disk full or
 *  write error. */
int star6e_recorder_write_au(Star6eRecorderState *state,
	const uint8_t *au, size_t len);

#if !defined(PLATFORM_MARUKO) && !defined(PLATFORM_CV610)
/** Write one encoded frame (all NAL units) to the recording file.
 *  No-op if not currently recording.  Returns bytes written, 0 if not
 *  active, or -1 on error.  Automatically stops recording on disk full
 *  or write error.
 *
 *  SigmaStar-typed, so it is compiled only where star6e_output.c is linked
 *  (the Star6E target and the host test build).  Maruko has its own adapter
 *  in maruko_recorder.c; CV610 uses star6e_recorder_write_au() directly. */
int star6e_recorder_write_frame(Star6eRecorderState *state,
	const MI_VENC_Stream_t *stream);
#endif

/** Stop recording: fsync and close the file.  No-op if not recording. */
void star6e_recorder_stop(Star6eRecorderState *state);

/** Return 1 if actively recording, 0 otherwise. */
int star6e_recorder_is_active(const Star6eRecorderState *state);

/* "A recording is in progress."  For this recorder that is the same *moment*
 * as is_active() — there is no rotation to open a gap — but it is the safe
 * one to ask from another thread, and it is the predicate a producer should
 * gate on so the two recorders are asked the same question.  Contrast
 * star6e_ts_recorder_is_recording(), where the two genuinely differ. */
int star6e_recorder_is_recording(const Star6eRecorderState *state);

/** Get current recording status.  Any output pointer may be NULL. */
/** Guard the status-visible fields of a Star6eRecorderState.
 *
 *  Any translation unit that mutates recording, the counters,
 *  last_stop_reason, start_time or path MUST bracket the update with these:
 *  a mutex only helps if every writer takes it, and maruko_recorder.c writes
 *  the same fields from the dual-mode ch1 thread.
 *
 *  Never wrap write(), open(), close() or fdatasync() in them -- a status
 *  poll must not be able to block on the disk. */
static inline void star6e_recorder_status_lock(Star6eRecorderState *state)
{
	if (state && state->status_lock_ready)
		pthread_mutex_lock(&state->status_lock);
}

static inline void star6e_recorder_status_unlock(Star6eRecorderState *state)
{
	if (state && state->status_lock_ready)
		pthread_mutex_unlock(&state->status_lock);
}

/** Copy one coherent instant of the recorder's status.  Safe to call from a
 *  thread other than the writer; `out` is zeroed when `state` is NULL or not
 *  yet initialised. */
void star6e_recorder_snapshot(Star6eRecorderState *state,
	Star6eRecorderSnapshot *out);

void star6e_recorder_status(const Star6eRecorderState *state,
	uint64_t *bytes_written, uint32_t *frames_written,
	const char **path, Star6eRecorderStopReason *last_stop_reason);

/** Check available space on the filesystem containing path.
 *  Returns free bytes, or 0 on error. */
uint64_t star6e_recorder_free_space(const char *path);

#endif /* STAR6E_RECORDER_H */
