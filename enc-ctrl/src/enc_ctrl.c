#include "enc_ctrl.h"
#include "ring_buffer.h"
#include "scene_estimator.h"
#include "gop_controller.h"
#include "venc_sdk_if.h"
#include "telemetry.h"

#include <stdio.h>
#include <string.h>

/* ── Internal state (module-scoped) ─────────────────────────────────── */

#define STATS_RING_SLOTS 512  /* must be power of 2 */

typedef struct {
	int                  active;
	uint32_t             frame_seq;

	/* Ring buffer for stats history */
	EncRingBuffer        ring;
	EncoderFrameStats    ring_slots[STATS_RING_SLOTS];

	/* Scene estimator */
	SceneEstimatorState  scene;

	/* GOP controller */
	GopControllerState   gop;

	/* SDK interface */
	VencSdkContext      *sdk_ctx;
	const VencSdkOps    *sdk_ops;

	/* Telemetry */
	TelemetryState       telemetry;

	/* Config */
	uint32_t             bitrate_bps;
	uint32_t             fps;

	/* Saved QP range for IDR boost restore */
	uint8_t              base_qp_min;
	uint8_t              base_qp_max;
	uint8_t              qp_boosted;  /* 1 if QP is currently boosted */
	uint8_t              _pad0;
} EncCtrlState;

static EncCtrlState g_enc_ctrl;

/* ── Public API ─────────────────────────────────────────────────────── */

int enc_ctrl_init(const GopConfig *config, int venc_chn, int codec,
	uint32_t bitrate_bps, uint32_t fps, int text_log)
{
	GopConfig cfg;
	uint32_t target_frame_size;

	if (g_enc_ctrl.active) {
		fprintf(stderr, "[enc_ctrl] already initialized\n");
		return -1;
	}

	memset(&g_enc_ctrl, 0, sizeof(g_enc_ctrl));

	/* Config */
	if (config)
		cfg = *config;
	else
		gop_config_defaults(&cfg);

	g_enc_ctrl.bitrate_bps = bitrate_bps;
	g_enc_ctrl.fps = fps > 0 ? fps : 60;

	/* Ring buffer */
	if (enc_ring_init(&g_enc_ctrl.ring, g_enc_ctrl.ring_slots,
	    STATS_RING_SLOTS) != 0) {
		fprintf(stderr, "[enc_ctrl] ring buffer init failed\n");
		return -1;
	}

	/* Scene estimator */
	target_frame_size = bitrate_bps / (g_enc_ctrl.fps * 8);
	scene_est_init(&g_enc_ctrl.scene, cfg.scene_change_threshold,
		cfg.scene_change_holdoff, target_frame_size);

	/* GOP controller */
	gop_ctrl_init(&g_enc_ctrl.gop, &cfg);

	/* SDK interface */
	if (venc_chn >= 0) {
		g_enc_ctrl.sdk_ctx = venc_sdk_create(venc_chn, codec);
	} else {
		g_enc_ctrl.sdk_ctx = venc_sdk_create_mock();
	}

	if (!g_enc_ctrl.sdk_ctx) {
		fprintf(stderr, "[enc_ctrl] SDK context creation failed\n");
		return -1;
	}

	g_enc_ctrl.sdk_ops = venc_sdk_ops();

	/* Read initial QP range */
	g_enc_ctrl.sdk_ops->get_qp_range(g_enc_ctrl.sdk_ctx,
		&g_enc_ctrl.base_qp_min, &g_enc_ctrl.base_qp_max);

	/* Telemetry */
	telemetry_init(&g_enc_ctrl.telemetry, text_log);

	g_enc_ctrl.active = 1;

	fprintf(stderr, "[enc_ctrl] initialized: vgop=%s bitrate=%u fps=%u "
		"max_gop=%u min_gop=%u threshold=%u\n",
		cfg.enable_variable_gop ? "on" : "off",
		bitrate_bps, g_enc_ctrl.fps,
		cfg.max_gop_length, cfg.min_gop_length,
		cfg.scene_change_threshold);

	return 0;
}

