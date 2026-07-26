#include "star6e_runtime.h"

#include "attitude_est.h"
#include "audio_codec.h"
#include "debug_osd.h"
#include "detect_wire.h"
#include "idr_rate_limit.h"
#include "imu_bmi270.h"
#include "imu_ring.h"
#include "star6e_framing_host.h"
#include "pipeline_common.h"
#include "scene_detector.h"
#include "sdk_quiet.h"
#include "star6e_controls.h"
#include "star6e_cus3a.h"
#if HAVE_FRAMING_STAB
#include "star6e_framing_stab.h"
#endif
#include "star6e_ipu.h"
#include "star6e_ipu_yolo.h"
#include "star6e_iq.h"
#include "star6e_pipeline.h"
#include "star6e.h"
#include "timing.h"
#include "venc_api.h"
#include "venc_config.h"
#include "venc_httpd.h"
#include "venc_respawn.h"

#include <ctype.h>
#include <dirent.h>
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

/* ── Attitude export (sidecar ATTITUDE trailer) ───────────────────────────
 *
 * The frame loop has just called imu_drain(), so the pipeline's IMU ring
 * holds everything up to "now". Feed the new samples into the
 * complementary filter and snapshot the attitude for this frame's sidecar
 * trailer. File-scope state is fine here: the Star6E respawn path is a
 * full re-exec (fresh process), and only the frame thread touches it. */
static AttitudeEst g_att_est;
static int g_att_inited;
static struct timespec g_att_cursor;   /* last consumed sample timestamp */
static AttitudeAxisMap g_att_map;      /* sensor→camera signed permutation */

/* Live snapshot for /api/v1/attitude (HTTP thread reads, frame thread
 * writes) and the level-trim calibration accumulator (HTTP thread arms,
 * frame thread fills, HTTP thread consumes). */
static pthread_mutex_t g_att_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	int valid, settled;
	float roll_deg, pitch_deg, yaw_deg;
} g_att_snap;
enum { ATT_CAL_IDLE = 0, ATT_CAL_PENDING, ATT_CAL_DONE };
#define ATT_CAL_SAMPLES     256u  /* early-complete target (~1.3 s @200 Hz) */
#define ATT_CAL_MIN_SAMPLES  32u  /* enough to average even at low IMU ODR */
static struct {
	int state;
	float sx, sy, sz;   /* raw sensor-frame accel sums */
	uint32_t n;
} g_att_cal;

static int16_t att_clamp16(int v)
{
	if (v > 32767) return 32767;
	if (v < -32768) return -32768;
	return (int16_t)v;
}

static const RtpSidecarAttitudeInfo *attitude_frame_update(
	const VencConfig *vcfg, RtpSidecarAttitudeInfo *out)
{
	ImuRing *ring = star6e_pipeline_imu_ring();
	struct timespec now;

	if (!ring)
		return NULL;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!g_att_inited) {
		attitude_est_init(&g_att_est, 0.0f);
		if (attitude_axis_map_init(&g_att_map,
		                           vcfg->attitude.axis_fwd,
		                           vcfg->attitude.axis_down,
		                           vcfg->attitude.trim_roll_deg,
		                           vcfg->attitude.trim_pitch_deg) != 0)
			fprintf(stderr, "attitude: invalid axisFwd/axisDown "
			        "(\"%s\"/\"%s\") — using identity\n",
			        vcfg->attitude.axis_fwd,
			        vcfg->attitude.axis_down);
		g_att_cursor = now;
		g_att_inited = 1;
	}

	ImuRingSample s[256];
	uint32_t n = imu_ring_read_range(ring, g_att_cursor, now, s, 256);
	for (uint32_t i = 0; i < n; i++) {
		uint64_t ts_us = (uint64_t)s[i].ts.tv_sec * 1000000ULL +
		                 (uint64_t)s[i].ts.tv_nsec / 1000ULL;
		float g[3], a[3];
		attitude_axis_map_apply(&g_att_map,
			s[i].gyro_x, s[i].gyro_y, s[i].gyro_z, g);
		attitude_axis_map_apply(&g_att_map,
			s[i].accel_x, s[i].accel_y, s[i].accel_z, a);
		attitude_est_update(&g_att_est,
			g[0], g[1], g[2], a[0], a[1], a[2], ts_us);
	}
	if (n > 0) {
		pthread_mutex_lock(&g_att_lock);
		if (g_att_cal.state == ATT_CAL_PENDING) {
			for (uint32_t i = 0; i < n &&
			     g_att_cal.n < ATT_CAL_SAMPLES; i++) {
				g_att_cal.sx += s[i].accel_x;
				g_att_cal.sy += s[i].accel_y;
				g_att_cal.sz += s[i].accel_z;
				g_att_cal.n++;
			}
			if (g_att_cal.n >= ATT_CAL_SAMPLES)
				g_att_cal.state = ATT_CAL_DONE;
		}
		pthread_mutex_unlock(&g_att_lock);
	}
	if (n > 0) {
		/* Advance past the newest consumed sample; read_range is
		 * inclusive and attitude_est_update drops dt<=0 duplicates,
		 * so a 1 ns bump avoids re-feeding the boundary sample. */
		g_att_cursor = s[n - 1].ts;
		g_att_cursor.tv_nsec++;
		if (g_att_cursor.tv_nsec >= 1000000000L) {
			g_att_cursor.tv_nsec = 0;
			g_att_cursor.tv_sec++;
		}
	}

	if (!attitude_est_healthy(&g_att_est))
		return NULL;   /* no gravity reference yet, or state went
		                * non-finite — emit no trailer rather than a
		                * false-valid level attitude */

	int roll = attitude_est_roll_cdeg(&g_att_est);
	int pitch = attitude_est_pitch_cdeg(&g_att_est);
	int yaw = attitude_est_yaw_cdeg(&g_att_est);

	/* Mount rotation about the camera axis, then sign trims. */
	switch (vcfg->attitude.mount_deg) {
	case 90:  { int t = roll; roll = pitch; pitch = -t; } break;
	case 180: roll = -roll; pitch = -pitch; break;
	case 270: { int t = roll; roll = -pitch; pitch = t; } break;
	default: break;
	}
	if (vcfg->attitude.invert_roll)  roll = -roll;
	if (vcfg->attitude.invert_pitch) pitch = -pitch;

	long age_ms = (now.tv_sec - g_att_cursor.tv_sec) * 1000L +
	              (now.tv_nsec - g_att_cursor.tv_nsec) / 1000000L;
	if (age_ms < 0) age_ms = 0;
	if (age_ms > 65535) age_ms = 65535;

	out->roll_cdeg  = att_clamp16(roll);
	out->pitch_cdeg = att_clamp16(pitch);
	out->yaw_cdeg   = att_clamp16(yaw);
	out->status     = RTP_SIDECAR_ATT_VALID |
	                  (attitude_est_settled(&g_att_est)
	                   ? RTP_SIDECAR_ATT_SETTLED : 0);
	out->imu_age_ms = (uint16_t)age_ms;

	pthread_mutex_lock(&g_att_lock);
	g_att_snap.valid = 1;
	g_att_snap.settled = attitude_est_settled(&g_att_est);
	g_att_snap.roll_deg  = (float)roll  * 0.1f;
	g_att_snap.pitch_deg = (float)pitch * 0.1f;
	g_att_snap.yaw_deg   = (float)yaw   * 0.1f;
	pthread_mutex_unlock(&g_att_lock);
	return out;
}

