#include "star6e_ts_recorder.h"

#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <signal.h>
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

/* A status poll runs on the httpd thread while the writer thread is inside
 * write_video() and segment rotation.  Star6E used to document those reads as
 * safe because the fields were "single words written by one thread" -- but
 * bytes_written is 64-bit and these targets are ARM32, so an unsynchronised
 * load can tear outright, and `path` is not a word at all: rotation rewrites
 * the whole buffer.
 *
 * Asserts what a torn or mixed read would violate -- monotonic counters and a
 * non-empty path whenever the snapshot says active -- and is written to be run
 * under TSAN, which is what actually proves the absence of the race. */
static void *status_writer_thread(void *arg)
{
	Star6eTsRecorderState *state = arg;
	uint8_t video[512];
	int i;

	memset(video, 0x5A, sizeof(video));
	for (i = 0; i < 400; i++)
		(void)star6e_ts_recorder_write_video(state, video,
			sizeof(video), (uint64_t)i * 3000, i == 0);
	return NULL;
}

static int test_ts_recorder_status_snapshot_is_coherent(void)
{
	Star6eTsRecorderState state;
	Star6eRecorderSnapshot snap;
	pthread_t writer;
	uint64_t last_bytes = 0;
	uint32_t last_frames = 0;
	int failures = 0;
	int polls = 0;
	int monotonic = 1;
	int path_ok = 1;

	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	/* Rotate often, so the poll races a path rewrite rather than only
	 * counter bumps -- the rotation is the interesting window. */
	state.max_bytes = 64 * 1024;
	CHECK("snapshot start ok",
		star6e_ts_recorder_start(&state, g_test_dir, NULL) == 0);

	CHECK("snapshot writer spawn",
		pthread_create(&writer, NULL, status_writer_thread, &state) == 0);

	/* Poll until the writer has finished, not a fixed count: an unbounded
	 * spin of 3000 iterations completes before the writer thread is even
	 * scheduled, and then nothing is actually raced. */
	while (last_frames < 400 && polls < 20000000) {
		star6e_ts_recorder_snapshot(&state, &snap);
		if (snap.bytes_written < last_bytes ||
		    snap.frames_written < last_frames)
			monotonic = 0;
		if (snap.active && snap.path[0] == '\0')
			path_ok = 0;
		last_bytes = snap.bytes_written;
		last_frames = snap.frames_written;
		polls++;
	}
	pthread_join(writer, NULL);

	CHECK("snapshot counters never went backwards", monotonic == 1);
	CHECK("snapshot never reported active with an empty path", path_ok == 1);
	CHECK("snapshot observed real progress", last_frames > 0);

	star6e_ts_recorder_snapshot(&state, &snap);
	CHECK("snapshot sees the writer's total",
		snap.frames_written == 400);
	star6e_ts_recorder_stop(&state);
	star6e_ts_recorder_snapshot(&state, &snap);
	CHECK("snapshot inactive after stop", snap.active == 0);
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

/* ── Rotation IDR request (the GDR path) ──────────────────────────────── */

/* Drive rotation-due by size, with a non-IRAP frame, and check the recorder
 * ASKS instead of silently waiting forever. */
static int test_ts_rotation_requests_idr_when_not_idr(void)
{
	Star6eTsRecorderState state;
	uint8_t video[500];
	int failures = 0;

	memset(video, 0xAB, sizeof(video));
	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("rot start ok",
		star6e_ts_recorder_start(&state, g_test_dir, NULL) == 0);
	state.max_seconds = 0;
	state.max_bytes = 1;   /* rotation due on the very first frame */

	CHECK("rot no request before any write",
		star6e_ts_recorder_take_idr_request(&state) == 0);

	/* First crossing only ARMS the grace period — a keyframing stream gets
	 * a chance to rotate on its own before anything is asked for. */
	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 0);
	CHECK("rot grace armed, nothing asked yet",
		state.idr_requests_unanswered == 0);
	CHECK("rot no flag during grace",
		star6e_ts_recorder_take_idr_request(&state) == 0);

	/* Grace elapsed: now it asks. */
	state.rotation_due_since -= 2;
	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 0);
	CHECK("rot did not cut on a non-IRAP", state.segments == 1);
	CHECK("rot asked for an IDR", state.idr_requests_unanswered == 1);
	CHECK("rot take returns the request",
		star6e_ts_recorder_take_idr_request(&state) == 1);
	CHECK("rot take is one-shot",
		star6e_ts_recorder_take_idr_request(&state) == 0);

	/* Same second: paced, so no second ask. */
	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 0);
	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 0);
	CHECK("rot paced within one second",
		state.idr_requests_unanswered == 1);
	CHECK("rot no new flag while paced",
		star6e_ts_recorder_take_idr_request(&state) == 0);

	star6e_ts_recorder_stop(&state);
	return failures;
}

