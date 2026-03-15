#include "sensor_select.h"
#include "sdk_quiet.h"

#include <stdio.h>
#include <string.h>

static SdkQuietState g_sensor_sdk_quiet = SDK_QUIET_STATE_INIT;

/* ── Scoring helpers ─────────────────────────────────────────────────── */

int sensor_mode_fps_supported(const MI_SNR_Res_t *mode, uint32_t fps)
{
	if (!mode || fps == 0)
		return 1;
	if (mode->maxFps && fps > mode->maxFps)
		return 0;
	if (mode->minFps && fps < mode->minFps)
		return 0;
	return 1;
}

uint32_t sensor_mode_clamp_fps(const MI_SNR_Res_t *mode, uint32_t fps)
{
	uint32_t out = fps ? fps : 30;
	if (mode->maxFps && out > mode->maxFps)
		out = mode->maxFps;
	if (mode->minFps && out < mode->minFps)
		out = mode->minFps;
	return out;
}

int sensor_mode_score(const MI_SNR_Res_t *mode,
	uint32_t target_w, uint32_t target_h, uint32_t target_fps)
{
	int fit = (mode->crop.width >= target_w && mode->crop.height >= target_h);
	int fps_ok = sensor_mode_fps_supported(mode, target_fps);
	if (fps_ok && fit) return 3;
	if (fps_ok) return 2;
	if (fit) return 1;
	return 0;
}

uint64_t sensor_mode_cost(const MI_SNR_Res_t *mode,
	uint32_t target_w, uint32_t target_h, uint32_t target_fps)
{
	uint64_t dw = (mode->crop.width > target_w)
		? (uint64_t)(mode->crop.width - target_w)
		: (uint64_t)(target_w - mode->crop.width);
	uint64_t dh = (mode->crop.height > target_h)
		? (uint64_t)(mode->crop.height - target_h)
		: (uint64_t)(target_h - mode->crop.height);
	uint64_t res_cost = dw * dw + dh * dh;
	/* Primary: prefer modes whose maxFps is closest to target_fps.
	 * A 90fps mode should beat a 120fps mode when 90fps is requested,
	 * even if the 120fps mode has a better resolution match.
	 * FPS excess in the high bits, resolution cost in the low bits. */
	uint64_t fps_excess = 0;
	if (target_fps > 0 && mode->maxFps > target_fps)
		fps_excess = (uint64_t)(mode->maxFps - target_fps);
	return (fps_excess << 32) | (res_cost & 0xFFFFFFFFULL);
}

/* ── Unlock strategy implementation ──────────────────────────────────── */

typedef struct {
	MI_U16 reg;
	MI_U16 data;
} SensorUnlockPayload;

static int unlock_pre_set_mode(MI_SNR_PAD_ID_e pad, int mode_index, void *ctx)
{
	(void)mode_index;
	SensorUnlockConfig *cfg = (SensorUnlockConfig *)ctx;
	if (!cfg || !cfg->enabled)
		return 0;

	SensorUnlockPayload payload = {
		.reg = cfg->reg,
		.data = cfg->value,
	};

	MI_S32 ret = MI_SNR_CustFunction(pad, cfg->cmd_id,
		(MI_U32)sizeof(payload), &payload, cfg->dir);
	if (ret != 0) {
		fprintf(stderr, "WARNING: sensor unlock failed at pre-setres "
			"(pad %d, cmd=0x%08x, reg=0x%04x, value=0x%04x, dir=%d) -> %d\n",
			pad, (unsigned)cfg->cmd_id, cfg->reg, cfg->value,
			cfg->dir, ret);
		return ret;
	}

	printf("> Sensor unlock applied at pre-setres "
		"(pad %d, cmd=0x%08x, reg=0x%04x, value=0x%04x)\n",
		pad, (unsigned)cfg->cmd_id, cfg->reg, cfg->value);
	return 0;
}

static int unlock_on_fps_retry(MI_SNR_PAD_ID_e pad, int mode_index, void *ctx)
{
	(void)mode_index;
	SensorUnlockConfig *cfg = (SensorUnlockConfig *)ctx;
	if (!cfg || !cfg->enabled)
		return 0;

	SensorUnlockPayload payload = {
		.reg = cfg->reg,
		.data = cfg->value,
	};

	MI_S32 ret = MI_SNR_CustFunction(pad, cfg->cmd_id,
		(MI_U32)sizeof(payload), &payload, cfg->dir);
	if (ret != 0) {
		fprintf(stderr, "WARNING: sensor unlock failed at fps-retry "
			"(pad %d, cmd=0x%08x, reg=0x%04x, value=0x%04x) -> %d\n",
			pad, (unsigned)cfg->cmd_id, cfg->reg, cfg->value, ret);
	} else {
		printf("> Sensor unlock applied at fps-retry "
			"(pad %d, cmd=0x%08x, reg=0x%04x, value=0x%04x)\n",
			pad, (unsigned)cfg->cmd_id, cfg->reg, cfg->value);
	}
	return 0; /* non-fatal during retry */
}

