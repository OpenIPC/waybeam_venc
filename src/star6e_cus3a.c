#include "star6e_cus3a.h"
#include "star6e.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── CUS3A / ISP ABI structures ──────────────────────────────────────── */

/* HW AE statistics block — one per grid cell. */
typedef struct {
	short r;
	short g;
	short b;
	short y;
} Cus3aAeSample;

#define CUS3A_AE_GRID_X 128
#define CUS3A_AE_GRID_Y  90
#define CUS3A_AE_GRID_SZ (CUS3A_AE_GRID_X * CUS3A_AE_GRID_Y)

typedef struct {
	Cus3aAeSample nAvg[CUS3A_AE_GRID_SZ];
} Cus3aAeHwStats;

/* CusAEInfo_t — Star6E SDK layout (verified via hex dump).
 * First 3 u32s are reserved, then block dims, then shutter/gain. */
typedef struct {
	uint32_t reserved0[3];
	uint32_t AvgBlkX;
	uint32_t AvgBlkY;
	uint32_t reserved1;
	uint32_t Shutter;
	uint32_t SensorGain;
	uint32_t IspGain;
	uint32_t ShutterHDRShort;
	uint32_t SensorGainHDRShort;
	uint32_t IspGainHDRShort;
} Cus3aAeInfo;

#define CUS3A_ISP_STATE_NORMAL 0

/* ── ISP exposure limit (same layout as star6e_controls.c) ───────────── */

typedef struct {
	unsigned int minShutterUs;
	unsigned int maxShutterUs;
	unsigned int minApertX10;
	unsigned int maxApertX10;
	unsigned int minSensorGain;
	unsigned int minIspGain;
	unsigned int maxSensorGain;
	unsigned int maxIspGain;
} Cus3aIspExposureLimit;


/* ── Function pointer types (resolved via dlsym) ─────────────────────── */

typedef int (*fn_ae_get_hw_stats_t)(int channel, Cus3aAeHwStats *stats);
typedef int (*fn_cus3a_get_ae_status_t)(int channel, Cus3aAeInfo *info);
typedef int (*fn_ae_get_state_t)(int channel, int *state);
typedef int (*fn_cus3a_open_frame_sync_t)(int *fd0, int *fd1);
typedef int (*fn_cus3a_wait_frame_sync_t)(int fd0, int fd1, int timeout_ms);
typedef int (*fn_cus3a_close_frame_sync_t)(int fd0, int fd1);
typedef int (*fn_ae_get_exposure_limit_t)(int channel,
	Cus3aIspExposureLimit *limit);
typedef int (*fn_ae_set_exposure_limit_t)(int channel,
	Cus3aIspExposureLimit *limit);

/* MI_SNR_GetPlaneInfo reads the actual sensor register values (shutter,
 * gain) which may differ from the ISP AE limit/target on cold boot. */
typedef int (*fn_snr_get_plane_info_t)(int pad, int plane_id, void *info);
typedef int (*fn_snr_set_fps_t)(int pad, unsigned int fps);

/* ── Module state ────────────────────────────────────────────────────── */

typedef struct {
	/* config */
	Star6eCus3aConfig cfg;

	/* constraint limits — written by main thread, read by monitor */
	volatile uint32_t shutter_max_us;   /* 0 = auto from sensor_fps */
	volatile uint32_t gain_max;         /* 0 = use ISP bin default */

	/* ISP bin baseline limits (read at startup) */
	uint32_t bin_max_shutter_us;
	uint32_t bin_max_sensor_gain;

	/* thread */
	pthread_t thread;
	volatile int running;

	/* ISP symbols */
	void *h_isp;
	void *h_cus3a;
	fn_ae_get_hw_stats_t       fn_get_hw_stats;
	fn_cus3a_get_ae_status_t   fn_get_ae_status;
	fn_ae_get_state_t          fn_ae_get_state;
	fn_cus3a_open_frame_sync_t  fn_open_sync;
	fn_cus3a_wait_frame_sync_t  fn_wait_sync;
	fn_cus3a_close_frame_sync_t fn_close_sync;
	fn_ae_get_exposure_limit_t  fn_get_exposure_limit;
	fn_ae_set_exposure_limit_t  fn_set_exposure_limit;
	fn_snr_get_plane_info_t     fn_get_plane_info;
	fn_snr_set_fps_t            fn_set_fps;
	void                       *h_sensor;
} Cus3aState;

