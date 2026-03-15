#include "star6e_cus3a.h"

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

/* CusAEResult_t — AE output to MI_ISP_CUS3A_SetAeParam. */
typedef struct {
	uint32_t Size;
	uint32_t Change;
	uint32_t Shutter;
	uint32_t SensorGain;
	uint32_t IspGain;
	uint32_t ShutterHdrShort;
	uint32_t SensorGainHdrShort;
	uint32_t IspGainHdrShort;
	uint32_t u4BVx16384;
	uint32_t AvgY;
	uint32_t HdrRatio;
} Cus3aAeResult;

/* HW AWB statistics block — 128x90 grid of {R,G,B} averages. */
typedef struct {
	uint8_t r;
	uint8_t g;
	uint8_t b;
} Cus3aAwbSample;

typedef struct {
	uint32_t nBlkX;
	uint32_t nBlkY;
	Cus3aAwbSample nAvg[CUS3A_AE_GRID_SZ];
} Cus3aAwbHwStats;

/* CusAWBInfo_t — current AWB state.  Layout matches star6e_controls.c
 * AwbCus3aInfo_t (proven to work via /api/v1/awb). */
typedef struct {
	uint32_t Size;
	uint32_t AvgBlkX;
	uint32_t AvgBlkY;
	uint32_t CurRGain;
	uint32_t CurGGain;
	uint32_t CurBGain;
	void *avgs;
	uint8_t HDRMode;
	void **pAwbStatisShort;
	uint32_t u4BVx16384;
	int32_t WeightY;
} __attribute__((packed, aligned(1))) Cus3aAwbInfo;

/* CusAWBResult_t — AWB output to MI_ISP_CUS3A_SetAwbParam. */
typedef struct {
	uint32_t Size;
	uint32_t Change;
	uint32_t R_gain;
	uint32_t G_gain;
	uint32_t B_gain;
	uint32_t ColorTmp;
} Cus3aAwbResult;

#define CUS3A_ISP_STATE_NORMAL 0
#define CUS3A_ISP_STATE_PAUSE  1

/* AWB gain limits */
#define AWB_GAIN_MIN  512   /* 0.5x */
#define AWB_GAIN_MAX  8192  /* 8x */
#define AWB_SMOOTH_OLD 7    /* IIR filter: 70% old + 30% new */
#define AWB_SMOOTH_NEW 3
#define AWB_DEADBAND_PCT 2  /* percent change threshold */

/* ── Function pointer types (resolved via dlsym) ─────────────────────── */

typedef int (*fn_ae_get_hw_stats_t)(int channel, Cus3aAeHwStats *stats);
typedef int (*fn_cus3a_get_ae_status_t)(int channel, Cus3aAeInfo *info);
typedef int (*fn_cus3a_set_ae_param_t)(int channel, Cus3aAeResult *result);
typedef int (*fn_ae_set_state_t)(int channel, int *state);
typedef int (*fn_ae_get_state_t)(int channel, int *state);
typedef int (*fn_cus3a_open_frame_sync_t)(int *fd0, int *fd1);
typedef int (*fn_cus3a_wait_frame_sync_t)(int fd0, int fd1, int timeout_ms);
typedef int (*fn_cus3a_close_frame_sync_t)(int fd0, int fd1);
typedef int (*fn_awb_get_hw_stats_t)(int channel, Cus3aAwbHwStats *stats);
typedef int (*fn_cus3a_get_awb_status_t)(int channel, Cus3aAwbInfo *info);
typedef int (*fn_cus3a_set_awb_param_t)(int channel, Cus3aAwbResult *result);
typedef int (*fn_cus3a_enable_t)(int channel, void *params);

/* ── Module state ────────────────────────────────────────────────────── */

