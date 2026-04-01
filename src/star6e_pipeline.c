#include "star6e_pipeline.h"

#include "codec_config.h"
#include "codec_types.h"
#include "debug_osd.h"
#include "eis.h"
#include "star6e_controls.h"
#include "star6e_cus3a.h"
#include "file_util.h"
#include "isp_runtime.h"
#include "pipeline_common.h"
#include "venc_api.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
	unsigned int minShutterUs;
	unsigned int maxShutterUs;
	unsigned int minApertX10;
	unsigned int maxApertX10;
	unsigned int minSensorGain;
	unsigned int minIspGain;
	unsigned int maxSensorGain;
	unsigned int maxIspGain;
} IspExposureLimit;

typedef int (*isp_get_exposure_limit_fn_t)(int channel,
	IspExposureLimit *config);
typedef int (*isp_set_exposure_limit_fn_t)(int channel,
	IspExposureLimit *config);

typedef struct {
	uint16_t x;
	uint16_t y;
	uint16_t w;
	uint16_t h;
} Star6ePrecropRect;

typedef int (*isp_load_bin_fn_t)(int channel, char *path, unsigned int key);
typedef int (*isp_disable_userspace3a_fn_t)(int channel);
typedef int (*cus3a_fn_t)(int channel, void *params);

static void star6e_pipeline_reset(Star6ePipelineState *state)
{
	if (!state)
		return;

	star6e_video_reset(&state->video);
	memset(state, 0, sizeof(*state));
	star6e_output_reset(&state->output);
}

/* VPE SCL clock workaround — written at shutdown, effective on next start.
 *
 * Root cause: at process exit, mi_vpe_process_exit → MI_VPE_IMPL_DeInit →
 * DrvSclModuleClkDeInit disables the VPE SCL clock (fclk1) via
 * clk_disable_unprepare. On the next run, MI_VPE_IMPL_Init has a persistent
 * kernel-side "already_inited" flag that causes it to skip
 * DrvSclModuleClkInit, so fclk1 is never re-enabled.
 *
 * Writing after MI_SYS_Exit() triggers the preset path while the VPE fd is
 * closed. The preset persists in kernel memory until the next init. */
void star6e_pipeline_vpe_scl_preset_shutdown(void)
{
	int fd = open("/sys/devices/virtual/mstar/mscl/clk", O_WRONLY);

	if (fd < 0)
		return;

	(void)write(fd, "384000000\n", 10);
	close(fd);
	(void)write(STDERR_FILENO, "[venc] VPE SCL preset stored for next run\n",
		42);
}

/* Called after MI_SYS_Init() to silently tear down any pipeline state left
 * by an unclean previous exit. All errors are ignored. */
static void star6e_pipeline_pre_init_teardown(void)
{
	MI_SYS_ChnPort_t vif_port = {
		.module = I6_SYS_MOD_VIF, .device = 0, .channel = 0, .port = 0 };
	MI_SYS_ChnPort_t vpe_port = {
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0, .port = 0 };
	MI_SYS_ChnPort_t venc_port = {
		.module = I6_SYS_MOD_VENC, .device = 0, .channel = 0, .port = 0 };

	(void)MI_SYS_UnBindChnPort(&vpe_port, &venc_port);
	(void)MI_SYS_UnBindChnPort(&vif_port, &vpe_port);
	(void)MI_VENC_StopRecvPic(0);
	(void)MI_VENC_DestroyChn(0);
}

static int star6e_pipeline_disable_userspace3a(const IspRuntimeLib *lib,
	void *ctx)
{
	isp_disable_userspace3a_fn_t fn;

	(void)ctx;
	fn = (isp_disable_userspace3a_fn_t)lib->disable_userspace3a;
	return fn ? fn(0) : 0;
}

/* Poll MI_ISP_IQ_GetParaInitStatus until bFlag==1 or timeout (2000 ms).
 * Called standalone after VIF→VPE bind when a new VPE channel was just
 * created (first start or AR-change reinit): the ISP channel initialises
 * asynchronously after MI_VPE_CreateChannel returns, so anything that
 * touches the ISP (bin load, exposure cap) must wait here first.
 * Without this poll the ISP would emit "IspApiGet channel not created"
 * kernel errors on the first probe attempt. */
static void star6e_pipeline_wait_isp_channel(void)
{
	typedef struct { int bFlag; } IspParaInitInfoParam;
	typedef struct { IspParaInitInfoParam stParaAPI; } IspParaInitInfoType;
	typedef int (*fn_get_para_init_t)(int, IspParaInitInfoType *);
	fn_get_para_init_t fn;
	void *handle;
	int elapsed_ms = 0;

	handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!handle) {
		usleep(100 * 1000);
		return;
	}

	fn = (fn_get_para_init_t)dlsym(handle, "MI_ISP_IQ_GetParaInitStatus");
	if (!fn) {
		usleep(100 * 1000);
		dlclose(handle);
		return;
	}

	while (elapsed_ms < 2000) {
		IspParaInitInfoType info;

		memset(&info, 0, sizeof(info));
		if (fn(0, &info) == 0 && info.stParaAPI.bFlag == 1) {
			printf("> ISP channel ready after %d ms\n", elapsed_ms);
			dlclose(handle);
			return;
		}
		usleep(1000);
		elapsed_ms++;
	}

	fprintf(stderr, "WARNING: ISP channel readiness timeout after 2000 ms\n");
	dlclose(handle);
}

static int star6e_pipeline_wait_isp_ready(const IspRuntimeLib *lib, void *ctx)
{
	typedef struct { int bFlag; } IspParaInitInfoParam;
	typedef struct { IspParaInitInfoParam stParaAPI; } IspParaInitInfoType;
	typedef int (*fn_get_para_init_t)(int, IspParaInitInfoType *);
	fn_get_para_init_t fn;
	int elapsed_ms = 0;

	(void)ctx;
	fn = (fn_get_para_init_t)dlsym(lib->handle,
		"MI_ISP_IQ_GetParaInitStatus");
	if (!fn) {
		/* Symbol not available — fall back to fixed delay */
		usleep(100 * 1000);
		return 0;
	}

	while (elapsed_ms < 2000) {
		IspParaInitInfoType info;

		memset(&info, 0, sizeof(info));
		if (fn(0, &info) == 0 && info.stParaAPI.bFlag == 1) {
			printf("> ISP ready after %d ms\n", elapsed_ms);
			return 0;
		}
		usleep(1000);
		elapsed_ms++;
	}

	fprintf(stderr, "WARNING: ISP readiness timeout after 2000 ms\n");
	return -1;
}

static int star6e_pipeline_call_load_bin(const IspRuntimeLib *lib,
	const char *path, unsigned int load_key, void *ctx)
{
	isp_load_bin_fn_t fn_api;
	isp_load_bin_fn_t fn_api_alt;
	int ret;

	(void)ctx;
	fn_api = (isp_load_bin_fn_t)lib->load_bin_api;
	fn_api_alt = (isp_load_bin_fn_t)lib->load_bin_api_alt;
	ret = -1;
	if (fn_api)
		ret = fn_api(0, (char *)path, load_key);
	if (ret != 0 && fn_api_alt && fn_api_alt != fn_api)
		ret = fn_api_alt(0, (char *)path, load_key);
	return ret;
}

static void star6e_pipeline_post_load_cus3a(const IspRuntimeLib *lib,
	void *ctx)
{
	cus3a_fn_t fn_cus3a;
	int p100[13] = {1, 0, 0};
	int p110[13] = {1, 1, 0};

	(void)ctx;
	fn_cus3a = (cus3a_fn_t)lib->cus3a_enable;
	if (!fn_cus3a)
		return;

	fn_cus3a(0, p100);
	fn_cus3a(0, p110);
}