/* A keyframing stream must cost ZERO extra keyframes: within the grace
 * window no request is made at all, however many frames arrive.  Measured on
 * Maruko before this guard existed, resilience=off/max_seconds=15 turned 1
 * honored IDR into 4 for rotations that happened naturally anyway. */
static int test_ts_rotation_grace_suppresses_requests(void)
{
	Star6eTsRecorderState state;
	uint8_t video[500];
	int failures = 0;
	int i;

	memset(video, 0xAB, sizeof(video));
	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("grace start ok",
		star6e_ts_recorder_start(&state, g_test_dir, NULL) == 0);
	state.max_seconds = 0;
	state.max_bytes = 1;   /* due immediately */

	/* Many non-IRAP frames, all inside the grace second: not one ask. */
	for (i = 0; i < 30; i++)
		(void)star6e_ts_recorder_write_video(&state, video,
			sizeof(video), 90000, 0);
	CHECK("grace suppressed every request",
		state.idr_requests_unanswered == 0);
	CHECK("grace raised no flag",
		star6e_ts_recorder_take_idr_request(&state) == 0);
	CHECK("grace armed the timer", state.rotation_due_since != 0);
	CHECK("grace did not rotate", state.segments == 1);

	/* A natural IRAP inside the window rotates and disarms — the
	 * keyframing-stream path, with no request ever issued. */
	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 1);
	CHECK("grace rotated naturally", state.segments == 2);
	CHECK("grace disarmed after rotation", state.rotation_due_since == 0);
	CHECK("grace never asked", state.idr_requests_unanswered == 0);

	star6e_ts_recorder_stop(&state);
	return failures;
}

/* The answering IRAP must rotate AND clear the outstanding ask, so the next
 * threshold starts from a clean slate. */
static int test_ts_rotation_idr_answers_and_resets(void)
{
	Star6eTsRecorderState state;
	uint8_t video[500];
	int failures = 0;

	memset(video, 0xAB, sizeof(video));
	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("rot2 start ok",
		star6e_ts_recorder_start(&state, g_test_dir, NULL) == 0);
	state.max_seconds = 0;
	state.max_bytes = 1;

	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 0);
	state.rotation_due_since -= 2;
	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 0);
	CHECK("rot2 asked", state.idr_requests_unanswered == 1);

	/* IRAP arrives: cut, and the ask is considered answered. */
	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 1);
	CHECK("rot2 rotated on the IRAP", state.segments == 2);
	CHECK("rot2 cleared the outstanding ask",
		state.idr_requests_unanswered == 0);
	CHECK("rot2 cleared the pacing anchor",
		state.idr_request_last_sec == 0);

	star6e_ts_recorder_stop(&state);
	return failures;
}

/* An ask nobody answers must give up, or it becomes a permanent 1 Hz
 * keyframe tax on the live link for the whole recording. */
static int test_ts_rotation_idr_requests_are_bounded(void)
{
	Star6eTsRecorderState state;
	uint8_t video[500];
	int failures = 0;
	int i;

	memset(video, 0xAB, sizeof(video));
	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("rot3 start ok",
		star6e_ts_recorder_start(&state, g_test_dir, NULL) == 0);
	state.max_seconds = 0;
	state.max_bytes = 1;

	/* Forge the pacing anchor backwards each round so every write is
	 * eligible, without the test sleeping for N seconds. */
	for (i = 0; i < TS_RECORDER_MAX_IDR_REQUESTS + 5; i++) {
		state.idr_request_last_sec -= 2;
		state.rotation_due_since -= 2;
		(void)star6e_ts_recorder_write_video(&state, video,
			sizeof(video), 90000, 0);
	}
	CHECK("rot3 stopped asking at the bound",
		state.idr_requests_unanswered == TS_RECORDER_MAX_IDR_REQUESTS);

	star6e_ts_recorder_stop(&state);
	return failures;
}

