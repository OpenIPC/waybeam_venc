#include "scene_detector.h"

#include <string.h>

#define SCENE_EMA_SHIFT           4
#define SCENE_WARMUP_FRAMES       8
#define SCENE_COOLDOWN_AFTER_IDR  30

void scene_init(SceneDetector *sd, uint16_t threshold, uint8_t holdoff)
{
	memset(sd, 0, sizeof(*sd));
	sd->threshold = threshold > 0 ? threshold : 150;
	sd->holdoff = holdoff > 0 ? holdoff : 2;
	sd->idr_enabled = threshold > 0 ? 1 : 0;
}

void scene_update(SceneDetector *sd, uint32_t frame_size, uint8_t is_idr,
	SceneRequestIdrFn request_idr, void *idr_ctx)
{
	uint32_t size_fp8, ema_size, ratio_x100;

	if (!sd) return;
	if (frame_size > (UINT32_MAX / 1000)) frame_size = (UINT32_MAX / 1000);
	size_fp8 = frame_size << 8;

	sd->last_frame_size = frame_size;
	sd->last_frame_type = is_idr ? SCENE_FRAME_IDR : SCENE_FRAME_P;

	if (sd->frame_count < UINT32_MAX) sd->frame_count++;
	sd->idr_inserted = 0;
	sd->scene_change = 0;

	if (is_idr) {
		sd->frames_since_idr = 0;
		sd->spike_pending = 0;
		sd->settle_count = 0;
		sd->consecutive_spikes = 0;
		sd->cooldown = SCENE_COOLDOWN_AFTER_IDR;
	} else {
		if (sd->frames_since_idr < UINT16_MAX) sd->frames_since_idr++;
	}

	if (sd->frame_count == 1) { sd->ema_size_fp8 = size_fp8; return; }

	if (size_fp8 > sd->ema_size_fp8)
		sd->ema_size_fp8 += (size_fp8 - sd->ema_size_fp8) >> SCENE_EMA_SHIFT;
	else
		sd->ema_size_fp8 -= (sd->ema_size_fp8 - size_fp8) >> SCENE_EMA_SHIFT;

	ema_size = sd->ema_size_fp8 >> 8;
	ratio_x100 = ema_size > 0 ? (frame_size * 100) / ema_size : 100;

	{
		uint32_t c;
		if (ratio_x100 <= 50) c = (ratio_x100 * 128) / 100;
		else if (ratio_x100 >= 300) c = 255;
		else c = 64 + ((ratio_x100 - 50) * 191) / 250;
		sd->complexity = (uint8_t)c;
	}

	if (!sd->warmup_done) {
		if (sd->frame_count > SCENE_WARMUP_FRAMES)
			sd->warmup_done = 1;
		return;
	}

	if (!sd->idr_enabled) return;

	if (sd->cooldown > 0) { sd->cooldown--; return; }

	if (sd->spike_pending) {
		if (ratio_x100 <= 120) {
			sd->spike_pending = 0;
			sd->settle_count = 0;
			sd->scene_change = 1;
			sd->idr_inserted = 1;
			if (request_idr)
				request_idr(idr_ctx);
		} else {
			sd->settle_count++;
			if (sd->settle_count > 30) {
				sd->spike_pending = 0;
				sd->settle_count = 0;
			}
		}
	} else {
		if (ratio_x100 >= sd->threshold) {
			sd->consecutive_spikes++;
			if (sd->consecutive_spikes >= sd->holdoff)
				sd->spike_pending = 1;
		} else {
			sd->consecutive_spikes = 0;
		}
	}
}

void scene_fill_sidecar(const SceneDetector *sd, RtpSidecarEncInfo *out)
{
	if (!sd || !out) return;
	memset(out, 0, sizeof(*out));
	out->frame_size_bytes = sd->last_frame_size;
	out->frame_type = sd->last_frame_type;
	out->complexity = sd->complexity;
	out->scene_change = sd->scene_change;
	out->idr_inserted = sd->idr_inserted;
	out->frames_since_idr = sd->frames_since_idr;
}
