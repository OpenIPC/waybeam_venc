#include "star6e_recorder.h"
/* The SigmaStar-typed adapters below need star6e_output.c, which only the
 * Star6E target and the host test build link.  Maruko has its own adapter
 * in maruko_recorder.c / maruko_ts_recorder.c; CV610 needs none, because it
 * hands the recorder one contiguous access unit and calls the SoC-
 * independent entry points directly.  Stated as "not those two" rather than
 * "PLATFORM_STAR6E" because the host test build defines no platform macro
 * at all and must keep compiling these. */
#if !defined(PLATFORM_MARUKO) && !defined(PLATFORM_CV610)
#include "star6e_output.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <unistd.h>

void star6e_recorder_init(Star6eRecorderState *state)
{
	if (!state)
		return;

	memset(state, 0, sizeof(*state));
	state->fd = -1;
	state->sync_interval_frames = RECORDER_SYNC_DEFAULT_FRAMES;
	state->last_stop_reason = RECORDER_STOP_MANUAL;
	state->rot.max_seconds = RECORDER_DEFAULT_MAX_SECONDS;
	state->rot.max_bytes = RECORDER_DEFAULT_MAX_BYTES;
	state->rot.write_headroom = RECORDER_RAW_WRITE_HEADROOM;
	if (pthread_mutex_init(&state->status_lock, NULL) == 0)
		state->status_lock_ready = 1;
}

uint64_t star6e_recorder_free_space(const char *path)
{
	struct statvfs st;

	if (!path || !path[0])
		return 0;

	if (statvfs(path, &st) != 0)
		return 0;

	return (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
}

/* ── Shared rotation policy ──────────────────────────────────────────── */

void recorder_rotation_segment_opened(RecorderRotation *rot,
	uint64_t initial_bytes)
{
	if (!rot)
		return;
	clock_gettime(CLOCK_MONOTONIC, &rot->segment_start_time);
	rot->segment_bytes = initial_bytes;
}

int recorder_rotation_due(RecorderRotation *rot, int is_cut_point)
{
	struct timespec now;
	unsigned long elapsed;
	int should_rotate = 0;

	if (!rot)
		return 0;

	/* Check time-based rotation */
	if (rot->max_seconds > 0) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (unsigned long)(now.tv_sec -
			rot->segment_start_time.tv_sec);
		if (elapsed >= rot->max_seconds)
			should_rotate = 1;
	}

	/* Check size-based rotation.
	 *
	 * Rotate on whichever limit binds first: the operator's, or the one
	 * this binary's off_t can actually reach.  A configured max_mb above
	 * the ceiling is not an error to reject -- the operator asked for big
	 * segments and gets the biggest writable ones -- and max_bytes == 0
	 * ("no size limit") still cannot mean past the ceiling.  One write of
	 * headroom keeps the write that follows this check on the near side of
	 * the wall.  Inert on a 64-bit off_t build, where the ceiling is
	 * UINT64_MAX and this is always just max_bytes. */
	{
		uint64_t ceiling = RECORDER_OFF_T_CEILING - rot->write_headroom;
		uint64_t limit = rot->max_bytes;

		if (limit == 0 || limit > ceiling)
			limit = ceiling;
		if (rot->segment_bytes >= limit)
			should_rotate = 1;
	}

	if (!should_rotate) {
		rot->rotation_due_since = 0;
		rot->warned_no_cut_point = 0;
		return 0;
	}

	/* Threshold crossed.  WAIT for a point a decoder can start from; never
	 * ask the encoder to manufacture one.  An IDR is a large frame, and one
	 * per segment raises the bitrate the link has to carry -- on a GDR
	 * craft that undoes the very thing intra-refresh is for, and in
	 * record.mode=mirror it would spike the LIVE channel for the benefit of
	 * a file. */
	if (!is_cut_point) {
		if (rot->rotation_due_since == 0) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			rot->rotation_due_since = now.tv_sec;
			return 0;
		}
		/* A cut point is due once per GOP on every stream shape these
		 * encoders produce -- an IRAP when keyframing, a parameter-set
		 * boundary under intra-refresh.  If one has not arrived long
		 * after the threshold, something is emitting neither, and the
		 * operator's max_seconds/max_mb are silently not happening.
		 * Say so once rather than growing the file in silence. */
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (!rot->warned_no_cut_point &&
		    now.tv_sec - rot->rotation_due_since >= 10) {
			rot->warned_no_cut_point = 1;
			fprintf(stderr,
				"[recorder] rotation due for 10s but the stream has "
				"produced no IRAP and no parameter sets to cut on; "
				"the segment will keep growing\n");
		}
		return 0;
	}

	rot->rotation_due_since = 0;
	rot->warned_no_cut_point = 0;
	return 1;
}

