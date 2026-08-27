#ifndef STAR6E_RECORDER_H
#define STAR6E_RECORDER_H

#include "star6e.h"

#include <stddef.h>
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
	uint64_t bytes_written;
	uint32_t frames_written;
	uint32_t sync_interval_frames;
	uint32_t frames_since_sync;
	uint32_t space_check_countdown;
	Star6eRecorderStopReason last_stop_reason;
	struct timespec start_time;
	char dir[RECORDER_PATH_MAX];
	char path[RECORDER_PATH_MAX];
} Star6eRecorderState;

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

/** Get current recording status.  Any output pointer may be NULL. */
void star6e_recorder_status(const Star6eRecorderState *state,
	uint64_t *bytes_written, uint32_t *frames_written,
	const char **path, Star6eRecorderStopReason *last_stop_reason);

/** Check available space on the filesystem containing path.
 *  Returns free bytes, or 0 on error. */
uint64_t star6e_recorder_free_space(const char *path);

#endif /* STAR6E_RECORDER_H */
