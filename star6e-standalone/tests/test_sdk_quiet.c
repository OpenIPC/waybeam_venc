#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sdk_quiet.h"
#include "test_helpers.h"

static int test_sdk_quiet_suppresses_buffered_output(void)
{
	char capture_path[] = "/tmp/test_sdk_quiet_capture_XXXXXX";
	char capture_buf[64];
	SdkQuietState state;
	int capture_fd;
	int saved_stdout;
	int saved_stderr;
	ssize_t captured;

	saved_stdout = dup(STDOUT_FILENO);
	saved_stderr = dup(STDERR_FILENO);
	if (saved_stdout < 0 || saved_stderr < 0) {
		if (saved_stdout >= 0) {
			close(saved_stdout);
		}
		if (saved_stderr >= 0) {
			close(saved_stderr);
		}
		return 0;
	}

	capture_fd = mkstemp(capture_path);
	if (capture_fd < 0) {
		close(saved_stdout);
		close(saved_stderr);
		return 0;
	}

	fflush(stdout);
	fflush(stderr);
	if (dup2(capture_fd, STDOUT_FILENO) < 0 ||
		dup2(capture_fd, STDERR_FILENO) < 0) {
		dup2(saved_stdout, STDOUT_FILENO);
		dup2(saved_stderr, STDERR_FILENO);
		close(saved_stdout);
		close(saved_stderr);
		close(capture_fd);
		unlink(capture_path);
		return 0;
	}

	sdk_quiet_state_init(&state);
	sdk_quiet_begin(&state);
	printf("suppressed-buffered-output");
	sdk_quiet_end(&state);
	fflush(stdout);
	fflush(stderr);

	dup2(saved_stdout, STDOUT_FILENO);
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stdout);
	close(saved_stderr);

	lseek(capture_fd, 0, SEEK_SET);
	captured = read(capture_fd, capture_buf, sizeof(capture_buf));
	close(capture_fd);
	unlink(capture_path);

	if (captured < 0) {
		return 0;
	}
	if (captured == 0) {
		return 1;
	}
	return captured < (ssize_t)sizeof(capture_buf) &&
		memcmp(capture_buf, "suppressed-buffered-output",
			(size_t)captured) != 0;
}

int test_sdk_quiet(void)
{
	int failures = 0;
	SdkQuietState state;
	int begin_ok;
	int end_ok;
	int nested_ok;

	sdk_quiet_state_init(&state);
	CHECK("sdk_quiet_init_devnull", state.devnull_fd == -1);
	CHECK("sdk_quiet_init_stdout", state.saved_stdout == -1);
	CHECK("sdk_quiet_init_stderr", state.saved_stderr == -1);

	sdk_quiet_begin(NULL);
	sdk_quiet_end(NULL);
	CHECK("sdk_quiet_null_safe", 1);

	sdk_quiet_begin(&state);
	begin_ok = (state.devnull_fd >= 0 &&
		state.saved_stdout >= 0 &&
		state.saved_stderr >= 0);
	sdk_quiet_end(&state);
	end_ok = (state.saved_stdout == -1 &&
		state.saved_stderr == -1);
	CHECK("sdk_quiet_begin_ok", begin_ok);
	CHECK("sdk_quiet_end_ok", end_ok);

	sdk_quiet_begin(&state);
	sdk_quiet_begin(&state);
	nested_ok = (state.saved_stdout >= 0 &&
		state.saved_stderr >= 0);
	sdk_quiet_end(&state);
	CHECK("sdk_quiet_nested_safe",
		nested_ok &&
		state.saved_stdout == -1 &&
		state.saved_stderr == -1);
	CHECK("sdk_quiet_suppresses_output",
		test_sdk_quiet_suppresses_buffered_output());

	return failures;
}
