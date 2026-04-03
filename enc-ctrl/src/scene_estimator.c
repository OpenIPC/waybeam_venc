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
	uint32_t ema_size;
	uint32_t budget_ratio;
	uint16_t intra_ratio;
	int spike;

	if (!state || !stats)
		return;

	size = stats->frame_size_bytes;
	intra_ratio = stats->intra_ratio;

	/* Clamp frame size to prevent overflow in * 1000 and << 8 */
	if (size > (UINT32_MAX / 1000))
		size = (UINT32_MAX / 1000);

	size_fp8 = size << 8;

	state->frame_count++;

	/* Seed EMA on first frame */
	if (state->frame_count == 1) {
		state->ema_p_size_fp8 = size_fp8;
		state->latest.complexity = 0;
		state->latest.scene_change = 0;
		state->latest.budget_ratio = 1000;
		return;
	}

	/* Update frame size EMA */
	if (size_fp8 > state->ema_p_size_fp8) {
		state->ema_p_size_fp8 += (size_fp8 - state->ema_p_size_fp8)
			>> EMA_SHIFT;
	} else {
		state->ema_p_size_fp8 -= (state->ema_p_size_fp8 - size_fp8)
			>> EMA_SHIFT;
	}

	/* Budget ratio: actual / target * 1000 */
	if (state->target_frame_size > 0)
		budget_ratio = (size * 1000) / state->target_frame_size;
	else
		budget_ratio = 1000;

	if (budget_ratio > 65535)
		budget_ratio = 65535;

	/* Complexity: map intra_ratio (0-1000) to 0-255.
	 * Stable P-frame: intra_ratio ~0-20 → complexity ~0-5
	 * Moderate motion: intra_ratio ~50-100 → complexity ~13-25
	 * Scene change: intra_ratio ~200+ → complexity ~50+ */
	{
		uint32_t cplx = (intra_ratio * 255) / 1000;
		if (cplx > 255)
			cplx = 255;
		state->latest.complexity = (uint8_t)cplx;
	}

	state->latest.budget_ratio = (uint16_t)budget_ratio;

	/* Scene change detection: intra_ratio spike above threshold.
	 * Under CBR, frame sizes stay constant but intra CU count spikes
	 * when inter prediction fails due to scene discontinuity.
	 * Only active after EMA warm-up to avoid false triggers at startup. */
	spike = 0;
	if (state->frame_count > EMA_WARMUP_FRAMES) {
		if (intra_ratio >= state->threshold)
			spike = 1;

		/* Secondary: frame size spike for VBR/AVBR modes */
		if (!spike) {
			ema_size = state->ema_p_size_fp8 >> 8;
			if (ema_size > 0 && (size * 100) / ema_size >= 300)
				spike = 1;
		}
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