/* ── Attitude HTTP hooks (called from the httpd thread) ─────────────────── */

/* Live attitude as malloc'd JSON for GET /api/v1/attitude. */
char *star6e_attitude_query(void)
{
	char *buf = malloc(192);
	if (!buf)
		return NULL;
	pthread_mutex_lock(&g_att_lock);
	if (!g_att_snap.valid) {
		snprintf(buf, 192, "{\"valid\":false}");
	} else {
		snprintf(buf, 192,
			"{\"valid\":true,\"settled\":%s,"
			"\"rollDeg\":%.1f,\"pitchDeg\":%.1f,\"yawDeg\":%.1f}",
			g_att_snap.settled ? "true" : "false",
			(double)g_att_snap.roll_deg,
			(double)g_att_snap.pitch_deg,
			(double)g_att_snap.yaw_deg);
	}
	pthread_mutex_unlock(&g_att_lock);
	return buf;
}

/* Level-trim calibration: arm the accumulator, wait for the frame loop
 * to fill it, solve the boresight trims. Completes early once
 * ATT_CAL_SAMPLES are in (~1.3 s @200 Hz); on the ≤2 s window it accepts
 * whatever arrived as long as ≥ ATT_CAL_MIN_SAMPLES — so calibration works
 * at any IMU ODR instead of always timing out below ~85 Hz. Blocking;
 * returns 0 on success, -1 on too few samples (attitude/IMU not running)
 * or an implausible gravity magnitude (device moving / free-fall). */
int star6e_attitude_calibrate_level(const VencConfig *vcfg,
	float *roll_deg, float *pitch_deg)
{
	float ax, ay, az;
	uint32_t cnt;
	int done = 0;

	pthread_mutex_lock(&g_att_lock);
	memset(&g_att_cal, 0, sizeof(g_att_cal));
	g_att_cal.state = ATT_CAL_PENDING;
	pthread_mutex_unlock(&g_att_lock);

	for (int i = 0; i < 40 && !done; i++) {   /* ≤2 s window */
		usleep(50 * 1000);
		pthread_mutex_lock(&g_att_lock);
		done = (g_att_cal.state == ATT_CAL_DONE);
		pthread_mutex_unlock(&g_att_lock);
	}

	pthread_mutex_lock(&g_att_lock);
	cnt = g_att_cal.n;
	ax = g_att_cal.sx; ay = g_att_cal.sy; az = g_att_cal.sz;
	g_att_cal.state = ATT_CAL_IDLE;
	pthread_mutex_unlock(&g_att_lock);

	if (cnt < ATT_CAL_MIN_SAMPLES)
		return -1;
	return attitude_axis_map_solve_trims(vcfg->attitude.axis_fwd,
		vcfg->attitude.axis_down, ax / (float)cnt, ay / (float)cnt,
		az / (float)cnt, roll_deg, pitch_deg);
}

/* ── Scene-detector stream decoders ───────────────────────────────────── */

static uint32_t star6e_scene_frame_size(const MI_VENC_Stream_t *s)
{
	uint32_t t = 0;
	unsigned int i;
	if (!s || !s->packet) return 0;
	for (i = 0; i < s->count; i++) t += s->packet[i].length;
	return t;
}

/* HEVC NAL types relevant for non-reference rewriting */
#define HEVC_NAL_TRAIL_N 0
#define HEVC_NAL_TRAIL_R 1
/* STAR6E_REFTYPE_ENHANCE_P_NOTFORREF (=5) is defined in star6e.h. Was locally
 * 4 (HiSilicon value) — wrong for the SigmaStar enum, so the TRAIL_N rewrite
 * marked referenced-enhance frames (or nothing under shallow SVC-T). */

/* Locate the NAL header byte 0 inside a payload buffer that may or may not
 * begin with a start-code prefix (00 00 01 / 00 00 00 01).  Returns the
 * index of NAL byte 0, or len on failure. */