static int star6e_pipeline_load_isp_bin(const char *isp_bin_path,
	SdkQuietState *sdk_quiet)
{
	IspRuntimeLoadHooks hooks;

	if (!isp_bin_path || !*isp_bin_path)
		return 0;

	memset(&hooks, 0, sizeof(hooks));
	hooks.load_key = 1234;
	hooks.ctx = sdk_quiet;
	hooks.quiet_begin = (void (*)(void *))sdk_quiet_begin;
	hooks.quiet_end = (void (*)(void *))sdk_quiet_end;
	hooks.disable_userspace3a = star6e_pipeline_disable_userspace3a;
	hooks.wait_ready = star6e_pipeline_wait_isp_ready;
	hooks.load_bin = star6e_pipeline_call_load_bin;
	hooks.post_load = star6e_pipeline_post_load_cus3a;

	return isp_runtime_load_bin_file(isp_bin_path, &hooks);
}

static void star6e_pipeline_enable_cus3a(SdkQuietState *sdk_quiet)
{
	typedef int (*cus3a_fn_t)(int channel, void *params);
	void *handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	cus3a_fn_t fn;

	if (!handle)
		return;

	fn = (cus3a_fn_t)dlsym(handle, "MI_ISP_CUS3A_Enable");
	if (fn) {
		int p100[13] = {1, 0, 0};
		int p110[13] = {1, 1, 0};
		int p111[13] = {1, 1, 1};
		MI_S32 ret;

		sdk_quiet_begin(sdk_quiet);
		fn(0, p100);
		fn(0, p110);
		ret = fn(0, p111);
		sdk_quiet_end(sdk_quiet);
		if (ret != 0)
			fprintf(stderr, "WARNING: MI_ISP_CUS3A_Enable(1,1,1) failed %d\n", ret);
	}

	dlclose(handle);
}

static void star6e_pipeline_cus3a_apply(SdkQuietState *sdk_quiet,
	int params[13])
{
	static void *lib_handle = NULL;
	static int (*fn)(int channel, void *params) = NULL;
	static int initialized = 0;

	if (!initialized) {
		initialized = 1;
		lib_handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
		if (lib_handle) {
			fn = (int (*)(int, void *))dlsym(lib_handle,
				"MI_ISP_CUS3A_Enable");
		}
	}

	if (!fn)
		return;

	sdk_quiet_begin(sdk_quiet);
	fn(0, params);
	sdk_quiet_end(sdk_quiet);
}

static int g_cus3a_handoff_done = 0;

void star6e_pipeline_cus3a_reset(void)
{
	g_cus3a_handoff_done = 0;
}

void star6e_pipeline_cus3a_tick(SdkQuietState *sdk_quiet,
	struct timespec *ts_last)
{
	struct timespec now;
	long long elapsed_ms;
	int p000[13] = {0, 0, 0};

	if (g_cus3a_handoff_done || !ts_last)
		return;

	clock_gettime(CLOCK_MONOTONIC, &now);
	elapsed_ms =
		((long long)(now.tv_sec - ts_last->tv_sec) * 1000LL) +
		((long long)(now.tv_nsec - ts_last->tv_nsec) / 1000000LL);
	if (elapsed_ms < 1000)
		return;

	star6e_pipeline_cus3a_apply(sdk_quiet, p000);
	g_cus3a_handoff_done = 1;
}

int star6e_pipeline_cap_exposure_for_fps(uint32_t fps, uint32_t user_cap_us)
{
	return pipeline_common_cap_exposure_for_fps(fps, user_cap_us);
}

static void star6e_pipeline_stop_sensor(MI_SNR_PAD_ID_e pad_id)
{
	MI_SNR_Disable(pad_id);
}

static Star6ePrecropRect star6e_pipeline_compute_precrop(uint32_t sensor_w,
	uint32_t sensor_h, uint32_t image_w, uint32_t image_h)
{
	PipelinePrecropRect common = pipeline_common_compute_precrop(
		sensor_w, sensor_h, image_w, image_h);
	Star6ePrecropRect rect = {common.x, common.y, common.w, common.h};
	return rect;
}

static int star6e_pipeline_start_vif(const SensorSelectResult *sensor,
	const Star6ePrecropRect *precrop)
{
	MI_VIF_DevAttr_t dev = {0};
	MI_VIF_PortAttr_t port = {0};
	MI_S32 ret;

	dev.intf = sensor->pad.intf;
	dev.work = (sensor->pad.intf == I6_INTF_BT656) ? I6_VIF_WORK_1MULTIPLEX :
		I6_VIF_WORK_RGB_REALTIME;
	dev.hdr = I6_HDR_OFF;

	if (sensor->pad.intf == I6_INTF_MIPI) {
		dev.edge = I6_EDGE_DOUBLE;
		dev.input = sensor->pad.intfAttr.mipi.input;
	} else if (sensor->pad.intf == I6_INTF_BT656) {
		dev.edge = sensor->pad.intfAttr.bt656.edge;
		dev.sync = sensor->pad.intfAttr.bt656.sync;
		dev.bitswap = sensor->pad.intfAttr.bt656.bitswap;
	}

	ret = MI_VIF_SetDevAttr(0, &dev);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VIF_SetDevAttr failed %d\n", ret);
		return ret;
	}

	ret = MI_VIF_EnableDev(0);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VIF_EnableDev failed %d\n", ret);
		return ret;
	}

	port.capt.x = sensor->plane.capt.x + precrop->x;
	port.capt.y = sensor->plane.capt.y + precrop->y;
	port.capt.width = precrop->w;
	port.capt.height = precrop->h;
	port.dest.width = precrop->w;
	port.dest.height = precrop->h;
	port.field = 0;
	port.interlaceOn = 0;
	if (sensor->plane.bayer > I6_BAYER_END) {
		port.pixFmt = sensor->plane.pixFmt;
	} else {
		port.pixFmt = (i6_common_pixfmt)(I6_PIXFMT_RGB_BAYER +
			sensor->plane.precision * I6_BAYER_END + sensor->plane.bayer);
	}
	port.frate = I6_VIF_FRATE_FULL;
	port.frameLineCnt = 0;

	ret = MI_VIF_SetChnPortAttr(0, 0, &port);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VIF_SetChnPortAttr failed %d\n", ret);
		MI_VIF_DisableDev(0);
		return ret;
	}

	ret = MI_VIF_EnableChnPort(0, 0);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VIF_EnableChnPort failed %d\n", ret);
		MI_VIF_DisableChnPort(0, 0);
		MI_VIF_DisableDev(0);
		return ret;
	}

	return 0;
}

static void star6e_pipeline_stop_vif(void)
{
	MI_VIF_DisableChnPort(0, 0);
	MI_VIF_DisableDev(0);
}

