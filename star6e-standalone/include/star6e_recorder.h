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

/** Zero-initialize recorder state (no file open). */
void star6e_recorder_init(Star6eRecorderState *state);

/** Begin recording to a timestamped .hevc file in dir.
 *  Returns 0 on success, -1 on error.
 *  If already recording, stops the current recording first. */
int star6e_recorder_start(Star6eRecorderState *state, const char *dir);

/** Write one encoded frame (all NAL units) to the recording file.
 *  No-op if not currently recording.  Returns bytes written, 0 if not
 *  active, or -1 on error.  Automatically stops recording on disk full
 *  or write error. */
int star6e_recorder_write_frame(Star6eRecorderState *state,
	const MI_VENC_Stream_t *stream);

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
