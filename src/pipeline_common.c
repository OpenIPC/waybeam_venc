#include "pipeline_common.h"

#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

uint32_t pipeline_common_gop_frames(double gop_seconds, uint32_t fps)
{
	uint32_t gop_fps = fps ? fps : 30;
	uint32_t gop_frames;

	if (gop_seconds <= 0.0) {
		return 1;
	}

	gop_frames = (uint32_t)(gop_seconds * gop_fps + 0.5);
	return gop_frames < 1 ? 1 : gop_frames;
}

#ifndef PLATFORM_CV610
SensorSelectConfig pipeline_common_build_sensor_select_config(int forced_pad,
	int forced_mode, uint32_t target_width, uint32_t target_height,
	uint32_t target_fps)
{
	SensorSelectConfig cfg;

	cfg.forced_pad = forced_pad;
	cfg.forced_mode = forced_mode;
	cfg.target_width = target_width;
	cfg.target_height = target_height;
	cfg.target_fps = target_fps;
	return cfg;
}

void pipeline_common_report_selected_fps(const char *prefix,
	uint32_t requested_fps, const SensorSelectResult *sensor)
{
	const char *tag = prefix ? prefix : "";

	if (!sensor || requested_fps == sensor->fps) {
		return;
	}

	printf("> %sRequested %u fps, using %u fps (mode range %u-%u)\n",
		tag, requested_fps, sensor->fps, sensor->mode.minFps,
		sensor->mode.maxFps);
}
#endif

void pipeline_common_clamp_image_size(const char *prefix, uint32_t max_width,
	uint32_t max_height, uint32_t *image_width, uint32_t *image_height)
{
	const char *tag = prefix ? prefix : "";

	if (!image_width || !image_height) {
		return;
	}

	if (*image_width > max_width || *image_height > max_height) {
		printf("> %sRequested %ux%u, clamped to sensor max %ux%u\n",
			tag, *image_width, *image_height, max_width, max_height);
	}
	if (*image_width > max_width) {
		*image_width = max_width;
	}
	if (*image_height > max_height) {
		*image_height = max_height;
	}
}

/* Permissive gain ceiling used when the ISP has not yet populated its
 * exposure limits on cold boot.  High enough that AE can compensate
 * for the capped shutter; cus3a / ISP bin will tighten later. */
#define SYNTHETIC_MAX_GAIN 500000

/* ISP exposure limit structure — matches SigmaStar SDK ABI. */
typedef struct {
	unsigned int minShutterUs;
	unsigned int maxShutterUs;
	unsigned int minApertX10;
	unsigned int maxApertX10;
	unsigned int minSensorGain;
	unsigned int minIspGain;
	unsigned int maxSensorGain;
	unsigned int maxIspGain;
} PipelineIspExposureLimit;

/* On i6c (Maruko) ISP functions take (dev, channel, data*).
 * On i6 (Star6E) they take (channel, data*). */
#ifdef PLATFORM_MARUKO
typedef int (*isp_get_exposure_limit_fn_t)(int dev, int channel,
	PipelineIspExposureLimit *config);
typedef int (*isp_set_exposure_limit_fn_t)(int dev, int channel,
	PipelineIspExposureLimit *config);
#define ISP_AE_CALL(fn, cfg) fn(0, 0, cfg)
#else
typedef int (*isp_get_exposure_limit_fn_t)(int channel,
	PipelineIspExposureLimit *config);
typedef int (*isp_set_exposure_limit_fn_t)(int channel,
	PipelineIspExposureLimit *config);
#define ISP_AE_CALL(fn, cfg) fn(0, cfg)
#endif

