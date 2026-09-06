#include "star6e_recorder.h"

#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_test_dir[256];

static void setup_test_dir(void)
{
	snprintf(g_test_dir, sizeof(g_test_dir), "/tmp/venc_recorder_test_%d",
		(int)getpid());
	mkdir(g_test_dir, 0755);
}

static void cleanup_test_dir(void)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "rm -rf %s", g_test_dir);
	(void)system(cmd);
}

static int test_recorder_init(void)
{
	Star6eRecorderState state;
	int failures = 0;

	memset(&state, 0xA5, sizeof(state));
	star6e_recorder_init(&state);
	CHECK("recorder init fd", state.fd == -1);
	CHECK("recorder init bytes", state.bytes_written == 0);
	CHECK("recorder init frames", state.frames_written == 0);
	CHECK("recorder init sync interval",
		state.sync_interval_frames == RECORDER_SYNC_DEFAULT_FRAMES);
	CHECK("recorder init not active", !star6e_recorder_is_active(&state));
	CHECK("recorder init stop reason",
		state.last_stop_reason == RECORDER_STOP_MANUAL);
	return failures;
}

static int test_recorder_init_null(void)
{
	int failures = 0;

	star6e_recorder_init(NULL);
	CHECK("recorder init null no crash", 1);
	return failures;
}

static int test_recorder_start_stop(void)
{
	Star6eRecorderState state;
	int failures = 0;
	int ret;

	star6e_recorder_init(&state);
	ret = star6e_recorder_start(&state, g_test_dir);
	CHECK("recorder start ok", ret == 0);
	CHECK("recorder active after start", star6e_recorder_is_active(&state));
	CHECK("recorder path not empty", state.path[0] != '\0');
	CHECK("recorder path ends with .hevc",
		strlen(state.path) > 5 &&
		strcmp(state.path + strlen(state.path) - 5, ".hevc") == 0);
	CHECK("recorder path contains rec_ prefix",
		strstr(state.path, "/rec_") != NULL);
	CHECK("recorder path contains h m s markers",
		strstr(state.path, "h") != NULL &&
		strstr(state.path, "m") != NULL &&
		strstr(state.path, "s") != NULL);

	star6e_recorder_stop(&state);
	CHECK("recorder not active after stop",
		!star6e_recorder_is_active(&state));
	CHECK("recorder fd after stop", state.fd == -1);
	CHECK("recorder stop reason manual",
		state.last_stop_reason == RECORDER_STOP_MANUAL);
	return failures;
}

static int test_recorder_start_bad_dir(void)
{
	Star6eRecorderState state;
	int failures = 0;
	int ret;

	star6e_recorder_init(&state);
	ret = star6e_recorder_start(&state, "/nonexistent/path/xyz");
	CHECK("recorder start bad dir fails", ret == -1);
	CHECK("recorder not active after bad start",
		!star6e_recorder_is_active(&state));
	return failures;
}

static int test_recorder_start_null(void)
{
	Star6eRecorderState state;
	int failures = 0;

	star6e_recorder_init(&state);
	CHECK("recorder start null state",
		star6e_recorder_start(NULL, g_test_dir) == -1);
	CHECK("recorder start null dir",
		star6e_recorder_start(&state, NULL) == -1);
	CHECK("recorder start empty dir",
		star6e_recorder_start(&state, "") == -1);
	return failures;
}

static int test_recorder_double_start(void)
{
	Star6eRecorderState state;
	int failures = 0;
	int ret;
	char first_path[RECORDER_PATH_MAX];

	star6e_recorder_init(&state);
	ret = star6e_recorder_start(&state, g_test_dir);
	CHECK("recorder first start ok", ret == 0);
	snprintf(first_path, sizeof(first_path), "%s", state.path);

	ret = star6e_recorder_start(&state, g_test_dir);
	CHECK("recorder second start ok", ret == 0);
	CHECK("recorder still active", star6e_recorder_is_active(&state));
	CHECK("recorder first file exists", access(first_path, F_OK) == 0);

	star6e_recorder_stop(&state);
	return failures;
}

