#include "star6e_runtime.h"

#include "debug_osd.h"
#include "eis.h"
#include "imu_bmi270.h"
#include "pipeline_common.h"
#include "sdk_quiet.h"
#include "star6e_controls.h"
#include "star6e_cus3a.h"
#include "star6e_iq.h"
#include "star6e_pipeline.h"
#include "star6e.h"
#include "venc_api.h"
#include "venc_config.h"
#include "venc_httpd.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static SdkQuietState g_sdk_quiet = SDK_QUIET_STATE_INIT;

static MI_VENC_Pack_t *ensure_packs(MI_VENC_Pack_t **buf,
	uint32_t *cap, uint32_t need)
{
	if (need <= *cap)
		return *buf;
	free(*buf);
	*buf = malloc(need * sizeof(MI_VENC_Pack_t));
	*cap = *buf ? need : 0;
	return *buf;
}

/* Forward declaration — record status callback for HTTP API */
static void record_status_callback(VencRecordStatus *out);

/*
 * Sleep for up to timeout_ms, but wake early to service the sidecar fd
 * (sync responses need low latency).  Falls back to usleep when the
 * sidecar is disabled (fd < 0).
 */
static void idle_wait(RtpSidecarSender *sc, int timeout_ms)
{
	if (!sc || sc->fd < 0) {
		usleep((unsigned)(timeout_ms * 1000));
		return;
	}
	struct pollfd pfd = { .fd = sc->fd, .events = POLLIN };
	if (poll(&pfd, 1, timeout_ms) > 0)
		rtp_sidecar_poll(sc);
}

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_signal_count = 0;
static struct timespec g_imu_eis_verbose_last = {0};

/* ── Lightweight scene detector ──────────────────────────────────────── */

#define ENC_FRAME_P   0
#define ENC_FRAME_IDR 2

#define SCENE_EMA_SHIFT      4
#define SCENE_WARMUP_FRAMES  8
#define SCENE_COOLDOWN_AFTER_IDR 30

typedef struct {
	uint32_t ema_size_fp8;
	uint32_t frame_count;
	uint32_t last_frame_size;  /* cached from scene_update */
	uint8_t  last_frame_type;  /* cached from scene_update */
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
	uint8_t  idr_enabled;   /* 1 = request IDR after settle, 0 = telemetry only */
	uint8_t  warmup_done;   /* set once frame_count passes SCENE_WARMUP_FRAMES */
} SceneDetector;

static uint32_t scene_frame_size(const MI_VENC_Stream_t *s)
{
	uint32_t t = 0;
	unsigned int i;
	if (!s || !s->packet) return 0;
	for (i = 0; i < s->count; i++) t += s->packet[i].length;
	return t;
}

static uint8_t scene_frame_type(const MI_VENC_Stream_t *s, int codec)
{
	unsigned int i;
	if (!s || !s->packet) return ENC_FRAME_P;
	for (i = 0; i < s->count; i++) {
		const MI_VENC_Pack_t *p = &s->packet[i];
		unsigned int k, n = p->packNum > 8 ? 8 : p->packNum;
		if (n > 0) {
			for (k = 0; k < n; k++) {
				if (codec == 0 && p->packetInfo[k].packType.h264Nalu == 5)
					return ENC_FRAME_IDR;
				if (codec != 0 && p->packetInfo[k].packType.h265Nalu == 19)
					return ENC_FRAME_IDR;
			}
		} else {
			if (codec == 0 && p->naluType.h264Nalu == 5) return ENC_FRAME_IDR;
			if (codec != 0 && p->naluType.h265Nalu == 19) return ENC_FRAME_IDR;
		}
	}
	return ENC_FRAME_P;
}

static void scene_init(SceneDetector *sd, uint16_t threshold, uint8_t holdoff)
{
	memset(sd, 0, sizeof(*sd));
	sd->threshold = threshold > 0 ? threshold : 150;
	sd->holdoff = holdoff > 0 ? holdoff : 2;
	sd->idr_enabled = threshold > 0 ? 1 : 0;
}

