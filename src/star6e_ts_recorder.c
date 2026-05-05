#include "star6e_ts_recorder.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* ── Helpers (shared patterns from star6e_recorder.c) ────────────────── */

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
		if (ret == 0)
			return (ssize_t)total;
		total += (size_t)ret;
	}
	return (ssize_t)total;
}

static int build_ts_path(char *path, size_t path_size, const char *dir,
	const TsMuxState *mux)
{
	struct timespec ts;
	unsigned long uptime_s;
	unsigned int hours, mins, secs;
	unsigned int rand_suffix;
	const char *sep;
	const char *codec_tag;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	uptime_s = (unsigned long)ts.tv_sec;
	hours = (unsigned int)(uptime_s / 3600);
	mins = (unsigned int)((uptime_s % 3600) / 60);
	secs = (unsigned int)(uptime_s % 60);

	rand_suffix = (unsigned int)(ts.tv_nsec / 1000) & 0xFFFF;

	sep = (dir[strlen(dir) - 1] == '/') ? "" : "/";

	/* Codec suffix marks how the audio in this segment was muxed so the
	 * file is self-describing without needing ffprobe.  Audio-less
	 * segments get no suffix (current behaviour). */
	if (mux && mux->audio_rate > 0 && mux->audio_channels > 0)
		codec_tag = (mux->audio_codec == TS_AUDIO_CODEC_OPUS)
			? "_opus" : "_pcm";
	else
		codec_tag = "";

	snprintf(path, path_size, "%s%srec_%02uh%02um%02us_%04x%s.ts",
		dir, sep, hours, mins, secs, rand_suffix, codec_tag);
	return 0;
}

static const char *stop_reason_str(Star6eRecorderStopReason reason)
{
	if (reason == RECORDER_STOP_DISK_FULL)
		return "disk full";
	if (reason == RECORDER_STOP_WRITE_ERROR)
		return "write error";
	return "manual";
}