/* Pre-0.70.0 behaviour must survive for anyone who never services the flag:
 * rotation simply waits for a natural keyframe. */
static int test_ts_rotation_unserviced_still_rotates_on_natural_idr(void)
{
	Star6eTsRecorderState state;
	uint8_t video[500];
	int failures = 0;

	memset(video, 0xAB, sizeof(video));
	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("rot4 start ok",
		star6e_ts_recorder_start(&state, g_test_dir, NULL) == 0);
	state.max_seconds = 0;
	state.max_bytes = 1;

	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 0);
	state.rotation_due_since -= 2;
	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 0);   /* flag raised, nobody takes it */
	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 1);   /* a natural IRAP still cuts */
	CHECK("rot4 rotated without the flag being serviced",
		state.segments == 2);

	star6e_ts_recorder_stop(&state);
	return failures;
}

/* A stop/restart inside the same second must not inherit the previous
 * recording's pacing anchor and swallow the new one's first request. */
static int test_ts_rotation_state_resets_on_restart(void)
{
	Star6eTsRecorderState state;
	uint8_t video[500];
	int failures = 0;

	memset(video, 0xAB, sizeof(video));
	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("rot5 start ok",
		star6e_ts_recorder_start(&state, g_test_dir, NULL) == 0);
	state.max_seconds = 0;
	state.max_bytes = 1;
	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 0);
	state.rotation_due_since -= 2;
	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 0);
	CHECK("rot5 asked once", state.idr_requests_unanswered == 1);
	star6e_ts_recorder_stop(&state);

	CHECK("rot5 restart ok",
		star6e_ts_recorder_start(&state, g_test_dir, NULL) == 0);
	CHECK("rot5 unanswered reset", state.idr_requests_unanswered == 0);
	CHECK("rot5 anchor reset", state.idr_request_last_sec == 0);
	CHECK("rot5 grace reset", state.rotation_due_since == 0);
	CHECK("rot5 pending reset",
		star6e_ts_recorder_take_idr_request(&state) == 0);

	state.max_seconds = 0;
	state.max_bytes = 1;
	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 0);
	state.rotation_due_since -= 2;
	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 0);
	CHECK("rot5 new recording asks immediately",
		state.idr_requests_unanswered == 1);

	star6e_ts_recorder_stop(&state);
	return failures;
}

/* No rotation due -> never ask.  Guards against a threshold bug turning into
 * a permanent keyframe request. */
static int test_ts_no_request_when_rotation_not_due(void)
{
	Star6eTsRecorderState state;
	uint8_t video[500];
	int failures = 0;
	int i;

	memset(video, 0xAB, sizeof(video));
	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("rot6 start ok",
		star6e_ts_recorder_start(&state, g_test_dir, NULL) == 0);
	state.max_seconds = 100000;
	state.max_bytes = 1024ULL * 1024 * 1024;

	for (i = 0; i < 20; i++)
		(void)star6e_ts_recorder_write_video(&state, video,
			sizeof(video), 90000, 0);
	CHECK("rot6 never asked", state.idr_requests_unanswered == 0);
	CHECK("rot6 no pending flag",
		star6e_ts_recorder_take_idr_request(&state) == 0);
	CHECK("rot6 single segment", state.segments == 1);

	star6e_ts_recorder_stop(&state);
	return failures;
}

/* take_idr_request must tolerate NULL — maruko passes d->ts_recorder, which
 * is NULL in dual-stream mode. */
static int test_ts_take_idr_request_null_safe(void)
{
	int failures = 0;

	CHECK("rot7 NULL state safe",
		star6e_ts_recorder_take_idr_request(NULL) == 0);
	return failures;
}

/* S2's primitive: a producer must be able to tell "recording in progress"
 * apart from "a segment file is open at this instant".  They differ for the
 * whole of a rotation, and a producer that cannot tell them apart drops every
 * frame in that window — silently, right after the new segment's IRAP.
 *
 * Rotation is not observable from outside write_video() (it closes and
 * reopens inside one call), so what is pinned here are the transitions at
 * either end of it: a rotation that succeeds must NOT clear the flag; one
 * that cannot reopen must. */