typedef struct {
	/* config */
	Star6eCus3aConfig cfg;

	/* thread */
	pthread_t thread;
	volatile int running;
	volatile int awb_manual;  /* 1 = manual mode, skip auto AWB */

	/* AE symbols */
	void *h_isp;
	void *h_cus3a;
	fn_ae_get_hw_stats_t       fn_get_hw_stats;
	fn_cus3a_get_ae_status_t   fn_get_ae_status;
	fn_cus3a_set_ae_param_t    fn_set_ae_param;
	fn_ae_set_state_t          fn_ae_set_state;
	fn_ae_get_state_t          fn_ae_get_state;
	fn_cus3a_open_frame_sync_t  fn_open_sync;
	fn_cus3a_wait_frame_sync_t  fn_wait_sync;
	fn_cus3a_close_frame_sync_t fn_close_sync;

	/* AWB symbols (optional — NULL if not available) */
	fn_awb_get_hw_stats_t      fn_get_awb_hw_stats;
	fn_cus3a_get_awb_status_t  fn_get_awb_status;
	fn_cus3a_set_awb_param_t   fn_set_awb_param;
	fn_cus3a_enable_t          fn_cus3a_enable;
} Cus3aState;

static Cus3aState g_cus3a;

/* ── Helpers ─────────────────────────────────────────────────────────── */

void star6e_cus3a_config_defaults(Star6eCus3aConfig *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->sensor_fps = 120;
	cfg->ae_fps = 15;
	cfg->target_y_low = 100;
	cfg->target_y_high = 140;
	cfg->change_pct = 10;
	cfg->gain_min = 1024;        /* 1x */
	cfg->gain_max = 20480;       /* 20x — FPV-oriented, low noise */
	cfg->shutter_min_us = 150;
	cfg->shutter_max_us = 0;     /* auto from sensor_fps */
}

static uint32_t compute_max_shutter(const Star6eCus3aConfig *cfg)
{
	if (cfg->shutter_max_us > 0)
		return cfg->shutter_max_us;
	if (cfg->sensor_fps > 0)
		return 1000000 / cfg->sensor_fps;
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

	/* AE symbols (required) */
	s->fn_get_hw_stats = (fn_ae_get_hw_stats_t)dlsym(
		s->h_isp, "MI_ISP_AE_GetAeHwAvgStats");
	s->fn_get_ae_status = (fn_cus3a_get_ae_status_t)dlsym(
		s->h_isp, "MI_ISP_CUS3A_GetAeStatus");
	s->fn_set_ae_param = (fn_cus3a_set_ae_param_t)dlsym(
		s->h_isp, "MI_ISP_CUS3A_SetAeParam");
	s->fn_ae_set_state = (fn_ae_set_state_t)dlsym(
		s->h_isp, "MI_ISP_AE_SetState");
	s->fn_ae_get_state = (fn_ae_get_state_t)dlsym(
		s->h_isp, "MI_ISP_AE_GetState");
	s->fn_open_sync = (fn_cus3a_open_frame_sync_t)dlsym(
		s->h_cus3a, "Cus3AOpenIspFrameSync");
	s->fn_wait_sync = (fn_cus3a_wait_frame_sync_t)dlsym(
		s->h_cus3a, "Cus3AWaitIspFrameSync");
	s->fn_close_sync = (fn_cus3a_close_frame_sync_t)dlsym(
		s->h_cus3a, "Cus3ACloseIspFrameSync");

	if (!s->fn_get_hw_stats || !s->fn_get_ae_status ||
	    !s->fn_set_ae_param) {
		fprintf(stderr, "[cus3a] missing required AE symbols\n");
		return -1;
	}

	/* AWB symbols (optional — degrade gracefully) */
	s->fn_get_awb_hw_stats = (fn_awb_get_hw_stats_t)dlsym(
		s->h_isp, "MI_ISP_AWB_GetAwbHwAvgStats");
	s->fn_get_awb_status = (fn_cus3a_get_awb_status_t)dlsym(
		s->h_isp, "MI_ISP_CUS3A_GetAwbStatus");
	s->fn_set_awb_param = (fn_cus3a_set_awb_param_t)dlsym(
		s->h_isp, "MI_ISP_CUS3A_SetAwbParam");
	s->fn_cus3a_enable = (fn_cus3a_enable_t)dlsym(
		s->h_isp, "MI_ISP_CUS3A_Enable");

	if (!s->fn_get_awb_hw_stats || !s->fn_get_awb_status ||
	    !s->fn_set_awb_param)
		printf("[cus3a] AWB symbols not found — AWB stays ISP-managed\n");

	return 0;
}

