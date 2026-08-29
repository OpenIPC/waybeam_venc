#include "maruko_recorder.h"
#include "maruko_video.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

/* Mirrors star6e_recorder_write_frame() with Maruko's i6c_venc_strm/pack
 * types.  Same iovec-collection pattern (zero-copy writev), same
 * disk-space + sync_file_range cadence, same stop-on-error/full
 * behaviour.  Kept as a separate translation unit so the SDK type
 * difference does not have to be juggled with #ifdefs in
 * star6e_recorder.c. */

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
		fprintf(stderr,
			"[maruko_recorder] disk space low (%llu bytes), "
			"stopping\n", (unsigned long long)free_bytes);
		star6e_recorder_status_lock(state);
		state->last_stop_reason = RECORDER_STOP_DISK_FULL;
		star6e_recorder_status_unlock(state);
		fdatasync(state->fd);
		close(state->fd);
		state->fd = -1;
		return -1;
	}

	return 0;
}

static void stop_with_error(Star6eRecorderState *state, int err)
{
	if (err == ENOSPC) {
		fprintf(stderr, "[maruko_recorder] disk full (ENOSPC)\n");
		star6e_recorder_status_lock(state);
		state->last_stop_reason = RECORDER_STOP_DISK_FULL;
		star6e_recorder_status_unlock(state);
	} else {
		fprintf(stderr, "[maruko_recorder] write error: %s\n",
			strerror(err));
		star6e_recorder_status_lock(state);
		state->last_stop_reason = RECORDER_STOP_WRITE_ERROR;
		star6e_recorder_status_unlock(state);
	}
	if (state->fd >= 0) {
		fdatasync(state->fd);
		close(state->fd);
		state->fd = -1;
	}
}

int maruko_recorder_write_frame(Star6eRecorderState *state,
	const i6c_venc_strm *stream)
{
	static int incomplete_warned;
	struct iovec iov[16];
	int iov_count = 0;
	ssize_t written;
	size_t total = 0;
	off_t frame_start;

	if (!state || state->fd < 0 || !stream || !stream->packet)
		return 0;
	if (!maruko_video_stream_packet_info_complete(stream)) {
		if (!incomplete_warned) {
			incomplete_warned = 1;
			fprintf(stderr,
				"[maruko_recorder] incomplete packetInfo table; "
				"dropping whole access unit\n");
		}
		return 0;
	}

	if (check_disk_space(state) != 0)
		return 0;
	frame_start = lseek(state->fd, 0, SEEK_CUR);

	for (unsigned int i = 0; i < stream->count; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];

		if (!pack->data)
			continue;

		if (pack->packNum > 0) {
			unsigned int nal_count = (unsigned int)pack->packNum;

			for (unsigned int k = 0; k < nal_count; ++k) {
				unsigned int off = pack->packetInfo[k].offset;
				unsigned int len = pack->packetInfo[k].length;

				if (len == 0 || off >= pack->length ||
				    len > (pack->length - off))
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
					(void *)(pack->data + off);
				iov[iov_count].iov_len = len;
				iov_count++;
			}
		} else {
			if (pack->length <= pack->offset)
				continue;

			if (iov_count >= 16) {
				written = writev_all(state->fd, iov,
					iov_count);
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

	/* One section for both: a poll between two would report frame N's
	 * bytes with frame N-1's count. */
	star6e_recorder_status_lock(state);
	state->bytes_written += total;
	state->frames_written++;
	star6e_recorder_status_unlock(state);
	state->frames_since_sync++;

	if (state->sync_interval_frames > 0 &&
	    state->frames_since_sync >= state->sync_interval_frames) {
		sync_file_range(state->fd, 0, 0, SYNC_FILE_RANGE_WRITE);
		state->frames_since_sync = 0;
	}

	return (int)total;

write_error:
	{
		int saved_errno = errno;
	/* A failure may follow a short successful write. Roll the regular file
	 * back to its frame boundary so recording never retains half an AU. */
		if (frame_start >= 0)
			(void)ftruncate(state->fd, frame_start);
		stop_with_error(state, saved_errno);
	}
	return -1;
}