static int test_recorder_write_frame(void)
{
	Star6eRecorderState state;
	int failures = 0;
	int ret;
	struct stat st;

	uint8_t nal1[] = { 0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01 };
	uint8_t nal2[] = { 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0xFF, 0xAA,
			   0xBB, 0xCC };

	i6_venc_packinfo info[2];
	memset(info, 0, sizeof(info));
	info[0].offset = 0;
	info[0].length = sizeof(nal1);
	info[1].offset = sizeof(nal1);
	info[1].length = sizeof(nal2);

	uint8_t combined[sizeof(nal1) + sizeof(nal2)];
	memcpy(combined, nal1, sizeof(nal1));
	memcpy(combined + sizeof(nal1), nal2, sizeof(nal2));

	MI_VENC_Pack_t pack;
	memset(&pack, 0, sizeof(pack));
	pack.data = combined;
	pack.length = sizeof(combined);
	pack.packNum = 2;
	memcpy(pack.packetInfo, info, sizeof(info));

	MI_VENC_Stream_t stream;
	memset(&stream, 0, sizeof(stream));
	stream.packet = &pack;
	stream.count = 1;

	star6e_recorder_init(&state);
	ret = star6e_recorder_start(&state, g_test_dir);
	CHECK("recorder write start ok", ret == 0);

	ret = star6e_recorder_write_frame(&state, &stream);
	CHECK("recorder write frame ok", ret > 0);
	CHECK("recorder write frame bytes",
		(size_t)ret == sizeof(nal1) + sizeof(nal2));
	CHECK("recorder bytes_written matches",
		state.bytes_written == sizeof(nal1) + sizeof(nal2));
	CHECK("recorder frames_written", state.frames_written == 1);

	star6e_recorder_stop(&state);

	if (stat(state.path, &st) == 0) {
		CHECK("recorder file size",
			(size_t)st.st_size == sizeof(nal1) + sizeof(nal2));
	} else {
		CHECK("recorder file exists", 0);
	}

	return failures;
}

static int test_recorder_write_single_nal(void)
{
	Star6eRecorderState state;
	int failures = 0;
	int ret;
	struct stat st;

	uint8_t nal_data[] = { 0x00, 0x00, 0x00, 0x01, 0x26, 0x01,
			       0xDE, 0xAD, 0xBE, 0xEF };

	MI_VENC_Pack_t pack;
	memset(&pack, 0, sizeof(pack));
	pack.data = nal_data;
	pack.length = sizeof(nal_data);
	pack.offset = 0;
	pack.packNum = 0;

	MI_VENC_Stream_t stream;
	memset(&stream, 0, sizeof(stream));
	stream.packet = &pack;
	stream.count = 1;

	star6e_recorder_init(&state);
	ret = star6e_recorder_start(&state, g_test_dir);
	CHECK("recorder single nal start ok", ret == 0);

	ret = star6e_recorder_write_frame(&state, &stream);
	CHECK("recorder single nal write ok", ret > 0);
	CHECK("recorder single nal size", (size_t)ret == sizeof(nal_data));

	star6e_recorder_stop(&state);

	if (stat(state.path, &st) == 0) {
		CHECK("recorder single nal file size",
			(size_t)st.st_size == sizeof(nal_data));
	} else {
		CHECK("recorder single nal file exists", 0);
	}

	return failures;
}

static int test_recorder_write_not_active(void)
{
	Star6eRecorderState state;
	int failures = 0;
	int ret;

	uint8_t nal[] = { 0x00, 0x00, 0x00, 0x01, 0x40, 0x01 };

	MI_VENC_Pack_t pack;
	memset(&pack, 0, sizeof(pack));
	pack.data = nal;
	pack.length = sizeof(nal);
	pack.packNum = 0;

	MI_VENC_Stream_t stream;
	memset(&stream, 0, sizeof(stream));
	stream.packet = &pack;
	stream.count = 1;

	star6e_recorder_init(&state);
	ret = star6e_recorder_write_frame(&state, &stream);
	CHECK("recorder write not active returns 0", ret == 0);
	return failures;
}