int pipeline_common_cap_exposure_for_fps(uint32_t fps,
	bool shutter_rule_180)
{
#ifdef PLATFORM_CV610
	(void)fps;
	(void)shutter_rule_180;
	return -1;
#else
	isp_get_exposure_limit_fn_t fn_get;
	isp_set_exposure_limit_fn_t fn_set;
	PipelineIspExposureLimit config;
	void *handle;
	uint32_t target_us;
	uint32_t divisor;
	int ret;

	if (fps == 0)
		return 0;

	handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!handle)
		return -1;

	fn_get = (isp_get_exposure_limit_fn_t)dlsym(handle,
		"MI_ISP_AE_GetExposureLimit");
	fn_set = (isp_set_exposure_limit_fn_t)dlsym(handle,
		"MI_ISP_AE_SetExposureLimit");
	if (!fn_get || !fn_set) {
		dlclose(handle);
		return -1;
	}

	memset(&config, 0, sizeof(config));
	ret = ISP_AE_CALL(fn_get, &config);
	if (ret != 0) {
		dlclose(handle);
		return ret;
	}

	/* Poll until ISP populates exposure limits (up to 500 ms).
	 * On cold boot the ISP AE has not processed enough frames yet,
	 * so the struct comes back as all zeros.  Without the shutter
	 * cap the AE converges on an exposure time that exceeds the
	 * frame period, locking the pipeline at a lower framerate. */
	if (config.maxShutterUs == 0 && config.maxSensorGain == 0) {
		int waited_ms;

		for (waited_ms = 0; waited_ms < 500; waited_ms += 10) {
			usleep(10 * 1000);
			memset(&config, 0, sizeof(config));
			ret = ISP_AE_CALL(fn_get, &config);
			if (ret != 0) {
				dlclose(handle);
				return ret;
			}
			if (config.maxShutterUs != 0 || config.maxSensorGain != 0)
				break;
		}
		if (config.maxShutterUs == 0 && config.maxSensorGain == 0) {
			fprintf(stderr,
				"WARNING: ISP exposure limits not populated "
				"after 500 ms, using synthetic defaults\n");
			config.maxShutterUs = 1000000;
			config.maxSensorGain = SYNTHETIC_MAX_GAIN;
			config.maxIspGain = SYNTHETIC_MAX_GAIN;
		} else {
			printf("> ISP exposure limits populated after %d ms\n",
				waited_ms);
		}
	}

	divisor = shutter_rule_180 ? fps * 2 : fps;
	target_us = 1000000 / divisor;
	if (config.maxShutterUs <= target_us) {
		printf("> Exposure %s: maxShutter %uus (already <= %uus for %u fps%s), enforcing\n",
			shutter_rule_180 ? "pin" : "cap",
			config.maxShutterUs, target_us, fps,
			shutter_rule_180 ? ", 180\xc2\xb0 rule" : "");
	} else {
		printf("> Exposure %s: maxShutter %uus -> %uus (for %u fps%s)\n",
			shutter_rule_180 ? "pin" : "cap",
			config.maxShutterUs, target_us, fps,
			shutter_rule_180 ? ", 180\xc2\xb0 rule" : "");
	}

	config.maxShutterUs = target_us;
	if (shutter_rule_180)
		config.minShutterUs = target_us;
	ret = ISP_AE_CALL(fn_set, &config);
	if (ret != 0)
		fprintf(stderr, "WARNING: MI_ISP_AE_SetExposureLimit failed %d\n", ret);

	dlclose(handle);
	return ret;
#endif
}

PipelinePrecropRect pipeline_common_compute_precrop(uint32_t sensor_w,
	uint32_t sensor_h, uint32_t image_w, uint32_t image_h,
	bool keep_aspect)
{
	PipelinePrecropRect rect = {0, 0, (uint16_t)sensor_w, (uint16_t)sensor_h};
	uint64_t sensor_ar;
	uint64_t image_ar;

	if (!keep_aspect)
		return rect;

	sensor_ar = (uint64_t)sensor_w * image_h;
	image_ar = (uint64_t)image_w * sensor_h;

	if (sensor_ar > image_ar) {
		rect.h = (uint16_t)sensor_h;
		rect.w = (uint16_t)((sensor_h * image_w / image_h) & ~1u);
		rect.x = (uint16_t)(((sensor_w - rect.w) / 2) & ~1u);
		rect.y = 0;
	} else if (sensor_ar < image_ar) {
		rect.w = (uint16_t)sensor_w;
		rect.h = (uint16_t)((sensor_w * image_h / image_w) & ~1u);
		rect.x = 0;
		rect.y = (uint16_t)(((sensor_h - rect.h) / 2) & ~1u);
	}

	return rect;
}

