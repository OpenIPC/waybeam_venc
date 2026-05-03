#include "star6e_runtime.h"

#include "debug_osd.h"
#include "idr_rate_limit.h"
#include "imu_bmi270.h"
#include "pipeline_common.h"
#include "scene_detector.h"
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
#include <sys/prctl.h>
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
static struct timespec g_imu_verbose_last = {0};

/* ── Scene-detector stream decoders ───────────────────────────────────── */

static uint32_t star6e_scene_frame_size(const MI_VENC_Stream_t *s)
{
	uint32_t t = 0;
	unsigned int i;
	if (!s || !s->packet) return 0;
	for (i = 0; i < s->count; i++) t += s->packet[i].length;
	return t;
}

static uint8_t star6e_scene_is_idr(const MI_VENC_Stream_t *s, int codec)
{
	unsigned int i;
	if (!s || !s->packet) return 0;
	for (i = 0; i < s->count; i++) {
		const MI_VENC_Pack_t *p = &s->packet[i];
		unsigned int k, n = p->packNum > 8 ? 8 : p->packNum;
		if (n > 0) {
			for (k = 0; k < n; k++) {
				if (codec == 0 && p->packetInfo[k].packType.h264Nalu == 5)
					return 1;
				if (codec != 0 && p->packetInfo[k].packType.h265Nalu == 19)
					return 1;
			}
		} else {
			if (codec == 0 && p->naluType.h264Nalu == 5) return 1;
			if (codec != 0 && p->naluType.h265Nalu == 19) return 1;
		}
	}
	return 0;
}