/* Does this Annex-B access unit BEGIN with a parameter set?
 *
 * Only the first NAL is examined, which is both cheap and correct: VPS/SPS/PPS
 * lead the access unit they configure -- measured on Star6E, every IRAP was
 * immediately preceded by VPS+SPS+PPS, and every GDR refresh wave head was a
 * VPS.  A parameter set buried mid-AU would not be a place a segment could
 * open anyway. */
static int au_leads_with_param_sets(const uint8_t *au, size_t len)
{
	size_t i = 0;

	if (!au)
		return 0;
	/* Annex-B start code: 00 00 01 or 00 00 00 01. */
	if (len >= 4 && au[0] == 0 && au[1] == 0 && au[2] == 0 && au[3] == 1)
		i = 4;
	else if (len >= 3 && au[0] == 0 && au[1] == 0 && au[2] == 1)
		i = 3;
	else
		return 0;
	if (i >= len)
		return 0;
	return recorder_nal_is_cut_point((unsigned int)((au[i] >> 1) & 0x3F));
}

static int build_recording_path(char *path, size_t path_size, const char *dir)
{
	struct timespec ts;
	unsigned long uptime_s;
	unsigned int hours, mins, secs;
	unsigned int rand_suffix;
	const char *sep;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	uptime_s = (unsigned long)ts.tv_sec;
	hours = (unsigned int)(uptime_s / 3600);
	mins = (unsigned int)((uptime_s % 3600) / 60);
	secs = (unsigned int)(uptime_s % 60);

	/* Derive suffix from nanoseconds — avoids reseeding global rand(). */
	rand_suffix = (unsigned int)(ts.tv_nsec / 1000) & 0xFFFF;

	sep = (dir[strlen(dir) - 1] == '/') ? "" : "/";
	snprintf(path, path_size, "%s%srec_%02uh%02um%02us_%04x.hevc",
		dir, sep, hours, mins, secs, rand_suffix);

	return 0;
}

#if !defined(PLATFORM_MARUKO) && !defined(PLATFORM_CV610)
static ssize_t writev_all(int fd, const struct iovec *iov, int iov_count)
{
	struct iovec pending[16];
	ssize_t total = 0;
	int first = 0;

	if (!iov || iov_count <= 0 || iov_count > 16) {
		errno = EINVAL;
		return -1;
	}
	memcpy(pending, iov, (size_t)iov_count * sizeof(pending[0]));
	while (first < iov_count) {
		ssize_t ret;
		ssize_t consumed;

		do {
			ret = writev(fd, &pending[first], iov_count - first);
		} while (ret < 0 && errno == EINTR);
		if (ret < 0)
			return -1;
		if (ret == 0) {
			errno = EIO;
			return -1;
		}
		total += ret;
		consumed = ret;
		while (first < iov_count &&
		    consumed >= (ssize_t)pending[first].iov_len) {
			consumed -= (ssize_t)pending[first].iov_len;
			first++;
		}
		if (first < iov_count && consumed > 0) {
			pending[first].iov_base =
				(char *)pending[first].iov_base + consumed;
			pending[first].iov_len -= (size_t)consumed;
		}
	}

	return total;
}
#endif

