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

	cleanup_test_dir();

	return failures;
}