static int star6e_pipeline_start_vpe(const SensorSelectResult *sensor,
	const Star6ePrecropRect *precrop, uint32_t out_width,
	uint32_t out_height, int mirror, int flip, int level_3dnr,
	SdkQuietState *sdk_quiet)
{
	MI_VPE_ChannelAttr_t channel_attr = {0};
	MI_VPE_ChannelParam_t param = {0};
	MI_VPE_PortAttr_t port = {0};
	MI_S32 ret;

	channel_attr.capt.width = precrop->w;
	channel_attr.capt.height = precrop->h;
	channel_attr.hdr = I6_HDR_OFF;
	channel_attr.sensor = (i6_vpe_sens)((int)sensor->pad_id + 1);
	channel_attr.mode = I6_VPE_MODE_REALTIME;
	if (sensor->plane.bayer > I6_BAYER_END) {
		channel_attr.pixFmt = sensor->plane.pixFmt;
	} else {
		channel_attr.pixFmt = (i6_common_pixfmt)(I6_PIXFMT_RGB_BAYER +
			sensor->plane.precision * I6_BAYER_END + sensor->plane.bayer);
	}

	sdk_quiet_begin(sdk_quiet);
	ret = MI_VPE_CreateChannel(0, &channel_attr);
	sdk_quiet_end(sdk_quiet);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VPE_CreateChannel failed %d\n", ret);
		return ret;
	}

	param.hdr = I6_HDR_OFF;
	param.level3DNR = level_3dnr;
	param.mirror = mirror ? 1 : 0;
	param.flip = flip ? 1 : 0;
	param.lensAdjOn = 0;
	ret = MI_VPE_SetChannelParam(0, &param);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VPE_SetChannelParam failed %d\n", ret);
		MI_VPE_DestroyChannel(0);
		return ret;
	}

	ret = MI_VPE_StartChannel(0);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VPE_StartChannel failed %d\n", ret);
		MI_VPE_DestroyChannel(0);
		return ret;
	}

	port.output.width = out_width;
	port.output.height = out_height;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;

	ret = MI_VPE_SetPortMode(0, 0, &port);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VPE_SetPortMode failed %d\n", ret);
		MI_VPE_StopChannel(0);
		MI_VPE_DestroyChannel(0);
		return ret;
	}

	ret = MI_VPE_EnablePort(0, 0);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VPE_EnablePort failed %d\n", ret);
		MI_VPE_DisablePort(0, 0);
		MI_VPE_StopChannel(0);
		MI_VPE_DestroyChannel(0);
		return ret;
	}

	return 0;
}

static void star6e_pipeline_stop_vpe(void)
{
	MI_VPE_DisablePort(0, 0);
	MI_VPE_StopChannel(0);
	MI_VPE_DestroyChannel(0);
}

static void star6e_pipeline_fill_h26x_attr(i6_venc_attr_h26x *attr,
	uint32_t width, uint32_t height)
{
	attr->maxWidth = width;
	attr->maxHeight = height;
	attr->bufSize = width * height * 3 / 2;
	attr->profile = 0;
	attr->byFrame = 1;
	attr->width = width;
	attr->height = height;
	attr->bFrameNum = 0;
	attr->refNum = 1;
}