static int test_recorder_write_multi_frame(void)
{
	Star6eRecorderState state;
	int failures = 0;
	int ret;
	struct stat st;

	uint8_t nal[] = { 0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0xAA, 0xBB };

	MI_VENC_Pack_t pack;
	memset(&pack, 0, sizeof(pack));
	pack.data = nal;
	pack.length = sizeof(nal);
	pack.packNum = 0;

	MI_VENC_Stream_t stream;
	memset(&stream, 0, sizeof(stream));
	stream.packet = &pack;
	stream.count = 1;

	star6e_recorder_init(&state);
	ret = star6e_recorder_start(&state, g_test_dir);
	CHECK("recorder multi start ok", ret == 0);

	for (int i = 0; i < 10; i++) {
		ret = star6e_recorder_write_frame(&state, &stream);
		CHECK("recorder multi write ok", ret > 0);
	}

	CHECK("recorder multi frames_written", state.frames_written == 10);
	CHECK("recorder multi bytes_written",
		state.bytes_written == 10 * sizeof(nal));

	star6e_recorder_stop(&state);

	if (stat(state.path, &st) == 0) {
		CHECK("recorder multi file size",
			(size_t)st.st_size == 10 * sizeof(nal));
	} else {
		CHECK("recorder multi file exists", 0);
	}

	return failures;
}

static int test_recorder_status(void)
{
	Star6eRecorderState state;
	int failures = 0;
	uint64_t bytes;
	uint32_t frames;
	const char *path;
	Star6eRecorderStopReason reason;

	star6e_recorder_init(&state);
	star6e_recorder_status(&state, &bytes, &frames, &path, &reason);
	CHECK("recorder status init bytes", bytes == 0);
	CHECK("recorder status init frames", frames == 0);
	CHECK("recorder status init reason", reason == RECORDER_STOP_MANUAL);

	star6e_recorder_status(NULL, &bytes, &frames, &path, &reason);
	CHECK("recorder status null bytes", bytes == 0);
	CHECK("recorder status null frames", frames == 0);
	CHECK("recorder status null path empty", path[0] == '\0');
	CHECK("recorder status null reason", reason == RECORDER_STOP_MANUAL);
	return failures;
}

static int test_recorder_stop_not_active(void)
{
	Star6eRecorderState state;
	int failures = 0;

	star6e_recorder_init(&state);
	star6e_recorder_stop(&state);
	CHECK("recorder stop not active no crash", 1);
	star6e_recorder_stop(NULL);
	CHECK("recorder stop null no crash", 1);
	return failures;
}

static int test_recorder_trailing_slash(void)
{
	Star6eRecorderState state;
	int failures = 0;
	int ret;
	char dir_with_slash[300];

	snprintf(dir_with_slash, sizeof(dir_with_slash), "%s/", g_test_dir);

	star6e_recorder_init(&state);
	ret = star6e_recorder_start(&state, dir_with_slash);
	CHECK("recorder trailing slash start ok", ret == 0);
	CHECK("recorder trailing slash active",
		star6e_recorder_is_active(&state));
	CHECK("recorder no double slash",
		strstr(state.path, "//") == NULL);
	star6e_recorder_stop(&state);
	return failures;
}

static int test_recorder_free_space(void)
{
	int failures = 0;
	uint64_t space;

	space = star6e_recorder_free_space("/tmp");
	CHECK("recorder free space /tmp > 0", space > 0);

	space = star6e_recorder_free_space("/nonexistent_mount_xyz");
	CHECK("recorder free space bad path is 0", space == 0);

	space = star6e_recorder_free_space(NULL);
	CHECK("recorder free space null is 0", space == 0);

	space = star6e_recorder_free_space("");
	CHECK("recorder free space empty is 0", space == 0);
	return failures;
}