/* ── Strategy constructors ───────────────────────────────────────────── */

SensorStrategy sensor_default_strategy(void)
{
	SensorStrategy s = {0};
	s.name = "default";
	return s;
}

SensorStrategy sensor_unlock_strategy(SensorUnlockConfig *unlock_cfg)
{
	SensorStrategy s = {0};
	s.name = "unlock";
	s.pre_set_mode = unlock_pre_set_mode;
	s.on_fps_retry = unlock_on_fps_retry;
	s.ctx = unlock_cfg;
	return s;
}

/* ── FPS retry with full recovery ────────────────────────────────────── */

static MI_S32 sensor_try_set_fps(MI_SNR_PAD_ID_e pad, MI_U32 mode_index,
	MI_U32 fps, const SensorStrategy *strategy)
{
	MI_S32 ret = MI_SNR_SetFps(pad, fps);
	if (ret == 0)
		return 0;

	printf("> MI_SNR_SetFps(pad %d, value %u) failed %d\n", pad, fps, ret);

	/* Full recovery cycle: disable → plane → strategy hook → setres → retry */
	(void)MI_SNR_Disable(pad);
	(void)MI_SNR_SetPlaneMode(pad, E_MI_SNR_PLANE_MODE_LINEAR);
	if (strategy && strategy->on_fps_retry)
		(void)strategy->on_fps_retry(pad, (int)mode_index, strategy->ctx);
	(void)MI_SNR_SetRes(pad, mode_index);

	ret = MI_SNR_SetFps(pad, fps);
	if (ret != 0)
		printf("> MI_SNR_SetFps(pad %d, value %u) retry failed %d\n",
			pad, fps, ret);
	return ret;
}

/* ── Mode listing ────────────────────────────────────────────────────── */

void sensor_list_modes(int forced_pad, int selected_pad, int selected_mode)
{
	int pad_start = 0;
	int pad_end = 3;
	if (forced_pad >= 0 && forced_pad <= 3) {
		pad_start = forced_pad;
		pad_end = forced_pad;
	}

	for (int p = pad_start; p <= pad_end; ++p) {
		MI_U32 count = 0;
		MI_S32 ret = MI_SNR_QueryResCount((MI_SNR_PAD_ID_e)p, &count);
		if (ret != 0)
			continue;

		printf("> Pad %d: %u mode(s)\n", p, count);
		for (MI_U32 idx = 0; idx < count; ++idx) {
			MI_SNR_Res_t res = {0};
			ret = MI_SNR_GetRes((MI_SNR_PAD_ID_e)p, idx, &res);
			if (ret != 0) {
				printf("  - [%u] MI_SNR_GetRes failed %d\n", idx, ret);
				continue;
			}
			int is_selected = (p == selected_pad && (int)idx == selected_mode);
			printf("  %s [%u] %ux%u min/max fps %u/%u desc \"%s\"",
				is_selected ? "*" : "-",
				idx, res.crop.width, res.crop.height,
				res.minFps, res.maxFps, res.desc);
			if (res.output.width > 0 && res.output.height > 0 &&
				(res.output.width != res.crop.width ||
				 res.output.height != res.crop.height)) {
				printf("  crop(%u,%u %ux%u) output(%ux%u)",
					res.crop.x, res.crop.y,
					res.crop.width, res.crop.height,
					res.output.width, res.output.height);
			}
			printf("\n");
		}
	}
}

/* ── Core selection ──────────────────────────────────────────────────── */

static int find_best_mode(const SensorSelectConfig *cfg,
	MI_SNR_PAD_ID_e *best_pad, MI_S32 *best_index, MI_SNR_Res_t *best_mode)
{
	int best_class = -1;
	uint64_t best_cost = ~(uint64_t)0;

	int pad_start = 0;
	int pad_end = 3;
	if (cfg->forced_pad >= 0 && cfg->forced_pad <= 3) {
		pad_start = cfg->forced_pad;
		pad_end = cfg->forced_pad;
	}

	for (int p = pad_start; p <= pad_end; ++p) {
		MI_U32 count = 0;
		MI_S32 ret = MI_SNR_QueryResCount((MI_SNR_PAD_ID_e)p, &count);
		if (ret != 0 || count == 0)
			continue;

		for (MI_U32 idx = 0; idx < count; ++idx) {
			if (cfg->forced_mode >= 0 && (int)idx != cfg->forced_mode)
				continue;

			MI_SNR_Res_t res = {0};
			ret = MI_SNR_GetRes((MI_SNR_PAD_ID_e)p, idx, &res);
			if (ret != 0)
				continue;

			int cls = sensor_mode_score(&res,
				cfg->target_width, cfg->target_height, cfg->target_fps);
			uint64_t cost = sensor_mode_cost(&res,
				cfg->target_width, cfg->target_height, cfg->target_fps);

			if (cls > best_class || (cls == best_class && cost < best_cost)) {
				best_class = cls;
				best_cost = cost;
				*best_pad = (MI_SNR_PAD_ID_e)p;
				*best_mode = res;
				*best_index = (MI_S32)idx;
			}
		}
	}

	if (*best_index < 0) {
		if (cfg->forced_mode >= 0)
			fprintf(stderr, "ERROR: --sensor-mode %d is not available on selected pad(s)\n",
				cfg->forced_mode);
		fprintf(stderr, "ERROR: Failed to select sensor mode on any pad\n");
		return -1;
	}
	return 0;
}

