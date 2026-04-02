#ifndef ENC_CTRL_H
#define ENC_CTRL_H

/*
 * Adaptive Encoder Control — Public API
 *
 * Clean C API for encoder feedback, scene-aware variable GOP,
 * and external control. A future link controller or coordinator
 * integrates against this surface.
 *
 * All calls are non-blocking.
 * Thread safety: encoder thread writes via enc_ctrl_on_frame();
 *   external threads read state and issue requests via the rest.
 *   Lock-free ring buffer for stats, atomic flags for IDR requests.
 */

#include "enc_types.h"

#include <stdint.h>

/* ── Initialization / shutdown ──────────────────────────────────────── */

/** Initialize encoder feedback and variable GOP.
 *  config: GOP configuration. NULL for defaults.
 *  venc_chn: MI_VENC channel index (-1 for mock/test mode).
 *  codec: 0 = H.264, 1 = H.265.
 *  bitrate_bps: initial target bitrate for budget ratio calculation.
 *  fps: frames per second for target frame size calculation.
 *  text_log: enable human-readable telemetry on stderr.
 *  Returns 0 on success, -1 on failure. */
int enc_ctrl_init(const GopConfig *config, int venc_chn, int codec,
	uint32_t bitrate_bps, uint32_t fps, int text_log);

/** Shutdown and free resources. */
void enc_ctrl_shutdown(void);

/* ── Per-frame processing (encoder thread) ──────────────────────────── */

/** Called after each frame is encoded.
 *  stream: pointer to MI_VENC_Stream_t (or NULL in test mode).
 *  Returns 0 on success. */
int enc_ctrl_on_frame(const void *stream);

/** Alternative: supply pre-built stats directly (for testing or
 *  when caller already parsed the stream). */
int enc_ctrl_on_frame_stats(const EncoderFrameStats *stats);

/* ── State queries (any thread) ─────────────────────────────────────── */

/** Get current scene estimate. */
int enc_ctrl_get_scene(SceneEstimate *out);

/** Get current GOP state and frames since last IDR. */
int enc_ctrl_get_gop_state(GopState *state, uint16_t *frames_since_idr);

/** Get full stats history (ring buffer, last N frames).
 *  Returns actual number of entries copied. */
uint16_t enc_ctrl_get_history(EncoderFrameStats *buf, uint16_t max_frames);

/** Get latest single frame stats. */
int enc_ctrl_get_latest(EncoderFrameStats *out);

/* ── External control (any thread) ──────────────────────────────────── */

/** Request IDR insertion on next eligible frame. */
int enc_ctrl_request_idr(uint8_t qp_boost);

/** Defer pending IDR (future: link controller says "not now"). */
int enc_ctrl_defer_idr(void);

/** Clear IDR deferral. */
int enc_ctrl_clear_defer(void);

/** Set target bitrate (passthrough to MI_VENC, updates budget calc). */
int enc_ctrl_set_bitrate(uint32_t bitrate_bps);

/** Set QP bounds (passthrough to MI_VENC). */
int enc_ctrl_set_qp_range(uint8_t qp_min, uint8_t qp_max);

/* ── Diagnostics ────────────────────────────────────────────────────── */

/** Dump recent telemetry to stderr. */
void enc_ctrl_dump_telemetry(uint32_t last_n);

/** Get total frames processed. */
uint32_t enc_ctrl_frame_count(void);

/** Check if initialized. */
int enc_ctrl_is_active(void);

#endif /* ENC_CTRL_H */