void enc_ctrl_shutdown(void)
{
	if (!g_enc_ctrl.active)
		return;

	/* Restore QP if boosted */
	if (g_enc_ctrl.qp_boosted && g_enc_ctrl.sdk_ops && g_enc_ctrl.sdk_ctx) {
		g_enc_ctrl.sdk_ops->set_qp_range(g_enc_ctrl.sdk_ctx,
			g_enc_ctrl.base_qp_min, g_enc_ctrl.base_qp_max);
	}

	if (g_enc_ctrl.sdk_ctx)
		venc_sdk_destroy(g_enc_ctrl.sdk_ctx);

	fprintf(stderr, "[enc_ctrl] shutdown: %u frames, %u IDRs "
		"(%u scene, %u forced, %u deferred)\n",
		g_enc_ctrl.frame_seq,
		g_enc_ctrl.gop.total_idrs,
		g_enc_ctrl.gop.scene_change_idrs,
		g_enc_ctrl.gop.forced_idrs,
		g_enc_ctrl.gop.deferred_idrs);

	memset(&g_enc_ctrl, 0, sizeof(g_enc_ctrl));
}

static void process_frame(EncoderFrameStats *stats)
{
	SceneEstimate scene;
	GopState gop_state;
	uint16_t frames_since_idr;
	uint8_t qp_boost = 0;
	int insert_idr;

	/* Write to ring buffer */
	enc_ring_write(&g_enc_ctrl.ring, stats);

	/* Update scene estimator */
	scene_est_update(&g_enc_ctrl.scene, stats);
	scene_est_get(&g_enc_ctrl.scene, &scene);

	/* Restore QP if previously boosted and this was the IDR frame */
	if (g_enc_ctrl.qp_boosted &&
	    (stats->frame_type == ENC_FRAME_IDR ||
	     stats->frame_type == ENC_FRAME_I)) {
		g_enc_ctrl.sdk_ops->set_qp_range(g_enc_ctrl.sdk_ctx,
			g_enc_ctrl.base_qp_min, g_enc_ctrl.base_qp_max);
		g_enc_ctrl.qp_boosted = 0;
	}

	/* Notify GOP controller if an IDR was received */
	if (stats->frame_type == ENC_FRAME_IDR ||
	    stats->frame_type == ENC_FRAME_I) {
		gop_ctrl_idr_encoded(&g_enc_ctrl.gop);
		scene_est_clear_change(&g_enc_ctrl.scene);
	}

	/* Evaluate GOP state machine for next frame */
	insert_idr = gop_ctrl_on_frame(&g_enc_ctrl.gop, &scene, &qp_boost);

	gop_ctrl_get_state(&g_enc_ctrl.gop, &gop_state, &frames_since_idr);

	/* Record telemetry */
	telemetry_record(&g_enc_ctrl.telemetry, stats, &scene,
		gop_state, frames_since_idr, insert_idr);

	/* Act on IDR decision */
	if (insert_idr && g_enc_ctrl.sdk_ops && g_enc_ctrl.sdk_ctx) {
		/* Apply QP boost before IDR */
		if (qp_boost > 0) {
			uint8_t boosted_min = g_enc_ctrl.base_qp_min + qp_boost;
			uint8_t boosted_max = g_enc_ctrl.base_qp_max;

			if (boosted_min > 51) boosted_min = 51;
			if (boosted_min > boosted_max)
				boosted_max = boosted_min;

			g_enc_ctrl.sdk_ops->set_qp_range(g_enc_ctrl.sdk_ctx,
				boosted_min, boosted_max);
			g_enc_ctrl.qp_boosted = 1;
		}

		g_enc_ctrl.sdk_ops->request_idr(g_enc_ctrl.sdk_ctx, 1);
	}
}

int enc_ctrl_on_frame(const void *stream)
{
	EncoderFrameStats stats;
	int rc;

	if (!g_enc_ctrl.active)
		return -1;

	if (!g_enc_ctrl.sdk_ops || !g_enc_ctrl.sdk_ctx)
		return -1;

	rc = g_enc_ctrl.sdk_ops->extract_frame_stats(g_enc_ctrl.sdk_ctx,
		stream, g_enc_ctrl.frame_seq, &stats);
	if (rc != 0)
		return -1;

	g_enc_ctrl.frame_seq++;
	process_frame(&stats);

	return 0;
}

