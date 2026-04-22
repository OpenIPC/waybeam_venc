#include "pipeline_common.h"

#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

uint32_t pipeline_common_frame_lost_threshold(uint32_t kbps)
{
	uint32_t bits, margin;

	if (kbps > 200000U)
		kbps = 200000U;
	bits = kbps * 1024U;
	margin = bits / 5U;
	if (margin < 524288U)
		margin = 524288U;
	return bits + margin;
}

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

int pipeline_common_cap_exposure_for_fps(uint32_t fps)
{
	isp_get_exposure_limit_fn_t fn_get;
	isp_set_exposure_limit_fn_t fn_set;
	PipelineIspExposureLimit config;
	void *handle;
	uint32_t target_us;
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

	target_us = 1000000 / fps;
	if (config.maxShutterUs <= target_us) {
		printf("> Exposure cap: maxShutter %uus (already <= %uus for %u fps), enforcing\n",
			config.maxShutterUs, target_us, fps);
	} else {
		printf("> Exposure cap: maxShutter %uus -> %uus (for %u fps)\n",
			config.maxShutterUs, target_us, fps);
	}

	config.maxShutterUs = target_us;
	ret = ISP_AE_CALL(fn_set, &config);
	if (ret != 0)
		fprintf(stderr, "WARNING: MI_ISP_AE_SetExposureLimit failed %d\n", ret);

	dlclose(handle);
	return ret;
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
