#include "gop_controller.h"

#include <string.h>

void gop_ctrl_init(GopControllerState *gc, const GopConfig *config)
{
	if (!gc)
		return;

	memset(gc, 0, sizeof(*gc));
	if (config)
		gc->config = *config;
	else
		gop_config_defaults(&gc->config);

	gc->state = GOP_STATE_NORMAL;
}

int gop_ctrl_on_frame(GopControllerState *gc, const SceneEstimate *scene,
	uint8_t *qp_boost_out)
{
	int insert_idr = 0;
	uint8_t boost = 0;

	if (!gc) {
		if (qp_boost_out)
			*qp_boost_out = 0;
		return 0;
	}

	/* Passthrough mode: just count frames, never trigger IDR */
	if (!gc->config.enable_variable_gop) {
		gc->frames_since_idr++;
		if (qp_boost_out)
			*qp_boost_out = 0;
		return 0;
	}

	gc->frames_since_idr++;

	switch (gc->state) {
	case GOP_STATE_NORMAL:
		/* Check forced IDR at max_gop_length */
		if (gc->frames_since_idr >= gc->config.max_gop_length) {
			gc->state = GOP_STATE_FORCED_IDR;
			insert_idr = 1;
			boost = gc->config.idr_qp_boost;
			gc->forced_idrs++;
			break;
		}

		/* Check external IDR request */
		if (gc->idr_requested &&
		    gc->frames_since_idr >= gc->config.min_gop_length) {
			gc->state = GOP_STATE_IDR_READY;
			insert_idr = 1;
			boost = gc->idr_qp_boost;
			gc->idr_requested = 0;
			break;
		}

		/* Check scene change (only if past min_gop_length) */
		if (scene && scene->scene_change &&
		    gc->frames_since_idr >= gc->config.min_gop_length) {
			gc->state = GOP_STATE_IDR_READY;
			insert_idr = 1;
			boost = gc->config.idr_qp_boost;
			gc->scene_change_idrs++;
			break;
		}
		break;

	case GOP_STATE_IDR_READY:
		/* Should not normally stay here — inserted previous frame */
		insert_idr = 1;
		boost = gc->config.idr_qp_boost;
		break;

	case GOP_STATE_IDR_DEFERRED:
		gc->defer_frames++;

		/* Defer timeout → force IDR */
		if (gc->defer_frames >= gc->config.defer_timeout_frames) {
			gc->state = GOP_STATE_FORCED_IDR;
			insert_idr = 1;
			boost = gc->config.idr_qp_boost;
			gc->forced_idrs++;
			gc->defer_active = 0;
			break;
		}

		/* Also force if max_gop_length reached while deferred */
		if (gc->frames_since_idr >= gc->config.max_gop_length) {
			gc->state = GOP_STATE_FORCED_IDR;
			insert_idr = 1;
			boost = gc->config.idr_qp_boost;
			gc->forced_idrs++;
			gc->defer_active = 0;
			break;
		}

		/* Check if defer was cleared externally */
		if (!gc->defer_active) {
			gc->state = GOP_STATE_IDR_READY;
			insert_idr = 1;
			boost = gc->config.idr_qp_boost;
		}
		break;

	case GOP_STATE_FORCED_IDR:
		/* Should have been handled — force it */
		insert_idr = 1;
		boost = gc->config.idr_qp_boost;
		break;
	}

	if (qp_boost_out)
		*qp_boost_out = boost;

	if (insert_idr)
		gc->total_idrs++;

	return insert_idr;
}

void gop_ctrl_idr_encoded(GopControllerState *gc)
{
	if (!gc)
		return;

	gc->frames_since_idr = 0;
	gc->defer_frames = 0;
	gc->defer_active = 0;
	gc->idr_requested = 0;
	gc->state = GOP_STATE_NORMAL;
}

void gop_ctrl_request_idr(GopControllerState *gc, uint8_t qp_boost)
{
	if (!gc)
		return;

	gc->idr_requested = 1;
	gc->idr_qp_boost = qp_boost;
}

void gop_ctrl_defer_idr(GopControllerState *gc)
{
	if (!gc)
		return;

	/* Only defer if we're in IDR_READY state */
	if (gc->state == GOP_STATE_IDR_READY) {
		gc->state = GOP_STATE_IDR_DEFERRED;
		gc->defer_frames = 0;
		gc->defer_active = 1;
		gc->deferred_idrs++;
	}
}

void gop_ctrl_clear_defer(GopControllerState *gc)
{
	if (!gc)
		return;

	gc->defer_active = 0;
	/* State transition happens in next on_frame call */
}

void gop_ctrl_get_state(const GopControllerState *gc, GopState *state,
	uint16_t *frames_since_idr)
{
	if (!gc) {
		if (state)
			*state = GOP_STATE_NORMAL;
		if (frames_since_idr)
			*frames_since_idr = 0;
		return;
	}

	if (state)
		*state = gc->state;
	if (frames_since_idr)
		*frames_since_idr = gc->frames_since_idr;
}

void gop_ctrl_set_config(GopControllerState *gc, const GopConfig *config)
{
	if (!gc || !config)
		return;

	gc->config = *config;
}