static const char *stop_reason_str(Star6eRecorderStopReason reason)
{
	if (reason == RECORDER_STOP_DISK_FULL)
		return "disk full";
	if (reason == RECORDER_STOP_WRITE_ERROR)
		return "write error";
	if (reason == RECORDER_STOP_SIZE_LIMIT)
		return "size limit";
	return "manual";
}

static void stop_with_reason(Star6eRecorderState *state,
	Star6eRecorderStopReason reason)
{
	if (!state)
		return;
	/* Cleared before the fd test: a stop that lands on an already-closed
	 * recorder must still end the recording. */
	star6e_recorder_status_lock(state);
	__atomic_store_n(&state->recording, 0, __ATOMIC_RELEASE);
	star6e_recorder_status_unlock(state);
	if (state->fd < 0)
		return;

	fdatasync(state->fd);
	close(state->fd);
	state->fd = -1;
	star6e_recorder_status_lock(state);
	state->last_stop_reason = reason;
	star6e_recorder_status_unlock(state);

	fprintf(stderr, "[recorder] stopped (%s): %s (%u frames, %llu bytes)\n",
		stop_reason_str(reason), state->path, state->frames_written,
		(unsigned long long)state->bytes_written);
}

int star6e_recorder_start(Star6eRecorderState *state, const char *dir)
{
	uint64_t free_bytes;

	if (!state || !dir || !dir[0])
		return -1;

	if (state->fd >= 0)
		star6e_recorder_stop(state);

	/* Pre-flight: check disk space before opening file */
	free_bytes = star6e_recorder_free_space(dir);
	if (free_bytes > 0 && free_bytes < RECORDER_MIN_FREE_BYTES) {
		fprintf(stderr, "[recorder] insufficient space on %s "
			"(%llu bytes free, need %llu)\n",
			dir, (unsigned long long)free_bytes,
			(unsigned long long)RECORDER_MIN_FREE_BYTES);
		star6e_recorder_status_lock(state);
		state->last_stop_reason = RECORDER_STOP_DISK_FULL;
		star6e_recorder_status_unlock(state);
		return -1;
	}

	snprintf(state->dir, sizeof(state->dir), "%s", dir);
	star6e_recorder_status_lock(state);
	build_recording_path(state->path, sizeof(state->path), dir);
	star6e_recorder_status_unlock(state);

	/* O_EXCL for the same reason as open_next_segment(): the generated name
	 * is not collision-proof and truncating would destroy an earlier
	 * recording.  A collision here is a failed start, which is loud, rather
	 * than silent data loss. */
	state->fd = open(state->path,
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (state->fd < 0) {
		fprintf(stderr, "[recorder] open %s failed: %s\n",
			state->path, strerror(errno));
		star6e_recorder_status_lock(state);
		state->path[0] = '\0';
		star6e_recorder_status_unlock(state);
		return -1;
	}

	recorder_rotation_segment_opened(&state->rot, 0);
	/* Part of the per-recording reset: a restart must not inherit the old
	 * recording's "threshold has been crossed since" anchor, or its warning
	 * latch.  The TS recorder clears the same pair in its start(). */
	state->rot.rotation_due_since = 0;
	state->rot.warned_no_cut_point = 0;
	star6e_recorder_status_lock(state);
	state->bytes_written = 0;
	state->frames_written = 0;
	state->segments = 1;
	state->frames_since_sync = 0;
	state->space_check_countdown = RECORDER_SPACE_CHECK_INTERVAL;
	state->last_stop_reason = RECORDER_STOP_MANUAL;
	clock_gettime(CLOCK_MONOTONIC, &state->start_time);
	/* Published last, once the file is genuinely open -- and inside the
	 * same section as the counters, so a poll cannot catch "not recording,
	 * stopped manually" naming a file that is about to start. */
	__atomic_store_n(&state->recording, 1, __ATOMIC_RELEASE);
	star6e_recorder_status_unlock(state);

	fprintf(stderr, "[recorder] started: %s\n", state->path);
	return 0;
}

/* Open the next .hevc segment.  The raw counterpart of the TS recorder's
 * open_new_segment(): no PAT/PMT to regenerate, because an elementary stream
 * carries its own parameter sets -- device-measured on Star6E, every IRAP
 * access unit is immediately preceded by VPS+SPS+PPS, so a segment cut on one
 * opens self-sufficiently. */
static int open_next_segment(Star6eRecorderState *state)
{
	char newpath[RECORDER_PATH_MAX];
	int attempt;

	/* Built into a LOCAL and published together with the segment count, so
	 * a status poll cannot pair the new path with the old count.
	 *
	 * O_EXCL, not O_TRUNC: the name carries only uptime seconds plus 16
	 * bits derived from the nanosecond clock, so it is not collision-proof
	 * -- and after a reboot the uptime restarts, which can reproduce a name
	 * left by an earlier session.  Truncating would silently destroy that
	 * recording.  Rotation multiplies the exposure (one name per segment
	 * now, not one per recording), so retry with a fresh name instead. */
	for (attempt = 0; attempt < 8; attempt++) {
		build_recording_path(newpath, sizeof(newpath), state->dir);
		state->fd = open(newpath,
			O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		if (state->fd >= 0)
			break;
		if (errno != EEXIST) {
			int saved_errno = errno;

			fprintf(stderr, "[recorder] open %s failed: %s\n",
				newpath, strerror(saved_errno));
			/* fprintf may clobber errno; the caller classifies on
			 * it (a card that filled between space checks must read
			 * as disk_full, not as a media error). */
			errno = saved_errno;
			return -1;
		}
	}
	if (state->fd < 0) {
		fprintf(stderr,
			"[recorder] could not find an unused segment name in %s\n",
			state->dir);
		return -1;
	}

	recorder_rotation_segment_opened(&state->rot, 0);

	star6e_recorder_status_lock(state);
	snprintf(state->path, sizeof(state->path), "%s", newpath);
	state->segments++;
	star6e_recorder_status_unlock(state);

	/* stderr like every other line this recorder emits, including
	 * "started" a few lines up: splitting one recording's progress across
	 * two streams would make it harder to follow, not easier. */
	fprintf(stderr, "[recorder] segment %u: %s\n",
		state->segments, state->path);
	return 0;
}

/* Cut a segment if one is due.  The WHEN is recorder_rotation_due(), shared
 * verbatim with the TS recorder; only the WHAT differs. */
int star6e_recorder_check_rotation(Star6eRecorderState *state,
	int is_cut_point)
{
	int finalise_failed;

	if (!state || state->fd < 0)
		return 0;
	if (!recorder_rotation_due(&state->rot, is_cut_point))
		return 0;

	/* Finalising the OLD segment can fail -- a delayed write surfacing at
	 * fdatasync(), or close() reporting one.  Reporting that as a clean
	 * rotation would leave the operator with a segment that may be short
	 * and no indication of it, so it is a stop, exactly like a failed
	 * reopen below. */
	finalise_failed = (fdatasync(state->fd) != 0);
	if (close(state->fd) != 0)
		finalise_failed = 1;
	state->fd = -1;
	if (finalise_failed) {
		fprintf(stderr,
			"[recorder] segment %u (%s) failed to finalise: %s\n",
			state->segments, state->path, strerror(errno));
		star6e_recorder_status_lock(state);
		__atomic_store_n(&state->recording, 0, __ATOMIC_RELEASE);
		state->last_stop_reason = RECORDER_STOP_WRITE_ERROR;
		star6e_recorder_status_unlock(state);
		return -1;
	}

	if (open_next_segment(state) != 0) {
		/* A rotation that cannot reopen is a stop, not a gap.  Keep the
		 * reason honest: the space check only runs every
		 * RECORDER_SPACE_CHECK_INTERVAL frames, so a card that filled in
		 * between arrives here as ENOSPC and must not be reported as a
		 * media error. */
		int reopen_errno = errno;

		star6e_recorder_status_lock(state);
		__atomic_store_n(&state->recording, 0, __ATOMIC_RELEASE);
		state->last_stop_reason = (reopen_errno == ENOSPC)
			? RECORDER_STOP_DISK_FULL : RECORDER_STOP_WRITE_ERROR;
		star6e_recorder_status_unlock(state);
		return -1;
	}
	/* The AU about to be written IS the cut point, so the new segment
	 * opens on it. */
	return 0;
}

static int check_disk_space(Star6eRecorderState *state)
{
	uint64_t free_bytes;

	if (state->space_check_countdown > 0) {
		state->space_check_countdown--;
		return 0;
	}

	state->space_check_countdown = RECORDER_SPACE_CHECK_INTERVAL;
	free_bytes = star6e_recorder_free_space(state->dir);

	if (free_bytes > 0 && free_bytes < RECORDER_MIN_FREE_BYTES) {
		fprintf(stderr, "[recorder] disk space low (%llu bytes), "
			"stopping recording\n",
			(unsigned long long)free_bytes);
		stop_with_reason(state, RECORDER_STOP_DISK_FULL);
		return -1;
	}

	return 0;
}

static ssize_t write_all(int fd, const uint8_t *data, size_t len)
{
	size_t total = 0;

	while (total < len) {
		ssize_t ret = write(fd, data + total, len - total);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (ret == 0) {
			errno = EIO;
			return -1;
		}
		total += (size_t)ret;
	}

	return (ssize_t)total;
}

int star6e_recorder_write_au(Star6eRecorderState *state,
	const uint8_t *au, size_t len, int is_idr)
{
	ssize_t written;
	off_t frame_start;

	if (!state || state->fd < 0 || !au || len == 0)
		return 0;

	/* Same order as star6e_recorder_write_frame() below: space check
	 * first (it can stop the recorder), then rotation -- which must run
	 * BEFORE the frame boundary is taken, because a cut replaces the fd
	 * and the offset it would have recorded -- then the write, then the
	 * counters and the sync cadence. */
	if (check_disk_space(state) != 0)
		return 0;
	if (star6e_recorder_check_rotation(state,
		    is_idr || au_leads_with_param_sets(au, len)) != 0)
		return -1;
	frame_start = lseek(state->fd, 0, SEEK_CUR);

	written = write_all(state->fd, au, len);
	if (written < 0) {
		int saved_errno = errno;

		/* A failure may follow a short successful write. Roll the
		 * regular file back to its frame boundary so it never retains
		 * half an AU. */
		if (frame_start >= 0)
			(void)ftruncate(state->fd, frame_start);
		errno = saved_errno;
		if (errno == ENOSPC) {
			fprintf(stderr, "[recorder] disk full (ENOSPC)\n");
			stop_with_reason(state, RECORDER_STOP_DISK_FULL);
		} else if (errno == EFBIG) {
			/* A file size ceiling, not bad media.  Reachable only
			 * when rotation could not cut in time -- the IDR ask is
			 * bounded, so a stream that answers none of them keeps
			 * one segment growing.  The rollback above has already
			 * put the file back on its frame boundary, so it stays
			 * playable; name the ceiling rather than blaming the
			 * card, as "write error" once did. */
			fprintf(stderr,
				"[recorder] file size limit reached at %llu bytes "
				"(EFBIG) -- lower record.maxMB\n",
				(unsigned long long)state->rot.segment_bytes);
			stop_with_reason(state, RECORDER_STOP_SIZE_LIMIT);
		} else {
			fprintf(stderr, "[recorder] write error: %s\n",
				strerror(errno));
			stop_with_reason(state, RECORDER_STOP_WRITE_ERROR);
		}
		return -1;
	}

	star6e_recorder_status_lock(state);
	state->rot.segment_bytes += (uint64_t)written;
	state->bytes_written += (uint64_t)written;
	state->frames_written++;
	star6e_recorder_status_unlock(state);
	state->frames_since_sync++;

	if (state->sync_interval_frames > 0 &&
	    state->frames_since_sync >= state->sync_interval_frames) {
		/* Non-blocking writeback hint: bounds the dirty page count
		 * without stalling the encoder thread. Durability checkpoint
		 * is the fdatasync at recorder stop. */
		sync_file_range(state->fd, 0, 0, SYNC_FILE_RANGE_WRITE);
		state->frames_since_sync = 0;
	}

	return (int)written;
}

/* The failure tail above is spelled out rather than shared with
 * write_frame(): folding them would edit a device-verified Star6E code path
 * for no behavioural gain, and would cost the byte-for-byte no-change control
 * on the star6e and maruko binaries that this change relies on. Collapsing
 * the two — together with the larger maruko_recorder.c duplication — is a
 * separate change with its own verification. */

#if !defined(PLATFORM_MARUKO) && !defined(PLATFORM_CV610)
/* Can a new segment open on this access unit?  An IRAP, or the parameter sets
 * that head a GDR refresh wave -- see RecorderRotation.  Deliberately the same
 * h265-only reading the TS recorder uses, so the two cannot disagree about
 * where a segment may start on one backend. */
static int stream_is_cut_point(const MI_VENC_Stream_t *stream)
{
	for (unsigned int i = 0; i < stream->count; ++i) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];

		if (!pack->data || pack->packNum <= 0)
			continue;
		for (unsigned int k = 0; k < (unsigned int)pack->packNum; ++k) {
			unsigned int nalu = (unsigned int)
				pack->packetInfo[k].packType.h265Nalu;

			if (recorder_nal_is_cut_point(nalu))
				return 1;
		}
	}
	return 0;
}