/* Lowercase sensor_name into out_buf, stopping at the first non-alnum
 * character or out_sz-1 bytes.  Returns the number of characters written
 * (0 if sensor_name is NULL/empty or starts with a non-alnum). */
static size_t sensor_name_normalize(const char *sensor_name,
	char *out_buf, size_t out_sz)
{
	size_t w = 0;

	if (!sensor_name || !out_buf || out_sz == 0)
		return 0;
	while (sensor_name[w] && w + 1 < out_sz) {
		unsigned char c = (unsigned char)sensor_name[w];
		if (!isalnum(c))
			break;
		out_buf[w] = (char)tolower(c);
		w++;
	}
	out_buf[w] = '\0';
	return w;
}

int pipeline_common_resolve_isp_bin(const char *configured_path,
	const char *sensor_name, char *out_path, size_t out_sz)
{
	char fallback[256];
	char normalized[64];
	size_t name_len;

	if (!out_path || out_sz == 0)
		return 0;
	out_path[0] = '\0';

	if (configured_path && *configured_path) {
		if (access(configured_path, R_OK) == 0) {
			snprintf(out_path, out_sz, "%s", configured_path);
			printf("> ISP bin: %s (configured)\n", configured_path);
			return 1;
		}
		fprintf(stderr,
			"WARNING: ISP bin '%s' not readable, attempting fallback\n",
			configured_path);
	}

	name_len = sensor_name_normalize(sensor_name, normalized,
		sizeof(normalized));
	if (name_len == 0) {
		printf("> ISP bin: none (no path configured%s)\n",
			(sensor_name && *sensor_name) ?
				", sensor name unrecognized" :
				", sensor name unavailable");
		return 0;
	}

	snprintf(fallback, sizeof(fallback), "/etc/sensors/%s.bin", normalized);
	if (access(fallback, R_OK) == 0) {
		snprintf(out_path, out_sz, "%s", fallback);
		printf("> ISP bin: %s (auto-detected for sensor '%s')\n",
			fallback, normalized);
		return 1;
	}

	printf("> ISP bin: none (no fallback at %s for sensor '%s')\n",
		fallback, normalized);
	return 0;
}

static uint32_t roi_align_down(uint32_t value, uint32_t align)
{
	return value / align * align;
}

/* Window long enough that one IDR cannot carry it, short enough that a real
 * collapse is reported within a few seconds.  Two consecutive windows are
 * required, which is what separates a sustained loss of control from the
 * 1.43x single-window transient a moving scene produced on the bench. */
#define RATE_WATCH_WINDOW_US    2000000ULL
#define RATE_WATCH_TRIP_X100    150u
#define RATE_WATCH_CLEAR_X100   120u
#define RATE_WATCH_WINDOWS      2u