int enc_ctrl_on_frame_stats(const EncoderFrameStats *stats)
{
	EncoderFrameStats local;

	if (!g_enc_ctrl.active || !stats)
		return -1;

	local = *stats;
	local.frame_seq = g_enc_ctrl.frame_seq;
	g_enc_ctrl.frame_seq++;

	process_frame(&local);
	return 0;
}

int enc_ctrl_get_scene(SceneEstimate *out)
{
	if (!g_enc_ctrl.active || !out)
		return -1;

	scene_est_get(&g_enc_ctrl.scene, out);
	return 0;
}

int enc_ctrl_get_gop_state(GopState *state, uint16_t *frames_since_idr)
{
	if (!g_enc_ctrl.active)
		return -1;

	gop_ctrl_get_state(&g_enc_ctrl.gop, state, frames_since_idr);
	return 0;
}

uint16_t enc_ctrl_get_history(EncoderFrameStats *buf, uint16_t max_frames)
{
	if (!g_enc_ctrl.active || !buf)
		return 0;

	return enc_ring_read_batch(&g_enc_ctrl.ring, buf, max_frames);
}

int enc_ctrl_get_latest(EncoderFrameStats *out)
{
	if (!g_enc_ctrl.active || !out)
		return -1;

	return enc_ring_peek_latest(&g_enc_ctrl.ring, out);
}

int enc_ctrl_request_idr(uint8_t qp_boost)
{
	if (!g_enc_ctrl.active)
		return -1;

	gop_ctrl_request_idr(&g_enc_ctrl.gop, qp_boost);
	return 0;
}

int enc_ctrl_defer_idr(void)
{
	if (!g_enc_ctrl.active)
		return -1;

	gop_ctrl_defer_idr(&g_enc_ctrl.gop);
	return 0;
}

int enc_ctrl_clear_defer(void)
{
	if (!g_enc_ctrl.active)
		return -1;

	gop_ctrl_clear_defer(&g_enc_ctrl.gop);
	return 0;
}

int enc_ctrl_set_bitrate(uint32_t bitrate_bps)
{
	int rc;

	if (!g_enc_ctrl.active)
		return -1;

	g_enc_ctrl.bitrate_bps = bitrate_bps;

	/* Update scene estimator target */
	if (g_enc_ctrl.fps > 0) {
		scene_est_set_target(&g_enc_ctrl.scene,
			bitrate_bps / (g_enc_ctrl.fps * 8));
	}

	/* Pass through to SDK */
	if (g_enc_ctrl.sdk_ops && g_enc_ctrl.sdk_ctx) {
		rc = g_enc_ctrl.sdk_ops->set_bitrate(g_enc_ctrl.sdk_ctx,
			bitrate_bps);
		if (rc != 0)
			fprintf(stderr, "[enc_ctrl] set_bitrate failed: %d\n",
				rc);
		return rc;
	}

	return 0;
}

int enc_ctrl_set_qp_range(uint8_t qp_min, uint8_t qp_max)
{
	if (!g_enc_ctrl.active)
		return -1;

	/* Update base QP range */
	g_enc_ctrl.base_qp_min = qp_min;
	g_enc_ctrl.base_qp_max = qp_max;

	/* Apply immediately if not currently boosted */
	if (!g_enc_ctrl.qp_boosted && g_enc_ctrl.sdk_ops &&
	    g_enc_ctrl.sdk_ctx) {
		return g_enc_ctrl.sdk_ops->set_qp_range(g_enc_ctrl.sdk_ctx,
			qp_min, qp_max);
	}

	return 0;
}

void enc_ctrl_dump_telemetry(uint32_t last_n)
{
	if (!g_enc_ctrl.active)
		return;

	telemetry_dump(&g_enc_ctrl.telemetry, last_n);
}

uint32_t enc_ctrl_frame_count(void)
{
	return g_enc_ctrl.frame_seq;
}

int enc_ctrl_is_active(void)
{
	return g_enc_ctrl.active;
}
