#include "enc_ctrl.h"
#include "gop_controller.h"
#include "ring_buffer.h"
#include "scene_estimator.h"
#include "telemetry.h"
#include "venc_sdk_if.h"

#include <pthread.h>
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
	uint8_t              qp_boosted;       /* 1 if QP is currently boosted */
	uint8_t              idr_boost_pending; /* frames remaining to wait for boosted IDR */
} EncCtrlState;

static EncCtrlState g_enc_ctrl;
static pthread_mutex_t g_enc_ctrl_lock = PTHREAD_MUTEX_INITIALIZER;

static void restore_qp_range_locked(void)
{
	if (!g_enc_ctrl.qp_boosted || !g_enc_ctrl.sdk_ops || !g_enc_ctrl.sdk_ctx)
		return;

	g_enc_ctrl.sdk_ops->set_qp_range(g_enc_ctrl.sdk_ctx,
		g_enc_ctrl.base_qp_min, g_enc_ctrl.base_qp_max);
	g_enc_ctrl.qp_boosted = 0;
	g_enc_ctrl.idr_boost_pending = 0;
}

static void update_scene_target_locked(void)
{
	if (g_enc_ctrl.fps == 0)
		return;

	scene_est_set_target(&g_enc_ctrl.scene,
		g_enc_ctrl.bitrate_bps / (g_enc_ctrl.fps * 8));
}

static void update_gop_lengths_locked(uint16_t max_gop_length,
	uint16_t min_gop_length)
{
	GopConfig cfg;

	cfg = g_enc_ctrl.gop.config;
	cfg.max_gop_length = max_gop_length;
	cfg.min_gop_length = min_gop_length;
	gop_ctrl_set_config(&g_enc_ctrl.gop, &cfg);
}

static int stats_is_idr_frame(const EncoderFrameStats *stats)
{
	return stats && (stats->frame_type == ENC_FRAME_IDR ||
		stats->frame_type == ENC_FRAME_I);
}

/* ── Public API ─────────────────────────────────────────────────────── */