static Cus3aState g_cus3a;

/* ── Helpers ─────────────────────────────────────────────────────────── */

void star6e_cus3a_config_defaults(Star6eCus3aConfig *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->sensor_fps = 120;
	cfg->ae_fps = 15;
}

static uint32_t compute_max_shutter(const Cus3aState *s)
{
	if (s->shutter_max_us > 0)
		return s->shutter_max_us;
	if (s->cfg.sensor_fps > 0)
		return 1000000 / s->cfg.sensor_fps;
	return 8333;  /* fallback ~120fps */
}

static int resolve_symbols(Cus3aState *s)
{
	s->h_isp = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!s->h_isp) {
		fprintf(stderr, "[cus3a] failed to open libmi_isp.so\n");
		return -1;
	}

	s->h_cus3a = dlopen("libcus3a.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!s->h_cus3a) {
		fprintf(stderr, "[cus3a] failed to open libcus3a.so\n");
		return -1;
	}

	/* AE symbols (required for monitoring) */
	s->fn_get_hw_stats = (fn_ae_get_hw_stats_t)dlsym(
		s->h_isp, "MI_ISP_AE_GetAeHwAvgStats");
	s->fn_get_ae_status = (fn_cus3a_get_ae_status_t)dlsym(
		s->h_isp, "MI_ISP_CUS3A_GetAeStatus");
	s->fn_ae_get_state = (fn_ae_get_state_t)dlsym(
		s->h_isp, "MI_ISP_AE_GetState");
	s->fn_open_sync = (fn_cus3a_open_frame_sync_t)dlsym(
		s->h_cus3a, "Cus3AOpenIspFrameSync");
	s->fn_wait_sync = (fn_cus3a_wait_frame_sync_t)dlsym(
		s->h_cus3a, "Cus3AWaitIspFrameSync");
	s->fn_close_sync = (fn_cus3a_close_frame_sync_t)dlsym(
		s->h_cus3a, "Cus3ACloseIspFrameSync");

	s->fn_get_exposure_limit = (fn_ae_get_exposure_limit_t)dlsym(
		s->h_isp, "MI_ISP_AE_GetExposureLimit");
	s->fn_set_exposure_limit = (fn_ae_set_exposure_limit_t)dlsym(
		s->h_isp, "MI_ISP_AE_SetExposureLimit");

	/* MI_SNR_GetPlaneInfo — read actual sensor register (shutter/gain).
	 * Optional: only used for cold-boot enforcement. */
	s->h_sensor = dlopen("libmi_sensor.so", RTLD_LAZY | RTLD_GLOBAL);
	if (s->h_sensor) {
		s->fn_get_plane_info = (fn_snr_get_plane_info_t)dlsym(
			s->h_sensor, "MI_SNR_GetPlaneInfo");
		s->fn_set_fps = (fn_snr_set_fps_t)dlsym(
			s->h_sensor, "MI_SNR_SetFps");
	}

	if (!s->fn_get_hw_stats || !s->fn_get_ae_status) {
		fprintf(stderr, "[cus3a] missing required AE symbols\n");
		return -1;
	}

	if (!s->fn_get_exposure_limit || !s->fn_set_exposure_limit) {
		fprintf(stderr,
			"[cus3a] missing exposure limit symbols\n");
		return -1;
	}

	return 0;
}

static void release_symbols(Cus3aState *s)
{
	if (s->h_sensor)
		dlclose(s->h_sensor);
	if (s->h_cus3a)
		dlclose(s->h_cus3a);
	if (s->h_isp)
		dlclose(s->h_isp);
	s->h_sensor = NULL;
	s->h_isp = NULL;
	s->h_cus3a = NULL;
}

