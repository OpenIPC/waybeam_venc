/*
 * Unit tests for sensor_select.c
 *
 * Provides compile-time stubs for MI_SNR_* functions so sensor_select()
 * can run on a host x86_64 build without real hardware.
 */

#include <stdio.h>
#include <string.h>

#include "sensor_select.h"
#include "test_helpers.h"

/* ── SNR API stubs ───────────────────────────────────────────────────── */

/* Simulated sensor modes per pad.  Only pad 0 has modes by default. */
#define STUB_MAX_MODES 4
static MI_SNR_Res_t g_stub_modes[4][STUB_MAX_MODES]; /* [pad][mode] */
static MI_U32 g_stub_mode_count[4];                   /* per-pad */
static MI_U32 g_stub_set_fps_value;
static int g_stub_set_fps_fail;  /* if >0, first N calls fail */
static int g_stub_enable_fail;
static MI_SNR_PadInfo_t g_stub_pad_info;
static MI_SNR_PlaneInfo_t g_stub_plane_info;

/* Track strategy hook calls */
static int g_hook_pre_set_mode_count;
static int g_hook_fps_retry_count;
static int g_hook_post_enable_count;

static void stub_reset(void)
{
	memset(g_stub_modes, 0, sizeof(g_stub_modes));
	memset(g_stub_mode_count, 0, sizeof(g_stub_mode_count));
	g_stub_set_fps_value = 0;
	g_stub_set_fps_fail = 0;
	g_stub_enable_fail = 0;
	memset(&g_stub_pad_info, 0, sizeof(g_stub_pad_info));
	memset(&g_stub_plane_info, 0, sizeof(g_stub_plane_info));
	g_hook_pre_set_mode_count = 0;
	g_hook_fps_retry_count = 0;
	g_hook_post_enable_count = 0;
}

static void stub_add_mode(int pad, uint32_t w, uint32_t h,
	uint32_t min_fps, uint32_t max_fps, const char *desc)
{
	MI_U32 idx = g_stub_mode_count[pad];
	if (idx >= STUB_MAX_MODES) return;
	g_stub_modes[pad][idx].crop.width = w;
	g_stub_modes[pad][idx].crop.height = h;
	g_stub_modes[pad][idx].output.width = w;
	g_stub_modes[pad][idx].output.height = h;
	g_stub_modes[pad][idx].minFps = min_fps;
	g_stub_modes[pad][idx].maxFps = max_fps;
	if (desc) {
		size_t len = strlen(desc);
		if (len > 31) len = 31;
		memcpy(g_stub_modes[pad][idx].desc, desc, len);
		g_stub_modes[pad][idx].desc[len] = '\0';
	}
	g_stub_mode_count[pad]++;
}

/* MI_SNR_* stub implementations */

MI_S32 MI_SNR_QueryResCount(MI_SNR_PAD_ID_e pad_id, MI_U32 *count)
{
	if ((int)pad_id < 0 || (int)pad_id > 3 || !count) return -1;
	if (g_stub_mode_count[pad_id] == 0) return -1;
	*count = g_stub_mode_count[pad_id];
	return 0;
}

MI_S32 MI_SNR_GetRes(MI_SNR_PAD_ID_e pad_id, MI_U32 res_idx, MI_SNR_Res_t *res)
{
	if ((int)pad_id < 0 || (int)pad_id > 3 || !res) return -1;
	if (res_idx >= g_stub_mode_count[pad_id]) return -1;
	*res = g_stub_modes[pad_id][res_idx];
	return 0;
}

MI_S32 MI_SNR_Disable(MI_SNR_PAD_ID_e pad_id)
{
	(void)pad_id;
	return 0;
}

MI_S32 MI_SNR_SetPlaneMode(MI_SNR_PAD_ID_e pad_id, MI_SNR_PlaneMode_e mode)
{
	(void)pad_id; (void)mode;
	return 0;
}

MI_S32 MI_SNR_SetRes(MI_SNR_PAD_ID_e pad_id, MI_U32 res_idx)
{
	(void)pad_id; (void)res_idx;
	return 0;
}

MI_S32 MI_SNR_SetFps(MI_SNR_PAD_ID_e pad_id, MI_U32 fps)
{
	(void)pad_id;
	if (g_stub_set_fps_fail > 0) {
		g_stub_set_fps_fail--;
		return -1;
	}
	g_stub_set_fps_value = fps;
	return 0;
}