static int test_recorder_dir_stored(void)
{
	Star6eRecorderState state;
	int failures = 0;
	int ret;

	star6e_recorder_init(&state);
	ret = star6e_recorder_start(&state, g_test_dir);
	CHECK("recorder dir stored start ok", ret == 0);
	CHECK("recorder dir stored matches",
		strcmp(state.dir, g_test_dir) == 0);
	star6e_recorder_stop(&state);
	return failures;
}

/* ── star6e_recorder_write_au: the SoC-independent contiguous writer ──── */

static int test_recorder_write_au_not_active(void)
{
	Star6eRecorderState state;
	uint8_t au[] = { 0x00, 0x00, 0x00, 0x01, 0x40, 0x01 };
	int failures = 0;

	star6e_recorder_init(&state);
	CHECK("write_au not active returns 0",
		star6e_recorder_write_au(&state, au, sizeof(au), 0) == 0);
	CHECK("write_au not active wrote nothing", state.bytes_written == 0);
	return failures;
}

static int test_recorder_write_au_null(void)
{
	Star6eRecorderState state;
	uint8_t au[] = { 0x00, 0x00, 0x00, 0x01, 0x40, 0x01 };
	int failures = 0;

	star6e_recorder_init(&state);
	CHECK("write_au null state", star6e_recorder_write_au(NULL, au, 1, 0) == 0);
	CHECK("write_au null buffer",
		star6e_recorder_start(&state, g_test_dir) == 0 &&
		star6e_recorder_write_au(&state, NULL, 4, 0) == 0);
	CHECK("write_au zero length",
		star6e_recorder_write_au(&state, au, 0, 0) == 0);
	CHECK("write_au null/zero left the file empty", state.bytes_written == 0);
	star6e_recorder_stop(&state);
	return failures;
}

static int test_recorder_write_au_bytes(void)
{
	Star6eRecorderState state;
	uint8_t au[] = { 0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xDE, 0xAD, 0xBE };
	uint8_t back[sizeof(au)];
	struct stat st;
	int failures = 0;
	FILE *f;

	star6e_recorder_init(&state);
	CHECK("write_au start ok", star6e_recorder_start(&state, g_test_dir) == 0);

	for (int i = 0; i < 10; i++)
		CHECK("write_au returns byte count",
			star6e_recorder_write_au(&state, au, sizeof(au), 0) ==
				(int)sizeof(au));

	CHECK("write_au frames_written", state.frames_written == 10);
	CHECK("write_au bytes_written",
		state.bytes_written == 10 * sizeof(au));

	/* The bytes must reach the file verbatim — this writer is the whole
	 * recorder interface for a backend that hands over one contiguous AU,
	 * so a silent truncation here would be a silently corrupt recording. */
	f = fopen(state.path, "rb");
	if (f) {
		size_t got = fread(back, 1, sizeof(back), f);
		fclose(f);
		CHECK("write_au first AU read back",
			got == sizeof(au) && memcmp(back, au, sizeof(au)) == 0);
	} else {
		CHECK("write_au file readable", 0);
	}

	star6e_recorder_stop(&state);
	if (stat(state.path, &st) == 0)
		CHECK("write_au file size",
			(size_t)st.st_size == 10 * sizeof(au));
	else
		CHECK("write_au file exists", 0);
	return failures;
}

/* A write to a closed fd must stop the recorder rather than spin: the drain
 * loop calls this unconditionally on every frame. */
static int test_recorder_write_au_error_stops(void)
{
	Star6eRecorderState state;
	uint8_t au[] = { 0x00, 0x00, 0x00, 0x01, 0x26, 0x01 };
	int failures = 0;

	star6e_recorder_init(&state);
	CHECK("write_au err start ok",
		star6e_recorder_start(&state, g_test_dir) == 0);
	CHECK("write_au err first write ok",
		star6e_recorder_write_au(&state, au, sizeof(au), 0) > 0);

	/* Close the descriptor behind the recorder's back; the next write
	 * fails with EBADF, which is neither ENOSPC nor success. */
	close(state.fd);
	CHECK("write_au err returns -1",
		star6e_recorder_write_au(&state, au, sizeof(au), 0) == -1);
	CHECK("write_au err recorder stopped",
		!star6e_recorder_is_active(&state));
	CHECK("write_au err stop reason",
		state.last_stop_reason == RECORDER_STOP_WRITE_ERROR);
	CHECK("write_au err is idempotent",
		star6e_recorder_write_au(&state, au, sizeof(au), 0) == 0);
	return failures;
}