/* ── Supervisory thread ──────────────────────────────────────────────── */

static void *cus3a_thread(void *arg)
{
	Cus3aState *s = arg;
	Cus3aAeHwStats *ae_hw = NULL;
	Cus3aAeInfo ae_info;
	Cus3aIspExposureLimit cur_limit;
	int fd0 = 0, fd1 = 0;
	int have_sync = 0;
	unsigned int sleep_ms;
	unsigned long frames = 0, limit_writes = 0;
	unsigned long last_log_ms = 0;
	uint32_t applied_shutter_max = 0;
	uint32_t applied_gain_max = 0;
	int fps_kick_done = 0;

	ae_hw = malloc(sizeof(Cus3aAeHwStats));
	if (!ae_hw) {
		fprintf(stderr, "[cus3a] AE stats alloc failed\n");
		return NULL;
	}

	/* Open frame sync if available */
	if (s->fn_open_sync) {
		int sync_ret = s->fn_open_sync(&fd0, &fd1);
		have_sync = (sync_ret == 0);
	}

	/* Verify ISP AE is in NORMAL state */
	if (s->fn_ae_get_state) {
		int state = -1;
		s->fn_ae_get_state(0, &state);
		if (state != CUS3A_ISP_STATE_NORMAL)
			fprintf(stderr,
				"[cus3a] WARNING: ISP AE not NORMAL "
				"(state=%d), exposure may not converge\n",
				state);
		else if (s->cfg.verbose)
			printf("[cus3a] ISP AE state: NORMAL\n");
	}

	/* Read current limits as baseline */
	memset(&cur_limit, 0, sizeof(cur_limit));
	s->fn_get_exposure_limit(0, &cur_limit);

	/* Apply initial constraints — always write the limit on startup.
	 * On cold boot the ISP bin AE may have set the sensor register to
	 * a shutter longer than the frame period.  SetExposureLimit was
	 * already called by pipeline init but the ISP bin AE ignores it
	 * for the physical sensor register.  Force a re-write here so the
	 * ISP AE re-evaluates with the correct limit on its next cycle. */
	{
		uint32_t want_shutter = compute_max_shutter(s);
		uint32_t want_gain = s->gain_max;  /* 0 = use bin default */

		if (want_shutter > 0 &&
		    want_shutter <= cur_limit.maxShutterUs)
			cur_limit.maxShutterUs = want_shutter;
		if (want_gain > 0 &&
		    want_gain <= cur_limit.maxSensorGain)
			cur_limit.maxSensorGain = want_gain;

		/* Always write — even if the limit value matches, the sensor
		 * register may not comply after a cold boot ISP bin load. */
		s->fn_set_exposure_limit(0, &cur_limit);
		limit_writes++;
		if (s->cfg.verbose)
			printf("[cus3a] initial limits enforced: "
				"maxShutter=%uus maxGain=%u\n",
				cur_limit.maxShutterUs,
				cur_limit.maxSensorGain);

		applied_shutter_max = cur_limit.maxShutterUs;
		applied_gain_max = cur_limit.maxSensorGain;
	}

	sleep_ms = s->cfg.ae_fps > 0 ? 1000 / s->cfg.ae_fps : 66;
	if (sleep_ms < 1)
		sleep_ms = 1;
	if (s->cfg.verbose)
		printf("[cus3a] supervisory thread started: %u Hz, "
			"shutter cap %uus, gain cap %u\n",
			s->cfg.ae_fps, applied_shutter_max,
			applied_gain_max);

	while (s->running) {
		/* Wait for frame boundary or sleep.  Frame sync may stop
		 * working after the CUS3A handoff (0,0,0) — fall back to
		 * nanosleep if that happens. */
		if (have_sync && s->fn_wait_sync) {
			unsigned int status = s->fn_wait_sync(
				fd0, fd1, 500);
			if (status == 0) {
				if (s->cfg.verbose)
					printf("[cus3a] frame sync ended, "
						"switching to timer\n");
				if (s->fn_close_sync)
					s->fn_close_sync(fd0, fd1);
				have_sync = 0;
			}
		}

		/* Read HW stats for avg_y monitoring */
		if (s->fn_get_hw_stats(0, ae_hw) != 0)
			goto next;

		memset(&ae_info, 0, sizeof(ae_info));
		s->fn_get_ae_status(0, &ae_info);

		/* Check if runtime limits changed */
		{
			uint32_t want_shutter = compute_max_shutter(s);
			uint32_t want_gain = s->gain_max;
			uint32_t effective_gain = want_gain > 0 ?
				want_gain : s->bin_max_sensor_gain;
			int changed = 0;

			/* Re-read current limits from ISP (they may have
			 * been changed externally by cap_exposure_for_fps) */
			s->fn_get_exposure_limit(0, &cur_limit);

			if (want_shutter > 0 &&
			    want_shutter != applied_shutter_max) {
				cur_limit.maxShutterUs = want_shutter;
				applied_shutter_max = want_shutter;
				changed = 1;
			}
			if (effective_gain > 0 &&
			    effective_gain != applied_gain_max) {
				cur_limit.maxSensorGain = effective_gain;
				applied_gain_max = effective_gain;
				changed = 1;
			}

			/* Cold-boot fix: the ISP bin AE may initialize
			 * the sensor at a shutter above our cap.
			 * SetExposureLimit alone does not force the
			 * physical sensor register on cold boot.
			 * After 15 ticks (~1s), if still stuck, call
			 * MI_SNR_SetFps to force the sensor driver to
			 * reconfigure its timing registers, then re-set
			 * the exposure limit.  Only fires once. */
			if (frames > 0 && frames <= 15 &&
			    !fps_kick_done &&
			    s->fn_get_plane_info &&
			    applied_shutter_max > 0) {
				MI_SNR_PlaneInfo_t pi;
				memset(&pi, 0, sizeof(pi));
				if (s->fn_get_plane_info(0, 0,
				    &pi) == 0 &&
				    pi.shutter >
				    applied_shutter_max) {
					cur_limit.maxShutterUs =
						applied_shutter_max;
					changed = 1;
					if (frames == 15 &&
					    s->fn_set_fps &&
					    s->cfg.sensor_fps > 0) {
						printf("[cus3a] sensor "
						    "shutter %uus stuck "
						    "above %uus, "
						    "kicking via "
						    "SetFps(%u)\n",
						    pi.shutter,
						    applied_shutter_max,
						    s->cfg.sensor_fps);
						s->fn_set_fps(0,
						    s->cfg.sensor_fps);
						fps_kick_done = 1;
					}
				}
			}

			if (changed) {
				s->fn_set_exposure_limit(0, &cur_limit);
				limit_writes++;
				if (s->cfg.verbose && frames >= 15)
					printf("[cus3a] limits updated: "
						"maxShutter=%uus "
						"maxGain=%u\n",
						cur_limit.maxShutterUs,
						cur_limit.maxSensorGain);
			}
		}

		frames++;

		/* Log every 5 seconds */
		{
			struct timespec ts;
			unsigned long now_ms;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			now_ms = ts.tv_sec * 1000UL +
				ts.tv_nsec / 1000000;
			if (now_ms - last_log_ms >= 5000) {
				if (s->cfg.verbose) {
					unsigned int total =
						ae_info.AvgBlkX *
						ae_info.AvgBlkY;
					unsigned long sum = 0;
					unsigned int avg_y = 0;
					unsigned int n;
					int ae_state = -1;
					const char *state_str;

					if (total == 0 ||
					    total > CUS3A_AE_GRID_SZ)
						total = CUS3A_AE_GRID_SZ;
					/* NEON histogram benchmarked at only 1.1x on
					 * Cortex-A7 (vld4_s16 deinterleave overhead
					 * vs branch elimination). Keep scalar. */
					for (n = 0; n < total; n++) {
						short y =
							ae_hw->nAvg[n].y;
						sum += y > 0 ?
							(unsigned long)y : 0;
					}
					avg_y = (unsigned int)(sum / total);

					if (s->fn_ae_get_state)
						s->fn_ae_get_state(0,
							&ae_state);
					state_str =
						ae_state ==
						CUS3A_ISP_STATE_NORMAL ?
						"normal" : "?";
					printf("[cus3a] %lu frames, "
						"%lu limit writes, "
						"shutter=%uus "
						"gain=%u avgY=%u "
						"isp_ae=%s\n",
						frames, limit_writes,
						ae_info.Shutter,
						ae_info.SensorGain,
						avg_y, state_str);
				}
				last_log_ms = now_ms;
			}
		}

next:
		{
			unsigned long ns = sleep_ms * 1000000UL;
			struct timespec req = {
				(time_t)(ns / 1000000000UL),
				(long)(ns % 1000000000UL)
			};
			nanosleep(&req, NULL);
		}
	}

	if (have_sync && s->fn_close_sync)
		s->fn_close_sync(fd0, fd1);

	free(ae_hw);
	if (s->cfg.verbose)
		printf("[cus3a] thread stopped (%lu frames, "
			"%lu limit writes)\n", frames, limit_writes);
	return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int star6e_cus3a_start(const Star6eCus3aConfig *cfg)
{
	int ret;

	if (g_cus3a.running) {
		fprintf(stderr, "[cus3a] already running\n");
		return -1;
	}

	memset(&g_cus3a, 0, sizeof(g_cus3a));
	g_cus3a.cfg = *cfg;
	g_cus3a.shutter_max_us = cfg->shutter_max_us;
	g_cus3a.gain_max = cfg->gain_max;

	if (resolve_symbols(&g_cus3a) != 0) {
		release_symbols(&g_cus3a);
		return -1;
	}

	/* Read ISP bin exposure limits to know the baseline */
	{
		Cus3aIspExposureLimit lim;
		memset(&lim, 0, sizeof(lim));
		if (g_cus3a.fn_get_exposure_limit(0, &lim) == 0 &&
		    lim.maxSensorGain > 0) {
			g_cus3a.bin_max_shutter_us = lim.maxShutterUs;
			g_cus3a.bin_max_sensor_gain = lim.maxSensorGain;
			if (cfg->verbose)
				printf("[cus3a] ISP bin limits: "
					"gain %u-%u, isp_gain max %u, "
					"shutter %u-%uus\n",
					lim.minSensorGain,
					lim.maxSensorGain,
					lim.maxIspGain,
					lim.minShutterUs,
					lim.maxShutterUs);
		} else if (cfg->verbose) {
			printf("[cus3a] ISP bin limits not available\n");
		}
	}

	g_cus3a.running = 1;
	ret = pthread_create(&g_cus3a.thread, NULL, cus3a_thread, &g_cus3a);
	if (ret != 0) {
		fprintf(stderr, "[cus3a] thread create failed: %d\n", ret);
		g_cus3a.running = 0;
		release_symbols(&g_cus3a);
		return -1;
	}

	return 0;
}

void star6e_cus3a_stop(void)
{
	star6e_cus3a_request_stop();
	star6e_cus3a_join();
}

void star6e_cus3a_request_stop(void)
{
	if (!g_cus3a.running)
		return;
	g_cus3a.running = 0;
}

void star6e_cus3a_join(void)
{
	/* Join is only needed if the thread was started (running was 1
	 * before request_stop set it to 0).  Check the thread handle. */
	if (g_cus3a.h_isp == NULL && g_cus3a.h_cus3a == NULL)
		return;  /* never started or already joined */

	pthread_join(g_cus3a.thread, NULL);
	release_symbols(&g_cus3a);
	printf("[cus3a] stopped\n");
}

int star6e_cus3a_running(void)
{
	return g_cus3a.running;
}

void star6e_cus3a_set_gain_max(uint32_t gain)
{
	g_cus3a.gain_max = gain;
}
