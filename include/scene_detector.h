#ifndef SCENE_DETECTOR_H
#define SCENE_DETECTOR_H

#include "rtp_sidecar.h"

#include <stdint.h>

#define SCENE_FRAME_P   0
#define SCENE_FRAME_IDR 2

/*
 * Lightweight scene-change IDR detector shared by star6e and maruko backends.
 *
 * Runs an EMA over frame size.  Frames whose size exceeds threshold%% of the
 * EMA for `holdoff` consecutive frames arm a pending spike; once the next
 * frame size drops back below 120%% the caller-supplied request_idr callback
 * fires.  A cooldown after every IDR suppresses repeat triggers.
 *
 * The detector is stream-type agnostic: callers extract frame_size_bytes and
 * the is_idr flag from their platform's pack array and pass primitives in.
 */

typedef struct {
	uint32_t ema_size_fp8;
	uint32_t frame_count;
	uint32_t last_frame_size;
	uint8_t  last_frame_type;
	uint16_t cooldown;
	uint16_t settle_count;
	uint8_t  spike_pending;
	uint8_t  idr_inserted;
	uint8_t  complexity;
	uint8_t  scene_change;
	uint16_t frames_since_idr;
	uint16_t threshold;
	uint8_t  holdoff;
	uint8_t  consecutive_spikes;
	uint8_t  idr_enabled;
	uint8_t  warmup_done;
} SceneDetector;

typedef void (*SceneRequestIdrFn)(void *ctx);

/* threshold=0 disables IDR insertion (telemetry still populated).
 * holdoff=0 defaults to 2. threshold>0 defaults to 150 when caller passes 0. */
void scene_init(SceneDetector *sd, uint16_t threshold, uint8_t holdoff);

/* Call per frame after decoding frame_size and is_idr from the stream. */
void scene_update(SceneDetector *sd, uint32_t frame_size, uint8_t is_idr,
	SceneRequestIdrFn request_idr, void *idr_ctx);

/* Populate RTP sidecar encoder-feedback fields from last scene_update. */
void scene_fill_sidecar(const SceneDetector *sd, RtpSidecarEncInfo *out);

#endif /* SCENE_DETECTOR_H */
