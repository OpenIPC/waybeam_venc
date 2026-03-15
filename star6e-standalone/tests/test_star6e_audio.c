#include "star6e_audio.h"

#include "test_helpers.h"

#include <string.h>

int MI_SYS_ConfigPrivateMMAPool(void *config)
{
	(void)config;
	return 0;
}

int MI_SYS_SetChnOutputPortDepth(const MI_SYS_ChnPort_t *port,
	unsigned int user_depth, unsigned int buf_depth)
{
	(void)port;
	(void)user_depth;
	(void)buf_depth;
	return 0;
}

static int g_test_star6e_audio_mute_called;
static int g_test_star6e_audio_mute_active;
static int g_test_star6e_audio_mute_return;

static int test_star6e_audio_set_mute_stub(int device, int channel, char active)
{
	(void)device;
	(void)channel;
	g_test_star6e_audio_mute_called++;
	g_test_star6e_audio_mute_active = active;
	return g_test_star6e_audio_mute_return;
}

static int test_star6e_audio_apply_mute_null_safe(void)
{
	int failures = 0;

	CHECK("star6e audio mute null", star6e_audio_apply_mute(NULL, 1) == -1);
	return failures;
}

static int test_star6e_audio_apply_mute_requires_active_channel(void)
{
	Star6eAudioState state;
	int failures = 0;

	memset(&state, 0, sizeof(state));
	CHECK("star6e audio mute inactive",
		star6e_audio_apply_mute(&state, 1) == -1);
	return failures;
}

static int test_star6e_audio_apply_mute_ok(void)
{
	Star6eAudioState state;
	int failures = 0;

	memset(&state, 0, sizeof(state));
	state.lib_loaded = 1;
	state.channel_enabled = 1;
	state.lib.fnSetMute = test_star6e_audio_set_mute_stub;
	g_test_star6e_audio_mute_called = 0;
	g_test_star6e_audio_mute_active = -1;
	g_test_star6e_audio_mute_return = 0;

	CHECK("star6e audio mute ok", star6e_audio_apply_mute(&state, 1) == 0);
	CHECK("star6e audio mute called", g_test_star6e_audio_mute_called == 1);
	CHECK("star6e audio mute active", g_test_star6e_audio_mute_active == 1);
	return failures;
}

static int test_star6e_audio_apply_mute_fail(void)
{
	Star6eAudioState state;
	int failures = 0;

	memset(&state, 0, sizeof(state));
	state.lib_loaded = 1;
	state.channel_enabled = 1;
	state.lib.fnSetMute = test_star6e_audio_set_mute_stub;
	g_test_star6e_audio_mute_called = 0;
	g_test_star6e_audio_mute_active = -1;
	g_test_star6e_audio_mute_return = -7;

	CHECK("star6e audio mute fail", star6e_audio_apply_mute(&state, 0) == -1);
	CHECK("star6e audio mute fail called", g_test_star6e_audio_mute_called == 1);
	CHECK("star6e audio mute fail active", g_test_star6e_audio_mute_active == 0);
	return failures;
}

int test_star6e_audio(void)
{
	int failures = 0;

	failures += test_star6e_audio_apply_mute_null_safe();
	failures += test_star6e_audio_apply_mute_requires_active_channel();
	failures += test_star6e_audio_apply_mute_ok();
	failures += test_star6e_audio_apply_mute_fail();
	return failures;
}
