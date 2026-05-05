#include "star6e_ts_recorder.h"

#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_test_dir[256];

static void setup_test_dir(void)
{
	snprintf(g_test_dir, sizeof(g_test_dir), "/tmp/venc_ts_rec_test_%d",
		(int)getpid());
	mkdir(g_test_dir, 0755);
}

static void cleanup_test_dir(void)
{
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", g_test_dir);
	(void)system(cmd);
}

static int test_ts_recorder_init(void)
{
	Star6eTsRecorderState state;
	int failures = 0;

	star6e_ts_recorder_init(&state, 16000, 1, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("ts_rec init fd", state.fd == -1);
	CHECK("ts_rec init bytes", state.bytes_written == 0);
	CHECK("ts_rec init frames", state.frames_written == 0);
	CHECK("ts_rec init segments", state.segments == 0);
	CHECK("ts_rec init not active",
		!star6e_ts_recorder_is_active(&state));
	CHECK("ts_rec init max_seconds",
		state.max_seconds == TS_RECORDER_DEFAULT_MAX_SECONDS);

	star6e_ts_recorder_init(NULL, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("ts_rec init null no crash", 1);
	return failures;
}

static int test_ts_recorder_start_stop(void)
{
	Star6eTsRecorderState state;
	int failures = 0;
	int ret;

	star6e_ts_recorder_init(&state, 16000, 1, TS_AUDIO_CODEC_PCM_S302M);
	ret = star6e_ts_recorder_start(&state, g_test_dir, NULL);
	CHECK("ts_rec start ok", ret == 0);
	CHECK("ts_rec active", star6e_ts_recorder_is_active(&state));
	CHECK("ts_rec path not empty", state.path[0] != '\0');
	CHECK("ts_rec path ends .ts",
		strlen(state.path) > 3 &&
		strcmp(state.path + strlen(state.path) - 3, ".ts") == 0);
	CHECK("ts_rec segments after start", state.segments == 1);

	/* PAT/PMT should have been written */
	CHECK("ts_rec bytes after start", state.bytes_written > 0);

	star6e_ts_recorder_stop(&state);
	CHECK("ts_rec not active after stop",
		!star6e_ts_recorder_is_active(&state));
	return failures;
}

static int test_ts_recorder_write_video(void)
{
	Star6eTsRecorderState state;
	int failures = 0;
	int ret;
	struct stat st;
	uint8_t video[500];

	memset(video, 0xAB, sizeof(video));
	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	ret = star6e_ts_recorder_start(&state, g_test_dir, NULL);
	CHECK("ts_rec write start ok", ret == 0);

	ret = star6e_ts_recorder_write_video(&state,
		video, sizeof(video), 90000, 1);
	CHECK("ts_rec write video ok", ret > 0);
	CHECK("ts_rec frames_written", state.frames_written == 1);

	star6e_ts_recorder_stop(&state);

	if (stat(state.path, &st) == 0) {
		CHECK("ts_rec file non-empty", st.st_size > 0);
		CHECK("ts_rec file is TS aligned",
			(st.st_size % TS_PACKET_SIZE) == 0);
	} else {
		CHECK("ts_rec file exists", 0);
	}
	return failures;
}

static int test_ts_recorder_with_audio(void)
{
	Star6eTsRecorderState state;
	AudioRing ring;
	int failures = 0;
	int ret;
	uint8_t video[200];
	uint8_t pcm[640];

	audio_ring_init(&ring);
	star6e_ts_recorder_init(&state, 16000, 1, TS_AUDIO_CODEC_PCM_S302M);
	ret = star6e_ts_recorder_start(&state, g_test_dir, &ring);
	CHECK("ts_rec audio start ok", ret == 0);

	/* Push some audio frames */
	memset(pcm, 0x55, sizeof(pcm));
	audio_ring_push(&ring, pcm, sizeof(pcm), 10000);
	audio_ring_push(&ring, pcm, sizeof(pcm), 20000);

	/* Write a video frame (should drain audio too) */
	memset(video, 0xCC, sizeof(video));
	ret = star6e_ts_recorder_write_video(&state,
		video, sizeof(video), 90000, 1);
	CHECK("ts_rec with audio write ok", ret > 0);

	/* Audio ring should be empty now */
	CHECK("ts_rec audio ring drained", audio_ring_count(&ring) == 0);

	star6e_ts_recorder_stop(&state);
	audio_ring_destroy(&ring);
	return failures;
}

static int test_ts_recorder_not_active(void)
{
	Star6eTsRecorderState state;
	uint8_t video[10];
	int failures = 0;

	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("ts_rec write not active returns 0",
		star6e_ts_recorder_write_video(&state, video, 10, 0, 0) == 0);
	return failures;
}

static int test_ts_recorder_status(void)
{
	Star6eTsRecorderState state;
	int failures = 0;
	uint64_t bytes;
	uint32_t frames, segs;
	const char *path;
	Star6eRecorderStopReason reason;

	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	star6e_ts_recorder_status(&state, &bytes, &frames, &segs,
		&path, &reason);
	CHECK("ts_rec status init bytes", bytes == 0);
	CHECK("ts_rec status init frames", frames == 0);
	CHECK("ts_rec status init segments", segs == 0);
	CHECK("ts_rec status init reason", reason == RECORDER_STOP_MANUAL);

	star6e_ts_recorder_status(NULL, &bytes, &frames, &segs,
		&path, &reason);
	CHECK("ts_rec status null bytes", bytes == 0);
	CHECK("ts_rec status null path empty", path[0] == '\0');
	return failures;
}

static int test_ts_recorder_start_null(void)
{
	Star6eTsRecorderState state;
	int failures = 0;

	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("ts_rec start null state",
		star6e_ts_recorder_start(NULL, g_test_dir, NULL) == -1);
	CHECK("ts_rec start null dir",
		star6e_ts_recorder_start(&state, NULL, NULL) == -1);
	CHECK("ts_rec start empty dir",
		star6e_ts_recorder_start(&state, "", NULL) == -1);
	return failures;
}

static int test_ts_recorder_multi_frame(void)
{
	Star6eTsRecorderState state;
	int failures = 0;
	int ret;
	uint8_t video[100];

	memset(video, 0xEE, sizeof(video));
	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	ret = star6e_ts_recorder_start(&state, g_test_dir, NULL);
	CHECK("ts_rec multi start ok", ret == 0);

	for (int i = 0; i < 10; i++) {
		ret = star6e_ts_recorder_write_video(&state,
			video, sizeof(video), (uint64_t)i * 3000, i == 0);
		CHECK("ts_rec multi write ok", ret > 0);
	}
	CHECK("ts_rec multi frames", state.frames_written == 10);

	star6e_ts_recorder_stop(&state);
	return failures;
}

int test_star6e_ts_recorder(void)
{
	int failures = 0;

	setup_test_dir();

	failures += test_ts_recorder_init();
	failures += test_ts_recorder_start_stop();
	failures += test_ts_recorder_write_video();
	failures += test_ts_recorder_with_audio();
	failures += test_ts_recorder_not_active();
	failures += test_ts_recorder_status();
	failures += test_ts_recorder_start_null();
	failures += test_ts_recorder_multi_frame();

	cleanup_test_dir();
	return failures;
}