static void release_symbols(Cus3aState *s)
{
	if (s->h_cus3a)
		dlclose(s->h_cus3a);
	if (s->h_isp)
		dlclose(s->h_isp);
	s->h_isp = NULL;
	s->h_cus3a = NULL;
}

static int awb_available(const Cus3aState *s)
{
	return s->fn_get_awb_hw_stats && s->fn_get_awb_status &&
		s->fn_set_awb_param;
}

/* ── AE algorithm ────────────────────────────────────────────────────── */

static void do_ae(const Cus3aState *s, const Cus3aAeHwStats *hw_stats,
	const Cus3aAeInfo *ae_info, Cus3aAeResult *result)
{
	const Star6eCus3aConfig *cfg = &s->cfg;
	uint32_t max_shutter = compute_max_shutter(cfg);
	unsigned int total = ae_info->AvgBlkX * ae_info->AvgBlkY;
	unsigned long sum = 0;
	unsigned int avg_y;
	unsigned int n;

	if (total == 0 || total > CUS3A_AE_GRID_SZ)
		total = CUS3A_AE_GRID_SZ;

	for (n = 0; n < total; n++) {
		short y = hw_stats->nAvg[n].y;
		sum += y > 0 ? (unsigned long)y : 0;
	}
	avg_y = (unsigned int)(sum / total);

	/* Start from current values — clamp to sane minimums to avoid
	 * getting stuck at zero (can happen on first frame). */
	result->Size = sizeof(Cus3aAeResult);
	result->Change = 0;
	result->Shutter = ae_info->Shutter >= cfg->shutter_min_us ?
		ae_info->Shutter : cfg->shutter_min_us;
	result->SensorGain = ae_info->SensorGain >= cfg->gain_min ?
		ae_info->SensorGain : cfg->gain_min;
	result->IspGain = 1024;
	result->u4BVx16384 = 16384;
	result->HdrRatio = 10;
	result->AvgY = avg_y;
	result->ShutterHdrShort = 2000;
	result->SensorGainHdrShort = 1024;
	result->IspGainHdrShort = 1024;

	/* Dead-band: no change if luma is within target range */
	if ((int)avg_y >= cfg->target_y_low && (int)avg_y <= cfg->target_y_high)
		return;

	/* Too dark: increase shutter first, then gain */
	if ((int)avg_y < cfg->target_y_low) {
		if (result->Shutter < max_shutter) {
			result->Shutter += result->Shutter * cfg->change_pct / 100;
			if (result->Shutter > max_shutter)
				result->Shutter = max_shutter;
		} else {
			result->SensorGain += result->SensorGain *
				cfg->change_pct / 100;
			if (result->SensorGain > cfg->gain_max)
				result->SensorGain = cfg->gain_max;
		}
		result->Change = 1;
	}

	/* Too bright: decrease gain first, then shutter */
	else if ((int)avg_y > cfg->target_y_high) {
		if (result->SensorGain > cfg->gain_min) {
			result->SensorGain -= result->SensorGain *
				cfg->change_pct / 100;
			if (result->SensorGain < cfg->gain_min)
				result->SensorGain = cfg->gain_min;
		} else {
			result->Shutter -= result->Shutter *
				cfg->change_pct / 100;
			if (result->Shutter < cfg->shutter_min_us)
				result->Shutter = cfg->shutter_min_us;
		}
		result->Change = 1;
	}
}

/* ── AWB algorithm (grey-world with IIR smoothing) ───────────────────── */