MI_S32 MI_SNR_Enable(MI_SNR_PAD_ID_e pad_id)
{
	(void)pad_id;
	return g_stub_enable_fail ? -1 : 0;
}

MI_S32 MI_SNR_GetPadInfo(MI_SNR_PAD_ID_e pad_id, MI_SNR_PadInfo_t *info)
{
	(void)pad_id;
	if (!info) return -1;
	*info = g_stub_pad_info;
	return 0;
}

MI_S32 MI_SNR_GetPlaneInfo(MI_SNR_PAD_ID_e pad_id, MI_U32 plane_idx,
	MI_SNR_PlaneInfo_t *info)
{
	(void)pad_id; (void)plane_idx;
	if (!info) return -1;
	*info = g_stub_plane_info;
	return 0;
}

MI_S32 MI_SNR_CustFunction(MI_SNR_PAD_ID_e pad_id, MI_U32 cmd_id,
	MI_U32 data_size, void *data, MI_SNR_CustDir_e dir)
{
	(void)pad_id; (void)cmd_id; (void)data_size; (void)data; (void)dir;
	return 0;
}

/* ── Tracking strategy hooks ─────────────────────────────────────────── */

static int tracking_pre_set_mode(MI_SNR_PAD_ID_e pad, int mode_index, void *ctx)
{
	(void)pad; (void)mode_index; (void)ctx;
	g_hook_pre_set_mode_count++;
	return 0;
}

static int tracking_on_fps_retry(MI_SNR_PAD_ID_e pad, int mode_index, void *ctx)
{
	(void)pad; (void)mode_index; (void)ctx;
	g_hook_fps_retry_count++;
	return 0;
}

static int tracking_post_enable(MI_SNR_PAD_ID_e pad,
	const SensorSelectResult *result, void *ctx)
{
	(void)pad; (void)result; (void)ctx;
	g_hook_post_enable_count++;
	return 0;
}