static int star6e_pipeline_start_venc(uint32_t width, uint32_t height,
	uint32_t bitrate, uint32_t framerate, uint32_t gop, PAYLOAD_TYPE_E codec,
	int rc_mode, bool frame_lost_enabled, MI_VENC_CHN *chn)
{
	MI_VENC_ChnAttr_t attr = {0};
	MI_U32 bit_rate_bits;
	MI_S32 ret;

	if (bitrate > 200000)
		bitrate = 200000;
	bit_rate_bits = bitrate * 1024;

	if (codec == PT_H265) {
		attr.attrib.codec = I6_VENC_CODEC_H265;
		star6e_pipeline_fill_h26x_attr(&attr.attrib.h265, width, height);
	} else {
		attr.attrib.codec = I6_VENC_CODEC_H264;
		star6e_pipeline_fill_h26x_attr(&attr.attrib.h264, width, height);
	}

	switch (codec) {
	case PT_H265:
		switch (rc_mode) {
		case 4:
			attr.rate.mode = I6_VENC_RATEMODE_H265VBR;
			attr.rate.h265Vbr = (i6_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 5:
			attr.rate.mode = I6_VENC_RATEMODE_H265AVBR;
			attr.rate.h265Avbr = (i6_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 6:
			attr.rate.mode = I6_VENC_RATEMODE_H265VBR;
			attr.rate.h265Vbr = (i6_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 40, .minQual = 28,
			};
			break;
		case 3:
		default:
			attr.rate.mode = I6_VENC_RATEMODE_H265CBR;
			attr.rate.h265Cbr = (i6_venc_rate_h26xcbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.bitrate = bit_rate_bits, .avgLvl = 1,
			};
			break;
		}
		break;

	case PT_H264:
	default:
		switch (rc_mode) {
		case 2:
			attr.rate.mode = I6_VENC_RATEMODE_H264VBR;
			attr.rate.h264Vbr = (i6_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 0:
			attr.rate.mode = I6_VENC_RATEMODE_H264AVBR;
			attr.rate.h264Avbr = (i6_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 1:
			attr.rate.mode = I6_VENC_RATEMODE_H264VBR;
			attr.rate.h264Vbr = (i6_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 40, .minQual = 28,
			};
			break;
		case 3:
		default:
			attr.rate.mode = I6_VENC_RATEMODE_H264CBR;
			attr.rate.h264Cbr = (i6_venc_rate_h26xcbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.bitrate = bit_rate_bits, .avgLvl = 1,
			};
			break;
		}
		break;
	}

	ret = MI_VENC_CreateChn(*chn, &attr);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VENC_CreateChn(%d) failed %d\n",
			*chn, ret);
		return ret;
	}

	ret = MI_VENC_StartRecvPic(*chn);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VENC_StartRecvPic failed %d\n", ret);
		MI_VENC_DestroyChn(*chn);
		return ret;
	}

	/* Frame lost strategy — see star6e_controls_apply_frame_lost_threshold. */
	if (star6e_controls_apply_frame_lost_threshold(*chn,
	    frame_lost_enabled, bitrate) != 0)
		fprintf(stderr, "[venc] WARNING: SetFrameLostStrategy"
			" failed\n");

	return 0;
}

static void star6e_pipeline_stop_venc(MI_VENC_CHN chn)
{
	MI_VENC_StopRecvPic(chn);
	MI_VENC_DestroyChn(chn);
}

static void star6e_pipeline_sysfs_write(const char *path, const char *value)
{
	FILE *handle = fopen(path, "w");

	if (!handle)
		return;

	fprintf(handle, "%s\n", value);
	fclose(handle);
}

static void star6e_pipeline_set_hw_clocks(int oc_level, int verbose)
{
	static const struct {
		const char *path;
		const char *label;
	} clocks[] = {
		{ "/sys/devices/virtual/mstar/isp0/isp_clk", "ISP" },
		{ "/sys/devices/virtual/mstar/mscl/clk", "SCL" },
	};
	unsigned int i;

	for (i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		FILE *handle = fopen(clocks[i].path, "w");

		if (!handle)
			continue;

		fprintf(handle, "384000000\n");
		fclose(handle);
		if (verbose)
			printf("> Set %s clock to 384 MHz\n", clocks[i].label);
	}

	if (oc_level >= 1) {
		star6e_pipeline_sysfs_write(
			"/sys/devices/virtual/mstar/venc0/clk", "480000000");
		if (verbose)
			printf("> Set VENC clock to 480 MHz (oc-level %d)\n",
				oc_level);
	}

	if (oc_level >= 2) {
		star6e_pipeline_sysfs_write(
			"/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
			"performance");
		star6e_pipeline_sysfs_write(
			"/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq",
			"1200000");
		star6e_pipeline_sysfs_write(
			"/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq",
			"1200000");
		if (verbose)
			printf("> Set CPU to 1200 MHz performance governor (oc-level %d)\n",
				oc_level);
	}
}

/* Aggregates all parameters derived from VencConfig before pipeline hardware
 * is touched.  Populated by prepare_pipeline_config(), consumed by the
 * remaining helpers and the main orchestrator. */
typedef struct {
	Star6eOutputSetup  output_setup;
	SensorSelectConfig sensor_cfg;
	SensorUnlockConfig sensor_unlock;
	SensorStrategy     sensor_strategy;
	const char        *isp_bin_path;
	uint32_t           sensor_width;
	uint32_t           sensor_height;
	uint32_t           image_width;
	uint32_t           image_height;
	uint32_t           sensor_framerate;
	uint32_t           venc_max_rate;
	uint32_t           venc_gop_size;
	uint32_t           exposure_cap_us;
	Star6ePrecropRect  precrop;
	PAYLOAD_TYPE_E     rc_codec;
	int                rc_mode;
	int                image_mirror;
	int                image_flip;
	int                vpe_level_3dnr;
	int                oc_level;
} Star6ePipelineConfig;

/* Phase 1: validate vcfg, derive all scalar parameters, build output_setup and
 * sensor strategy.  No hardware is touched here. */
static int prepare_pipeline_config(Star6ePipelineState *state,
	const VencConfig *vcfg, Star6ePipelineConfig *pconf)
{
	memset(pconf, 0, sizeof(*pconf));

	pconf->sensor_width    = vcfg->video0.width;
	pconf->sensor_height   = vcfg->video0.height;
	pconf->image_width     = pconf->sensor_width;
	pconf->image_height    = pconf->sensor_height;
	pconf->sensor_framerate = vcfg->video0.fps;
	pconf->venc_max_rate   = vcfg->video0.bitrate;

	if (codec_config_resolve_codec_rc(vcfg->video0.codec, vcfg->video0.rc_mode,
	    &pconf->rc_codec, &pconf->rc_mode) != 0)
		return -1;

	state->output_enabled = vcfg->outgoing.enabled ? 1 : 0;
	if (!vcfg->outgoing.server[0] && vcfg->outgoing.enabled) {
		fprintf(stderr,
			"ERROR: outgoing.enabled=true but outgoing.server is empty\n");
		return -1;
	}
	if (star6e_output_prepare(&pconf->output_setup, vcfg->outgoing.server,
	    vcfg->outgoing.stream_mode, vcfg->outgoing.max_payload_size,
	    vcfg->outgoing.connected_udp) != 0)
		return -1;

	if (star6e_output_setup_is_rtp(&pconf->output_setup) &&
	    pconf->rc_codec != PT_H265) {
		fprintf(stderr,
			"ERROR: RTP mode on star6e currently supports H.265 only.\n");
		fprintf(stderr,
			"       Set video0.codec to h265 or outgoing.server to udp://.\n");
		return -1;
	}

	/* When legacyAe is active and no explicit exposure cap is set, the
	 * ISP bin's built-in AE ignores SetExposureLimit for the physical
	 * sensor register.  Auto-derive a cap from the sensor fps so the
	 * shutter never exceeds the frame period.  Without this, the AE
	 * converges on a long exposure that locks fps below the target. */
	if (vcfg->isp.exposure > 0)
		pconf->exposure_cap_us = vcfg->isp.exposure * 1000;
	else if (vcfg->isp.legacy_ae && pconf->sensor_framerate > 0)
		pconf->exposure_cap_us = 1000000 / pconf->sensor_framerate;
	else
		pconf->exposure_cap_us = 0;
	pconf->image_mirror    = vcfg->image.mirror ? 1 : 0;
	pconf->image_flip      = vcfg->image.flip   ? 1 : 0;
	pconf->vpe_level_3dnr  = vcfg->fpv.noise_level;
	pconf->oc_level        = vcfg->system.overclock_level;
	pconf->isp_bin_path    = vcfg->isp.sensor_bin[0] ? vcfg->isp.sensor_bin : NULL;

	pconf->sensor_cfg = pipeline_common_build_sensor_select_config(
		vcfg->sensor.index, vcfg->sensor.mode,
		pconf->sensor_width, pconf->sensor_height, pconf->sensor_framerate);
	pconf->sensor_unlock = (SensorUnlockConfig){
		.enabled = vcfg->sensor.unlock_enabled ? 1 : 0,
		.cmd_id  = vcfg->sensor.unlock_cmd,
		.reg     = vcfg->sensor.unlock_reg,
		.value   = vcfg->sensor.unlock_value,
		.dir     = (MI_SNR_CustDir_e)vcfg->sensor.unlock_dir,
	};
	pconf->sensor_strategy = pconf->sensor_unlock.enabled ?
		sensor_unlock_strategy(&pconf->sensor_unlock) :
		sensor_default_strategy();

	return 0;
}

/* Phase 2: run sensor_select(), resolve actual dimensions, compute precrop and
 * populate the relevant pconf fields.  Logs the pipeline geometry summary. */
static int select_and_configure_sensor(Star6ePipelineState *state,
	Star6ePipelineConfig *pconf, const VencConfig *vcfg,
	SdkQuietState *sdk_quiet)
{
	uint32_t sensor_width;
	uint32_t sensor_height;
	uint16_t overscan_x;
	uint16_t overscan_y;
	int ret;

	sdk_quiet_begin(sdk_quiet);
	star6e_pipeline_pre_init_teardown();
	sdk_quiet_end(sdk_quiet);

	ret = sensor_select(&pconf->sensor_cfg, &pconf->sensor_strategy,
		&state->sensor);
	if (ret != 0)
		return ret;

	/* Expose sensor info to HTTP API for /api/v1/modes */
	venc_api_set_sensor_info((int)state->sensor.pad_id,
		state->sensor.mode_index, pconf->sensor_cfg.forced_pad);

	sensor_width  = state->sensor.plane.capt.width;
	sensor_height = state->sensor.plane.capt.height;
	if (state->sensor.mode.output.width > 0 &&
	    state->sensor.mode.output.height > 0 &&
	    (state->sensor.mode.output.width  < sensor_width ||
	     state->sensor.mode.output.height < sensor_height)) {
		if (vcfg->system.verbose)
			printf("> Note: MIPI frame %ux%u, usable output %ux%u (cropping overscan)\n",
				sensor_width, sensor_height,
				state->sensor.mode.output.width,
				state->sensor.mode.output.height);
		if (state->sensor.mode.output.width  < sensor_width)
			sensor_width  = state->sensor.mode.output.width;
		if (state->sensor.mode.output.height < sensor_height)
			sensor_height = state->sensor.mode.output.height;
	}

	pipeline_common_report_selected_fps("", pconf->sensor_framerate,
		&state->sensor);
	pconf->sensor_framerate = state->sensor.fps;
	pconf->venc_gop_size = pipeline_common_gop_frames(vcfg->video0.gop_size,
		pconf->sensor_framerate);
	pipeline_common_clamp_image_size("", sensor_width, sensor_height,
		&pconf->image_width, &pconf->image_height);
	state->image_width  = pconf->image_width;
	state->image_height = pconf->image_height;

	pconf->precrop = star6e_pipeline_compute_precrop(sensor_width,
		sensor_height, pconf->image_width, pconf->image_height);
	overscan_x = (uint16_t)(((state->sensor.plane.capt.width  - sensor_width)
		/ 2) & ~1u);
	overscan_y = (uint16_t)(((state->sensor.plane.capt.height - sensor_height)
		/ 2) & ~1u);
	pconf->precrop.x += overscan_x;
	pconf->precrop.y += overscan_y;

	if (vcfg->system.verbose) {
		printf("> Starting star6e pipeline\n");
		printf("  - Sensor: %ux%u @ %u\n", sensor_width, sensor_height,
			pconf->sensor_framerate);
		if (overscan_x || overscan_y) {
			printf("  - MIPI  : %ux%u, cropped to %ux%u (offset %u,%u)\n",
				state->sensor.plane.capt.width,
				state->sensor.plane.capt.height,
				sensor_width, sensor_height, overscan_x, overscan_y);
		}
		printf("  - Image : %ux%u\n", pconf->image_width,
			pconf->image_height);
		if (pconf->precrop.w != sensor_width ||
		    pconf->precrop.h != sensor_height) {
			printf("  - Precrop: %ux%u -> %ux%u (VIF offset %u,%u)\n",
				sensor_width, sensor_height,
				pconf->precrop.w, pconf->precrop.h,
				pconf->precrop.x, pconf->precrop.y);
		}
		if (pconf->image_width  != pconf->precrop.w ||
		    pconf->image_height != pconf->precrop.h) {
			printf("  - VPE scaling: %ux%u -> %ux%u\n",
				pconf->precrop.w, pconf->precrop.h,
				pconf->image_width, pconf->image_height);
		}
		printf("  - 3DNR  : level %d\n", pconf->vpe_level_3dnr);
	}

	return 0;
}

/* IMU push callback: forwards each sample to the EIS module */
static void star6e_pipeline_imu_push(void *ctx, const ImuSample *sample)
{
	Star6ePipelineState *ps = (Star6ePipelineState *)ctx;
	if (ps->eis)
		eis_push_sample(ps->eis, sample->gyro_x, sample->gyro_y,
			sample->gyro_z, &sample->ts);
}

/* Track whether CUS3A has been enabled.  Without MI_SYS_Exit(), the kernel
 * ISP driver retains CUS3A state, so re-enabling (100→110→111 sequence)
 * causes a mutex deadlock.  ISP bin and exposure cap are safe to reapply. */
static int g_isp_initialized = 0;

/* Phase 3: assign port structs, issue all MI_SYS bind calls, init output,
 * video, ISP bin, exposure cap, cus3a, clocks, and audio. */
static int bind_and_finalize_pipeline(Star6ePipelineState *state,
	const VencConfig *vcfg, const Star6ePipelineConfig *pconf,
	SdkQuietState *sdk_quiet)
{
	MI_U32 venc_device = 0;
	uint32_t bind_src_fps;
	uint32_t bind_dst_fps;
	int ret;

	state->vif_port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VIF, .device = 0, .channel = 0, .port = 0 };
	state->vpe_port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0, .port = 0 };

	if (MI_VENC_GetChnDevid(state->venc_channel, &venc_device) != 0) {
		fprintf(stderr, "ERROR: MI_VENC_GetChnDevid failed\n");
		return -1;
	}
	state->venc_port = (MI_SYS_ChnPort_t){
		.module  = I6_SYS_MOD_VENC, .device  = venc_device,
		.channel = state->venc_channel, .port = 0 };

	if (!state->bound_vif_vpe) {
		ret = MI_SYS_BindChnPort2(&state->vif_port, &state->vpe_port,
			pconf->sensor_framerate, pconf->sensor_framerate,
			I6_SYS_LINK_REALTIME, 0);
		if (ret != 0) {
			fprintf(stderr, "ERROR: MI_SYS_Bind VIF->VPE failed %d\n", ret);
			return ret;
		}
		state->bound_vif_vpe = 1;

		/* A new VPE channel was just created (first start or AR-change
		 * reinit). The ISP channel initialises asynchronously after
		 * MI_VPE_CreateChannel.  Poll here before the bin load and
		 * cap_exposure_for_fps touch the ISP, so the kernel ISP driver
		 * does not emit "IspApiGet channel not created" errors. */
		star6e_pipeline_wait_isp_channel();
	}

	/* Cap exposure BEFORE binding VPE→VENC.  The AE starts running as
	 * soon as VIF→VPE is bound (above).  Without an early cap the AE
	 * can converge on a shutter time longer than the frame period during
	 * the ISP bin load + CUS3A init window, locking the pipeline at a
	 * lower framerate until reinit. */
	star6e_pipeline_cap_exposure_for_fps(pconf->sensor_framerate,
		pconf->exposure_cap_us);

	bind_src_fps = state->sensor.mode.maxFps ?
		state->sensor.mode.maxFps : pconf->sensor_framerate;
	bind_dst_fps = vcfg->video0.fps;
	if (bind_dst_fps == 0 || bind_dst_fps > bind_src_fps)
		bind_dst_fps = bind_src_fps;

	ret = MI_SYS_BindChnPort2(&state->vpe_port, &state->venc_port,
		bind_src_fps, bind_dst_fps, I6_SYS_LINK_FRAMEBASE, 0);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_SYS_Bind VPE->VENC failed %d\n", ret);
		MI_SYS_UnBindChnPort(&state->vif_port, &state->vpe_port);
		state->bound_vif_vpe = 0;
		return ret;
	}
	state->bound_vpe_venc = 1;
	MI_SYS_SetChnOutputPortDepth(&state->venc_port, 1, 3);

	if (star6e_output_init(&state->output, &pconf->output_setup) != 0) {
		MI_SYS_UnBindChnPort(&state->vpe_port, &state->venc_port);
		state->bound_vpe_venc = 0;
		MI_SYS_UnBindChnPort(&state->vif_port, &state->vpe_port);
		state->bound_vif_vpe = 0;
		return -1;
	}

	star6e_video_init(&state->video, vcfg, pconf->sensor_framerate,
		&state->output);

	/* Load ISP bin on every start/reinit.  The kernel ISP driver accepts
	 * repeated loads without issues on current firmware. */
	if (pconf->isp_bin_path && *pconf->isp_bin_path) {
		ret = star6e_pipeline_load_isp_bin(pconf->isp_bin_path, sdk_quiet);
		if (ret != 0)
			fprintf(stderr, "WARNING: ISP bin load failed; continuing with default ISP settings\n");
	}
	if (!g_isp_initialized) {
		star6e_pipeline_enable_cus3a(sdk_quiet);
		g_isp_initialized = 1;
	}
	/* Reapply exposure cap after ISP bin load — the bin may reset AE
	 * limits to its own defaults which could exceed the frame period. */
	star6e_pipeline_cap_exposure_for_fps(pconf->sensor_framerate,
		pconf->exposure_cap_us);
	star6e_pipeline_set_hw_clocks(pconf->oc_level, vcfg->system.verbose);

	if (star6e_output_is_shm(&state->output) &&
	    vcfg->outgoing.audio_port == 0) {
		printf("[audio] Disabled in SHM mode (audioPort=0 has no UDP socket to share)\n");
	} else {
		star6e_audio_init(&state->audio, vcfg, &state->output);
	}

	/* IMU */
	if (vcfg->imu.enabled) {
		ImuConfig imu_cfg = {
			.i2c_device = vcfg->imu.i2c_device,
			.i2c_addr = vcfg->imu.i2c_addr,
			.sample_rate_hz = vcfg->imu.sample_rate_hz,
			.gyro_range_dps = vcfg->imu.gyro_range_dps,
			.cal_file = vcfg->imu.cal_file,
			.cal_samples = vcfg->imu.cal_samples,
			.push_fn = star6e_pipeline_imu_push,
			.push_ctx = state,
			.use_thread = 0,
		};
		state->imu = imu_init(&imu_cfg);
		if (state->imu) {
			imu_start(state->imu);
		} else {
			fprintf(stderr, "WARNING: IMU init failed, continuing without IMU\n");
		}
	}

	/* EIS */
	if (vcfg->eis.enabled) {
		if (!state->imu && !vcfg->eis.test_mode) {
			fprintf(stderr, "WARNING: EIS requires IMU (unless testMode), skipping\n");
		} else {
			EisConfig eis_cfg = {
				.mode = vcfg->eis.mode,
				.margin_percent = vcfg->eis.margin_percent,
				.capture_w = (uint16_t)state->image_width,
				.capture_h = (uint16_t)state->image_height,
				.vpe_channel = 0,
				.vpe_port = 0,
				.pixels_per_radian = 0.0f,  /* auto: capture_w/2 */
				.test_mode = vcfg->eis.test_mode ? 1 : 0,
				.swap_xy = vcfg->eis.swap_xy ? 1 : 0,
				.invert_x = vcfg->eis.invert_x ? 1 : 0,
				.invert_y = vcfg->eis.invert_y ? 1 : 0,
				.gain = vcfg->eis.gain,
				.deadband_rad = vcfg->eis.deadband_rad,
				.recenter_rate = vcfg->eis.recenter_rate,
				.max_slew_px = vcfg->eis.max_slew_px,
				.bias_alpha = vcfg->eis.bias_alpha,
			};
			state->eis = eis_create(&eis_cfg);
			if (state->eis && state->imu)
				eis_set_imu_active(state->eis, 1);
		}
	}

	/* Debug OSD */
	if (vcfg->debug.show_osd) {
		state->debug_osd = debug_osd_create(
			state->image_width, state->image_height, &state->vpe_port);
		if (!state->debug_osd)
			fprintf(stderr, "WARNING: debug OSD requested but MI_RGN unavailable\n");
	}

	return 0;
}