static void star6e_scene_request_idr(void *ctx)
{
	int venc_chn = *(const int *)ctx;
	if (idr_rate_limit_allow(venc_chn))
		MI_VENC_RequestIdr(venc_chn, 1);
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

		venc_api_request_reinit();
		(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
		return;
	}

	g_running = 0;
	g_signal_count++;

	if (g_signal_count == 1) {
		static const char msg[] =
			"\n> Interrupt received, shutting down...\n";

		(void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
		alarm(2);
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
	int chn;

	if (!g_runner_ctx)
		return -1;

	chn = g_runner_ctx->ps.venc_channel;
	if (!idr_rate_limit_allow(chn))
		return 0;  /* coalesced — not an error */
	return MI_VENC_RequestIdr(chn, 1) == 0 ? 0 : -1;
}

static void start_custom_ae(const Star6ePipelineState *ps,
	const VencConfig *vcfg)
{
	Star6eCus3aConfig ae_cfg;

	star6e_cus3a_config_defaults(&ae_cfg);
	ae_cfg.sensor_fps = ps->sensor.fps;
	if (vcfg->isp.ae_fps > 0)
		ae_cfg.ae_fps = vcfg->isp.ae_fps;
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

	/* Block on MI_VENC_GetFd(chn) via poll() instead of spinning on
	 * MI_VENC_Query + usleep(1000).  The fd signals POLLIN when a
	 * frame is ready, so we wake exactly once per frame (~120/s at
	 * 120 fps) instead of ~1000/s from the old 1 ms spin.  If the
	 * SDK returns fd < 0 (unknown BSP variant), fall back to the
	 * original polling loop. */
	int venc_fd = MI_VENC_GetFd(d->channel);

	while (d->rec_running) {
		MI_VENC_Stat_t stat = {0};
		MI_VENC_Stream_t stream = {0};
		int ret;

		if (venc_fd >= 0) {
			/* POLL_TIMEOUT_MS = 1000 is large on purpose — it
			 * caps the rec_running cancellation latency
			 * without wasting cycles on short periodic wakes.
			 * Encoder frames arrive every 8-9 ms at 120 fps,
			 * long before this timeout expires. */
			struct pollfd pfd = { .fd = venc_fd, .events = POLLIN };
			(void)poll(&pfd, 1, 1000);
			/* POLLERR/POLLHUP/POLLNVAL: the SDK closed the fd
			 * under us (BSP quirk, pipeline reinit, VPE unbind).
			 * Fall back to the Query+usleep path for the rest
			 * of the thread's lifetime — don't busy-loop on a
			 * dead fd. */
			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				MI_VENC_CloseFd(d->channel);
				venc_fd = -1;
				usleep(1000);
				continue;
			}
			if (!(pfd.revents & POLLIN))
				continue;  /* timeout / spurious wake */
		}

		ret = MI_VENC_Query(d->channel, &stat);
		if (ret != 0 || stat.curPacks == 0) {
			/* Always sleep before retry: even with venc_fd >= 0,
			 * a spurious POLLIN that's not matched by an actual
			 * Query packet (rare BSP edge case) would otherwise
			 * busy-loop.  100us keeps wakeup latency low while
			 * preventing a runaway spin. */
			usleep(venc_fd >= 0 ? 100 : 1000);
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
			/* EAGAIN on either path: sleep briefly before
			 * retrying so we don't spin if Query said
			 * stat.curPacks>0 but GetStream keeps contending. */
			if (ret == -EAGAIN || ret == EAGAIN)
				usleep(1000);
			continue;
		}

			/* Skip slow SD writes during shutdown — keep draining
			 * to prevent VPE backpressure while pipeline tears down. */
			if (g_running) {
				if (d->is_dual_stream) {
					/* Dual-stream ch1 has no sidecar of its own;
					 * pressure observation is purely a sidecar-
					 * trailer signal so skip it here.  The send
					 * always runs — see HISTORY 0.9.2 for why
					 * post-encode skip is the wrong adaptation
					 * knob for inter-frame-coded video. */
					(void)star6e_video_send_frame(&d->video,
						&d->output, &stream, 1, 0, NULL);
				} else if (d->ts_recorder) {
					star6e_ts_recorder_write_stream(
						d->ts_recorder, &stream);
				}
			}

		MI_VENC_ReleaseStream(d->channel, &stream);
		total_count++;

		/* Backpressure signal: the pre-GetStream Query found >= 2
		 * packets queued, meaning the encoder produced another frame
		 * before we consumed the prior one.  Equivalent semantics to
		 * a post-ReleaseStream peek but avoids one MI_VENC_Query
		 * syscall per recorded frame (~120/s at 120 fps). */
		if (stat.curPacks >= 2)
			behind_count++;

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

	if (venc_fd >= 0)
		MI_VENC_CloseFd(d->channel);

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
	venc_api_set_config_path(VENC_CONFIG_DEFAULT_PATH);
	venc_api_set_record_status_fn(record_status_callback);
	venc_api_set_record_http_control_supported(true);

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

/* In-process MI_SYS_Exit + MI_SYS_Init does NOT yield a clean kernel
 * state on Star6E — the SigmaStar driver retains "already_inited" flags
 * tied to the PID, so a second MI_SYS_Init in the same process trips
 * MI_DEVICE_Open hangs and VIF "layout type 2 bindmode 4 not sync err"
 * after one or two cycles.  Empirically verified on imx335 @ 192.168.1.13.
 *
 * The only reliable cold restart is a *new PID*.  Approach: handle_reinit
 * sets a flag; main exits cleanly via the normal teardown path; the
 * backend's last act before main returns is to fork a child, parent
 * exits, child execv's a fresh venc.  Forking AFTER teardown is critical
 * — if we forked before, the child would inherit MI device fds and the
 * kernel's per-pid cleanup wouldn't fully fire when the parent calls
 * MI_SYS_Exit.  Bench-validated against 12 consecutive cross-mode sensor
 * SIGHUPs (rounds 0→1→2→3 ×3) with no degradation. */

static int g_respawn_after_exit = 0;

void star6e_runtime_respawn_after_exit(void)
{
	/* Capture our PID BEFORE fork.  The child cannot use getppid()
	 * to learn it: by the time the child first runs, the parent may
	 * already have exited (we are about to return into main, which
	 * exits immediately), so the child gets reparented to init/
	 * subreaper and getppid() returns 1.  COW inheritance carries
	 * this static across fork verbatim. */
	pid_t parent_pid = getpid();
	pid_t child = fork();

	if (child < 0) {
		fprintf(stderr,
			"ERROR: fork() for respawn failed: %s — process exiting\n",
			strerror(errno));
		return;
	}

	if (child > 0) {
		fprintf(stderr,
			"> Respawning: parent %d → child %d\n",
			(int)getpid(), (int)child);
		return;
	}

	(void)prctl(PR_SET_NAME, VENC_COMM_RESPAWN, 0, 0, 0);

	/* Wait for the parent to ACTUALLY exit before we exec.  The previous
	 * heuristic (getppid() != 1) is unreliable under procd / systemd /
	 * any process that calls PR_SET_CHILD_SUBREAPER: when the parent
	 * dies, we reparent to the subreaper instead of init, so getppid()
	 * never reaches 1 and the loop times out while the parent is still
	 * alive — the new image then runs while the old SDK state is
	 * still owned by the dying parent.
	 *
	 * Polling kill(parent_pid, 0) for ESRCH is the authoritative test:
	 * it reports the actual death of that specific PID regardless of
	 * who the new parent is.  Cap raised to 30 s because MI_SYS_Exit
	 * can stall in D-state on Star6E during teardown (observed: ~9
	 * load-avg with the pipeline draining).  On timeout, abort the
	 * exec rather than racing live SDK state — silently losing the
	 * SIGHUP is better than two venc's stomping on each other. */
	int waited_ms = 0;
	const int wait_cap_ms = 30 * 1000;
	while (waited_ms < wait_cap_ms) {
		if (kill(parent_pid, 0) == -1 && errno == ESRCH)
			break;
		usleep(100 * 1000);
		waited_ms += 100;
	}
	fprintf(stderr,
		"> Respawn child: parent pid %d gone after %d ms, proceeding to exec\n",
		(int)parent_pid, waited_ms);
	if (waited_ms >= wait_cap_ms) {
		/* stderr is unbuffered and still wired to the parent's log
		 * sink at this point — emit before the dup2 swap so the
		 * message lands in the same /tmp/venc.log tail an operator
		 * will check. */
		fprintf(stderr,
			"ERROR: respawn child timed out after %d s waiting "
			"for parent pid %d to exit — aborting exec to avoid "
			"racing live SDK state.  Parent likely stuck in "
			"MI_SYS teardown; reboot may be required.\n",
			wait_cap_ms / 1000, (int)parent_pid);
		_exit(1);
	}

	sigset_t empty;
	sigemptyset(&empty);
	sigprocmask(SIG_SETMASK, &empty, NULL);

	/* Re-route stdout/stderr to /tmp/venc.log in append mode so the
	 * pre-respawn tail stays available for diagnosis.  If dup2 fails
	 * (e.g. fd table exhaustion from a leaked descriptor we missed in
	 * teardown) bail before execv — running blind would lose any panic
	 * output the fresh venc emits. */
	int log = open(VENC_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (log >= 0) {
		if (dup2(log, STDOUT_FILENO) < 0 ||
		    dup2(log, STDERR_FILENO) < 0)
			_exit(127);
		if (log > 2)
			close(log);
	}

	/* argv is intentionally minimal — main.c currently ignores argv, so
	 * any future flag (e.g. `-c <path>`) added without saving the parent's
	 * argv into a static and replaying it here would silently disappear
	 * across SIGHUP-respawn. */
	char *args[] = { (char *)"venc", NULL };
	execv(VENC_SELF_EXE_PATH, args);
	/* exec failed — fall through and exit so init/operator notices */
	_exit(127);
}

int star6e_runtime_respawn_pending(void)
{
	return g_respawn_after_exit;
}

static int star6e_runtime_handle_reinit(int *handled)
{
	*handled = 0;

	if (!venc_api_get_reinit())
		return 0;
	*handled = 1;
	venc_api_clear_reinit();

	printf("> Reinit requested: cold restart via fork+exec on shutdown\n");
	fflush(stdout);

	/* Mark for respawn after teardown, then exit the run loop.
	 * main() will execute backend->teardown (clean MI_SYS_Exit) then
	 * fork+exec the successor process from a clean state. */
	g_respawn_after_exit = 1;
	g_running = 0;
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

	/* Drain IMU FIFO BEFORE GetStream so any future telemetry/sidecar
	 * consumer sees fresh samples for the frame currently being
	 * captured.  Without EIS (removed in 0.8.0) the drained samples
	 * go to the stub push callback and are discarded — cheap when
	 * imu.enabled=false, as it is by default. */
	if (ps->imu)
		imu_drain(ps->imu);

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
		uint32_t frame_size = star6e_scene_frame_size(&stream);
		uint8_t is_idr = star6e_scene_is_idr(&stream, codec);

		scene_update(&ctx->scene, frame_size, is_idr,
			star6e_scene_request_idr, &ps->venc_channel);
		scene_fill_sidecar(&ctx->scene, &enc_info);

		/* Observe pressure only when a sidecar probe is subscribed
		 * — it is the only consumer of in_pressure / fill_pct / the
		 * pressure_drops counter on the producer hot path.  When no
		 * one is listening, skip the SIOCOUTQ ioctl / ring-fill load
		 * entirely.  Always sending — a producer-side skip would
		 * break the H.265 reference chain (see HISTORY 0.9.2). */
		if (rtp_sidecar_is_subscribed(&ps->video.sidecar))
			star6e_output_observe_pressure(&ps->output);
		(void)star6e_video_send_frame(&ps->video, &ps->output, &stream,
			ps->output_enabled, vcfg->system.verbose, &enc_info);
	}

	/* In dual/dual-stream mode, ch1 handles recording (see below).
	 * In mirror/off mode, ch0 feeds the recorder directly. */
	if (!ps->dual) {
		star6e_recorder_write_frame(&ps->recorder, &stream);
		star6e_ts_recorder_write_stream(&ps->ts_recorder, &stream);
	}

	/* Release the encoder stream as soon as the last consumer of the
	 * stream payload (recorder writes above) is done. Everything
	 * below — HTTP record control, verbose IMU/EIS output, debug OSD —
	 * only reads independent state, so releasing here frees the VENC
	 * output slot for the next frame instead of waiting on blocking
	 * work (stdout printf, OSD draw) that can otherwise push send
	 * spread past a full frame period at 120 fps. */
	MI_VENC_ReleaseStream(ps->venc_channel, &stream);

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

	if (vcfg->system.verbose && ps->imu) {
		struct timespec imu_now;
		clock_gettime(CLOCK_MONOTONIC, &imu_now);
		long long elapsed_ms =
			((long long)(imu_now.tv_sec - g_imu_verbose_last.tv_sec) * 1000LL) +
			((long long)(imu_now.tv_nsec - g_imu_verbose_last.tv_nsec) / 1000000LL);
		if (elapsed_ms >= 1000) {
			ImuStats ist;
			imu_get_stats(ps->imu, &ist);
			printf("[imu] samples=%lu gyro=(%.3f,%.3f,%.3f)\n",
				(unsigned long)ist.samples_read,
				ist.last_gyro_x, ist.last_gyro_y, ist.last_gyro_z);
			fflush(stdout);
			g_imu_verbose_last = imu_now;
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

		debug_osd_end_frame(ps->debug_osd);
	}

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
		ret = star6e_runtime_handle_reinit(&handled);
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
			/* Rename so /proc/<pid>/comm reads "venc-wd" instead of
			 * "venc".  Required for SIGHUP-respawn flow: the new
			 * venc spawned by star6e_runtime_respawn_after_exit()
			 * runs is_another_venc_running() at startup; without
			 * this rename the still-alive watchdog (kept around
			 * to SIGKILL/sysrq-b a hung parent) reads as "venc"
			 * and the respawn aborts with "venc already running". */
			(void)prctl(PR_SET_NAME, VENC_COMM_WATCHDOG, 0, 0, 0);
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
			for (i = 0; i < 6; i++) {
				usleep(500 * 1000);
				if (kill(parent, 0) != 0)
					_exit(0);  /* parent exited cleanly */
			}
			if (kill(parent, 0) == 0) {
				static const char m1[] =
					"[watchdog] teardown hung, kill -9\n";
				(void)write(STDERR_FILENO, m1, sizeof(m1) - 1);
				kill(parent, SIGKILL);
				sleep(1);
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
