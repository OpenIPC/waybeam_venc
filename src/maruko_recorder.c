#include "maruko_recorder.h"

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

static ssize_t writev_retry(int fd, const struct iovec *iov, int iov_count)
{
	ssize_t ret;

	do {
		ret = writev(fd, iov, iov_count);
	} while (ret < 0 && errno == EINTR);

	return ret;
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
		state->last_stop_reason = RECORDER_STOP_DISK_FULL;
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
		state->last_stop_reason = RECORDER_STOP_DISK_FULL;
	} else {
		fprintf(stderr, "[maruko_recorder] write error: %s\n",
			strerror(err));
		state->last_stop_reason = RECORDER_STOP_WRITE_ERROR;
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
	struct iovec iov[16];
	int iov_count = 0;
	ssize_t written;
	size_t total = 0;

	if (!state || state->fd < 0 || !stream || !stream->packet)
		return 0;

	if (check_disk_space(state) != 0)
		return 0;

	for (unsigned int i = 0; i < stream->count; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];

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
				unsigned int off = pack->packetInfo[k].offset;
				unsigned int len = pack->packetInfo[k].length;

				if (len == 0 || off >= pack->length ||
				    len > (pack->length - off))
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
					(void *)(pack->data + off);
				iov[iov_count].iov_len = len;
				iov_count++;
			}
		} else {
			if (pack->length <= pack->offset)
				continue;

			if (iov_count >= 16) {
				written = writev_retry(state->fd, iov,
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
		sync_file_range(state->fd, 0, 0, SYNC_FILE_RANGE_WRITE);
		state->frames_since_sync = 0;
	}

	return (int)total;

write_error:
	stop_with_error(state, errno);
	return -1;
}
