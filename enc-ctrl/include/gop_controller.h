#ifndef GOP_CONTROLLER_H
#define GOP_CONTROLLER_H

/*
 * Variable GOP state machine — decides when to insert IDR frames
 * based on scene content and external hints.
 *
 * States: NORMAL → IDR_READY → NORMAL (or IDR_DEFERRED → IDR_READY)
 *         NORMAL → FORCED_IDR → NORMAL (at max_gop_length)
 */

#include "enc_types.h"

#include <stdint.h>

typedef struct {
	GopConfig config;
	GopState  state;

	uint16_t  frames_since_idr;     /* monotonic counter within current GOP */
	uint16_t  defer_frames;         /* frames spent in IDR_DEFERRED state */
	uint8_t   idr_requested;        /* external IDR request pending */
	uint8_t   idr_qp_boost;        /* QP boost for next IDR (from request) */
	uint8_t   defer_active;         /* 1 if defer was explicitly requested */
	uint8_t   _pad0;

	/* Stats */
	uint32_t  total_idrs;
	uint32_t  scene_change_idrs;
	uint32_t  forced_idrs;
	uint32_t  deferred_idrs;
} GopControllerState;

/** Initialize GOP controller with config. */
void gop_ctrl_init(GopControllerState *gc, const GopConfig *config);

/** Called per-frame: evaluate state machine, return 1 if IDR should
 *  be inserted on this frame, 0 otherwise. qp_boost_out receives
 *  the QP boost to apply (0 if no boost). */
int gop_ctrl_on_frame(GopControllerState *gc, const SceneEstimate *scene,
	uint8_t *qp_boost_out);

/** Notify that an IDR was successfully encoded (resets counters). */
void gop_ctrl_idr_encoded(GopControllerState *gc);

/** External IDR request (from API caller). */
void gop_ctrl_request_idr(GopControllerState *gc, uint8_t qp_boost);

/** Defer a pending IDR (future: link controller). */
void gop_ctrl_defer_idr(GopControllerState *gc);

/** Clear IDR deferral. */
void gop_ctrl_clear_defer(GopControllerState *gc);

/** Get current state and frames-since-IDR. */
void gop_ctrl_get_state(const GopControllerState *gc, GopState *state,
	uint16_t *frames_since_idr);

/** Update config at runtime (e.g. threshold tuning). */
void gop_ctrl_set_config(GopControllerState *gc, const GopConfig *config);

#endif /* GOP_CONTROLLER_H */