void pipeline_common_rate_watch(PipelineRateWatch *rw, const VencConfig *cfg,
	uint32_t frame_bytes, uint64_t now_us)
{
	uint64_t elapsed, delivered_kbps;
	uint32_t ratio_x100;

	/* cfg->video0.bitrate is read on the encode thread while the httpd
	 * thread may be committing a new config under g_cfg_mutex.  Deliberately
	 * unlocked, but NOT because the field is naturally aligned -- alignment
	 * is not a validity argument in C, and this reads bitrate more than once
	 * plus several ROI fields that can straddle one commit.
	 *
	 * The argument that actually carries the weight is that no concurrently
	 * committable value changes control flow here.  bitrate == 0 is the only
	 * one that could (it guards below and divides further down), and
	 * validation rejects it, defaults set 8192, and /api/v1/defaults commits
	 * defaults -- so no HTTP path can make the field transition to 0.  The
	 * residue is a diagnostic that may quote a target or an ROI hint from
	 * either side of a config change, and the two-window rule bounds even
	 * that: one window computed against a stale target cannot raise a report
	 * on its own.  Taking the config mutex once per encoded frame to protect
	 * a diagnostic would be the worse trade. */
	if (!rw || !cfg || cfg->video0.bitrate == 0)
		return;

	if (rw->window_start_us == 0)
		rw->window_start_us = now_us;
	rw->window_bytes += frame_bytes;

	/* Unsigned, so a clock that went backwards wraps huge rather than
	 * negative; treat any implausible span as a restart of the window. */
	elapsed = now_us - rw->window_start_us;
	if (now_us < rw->window_start_us) {
		rw->window_start_us = now_us;
		rw->window_bytes = 0;
		return;
	}
	if (elapsed < RATE_WATCH_WINDOW_US)
		return;

	/* bytes * 8 bits / (elapsed us / 1e6) / 1000 == bytes * 8000 / elapsed. */
	delivered_kbps = (rw->window_bytes * 8000ULL) / elapsed;
	ratio_x100 = (uint32_t)((delivered_kbps * 100ULL) / cfg->video0.bitrate);
	rw->window_start_us = now_us;
	rw->window_bytes = 0;

	if (ratio_x100 >= RATE_WATCH_TRIP_X100) {
		if (rw->over_windows < 255u)
			rw->over_windows++;
		if (rw->over_windows >= RATE_WATCH_WINDOWS && !rw->reported) {
			rw->reported = 1;
			if (rw->reports < UINT16_MAX)
				rw->reports++;
			fprintf(stderr,
				"WARNING: encoder delivered %u%% of the %u kbps target "
				"for %llu s -- rate control has no headroom left\n",
				ratio_x100, cfg->video0.bitrate,
				(unsigned long long)
					((RATE_WATCH_WINDOW_US * RATE_WATCH_WINDOWS)
						/ 1000000ULL));
			if (cfg->fpv.roi_enabled && cfg->fpv.roi_qp != 0)
				fprintf(stderr,
					"         fpv.roiQp is %+d against a QP ceiling of "
					"%s: the ROI delta is subtracted from the frame QP, "
					"so the controller raises the base QP to compensate "
					"and cannot go past that ceiling.  Reduce |roiQp| or "
					"raise video0.maxQp.\n",
					cfg->fpv.roi_qp,
					cfg->video0.max_qp ? "video0.maxQp" :
						"the encoder default");
		}
	} else if (ratio_x100 <= RATE_WATCH_CLEAR_X100) {
		if (rw->reported)
			printf("> Bitrate overrun cleared (%u%% of target)\n",
				ratio_x100);
		rw->over_windows = 0;
		rw->reported = 0;
	}
}

int pipeline_common_roi_band(uint32_t width, uint32_t height,
	float center_frac, int qp, int steps, int index,
	PipelineRoiBand *out)
{
	float frac;
	uint32_t rw, rh, rx;
	int level;

	if (!out || index < 0 || index >= steps)
		return -1;

	/* Clamp rather than trust the caller -- see the header comment.  Without
	 * it, frac outside [0,1] returns success with an underflowed origin or a
	 * multi-gigabyte width, and a negative frac is undefined behaviour at the
	 * float-to-unsigned conversion. */
	if (!(center_frac >= 0.1f))   /* also catches NaN */
		center_frac = 0.1f;
	else if (center_frac > 0.9f)
		center_frac = 0.9f;

	level = index + 1;
	frac = center_frac + (1.0f - center_frac) *
		(float)(steps - level) / (float)steps;
	rw = roi_align_down((uint32_t)(frac * width), 32);
	rh = roi_align_down(height, 32);
	rx = roi_align_down((width - rw) / 2, 32);
	if (rw == 0 || rh == 0)
		return -1;

	out->x = rx;
	out->y = 0;
	out->width = rw;
	out->height = rh;
	out->qp = pipeline_common_scale_roi_qp(qp, level, steps);
	return 0;
}