static int set_sensor_fps(MI_SNR_PAD_ID_e pad, MI_U32 mode_index,
	const MI_SNR_Res_t *mode, uint32_t target_fps,
	const SensorStrategy *strategy, SensorSelectResult *result)
{
	uint32_t requested_fps = target_fps ? target_fps : 30;
	uint32_t clamped = mode->maxFps ? mode->maxFps
		: sensor_mode_clamp_fps(mode, requested_fps);
	result->fps = 0;

	MI_S32 err = sensor_try_set_fps(pad, mode_index, clamped, strategy);
	if (err == 0) {
		result->fps = clamped;
		return 0;
	}

	printf("> Requested %u fps (clamped %u) failed with %d; trying safe fallback\n",
		requested_fps, clamped, err);

	uint32_t fallback = sensor_mode_clamp_fps(mode, 30);
	if (fallback != 0 && fallback != clamped &&
		sensor_mode_fps_supported(mode, fallback)) {
		err = sensor_try_set_fps(pad, mode_index, fallback, strategy);
		if (err == 0) {
			result->fps = fallback;
			printf("> Requested %u fps, sensor accepted %u fps "
				"(mode min %u max %u)\n",
				requested_fps, fallback, mode->minFps, mode->maxFps);
			return 0;
		}
	}

	fprintf(stderr, "ERROR: MI_SNR_SetFps(pad %d) failed for requested %u fps "
		"(last err %d, mode min %u max %u)\n",
		pad, requested_fps, err, mode->minFps, mode->maxFps);
	return err ? err : -1;
}

static int configure_selected_sensor(const SensorSelectConfig *cfg,
	const SensorStrategy *strategy, MI_SNR_PAD_ID_e best_pad,
	MI_S32 best_index, const MI_SNR_Res_t *best_mode, SensorSelectResult *result)
{
	result->pad_id = best_pad;
	result->mode = *best_mode;
	result->mode_index = best_index;

	(void)MI_SNR_Disable(best_pad);

	MI_S32 ret = MI_SNR_SetPlaneMode(best_pad, E_MI_SNR_PLANE_MODE_LINEAR);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_SNR_SetPlaneMode(pad %d) failed %d\n", best_pad, ret);
		return ret;
	}

	if (strategy && strategy->pre_set_mode) {
		ret = strategy->pre_set_mode(best_pad, best_index, strategy->ctx);
		if (ret != 0) {
			fprintf(stderr, "ERROR: strategy pre_set_mode failed %d\n", ret);
			return ret;
		}
	}

	ret = MI_SNR_SetRes(best_pad, (MI_U32)best_index);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_SNR_SetRes(pad %d, index %d) failed %d\n",
			best_pad, best_index, ret);
		return ret;
	}

	ret = set_sensor_fps(best_pad, (MI_U32)best_index, best_mode,
		cfg->target_fps, strategy, result);
	if (ret != 0)
		return ret;

	sdk_quiet_begin(&g_sensor_sdk_quiet);
	ret = MI_SNR_Enable(best_pad);
	sdk_quiet_end(&g_sensor_sdk_quiet);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_SNR_Enable(pad %d) failed %d\n", best_pad, ret);
		return ret;
	}

	if (strategy && strategy->post_enable)
		(void)strategy->post_enable(best_pad, result, strategy->ctx);

	ret = MI_SNR_GetPadInfo(best_pad, &result->pad);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_SNR_GetPadInfo(pad %d) failed %d\n", best_pad, ret);
		return ret;
	}

	ret = MI_SNR_GetPlaneInfo(best_pad, 0, &result->plane);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_SNR_GetPlaneInfo(pad %d) failed %d\n", best_pad, ret);
		return ret;
	}

	sensor_list_modes(cfg->forced_pad, (int)best_pad, best_index);

	printf("> Sensor pad selected: %d (mode index %d, %ux%u, "
		"min/max fps %u/%u, actual %u)\n",
		best_pad, best_index,
		best_mode->crop.width, best_mode->crop.height,
		best_mode->minFps, best_mode->maxFps, result->fps);
	return 0;
}

int sensor_select(const SensorSelectConfig *cfg,
	const SensorStrategy *strategy, SensorSelectResult *result)
{
	if (!cfg || !result)
		return -1;

	memset(result, 0, sizeof(*result));

	MI_SNR_PAD_ID_e best_pad = E_MI_SNR_PAD_ID_0;
	MI_S32 best_index = -1;
	MI_SNR_Res_t best_mode = {0};

	int err = find_best_mode(cfg, &best_pad, &best_index, &best_mode);
	if (err != 0)
		return err;

	return configure_selected_sensor(cfg, strategy, best_pad, best_index,
		&best_mode, result);
}