static SensorStrategy tracking_strategy(void)
{
	SensorStrategy s = {0};
	s.name = "tracking";
	s.pre_set_mode = tracking_pre_set_mode;
	s.on_fps_retry = tracking_on_fps_retry;
	s.post_enable = tracking_post_enable;
	return s;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

static int test_default_strategy_fields(void)
{
	int failures = 0;
	SensorStrategy s = sensor_default_strategy();
	CHECK("default_name", strcmp(s.name, "default") == 0);
	CHECK("default_pre_null", s.pre_set_mode == NULL);
	CHECK("default_retry_null", s.on_fps_retry == NULL);
	CHECK("default_post_null", s.post_enable == NULL);
	CHECK("default_ctx_null", s.ctx == NULL);
	return failures;
}

static int test_unlock_strategy_fields(void)
{
	int failures = 0;
	SensorUnlockConfig cfg = {
		.enabled = 1, .cmd_id = 0x23,
		.reg = 0x300a, .value = 0x80,
		.dir = E_MI_SNR_CUSTDATA_TO_DRIVER,
	};
	SensorStrategy s = sensor_unlock_strategy(&cfg);
	CHECK("unlock_name", strcmp(s.name, "unlock") == 0);
	CHECK("unlock_pre_set", s.pre_set_mode != NULL);
	CHECK("unlock_retry_set", s.on_fps_retry != NULL);
	CHECK("unlock_post_null", s.post_enable == NULL);
	CHECK("unlock_ctx_set", s.ctx == &cfg);
	return failures;
}

static int test_fps_supported(void)
{
	int failures = 0;
	MI_SNR_Res_t mode = {0};
	mode.minFps = 15;
	mode.maxFps = 90;

	CHECK("fps_in_range", sensor_mode_fps_supported(&mode, 30) == 1);
	CHECK("fps_at_min", sensor_mode_fps_supported(&mode, 15) == 1);
	CHECK("fps_at_max", sensor_mode_fps_supported(&mode, 90) == 1);
	CHECK("fps_below_min", sensor_mode_fps_supported(&mode, 10) == 0);
	CHECK("fps_above_max", sensor_mode_fps_supported(&mode, 120) == 0);
	CHECK("fps_zero", sensor_mode_fps_supported(&mode, 0) == 1);
	return failures;
}

static int test_fps_clamping(void)
{
	int failures = 0;
	MI_SNR_Res_t mode = {0};
	mode.minFps = 15;
	mode.maxFps = 90;

	CHECK("clamp_in_range", sensor_mode_clamp_fps(&mode, 30) == 30);
	CHECK("clamp_below", sensor_mode_clamp_fps(&mode, 5) == 15);
	CHECK("clamp_above", sensor_mode_clamp_fps(&mode, 120) == 90);
	CHECK("clamp_zero", sensor_mode_clamp_fps(&mode, 0) == 30);
	return failures;
}

static int test_mode_scoring(void)
{
	int failures = 0;
	MI_SNR_Res_t mode = {0};
	mode.crop.width = 1920;
	mode.crop.height = 1080;
	mode.minFps = 1;
	mode.maxFps = 60;

	/* fps ok + fit → 3 */
	CHECK("score_3", sensor_mode_score(&mode, 1920, 1080, 30) == 3);
	/* fps ok + no fit (requesting larger) → 2 */
	CHECK("score_2", sensor_mode_score(&mode, 2560, 1440, 30) == 2);
	/* no fps + fit → 1 */
	CHECK("score_1", sensor_mode_score(&mode, 1280, 720, 120) == 1);
	/* neither → 0 */
	CHECK("score_0", sensor_mode_score(&mode, 2560, 1440, 120) == 0);
	return failures;
}

static int test_mode_cost(void)
{
	int failures = 0;
	MI_SNR_Res_t exact = {0};
	exact.crop.width = 1920;
	exact.crop.height = 1080;

	MI_SNR_Res_t larger = {0};
	larger.crop.width = 2560;
	larger.crop.height = 1440;

	CHECK("cost_exact", sensor_mode_cost(&exact, 1920, 1080, 0) == 0);
	CHECK("cost_larger", sensor_mode_cost(&larger, 1920, 1080, 0) > 0);
	/* Exact should be cheaper than larger */
	CHECK("cost_exact_cheaper",
		sensor_mode_cost(&exact, 1920, 1080, 0) < sensor_mode_cost(&larger, 1920, 1080, 0));
	return failures;
}

static int test_select_best_mode(void)
{
	int failures = 0;
	stub_reset();

	/* Pad 0: two modes — 720p30 and 1080p60 */
	stub_add_mode(0, 1280, 720, 1, 30, "720p30");
	stub_add_mode(0, 1920, 1080, 1, 60, "1080p60");

	SensorSelectConfig cfg = {
		.forced_pad = -1, .forced_mode = -1,
		.target_width = 1920, .target_height = 1080,
		.target_fps = 30,
	};
	SensorStrategy strategy = sensor_default_strategy();
	SensorSelectResult result;

	int ret = sensor_select(&cfg, &strategy, &result);
	CHECK("select_ok", ret == 0);
	CHECK("select_pad", (int)result.pad_id == 0);
	CHECK("select_mode_1080p", result.mode.crop.width == 1920);
	CHECK("select_mode_index", result.mode_index == 1);
	/* Sensor always runs at mode maxFps; downstream bind handles decimation */
	CHECK("select_fps", result.fps == 60);
	return failures;
}

static int test_select_forced_pad(void)
{
	int failures = 0;
	stub_reset();

	/* Pad 0: 1080p, Pad 1: 720p */
	stub_add_mode(0, 1920, 1080, 1, 60, "1080p60");
	stub_add_mode(1, 1280, 720, 1, 30, "720p30");

	SensorSelectConfig cfg = {
		.forced_pad = 1, .forced_mode = -1,
		.target_width = 1920, .target_height = 1080,
		.target_fps = 30,
	};
	SensorStrategy strategy = sensor_default_strategy();
	SensorSelectResult result;

	int ret = sensor_select(&cfg, &strategy, &result);
	CHECK("forced_pad_ok", ret == 0);
	CHECK("forced_pad_selected", (int)result.pad_id == 1);
	CHECK("forced_pad_720p", result.mode.crop.width == 1280);
	return failures;
}

static int test_select_forced_mode(void)
{
	int failures = 0;
	stub_reset();

	stub_add_mode(0, 1280, 720, 1, 30, "720p30");
	stub_add_mode(0, 1920, 1080, 1, 60, "1080p60");
	stub_add_mode(0, 2560, 1440, 1, 30, "2K30");

	SensorSelectConfig cfg = {
		.forced_pad = -1, .forced_mode = 2,
		.target_width = 1920, .target_height = 1080,
		.target_fps = 30,
	};
	SensorStrategy strategy = sensor_default_strategy();
	SensorSelectResult result;

	int ret = sensor_select(&cfg, &strategy, &result);
	CHECK("forced_mode_ok", ret == 0);
	CHECK("forced_mode_2K", result.mode.crop.width == 2560);
	CHECK("forced_mode_index", result.mode_index == 2);
	return failures;
}

static int test_no_modes_available(void)
{
	int failures = 0;
	stub_reset();
	/* No modes on any pad */

	SensorSelectConfig cfg = {
		.forced_pad = -1, .forced_mode = -1,
		.target_width = 1920, .target_height = 1080,
		.target_fps = 30,
	};
	SensorStrategy strategy = sensor_default_strategy();
	SensorSelectResult result;

	int ret = sensor_select(&cfg, &strategy, &result);
	CHECK("no_modes_fail", ret != 0);
	return failures;
}

static int test_strategy_hooks_called(void)
{
	int failures = 0;
	stub_reset();
	stub_add_mode(0, 1920, 1080, 1, 60, "1080p60");

	SensorSelectConfig cfg = {
		.forced_pad = -1, .forced_mode = -1,
		.target_width = 1920, .target_height = 1080,
		.target_fps = 30,
	};
	SensorStrategy strategy = tracking_strategy();
	SensorSelectResult result;

	int ret = sensor_select(&cfg, &strategy, &result);
	CHECK("hooks_ok", ret == 0);
	CHECK("pre_set_mode_called", g_hook_pre_set_mode_count == 1);
	CHECK("post_enable_called", g_hook_post_enable_count == 1);
	/* fps_retry not called because fps succeeds on first try */
	CHECK("fps_retry_not_called", g_hook_fps_retry_count == 0);
	return failures;
}

static int test_fps_retry_calls_hook(void)
{
	int failures = 0;
	stub_reset();
	stub_add_mode(0, 1920, 1080, 1, 60, "1080p60");

	/* Make first SetFps call fail, second succeeds */
	g_stub_set_fps_fail = 1;

	SensorSelectConfig cfg = {
		.forced_pad = -1, .forced_mode = -1,
		.target_width = 1920, .target_height = 1080,
		.target_fps = 30,
	};
	SensorStrategy strategy = tracking_strategy();
	SensorSelectResult result;

	int ret = sensor_select(&cfg, &strategy, &result);
	CHECK("retry_ok", ret == 0);
	CHECK("retry_hook_called", g_hook_fps_retry_count == 1);
	/* Sensor always runs at mode maxFps; downstream bind handles decimation */
	CHECK("retry_fps_set", result.fps == 60);
	return failures;
}

static int test_list_modes(void)
{
	int failures = 0;
	stub_reset();
	stub_add_mode(0, 1920, 1080, 1, 60, "1080p60");
	stub_add_mode(0, 1280, 720, 1, 30, "720p30");

	/* Just verify it doesn't crash */
	sensor_list_modes(-1, -1, -1);
	sensor_list_modes(0, 0, 0);
	sensor_list_modes(2, -1, -1); /* empty pad */
	CHECK("list_modes_ok", 1);
	return failures;
}

static int test_euclidean_tiebreak(void)
{
	int failures = 0;
	stub_reset();

	/* Two modes both score class 3 — pick closest to target */
	stub_add_mode(0, 2560, 1440, 1, 60, "2K60");
	stub_add_mode(0, 1920, 1080, 1, 60, "1080p60");

	SensorSelectConfig cfg = {
		.forced_pad = -1, .forced_mode = -1,
		.target_width = 1920, .target_height = 1080,
		.target_fps = 30,
	};
	SensorStrategy strategy = sensor_default_strategy();
	SensorSelectResult result;

	int ret = sensor_select(&cfg, &strategy, &result);
	CHECK("tiebreak_ok", ret == 0);
	CHECK("tiebreak_1080p", result.mode.crop.width == 1920);
	return failures;
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int test_sensor_select(void)
{
	int failures = 0;
	failures += test_default_strategy_fields();
	failures += test_unlock_strategy_fields();
	failures += test_fps_supported();
	failures += test_fps_clamping();
	failures += test_mode_scoring();
	failures += test_mode_cost();
	failures += test_select_best_mode();
	failures += test_select_forced_pad();
	failures += test_select_forced_mode();
	failures += test_no_modes_available();
	failures += test_strategy_hooks_called();
	failures += test_fps_retry_calls_hook();
	failures += test_list_modes();
	failures += test_euclidean_tiebreak();
	return failures;
}