/* The whole point of #123: thresholds that the raw recorder used to ignore.
 * A cut still has to land on an IRAP, so a crossed threshold alone must not
 * produce one. */
static int test_recorder_rotates_on_idr_after_threshold(void)
{
	Star6eRecorderState state;
	uint8_t au[256];
	int failures = 0;

	memset(au, 0xA5, sizeof(au));
	/* A plain slice: the recorder scans the access unit itself, so the
	 * bytes must not lead with a parameter set either. */
	au[0] = 0; au[1] = 0; au[2] = 0; au[3] = 1;
	au[4] = (uint8_t)(1 << 1); au[5] = 0x01;
	star6e_recorder_init(&state);
	CHECK("raw rot start ok",
		star6e_recorder_start(&state, g_test_dir) == 0);
	CHECK("raw rot starts at one segment", state.segments == 1);
	state.rot.max_seconds = 0;
	state.rot.max_bytes = 1;   /* due on the very first frame */

	/* Prime first: the threshold is tested BEFORE the write, so on the very
	 * first access unit segment_bytes is still 0 and the cut-point gate is
	 * never reached -- an unprimed assertion here passes even with the gate
	 * removed entirely. */
	CHECK("raw rot priming write ok",
		star6e_recorder_write_au(&state, au, sizeof(au), 0) > 0);
	CHECK("raw rot priming did not rotate", state.segments == 1);

	/* NOW the threshold is genuinely crossed.  Still no cut point, so still
	 * no cut. */
	CHECK("raw rot non-IRAP write ok",
		star6e_recorder_write_au(&state, au, sizeof(au), 0) > 0);
	CHECK("raw rot did not cut on a non-IRAP", state.segments == 1);

	/* An IRAP answers it -- flagged by the caller, which is the other half
	 * of the gate. */
	CHECK("raw rot IRAP write ok",
		star6e_recorder_write_au(&state, au, sizeof(au), 1) > 0);
	CHECK("raw rot cut on the IRAP", state.segments == 2);
	CHECK("raw rot still recording",
		star6e_recorder_is_recording(&state));
	/* The new segment holds only the AU that opened it, while the lifetime
	 * counter keeps both -- the distinction rotation exists to make. */
	CHECK("raw rot per-segment bytes reset",
		state.rot.segment_bytes == (uint64_t)sizeof(au));
	CHECK("raw rot lifetime bytes kept",
		state.bytes_written == (uint64_t)(3 * sizeof(au)));

	star6e_recorder_stop(&state);
	return failures;
}

/* Rotation must not fire on an IRAP that arrives with no threshold crossed --
 * otherwise every keyframe would start a new file. */
static int test_recorder_no_rotation_without_threshold(void)
{
	Star6eRecorderState state;
	uint8_t au[256];
	int failures = 0;
	int i;

	memset(au, 0xA5, sizeof(au));
	star6e_recorder_init(&state);
	CHECK("raw nothr start ok",
		star6e_recorder_start(&state, g_test_dir) == 0);
	state.rot.max_seconds = 0;
	state.rot.max_bytes = 1024 * 1024;   /* far above what we write */

	for (i = 0; i < 8; i++)
		(void)star6e_recorder_write_au(&state, au, sizeof(au), 1);

	CHECK("raw nothr no rotation on keyframes alone",
		state.segments == 1);
	CHECK("raw nothr armed no wait",
		state.rot.rotation_due_since == 0);
	CHECK("raw nothr wrote everything",
		state.bytes_written == (uint64_t)(8 * sizeof(au)));

	star6e_recorder_stop(&state);
	return failures;
}

/* The GDR case on the raw path: no IRAP ever, rotation still happens, and
 * nothing is asked of the encoder -- an injected IDR would raise the bitrate
 * the live link has to carry, which is what intra-refresh exists to avoid. */