static void do_awb(const Cus3aAwbHwStats *hw_stats,
	const Cus3aAwbInfo *awb_info, Cus3aAwbResult *result)
{
	unsigned int total = hw_stats->nBlkX * hw_stats->nBlkY;
	unsigned long sum_r = 0, sum_g = 0, sum_b = 0;
	unsigned int avg_r, avg_g, avg_b;
	uint32_t target_r, target_b;
	int r_delta, b_delta, r_thresh, b_thresh;
	unsigned int n;

	if (total == 0 || total > CUS3A_AE_GRID_SZ)
		total = CUS3A_AE_GRID_SZ;

	for (n = 0; n < total; n++) {
		sum_r += hw_stats->nAvg[n].r;
		sum_g += hw_stats->nAvg[n].g;
		sum_b += hw_stats->nAvg[n].b;
	}

	avg_r = (unsigned int)(sum_r / total);
	avg_g = (unsigned int)(sum_g / total);
	avg_b = (unsigned int)(sum_b / total);

	result->Size = sizeof(Cus3aAwbResult);
	result->Change = 0;
	result->ColorTmp = 5500;

	/* Keep current gains if stats are degenerate */
	if (avg_r == 0 || avg_g == 0 || avg_b == 0) {
		result->R_gain = awb_info->CurRGain;
		result->G_gain = awb_info->CurGGain;
		result->B_gain = awb_info->CurBGain;
		return;
	}

	/* Grey-world: normalize R and B channels to match G.
	 * G_gain stays at unity (1024). */
	target_r = (uint32_t)((uint64_t)avg_g * 1024 / avg_r);
	target_b = (uint32_t)((uint64_t)avg_g * 1024 / avg_b);

	/* IIR smoothing: 70% old + 30% new to prevent oscillation */
	result->R_gain = (awb_info->CurRGain * AWB_SMOOTH_OLD +
		target_r * AWB_SMOOTH_NEW) /
		(AWB_SMOOTH_OLD + AWB_SMOOTH_NEW);
	result->G_gain = 1024;
	result->B_gain = (awb_info->CurBGain * AWB_SMOOTH_OLD +
		target_b * AWB_SMOOTH_NEW) /
		(AWB_SMOOTH_OLD + AWB_SMOOTH_NEW);

	/* Clamp to reasonable range */
	if (result->R_gain < AWB_GAIN_MIN) result->R_gain = AWB_GAIN_MIN;
	if (result->R_gain > AWB_GAIN_MAX) result->R_gain = AWB_GAIN_MAX;
	if (result->B_gain < AWB_GAIN_MIN) result->B_gain = AWB_GAIN_MIN;
	if (result->B_gain > AWB_GAIN_MAX) result->B_gain = AWB_GAIN_MAX;

	/* Dead-band: skip update if change is below threshold */
	r_delta = (int)result->R_gain - (int)awb_info->CurRGain;
	b_delta = (int)result->B_gain - (int)awb_info->CurBGain;
	r_thresh = (int)(awb_info->CurRGain * AWB_DEADBAND_PCT / 100);
	b_thresh = (int)(awb_info->CurBGain * AWB_DEADBAND_PCT / 100);
	if (r_thresh < 10) r_thresh = 10;
	if (b_thresh < 10) b_thresh = 10;

	if (abs(r_delta) < r_thresh && abs(b_delta) < b_thresh) {
		result->R_gain = awb_info->CurRGain;
		result->G_gain = awb_info->CurGGain;
		result->B_gain = awb_info->CurBGain;
		return;
	}

	result->Change = 1;
}

/* ── 3A thread ───────────────────────────────────────────────────────── */

