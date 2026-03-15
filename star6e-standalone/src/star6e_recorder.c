#include "star6e_recorder.h"

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

static ssize_t writev_retry(int fd, const struct iovec *iov, int iov_count)
{
	ssize_t ret;

	do {
		ret = writev(fd, iov, iov_count);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

static const char *stop_reason_str(Star6eRecorderStopReason reason)
{
	if (reason == RECORDER_STOP_DISK_FULL)
		return "disk full";
	if (reason == RECORDER_STOP_WRITE_ERROR)
		return "write error";
	return "manual";
}

static void stop_with_reason(Star6eRecorderState *state,
	Star6eRecorderStopReason reason)
{
	if (!state || state->fd < 0)
		return;

	fdatasync(state->fd);
	close(state->fd);
	state->fd = -1;
	state->last_stop_reason = reason;

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
		state->last_stop_reason = RECORDER_STOP_DISK_FULL;
		return -1;
	}

	snprintf(state->dir, sizeof(state->dir), "%s", dir);
	build_recording_path(state->path, sizeof(state->path), dir);

	state->fd = open(state->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (state->fd < 0) {
		fprintf(stderr, "[recorder] open %s failed: %s\n",
			state->path, strerror(errno));
		state->path[0] = '\0';
		return -1;
	}

	state->bytes_written = 0;
	state->frames_written = 0;
	state->frames_since_sync = 0;
	state->space_check_countdown = RECORDER_SPACE_CHECK_INTERVAL;
	state->last_stop_reason = RECORDER_STOP_MANUAL;
	clock_gettime(CLOCK_MONOTONIC, &state->start_time);

	fprintf(stderr, "[recorder] started: %s\n", state->path);
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

int star6e_recorder_write_frame(Star6eRecorderState *state,
	const MI_VENC_Stream_t *stream)
{
	struct iovec iov[16];
	int iov_count = 0;
	ssize_t written;
	size_t total = 0;

	if (!state || state->fd < 0 || !stream || !stream->packet)
		return 0;

	/* Periodic disk space check */
	if (check_disk_space(state) != 0)
		return 0;

	for (unsigned int i = 0; i < stream->count; ++i) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];

		if (!pack->data)
			continue;

		if (pack->packNum > 0) {
			const unsigned int info_cap = (unsigned int)(
				sizeof(pack->packetInfo) /
				sizeof(pack->packetInfo[0]));
			unsigned int nal_count = (unsigned int)pack->packNum;

			if (nal_count > info_cap)
				nal_count = info_cap;

			for (unsigned int k = 0; k < nal_count; ++k) {
				MI_U32 offset = pack->packetInfo[k].offset;
				MI_U32 len = pack->packetInfo[k].length;

				if (len == 0 || offset >= pack->length ||
				    len > (pack->length - offset))
					continue;

				if (iov_count >= 16) {
					written = writev_retry(state->fd,
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
				written = writev_retry(state->fd, iov, iov_count);
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
		written = writev_retry(state->fd, iov, iov_count);
		if (written < 0)
			goto write_error;
		total += (size_t)written;
	}

	state->bytes_written += total;
	state->frames_written++;
	state->frames_since_sync++;

	if (state->sync_interval_frames > 0 &&
	    state->frames_since_sync >= state->sync_interval_frames) {
		fdatasync(state->fd);
		state->frames_since_sync = 0;
	}

	return (int)total;

write_error:
	if (errno == ENOSPC) {
		fprintf(stderr, "[recorder] disk full (ENOSPC)\n");
		stop_with_reason(state, RECORDER_STOP_DISK_FULL);
	} else {
		fprintf(stderr, "[recorder] write error: %s\n",
			strerror(errno));
		stop_with_reason(state, RECORDER_STOP_WRITE_ERROR);
	}
	return -1;
}

void star6e_recorder_stop(Star6eRecorderState *state)
{
	stop_with_reason(state, RECORDER_STOP_MANUAL);
}

int star6e_recorder_is_active(const Star6eRecorderState *state)
{
	return state && state->fd >= 0;
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