/* Drain buffered frames from a VENC channel (non-blocking).
 * Relieves VPE backpressure before StopRecvPic to prevent D-state hangs.
 * Drains until no frames remain or max_ms elapsed — time-bounded to
 * handle continuous 120fps production during teardown. */
static void drain_venc_channel(MI_VENC_CHN chn, int max_ms,
	const char *label)
{
	struct timespec start;
	int drained = 0;

	clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		MI_VENC_Stat_t stat = {0};
		MI_VENC_Stream_t stream = {0};
		struct timespec now;
		long long elapsed_ms;

		if (MI_VENC_Query(chn, &stat) != 0 || stat.curPacks == 0)
			break;

		stream.count = stat.curPacks;
		stream.packet = calloc(stat.curPacks, sizeof(MI_VENC_Pack_t));
		if (!stream.packet)
			break;

		if (MI_VENC_GetStream(chn, &stream, 0) != 0) {
			free(stream.packet);
			break;
		}

		MI_VENC_ReleaseStream(chn, &stream);
		free(stream.packet);
		drained++;

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed_ms = (long long)(now.tv_sec - start.tv_sec) * 1000LL +
			(long long)(now.tv_nsec - start.tv_nsec) / 1000000LL;
		if (elapsed_ms >= max_ms)
			break;
	}

	if (drained > 0)
		printf("> Drained %d frames from VENC %s before stop\n",
			drained, label);
}