int star6e_recorder_write_frame(Star6eRecorderState *state,
	const MI_VENC_Stream_t *stream)
{
	static int incomplete_warned;
	struct iovec iov[16];
	int iov_count = 0;
	ssize_t written;
	size_t total = 0;
	off_t frame_start;

	if (!state || state->fd < 0 || !stream || !stream->packet)
		return 0;
	if (!star6e_output_stream_packet_info_complete(stream)) {
		if (!incomplete_warned) {
			incomplete_warned = 1;
			fprintf(stderr, "[recorder] invalid packetInfo; "
				"dropping whole access unit\n");
		}
		return 0;
	}

	/* Periodic disk space check */
	if (check_disk_space(state) != 0)
		return 0;
	/* Before the frame boundary: a cut replaces the fd, and with it the
	 * offset the rollback would restore. */
	if (star6e_recorder_check_rotation(state,
		    stream_is_cut_point(stream)) != 0)
		return -1;
	frame_start = lseek(state->fd, 0, SEEK_CUR);

	for (unsigned int i = 0; i < stream->count; ++i) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];

		if (!pack->data)
			continue;

		if (pack->packNum > 0) {
			unsigned int nal_count = (unsigned int)pack->packNum;

			for (unsigned int k = 0; k < nal_count; ++k) {
				MI_U32 offset = pack->packetInfo[k].offset;
				MI_U32 len = pack->packetInfo[k].length;

				if (len == 0 || offset >= pack->length ||
				    len > (pack->length - offset))
					continue;

				if (iov_count >= 16) {
					written = writev_all(state->fd,
						iov, iov_count);
					if (written < 0)
						goto write_error;
					total += (size_t)written;
					iov_count = 0;
				}

				iov[iov_count].iov_base =
					(void *)(pack->data + offset);
				iov[iov_count].iov_len = len;
				iov_count++;
			}
		} else {
			if (pack->length <= pack->offset)
				continue;

			if (iov_count >= 16) {
				written = writev_all(state->fd, iov, iov_count);
				if (written < 0)
					goto write_error;
				total += (size_t)written;
				iov_count = 0;
			}

			iov[iov_count].iov_base =
				(void *)(pack->data + pack->offset);
			iov[iov_count].iov_len =
				pack->length - pack->offset;
			iov_count++;
		}
	}

	if (iov_count > 0) {
		written = writev_all(state->fd, iov, iov_count);
		if (written < 0)
			goto write_error;
		total += (size_t)written;
	}

	star6e_recorder_status_lock(state);
	state->rot.segment_bytes += total;
	state->bytes_written += total;
	state->frames_written++;
	star6e_recorder_status_unlock(state);
	state->frames_since_sync++;

	if (state->sync_interval_frames > 0 &&
	    state->frames_since_sync >= state->sync_interval_frames) {
		/* Non-blocking writeback hint: bounds the dirty page count
		 * without stalling the encoder thread. Durability checkpoint
		 * is the fdatasync at recorder stop. */
		sync_file_range(state->fd, 0, 0, SYNC_FILE_RANGE_WRITE);
		state->frames_since_sync = 0;
	}

	return (int)total;

