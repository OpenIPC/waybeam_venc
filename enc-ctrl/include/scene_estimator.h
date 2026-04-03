#ifndef SCENE_ESTIMATOR_H
#define SCENE_ESTIMATOR_H

/*
 * Scene complexity estimator — derives complexity and scene-change
 * signals from encoder feedback without any additional computation.
 *
 * Primary signal: intra_ratio (intra CUs / total CUs) — works under CBR
 * where frame sizes are constant but intra CU count spikes on scene changes.
 * Secondary signal: frame_size_bytes spike vs. EMA (useful under VBR/AVBR).
 * Hysteresis: consecutive frames above threshold before triggering.
 */

#include "enc_types.h"

#include <stdint.h>

typedef struct {
	/* EMA of P-frame sizes (fixed-point: value << 8) */
	uint32_t ema_p_size_fp8;

	/* Scene change detection state */
	uint8_t  consecutive_spikes;
	uint8_t  scene_change_fired;

	/* Configuration (copied from GopConfig at init) */
	uint16_t threshold;     /* intra_ratio permille (e.g. 150 = 15% intra CUs) */
	uint8_t  holdoff;       /* consecutive frames required */

	/* Target frame size for budget ratio (set from bitrate / fps) */
	uint32_t target_frame_size;

	/* Frame count since init for EMA warm-up */
	uint32_t frame_count;

	/* Latest estimate */
	SceneEstimate latest;
} SceneEstimatorState;

/** Initialize scene estimator. target_frame_size = bitrate_bps / fps / 8. */
void scene_est_init(SceneEstimatorState *state, uint16_t threshold,
	uint8_t holdoff, uint32_t target_frame_size);

/** Process one frame's stats. Updates internal state and latest estimate. */
void scene_est_update(SceneEstimatorState *state,
	const EncoderFrameStats *stats);

/** Get the latest scene estimate (non-blocking). */
void scene_est_get(const SceneEstimatorState *state, SceneEstimate *out);

/** Reset the scene change flag after it has been consumed. */
void scene_est_clear_change(SceneEstimatorState *state);

/** Update target frame size (e.g. after bitrate change). */
void scene_est_set_target(SceneEstimatorState *state,
	uint32_t target_frame_size);

#endif /* SCENE_ESTIMATOR_H */