static void scene_update(SceneDetector *sd, const MI_VENC_Stream_t *stream,
	int codec, int venc_chn)
{
	uint32_t size, size_fp8, ema_size, ratio_x100;
	uint8_t ftype;

	if (!sd) return;
	size = scene_frame_size(stream);
	ftype = scene_frame_type(stream, codec);
	if (size > (UINT32_MAX / 1000)) size = (UINT32_MAX / 1000);
	size_fp8 = size << 8;

	/* Cache for scene_fill_sidecar — avoids re-walking packet arrays */
	sd->last_frame_size = size;
	sd->last_frame_type = ftype;

	if (sd->frame_count < UINT32_MAX) sd->frame_count++;
	sd->idr_inserted = 0;
	sd->scene_change = 0;

	if (ftype == ENC_FRAME_IDR) {
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
	ratio_x100 = ema_size > 0 ? (size * 100) / ema_size : 100;

	{ /* complexity 0-255 from frame size ratio */
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

	/* Scene-change IDR logic — skip entirely when disabled (threshold=0) */
	if (!sd->idr_enabled) return;

	if (sd->cooldown > 0) { sd->cooldown--; return; }

	if (sd->spike_pending) {
		if (ratio_x100 <= 120) {
			sd->spike_pending = 0;
			sd->settle_count = 0;
			sd->scene_change = 1;
			sd->idr_inserted = 1;
			MI_VENC_RequestIdr(venc_chn, 1);
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

static void scene_fill_sidecar(const SceneDetector *sd,
	RtpSidecarEncInfo *out)
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

/* ── Runner context ────────────────────────────────────────────────────── */

typedef struct {
	VencConfig vcfg;
	Star6ePipelineState ps;
	int system_initialized;
	int httpd_started;
	int pipeline_started;
	SceneDetector scene;
} Star6eRunnerContext;

static void install_signal_handlers(void);

/* Write VPE SCL clock preset before forced exit.  Uses only
 * async-signal-safe syscalls (open/write/close). */
static void scl_preset_emergency(void)
{
	static const char path[] = "/sys/devices/virtual/mstar/mscl/clk";
	static const char val[] = "384000000\n";
	static const char msg[] = "[venc] Emergency SCL preset written\n";
	int fd = open(path, O_WRONLY);

	if (fd >= 0) {
		(void)write(fd, val, sizeof(val) - 1);
		close(fd);
		(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
	}
}

static void handle_signal(int sig)
{
	if (sig == SIGALRM) {
		static const char msg[] =
			"\n> Shutdown timeout reached, force exiting.\n";

		(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
		scl_preset_emergency();
		_exit(128 + SIGINT);
	}

	if (sig == SIGHUP) {
		static const char msg[] =
			"\n> SIGHUP received, reinit pending...\n";

		venc_api_request_reinit(1);
		(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
		return;
	}

	g_running = 0;
	g_signal_count++;

	if (g_signal_count == 1) {
		static const char msg[] =
			"\n> Interrupt received, shutting down...\n";

		(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
		alarm(5);
		return;
	}

	{
		static const char msg[] = "\n> Force exiting.\n";

		(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
	}
	scl_preset_emergency();
	_exit(128 + sig);
}

static void install_signal_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGALRM, &sa, NULL);
}

static Star6eRunnerContext *g_runner_ctx;

static void record_status_callback(VencRecordStatus *out)
{
	Star6ePipelineState *ps;

	memset(out, 0, sizeof(*out));
	if (!g_runner_ctx)
		return;
	ps = &g_runner_ctx->ps;

	if (star6e_ts_recorder_is_active(&ps->ts_recorder)) {
		out->active = 1;
		snprintf(out->format, sizeof(out->format), "ts");
		star6e_ts_recorder_status(&ps->ts_recorder,
			&out->bytes_written, &out->frames_written,
			&out->segments, NULL, NULL);
		snprintf(out->path, sizeof(out->path), "%s", ps->ts_recorder.path);
		snprintf(out->stop_reason, sizeof(out->stop_reason), "none");
	} else if (star6e_recorder_is_active(&ps->recorder)) {
		out->active = 1;
		snprintf(out->format, sizeof(out->format), "hevc");
		star6e_recorder_status(&ps->recorder,
			&out->bytes_written, &out->frames_written,
			NULL, NULL);
		snprintf(out->path, sizeof(out->path), "%s", ps->recorder.path);
		snprintf(out->stop_reason, sizeof(out->stop_reason), "none");
	} else {
		/* Check both recorders for last stop reason */
		const char *reason = "manual";
		Star6eRecorderStopReason sr = ps->ts_recorder.last_stop_reason;
		if (sr == RECORDER_STOP_MANUAL)
			sr = ps->recorder.last_stop_reason;
		if (sr == RECORDER_STOP_DISK_FULL)
			reason = "disk_full";
		else if (sr == RECORDER_STOP_WRITE_ERROR)
			reason = "write_error";
		snprintf(out->stop_reason, sizeof(out->stop_reason), "%s", reason);
		snprintf(out->format, sizeof(out->format), "%s",
			g_runner_ctx->vcfg.record.format);
	}
}

static int runtime_request_idr(void)
{
	if (!g_runner_ctx)
		return -1;

	return MI_VENC_RequestIdr(g_runner_ctx->ps.venc_channel, 1) == 0 ? 0 : -1;
}

static void start_custom_ae(const Star6ePipelineState *ps,
	const VencConfig *vcfg)
{
	Star6eCus3aConfig ae_cfg;

	star6e_cus3a_config_defaults(&ae_cfg);
	ae_cfg.sensor_fps = ps->sensor.fps;
	if (vcfg->isp.ae_fps > 0)
		ae_cfg.ae_fps = vcfg->isp.ae_fps;
	if (vcfg->isp.exposure > 0)
		ae_cfg.shutter_max_us = vcfg->isp.exposure * 1000;
	if (vcfg->isp.gain_max > 0)
		ae_cfg.gain_max = vcfg->isp.gain_max;
	ae_cfg.verbose = vcfg->system.verbose;
	star6e_cus3a_start(&ae_cfg);
}
/* Reduce ch1 bitrate by 10%.  Mirrors apply_bitrate() from
 * star6e_controls.c but operates on the dual VENC channel. */
static int dual_rec_reduce_bitrate(MI_VENC_CHN chn, uint32_t *current_kbps,
	uint32_t min_kbps)
{
	MI_VENC_ChnAttr_t attr = {0};
	uint32_t new_kbps;
	MI_U32 bits;

	if (MI_VENC_GetChnAttr(chn, &attr) != 0)
		return -1;

	new_kbps = *current_kbps * 9 / 10;
	if (new_kbps < min_kbps)
		new_kbps = min_kbps;
	if (new_kbps == *current_kbps)
		return 0;  /* already at floor */

	bits = new_kbps * 1024;
	switch (attr.rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		attr.rate.h265Cbr.bitrate = bits;
		break;
	case I6_VENC_RATEMODE_H264CBR:
		attr.rate.h264Cbr.bitrate = bits;
		break;
	case I6_VENC_RATEMODE_H265VBR:
		attr.rate.h265Vbr.maxBitrate = bits;
		break;
	case I6_VENC_RATEMODE_H264VBR:
		attr.rate.h264Vbr.maxBitrate = bits;
		break;
	case I6_VENC_RATEMODE_H265AVBR:
		attr.rate.h265Avbr.maxBitrate = bits;
		break;
	case I6_VENC_RATEMODE_H264AVBR:
		attr.rate.h264Avbr.maxBitrate = bits;
		break;
	default:
		return -1;
	}

	if (MI_VENC_SetChnAttr(chn, &attr) != 0)
		return -1;

	printf("[dual] SD backpressure: bitrate %u -> %u kbps\n",
		*current_kbps, new_kbps);
	*current_kbps = new_kbps;
	return 0;
}

/* Recording thread: drains ch1 frames at full speed so the main loop
 * is never blocked by TS mux + SD write.  Follows the audio thread
 * pattern (volatile running flag + pthread_join on stop).
 *
 * Adaptive bitrate: if the SD card can't keep up, the thread detects
 * backpressure (frames queuing faster than written) and reduces ch1
 * bitrate by 10% per second until stabilized.  Once reduced, the
 * bitrate stays at the lower level for the rest of the session. */
static void *dual_rec_thread_fn(void *arg)
{
	Star6eDualVenc *d = arg;
	uint32_t current_kbps = d->bitrate;
	uint32_t min_kbps = d->bitrate / 4;
	if (min_kbps < 1000) min_kbps = 1000;  /* floor at 25%, min 1 Mbps */
	struct timespec interval_start;
	unsigned int behind_count = 0;
	unsigned int total_count = 0;
	unsigned int pressure_seconds = 0;

	clock_gettime(CLOCK_MONOTONIC, &interval_start);

	while (d->rec_running) {
		MI_VENC_Stat_t stat = {0};
		MI_VENC_Stream_t stream = {0};
		int ret;

		ret = MI_VENC_Query(d->channel, &stat);
		if (ret != 0 || stat.curPacks == 0) {
			usleep(1000);
			continue;
		}

		stream.count = stat.curPacks;
		stream.packet = ensure_packs(&d->stream_packs,
			&d->stream_packs_cap, stat.curPacks);
		if (!stream.packet) {
			usleep(1000);
			continue;
		}

		ret = MI_VENC_GetStream(d->channel, &stream,
			g_running ? 40 : 0);
		if (ret != 0) {
			if (ret == -EAGAIN || ret == EAGAIN)
				usleep(1000);
			continue;
		}

			/* Skip slow SD writes during shutdown — keep draining
			 * to prevent VPE backpressure while pipeline tears down. */
			if (g_running) {
				if (d->is_dual_stream) {
					(void)star6e_video_send_frame(&d->video,
						&d->output, &stream, 1, 0, NULL);
				} else if (d->ts_recorder) {
					star6e_ts_recorder_write_stream(
						d->ts_recorder, &stream);
				}
			}

		MI_VENC_ReleaseStream(d->channel, &stream);
		total_count++;

		/* Peek: is another frame already waiting?  If so, we're
		 * not keeping up — the write took longer than 1/fps. */
		{
			MI_VENC_Stat_t peek = {0};
			if (MI_VENC_Query(d->channel, &peek) == 0 &&
			    peek.curPacks > 0)
				behind_count++;
		}

		/* Every second: evaluate backpressure */
		{
			struct timespec now;
			long long elapsed_ms;

			clock_gettime(CLOCK_MONOTONIC, &now);
			elapsed_ms = (long long)(now.tv_sec - interval_start.tv_sec) * 1000LL +
				(long long)(now.tv_nsec - interval_start.tv_nsec) / 1000000LL;

			if (elapsed_ms >= 1000) {
				/* Sustained pressure: >80% of frames had
				 * another waiting behind them.  Transient
				 * peaks (single slow write) won't trigger
				 * because most frames will have empty queue. */
				if (total_count > 0 &&
				    behind_count > total_count * 4 / 5) {
					pressure_seconds++;
					if (pressure_seconds >= 3) {
						dual_rec_reduce_bitrate(
							d->channel,
							&current_kbps,
							min_kbps);
						pressure_seconds = 0;
					}
				} else {
					pressure_seconds = 0;
				}

				behind_count = 0;
				total_count = 0;
				interval_start = now;
			}
		}
	}

	return NULL;
}

static void dual_rec_thread_start(Star6eDualVenc *d)
{
	d->rec_running = 1;
	if (pthread_create(&d->rec_thread, NULL, dual_rec_thread_fn, d) != 0) {
		fprintf(stderr, "[dual] ERROR: pthread_create failed for recording thread\n");
		d->rec_running = 0;
		return;
	}
	d->rec_started = 1;
	printf("> Dual recording thread started (mode: %s)\n", d->mode);
}

static int star6e_runtime_apply_startup_controls(Star6eRunnerContext *ctx)
{
	Star6ePipelineState *ps = &ctx->ps;
	VencConfig *vcfg = &ctx->vcfg;

	g_runner_ctx = ctx;
	star6e_controls_bind(ps, vcfg);
	star6e_iq_init();
	venc_api_register(vcfg, "star6e", star6e_controls_callbacks());
	venc_api_set_record_status_fn(record_status_callback);

	scene_init(&ctx->scene, ctx->vcfg.video0.scene_threshold,
		ctx->vcfg.video0.scene_holdoff);

	if (!vcfg->isp.legacy_ae)
		start_custom_ae(ps, vcfg);

	if (vcfg->fpv.roi_enabled) {
		star6e_controls_apply_roi_qp(vcfg->fpv.roi_qp);
	}
	if (vcfg->video0.qp_delta != 0) {
		star6e_controls_apply_qp_delta(vcfg->video0.qp_delta);
	}

	if (!ps->output_enabled) {
		ps->stored_fps = vcfg->video0.fps;
		star6e_controls_apply_fps(STAR6E_CONTROLS_IDLE_FPS);
		printf("> Output disabled at startup, idling at %u fps\n",
			STAR6E_CONTROLS_IDLE_FPS);
	}

	star6e_recorder_init(&ps->recorder);
	audio_ring_init(&ps->audio_ring);
	star6e_ts_recorder_init(&ps->ts_recorder,
		vcfg->audio.enabled ? vcfg->audio.sample_rate : 0,
		vcfg->audio.enabled ? (uint8_t)vcfg->audio.channels : 0);
	ps->ts_recorder.request_idr = runtime_request_idr;
	if (vcfg->record.max_seconds > 0)
		ps->ts_recorder.max_seconds = vcfg->record.max_seconds;
	if (vcfg->record.max_mb > 0)
		ps->ts_recorder.max_bytes = (uint64_t)vcfg->record.max_mb * 1024 * 1024;

	/* Start dual VENC if mode is "dual" or "dual-stream" */
	if (vcfg->record.enabled &&
	    (strcmp(vcfg->record.mode, "dual") == 0 ||
	     strcmp(vcfg->record.mode, "dual-stream") == 0)) {
		star6e_pipeline_start_dual(ps,
			vcfg->record.bitrate, vcfg->record.fps,
			vcfg->record.gop_size, vcfg->record.mode,
			vcfg->record.server[0] ? vcfg->record.server : "",
			vcfg->video0.frame_lost);

		/* For dual-stream: init second RTP output */
		if (ps->dual && strcmp(vcfg->record.mode, "dual-stream") == 0 &&
		    ps->dual->server[0]) {
			Star6eOutputSetup ds_setup;
			if (star6e_output_prepare(&ds_setup, ps->dual->server,
			    vcfg->outgoing.stream_mode,
			    vcfg->outgoing.max_payload_size,
			    vcfg->outgoing.connected_udp) == 0 &&
			    star6e_output_init(&ps->dual->output, &ds_setup) == 0) {
				star6e_video_init(&ps->dual->video, vcfg,
					ps->sensor.mode.maxFps,
					&ps->dual->output);
				printf("> Dual-stream: ch1 → %s\n",
					ps->dual->server);
			}
		}

		/* Launch recording thread for ch1 frame draining */
		if (ps->dual) {
			ps->dual->is_dual_stream =
				(strcmp(vcfg->record.mode, "dual-stream") == 0);
			if (!ps->dual->is_dual_stream)
				ps->dual->ts_recorder = &ps->ts_recorder;
			dual_rec_thread_start(ps->dual);
			venc_api_dual_register(ps->dual->channel,
				ps->dual->bitrate, ps->dual->fps,
				ps->dual->gop, vcfg->video0.frame_lost);
		}
	}

	/* Start recording (mirror or dual mode, not dual-stream) */
	if (vcfg->record.enabled &&
	    strcmp(vcfg->record.mode, "dual-stream") != 0 &&
	    strcmp(vcfg->record.mode, "off") != 0 &&
	    vcfg->record.dir[0]) {
		if (strcmp(vcfg->record.format, "hevc") == 0) {
			star6e_recorder_start(&ps->recorder, vcfg->record.dir);
		} else {
			if (vcfg->audio.enabled)
				ps->audio.rec_ring = &ps->audio_ring;
			star6e_ts_recorder_start(&ps->ts_recorder,
				vcfg->record.dir,
				vcfg->audio.enabled ? &ps->audio_ring : NULL);
		}
	}

	return 0;
}

static int star6e_runtime_restart_pipeline(Star6eRunnerContext *ctx,
	int reinit_mode)
{
	Star6ePipelineState *ps = &ctx->ps;
	VencConfig *vcfg = &ctx->vcfg;
	int ret;

	uint32_t prev_max_fps = ps->sensor.mode.maxFps;

	star6e_cus3a_request_stop();

	star6e_controls_reset();
	star6e_pipeline_cus3a_reset();

	star6e_cus3a_join();

	star6e_recorder_stop(&ps->recorder);
	star6e_ts_recorder_stop(&ps->ts_recorder);
	ps->audio.rec_ring = NULL;

	if (reinit_mode == 1 || reinit_mode == 3) {
		/* Both /api/v1/restart and /api/v1/restart_full reload from disk
		 * so callers can write /etc/venc.json (e.g. via json_cli) and
		 * have the new values applied without going through /api/v1/set. */
		venc_config_defaults(vcfg);
		if (venc_config_load(VENC_CONFIG_DEFAULT_PATH, vcfg) != 0) {
			fprintf(stderr,
				"ERROR: config reload failed, shutting down\n");
			return 1;
		}
	}

	/* Clamp FPS to current sensor mode (modes 1/2 only — they keep the
	 * existing sensor running). Mode 3 re-runs sensor_select against the
	 * requested fps, so don't clamp away the user's request. */
	if (reinit_mode != 3 && prev_max_fps > 0 && vcfg->video0.fps > prev_max_fps) {
		printf("> Reinit: clamping FPS %u -> %u "
			"(limited by current sensor mode)\n",
			vcfg->video0.fps, prev_max_fps);
		vcfg->video0.fps = prev_max_fps;
	}

	/* Mode 3 (full restart): tear down the entire pipeline and re-run
	 * sensor_select. Heavier than the partial reinit below — multi-second
	 * blackout — but the only in-process path that picks a different sensor
	 * mode. Note: the SigmaStar MIPI PHY does not always recover from
	 * MI_SNR_Disable/Enable cycles. If callers report "encoder stalled after
	 * /api/v1/restart_full", that's the cause — see HTTP_API_CONTRACT.md.
	 *
	 * Modes 1 and 2 (partial reinit): keep sensor/VIF/VPE running, only
	 * rebuild VENC. Avoids the MIPI PHY issue but cannot change sensor mode. */
	if (reinit_mode == 3) {
		star6e_pipeline_stop(ps);
		ctx->pipeline_started = 0;
		ret = star6e_pipeline_start(ps, vcfg, &g_sdk_quiet);
		if (ret != 0) {
			fprintf(stderr, "ERROR: pipeline full restart failed (%d), shutting down\n",
				ret);
			return ret;
		}
		ctx->pipeline_started = 1;
	} else {
		ret = star6e_pipeline_reinit(ps, vcfg, &g_sdk_quiet);
		if (ret != 0) {
			fprintf(stderr, "ERROR: pipeline reinit failed (%d), shutting down\n",
				ret);
			ctx->pipeline_started = 0;
			return ret;
		}
	}

	star6e_controls_bind(ps, vcfg);
	install_signal_handlers();

	scene_init(&ctx->scene, ctx->vcfg.video0.scene_threshold,
		ctx->vcfg.video0.scene_holdoff);

	if (!vcfg->isp.legacy_ae)
		start_custom_ae(ps, vcfg);

	if (vcfg->fpv.roi_enabled) {
		star6e_controls_apply_roi_qp(vcfg->fpv.roi_qp);
	}
	if (vcfg->video0.qp_delta != 0) {
		star6e_controls_apply_qp_delta(vcfg->video0.qp_delta);
	}

	if (!ps->output_enabled) {
		ps->stored_fps = vcfg->video0.fps;
		star6e_controls_apply_fps(STAR6E_CONTROLS_IDLE_FPS);
	}

	return 0;
}

static int star6e_runtime_handle_reinit(Star6eRunnerContext *ctx,
	struct timespec *cus3a_ts_last, unsigned int *idle_counter,
	int *handled)
{
	int reinit_mode;
	int updated;
	int ret;

	*handled = 0;

	reinit_mode = venc_api_get_reinit();
	if (!reinit_mode) {
		return 0;
	}
	*handled = 1;

	usleep(200000);
	updated = venc_api_get_reinit();
	if (updated) {
		reinit_mode = updated;
	}
	venc_api_clear_reinit();

	const char *mode_desc;
	switch (reinit_mode) {
	case 1:  mode_desc = "reload config from disk (partial reinit)"; break;
	case 2:  mode_desc = "apply in-memory config (partial reinit)"; break;
	case 3:  mode_desc = "full pipeline restart with sensor_select"; break;
	default: mode_desc = "unknown"; break;
	}
	printf("> Reinit requested (mode %d: %s)\n", reinit_mode, mode_desc);

	ret = star6e_runtime_restart_pipeline(ctx, reinit_mode);
	if (ret != 0) {
		return ret;
	}

	clock_gettime(CLOCK_MONOTONIC, cus3a_ts_last);
	memset(&g_imu_eis_verbose_last, 0, sizeof(g_imu_eis_verbose_last));
	*idle_counter = 0;

	printf("> Pipeline reinit complete\n");
	fflush(stdout);
	return 0;
}

static int star6e_runtime_process_stream(Star6eRunnerContext *ctx,
	struct timespec *cus3a_ts_last, unsigned int *idle_counter)
{
	Star6ePipelineState *ps = &ctx->ps;
	VencConfig *vcfg = &ctx->vcfg;
	MI_VENC_Stat_t stat = {0};
	MI_VENC_Stream_t stream = {0};
	int ret;

	ret = MI_VENC_Query(ps->venc_channel, &stat);
	if (ret != 0) {
		if ((++(*idle_counter) % 60) == 0) {
			printf("MI_VENC_Query failed %d\n", ret);
			fflush(stdout);
		}
		star6e_pipeline_cus3a_tick(&g_sdk_quiet, cus3a_ts_last);
		idle_wait(&ps->video.sidecar, 5);
		return 0;
	}

	if (stat.curPacks == 0) {
		if ((++(*idle_counter) % 120) == 0) {
			printf("waiting for encoder data...\n");
			fflush(stdout);
		}
		star6e_pipeline_cus3a_tick(&g_sdk_quiet, cus3a_ts_last);
		idle_wait(&ps->video.sidecar, 1);
		return 0;
	}
	*idle_counter = 0;

	stream.count = stat.curPacks;
	stream.packet = ensure_packs(&ps->stream_packs,
		&ps->stream_packs_cap, stat.curPacks);
	if (!stream.packet) {
		fprintf(stderr, "ERROR: Unable to allocate stream packets\n");
		return -1;
	}

	/* Drain IMU FIFO and update EIS crop BEFORE GetStream so the
	 * new crop position is latched by VPE for the frame currently
	 * being captured, reducing stabilization latency by one frame. */
	if (ps->imu)
		imu_drain(ps->imu);
	if (ps->eis)
		eis_update(ps->eis);

	ret = MI_VENC_GetStream(ps->venc_channel, &stream, 40);
	if (ret != 0) {
		if (ret == -EAGAIN || ret == EAGAIN) {
			idle_wait(&ps->video.sidecar, 2);
			return 0;
		}
		fprintf(stderr, "ERROR: MI_VENC_GetStream failed %d\n", ret);
		return ret;
	}

	{
		RtpSidecarEncInfo enc_info;
		int codec = (strcmp(vcfg->video0.codec, "h264") == 0) ? 0 : 1;

		scene_update(&ctx->scene, &stream, codec, ps->venc_channel);
		scene_fill_sidecar(&ctx->scene, &enc_info);

		(void)star6e_video_send_frame(&ps->video, &ps->output, &stream,
			ps->output_enabled, vcfg->system.verbose, &enc_info);
	}

	/* In dual/dual-stream mode, ch1 handles recording (see below).
	 * In mirror/off mode, ch0 feeds the recorder directly. */
	if (!ps->dual) {
		star6e_recorder_write_frame(&ps->recorder, &stream);
		star6e_ts_recorder_write_stream(&ps->ts_recorder, &stream);
	}

	/* Check HTTP record control flags.
	 * In dual mode, the recording thread owns the ts_recorder
	 * exclusively — skip HTTP record start/stop to prevent races. */
	if (!ps->dual) {
		char rec_dir[256];
		if (venc_api_get_record_start(rec_dir, sizeof(rec_dir))) {
			/* Stop any active recording first */
			star6e_recorder_stop(&ps->recorder);
			star6e_ts_recorder_stop(&ps->ts_recorder);
			ps->audio.rec_ring = NULL;
			if (strcmp(vcfg->record.format, "hevc") == 0) {
				star6e_recorder_start(&ps->recorder, rec_dir);
			} else {
				if (vcfg->audio.enabled)
					ps->audio.rec_ring = &ps->audio_ring;
				star6e_ts_recorder_start(&ps->ts_recorder,
					rec_dir,
					vcfg->audio.enabled ? &ps->audio_ring : NULL);
			}
			/* Request IDR so the recording starts with a keyframe */
			runtime_request_idr();
		}
		if (venc_api_get_record_stop()) {
			star6e_recorder_stop(&ps->recorder);
			star6e_ts_recorder_stop(&ps->ts_recorder);
			ps->audio.rec_ring = NULL;
		}
	}

	if (vcfg->system.verbose && (ps->imu || ps->eis)) {
		struct timespec imu_eis_now;
		clock_gettime(CLOCK_MONOTONIC, &imu_eis_now);
		long long elapsed_ms =
			((long long)(imu_eis_now.tv_sec - g_imu_eis_verbose_last.tv_sec) * 1000LL) +
			((long long)(imu_eis_now.tv_nsec - g_imu_eis_verbose_last.tv_nsec) / 1000000LL);
		if (elapsed_ms >= 1000) {
			if (ps->imu) {
				ImuStats ist;
				imu_get_stats(ps->imu, &ist);
				printf("[imu] samples=%lu gyro=(%.3f,%.3f,%.3f)\n",
					(unsigned long)ist.samples_read,
					ist.last_gyro_x, ist.last_gyro_y, ist.last_gyro_z);
			}
			if (ps->eis) {
				EisStatus est;
				eis_get_status(ps->eis, &est);
				printf("[eis] crop(%u,%u) off(%.1f,%.1f) n=%u ring=%u\n",
					est.crop_x, est.crop_y,
					est.offset_x, est.offset_y,
					est.last_n_samples, est.ring_count);
			}
			fflush(stdout);
			g_imu_eis_verbose_last = imu_eis_now;
		}
	}

	/* Debug OSD overlay */
	if (ps->debug_osd) {
		static unsigned int osd_prev_frame;
		static struct timespec osd_prev_ts;
		static unsigned int osd_fps;
		struct timespec osd_now;

		debug_osd_begin_frame(ps->debug_osd);
		debug_osd_sample_cpu(ps->debug_osd);

		/* Compute fps from frame counter delta */
		clock_gettime(CLOCK_MONOTONIC, &osd_now);
		long osd_ms = (osd_now.tv_sec - osd_prev_ts.tv_sec) * 1000 +
			(osd_now.tv_nsec - osd_prev_ts.tv_nsec) / 1000000;
		if (osd_ms >= 1000) {
			unsigned int df = ps->video.frame_counter - osd_prev_frame;
			osd_fps = (unsigned int)(df * 1000 / (unsigned long)osd_ms);
			osd_prev_frame = ps->video.frame_counter;
			osd_prev_ts = osd_now;
		}

		debug_osd_text(ps->debug_osd, 0, "fps", "%u", osd_fps);
		debug_osd_text(ps->debug_osd, 1, "cpu", "%d%%",
			debug_osd_get_cpu(ps->debug_osd));

		if (ps->eis) {
			EisStatus est;
			eis_get_status(ps->eis, &est);

			/* 1/3 scale miniature in bottom-right corner */
			uint16_t iw = (uint16_t)(ps->image_width / 3);
			uint16_t ih = (uint16_t)(ps->image_height / 3);
			uint16_t ox = (uint16_t)(ps->image_width - iw - 8);
			uint16_t oy = (uint16_t)(ps->image_height - ih - 8);

			/* Outer: full sensor area */
			debug_osd_rect(ps->debug_osd, ox, oy, iw, ih,
				DEBUG_OSD_WHITE, 0);
			/* Middle: margin boundary */
			debug_osd_rect(ps->debug_osd,
				(uint16_t)(ox + est.margin_x / 3),
				(uint16_t)(oy + est.margin_y / 3),
				(uint16_t)(iw - 2 * est.margin_x / 3),
				(uint16_t)(ih - 2 * est.margin_y / 3),
				DEBUG_OSD_YELLOW, 0);
			/* Inner: current crop window */
			debug_osd_rect(ps->debug_osd,
				(uint16_t)(ox + est.crop_x / 3),
				(uint16_t)(oy + est.crop_y / 3),
				(uint16_t)(est.crop_w / 3),
				(uint16_t)(est.crop_h / 3),
				DEBUG_OSD_SEMITRANS_GREEN, 1);

			debug_osd_text(ps->debug_osd, 3, "crop", "%u,%u",
				est.crop_x, est.crop_y);
			debug_osd_text(ps->debug_osd, 4, "off", "%.1f,%.1f",
				est.offset_x, est.offset_y);
			debug_osd_text(ps->debug_osd, 5, "margin", "%ux%u",
				est.margin_x, est.margin_y);
		}

		debug_osd_end_frame(ps->debug_osd);
	}

	MI_VENC_ReleaseStream(ps->venc_channel, &stream);

	/* ch1 frames are now drained by the dedicated recording thread
	 * (dual_rec_thread_fn) — no polling needed here. */

	star6e_pipeline_cus3a_tick(&g_sdk_quiet, cus3a_ts_last);
	return 0;
}

static int star6e_prepare(void *opaque)
{
	(void)opaque;
	g_running = 1;
	g_signal_count = 0;
	install_signal_handlers();

	sdk_quiet_state_init(&g_sdk_quiet);
	star6e_controls_reset();
	return 0;
}

static int star6e_runner_init(void *opaque)
{
	Star6eRunnerContext *ctx = opaque;
	int ret;

	if (star6e_mi_init() != 0) {
		fprintf(stderr, "ERROR: MI library load failed\n");
		return -1;
	}

	sdk_quiet_begin(&g_sdk_quiet);
	ret = MI_SYS_Init();
	sdk_quiet_end(&g_sdk_quiet);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_SYS_Init failed %d\n", ret);
		star6e_mi_deinit();
		return ret;
	}
	ctx->system_initialized = 1;

	venc_httpd_start(ctx->vcfg.system.web_port);
	ctx->httpd_started = 1;

	ret = star6e_pipeline_start(&ctx->ps, &ctx->vcfg, &g_sdk_quiet);
	if (ret != 0) {
		return ret;
	}
	ctx->pipeline_started = 1;

	ret = star6e_runtime_apply_startup_controls(ctx);
	if (ret != 0)
		return ret;
	install_signal_handlers();
	return 0;
}

static int star6e_runner_run(void *opaque)
{
	Star6eRunnerContext *ctx = opaque;
	struct timespec cus3a_ts_last = {0};
	unsigned int idle_counter = 0;
	int handled;
	int ret;

	clock_gettime(CLOCK_MONOTONIC, &cus3a_ts_last);

	/* Pin encoder to CPU 0 with minimum RT priority.  Reduces
	 * scheduling jitter from ISP/audio/httpd threads.  Silent
	 * fallback if unprivileged or single-core. */
	{
		unsigned long mask = 1UL;  /* CPU 0 */
		syscall(__NR_sched_setaffinity, 0, sizeof(mask), &mask);

		struct sched_param sp;
		sp.sched_priority = 1;
		if (pthread_setschedparam(pthread_self(), SCHED_FIFO,
		    &sp) != 0)
			printf("> note: RT priority not available"
				" (run as root)\n");
	}

	while (g_running) {
		ret = star6e_runtime_handle_reinit(ctx, &cus3a_ts_last,
			&idle_counter, &handled);
		if (ret != 0) {
			return ret;
		}
		if (handled) {
			continue;
		}

		ret = star6e_runtime_process_stream(ctx, &cus3a_ts_last,
			&idle_counter);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static void star6e_runner_teardown(void *opaque)
{
	Star6eRunnerContext *ctx = opaque;

	/* Fork a watchdog child that will force-kill us if teardown hangs.
	 * Unlike SIGALRM + _exit(), a child's kill -9 works even if the
	 * parent is in D-state on a kernel-side VPE flush — the kernel
	 * delivers SIGKILL to the parent process, which tears down all
	 * threads and releases driver resources from a clean context.
	 *
	 * If even SIGKILL can't recover (driver holds an uninterruptible
	 * lock), the child triggers sysrq-b (emergency reboot) as a
	 * last resort to prevent an indefinitely hung system. */
	{
		pid_t watchdog = fork();
		if (watchdog == 0) {
			/* Close inherited stdout — it may be a pipe from the
			 * audio stdout filter.  Keeping it open prevents the
			 * filter thread's read() from seeing EOF, which
			 * deadlocks pthread_join in audio teardown. */
			close(STDOUT_FILENO);
			/* Child: poll parent liveness, escalate if stuck.
			 * Check every second — exit early if parent dies
			 * normally so we don't linger as an orphan. */
			pid_t parent = getppid();
			int i;
			for (i = 0; i < 8; i++) {
				sleep(1);
				if (kill(parent, 0) != 0)
					_exit(0);  /* parent exited cleanly */
			}
			if (kill(parent, 0) == 0) {
				static const char m1[] =
					"[watchdog] teardown hung, kill -9\n";
				(void)write(STDERR_FILENO, m1, sizeof(m1) - 1);
				kill(parent, SIGKILL);
				sleep(3);
				if (kill(parent, 0) == 0) {
					static const char m2[] =
						"[watchdog] D-state, sysrq reboot\n";
					(void)write(STDERR_FILENO, m2,
						sizeof(m2) - 1);
					int fd = open("/proc/sysrq-trigger",
						O_WRONLY);
					if (fd >= 0) {
						(void)write(fd, "b", 1);
						close(fd);
					}
				}
			}
			_exit(0);
		}
		/* Parent: ignore watchdog errors, continue teardown */
	}
	alarm(0);  /* cancel SIGALRM — watchdog replaces it */

	star6e_cus3a_request_stop();

	/* Pipeline stop MUST happen before recorder stop.  The recording
	 * thread runs inside pipeline_stop() and needs the ts_recorder fd
	 * open until StopRecvPic completes.  The thread skips SD writes
	 * when g_running==0 (already set by the signal handler). */
	if (ctx->pipeline_started) {
		star6e_iq_cleanup();
		star6e_controls_reset();
		star6e_pipeline_stop(&ctx->ps);
		ctx->pipeline_started = 0;
	}

	/* Now safe to join the 3A thread — pipeline is stopped so ISP
	 * calls will return errors and the thread will exit. */
	star6e_cus3a_join();

	/* Safe to close files now — recording thread has been joined
	 * inside pipeline_stop(). */
	star6e_recorder_stop(&ctx->ps.recorder);
	star6e_ts_recorder_stop(&ctx->ps.ts_recorder);
	audio_ring_destroy(&ctx->ps.audio_ring);
	if (ctx->httpd_started) {
		venc_httpd_stop();
		ctx->httpd_started = 0;
	}
	if (ctx->system_initialized) {
		MI_SYS_Exit();
		ctx->system_initialized = 0;
		star6e_pipeline_vpe_scl_preset_shutdown();
	}
	star6e_mi_deinit();
}

static VencConfig *star6e_config(void *opaque)
{
	Star6eRunnerContext *ctx = opaque;

	return &ctx->vcfg;
}

static int star6e_map_pipeline_result(int result)
{
	return result;
}

static const BackendOps g_backend_ops = {
	.name = "star6e",
	.config_path = VENC_CONFIG_DEFAULT_PATH,
	.context_size = sizeof(Star6eRunnerContext),
	.config = star6e_config,
	.prepare = star6e_prepare,
	.init = star6e_runner_init,
	.run = star6e_runner_run,
	.teardown = star6e_runner_teardown,
	.map_pipeline_result = star6e_map_pipeline_result,
};

const BackendOps *star6e_runtime_backend_ops(void)
{
	return &g_backend_ops;
}