static int test_ts_is_recording_survives_rotation_but_not_a_stop(void)
{
	Star6eTsRecorderState state;
	uint8_t video[500];
	int failures = 0;

	memset(video, 0xAB, sizeof(video));
	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("is_recording false before start",
		!star6e_ts_recorder_is_recording(&state));
	CHECK("is_recording null safe",
		!star6e_ts_recorder_is_recording(NULL));

	CHECK("rec start ok",
		star6e_ts_recorder_start(&state, g_test_dir, NULL) == 0);
	CHECK("is_recording true after start",
		star6e_ts_recorder_is_recording(&state) == 1);

	state.max_seconds = 0;
	state.max_bytes = 1;            /* rotation due immediately */
	state.rotation_due_since = 1;   /* skip the grace window */

	/* An IRAP cuts the segment: close + reopen, all inside this call. */
	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 1);
	CHECK("rotation happened", state.segments == 2);
	CHECK("is_recording survives a rotation",
		star6e_ts_recorder_is_recording(&state) == 1);
	CHECK("is_active also true once the new segment is open",
		star6e_ts_recorder_is_active(&state) == 1);

	/* The two assertions above cannot tell the predicates apart: at every
	 * quiescent point fd >= 0 and recording == 1 agree, so a is_recording()
	 * that merely returned `fd >= 0` would satisfy them.  The state that
	 * separates them exists only INSIDE write_video(), between close() and
	 * open(), and lasts microseconds on tmpfs — too short to sample from
	 * another thread with any reliability.  So construct it: this is
	 * exactly what the encode loop sees while the writer thread rotates on
	 * an SD card, and it is where every dropped frame in S2 came from. */
	state.fd = -1;
	state.recording = 1;
	CHECK("mid-rotation: no file is open",
		!star6e_ts_recorder_is_active(&state));
	CHECK("mid-rotation: the recording is still running",
		star6e_ts_recorder_is_recording(&state) == 1);
	CHECK("rot restore", (state.fd = open(state.path, O_WRONLY)) >= 0);

	star6e_ts_recorder_stop(&state);
	CHECK("is_recording false after stop",
		!star6e_ts_recorder_is_recording(&state));
	return failures;
}

/* The other transition: a rotation whose reopen fails is a stop, not a gap.
 * Making the directory unwritable mid-recording is the cheapest way to force
 * open_new_segment() to fail on a path that already worked once. */
static int test_ts_is_recording_clears_when_rotation_cannot_reopen(void)
{
	Star6eTsRecorderState state;
	uint8_t video[500];
	char subdir[320];
	int failures = 0;

	snprintf(subdir, sizeof(subdir), "%s/rotfail", g_test_dir);
	(void)mkdir(subdir, 0755);

	memset(video, 0xAB, sizeof(video));
	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("rotfail start ok",
		star6e_ts_recorder_start(&state, subdir, NULL) == 0);
	CHECK("rotfail recording true", star6e_ts_recorder_is_recording(&state) == 1);

	state.max_seconds = 0;
	state.max_bytes = 1;
	state.rotation_due_since = 1;
	CHECK("rotfail chmod", chmod(subdir, 0500) == 0);

	(void)star6e_ts_recorder_write_video(&state, video, sizeof(video),
		90000, 1);

	CHECK("rotfail no file open", !star6e_ts_recorder_is_active(&state));
	CHECK("rotfail recording cleared",
		!star6e_ts_recorder_is_recording(&state));
	CHECK("rotfail reason recorded",
		state.last_stop_reason == RECORDER_STOP_WRITE_ERROR);

	(void)chmod(subdir, 0755);
	star6e_ts_recorder_stop(&state);
	return failures;
}

/* The rotation invariant, tested through the REAL rotation path.
 *
 * The construct-the-state test above pins which field each accessor reads.  It
 * cannot pin what check_rotation() leaves behind, and that is the actual
 * invariant: a mutant that clears `recording` for the duration of the
 * close/reopen — which IS the bug this all exists to fix — satisfies every
 * quiescent assertion.
 *
 * I claimed the window was too short to sample from another thread.  That was
 * wrong: a spin-sampling reader lands inside it on a large fraction of reads,
 * so two rotations are enough to catch it.  What the sampler asks is exactly
 * what every backend's drain loop asks, once per frame:
 * star6e_record_wants_frame().
 *
 * The consequence of a false reading is now a TEARDOWN, not a dropped frame.
 * The drain loop reads this as "the recorder stopped itself" and joins the
 * writer, so a predicate that blinked during a rotation would end a healthy
 * recording once per segment instead of costing it a few frames. */