write_error:
	{
		int saved_errno = errno;

		/* A failure may follow a short successful write. Roll the regular
		 * file back to its frame boundary so it never retains half an AU. */
		if (frame_start >= 0)
			(void)ftruncate(state->fd, frame_start);
		errno = saved_errno;
	}
	if (errno == ENOSPC) {
		fprintf(stderr, "[recorder] disk full (ENOSPC)\n");
		stop_with_reason(state, RECORDER_STOP_DISK_FULL);
	} else if (errno == EFBIG) {
		fprintf(stderr,
			"[recorder] file size limit reached at %llu bytes "
			"(EFBIG) -- lower record.maxMB\n",
			(unsigned long long)state->rot.segment_bytes);
		stop_with_reason(state, RECORDER_STOP_SIZE_LIMIT);
	} else {
		fprintf(stderr, "[recorder] write error: %s\n",
			strerror(errno));
		stop_with_reason(state, RECORDER_STOP_WRITE_ERROR);
	}
	return -1;
}
#endif

void star6e_recorder_stop(Star6eRecorderState *state)
{
	stop_with_reason(state, RECORDER_STOP_MANUAL);
}

int star6e_recorder_is_active(const Star6eRecorderState *state)
{
	return state && state->fd >= 0;
}

int star6e_recorder_is_recording(const Star6eRecorderState *state)
{
	return state && __atomic_load_n(&state->recording, __ATOMIC_ACQUIRE);
}

