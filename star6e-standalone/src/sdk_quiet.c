#include "sdk_quiet.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

void sdk_quiet_state_init(SdkQuietState *state)
{
	if (!state) {
		return;
	}
	state->devnull_fd = -1;
	state->saved_stdout = -1;
	state->saved_stderr = -1;
}

static int sdk_quiet_open_devnull(SdkQuietState *state)
{
	if (!state) {
		return -1;
	}
	if (state->devnull_fd >= 0) {
		return 0;
	}

	state->devnull_fd = open("/dev/null", O_WRONLY);
	return (state->devnull_fd >= 0) ? 0 : -1;
}

void sdk_quiet_begin(SdkQuietState *state)
{
	if (!state) {
		return;
	}
	if (state->saved_stdout >= 0 || state->saved_stderr >= 0) {
		return;
	}
	if (sdk_quiet_open_devnull(state) != 0) {
		return;
	}

	fflush(stdout);
	fflush(stderr);
	state->saved_stdout = dup(STDOUT_FILENO);
	state->saved_stderr = dup(STDERR_FILENO);
	if (state->saved_stdout < 0 || state->saved_stderr < 0) {
		if (state->saved_stdout >= 0) {
			close(state->saved_stdout);
			state->saved_stdout = -1;
		}
		if (state->saved_stderr >= 0) {
			close(state->saved_stderr);
			state->saved_stderr = -1;
		}
		return;
	}

	dup2(state->devnull_fd, STDOUT_FILENO);
	dup2(state->devnull_fd, STDERR_FILENO);
}

void sdk_quiet_end(SdkQuietState *state)
{
	if (!state) {
		return;
	}
	if (state->saved_stdout >= 0 || state->saved_stderr >= 0) {
		fflush(stdout);
		fflush(stderr);
	}

	if (state->saved_stdout >= 0) {
		dup2(state->saved_stdout, STDOUT_FILENO);
		close(state->saved_stdout);
		state->saved_stdout = -1;
	}
	if (state->saved_stderr >= 0) {
		dup2(state->saved_stderr, STDERR_FILENO);
		close(state->saved_stderr);
		state->saved_stderr = -1;
	}
}