void star6e_pipeline_stop(Star6ePipelineState *state)
{
	if (!state)
		return;

	/* The recording thread must keep consuming ch1 frames through
	 * the ENTIRE teardown sequence.  At 120fps, the 12-frame ch1
	 * buffer fills in ~100ms — any gap in consumption causes VPE
	 * backpressure → kernel D-state → StopRecvPic hangs.
	 *
	 * Sequence: teardown peripherals → drain both channels →
	 * StopRecvPic (thread still draining ch1) → signal thread
	 * to stop → join thread. */

	/* Stop IMU thread — the push callback accesses state->eis,
	 * so the thread must be halted before EIS is destroyed. */
	if (state->imu)
		imu_stop(state->imu);
	if (state->eis) {
		eis_destroy(state->eis);
		state->eis = NULL;
	}
	if (state->imu) {
		imu_destroy(state->imu);
		state->imu = NULL;
	}
	if (state->debug_osd) {
		debug_osd_destroy(state->debug_osd);
		state->debug_osd = NULL;
	}

	star6e_audio_teardown(&state->audio);
	star6e_output_teardown(&state->output);
	if (state->dual)
		star6e_output_teardown(&state->dual->output);

	/* Stop the recording thread FIRST.  The thread's GetStream and
	 * the main thread's UnBindChnPort contend for the same kernel
	 * VPE lock — running them concurrently causes intermittent
	 * deadlock (D-state).  By joining the thread first, we ensure
	 * no concurrent VENC consumers exist during unbind/stop.
	 *
	 * The thread checks rec_running at the top of each loop and
	 * uses non-blocking GetStream (timeout=0) when g_running==0,
	 * so it exits within one iteration (~1ms). */
	if (state->dual && state->dual->rec_started) {
		state->dual->rec_running = 0;
		pthread_join(state->dual->rec_thread, NULL);
		state->dual->rec_started = 0;
		printf("> Dual recording thread joined\n");
	}

	/* Unbind VPE→VENC.  Safe now — no concurrent consumers calling
	 * GetStream, so the kernel unbind won't deadlock. */
	if (state->dual && state->dual->bound) {
		MI_SYS_UnBindChnPort(&state->vpe_port, &state->dual->port);
		state->dual->bound = 0;
	}
	if (state->bound_vpe_venc) {
		MI_SYS_UnBindChnPort(&state->vpe_port, &state->venc_port);
		state->bound_vpe_venc = 0;
	}

	/* Drain remaining buffered frames after unbind. */
	drain_venc_channel(state->venc_channel, 500, "ch0");
	if (state->dual)
		drain_venc_channel(state->dual->channel, 500, "ch1-post");

	/* StopRecvPic — VPE is unbound, no flush wait needed. */
	if (state->dual)
		MI_VENC_StopRecvPic(state->dual->channel);
	MI_VENC_StopRecvPic(state->venc_channel);

	if (state->bound_vif_vpe) {
		MI_SYS_UnBindChnPort(&state->vif_port, &state->vpe_port);
		state->bound_vif_vpe = 0;
	}

	/* Destroy channels */
	if (state->dual) {
		venc_api_dual_unregister();
		MI_VENC_DestroyChn(state->dual->channel);
		free(state->dual->stream_packs);
		free(state->dual);
		state->dual = NULL;
	}
	MI_VENC_DestroyChn(state->venc_channel);
	free(state->stream_packs);
	state->stream_packs = NULL;
	state->stream_packs_cap = 0;
	star6e_pipeline_stop_vpe();
	star6e_pipeline_stop_vif();
	star6e_pipeline_stop_sensor(state->sensor.pad_id);
}

/* Partial teardown for SIGHUP reinit: tears down VENC channels, binds,
 * output, audio, IMU/EIS but keeps sensor/VIF/VPE running.  The SigmaStar
 * MIPI PHY does not recover from MI_SNR_Disable/Enable cycles, so the
 * sensor must stay active across reinit. */
static void star6e_pipeline_stop_venc_level(Star6ePipelineState *state)
{
	if (!state)
		return;

	if (state->imu)
		imu_stop(state->imu);
	if (state->eis) {
		eis_destroy(state->eis);
		state->eis = NULL;
	}
	if (state->imu) {
		imu_destroy(state->imu);
		state->imu = NULL;
	}
	if (state->debug_osd) {
		debug_osd_destroy(state->debug_osd);
		state->debug_osd = NULL;
	}

	star6e_audio_teardown(&state->audio);
	star6e_output_teardown(&state->output);
	if (state->dual)
		star6e_output_teardown(&state->dual->output);

	if (state->dual && state->dual->rec_started) {
		state->dual->rec_running = 0;
		pthread_join(state->dual->rec_thread, NULL);
		state->dual->rec_started = 0;
	}

	if (state->dual && state->dual->bound) {
		MI_SYS_UnBindChnPort(&state->vpe_port, &state->dual->port);
		state->dual->bound = 0;
	}
	if (state->bound_vpe_venc) {
		MI_SYS_UnBindChnPort(&state->vpe_port, &state->venc_port);
		state->bound_vpe_venc = 0;
	}

	drain_venc_channel(state->venc_channel, 500, "ch0");
	if (state->dual)
		drain_venc_channel(state->dual->channel, 500, "ch1-post");

	if (state->dual)
		MI_VENC_StopRecvPic(state->dual->channel);
	MI_VENC_StopRecvPic(state->venc_channel);

	/* VIF→VPE bind stays active — do NOT unbind */

	if (state->dual) {
		venc_api_dual_unregister();
		MI_VENC_DestroyChn(state->dual->channel);
		free(state->dual->stream_packs);
		free(state->dual);
		state->dual = NULL;
	}
	MI_VENC_DestroyChn(state->venc_channel);

	/* Reset VENC-level state but preserve sensor/VIF/VPE state */
	star6e_output_reset(&state->output);
	star6e_video_reset(&state->video);
}

