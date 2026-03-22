#ifndef STAR6E_CONTROLS_H
#define STAR6E_CONTROLS_H

#include "star6e_pipeline.h"
#include "venc_api.h"

enum {
	STAR6E_CONTROLS_IDLE_FPS = 5,
};

/** Bind pipeline and config to runtime control state. */
void star6e_controls_bind(Star6ePipelineState *pipeline, VencConfig *vcfg);

/** Reset control state to defaults (called between pipeline restarts). */
void star6e_controls_reset(void);

/** Return Star6E backend's live control callback table. */
const VencApplyCallbacks *star6e_controls_callbacks(void);

/** Apply frame rate change to running encoder pipeline. */
int star6e_controls_apply_fps(uint32_t fps);

/** Apply ROI-based QP adjustment for FPV center emphasis. */
int star6e_controls_apply_roi_qp(int qp);

/** Apply relative I/P QP delta to the running encoder. */
int star6e_controls_apply_qp_delta(int delta);

/** Apply frame-lost threshold to a VENC channel.
 *  When enabled, sets the threshold to 120% of target bitrate.
 *  No-op when enabled is false. Returns 0 on success, -1 on error. */
int star6e_controls_apply_frame_lost_threshold(MI_VENC_CHN chn, bool enabled,
	uint32_t kbps);

#endif /* STAR6E_CONTROLS_H */