static size_t star6e_nal_header_idx(const uint8_t *buf, size_t len)
{
	size_t i = 0;
	while (i < len && buf[i] == 0) i++;
	if (i < len && buf[i] == 0x01) i++;
	return i < len ? i : len;
}

/* If a NAL is TRAIL_R (type 1) and the SDK marked this frame as
 * ENHANCE_P_NOTFORREF, rewrite the NAL header to TRAIL_N (type 0).
 *
 * Byte 0 bit layout: forbidden_zero(1) | nal_unit_type(6) | layer_id_msb(1)
 *   TRAIL_R = 0x02   (type=1, layer_msb=0)
 *   TRAIL_N = 0x00   (type=0, layer_msb=0)
 *
 * No-op if NAL layer_id_msb != 0 (we only touch single-layer streams), if
 * the NAL is anything other than TRAIL_R, or if no slice NALs are present
 * in the pack (we never touch VPS/SPS/PPS — those are nal_type >= 32 and
 * fail the TRAIL_R check). */
static void star6e_patch_pack_to_trail_n(MI_VENC_Pack_t *pack)
{
	if (!pack || !pack->data || pack->length == 0)
		return;
	if (pack->packNum > 0) {
		const unsigned int info_cap = (unsigned int)(sizeof(pack->packetInfo) /
			sizeof(pack->packetInfo[0]));
		unsigned int n = pack->packNum > info_cap ? info_cap : pack->packNum;
		unsigned int k;
		for (k = 0; k < n; ++k) {
			MI_U32 off = pack->packetInfo[k].offset;
			MI_U32 nlen = pack->packetInfo[k].length;
			if (off >= pack->length || nlen == 0 ||
			    off + nlen > pack->length)
				continue;
			size_t hdr = star6e_nal_header_idx(pack->data + off, nlen);
			if (hdr >= nlen) continue;
			if (pack->data[off + hdr] == 0x02) {
				pack->data[off + hdr] = 0x00;
			}
		}
		return;
	}
	/* packNum == 0: single NAL */
	if (pack->offset >= pack->length)
		return;
	{
		MI_U32 off = pack->offset;
		MI_U32 nlen = pack->length - off;
		size_t hdr = star6e_nal_header_idx(pack->data + off, nlen);
		if (hdr >= nlen) return;
		if (pack->data[off + hdr] == 0x02) {
			pack->data[off + hdr] = 0x00;
		}
	}
}

static void star6e_patch_stream_to_trail_n(MI_VENC_Stream_t *s)
{
	unsigned int i;
	if (!s || !s->packet) return;
	for (i = 0; i < s->count; i++)
		star6e_patch_pack_to_trail_n(&s->packet[i]);
}