int enc_ctrl_init(const GopConfig *config, int venc_chn, int codec,
	uint32_t bitrate_bps, uint32_t fps, int text_log)
{
	GopConfig cfg;
	uint32_t target_frame_size;
	int rc;

	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (g_enc_ctrl.active) {
		fprintf(stderr, "[enc_ctrl] already initialized\n");
		pthread_mutex_unlock(&g_enc_ctrl_lock);
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
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	/* Scene estimator */
	target_frame_size = bitrate_bps / (g_enc_ctrl.fps * 8);
	scene_est_init(&g_enc_ctrl.scene, cfg.scene_change_threshold,
		cfg.scene_change_holdoff, target_frame_size);

	/* GOP controller */
	gop_ctrl_init(&g_enc_ctrl.gop, &cfg);

	/* SDK interface */
	if (venc_chn >= 0)
		g_enc_ctrl.sdk_ctx = venc_sdk_create(venc_chn, codec);
	else
		g_enc_ctrl.sdk_ctx = venc_sdk_create_mock();

	if (!g_enc_ctrl.sdk_ctx) {
		fprintf(stderr, "[enc_ctrl] SDK context creation failed\n");
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	g_enc_ctrl.sdk_ops = venc_sdk_get_ops(g_enc_ctrl.sdk_ctx);

	/* Read initial QP range */
	rc = g_enc_ctrl.sdk_ops->get_qp_range(g_enc_ctrl.sdk_ctx,
		&g_enc_ctrl.base_qp_min, &g_enc_ctrl.base_qp_max);
	if (rc != 0) {
		g_enc_ctrl.base_qp_min = 0;
		g_enc_ctrl.base_qp_max = 51;
	}

	/* Telemetry */
	telemetry_init(&g_enc_ctrl.telemetry, text_log);

	g_enc_ctrl.active = 1;

	fprintf(stderr, "[enc_ctrl] initialized: vgop=%s bitrate=%u fps=%u "
		"max_gop=%u min_gop=%u threshold=%u\n",
		cfg.enable_variable_gop ? "on" : "off",
		bitrate_bps, g_enc_ctrl.fps,
		cfg.max_gop_length, cfg.min_gop_length,
		cfg.scene_change_threshold);

	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return 0;
}

void enc_ctrl_shutdown(void)
{
	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (!g_enc_ctrl.active) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return;
	}

	restore_qp_range_locked();

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
	pthread_mutex_unlock(&g_enc_ctrl_lock);
}

static void process_frame(EncoderFrameStats *stats)
{
	SceneEstimate scene;
	GopState gop_state;
	uint16_t frames_since_idr;
	uint8_t qp_boost = 0;
	int insert_idr;
	int is_idr_frame;

	/* Write to ring buffer */
	enc_ring_write(&g_enc_ctrl.ring, stats);

	/* Update scene estimator */
	scene_est_update(&g_enc_ctrl.scene, stats);
	scene_est_get(&g_enc_ctrl.scene, &scene);
	is_idr_frame = stats_is_idr_frame(stats);

	/* Restore QP if we boosted it for an IDR we requested.
	 * Only restore when we see that IDR arrive, or if the
	 * pending counter expires (IDR took too long). */
	if (g_enc_ctrl.qp_boosted) {
		if (is_idr_frame) {
			restore_qp_range_locked();
		} else if (g_enc_ctrl.idr_boost_pending > 0) {
			g_enc_ctrl.idr_boost_pending--;
			if (g_enc_ctrl.idr_boost_pending == 0) {
				/* Timeout: IDR never arrived, restore QP */
				restore_qp_range_locked();
			}
		}
	}

	/* Notify GOP controller if an IDR was received */
	if (is_idr_frame) {
		gop_ctrl_idr_encoded(&g_enc_ctrl.gop);
		scene_est_clear_change(&g_enc_ctrl.scene);
		scene_est_get(&g_enc_ctrl.scene, &scene);
		gop_ctrl_get_state(&g_enc_ctrl.gop, &gop_state, &frames_since_idr);
		telemetry_record(&g_enc_ctrl.telemetry, stats, &scene,
			gop_state, frames_since_idr, 0);
		return;
	}

	/* Evaluate GOP state machine for the next encoded frame. */
	insert_idr = gop_ctrl_on_frame(&g_enc_ctrl.gop, &scene, &qp_boost);

	gop_ctrl_get_state(&g_enc_ctrl.gop, &gop_state, &frames_since_idr);

	/* Record telemetry */
	telemetry_record(&g_enc_ctrl.telemetry, stats, &scene,
		gop_state, frames_since_idr, insert_idr);

	/* Act on IDR decision */
	if (insert_idr && g_enc_ctrl.sdk_ops && g_enc_ctrl.sdk_ctx) {
		/* Apply QP boost before IDR. Only arm it once per pending IDR. */
		if (qp_boost > 0 && !g_enc_ctrl.qp_boosted) {
			uint8_t boosted_min = g_enc_ctrl.base_qp_min + qp_boost;
			uint8_t boosted_max = g_enc_ctrl.base_qp_max;

			if (boosted_min > 51)
				boosted_min = 51;
			if (boosted_min > boosted_max)
				boosted_max = boosted_min;

			g_enc_ctrl.sdk_ops->set_qp_range(g_enc_ctrl.sdk_ctx,
				boosted_min, boosted_max);
			g_enc_ctrl.qp_boosted = 1;
			/* Allow up to 10 frames for the IDR to arrive;
			 * restore QP if it doesn't (avoids stuck boost). */
			g_enc_ctrl.idr_boost_pending = 10;
		}

		g_enc_ctrl.sdk_ops->request_idr(g_enc_ctrl.sdk_ctx, 1);
	}
}

int enc_ctrl_on_frame(const void *stream)
{
	EncoderFrameStats stats;
	int rc;

	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (!g_enc_ctrl.active || !g_enc_ctrl.sdk_ops || !g_enc_ctrl.sdk_ctx) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	rc = g_enc_ctrl.sdk_ops->extract_frame_stats(g_enc_ctrl.sdk_ctx,
		stream, g_enc_ctrl.frame_seq, &stats);
	if (rc != 0) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	g_enc_ctrl.frame_seq++;
	process_frame(&stats);

	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return 0;
}

int enc_ctrl_on_frame_stats(const EncoderFrameStats *stats)
{
	EncoderFrameStats local;

	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (!g_enc_ctrl.active || !stats) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	local = *stats;
	local.frame_seq = g_enc_ctrl.frame_seq;
	g_enc_ctrl.frame_seq++;

	process_frame(&local);

	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return 0;
}

int enc_ctrl_get_scene(SceneEstimate *out)
{
	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (!g_enc_ctrl.active || !out) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	scene_est_get(&g_enc_ctrl.scene, out);

	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return 0;
}

int enc_ctrl_get_gop_state(GopState *state, uint16_t *frames_since_idr)
{
	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (!g_enc_ctrl.active) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	gop_ctrl_get_state(&g_enc_ctrl.gop, state, frames_since_idr);

	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return 0;
}

uint16_t enc_ctrl_get_history(EncoderFrameStats *buf, uint16_t max_frames)
{
	uint16_t count;

	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (!g_enc_ctrl.active || !buf) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return 0;
	}

	count = enc_ring_snapshot(&g_enc_ctrl.ring, buf, max_frames);

	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return count;
}