static void *cus3a_thread(void *arg)
{
	Cus3aState *s = arg;
	Cus3aAeHwStats *ae_hw = NULL;
	Cus3aAwbHwStats *awb_hw = NULL;
	Cus3aAeInfo ae_info;
	Cus3aAeResult ae_result;
	Cus3aAwbInfo awb_info;
	Cus3aAwbResult awb_result;
	uint32_t last_awb_r = 0, last_awb_g = 0, last_awb_b = 0;
	int fd0 = 0, fd1 = 0;
	int have_sync = 0;
	int have_awb = 0;
	unsigned int sleep_ms;
	unsigned long frames = 0, ae_changes = 0, awb_changes = 0;
	unsigned long last_log_ms = 0;

	ae_hw = malloc(sizeof(Cus3aAeHwStats));
	if (!ae_hw) {
		fprintf(stderr, "[cus3a] AE stats alloc failed\n");
		return NULL;
	}

	/* Allocate AWB stats if symbols available */
	if (awb_available(s)) {
		awb_hw = malloc(sizeof(Cus3aAwbHwStats));
		if (!awb_hw)
			fprintf(stderr, "[cus3a] AWB stats alloc failed\n");
	}
	have_awb = (awb_hw != NULL);

	/* Open frame sync if available */
	if (s->fn_open_sync) {
		int sync_ret = s->fn_open_sync(&fd0, &fd1);
		have_sync = (sync_ret == 0);
	}

	/* Pause ISP internal AE */
	if (s->fn_ae_set_state) {
		int pause = CUS3A_ISP_STATE_PAUSE;
		int ret = s->fn_ae_set_state(0, &pause);
		printf("[cus3a] ISP AE pause request (ret=%d)\n", ret);
	}

	/* Verify ISP AE is actually paused */
	if (s->fn_ae_get_state) {
		int state = -1;
		int ret = s->fn_ae_get_state(0, &state);
		printf("[cus3a] ISP AE state after pause: %s (raw=%d, ret=%d)\n",
			state == CUS3A_ISP_STATE_PAUSE ? "PAUSED" :
			state == CUS3A_ISP_STATE_NORMAL ? "NORMAL(!)" :
			"UNKNOWN", state, ret);
		if (state != CUS3A_ISP_STATE_PAUSE)
			fprintf(stderr, "[cus3a] WARNING: ISP AE did not pause "
				"— internal AE may override custom AE\n");
	}

	/* Disable CUS3A AWB/AF callbacks — we drive AWB from this thread */
	if (have_awb && s->fn_cus3a_enable) {
		int p100[13] = {1, 0, 0};
		int ret = s->fn_cus3a_enable(0, p100);
		printf("[cus3a] CUS3A AWB/AF disabled (ret=%d)\n", ret);
	}

	sleep_ms = s->cfg.ae_fps > 0 ? 1000 / s->cfg.ae_fps : 66;
	if (sleep_ms < 1)
		sleep_ms = 1;
	printf("[cus3a] thread started: %u Hz, target Y %d-%d, "
		"gain %u-%u, shutter %u-%uus, step %d%%, awb=%s\n",
		s->cfg.ae_fps, s->cfg.target_y_low, s->cfg.target_y_high,
		s->cfg.gain_min, s->cfg.gain_max,
		s->cfg.shutter_min_us, compute_max_shutter(&s->cfg),
		s->cfg.change_pct, have_awb ? "on" : "off");

	while (s->running) {
		/* Wait for frame boundary or sleep */
		if (have_sync && s->fn_wait_sync) {
			unsigned int status = s->fn_wait_sync(fd0, fd1, 500);
			if (status == 0) {
				fprintf(stderr, "[cus3a] frame sync lost\n");
				break;
			}
		}

		/* ── AE ──────────────────────────────────────────────── */
		if (s->fn_get_hw_stats(0, ae_hw) != 0)
			goto next;

		memset(&ae_info, 0, sizeof(ae_info));
		if (s->fn_get_ae_status(0, &ae_info) != 0)
			goto next;

		if (frames < 3) {
			printf("[cus3a] AeInfo: Shutter=%u SensorGain=%u "
				"IspGain=%u AvgBlkX=%u AvgBlkY=%u\n",
				ae_info.Shutter, ae_info.SensorGain,
				ae_info.IspGain, ae_info.AvgBlkX,
				ae_info.AvgBlkY);
		}

		memset(&ae_result, 0, sizeof(ae_result));
		do_ae(s, ae_hw, &ae_info, &ae_result);

		if (ae_result.Change) {
			s->fn_set_ae_param(0, &ae_result);
			ae_changes++;
		}

		/* ── AWB ─────────────────────────────────────────────── */
		if (have_awb && !s->awb_manual) {
			if (s->fn_get_awb_hw_stats(0, awb_hw) == 0) {
				memset(&awb_info, 0, sizeof(awb_info));
				awb_info.Size = sizeof(awb_info);
				if (s->fn_get_awb_status(0, &awb_info) == 0) {
					last_awb_r = awb_info.CurRGain;
					last_awb_g = awb_info.CurGGain;
					last_awb_b = awb_info.CurBGain;
					if (frames < 3) {
						printf("[cus3a] AwbInfo: "
							"R=%u G=%u B=%u\n",
							last_awb_r,
							last_awb_g,
							last_awb_b);
					}
					memset(&awb_result, 0,
						sizeof(awb_result));
					do_awb(awb_hw, &awb_info,
						&awb_result);
					if (awb_result.Change) {
						s->fn_set_awb_param(0,
							&awb_result);
						awb_changes++;
					}
				}
			}
		}

		frames++;

		/* Log every 5 seconds + verify ISP AE still paused */
		{
			struct timespec ts;
			unsigned long now_ms;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			now_ms = ts.tv_sec * 1000UL + ts.tv_nsec / 1000000;
			if (now_ms - last_log_ms >= 5000) {
				int ae_state = -1;
				const char *state_str = "?";
				if (s->fn_ae_get_state) {
					s->fn_ae_get_state(0, &ae_state);
					state_str = ae_state ==
						CUS3A_ISP_STATE_PAUSE ?
						"paused" : "ACTIVE(!)";
				}
				printf("[cus3a] %lu frames, ae=%lu awb=%lu, "
					"shutter=%uus gain=%u avgY=%u "
					"wb=R%u/G%u/B%u isp_ae=%s\n",
					frames, ae_changes, awb_changes,
					ae_result.Shutter,
					ae_result.SensorGain,
					ae_result.AvgY,
					last_awb_r, last_awb_g, last_awb_b,
					state_str);
				/* Re-pause if something re-enabled ISP AE */
				if (ae_state == CUS3A_ISP_STATE_NORMAL &&
				    s->fn_ae_set_state) {
					int pause = CUS3A_ISP_STATE_PAUSE;
					s->fn_ae_set_state(0, &pause);
					fprintf(stderr, "[cus3a] re-paused "
						"ISP internal AE\n");
				}
				last_log_ms = now_ms;
			}
		}

next:
		{
			struct timespec req = {0, sleep_ms * 1000000UL};
			nanosleep(&req, NULL);
		}
	}

	/* Resume ISP internal AE */
	if (s->fn_ae_set_state) {
		int normal = CUS3A_ISP_STATE_NORMAL;
		s->fn_ae_set_state(0, &normal);
		printf("[cus3a] ISP AE resumed\n");
	}

	/* Re-enable CUS3A AWB/AF */
	if (have_awb && s->fn_cus3a_enable) {
		int p111[13] = {1, 1, 1};
		s->fn_cus3a_enable(0, p111);
		printf("[cus3a] CUS3A AWB/AF re-enabled\n");
	}

	if (have_sync && s->fn_close_sync)
		s->fn_close_sync(fd0, fd1);

	free(awb_hw);
	free(ae_hw);
	printf("[cus3a] thread stopped (%lu frames, %lu ae, %lu awb)\n",
		frames, ae_changes, awb_changes);
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

	if (resolve_symbols(&g_cus3a) != 0) {
		release_symbols(&g_cus3a);
		return -1;
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

void star6e_cus3a_set_shutter_max(uint32_t max_us)
{
	if (max_us > 0)
		g_cus3a.cfg.shutter_max_us = max_us;
}

void star6e_cus3a_set_awb_manual(int manual)
{
	g_cus3a.awb_manual = manual ? 1 : 0;
}
