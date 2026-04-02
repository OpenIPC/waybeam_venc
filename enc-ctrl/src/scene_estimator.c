#include "scene_estimator.h"

#include <string.h>

/*
 * EMA alpha = 1/32 in fixed-point (shift 5).
 * new_ema = ema + (sample - ema) / 32
 * This gives ~30-frame effective window at 60fps.
 */
#define EMA_SHIFT 5

/* Minimum frames before EMA is considered stable */
#define EMA_WARMUP_FRAMES 8

void scene_est_init(SceneEstimatorState *state, uint16_t threshold,
	uint8_t holdoff, uint32_t target_frame_size)
{
	if (!state)
		return;

	memset(state, 0, sizeof(*state));
	state->threshold = threshold > 0 ? threshold : GOP_CONFIG_DEFAULT_SC_THRESHOLD;
	state->holdoff = holdoff > 0 ? holdoff : GOP_CONFIG_DEFAULT_SC_HOLDOFF;
	state->target_frame_size = target_frame_size;
}

void scene_est_update(SceneEstimatorState *state,
	const EncoderFrameStats *stats)
{
	uint32_t size;
	uint32_t size_fp8;
	uint32_t qp_fp8;
	uint32_t ema_size;
	uint32_t ratio_x100;
	uint32_t budget_ratio;
	int spike;

	if (!state || !stats)
		return;

	size = stats->frame_size_bytes;

	/* Clamp before shifting to prevent uint32_t overflow.
	 * 16 MB << 8 = 4 GB which wraps.  Cap at ~16 MB.
	 * Also prevents size * 1000 overflow in budget_ratio. */
	if (size > (UINT32_MAX / 1000))
		size = (UINT32_MAX / 1000);  /* prevent overflow in * 1000 and << 8 */

	size_fp8 = size << 8;
	qp_fp8 = (uint32_t)stats->qp_avg << 8;

	state->frame_count++;

	/* Seed EMA on first frame */
	if (state->frame_count == 1) {
		state->ema_p_size_fp8 = size_fp8;
		state->ema_qp_fp8 = qp_fp8;
		state->latest.complexity = 128;
		state->latest.scene_change = 0;
		state->latest.budget_ratio = 1000;
		return;
	}

	/* Update EMA: ema += (sample - ema) >> EMA_SHIFT */
	if (size_fp8 > state->ema_p_size_fp8) {
		state->ema_p_size_fp8 += (size_fp8 - state->ema_p_size_fp8)
			>> EMA_SHIFT;
	} else {
		state->ema_p_size_fp8 -= (state->ema_p_size_fp8 - size_fp8)
			>> EMA_SHIFT;
	}

	if (qp_fp8 > state->ema_qp_fp8) {
		state->ema_qp_fp8 += (qp_fp8 - state->ema_qp_fp8)
			>> EMA_SHIFT;
	} else {
		state->ema_qp_fp8 -= (state->ema_qp_fp8 - qp_fp8)
			>> EMA_SHIFT;
	}

	/* Compute size ratio vs EMA (x100) */
	ema_size = state->ema_p_size_fp8 >> 8;
	if (ema_size > 0)
		ratio_x100 = (size * 100) / ema_size;
	else
		ratio_x100 = 100;

	/* Budget ratio: actual / target * 1000 */
	if (state->target_frame_size > 0)
		budget_ratio = (size * 1000) / state->target_frame_size;
	else
		budget_ratio = 1000;

	if (budget_ratio > 65535)
		budget_ratio = 65535;

	/* Complexity: map ratio to 0-255 range.
	 * ratio_x100 = 100 means average → complexity ~128.
	 * ratio_x100 = 50 means easy → complexity ~64.
	 * ratio_x100 = 300 means hard → complexity ~255. */
	{
		uint32_t cplx;
		if (ratio_x100 <= 50)
			cplx = (ratio_x100 * 128) / 100;
		else if (ratio_x100 >= 300)
			cplx = 255;
		else
			cplx = 64 + ((ratio_x100 - 50) * 191) / 250;

		state->latest.complexity = (uint8_t)cplx;
	}

	state->latest.budget_ratio = (uint16_t)budget_ratio;

	/* Scene change detection — only after EMA warm-up */
	spike = 0;
	if (state->frame_count > EMA_WARMUP_FRAMES) {
		/* Primary: frame size spike above threshold */
		if (ratio_x100 >= state->threshold)
			spike = 1;
	}

	if (spike) {
		state->consecutive_spikes++;
		if (state->consecutive_spikes >= state->holdoff &&
		    !state->scene_change_fired) {
			state->latest.scene_change = 1;
			state->scene_change_fired = 1;
		}
	} else {
		state->consecutive_spikes = 0;
		state->scene_change_fired = 0;
		state->latest.scene_change = 0;
	}
}

void scene_est_get(const SceneEstimatorState *state, SceneEstimate *out)
{
	if (!state || !out)
		return;

	*out = state->latest;
}

void scene_est_clear_change(SceneEstimatorState *state)
{
	if (!state)
		return;

	state->latest.scene_change = 0;
	state->scene_change_fired = 0;
	state->consecutive_spikes = 0;
}

void scene_est_set_target(SceneEstimatorState *state,
	uint32_t target_frame_size)
{
	if (!state)
		return;

	state->target_frame_size = target_frame_size;
}
