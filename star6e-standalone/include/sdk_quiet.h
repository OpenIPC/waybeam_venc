#ifndef SDK_QUIET_H
#define SDK_QUIET_H

typedef struct {
	int devnull_fd;
	int saved_stdout;
	int saved_stderr;
} SdkQuietState;

#define SDK_QUIET_STATE_INIT { \
	.devnull_fd = -1, \
	.saved_stdout = -1, \
	.saved_stderr = -1, \
}

/** Initialize quiet state (opens /dev/null). */
void sdk_quiet_state_init(SdkQuietState *state);

/** Redirect stdout/stderr to /dev/null to silence SDK output. */
void sdk_quiet_begin(SdkQuietState *state);

/** Restore original stdout/stderr file descriptors. */
void sdk_quiet_end(SdkQuietState *state);

#endif /* SDK_QUIET_H */