static void stop_with_reason(Star6eTsRecorderState *state,
	Star6eRecorderStopReason reason)
{
	if (!state || state->fd < 0)
		return;

	fdatasync(state->fd);
	close(state->fd);
	state->fd = -1;
	state->last_stop_reason = reason;

	fprintf(stderr, "[ts_recorder] stopped (%s): %s (%u frames, %llu bytes, %u segments)\n",
		stop_reason_str(reason), state->path, state->frames_written,
		(unsigned long long)state->bytes_written, state->segments);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void star6e_ts_recorder_init(Star6eTsRecorderState *state,
	uint32_t audio_rate, uint8_t audio_channels, uint8_t audio_codec)
{
	if (!state)
		return;
	memset(state, 0, sizeof(*state));
	state->fd = -1;
	state->sync_interval_frames = RECORDER_SYNC_DEFAULT_FRAMES;
	state->max_seconds = TS_RECORDER_DEFAULT_MAX_SECONDS;
	state->max_bytes = TS_RECORDER_DEFAULT_MAX_BYTES;
	ts_mux_init(&state->mux, audio_rate, audio_channels, audio_codec);
}

static int open_new_segment(Star6eTsRecorderState *state)
{
	uint8_t pat_pmt_buf[2 * TS_PACKET_SIZE];
	size_t pat_pmt_len;
	ssize_t ret;

	build_ts_path(state->path, sizeof(state->path), state->dir,
		&state->mux);

	state->fd = open(state->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (state->fd < 0) {
		fprintf(stderr, "[ts_recorder] open %s failed: %s\n",
			state->path, strerror(errno));
		return -1;
	}

	/* Reset CCs and set discontinuity for segment-independent playback */
	ts_mux_reset_cc(&state->mux);
	state->mux.video_frames = 0;
	state->mux.discontinuity = 1;

	pat_pmt_len = ts_mux_write_pat_pmt(&state->mux, pat_pmt_buf,
		sizeof(pat_pmt_buf));
	if (pat_pmt_len > 0) {
		ret = write_all(state->fd, pat_pmt_buf, pat_pmt_len);
		if (ret < 0) {
			close(state->fd);
			state->fd = -1;
			return -1;
		}
		state->bytes_written += (uint64_t)ret;
		state->segment_bytes = (uint64_t)ret;
	}

	clock_gettime(CLOCK_MONOTONIC, &state->segment_start_time);
	state->segments++;

	fprintf(stderr, "[ts_recorder] segment %u: %s\n",
		state->segments, state->path);
	return 0;
}

int star6e_ts_recorder_start(Star6eTsRecorderState *state, const char *dir,
	AudioRing *audio_ring)
{
	uint64_t free_bytes;

	if (!state || !dir || !dir[0])
		return -1;

	if (state->fd >= 0)
		star6e_ts_recorder_stop(state);

	free_bytes = star6e_recorder_free_space(dir);
	if (free_bytes > 0 && free_bytes < RECORDER_MIN_FREE_BYTES) {
		fprintf(stderr, "[ts_recorder] insufficient space on %s\n", dir);
		state->last_stop_reason = RECORDER_STOP_DISK_FULL;
		return -1;
	}

	snprintf(state->dir, sizeof(state->dir), "%s", dir);
	state->audio_ring = audio_ring;
	state->bytes_written = 0;
	state->frames_written = 0;
	state->segments = 0;
	state->frames_since_sync = 0;
	state->space_check_countdown = RECORDER_SPACE_CHECK_INTERVAL;
	state->last_stop_reason = RECORDER_STOP_MANUAL;
	state->segment_bytes = 0;
	clock_gettime(CLOCK_MONOTONIC, &state->start_time);

	if (open_new_segment(state) != 0)
		return -1;

	fprintf(stderr, "[ts_recorder] started: %s\n", state->path);
	return 0;
}

static int check_disk_space(Star6eTsRecorderState *state)
{
	uint64_t free_bytes;

	if (state->space_check_countdown > 0) {
		state->space_check_countdown--;
		return 0;
	}
	state->space_check_countdown = RECORDER_SPACE_CHECK_INTERVAL;
	free_bytes = star6e_recorder_free_space(state->dir);

	if (free_bytes > 0 && free_bytes < RECORDER_MIN_FREE_BYTES) {
		fprintf(stderr, "[ts_recorder] disk space low, stopping\n");
		stop_with_reason(state, RECORDER_STOP_DISK_FULL);
		return -1;
	}
	return 0;
}

static int check_rotation(Star6eTsRecorderState *state, int is_idr)
{
	struct timespec now;
	unsigned long elapsed;
	int should_rotate = 0;

	if (!is_idr)
		return 0;

	/* Check time-based rotation */
	if (state->max_seconds > 0) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (unsigned long)(now.tv_sec - state->segment_start_time.tv_sec);
		if (elapsed >= state->max_seconds)
			should_rotate = 1;
	}

	/* Check size-based rotation */
	if (state->max_bytes > 0 && state->segment_bytes >= state->max_bytes)
		should_rotate = 1;

	if (!should_rotate)
		return 0;

	/* Close current segment, open new one */
	fdatasync(state->fd);
	close(state->fd);
	state->fd = -1;
	state->segment_bytes = 0;

	if (open_new_segment(state) != 0) {
		state->last_stop_reason = RECORDER_STOP_WRITE_ERROR;
		return -1;
	}

	/* No IDR request needed: check_rotation only fires when is_idr==1,
	 * so the current frame (about to be written) IS already an IDR. */
	return 0;
}

/* TS packet buffer — sized for worst case:
 * PAT/PMT (2 × 188) + video (128KB → ~713 packets × 188) +
 * audio (~8 frames × 5 packets × 188) = ~142KB.
 * Round up to 800 packets for margin. */
/* 512KB video = ~2783 TS packets + PAT/PMT + audio = ~2900 packets */
#define TS_BUF_SIZE (3000 * TS_PACKET_SIZE)

int star6e_ts_recorder_write_video(Star6eTsRecorderState *state,
	const uint8_t *video_data, size_t video_len,
	uint64_t pts_90khz, int is_idr)
{
	uint8_t ts_buf[TS_BUF_SIZE];
	size_t ts_len = 0;
	ssize_t written;

	if (!state || state->fd < 0 || !video_data || video_len == 0)
		return 0;

	if (check_disk_space(state) != 0)
		return 0;

	/* Check rotation (only at IDR boundaries) */
	if (check_rotation(state, is_idr) != 0)
		return -1;

	/* 1. Emit video TS packets first (ensures IDR is right after PAT/PMT
	 *    at segment boundaries for fast random access) */
	size_t vlen = ts_mux_write_video(&state->mux,
		ts_buf + ts_len, sizeof(ts_buf) - ts_len,
		video_data, video_len, pts_90khz, is_idr);
	ts_len += vlen;

	/* 2. Drain audio ring → emit TS audio packets after video.
	 *    Stop when TS buffer is nearly full (leave room for at least
	 *    one max-size audio PES: ~8 TS packets).  Remaining audio
	 *    frames stay in the ring for the next video frame. */
	if (state->audio_ring) {
		AudioRingEntry ae;
		while (sizeof(ts_buf) - ts_len >= 8 * TS_PACKET_SIZE &&
		       audio_ring_pop(state->audio_ring, &ae)) {
			uint64_t audio_pts = ts_mux_timespec_to_pts(
				(uint32_t)(ae.timestamp_us / 1000000ULL),
				(uint32_t)((ae.timestamp_us % 1000000ULL) * 1000ULL));
			size_t alen = ts_mux_write_audio(&state->mux,
				ts_buf + ts_len, sizeof(ts_buf) - ts_len,
				ae.pcm, ae.length, audio_pts);
			ts_len += alen;
		}
	}

	/* 3. Write to file */
	if (ts_len > 0) {
		written = write_all(state->fd, ts_buf, ts_len);
		if (written < 0) {
			if (errno == ENOSPC) {
				fprintf(stderr, "[ts_recorder] disk full (ENOSPC)\n");
				stop_with_reason(state, RECORDER_STOP_DISK_FULL);
			} else {
				fprintf(stderr, "[ts_recorder] write error: %s\n",
					strerror(errno));
				stop_with_reason(state, RECORDER_STOP_WRITE_ERROR);
			}
			return -1;
		}
		state->bytes_written += (uint64_t)written;
		state->segment_bytes += (uint64_t)written;
	}

	state->frames_written++;
	state->frames_since_sync++;

	if (state->sync_interval_frames > 0 &&
	    state->frames_since_sync >= state->sync_interval_frames) {
		/* Non-blocking writeback hint: bounds the dirty page count
		 * without stalling the encoder thread. Durability checkpoint
		 * is the fdatasync at segment rotation/stop. */
		sync_file_range(state->fd, 0, 0, SYNC_FILE_RANGE_WRITE);
		state->frames_since_sync = 0;
	}

	return (int)ts_len;
}

void star6e_ts_recorder_stop(Star6eTsRecorderState *state)
{
	stop_with_reason(state, RECORDER_STOP_MANUAL);
}

int star6e_ts_recorder_is_active(const Star6eTsRecorderState *state)
{
	return state && state->fd >= 0;
}

int star6e_ts_recorder_write_stream(Star6eTsRecorderState *state,
	const MI_VENC_Stream_t *stream)
{
	uint8_t nal_buf[512 * 1024];  /* 512KB — supports up to ~50 Mbps IDR frames */
	size_t nal_len = 0;
	int is_idr = 0;
	struct timespec now;
	uint64_t pts;

	if (!state || state->fd < 0 || !stream || !stream->packet)
		return 0;

	/* Extract all NAL data from stream packs */
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
				MI_U32 off = pack->packetInfo[k].offset;
				MI_U32 len = pack->packetInfo[k].length;

				if (len == 0 || off >= pack->length ||
				    len > (pack->length - off))
					continue;

				/* Check for IDR (both IDR_W_RADL=19 and IDR_N_LP=20) */
				unsigned int nalu = (unsigned int)
					pack->packetInfo[k].packType.h265Nalu;
				if (nalu == 19 || nalu == 20)
					is_idr = 1;

				if (nal_len + len > sizeof(nal_buf)) {
					fprintf(stderr,
						"[ts_recorder] frame too large "
						"(%zu + %u > %zu), truncated\n",
						nal_len, len, sizeof(nal_buf));
					break;
				}
				memcpy(nal_buf + nal_len,
					pack->data + off, len);
				nal_len += len;
			}
		} else {
			if (pack->length <= pack->offset)
				continue;
			MI_U32 len = pack->length - pack->offset;
			if (nal_len + len > sizeof(nal_buf)) {
				fprintf(stderr,
					"[ts_recorder] frame too large "
					"(%zu + %u > %zu), truncated\n",
					nal_len, len, sizeof(nal_buf));
				break;
			}
			memcpy(nal_buf + nal_len,
				pack->data + pack->offset, len);
			nal_len += len;
		}
	}

	if (nal_len == 0)
		return 0;

	clock_gettime(CLOCK_MONOTONIC, &now);
	pts = ts_mux_timespec_to_pts((uint32_t)now.tv_sec,
		(uint32_t)now.tv_nsec);

	return star6e_ts_recorder_write_video(state,
		nal_buf, nal_len, pts, is_idr);
}

void star6e_ts_recorder_status(const Star6eTsRecorderState *state,
	uint64_t *bytes_written, uint32_t *frames_written,
	uint32_t *segments, const char **path,
	Star6eRecorderStopReason *last_stop_reason)
{
	if (!state) {
		if (bytes_written) *bytes_written = 0;
		if (frames_written) *frames_written = 0;
		if (segments) *segments = 0;
		if (path) *path = "";
		if (last_stop_reason)
			*last_stop_reason = RECORDER_STOP_MANUAL;
		return;
	}
	if (bytes_written) *bytes_written = state->bytes_written;
	if (frames_written) *frames_written = state->frames_written;
	if (segments) *segments = state->segments;
	if (path) *path = state->path;
	if (last_stop_reason) *last_stop_reason = state->last_stop_reason;
}