static int test_recorder_rotates_on_param_sets_without_any_idr(void)
{
	Star6eRecorderState state;
	uint8_t slice[256];
	uint8_t vps[256];
	int failures = 0;

	memset(slice, 0xA5, sizeof(slice));
	slice[0] = 0; slice[1] = 0; slice[2] = 0; slice[3] = 1;
	slice[4] = (uint8_t)(1 << 1); slice[5] = 0x01;
	memcpy(vps, slice, sizeof(vps));
	vps[4] = (uint8_t)(32 << 1);

	star6e_recorder_init(&state);
	CHECK("raw gdr start ok",
		star6e_recorder_start(&state, g_test_dir) == 0);
	state.rot.max_seconds = 0;
	state.rot.max_bytes = 1;

	/* is_idr is 0 for every write below -- this stream has no keyframes.
	 * Prime first so the threshold is genuinely crossed; otherwise the
	 * cut-point gate is never consulted and this passes vacuously. */
	CHECK("raw gdr priming write ok",
		star6e_recorder_write_au(&state, slice, sizeof(slice), 0) > 0);
	CHECK("raw gdr priming did not rotate", state.segments == 1);

	CHECK("raw gdr slice write ok",
		star6e_recorder_write_au(&state, slice, sizeof(slice), 0) > 0);
	CHECK("raw gdr no cut on a plain slice", state.segments == 1);

	CHECK("raw gdr wave-head write ok",
		star6e_recorder_write_au(&state, vps, sizeof(vps), 0) > 0);
	CHECK("raw gdr cut on the parameter-set boundary",
		state.segments == 2);
	CHECK("raw gdr still recording",
		star6e_recorder_is_recording(&state));
	CHECK("raw gdr new segment holds only the wave head",
		state.rot.segment_bytes == (uint64_t)sizeof(vps));

	star6e_recorder_stop(&state);
	return failures;
}

/* Rotation must hand out a fresh name every time and lose no segment.
 *
 * NOTE ON WHAT THIS DOES NOT COVER: the O_EXCL-and-retry in
 * open_next_segment() guards a name COLLISION, which this cannot provoke --
 * the name derives from the nanosecond clock, so a test cannot force a repeat.
 * Mutation-checked and confirmed: reverting that open to O_TRUNC leaves this
 * test green.  O_EXCL rests on open(2) semantics, not on this test; what is
 * covered here is that rotation itself neither reuses state->path nor drops a
 * segment. */
static int test_recorder_rotation_hands_out_fresh_names(void)
{
	Star6eRecorderState state;
	uint8_t vps[256];
	char seen[6][RECORDER_PATH_MAX];
	struct stat st;
	int failures = 0;
	int i, j, n = 0;

	memset(vps, 0xA5, sizeof(vps));
	vps[0] = 0; vps[1] = 0; vps[2] = 0; vps[3] = 1;
	vps[4] = (uint8_t)(32 << 1); vps[5] = 0x01;

	star6e_recorder_init(&state);
	CHECK("noerase start ok",
		star6e_recorder_start(&state, g_test_dir) == 0);
	state.rot.max_seconds = 0;
	state.rot.max_bytes = 1;            /* every cut point rotates */
	snprintf(seen[n++], RECORDER_PATH_MAX, "%s", state.path);

	/* The threshold is tested BEFORE the write, so the first access unit
	 * cannot rotate -- segment_bytes is still 0.  Prime it. */
	(void)star6e_recorder_write_au(&state, vps, sizeof(vps), 0);
	CHECK("noerase first write did not rotate", state.segments == 1);

	for (i = 0; i < 5 && n < 6; i++) {
		(void)star6e_recorder_write_au(&state, vps, sizeof(vps), 0);
		snprintf(seen[n++], RECORDER_PATH_MAX, "%s", state.path);
	}
	star6e_recorder_stop(&state);

	/* `n` is loop-controlled, so it proves nothing on its own -- assert the
	 * recorder really did cut a segment for each captured path. */
	CHECK("noerase rotated once per capture",
		state.segments == (uint32_t)n);
	for (i = 0; i < n; i++) {
		for (j = i + 1; j < n; j++) {
			if (strcmp(seen[i], seen[j]) == 0) {
				CHECK("noerase paths are all distinct", 0);
				break;
			}
		}
	}
	/* Every segment must still be on disk: a reused name would have
	 * truncated an earlier one rather than opened a new file. */
	for (i = 0; i < n; i++) {
		int ok = (stat(seen[i], &st) == 0);

		CHECK("noerase every segment still exists", ok);
		if (ok)
			unlink(seen[i]);
	}
	return failures;
}