void star6e_recorder_snapshot(Star6eRecorderState *state,
	Star6eRecorderSnapshot *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (!state || !state->status_lock_ready)
		return;

	pthread_mutex_lock(&state->status_lock);
	out->active = __atomic_load_n(&state->recording, __ATOMIC_ACQUIRE);
	out->bytes_written = state->bytes_written;
	out->frames_written = state->frames_written;
	out->segments = state->segments;
	out->elapsed_ms = out->active
		? star6e_recorder_elapsed_ms(&state->start_time) : 0;
	out->last_stop_reason = state->last_stop_reason;
	snprintf(out->path, sizeof(out->path), "%s", state->path);
	pthread_mutex_unlock(&state->status_lock);
}

void star6e_recorder_status(const Star6eRecorderState *state,
	uint64_t *bytes_written, uint32_t *frames_written,
	const char **path, Star6eRecorderStopReason *last_stop_reason)
{
	if (!state) {
		if (bytes_written) *bytes_written = 0;
		if (frames_written) *frames_written = 0;
		if (path) *path = "";
		if (last_stop_reason)
			*last_stop_reason = RECORDER_STOP_MANUAL;
		return;
	}

	if (bytes_written) *bytes_written = state->bytes_written;
	if (frames_written) *frames_written = state->frames_written;
	if (path) *path = state->path;
	if (last_stop_reason) *last_stop_reason = state->last_stop_reason;
}
