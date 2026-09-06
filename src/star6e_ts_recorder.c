#include "star6e_ts_recorder.h"
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
#include <time.h>
#include <unistd.h>

/* TS packet buffer — sized for worst case:
 * PAT/PMT (2 × 188) + video (128KB → ~713 packets × 188) +
 * audio (~8 frames × 5 packets × 188) = ~142KB.
 * Round up to 800 packets for margin. */
/* 512KB video = ~2783 TS packets + PAT/PMT + audio = ~2900 packets */
/* Declared here rather than above its only consumer because check_rotation()
 * reserves one buffer of headroom below the off_t ceiling. */
#define TS_BUF_SIZE (3000 * TS_PACKET_SIZE)

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

/* Guard the status-visible fields.  Never wrap I/O in these. */
static void ts_status_lock(Star6eTsRecorderState *state)
{
	if (state->status_lock_ready)
		pthread_mutex_lock(&state->status_lock);
}

static void ts_status_unlock(Star6eTsRecorderState *state)
{
	if (state->status_lock_ready)
		pthread_mutex_unlock(&state->status_lock);
}

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

static void stop_with_reason(Star6eTsRecorderState *state,
	Star6eRecorderStopReason reason)
{
	if (!state)
		return;
	/* Cleared before the fd test, not after it: a stop that lands while fd
	 * is already -1 (mid-rotation, or after a failed reopen) must still end
	 * the recording, or a producer would go on handing frames to a dead
	 * recorder.  Today the state lock makes that unreachable; the invariant
	 * should not depend on the caller's locking. */
	ts_status_lock(state);
	__atomic_store_n(&state->recording, 0, __ATOMIC_RELEASE);
	ts_status_unlock(state);
	if (state->fd < 0)
		return;

	fdatasync(state->fd);
	close(state->fd);
	state->fd = -1;
	ts_status_lock(state);
	state->last_stop_reason = reason;
	ts_status_unlock(state);

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
	state->rot.max_seconds = TS_RECORDER_DEFAULT_MAX_SECONDS;
	state->rot.max_bytes = TS_RECORDER_DEFAULT_MAX_BYTES;
	/* One muxed frame is the most this recorder writes in one call. */
	state->rot.write_headroom = TS_BUF_SIZE;
	ts_mux_init(&state->mux, audio_rate, audio_channels, audio_codec);
	if (pthread_mutex_init(&state->status_lock, NULL) == 0)
		state->status_lock_ready = 1;
}