int enc_ctrl_get_latest(EncoderFrameStats *out)
{
	int rc;

	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (!g_enc_ctrl.active || !out) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	rc = enc_ring_peek_latest(&g_enc_ctrl.ring, out);

	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return rc;
}

int enc_ctrl_get_latest_telemetry(TelemetryRecord *out)
{
	int rc;

	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (!g_enc_ctrl.active || !out) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	rc = telemetry_get_latest(&g_enc_ctrl.telemetry, out);

	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return rc;
}

int enc_ctrl_request_idr(uint8_t qp_boost)
{
	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (!g_enc_ctrl.active) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	gop_ctrl_request_idr(&g_enc_ctrl.gop, qp_boost);

	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return 0;
}

int enc_ctrl_defer_idr(void)
{
	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (!g_enc_ctrl.active) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	gop_ctrl_defer_idr(&g_enc_ctrl.gop);

	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return 0;
}

int enc_ctrl_clear_defer(void)
{
	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (!g_enc_ctrl.active) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	gop_ctrl_clear_defer(&g_enc_ctrl.gop);

	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return 0;
}

int enc_ctrl_set_bitrate(uint32_t bitrate_bps)
{
	int rc = 0;

	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (!g_enc_ctrl.active) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	g_enc_ctrl.bitrate_bps = bitrate_bps;
	update_scene_target_locked();

	if (g_enc_ctrl.sdk_ops && g_enc_ctrl.sdk_ctx) {
		rc = g_enc_ctrl.sdk_ops->set_bitrate(g_enc_ctrl.sdk_ctx,
			bitrate_bps);
		if (rc != 0)
			fprintf(stderr, "[enc_ctrl] set_bitrate failed: %d\n",
				rc);
	}

	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return rc;
}

int enc_ctrl_set_fps(uint32_t fps, uint16_t max_gop_length,
	uint16_t min_gop_length)
{
	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (!g_enc_ctrl.active || fps == 0 || max_gop_length == 0 ||
	    min_gop_length > max_gop_length) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	g_enc_ctrl.fps = fps;
	update_scene_target_locked();
	update_gop_lengths_locked(max_gop_length, min_gop_length);

	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return 0;
}

int enc_ctrl_set_qp_range(uint8_t qp_min, uint8_t qp_max)
{
	int rc = 0;

	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (!g_enc_ctrl.active) {
		pthread_mutex_unlock(&g_enc_ctrl_lock);
		return -1;
	}

	/* Update base QP range */
	g_enc_ctrl.base_qp_min = qp_min;
	g_enc_ctrl.base_qp_max = qp_max;

	/* Apply immediately if not currently boosted */
	if (!g_enc_ctrl.qp_boosted && g_enc_ctrl.sdk_ops &&
	    g_enc_ctrl.sdk_ctx) {
		rc = g_enc_ctrl.sdk_ops->set_qp_range(g_enc_ctrl.sdk_ctx,
			qp_min, qp_max);
	}

	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return rc;
}

void enc_ctrl_dump_telemetry(uint32_t last_n)
{
	pthread_mutex_lock(&g_enc_ctrl_lock);

	if (g_enc_ctrl.active)
		telemetry_dump(&g_enc_ctrl.telemetry, last_n);

	pthread_mutex_unlock(&g_enc_ctrl_lock);
}

uint32_t enc_ctrl_frame_count(void)
{
	uint32_t count;

	pthread_mutex_lock(&g_enc_ctrl_lock);
	count = g_enc_ctrl.frame_seq;
	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return count;
}

int enc_ctrl_is_active(void)
{
	int active;

	pthread_mutex_lock(&g_enc_ctrl_lock);
	active = g_enc_ctrl.active;
	pthread_mutex_unlock(&g_enc_ctrl_lock);
	return active;
}