typedef struct {
	Star6eTsRecorderState *ts;
	Star6eRecorderState   *hevc;
	int                    stop;
	int                    running;           /* published on entry */
	unsigned long          samples;
	unsigned long          would_tear_down;   /* must stay 0 */
} RotationSampler;

static void *rotation_sampler(void *arg)
{
	RotationSampler *s = arg;

	__atomic_store_n(&s->running, 1, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
		s->samples++;
		if (!star6e_record_wants_frame(s->ts, s->hevc))
			s->would_tear_down++;
	}
	return NULL;
}

static int test_ts_rotation_never_makes_a_producer_drop(void)
{
	Star6eTsRecorderState state;
	Star6eRecorderState hevc;
	RotationSampler s;
	pthread_t th;
	uint8_t video[500];
	int failures = 0;
	int i;

	memset(video, 0xAB, sizeof(video));
	star6e_recorder_init(&hevc);          /* never started: TS-only recording */
	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("rotdrop start ok",
		star6e_ts_recorder_start(&state, g_test_dir, NULL) == 0);

	state.max_seconds = 0;
	state.max_bytes = 1;            /* rotate on every IRAP */
	state.rotation_due_since = 1;   /* skip the grace window */

	memset(&s, 0, sizeof(s));
	s.ts = &state;
	s.hevc = &hevc;
	CHECK("rotdrop sampler start",
		pthread_create(&th, NULL, rotation_sampler, &s) == 0);

	/* Wait for the sampler to actually be on a CPU before the writes start.
	 * Without this the test fails on the MACHINE rather than on the code: on
	 * an oversubscribed box the sampler can be scheduled zero times before
	 * the 40 writes finish, and the samples floor below then trips on
	 * correct code (measured: 6 of 8 runs at 4x oversubscription, with
	 * samples reading 0).  A gate must not depend on the author's load. */
	{
		int spins = 0;

		while (!__atomic_load_n(&s.running, __ATOMIC_ACQUIRE) &&
		       spins < 50000) {
			usleep(200);
			spins++;
		}
		CHECK("rotdrop sampler reached a CPU",
			__atomic_load_n(&s.running, __ATOMIC_ACQUIRE) == 1);
	}

	/* Every IRAP cuts a segment, so this is 40 real close/reopen windows
	 * with a reader spinning on the producer's own question throughout. */
	for (i = 0; i < 40; i++)
		(void)star6e_ts_recorder_write_video(&state, video,
			sizeof(video), (uint64_t)i * 1500, 1);

	__atomic_store_n(&s.stop, 1, __ATOMIC_RELEASE);
	pthread_join(th, NULL);

	CHECK("rotdrop rotations happened", state.segments >= 40);
	CHECK("rotdrop sampler actually sampled", s.samples > 1000);
	/* The whole point: not one sample, across 40 rotations, told the drain
	 * loop the recorder had stopped itself. */
	CHECK("rotation never looks like a self-stop", s.would_tear_down == 0);

	star6e_ts_recorder_stop(&state);
	CHECK("rotdrop stops the recording",
		!star6e_record_wants_frame(&state, &hevc));
	return failures;
}

/* A start that fails must not leave a producer thinking a recording is
 * running.  Nothing exercised a failing start, so the ordering that makes the
 * flag safe — published only after open_new_segment() succeeds — was a comment
 * with no test under it. */
static int test_ts_failed_start_leaves_nothing_recording(void)
{
	Star6eTsRecorderState state;
	Star6eRecorderState hevc;
	int failures = 0;

	star6e_recorder_init(&hevc);
	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);

	CHECK("failed start is reported",
		star6e_ts_recorder_start(&state, "/nonexistent/path/xyz",
			NULL) != 0);
	CHECK("failed start records nothing",
		!star6e_ts_recorder_is_recording(&state));
	CHECK("failed start opens no file",
		!star6e_ts_recorder_is_active(&state));
	CHECK("failed start leaves nothing for the drain loop to keep alive",
		!star6e_record_wants_frame(&state, &hevc));

	/* And a good start after a failed one still works. */
	CHECK("start after a failed start succeeds",
		star6e_ts_recorder_start(&state, g_test_dir, NULL) == 0);
	CHECK("start after a failed start records",
		star6e_record_wants_frame(&state, &hevc));
	star6e_ts_recorder_stop(&state);
	return failures;
}