/* flatten: same rationale as star6e_pipeline_start — when this function gains
 * a call to star6e_pipeline_start_vpe (on aspect-ratio resize), GCC -Os would
 * de-inline it, changing the stack layout for MI_VPE_CreateChannel and breaking
 * ISP channel creation.  flatten keeps all static callees inlined here too. */
__attribute__((flatten))
int star6e_pipeline_reinit(Star6ePipelineState *state, const VencConfig *vcfg,
	SdkQuietState *sdk_quiet)
{
	Star6ePipelineConfig pconf;
	uint32_t venc_fps;
	uint32_t prev_image_width;
	uint32_t prev_image_height;
	int ret;

	if (!state || !vcfg)
		return -1;

	star6e_pipeline_stop_venc_level(state);

	if (prepare_pipeline_config(state, vcfg, &pconf) != 0)
		return -1;

	/* Reuse existing sensor state — don't call sensor_select.
	 * Recompute image dimensions from the existing sensor mode. */
	pconf.sensor_framerate = state->sensor.fps;
	pconf.venc_gop_size = pipeline_common_gop_frames(vcfg->video0.gop_size,
		pconf.sensor_framerate);

	/* Capture current dimensions before clamping overwrites state. */
	prev_image_width  = state->image_width;
	prev_image_height = state->image_height;

	pipeline_common_clamp_image_size("",
		state->sensor.plane.capt.width,
		state->sensor.plane.capt.height,
		&pconf.image_width, &pconf.image_height);
	state->image_width  = pconf.image_width;
	state->image_height = pconf.image_height;

	if (pconf.image_width != prev_image_width ||
	    pconf.image_height != prev_image_height) {
		uint32_t sensor_w = state->sensor.plane.capt.width;
		uint32_t sensor_h = state->sensor.plane.capt.height;

		/* Use usable sensor dimensions for precrop (same logic as
		 * select_and_configure_sensor): sensors with MIPI overscan report
		 * a larger plane.capt than the usable mode.output area. */
		uint32_t usable_w = sensor_w;
		uint32_t usable_h = sensor_h;
		if (state->sensor.mode.output.width > 0 &&
		    state->sensor.mode.output.height > 0) {
			if (state->sensor.mode.output.width < usable_w)
				usable_w = state->sensor.mode.output.width;
			if (state->sensor.mode.output.height < usable_h)
				usable_h = state->sensor.mode.output.height;
		}
		uint16_t overscan_x = (uint16_t)(
			((sensor_w - usable_w) / 2) & ~1u);
		uint16_t overscan_y = (uint16_t)(
			((sensor_h - usable_h) / 2) & ~1u);

		Star6ePrecropRect old_precrop = star6e_pipeline_compute_precrop(
			usable_w, usable_h, prev_image_width, prev_image_height);
		old_precrop.x += overscan_x;
		old_precrop.y += overscan_y;
		Star6ePrecropRect new_precrop = star6e_pipeline_compute_precrop(
			usable_w, usable_h, pconf.image_width, pconf.image_height);
		new_precrop.x += overscan_x;
		new_precrop.y += overscan_y;

		if (old_precrop.x != new_precrop.x ||
		    old_precrop.y != new_precrop.y ||
		    old_precrop.w != new_precrop.w ||
		    old_precrop.h != new_precrop.h) {
			/* Aspect ratio changed: unbind VIF→VPE, destroy VPE, reconfigure
			 * VIF capture region, recreate VPE with new dimensions.
			 * The VIF device stays running — MIPI PHY is never touched.
			 * bound_vif_vpe is cleared so bind_and_finalize_pipeline will
			 * re-establish the VIF→VPE REALTIME bind. */
			printf("> Reinit: AR change %ux%u -> %ux%u, reconfiguring VIF+VPE\n",
				prev_image_width, prev_image_height,
				pconf.image_width, pconf.image_height);

			if (state->bound_vif_vpe) {
				MI_SYS_UnBindChnPort(&state->vif_port,
					&state->vpe_port);
				state->bound_vif_vpe = 0;
			}

			star6e_pipeline_stop_vpe();

			/* Reconfigure VIF port crop (device stays enabled). */
			MI_VIF_PortAttr_t vif_port = {0};
			vif_port.capt.x      = state->sensor.plane.capt.x +
				new_precrop.x;
			vif_port.capt.y      = state->sensor.plane.capt.y +
				new_precrop.y;
			vif_port.capt.width  = new_precrop.w;
			vif_port.capt.height = new_precrop.h;
			vif_port.dest.width  = new_precrop.w;
			vif_port.dest.height = new_precrop.h;
			vif_port.field       = 0;
			vif_port.interlaceOn = 0;
			if (state->sensor.plane.bayer > I6_BAYER_END) {
				vif_port.pixFmt = state->sensor.plane.pixFmt;
			} else {
				vif_port.pixFmt = (i6_common_pixfmt)(
					I6_PIXFMT_RGB_BAYER +
					state->sensor.plane.precision *
					I6_BAYER_END +
					state->sensor.plane.bayer);
			}
			vif_port.frate        = I6_VIF_FRATE_FULL;
			vif_port.frameLineCnt = 0;

			MI_VIF_DisableChnPort(0, 0);
			ret = MI_VIF_SetChnPortAttr(0, 0, &vif_port);
			if (ret != 0) {
				fprintf(stderr,
					"ERROR: MI_VIF_SetChnPortAttr reinit"
					" failed %d\n", ret);
				/* VPE is destroyed; fully stop VIF for clean
				 * state before the caller recovers or exits. */
				MI_VIF_DisableDev(0);
				return ret;
			}
			ret = MI_VIF_EnableChnPort(0, 0);
			if (ret != 0) {
				fprintf(stderr,
					"ERROR: MI_VIF_EnableChnPort reinit"
					" failed %d\n", ret);
				MI_VIF_DisableDev(0);
				return ret;
			}

			/* Recreate VPE with new input crop and output dimensions. */
			pconf.precrop = new_precrop;
			ret = star6e_pipeline_start_vpe(&state->sensor,
				&new_precrop,
				pconf.image_width, pconf.image_height,
				pconf.image_mirror, pconf.image_flip,
				pconf.vpe_level_3dnr, sdk_quiet);
			if (ret != 0) {
				/* VIF is enabled with new crop but VPE failed;
				 * disable VIF to leave pipeline in consistent
				 * stopped state. */
				MI_VIF_DisableChnPort(0, 0);
				MI_VIF_DisableDev(0);
				return ret;
			}

		} else {
			/* Same aspect ratio: only resize VPE output port.
			 * VIF and the VIF→VPE REALTIME bind are unchanged. */
			printf("> Reinit: resolution change %ux%u -> %ux%u,"
				" resizing VPE port\n",
				prev_image_width, prev_image_height,
				pconf.image_width, pconf.image_height);

			MI_VPE_PortAttr_t vpe_port = {0};
			vpe_port.output.width  = pconf.image_width;
			vpe_port.output.height = pconf.image_height;
			vpe_port.pixFmt        = I6_PIXFMT_YUV420SP;
			vpe_port.compress      = I6_COMPR_NONE;

			MI_VPE_DisablePort(0, 0);
			ret = MI_VPE_SetPortMode(0, 0, &vpe_port);
			if (ret != 0) {
				fprintf(stderr,
					"ERROR: MI_VPE_SetPortMode(%ux%u)"
					" failed %d\n",
					pconf.image_width, pconf.image_height,
					ret);
				/* Restore port enable so VPE remains usable
				 * at the previous output dimensions. */
				MI_VPE_EnablePort(0, 0);
				return ret;
			}
			ret = MI_VPE_EnablePort(0, 0);
			if (ret != 0) {
				fprintf(stderr,
					"ERROR: MI_VPE_EnablePort after"
					" resize failed %d\n", ret);
				return ret;
			}
		}
	}

	state->venc_channel = 0;
	venc_fps = vcfg->video0.fps;
	if (venc_fps == 0 || venc_fps > pconf.sensor_framerate)
		venc_fps = pconf.sensor_framerate;
	ret = star6e_pipeline_start_venc(pconf.image_width, pconf.image_height,
		pconf.venc_max_rate, venc_fps, pconf.venc_gop_size,
		pconf.rc_codec, pconf.rc_mode,
		vcfg->video0.frame_lost, &state->venc_channel);
	if (ret != 0)
		return ret;

	ret = bind_and_finalize_pipeline(state, vcfg, &pconf, sdk_quiet);
	if (ret != 0) {
		star6e_pipeline_stop_venc(state->venc_channel);
		return ret;
	}

	return 0;
}