static int open_new_segment(Star6eTsRecorderState *state)
{
	uint8_t pat_pmt_buf[2 * TS_PACKET_SIZE];
	char newpath[RECORDER_PATH_MAX];
	size_t pat_pmt_len;
	uint64_t pat_bytes = 0;
	ssize_t ret;

	/* Built into a LOCAL and published at the end together with segments
	 * and bytes.  Writing it into state->path up front would let a status
	 * poll pair the new path with the old segment count, and the open()
	 * below must not run under the lock anyway. */
	build_ts_path(newpath, sizeof(newpath), state->dir, &state->mux);

	/* O_EXCL, not O_TRUNC: the name carries only uptime seconds plus 16 bits
	 * from the nanosecond clock, and after a reboot the uptime restarts, so
	 * it can reproduce a name an earlier session left behind.  Truncating
	 * would silently destroy that recording.  Retry with a fresh name. */
	{
		int attempt;

		for (attempt = 0; attempt < 8; attempt++) {
			state->fd = open(newpath,
				O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
			if (state->fd >= 0)
				break;
			if (errno != EEXIST) {
				fprintf(stderr,
					"[ts_recorder] open %s failed: %s\n",
					newpath, strerror(errno));
				return -1;
			}
			build_ts_path(newpath, sizeof(newpath), state->dir,
				&state->mux);
		}
		if (state->fd < 0) {
			fprintf(stderr,
				"[ts_recorder] could not find an unused segment "
				"name in %s\n", state->dir);
			return -1;
		}
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
		pat_bytes = (uint64_t)ret;
	}

	recorder_rotation_segment_opened(&state->rot, pat_bytes);

	/* ONE publish: path, segment count and byte total change together, so
	 * a poll mid-rotation cannot report the new file with the old count. */
	ts_status_lock(state);
	snprintf(state->path, sizeof(state->path), "%s", newpath);
	state->bytes_written += pat_bytes;
	state->segments++;
	ts_status_unlock(state);

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
		ts_status_lock(state);
		state->last_stop_reason = RECORDER_STOP_DISK_FULL;
		ts_status_unlock(state);
		return -1;
	}

	snprintf(state->dir, sizeof(state->dir), "%s", dir);
	state->audio_ring = audio_ring;
	ts_status_lock(state);
	state->bytes_written = 0;
	state->frames_written = 0;
	state->segments = 0;
	state->frames_since_sync = 0;
	state->space_check_countdown = RECORDER_SPACE_CHECK_INTERVAL;
	state->last_stop_reason = RECORDER_STOP_MANUAL;
	state->rot.segment_bytes = 0;
	/* Part of the per-recording reset: a restart must not inherit the old
	 * recording's "threshold has been crossed since" anchor. */
	state->rot.rotation_due_since = 0;
	state->rot.warned_no_cut_point = 0;
	clock_gettime(CLOCK_MONOTONIC, &state->start_time);
	ts_status_unlock(state);

	if (open_new_segment(state) != 0)
		return -1;

	/* Published only once the first segment is actually open, so a failed
	 * start never leaves a producer thinking a recording is running -- and
	 * under the lock, so a poll cannot pair active == 0 with the new path
	 * and counters. */
	ts_status_lock(state);
	__atomic_store_n(&state->recording, 1, __ATOMIC_RELEASE);
	ts_status_unlock(state);

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

/* Does this Annex-B access unit BEGIN with a parameter set?  Same reading as
 * the raw recorder's au_leads_with_param_sets(); duplicated rather than shared
 * because the two files are compiled into different backends and this is four
 * lines of byte inspection, not policy. */
static int ts_au_leads_with_param_sets(const uint8_t *au, size_t len)
{
	size_t i = 0;

	if (!au)
		return 0;
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

static int check_rotation(Star6eTsRecorderState *state, int is_cut_point)
{
	/* The WHEN lives in recorder_rotation_due() -- thresholds and the
	 * boundary a segment may open on -- shared with the raw recorder.  What
	 * stays here is the WHAT: a .ts segment is cut with fdatasync/close and
	 * opened with fresh PAT/PMT. */
	if (!recorder_rotation_due(&state->rot, is_cut_point))
		return 0;

	/* Close current segment, open new one.  Finalising the old one can fail
	 * -- a delayed write surfacing at fdatasync(), or close() reporting one
	 * -- and calling that a clean rotation would hide a possibly short
	 * segment, so it is a stop just like a failed reopen. */
	{
		int finalise_failed = (fdatasync(state->fd) != 0);

		if (close(state->fd) != 0)
			finalise_failed = 1;
		state->fd = -1;
		if (finalise_failed) {
			fprintf(stderr,
				"[ts_recorder] segment %u (%s) failed to "
				"finalise: %s\n", state->segments, state->path,
				strerror(errno));
			ts_status_lock(state);
			__atomic_store_n(&state->recording, 0, __ATOMIC_RELEASE);
			state->last_stop_reason = RECORDER_STOP_WRITE_ERROR;
			ts_status_unlock(state);
			return -1;
		}
	}
	state->rot.segment_bytes = 0;

	if (open_new_segment(state) != 0) {
		/* A rotation that cannot reopen is a stop, not a gap: clear the
		 * recording flag so producers stop handing over frames. */
		ts_status_lock(state);
		__atomic_store_n(&state->recording, 0, __ATOMIC_RELEASE);
		state->last_stop_reason = RECORDER_STOP_WRITE_ERROR;
		ts_status_unlock(state);
		return -1;
	}

	/* The frame about to be written IS the IDR — either a natural one or
	 * the answer to the request above — so the new segment opens on it. */
	return 0;
}

/* Write one muxed frame, classifying the ways it can fail.
 *
 * Split out of star6e_ts_recorder_write_video(), which AGENTS.md wants under
 * ~80 lines: muxing is that function's job, and this is the file-ceiling and
 * error-classification half of it.
 *
 * Returns the byte count, or -1 having already stopped the recorder. */
static ssize_t ts_write_muxed(Star6eTsRecorderState *state,
	const uint8_t *ts_buf, size_t ts_len)
{
	ssize_t written;

	/* The rotation clamp cuts before the ceiling, but only on a cut point,
	 * and rotation never asks for one -- so a stream that produces neither
	 * an IRAP nor a parameter-set boundary carries the segment past the
	 * threshold with no cut in sight.  Walking
	 * into the resulting EFBIG would truncate the file mid-AU at an
	 * arbitrary byte.  Stop on this frame boundary instead: nothing has
	 * been written, so the .ts ends on its last complete access unit and
	 * the status names a size ceiling rather than a write error.  Inert on
	 * a 64-bit off_t build. */
	if ((uint64_t)ts_len > RECORDER_OFF_T_CEILING - state->rot.segment_bytes) {
		fprintf(stderr,
			"[ts_recorder] segment at %llu bytes cannot grow past the "
			"32-bit file ceiling and the stream produced no IRAP to "
			"rotate on; stopping\n",
			(unsigned long long)state->rot.segment_bytes);
		stop_with_reason(state, RECORDER_STOP_SIZE_LIMIT);
		return -1;
	}

	written = write_all(state->fd, ts_buf, ts_len);
	if (written >= 0) {
		/* The rollback below and the whole size-rotation test read this,
		 * so it MUST advance here: it is the offset of the last complete
		 * frame.  Extracting this function out of write_video() once
		 * dropped this line, which silently disabled maxMB for .ts and
		 * made the rollback truncate the entire segment. */
		state->rot.segment_bytes += (uint64_t)written;
		return written;
	}

	{
		int saved_errno = errno;

		/* A failure can follow a PARTIAL successful write -- measured:
		 * an unpatched recorder's own counter stopped at 2147464968
		 * while the file on disk was 2147483647, so 18679 bytes of a
		 * half-written access unit stayed behind.  segment_bytes IS the
		 * offset of the last complete frame (the segment opened at 0
		 * and only completed writes are added), so rolling back to it
		 * costs no lseek and leaves whole TS packets only. */
		(void)ftruncate(state->fd, (off_t)state->rot.segment_bytes);
		errno = saved_errno;
	}

	if (errno == ENOSPC) {
		fprintf(stderr, "[ts_recorder] disk full (ENOSPC)\n");
		stop_with_reason(state, RECORDER_STOP_DISK_FULL);
	} else if (errno == EFBIG) {
		/* A ceiling below the off_t one -- FAT32 stops at 4 GB -- so the
		 * guard above could not have anticipated it.  Name it: "write
		 * error" sent the original report hunting the SD card. */
		fprintf(stderr,
			"[ts_recorder] file size limit reached at %llu bytes "
			"(EFBIG) -- lower record.maxMB\n",
			(unsigned long long)state->rot.segment_bytes);
		stop_with_reason(state, RECORDER_STOP_SIZE_LIMIT);
	} else {
		fprintf(stderr, "[ts_recorder] write error: %s\n",
			strerror(errno));
		stop_with_reason(state, RECORDER_STOP_WRITE_ERROR);
	}
	return -1;
}

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
	if (check_rotation(state, is_idr ||
		    ts_au_leads_with_param_sets(video_data, video_len)) != 0)
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
		written = ts_write_muxed(state, ts_buf, ts_len);
		if (written < 0)
			return -1;
	} else {
		written = 0;
	}

	/* ONE section for both counters: a poll landing between two separate
	 * ones would report frame N's bytes with frame N-1's count.
	 *
	 * frames_written is incremented unconditionally, as it always was --
	 * a frame that muxed to zero TS bytes still passed through the
	 * recorder, and quietly making the count conditional here would be a
	 * semantic change smuggled in by a locking refactor. */
	ts_status_lock(state);
	state->bytes_written += (uint64_t)written;
	state->frames_written++;
	ts_status_unlock(state);

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

int star6e_ts_recorder_is_recording(const Star6eTsRecorderState *state)
{
	return state && __atomic_load_n(&state->recording, __ATOMIC_ACQUIRE);
}

#if !defined(PLATFORM_MARUKO) && !defined(PLATFORM_CV610)
int star6e_ts_recorder_write_stream(Star6eTsRecorderState *state,
	const MI_VENC_Stream_t *stream)
{
	static int incomplete_warned;
	uint8_t nal_buf[512 * 1024];  /* 512KB — supports up to ~50 Mbps IDR frames */
	size_t nal_len = 0;
	int is_idr = 0;
	struct timespec now;
	uint64_t pts;

	if (!state || state->fd < 0 || !stream || !stream->packet)
		return 0;
	if (!star6e_output_stream_packet_info_complete(stream)) {
		if (!incomplete_warned) {
			incomplete_warned = 1;
			fprintf(stderr, "[ts_recorder] invalid packetInfo; "
				"dropping whole access unit\n");
		}
		return 0;
	}

	/* Extract all NAL data from stream packs */
	for (unsigned int i = 0; i < stream->count; ++i) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];
		if (!pack->data)
			continue;

		if (pack->packNum > 0) {
			unsigned int nal_count = (unsigned int)pack->packNum;

			for (unsigned int k = 0; k < nal_count; ++k) {
				MI_U32 off = pack->packetInfo[k].offset;
				MI_U32 len = pack->packetInfo[k].length;

				if (len == 0 || off >= pack->length ||
				    len > (pack->length - off))
					continue;

				/* A segment may open on an IRAP (19/20) or on
				 * the parameter sets that head a GDR refresh
				 * wave (32/33/34) -- see RecorderRotation. */
				unsigned int nalu = (unsigned int)
					pack->packetInfo[k].packType.h265Nalu;
				if (recorder_nal_is_cut_point(nalu))
					is_idr = 1;

				if (nal_len + len > sizeof(nal_buf)) {
					fprintf(stderr,
						"[ts_recorder] frame too large "
						"(%zu + %u > %zu), access unit dropped\n",
						nal_len, len, sizeof(nal_buf));
					return 0;
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
					"(%zu + %u > %zu), access unit dropped\n",
					nal_len, len, sizeof(nal_buf));
				return 0;
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
#endif

void star6e_ts_recorder_snapshot(Star6eTsRecorderState *state,
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