/* A file-size ceiling BELOW the off_t one -- FAT32 stops at 4 GB -- is what
 * the pre-write guard cannot anticipate, so the failure lands in write_all()
 * mid-access-unit.  RLIMIT_FSIZE reproduces exactly that on any build, which
 * is what makes this reachable from a host test at all: the off_t ceiling
 * itself is unreachable where off_t is 64-bit.
 *
 * Two things must hold: the stop is classified as a size limit rather than a
 * write error, and the partial write is rolled back so the .ts ends on a
 * complete access unit -- measured on an unpatched build, the recorder's own
 * counter stopped at 2147464968 while the file was 2147483647, leaving 18679
 * bytes of half-written AU behind. */
static int test_ts_size_limit_rolls_back_partial_write(void)
{
	Star6eTsRecorderState state;
	uint8_t video[16 * 1024];
	struct rlimit saved, small;
	void (*saved_xfsz)(int);
	struct stat st;
	char path[RECORDER_PATH_MAX];
	int failures = 0;
	int i;

	memset(video, 0xC3, sizeof(video));
	if (getrlimit(RLIMIT_FSIZE, &saved) != 0)
		return 0;                       /* cannot pose the question */
	/* Exceeding RLIMIT_FSIZE also raises SIGXFSZ, whose default action is
	 * to kill the test runner. */
	saved_xfsz = signal(SIGXFSZ, SIG_IGN);

	star6e_ts_recorder_init(&state, 0, 0, TS_AUDIO_CODEC_PCM_S302M);
	CHECK("efbig start ok",
		star6e_ts_recorder_start(&state, g_test_dir, NULL) == 0);
	snprintf(path, sizeof(path), "%s", state.path);

	small = saved;
	small.rlim_cur = 100 * 1024;        /* a few frames in */
	if (setrlimit(RLIMIT_FSIZE, &small) != 0) {
		star6e_ts_recorder_stop(&state);
		signal(SIGXFSZ, saved_xfsz);
		return failures;
	}

	/* Write past the limit.  The crossing write is short, then the next
	 * one fails with EFBIG -- the partial-write case. */
	for (i = 0; i < 64; i++) {
		if (star6e_ts_recorder_write_video(&state, video,
			sizeof(video), (uint64_t)i * 3000, i == 0) < 0)
			break;
	}

	(void)setrlimit(RLIMIT_FSIZE, &saved);
	signal(SIGXFSZ, saved_xfsz);

	CHECK("efbig stopped the recorder",
		!star6e_ts_recorder_is_recording(&state));
	CHECK("efbig is a size limit, not a write error",
		state.last_stop_reason == RECORDER_STOP_SIZE_LIMIT);
	/* The rollback target: segment_bytes only ever advances by completed
	 * writes, so the file must end exactly there -- and therefore on a
	 * whole number of TS packets. */
	CHECK("efbig file exists", stat(path, &st) == 0);
	CHECK("efbig rolled back to the frame boundary",
		(uint64_t)st.st_size == state.segment_bytes);
	CHECK("efbig left whole TS packets only",
		(st.st_size % TS_PACKET_SIZE) == 0);

	star6e_ts_recorder_stop(&state);
	unlink(path);
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
	failures += test_ts_recorder_status_snapshot_is_coherent();
	failures += test_ts_recorder_start_null();
	failures += test_ts_recorder_multi_frame();
	failures += test_ts_rotation_requests_idr_when_not_idr();
	failures += test_ts_rotation_grace_suppresses_requests();
	failures += test_ts_rotation_idr_answers_and_resets();
	failures += test_ts_rotation_idr_requests_are_bounded();
	failures += test_ts_rotation_unserviced_still_rotates_on_natural_idr();
	failures += test_ts_rotation_state_resets_on_restart();
	failures += test_ts_no_request_when_rotation_not_due();
	failures += test_ts_take_idr_request_null_safe();
	failures += test_ts_is_recording_survives_rotation_but_not_a_stop();
	failures += test_ts_is_recording_clears_when_rotation_cannot_reopen();
	failures += test_ts_rotation_never_makes_a_producer_drop();
	failures += test_ts_failed_start_leaves_nothing_recording();
	failures += test_ts_size_limit_rolls_back_partial_write();

	cleanup_test_dir();
	return failures;
}