/* Finalising the OLD segment can fail -- a delayed write surfacing at
 * fdatasync().  Calling that a clean rotation would leave the operator with a
 * possibly short segment and no sign of it. */
static int test_recorder_rotation_reports_finalise_failure(void)
{
	Star6eRecorderState state;
	uint8_t vps[256];
	int failures = 0;
	int pipefd[2];

	memset(vps, 0xA5, sizeof(vps));
	vps[0] = 0; vps[1] = 0; vps[2] = 0; vps[3] = 1;
	vps[4] = (uint8_t)(32 << 1); vps[5] = 0x01;

	star6e_recorder_init(&state);
	CHECK("finfail start ok",
		star6e_recorder_start(&state, g_test_dir) == 0);
	state.rot.max_seconds = 0;
	state.rot.max_bytes = 1;

	/* Prime the segment so the NEXT cut point actually rotates: the
	 * threshold is tested before the write. */
	(void)star6e_recorder_write_au(&state, vps, sizeof(vps), 0);
	CHECK("finfail primed without rotating", state.segments == 1);

	/* Put a pipe under the recorder's descriptor: fdatasync() on a pipe
	 * fails with EINVAL, which is a finalise failure without the fd-reuse
	 * hazard of closing it behind the recorder's back. */
	if (pipe(pipefd) != 0) {
		star6e_recorder_stop(&state);
		return failures;
	}
	if (dup2(pipefd[1], state.fd) < 0) {
		close(pipefd[0]); close(pipefd[1]);
		star6e_recorder_stop(&state);
		return failures;
	}

	CHECK("finfail rotation returns an error",
		star6e_recorder_write_au(&state, vps, sizeof(vps), 0) == -1);
	CHECK("finfail recorder stopped",
		!star6e_recorder_is_recording(&state));
	CHECK("finfail reported as a write error",
		state.last_stop_reason == RECORDER_STOP_WRITE_ERROR);
	CHECK("finfail did not publish a new segment", state.segments == 1);

	close(pipefd[0]); close(pipefd[1]);
	return failures;
}

int test_star6e_recorder(void)
{
	int failures = 0;

	setup_test_dir();

	failures += test_recorder_init();
	failures += test_recorder_init_null();
	failures += test_recorder_start_stop();
	failures += test_recorder_start_bad_dir();
	failures += test_recorder_start_null();
	failures += test_recorder_double_start();
	failures += test_recorder_write_frame();
	failures += test_recorder_write_single_nal();
	failures += test_recorder_write_not_active();
	failures += test_recorder_write_multi_frame();
	failures += test_recorder_status();
	failures += test_recorder_stop_not_active();
	failures += test_recorder_trailing_slash();
	failures += test_recorder_free_space();
	failures += test_recorder_dir_stored();
	failures += test_recorder_write_au_not_active();
	failures += test_recorder_rotates_on_idr_after_threshold();
	failures += test_recorder_rotates_on_param_sets_without_any_idr();
	failures += test_recorder_rotation_hands_out_fresh_names();
	failures += test_recorder_rotation_reports_finalise_failure();
	failures += test_recorder_no_rotation_without_threshold();
	failures += test_recorder_write_au_null();
	failures += test_recorder_write_au_bytes();
	failures += test_recorder_write_au_error_stops();

	cleanup_test_dir();

	return failures;
}