/* flatten: force GCC to inline all static callees into this function.
 * The SigmaStar I6E ISP driver depends on the monolithic stack layout
 * that results from inlining bind_and_finalize_pipeline() and
 * prepare_pipeline_config().  When these are emitted as separate functions
 * (as happens with -Os when they have multiple call-sites), the VPE→ISP
 * channel init fails (MI_ISP_IQ_GetParaInitStatus returns error 6). */
__attribute__((flatten))
int star6e_pipeline_start(Star6ePipelineState *state, const VencConfig *vcfg,
	SdkQuietState *sdk_quiet)
{
	Star6ePipelineConfig pconf;
	uint32_t venc_fps;
	int ret;

	if (!state || !vcfg)
		return -1;

	star6e_pipeline_reset(state);

	if (prepare_pipeline_config(state, vcfg, &pconf) != 0)
		return -1;

	ret = select_and_configure_sensor(state, &pconf, vcfg, sdk_quiet);
	if (ret != 0)
		return ret;

	ret = star6e_pipeline_start_vif(&state->sensor, &pconf.precrop);
	if (ret != 0)
		goto fail_sensor;

	ret = star6e_pipeline_start_vpe(&state->sensor, &pconf.precrop,
		pconf.image_width, pconf.image_height,
		pconf.image_mirror, pconf.image_flip,
		pconf.vpe_level_3dnr, sdk_quiet);
	if (ret != 0)
		goto fail_vif;

	state->venc_channel = 0;
	venc_fps = vcfg->video0.fps;
	if (venc_fps == 0 || venc_fps > pconf.sensor_framerate)
		venc_fps = pconf.sensor_framerate;
	ret = star6e_pipeline_start_venc(pconf.image_width, pconf.image_height,
		pconf.venc_max_rate, venc_fps, pconf.venc_gop_size,
		pconf.rc_codec, pconf.rc_mode,
		vcfg->video0.frame_lost, &state->venc_channel);
	if (ret != 0)
		goto fail_vpe;

	ret = bind_and_finalize_pipeline(state, vcfg, &pconf, sdk_quiet);
	if (ret != 0)
		goto fail_venc;

	return 0;

fail_venc:
	star6e_pipeline_stop_venc(state->venc_channel);
fail_vpe:
	star6e_pipeline_stop_vpe();
fail_vif:
	star6e_pipeline_stop_vif();
fail_sensor:
	star6e_pipeline_stop_sensor(state->sensor.pad_id);
	return ret ? ret : -1;
}

int star6e_pipeline_start_dual(Star6ePipelineState *state,
	uint32_t bitrate, uint32_t fps, double gop_sec,
	const char *mode, const char *server, bool frame_lost)
{
	Star6eDualVenc *d;
	MI_U32 dev = 0;
	MI_VENC_CHN ch1 = 1;
	uint32_t sensor_fps;
	uint32_t gop;
	int ret;

	if (!state || !mode)
		return -1;

	sensor_fps = state->sensor.mode.maxFps;
	if (sensor_fps == 0) sensor_fps = 30;
	if (fps == 0) fps = sensor_fps;
	if (fps > sensor_fps) fps = sensor_fps;
	if (bitrate == 0) bitrate = 8000;
	gop = (uint32_t)(gop_sec * fps + 0.5);
	if (gop < 1) gop = fps;  /* default 1-second GOP */

	d = calloc(1, sizeof(*d));
	if (!d)
		return -1;

	d->channel = ch1;
	d->bitrate = bitrate;
	d->fps = fps;
	d->gop = gop;
	snprintf(d->mode, sizeof(d->mode), "%s", mode);
	if (server)
		snprintf(d->server, sizeof(d->server), "%s", server);

	ret = star6e_pipeline_start_venc(state->image_width,
		state->image_height, bitrate, fps, gop,
		PT_H265, 3 /* CBR */, frame_lost, &d->channel);
	if (ret != 0) {
		fprintf(stderr, "WARNING: dual VENC ch1 create failed (%d), "
			"falling back to mirror mode\n", ret);
		free(d);
		return -1;
	}

	MI_VENC_GetChnDevid(d->channel, &dev);
	d->port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VENC, .device = dev,
		.channel = d->channel, .port = 0 };

	ret = MI_SYS_BindChnPort2(&state->vpe_port, &d->port,
		sensor_fps, fps, I6_SYS_LINK_FRAMEBASE, 0);
	if (ret != 0) {
		fprintf(stderr, "WARNING: dual VENC bind failed (%d), "
			"falling back to mirror mode\n", ret);
		star6e_pipeline_stop_venc(d->channel);
		free(d);
		return -1;
	}
	d->bound = 1;
	/* Deep buffer for ch1: the recording thread can stall on SD card
	 * writes (flash GC takes up to 500ms).  At 120fps, a 64-frame
	 * buffer gives ~533ms of headroom before VPE backpressure. */
	MI_SYS_SetChnOutputPortDepth(&d->port, 8, 56);

	state->dual = d;
	printf("> Dual VENC: ch1 = %u kbps %u fps (mode: %s)\n",
		bitrate, fps, mode);
	return 0;
}

void star6e_pipeline_stop_dual(Star6ePipelineState *state)
{
	Star6eDualVenc *d;

	if (!state || !state->dual)
		return;

	d = state->dual;
	star6e_output_teardown(&d->output);
	/* Stop receiving first, then unbind. Reverse order deadlocks
	 * because UnBind waits for the pipeline to drain while VPE
	 * is still feeding frames to VENC. */
	MI_VENC_StopRecvPic(d->channel);
	if (d->bound) {
		MI_SYS_UnBindChnPort(&state->vpe_port, &d->port);
		d->bound = 0;
	}
	MI_VENC_DestroyChn(d->channel);
	free(d->stream_packs);
	free(d);
	state->dual = NULL;
}