/* HEVC-only since 0.10.12: IDR_W_RADL = nal_type 19. */
static uint8_t star6e_scene_is_idr(const MI_VENC_Stream_t *s)
{
	unsigned int i;
	if (!s || !s->packet) return 0;
	for (i = 0; i < s->count; i++) {
		const MI_VENC_Pack_t *p = &s->packet[i];
		unsigned int k, n = p->packNum > 8 ? 8 : p->packNum;
		if (n > 0) {
			for (k = 0; k < n; k++) {
				if (p->packetInfo[k].packType.h265Nalu == 19)
					return 1;
			}
		} else {
			if (p->naluType.h265Nalu == 19) return 1;
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
	/* Configured base size this pipeline started with — used at reinit to
	 * detect a video0.size change (the only config field that crosses a
	 * sensor-mode boundary), so the respawn can cold-init VIF/VPE. */
	uint32_t started_base_w;
	uint32_t started_base_h;
} Star6eRunnerContext;

static void install_signal_handlers(void);

/* Write VPE SCL clock preset before forced exit.  Uses only
 * async-signal-safe syscalls (open/write/close). */
static void scl_preset_emergency(void)
{
	static const char path[] = "/sys/devices/virtual/mstar/mscl/clk";
	static const char val[] = "384000000\n";
	static const char msg[] = "[waybeam] Emergency SCL preset written\n";
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

/* Start the supervisory AE limit enforcer.  This is the sole AE path on
 * Star6E: the ISP firmware/bin AE does convergence and this thread re-asserts
 * the user's gain/shutter min/max on the exposure limit each tick.  The
 * historical aeEngine=custom userspace governor was retired — both engine
 * values now run this same enforcer (see start_ae_enforcer's caller). */
static void start_ae_enforcer(const Star6ePipelineState *ps,
	const VencConfig *vcfg)
{
	Star6eCus3aConfig ae_cfg;

	star6e_cus3a_config_defaults(&ae_cfg);
	if (vcfg->isp.ae_fps > 0)
		ae_cfg.ae_fps = vcfg->isp.ae_fps;
	if (vcfg->isp.gain_max > 0)
		ae_cfg.gain_max = vcfg->isp.gain_max;
	if (vcfg->isp.shutter_rule_180 && ps->sensor.fps > 0) {
		ae_cfg.shutter_max_us = 1000000 / (ps->sensor.fps * 2);
		ae_cfg.shutter_pin = 1;
	} else if (vcfg->isp.shutter_max_us > 0) {
		ae_cfg.shutter_max_us = vcfg->isp.shutter_max_us;
	}
	if (vcfg->isp.gain_min > 0)
		ae_cfg.gain_min = vcfg->isp.gain_min;
	if (vcfg->isp.shutter_min_us > 0)
		ae_cfg.shutter_min_us = vcfg->isp.shutter_min_us;
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

		/* Service HTTP record start/stop forwarded by main loop.
		 * Dual-stream skips this (ts_recorder is NULL); dual mode
		 * routes here so the ts_recorder is opened/closed by exactly
		 * one thread. */
		if (d->rec_req_stop && d->ts_recorder) {
			star6e_ts_recorder_stop(d->ts_recorder);
			d->rec_req_stop = 0;
		}
		if (d->rec_req_start && d->ts_recorder) {
			star6e_ts_recorder_stop(d->ts_recorder);
			star6e_ts_recorder_start(d->ts_recorder,
				d->rec_req_start_dir, d->audio_ring);
			if (d->ts_recorder->request_idr)
				d->ts_recorder->request_idr();
			d->rec_req_start = 0;
		}

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
						&d->output, &stream, 1, 0, NULL,
						NULL, NULL, 0);
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

	/* AE runs in ONE mode on Star6E: the SDK firmware/bin AE converges and
	 * the supervisory thread enforces the gain/shutter limits beside it.  The
	 * thread needs aeFps>0 for a tick rate. */
	if (vcfg->isp.ae_fps > 0)
		start_ae_enforcer(ps, vcfg);

	if (vcfg->fpv.roi_enabled) {
		star6e_controls_apply_roi_qp(vcfg->fpv.roi_qp);
	}
	if (vcfg->video0.qp_delta != 0) {
		star6e_controls_apply_qp_delta(vcfg->video0.qp_delta);
	}
	if (vcfg->video0.max_i_bytes > 0 || vcfg->video0.max_p_bytes > 0) {
		const VencApplyCallbacks *cb = star6e_controls_callbacks();
		if (cb->apply_max_frame_size)
			cb->apply_max_frame_size(vcfg->video0.max_i_bytes,
				vcfg->video0.max_p_bytes);
	}

	if (!ps->output_enabled) {
		ps->stored_fps = vcfg->video0.fps;
		star6e_controls_apply_fps(STAR6E_CONTROLS_IDLE_FPS);
		printf("> Output disabled at startup, idling at %u fps\n",
			STAR6E_CONTROLS_IDLE_FPS);
	}

	star6e_recorder_init(&ps->recorder);
	audio_ring_init(&ps->audio_ring);
	{
		/* Match the TS-mux codec to audio.codec.  Opus and PCM are the
		 * only TS-recordable formats; G.711 is not recorded (rec_ring
		 * stays unfed in star6e_audio.c, audio_rate set to 0 below). */
		int parsed = audio_codec_parse_name(vcfg->audio.codec);
		uint8_t ts_codec = (parsed == AUDIO_CODEC_TYPE_OPUS)
			? TS_AUDIO_CODEC_OPUS : TS_AUDIO_CODEC_PCM_S302M;
		uint32_t rate = 0;
		uint8_t  ch   = 0;
		if (vcfg->audio.enabled &&
		    (parsed == AUDIO_CODEC_TYPE_RAW ||
		     parsed == AUDIO_CODEC_TYPE_OPUS)) {
			rate = vcfg->audio.sample_rate;
			ch   = (uint8_t)vcfg->audio.channels;
		}
		star6e_ts_recorder_init(&ps->ts_recorder, rate, ch, ts_codec);
	}
	ps->ts_recorder.request_idr = runtime_request_idr;
	if (vcfg->record.max_seconds > 0)
		ps->ts_recorder.max_seconds = vcfg->record.max_seconds;
	if (vcfg->record.max_mb > 0)
		ps->ts_recorder.max_bytes = (uint64_t)vcfg->record.max_mb * 1024 * 1024;

	/* Start dual VENC if mode is "dual" or "dual-stream".
	 *
	 * Gated on mode ALONE, not record.enabled: the VENC channel topology
	 * is independent of whether recording auto-starts at boot.  A
	 * runtime-control client (e.g. RubyFPV) sets record.enabled=false and
	 * starts recording later via /api/v1/record/start.  ps->dual must
	 * already exist by then, otherwise the record path falls back to
	 * mirror mode and captures ch0 (the video0 stream bitrate) instead of
	 * ch1 (record.bitrate) — the high-bitrate recording channel is silently
	 * never used.  The ch1 TS writer no-ops while the recorder is closed
	 * (see star6e_ts_recorder_write_stream), so an idle ch1 records nothing
	 * until a record/start opens the file. */
	if (strcmp(vcfg->record.mode, "dual") == 0 ||
	    strcmp(vcfg->record.mode, "dual-stream") == 0) {
		star6e_pipeline_start_dual(ps,
			vcfg->record.bitrate, vcfg->record.fps,
			vcfg->record.gop_size, vcfg->record.mode,
			vcfg->record.server[0] ? vcfg->record.server : "");

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
			if (!ps->dual->is_dual_stream) {
				ps->dual->ts_recorder = &ps->ts_recorder;
				ps->dual->audio_ring =
					vcfg->audio.enabled ? &ps->audio_ring : NULL;
			}
			dual_rec_thread_start(ps->dual);
			venc_api_dual_register(ps->dual->channel,
				ps->dual->bitrate, ps->dual->fps,
				ps->dual->gop);
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

/* Same-PID (in-process) pipeline reinit does NOT work on Star6E — the
 * reliable cold restart is a *new PID* via fork+exec.  This was retested
 * exhaustively on imx335 @ 192.168.1.13 (2026-06-07) and confirmed at the
 * SigmaStar I6E driver level — see
 * documentation/STAR6E_SINGLE_PID_REINIT_FINDINGS.md for the full evidence.
 *
 * Note the historical reason recorded here was partly OUTDATED: a second
 * MI_SYS_Init in the same PID no longer hangs MI_DEVICE_Open (that mode is
 * gone).  But resetting MI_SYS, the /dev/mi_vif+vpe fds, AND the MI vendor
 * lib globals (dlclose+dlopen) in one process is STILL insufficient — the
 * VIF/VPE/ISP channel state the rebuild needs is pinned to the task in the
 * kernel driver and only released by execv (fresh address space + fd
 * context, same PID slot).  Symptom of an in-process rebuild: ISP readiness
 * timeout + CmdLoadBinFile -1 + VIF "layout type 2 bindmode 4 not sync err"
 * → no frames.
 *
 * The fork+exec machinery lives in src/venc_respawn.c (shared with Maruko);
 * the Star6E runtime just calls venc_respawn_request() in its reinit
 * handler.  Bench-validated against 12 consecutive cross-mode sensor
 * SIGHUPs (rounds 0→1→2→3 ×3) with no degradation. */

static int star6e_runtime_handle_reinit(int *handled)
{
	*handled = 0;

	if (!venc_api_get_reinit())
		return 0;
	*handled = 1;
	venc_api_clear_reinit();

	printf("> Reinit requested: cold restart via fork+exec on shutdown\n");
	fflush(stdout);

	/* Detect a video0.size change.  size is the only config field that
	 * crosses a sensor-mode boundary; resilience/framing(stab|zoom) stay
	 * within one mode.  On a mode change the respawn must cold-init VIF/VPE
	 * (close their inherited /dev/mi_* fds in the fd-scrub) — otherwise the
	 * fresh process re-inits VIF to a different mode against the old mode's
	 * kernel state and wedges vpe0_P0_MAIN.  Same-size respawns leave the
	 * fds inherited (the deadlock-safe default).  See venc_respawn.c.
	 *
	 * NOTE: the same-mode MMU read-fault storm (MMU client 0x15, IsWrite=0)
	 * that used to wedge the ~2nd consecutive respawn is now fixed at its
	 * root — see debug_osd_destroy()'s detach→destroy settle.  Cold-vif here
	 * remains gated strictly to size changes (forcing it on every respawn
	 * storms immediately because the close lands mid-flight). */
	if (g_runner_ctx &&
	    (g_runner_ctx->vcfg.video0.width != g_runner_ctx->started_base_w ||
	     g_runner_ctx->vcfg.video0.height != g_runner_ctx->started_base_h)) {
		printf("> size change %ux%u -> %ux%u: cold-init VIF/VPE on respawn\n",
			g_runner_ctx->started_base_w, g_runner_ctx->started_base_h,
			g_runner_ctx->vcfg.video0.width,
			g_runner_ctx->vcfg.video0.height);
		venc_respawn_set_cold_vif(1);
	}

	/* Mark for respawn after teardown, then exit the run loop.
	 * main() will execute backend->teardown (clean MI_SYS_Exit) then
	 * fork+exec the successor process from a clean state. */
	venc_respawn_request();
	g_running = 0;
	return 0;
}

/* Map a plugin box edge (net-input coords) to a canvas pixel, doing the whole
 * range clamp in float BEFORE the cast so a negative/huge/inf/NaN edge from an
 * out-of-contract plugin cannot trigger float->unsigned UB.  `net` is non-zero
 * at the call sites; guarded anyway. */
static uint32_t osd_box_px(float v, uint32_t canvas, uint32_t net)
{
	float r;
	if (canvas == 0 || net == 0)
		return 0;
	r = v * (float)canvas / (float)net;
	if (!(r >= 0.0f))                 /* negative, or NaN (compares false) */
		r = 0.0f;
	if (r > (float)(canvas - 1))      /* also catches +inf */
		r = (float)(canvas - 1);
	return (uint32_t)r;
}

/* Encoded bytes accumulated since the debug OSD's last 1 Hz refresh.  Summed
 * from the encoder's own frame sizes rather than the transport's byte count so
 * the row stays truthful with output disabled and excludes packetization
 * overhead — it reads the encoder against its RC target, not the wire.  Single
 * writer (the pipeline thread, which is also the only reader). */
static uint64_t g_osd_enc_bytes;

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

	/* refPred error-resilience marking — rewrite TRAIL_R → TRAIL_N for
	 * frames the SDK marked as ENHANCE_P_NOTFORREF.  The encoder's own
	 * SVC-T pyramid logic determines which frames are non-reference; we
	 * just propagate that designation into the bitstream so generic
	 * receivers can safely drop those NALs without cascade.
	 *
	 * Only active when refPred is enabled (ref_base > 0) — otherwise the
	 * encoder produces a flat single-ref stream and every frame matters. */
	if (vcfg->video0.ref_base > 0 &&
	    stream.h265Info.refType == STAR6E_REFTYPE_ENHANCE_P_NOTFORREF) {
		star6e_patch_stream_to_trail_n(&stream);
	}

	{
		RtpSidecarEncInfo enc_info;
		uint32_t frame_size = star6e_scene_frame_size(&stream);
		uint8_t is_idr = star6e_scene_is_idr(&stream);

		g_osd_enc_bytes += frame_size;

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

		/* Attitude: runs whenever configured (a few µs/frame + the
		 * IMU ring mutex) — no subscription gate, so the WebUI live
		 * readout and the level-trim calibration work standalone.
		 * The trailer itself still only reaches the wire when a
		 * sidecar subscriber is live (send fans out to live slots). */
		RtpSidecarAttitudeInfo att_info;
		const RtpSidecarAttitudeInfo *att_ptr = NULL;
		if (vcfg->attitude.enabled && ps->imu)
			att_ptr = attitude_frame_update(vcfg, &att_info);

		/* Detection: serialise the latest IPU snapshot into a DETECT
		 * trailer, but only when detection is active AND a sidecar
		 * subscriber is live — the blob is dead weight otherwise.  The
		 * object cap keeps the trailer well under the datagram buffer,
		 * so the full buffer is a safe budget. */
		uint8_t detect_buf[RTP_SIDECAR_DGRAM_MAX];
		const void *detect_ptr = NULL;
		uint16_t detect_len = 0;
		if (vcfg->detect.enabled &&
		    rtp_sidecar_is_subscribed(&ps->video.sidecar)) {
			Star6eDetectSnapshot snap;
			if (star6e_ipu_yolo_snapshot(ps, &snap)) {
				uint64_t now_us = wb_monotonic_us();
				uint64_t age = now_us > snap.produced_us
					? (now_us - snap.produced_us) / 1000 : 0;
				/* model_id comes from the snapshot (latched with the
				 * boxes), not vcfg — so a live model swap flips it in
				 * lockstep with the first new-model DETECT instead of
				 * tagging the last old-model boxes with the new id. */
				size_t len = detect_wire_build(detect_buf,
					sizeof(detect_buf), snap.boxes, snap.count,
					snap.model_id, snap.seq,
					age > 0xFFFF ? 0xFFFF : (uint16_t)age,
					snap.net_w, snap.net_h, sizeof(detect_buf));
				if (len > 0) {
					detect_ptr = detect_buf;
					detect_len = (uint16_t)len;
				}
			}
		}

		(void)star6e_video_send_frame(&ps->video, &ps->output, &stream,
			ps->output_enabled, vcfg->system.verbose, &enc_info,
			att_ptr, detect_ptr, detect_len);
	}

	/* Orientation (image.flip / image.mirror) is applied once at bring-up
	 * (sensor_select before MI_SNR_Enable + start_vpe before VPE start) and
	 * holds for the life of the stream — device-verified on IMX335 and
	 * IMX415.  The sensor driver only rewrites orientation when we set it
	 * (orien_dirty), so nothing clears it mid-stream; no per-frame re-apply
	 * is needed.  (MI_SNR_GetOrien proved unreliable under AE I2C load on
	 * IMX335 — it reads 0 while the image is plainly held — so we do not
	 * use it to second-guess the applied state.) */

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
	 *
	 * Mirror mode: act on the ts_recorder / hevc recorder directly here.
	 *
	 * Dual mode (not dual-stream): forward the request to the dual
	 * recording thread, which owns the ts_recorder exclusively.  This
	 * keeps the recorder single-threaded; the dual thread acts on the
	 * request between frame writes.
	 *
	 * Dual-stream mode: ch1 is sent over RTP, no on-disk recorder —
	 * consume and ignore the flag.
	 */
	{
		char rec_dir[256];
		int start_pending = venc_api_get_record_start(rec_dir,
			sizeof(rec_dir));
		int stop_pending = venc_api_get_record_stop();

		if (start_pending) {
			if (ps->dual && !ps->dual->is_dual_stream) {
				if (vcfg->audio.enabled)
					ps->audio.rec_ring = &ps->audio_ring;
				snprintf(ps->dual->rec_req_start_dir,
					sizeof(ps->dual->rec_req_start_dir),
					"%s", rec_dir);
				/* Set start flag last so the dual thread sees the
				 * dir already populated when it consumes the flag. */
				ps->dual->rec_req_stop = 0;
				ps->dual->rec_req_start = 1;
			} else if (!ps->dual) {
				/* Mirror mode: act directly on the recorders */
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
			/* dual-stream: nothing to do */
		}
		if (stop_pending) {
			if (ps->dual && !ps->dual->is_dual_stream) {
				ps->audio.rec_ring = NULL;
				ps->dual->rec_req_start = 0;
				ps->dual->rec_req_stop = 1;
			} else if (!ps->dual) {
				star6e_recorder_stop(&ps->recorder);
				star6e_ts_recorder_stop(&ps->ts_recorder);
				ps->audio.rec_ring = NULL;
			}
			/* dual-stream: nothing to do */
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
		static unsigned int osd_kbps;
		static Star6eAeOsdStatus osd_ae;
		struct timespec osd_now;

			/* HW-crop stab outputs the cropped encoded dim on port0, so the
			 * OSD canvas is 1:1 with the encoded frame (static) — no
			 * per-frame panel-offset tracking needed. */
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
			/* bytes*8/ms is bits/ms, i.e. kbps directly. */
			osd_kbps = (unsigned int)(g_osd_enc_bytes * 8 /
				(uint64_t)osd_ms);
			g_osd_enc_bytes = 0;
			/* AE/AWB readouts ride the same 1Hz window — each
			 * refresh dlopens libmi_isp and round-trips several
			 * MI_ISP getters. */
			star6e_controls_ae_osd_status(&osd_ae);
		}

		debug_osd_text(ps->debug_osd, 0, "fps", "%u", osd_fps);
		debug_osd_text(ps->debug_osd, 1, "cpu", "%d%%",
			debug_osd_get_cpu(ps->debug_osd));

		/* Sensor readout + encoded output — read back exactly which mode
		 * is live (WxH@fps + mode idx) and the encoded WxH straight off
		 * the overlay.  Star6E video codec is always H.265. */
		debug_osd_text(ps->debug_osd, 2, "snr", "%ux%u@%u m%d",
			(unsigned)ps->sensor.plane.capt.width,
			(unsigned)ps->sensor.plane.capt.height,
			ps->sensor.fps, ps->sensor.mode_index);
		debug_osd_text(ps->debug_osd, 3, "enc", "%ux%u h265",
			ps->image_width, ps->image_height);

		/* Actual encoded rate against the configured RC target — the
		 * gap between the two is the RC undershoot/overshoot, and it
		 * separates an encoder problem from a link problem at a
		 * glance (a healthy encoder tracking target while the picture
		 * stutters points at the radio, not here). */
		debug_osd_text(ps->debug_osd, 4, "br", "%u/%uk",
			osd_kbps, vcfg->video0.bitrate);

		{
			int osd_row = 5;

#if HAVE_FRAMING_STAB
			/* Stabilization telemetry: Kalman correction (a) +
			 * raw detector measurement (m), in stab pixels.
			 * "sfil" = stab-fill (correction applied as the
			 * compose shift), "stab" = HW-crop.  Hidden when no
			 * stab thread runs. */
			{
				int sx, sy, mx, my, sp, sf;
				if (star6e_framing_stab_osd_status(&sx, &sy,
				    &mx, &my, &sp, &sf))
					debug_osd_text(ps->debug_osd, osd_row++,
						sf ? "sfil" : "stab",
						"a%+d%+d m%+d%+d%s",
						sx, sy, mx, my,
						sp ? " paused" : "");
			}
#endif

			if (osd_ae.ae_valid) {
				debug_osd_text(ps->debug_osd, osd_row++,
					"exp", "%uus sg%u/%u ig%u",
					osd_ae.shutter_us,
					osd_ae.sgain_x1024, osd_ae.max_sgain,
					osd_ae.igain_x1024);
			}
			if (osd_ae.ae_info_valid) {
				debug_osd_text(ps->debug_osd, osd_row++,
					"ae", "y%u t%u %s",
					osd_ae.luma_y, osd_ae.scene_target,
					osd_ae.boundary ? "bound" :
					osd_ae.stable ? "stable" : "adj");
			}
			if (osd_ae.awb_valid && osd_ae.awb_userspace) {
				/* Userspace loop drives AWB: show the applied
				 * gains and the running apply count (which is
				 * the liveness signal — colour temperature is
				 * not estimated in this mode). */
				debug_osd_text(ps->debug_osd, osd_row++,
					"awb", "r%u b%u usr#%u",
					osd_ae.rgain, osd_ae.bgain,
					osd_ae.awb_ticks);
			} else if (osd_ae.awb_valid) {
				debug_osd_text(ps->debug_osd, osd_row++,
					"awb", "r%u b%u %uk %s",
					osd_ae.rgain, osd_ae.bgain,
					osd_ae.color_temp,
					osd_ae.awb_stable ? "stable" : "adj");
			}

			Star6eIntraRefreshStatus ir;
			Star6eRefPredStatus      rp;
			star6e_pipeline_intra_refresh_status(&ir);
			star6e_pipeline_ref_pred_status(&rp);
			/* Resilience banner: only render when the preset is set
			 * to something other than "off" — keeps the OSD compact
			 * when no resilience features are active. */
			if (vcfg->video0.resilience[0] &&
			    strcmp(vcfg->video0.resilience, "off") != 0) {
				if (rp.active) {
					debug_osd_text(ps->debug_osd, osd_row++,
						"res", "%s rp=%u/%u",
						vcfg->video0.resilience,
						rp.base, rp.enhance);
				} else {
					debug_osd_text(ps->debug_osd, osd_row++,
						"res", "%s",
						vcfg->video0.resilience);
				}
			}
			if (ir.active) {
				debug_osd_text(ps->debug_osd, osd_row++, "intra",
					"%s L%u q%u",
					ir.mode_name, ir.effective_lines_per_p,
					ir.effective_qp);
				debug_osd_text(ps->debug_osd, osd_row++, "gop",
					"%.2fs %s",
					ir.effective_gop_sec,
					ir.gop_auto ? "auto" : "fixed");
			}

			Star6eZoomStatus zoom;
			star6e_pipeline_zoom_status(&zoom);
			if (zoom.active) {
				debug_osd_text(ps->debug_osd, osd_row++, "zoom",
					"%u.%02ux %ux%u",
					zoom.level_x100 / 100,
					zoom.level_x100 % 100,
					zoom.output_w, zoom.output_h);
				debug_osd_text(ps->debug_osd, osd_row++, "crop",
					"%ux%u+%u+%u",
					zoom.crop_w, zoom.crop_h,
					zoom.crop_x, zoom.crop_y);
			}

			/* Detection boxes (detect.osd): scale net-space boxes
			 * onto the canvas — the port1 tap squashes the full
			 * FOV linearly per axis, so net->canvas is a straight
			 * ratio (the same mapping the sidecar normalizes by).
			 * A stale snapshot (reader stalled) is not drawn. */
			if (vcfg->detect.enabled && vcfg->detect.osd) {
				Star6eDetectSnapshot snap;
				static const uint16_t det_col[] = {
					DEBUG_OSD_RED, DEBUG_OSD_GREEN,
					DEBUG_OSD_YELLOW, DEBUG_OSD_CYAN,
					DEBUG_OSD_BLUE, DEBUG_OSD_WHITE,
				};
				const unsigned ncol =
					sizeof(det_col) / sizeof(det_col[0]);

				if (star6e_ipu_yolo_snapshot(ps, &snap) &&
				    snap.count > 0 &&
				    snap.net_w && snap.net_h &&
				    wb_monotonic_us() - snap.produced_us <
					700000) {
					uint32_t cw = ps->image_width;
					uint32_t chh = ps->image_height;
					int di;

					for (di = 0; di < snap.count; di++) {
						const DetectBox *b =
							&snap.boxes[di];
						uint32_t x1 = osd_box_px(b->x1,
							cw, snap.net_w);
						uint32_t y1 = osd_box_px(b->y1,
							chh, snap.net_h);
						uint32_t x2 = osd_box_px(b->x2,
							cw, snap.net_w);
						uint32_t y2 = osd_box_px(b->y2,
							chh, snap.net_h);

						if (x1 >= x2 || y1 >= y2)
							continue;
						debug_osd_rect(ps->debug_osd,
							(uint16_t)x1,
							(uint16_t)y1,
							(uint16_t)(x2 - x1),
							(uint16_t)(y2 - y1),
							det_col[(unsigned)
								b->cls % ncol],
							0);
					}
					debug_osd_text(ps->debug_osd,
						osd_row++, "det", "%d",
						snap.count);
				}
			}
		}

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

	/* Always, before any VIF/VPE/ISP bring-up: reconcile NPU driver
	 * state a predecessor may have poisoned (see star6e_ipu_scrub).
	 * Unconditional by design — the poison survives process exit and
	 * fd release, so no flag carried from the previous instance can
	 * be trusted to know whether it is needed. */
	(void)star6e_ipu_scrub();

	venc_httpd_start(ctx->vcfg.system.web_port);
	ctx->httpd_started = 1;

	ret = star6e_pipeline_start(&ctx->ps, &ctx->vcfg, &g_sdk_quiet);
	if (ret != 0) {
		return ret;
	}
	ctx->pipeline_started = 1;
	/* Snapshot the base size the pipeline started with (before any live
	 * SET mutates ctx->vcfg in place via the aliased g_cfg). */
	ctx->started_base_w = ctx->vcfg.video0.width;
	ctx->started_base_h = ctx->vcfg.video0.height;

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
	struct timespec run_start;
	int cold_boot_fps_kick_done = 0;
	unsigned int idle_counter = 0;
	int handled;
	int ret;

	clock_gettime(CLOCK_MONOTONIC, &cus3a_ts_last);
	clock_gettime(CLOCK_MONOTONIC, &run_start);

	/* Pin encoder to CPU 0 and run the GetStream->packetize->send path at
	 * an elevated SCHED_FIFO priority.  At the previous minimum priority (1)
	 * this thread was preempted mid-frame by other userspace RT threads
	 * (audio capture / IMU, also FIFO/1) and SCHED_OTHER work, surfacing as
	 * a periodic ~one-frame RTP delivery stall (a single idle gap on the
	 * wire, no catch-up burst).  The SDK pipeline kernel threads run at
	 * SCHED_RR/98 and MUST keep outranking us — we depend on them to produce
	 * frames — so the priority is clamped well below 98 (audio testing also
	 * found ~90 made timing worse, likely priority inversion).
	 *
	 * Tunable for on-device A/B without a rebuild via the VENC_RT_PRIO env
	 * var (clamped 1..80); VENC_RT_PRIO=1 reproduces the old behaviour.
	 * Silent fallback if unprivileged or single-core. */
	{
		unsigned long mask = 1UL;  /* CPU 0 */
		syscall(__NR_sched_setaffinity, 0, sizeof(mask), &mask);

		int rt_prio = 50;
		const char *env = getenv("VENC_RT_PRIO");
		if (env && *env) {
			int v = atoi(env);
			if (v < 1)
				v = 1;
			else if (v > 80)
				v = 80;
			rt_prio = v;
		}

		struct sched_param sp;
		sp.sched_priority = rt_prio;
		if (pthread_setschedparam(pthread_self(), SCHED_FIFO,
		    &sp) != 0)
			printf("> note: RT priority not available"
				" (run as root)\n");
		else
			printf("> encoder thread: SCHED_FIFO prio %d,"
				" pinned CPU0\n", rt_prio);
	}

	while (g_running) {
		ret = star6e_runtime_handle_reinit(&handled);
		if (ret != 0) {
			return ret;
		}
		if (handled) {
			continue;
		}

		/* Service any pending detector live model-swap on this (pipeline)
		 * thread, between frames, so the VPE port1 recreate is atomic w.r.t.
		 * the per-frame detect snapshot query below. */
		star6e_controls_service_detect_reload();

		ret = star6e_runtime_process_stream(ctx, &cus3a_ts_last,
			&idle_counter);
		if (ret != 0) {
			return ret;
		}

		/* One-shot cold-boot fps re-kick ~1.5s after start, once the ISP
		 * bin load + AE have settled (the init-time kick fires too early
		 * and doesn't stick on a cold boot). */
		if (!cold_boot_fps_kick_done) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			if ((now.tv_sec - run_start.tv_sec) +
			    (now.tv_nsec - run_start.tv_nsec) / 1e9 >= 1.5) {
				star6e_pipeline_cold_boot_fps_rekick(&ctx->ps,
					&ctx->vcfg);
				cold_boot_fps_kick_done = 1;
			}
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
			/* Rename so /proc/<pid>/comm reads "waybeam-wd"
			 * instead of "waybeam".  Required for SIGHUP-respawn
			 * flow: the new instance spawned by
			 * star6e_runtime_respawn_after_exit() runs the
			 * comm-based duplicate check at startup; without this
			 * rename the still-alive watchdog (kept around to
			 * SIGKILL/sysrq-b a hung parent) reads as "waybeam"
			 * and the respawn aborts with the "already running"
			 * banner. */
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
				if (kill(parent, 0) != 0) {
					_exit(0);  /* parent exited cleanly */
				}
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

	/* Pause HTTP dispatch across the SDK teardown window: the httpd
	 * worker is still alive (venc_httpd_stop runs later, and even then
	 * it only detaches), so any in-flight HTTP handler would dereference
	 * the static control context that star6e_controls_reset is about to
	 * zero, plus VENC channels that star6e_pipeline_stop destroys.
	 * pause() drains any in-flight handler before returning; new requests
	 * during the window receive 503.  No resume — the process is exiting
	 * (SIGHUP fork+exec parent, or normal shutdown). */
	venc_httpd_pause();
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
