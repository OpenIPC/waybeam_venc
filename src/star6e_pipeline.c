#include "star6e_pipeline.h"

#include "codec_config.h"
#include "codec_types.h"
#include "debug_osd.h"
#include "imu_ring.h"
#include "star6e_controls.h"
#include "star6e_cus3a.h"
#include "file_util.h"
#include "intra_refresh.h"
#include "isp_runtime.h"
#include "pipeline_common.h"
#include "venc_api.h"
#include "venc_jpeg.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
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
	(void)write(STDERR_FILENO, "[waybeam] VPE SCL preset stored for next run\n",
		42);
}

/* Called after MI_SYS_Init() to silently tear down any pipeline state left
 * by an unclean previous exit. All errors are ignored.
 *
 * When MI libs are loaded via dlopen (no direct linking), the vendor VPE
 * module calls exit(127) on API calls before a channel is created. Guard
 * VPE teardown with a channel existence check to avoid this. */
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

	/* VPE: probe channel existence before teardown — MI_VPE_DisablePort
	 * calls exit(127) when called on a non-existent channel under dlopen. */
	MI_VPE_ChannelAttr_t probe_attr;
	if (MI_VPE_GetChannelAttr(0, &probe_attr) == 0) {
		(void)MI_VPE_DisablePort(0, 0);
		(void)MI_VPE_StopChannel(0);
		(void)MI_VPE_DestroyChannel(0);
	}

	(void)MI_VIF_DisableChnPort(0, 0);
	(void)MI_VIF_DisableDev(0);
}

static int star6e_pipeline_disable_userspace3a(const IspRuntimeLib *lib,
	void *ctx)
{
	isp_disable_userspace3a_fn_t fn;

	(void)ctx;
	fn = (isp_disable_userspace3a_fn_t)lib->disable_userspace3a;
	return fn ? fn(0) : 0;
}

/* Forward decls — definitions live alongside the zoom/pan block but they
 * are referenced from the ISP-ready waiters above it (and, for the stab AE
 * crop, from the stabilization block which precedes that definition). */
static void star6e_ae_crop_mark_ready(void);
static void star6e_stab_apply_ae_crop(void);

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
			star6e_ae_crop_mark_ready();
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
			star6e_ae_crop_mark_ready();
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
		MI_S32 ret;

		sdk_quiet_begin(sdk_quiet);
		fn(0, p100);
		ret = fn(0, p110);
		sdk_quiet_end(sdk_quiet);
		if (ret != 0)
			fprintf(stderr, "WARNING: MI_ISP_CUS3A_Enable(1,1,0) failed %d\n", ret);
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

/* Delayed legacy-AE cold-boot fps re-kick.  The pipeline-init MI_SNR_SetFps
 * (bind_and_finalize_pipeline) fires before the ISP bin's AE has settled, so on
 * a cold boot the sensor's physical timing register can be left below the
 * configured fps (observed ~70fps @ target 90; a warm restart keeps the kernel
 * sensor state so it shows ~90).  CUS3A handles this via its frame-15 thread
 * kick; legacy AE has no periodic thread, so re-issue SetFps once from the run
 * loop ~1.5s after start, after the bin load + AE converge, to force the sensor
 * register to the target.  No-op when legacy_ae is off (CUS3A path) or fps is 0. */
void star6e_pipeline_legacy_fps_rekick(const Star6ePipelineState *state,
	const VencConfig *vcfg)
{
	if (!state || !vcfg || !vcfg->isp.legacy_ae)
		return;
	if (state->sensor.fps == 0)
		return;
	printf("[waybeam] legacy cold-boot fps re-kick: SetFps(%u)\n",
		state->sensor.fps);
	fflush(stdout);
	MI_SNR_SetFps(state->sensor.pad_id, state->sensor.fps);
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

int star6e_pipeline_cap_exposure_for_fps(uint32_t fps)
{
	return pipeline_common_cap_exposure_for_fps(fps);
}

static void star6e_pipeline_stop_sensor(MI_SNR_PAD_ID_e pad_id)
{
	MI_SNR_Disable(pad_id);
}

static Star6ePrecropRect star6e_pipeline_compute_precrop(uint32_t sensor_w,
	uint32_t sensor_h, uint32_t image_w, uint32_t image_h,
	bool keep_aspect)
{
	PipelinePrecropRect common = pipeline_common_compute_precrop(
		sensor_w, sensor_h, image_w, image_h, keep_aspect);
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

	/* Sensor-level orientation.  VPE digital flip is unreliable on some
	 * sensor combos; MI_SNR_SetOrien programs the sensor's own flip
	 * register which is what actually inverts scan-line order.  Applied
	 * after MI_VPE_SetChannelParam so both paths agree.  Non-fatal: some
	 * sensor drivers may reject mid-stream orientation changes; we log
	 * so BSP regressions surface instead of silently leaving the image
	 * upside down. */
	{
		MI_S32 orien_ret = MI_SNR_SetOrien(sensor->pad_id,
			mirror ? 1 : 0, flip ? 1 : 0);
		if (orien_ret != 0)
			fprintf(stderr, "[pipeline] WARNING: "
				"MI_SNR_SetOrien(pad=%d mirror=%d flip=%d) "
				"returned %d\n",
				(int)sensor->pad_id, mirror ? 1 : 0,
				flip ? 1 : 0, (int)orien_ret);
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

/* ── Image stabilization on VPE (Star6E) ─────────────────────────────────
 *
 * One-file integration of MI_IVE_Shift_Detector based DIS:
 *
 *   VIF → VPE → port0 (full image_w×h NV12) → stab thread:
 *       1. drain VPE port0 frame
 *       2. run MI_IVE_Shift_Detector on a center Y patch vs previous frame
 *       3. accumulate dx/dy → off_x/off_y, clipped to half the dead border
 *       4. BufBlitPa NV12 crop (image_w*pct × image_h*pct) shifted by off
 *          into a VENC ch0 input buffer, submit it.
 *
 * VPE → VENC is NOT bound on this path.  Dual ch1, JPEG snapshot, and the
 * debug OSD still see the unstabilized full port0 frame — only VENC ch0
 * gets the stabilized crop.  Configured by the video0.framing preset
 * (low|medium|high stab presets), which expand into the derived stab_crop_pct.
 *
 * Local types and dlsym-resolved symbols mirror the working standalone
 * star.c sample; pulling SigmaStar mi_sys.h / mi_ive.h here would collide
 * with waybeam's own MI compatibility layer in include/star6e.h. */

typedef MI_U64 STAB_MI_PHY;

typedef struct {
	MI_U16 u16X;
	MI_U16 u16Y;
	MI_U16 u16Width;
	MI_U16 u16Height;
} StabSysWindowRect_t;

typedef struct {
	int eTileMode;
	int ePixelFormat;
	int eCompressMode;
	int eFrameScanMode;
	int eFieldType;
	int ePhylayoutType;
	MI_U16 u16Width;
	MI_U16 u16Height;
	void *pVirAddr[3];
	STAB_MI_PHY phyAddr[3];
	MI_U32 u32Stride[3];
	MI_U32 u32BufSize;
	MI_U16 u16RingBufStartLine;
	MI_U16 u16RingBufRealTotalHeight;
	struct {
		int eType;
		union {
			MI_U32 u32GlobalGradient;
		} uIspInfo;
	} stFrameIspInfo;
	StabSysWindowRect_t stContentCropWindow;
} StabSysFrameData_t;

typedef struct {
	void *pVirAddr;
	STAB_MI_PHY phyAddr;
	MI_U32 u32BufSize;
	MI_U32 u32ContentSize;
	MI_BOOL bEndOfFrame;
	MI_U64 u64SeqNum;
} StabSysRawData_t;

typedef struct {
	void *pVirAddr;
	STAB_MI_PHY phyAddr;
	MI_U32 u32Size;
	MI_U32 u32ExtraData;
	MI_U32 eDataFromModule;
} StabSysMetaData_t;

typedef struct {
	MI_U64 u64Pts;
	MI_U64 u64SidebandMsg;
	int eBufType;
	MI_BOOL bEndOfStream;
	MI_BOOL bUsrBuf;
	MI_U32 u32SequenceNumber;
	MI_BOOL bDrop;
	union {
		StabSysFrameData_t stFrameData;
		StabSysRawData_t stRawData;
		StabSysMetaData_t stMetaData;
		MI_U8 reserved_union[512];
	};
	MI_U8 u8CusFlag;
} StabSysBufInfo_t;

typedef struct {
	MI_U16 u16BufHAlignment;
	MI_U16 u16BufVAlignment;
	MI_U16 u16BufChromaAlignment;
	MI_BOOL bClearPadding;
} StabSysFrameBufExtraConfig_t;

typedef struct {
	MI_U16 u16Width;
	MI_U16 u16Height;
	int eFrameScanMode;
	int eFormat;
	StabSysFrameBufExtraConfig_t stFrameBufExtraConf;
	int eCompressMode;
} StabSysBufFrameConfig_t;

typedef struct {
	int eBufType;
	MI_U32 u32Flags;
	MI_U64 u64TargetPts;
	union {
		StabSysBufFrameConfig_t stFrameCfg;
		struct { MI_U32 u32Size; } stRawCfg;
		struct { MI_U32 u32Size; } stMetaCfg;
	};
	MI_U8 u8CusFlag;
} StabSysBufConf_t;

typedef MI_S32 StabSysBufHandle_t;

#define STAB_E_BUFDATA_FRAME              1
#define STAB_E_FRAME_SCAN_MODE_PROGRESSIVE 0
#define STAB_E_PIXEL_FRAME_I8             9

typedef int StabIveImageType_e;
#define STAB_E_IVE_IMAGE_TYPE_U8C1 0x0
#define STAB_E_IVE_IMAGE_TYPE_S8C1 0x1

typedef struct {
	StabIveImageType_e eType;
	STAB_MI_PHY aphyPhyAddr[3];
	MI_U8 *apu8VirAddr[3];
	MI_U16 azu16Stride[3];
	MI_U16 u16Width;
	MI_U16 u16Height;
	MI_U16 u16Reserved;
} StabIveImage_t;

#define STAB_E_IVE_SHIFT_DETECT_MODE_SINGLE 0x00

typedef struct {
	int enMode;
	MI_U8 pyramid_level;
	MI_U8 search_range;
	MI_U16 u16Left;
	MI_U16 u16Top;
	MI_U16 u16Width;
	MI_U16 u16Height;
} StabIveShiftDetectCtrl_t;

typedef MI_S32 (*stab_sys_get_fd_fn_t)(MI_SYS_ChnPort_t *port, MI_S32 *fd);
typedef MI_S32 (*stab_sys_close_fd_fn_t)(MI_S32 fd);
typedef MI_S32 (*stab_sys_out_get_buf_fn_t)(MI_SYS_ChnPort_t *port,
	StabSysBufInfo_t *buf, StabSysBufHandle_t *handle);
typedef MI_S32 (*stab_sys_out_put_buf_fn_t)(StabSysBufHandle_t handle);
typedef MI_S32 (*stab_sys_in_get_buf_fn_t)(MI_SYS_ChnPort_t *port,
	StabSysBufConf_t *conf, StabSysBufInfo_t *buf,
	StabSysBufHandle_t *handle, MI_S32 timeout_ms);
typedef MI_S32 (*stab_sys_in_put_buf_fn_t)(StabSysBufHandle_t handle,
	StabSysBufInfo_t *buf, MI_BOOL drop);
typedef MI_S32 (*stab_sys_blit_pa_fn_t)(StabSysFrameData_t *dst,
	StabSysWindowRect_t *dst_rect, StabSysFrameData_t *src,
	StabSysWindowRect_t *src_rect);
typedef MI_S32 (*stab_sys_flush_inv_cache_fn_t)(void *vir, MI_U32 size);
typedef MI_S32 (*stab_sys_va2pa_fn_t)(void *vir, STAB_MI_PHY *phy);

typedef int StabIveHandle_t;
typedef MI_S32 (*stab_ive_create_fn_t)(StabIveHandle_t handle);
typedef MI_S32 (*stab_ive_destroy_fn_t)(StabIveHandle_t handle);
typedef MI_S32 (*stab_ive_shift_fn_t)(StabIveHandle_t handle,
	StabIveImage_t *src1, StabIveImage_t *src2,
	StabIveImage_t *dst_x, StabIveImage_t *dst_y,
	StabIveShiftDetectCtrl_t *ctrl, MI_BOOL instant);

/* Shift_Detector geometry.  On Star6E there is no IVE kernel module, so
 * MI_IVE_Shift_Detector runs as a userspace CPU fallback — it is the
 * dominant per-frame stab cost (~19ms on the A7), independent of the crop
 * resolution.  These are the FULL 384/256/3 values: a larger correlation
 * box and a 3-level pyramid give noticeably smoother motion estimates than
 * the cheapened 256/128/2 config (which was tried for fps but produced
 * visibly jittery/shaky stabilization — the estimates are noisier and the
 * offset is applied raw every frame).  Smoothness was chosen over the fps
 * the cheaper detector bought.  margin = (crop-box)/2 = 64px and
 * SEARCH_RANGE = 96 are unchanged. */
#define STAB_SHIFT_CROP_W   384
#define STAB_SHIFT_CROP_H   384
#define STAB_BOX_SIZE       256
#define STAB_PYRAMID        3
#define STAB_SEARCH_RANGE   96
#define STAB_SHIFT_SIGN_X   (-1)
#define STAB_SHIFT_SIGN_Y   (-1)
/* Run the (CPU-bound) detector every Nth drained frame.  N=1 (detect +
 * correct every frame) is required for smooth stabilization: sampling
 * motion below the frame rate aliases real jitter (Nyquist), so the
 * accumulator mis-corrects and the image visibly fights/shakes.  Keep
 * this at 1 unless a higher resolution makes the per-frame detector cost
 * exceed the frame budget — at 1152x864 the cheapened detector (256 crop,
 * 128 box, 2-level pyramid) fits 60fps every frame.  Bump only as a last
 * resort; it trades stabilization quality for fps. */
#define STAB_DETECT_EVERY   1
/* Return-to-center policy (see the recenter block in the stab thread).
 * Unconditional decay fights live stabilization, so gate it:
 *  - MOTION_THRESH: |inter-frame shift| (px) above which the camera counts
 *    as actively moving — re-arms the stillness timer.
 *  - STILL_FRAMES: consecutive sub-threshold frames before the offset
 *    decays fully back to center (the "cooldown").
 *  - EDGE_PCT: while still moving, only give margin back on an axis once
 *    its offset passes this % of the dead-border, so corrections in the
 *    central zone are never eroded (no fight); during sustained motion the
 *    offset settles near the edge instead of being pinned/saturated. */
#define STAB_MOTION_THRESH         1
#define STAB_RECENTER_STILL_FRAMES 30
#define STAB_RECENTER_EDGE_PCT     70

static stab_sys_get_fd_fn_t g_stab_sys_get_fd;
static stab_sys_close_fd_fn_t g_stab_sys_close_fd;
static stab_sys_out_get_buf_fn_t g_stab_sys_out_get_buf;
static stab_sys_out_put_buf_fn_t g_stab_sys_out_put_buf;
static stab_sys_in_get_buf_fn_t g_stab_sys_in_get_buf;
static stab_sys_in_put_buf_fn_t g_stab_sys_in_put_buf;
static stab_sys_blit_pa_fn_t g_stab_sys_blit_pa;
static stab_sys_flush_inv_cache_fn_t g_stab_sys_flush_inv_cache;
static stab_sys_va2pa_fn_t g_stab_sys_va2pa;

static stab_ive_create_fn_t g_stab_ive_create;
static stab_ive_destroy_fn_t g_stab_ive_destroy;
static stab_ive_shift_fn_t g_stab_ive_shift;
static StabIveHandle_t g_stab_ive_handle;
static int g_stab_ive_created;
static void *g_stab_ive_lib;

static pthread_t g_stab_thread;
static volatile int g_stab_running;
/* Stabilization data path:
 *   HW (g_stab_hw_mode=1): VPE port0 hardware-crops the stab window straight
 *     to VENC (zero-copy bind); a tiny port1 256x256 tap feeds the detector.
 *     The detector thread updates port0's SetPortCrop rect per detect.  No
 *     per-frame BufBlitPa, and port0 tears down via the standard bound path.
 *   Legacy (g_stab_hw_mode=0): the historic single-port manual drain — port0
 *     full-frame, detector + per-frame BufBlitPa crop into VENC input.  Used
 *     only as a fallback when this BSP rejects a simultaneous port1.
 * g_stab_pause/parked are a quiesce handshake so teardown can disable port1
 * while the detector is guaranteed not inside an MI_SYS call. */
static volatile int g_stab_hw_mode;
static volatile int g_stab_pause;
static volatile int g_stab_parked;
static pthread_mutex_t g_stab_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_stab_src_w;     /* image (port0-output) domain — detector + */
static uint32_t g_stab_src_h;     /* accumulator + recenter all use this dim   */
static uint32_t g_stab_enc_w;
static uint32_t g_stab_enc_h;
/* VPE channel input dim (precrop / VIF→VPE window).  MI_VPE_SetPortCrop is an
 * INPUT-domain crop, so HW mode scales the image-domain stab window into this
 * domain.  Equals g_stab_src_* when the sensor isn't aspect-cropped (ratio 1).
 * Only HW-crop mode uses these. */
static uint32_t g_stab_pre_w;
static uint32_t g_stab_pre_h;
static uint32_t g_stab_crop_percent;
static uint32_t g_stab_recenter_period;   /* frames between 1-pixel leak; 0=off */
static volatile int g_stab_off_x;
static volatile int g_stab_off_y;
/* User-controlled pan center as parts-per-thousand of (src_w, src_h).
 * 500/500 = exact center.  Updated live via star6e_stab_set_pan() so
 * the existing zoomX/zoomY HTTP controls steer the stabilized framing
 * without a pipeline restart. */
static volatile int g_stab_pan_x_mil = 500;
static volatile int g_stab_pan_y_mil = 500;
static MI_SYS_ChnPort_t g_stab_vpe_port;
static MI_SYS_ChnPort_t g_stab_venc_port;

/* ── Gyro motion source (IMU-assisted stabilization seam) ────────────────
 * The BMI270 driver (imu_bmi270.c) already runs frame-synced; when
 * imu.enabled it pushes timestamped 6-axis samples here via
 * star6e_pipeline_imu_push().  Today motion is estimated optically with
 * MI_IVE_Shift_Detector; this ring makes CLOCK_MONOTONIC-stamped gyro data
 * available frame-aligned so a gyro/optical fusion can drop into
 * star6e_stab_estimate_shift() without new plumbing.  Populated whenever the
 * IMU is enabled (also useful for telemetry/sidecar logging), independent of
 * whether stabilization is active. */
static ImuRing g_stab_imu_ring;
static volatile int g_stab_imu_ring_ready;

static int star6e_stab_load_sys_extra_symbols(void)
{
	void *h;

	if (g_stab_sys_out_get_buf && g_stab_sys_out_put_buf &&
	    g_stab_sys_in_get_buf && g_stab_sys_in_put_buf &&
	    g_stab_sys_blit_pa && g_stab_sys_flush_inv_cache &&
	    g_stab_sys_va2pa)
		return 0;

	h = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h)
		return -1;

	g_stab_sys_get_fd = (stab_sys_get_fd_fn_t)dlsym(h, "MI_SYS_GetFd");
	g_stab_sys_close_fd = (stab_sys_close_fd_fn_t)dlsym(h, "MI_SYS_CloseFd");
	g_stab_sys_out_get_buf = (stab_sys_out_get_buf_fn_t)dlsym(h,
		"MI_SYS_ChnOutputPortGetBuf");
	g_stab_sys_out_put_buf = (stab_sys_out_put_buf_fn_t)dlsym(h,
		"MI_SYS_ChnOutputPortPutBuf");
	g_stab_sys_in_get_buf = (stab_sys_in_get_buf_fn_t)dlsym(h,
		"MI_SYS_ChnInputPortGetBuf");
	g_stab_sys_in_put_buf = (stab_sys_in_put_buf_fn_t)dlsym(h,
		"MI_SYS_ChnInputPortPutBuf");
	g_stab_sys_blit_pa = (stab_sys_blit_pa_fn_t)dlsym(h,
		"MI_SYS_BufBlitPa");
	g_stab_sys_flush_inv_cache = (stab_sys_flush_inv_cache_fn_t)dlsym(h,
		"MI_SYS_FlushInvCache");
	g_stab_sys_va2pa = (stab_sys_va2pa_fn_t)dlsym(h, "MI_SYS_Va2Pa");

	return (g_stab_sys_out_get_buf && g_stab_sys_out_put_buf &&
		g_stab_sys_in_get_buf && g_stab_sys_in_put_buf &&
		g_stab_sys_blit_pa && g_stab_sys_flush_inv_cache &&
		g_stab_sys_va2pa) ? 0 : -1;
}

static uint64_t star6e_stab_pts_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* Compute encoded dims preserving the configured image_w:image_h aspect.
 * Integer math only — no floats per CONVENTIONS.md §1.  Width is 8-aligned
 * down (VENC + SCL); height is 2-aligned down (NV12). */
static void star6e_stab_compute_crop_dims(uint32_t src_w, uint32_t src_h,
	uint32_t pct, uint32_t *out_w, uint32_t *out_h)
{
	uint32_t w;
	uint32_t h;

	w = (src_w * pct) / 100u;
	w &= ~7u;
	if (w == 0) w = src_w & ~7u;
	h = (uint32_t)(((uint64_t)w * src_h) / src_w);
	h &= ~1u;

	if (h > src_h) {
		h = ((src_h * pct) / 100u) & ~1u;
		if (h == 0) h = src_h & ~1u;
		w = (uint32_t)(((uint64_t)h * src_w) / src_h);
		w &= ~7u;
	}

	if (w < 64 || h < 64 || w > src_w || h > src_h) {
		w = src_w & ~7u;
		h = src_h & ~1u;
	}

	*out_w = w;
	*out_h = h;
}

static int star6e_stab_pan_clamp_mil(double v)
{
	int mil;

	if (!isfinite(v) || v <= 0.0) return 0;
	if (v >= 1.0) return 1000;
	mil = (int)(v * 1000.0 + 0.5);
	if (mil < 0) mil = 0;
	if (mil > 1000) mil = 1000;
	return mil;
}

static void star6e_stab_configure(uint32_t src_w, uint32_t src_h,
	uint32_t crop_pct, uint32_t recenter_speed, uint32_t venc_fps,
	double pan_x, double pan_y)
{
	g_stab_src_w = src_w & ~1u;
	g_stab_src_h = src_h & ~1u;
	g_stab_crop_percent = crop_pct;
	star6e_stab_compute_crop_dims(g_stab_src_w, g_stab_src_h,
		crop_pct, &g_stab_enc_w, &g_stab_enc_h);

	/* recenter_speed is "frames between 1-pixel leak":
	 * 0 = no leak (stick to current patch),
	 * lower = faster recenter (user feedback: "Lower the second number
	 * faster crop recenter, 0 is no recenter"). venc_fps is passed for
	 * future "pixels/sec" remapping; currently the value IS the period
	 * in frames so the knob stays direct + deterministic. */
	(void)venc_fps;
	g_stab_recenter_period = recenter_speed;
	g_stab_pan_x_mil = star6e_stab_pan_clamp_mil(pan_x);
	g_stab_pan_y_mil = star6e_stab_pan_clamp_mil(pan_y);

	pthread_mutex_lock(&g_stab_lock);
	g_stab_off_x = 0;
	g_stab_off_y = 0;
	pthread_mutex_unlock(&g_stab_lock);
}

/* Live pan update — called from the LIVE_GROUP_ZOOM apply path so that
 * the existing zoomX/zoomY HTTP controls steer the stabilized framing
 * without a pipeline restart. */
static void star6e_stab_set_pan(double pan_x, double pan_y)
{
	g_stab_pan_x_mil = star6e_stab_pan_clamp_mil(pan_x);
	g_stab_pan_y_mil = star6e_stab_pan_clamp_mil(pan_y);
	/* Keep the AE meter on the stabilized crop as it pans, mirroring the
	 * zoom path's AE tracking — the only intended runtime difference
	 * between the two modes is the pan ramp. */
	star6e_stab_apply_ae_crop();
}

int star6e_pipeline_stab_panel_anchor(int *out_x, int *out_y)
{
	int off_x;
	int off_y;
	int pan_x;
	int pan_y;
	int center_x;
	int center_y;
	int src_x;
	int src_y;
	int max_x;
	int max_y;

	if (!g_stab_running || g_stab_enc_w == 0 || g_stab_enc_h == 0)
		return 0;
	/* HW-crop mode: the OSD is 1:1 on the post-crop port0 output (static),
	 * so there is no pre-crop anchor to track — behave like non-stab. */
	if (g_stab_hw_mode)
		return 0;
	if (!out_x || !out_y)
		return 0;

	pthread_mutex_lock(&g_stab_lock);
	off_x = g_stab_off_x;
	off_y = g_stab_off_y;
	pthread_mutex_unlock(&g_stab_lock);

	pan_x = g_stab_pan_x_mil;
	pan_y = g_stab_pan_y_mil;
	center_x = (int)((g_stab_src_w * (uint32_t)pan_x) / 1000u);
	center_y = (int)((g_stab_src_h * (uint32_t)pan_y) / 1000u);
	src_x = center_x - (int)g_stab_enc_w / 2 + off_x;
	src_y = center_y - (int)g_stab_enc_h / 2 + off_y;
	max_x = (int)(g_stab_src_w - g_stab_enc_w);
	max_y = (int)(g_stab_src_h - g_stab_enc_h);
	if (src_x < 0) src_x = 0;
	if (src_x > max_x) src_x = max_x;
	if (src_y < 0) src_y = 0;
	if (src_y > max_y) src_y = max_y;

	*out_x = src_x;
	*out_y = src_y;
	return 1;
}

static int star6e_stab_max_off_x(void)
{
	return (int)((g_stab_src_w - g_stab_enc_w) / 2u);
}

static int star6e_stab_max_off_y(void)
{
	return (int)((g_stab_src_h - g_stab_enc_h) / 2u);
}

/* Map an image-domain length to the VPE-input (precrop) domain.  When the
 * sensor isn't aspect-cropped pre==src and this is the identity. */
static int star6e_stab_img_to_pre_x(int v)
{
	return (int)((int64_t)v * (int64_t)g_stab_pre_w / (int64_t)g_stab_src_w);
}
static int star6e_stab_img_to_pre_y(int v)
{
	return (int)((int64_t)v * (int64_t)g_stab_pre_h / (int64_t)g_stab_src_h);
}

/* HW-crop mode output: program VPE port0's SetPortCrop to the stab window
 * (enc_w x enc_h positioned at pan-center + accumulated shake offset).  This
 * is the hardware-SCL equivalent of the legacy per-frame BufBlitPa.  The
 * window is computed in image domain (identical to the legacy blit's src_x/
 * src_y math, so the accumulator/feel is unchanged) and then scaled into the
 * VPE INPUT (precrop) domain, because MI_VPE_SetPortCrop crops the channel
 * input — the SCL then scales that window down to the encoded port output,
 * the same downscale ratio the non-stab full-frame path uses.  x/y/w/h
 * aligned to 2 for NV12 chroma; the SDK accepts 2-px granularity (zoom path
 * precedent), preserving fine stabilization steps. */
static void star6e_stab_apply_port_crop(int acc_x, int acc_y)
{
	int pan_x = g_stab_pan_x_mil;
	int pan_y = g_stab_pan_y_mil;
	int center_x = (int)((g_stab_src_w * (uint32_t)pan_x) / 1000u);
	int center_y = (int)((g_stab_src_h * (uint32_t)pan_y) / 1000u);
	int src_x = center_x - (int)g_stab_enc_w / 2 + acc_x;
	int src_y = center_y - (int)g_stab_enc_h / 2 + acc_y;
	int max_x = (int)(g_stab_src_w - g_stab_enc_w);
	int max_y = (int)(g_stab_src_h - g_stab_enc_h);
	int rx, ry, rw, rh, rmax_x, rmax_y;
	i6_common_rect rect;
	MI_S32 ret;

	if (src_x < 0) src_x = 0;
	if (src_x > max_x) src_x = max_x;
	if (src_y < 0) src_y = 0;
	if (src_y > max_y) src_y = max_y;

	/* Scale image window → precrop (input) domain. */
	rx = star6e_stab_img_to_pre_x(src_x) & ~1;
	ry = star6e_stab_img_to_pre_y(src_y) & ~1;
	rw = star6e_stab_img_to_pre_x((int)g_stab_enc_w) & ~1;
	rh = star6e_stab_img_to_pre_y((int)g_stab_enc_h) & ~1;
	if (rw < 2) rw = 2;
	if (rh < 2) rh = 2;
	rmax_x = (int)g_stab_pre_w - rw;
	rmax_y = (int)g_stab_pre_h - rh;
	if (rx < 0) rx = 0;
	if (rx > rmax_x) rx = rmax_x;
	if (ry < 0) ry = 0;
	if (ry > rmax_y) ry = rmax_y;

	rect.x = (unsigned short)rx;
	rect.y = (unsigned short)ry;
	rect.width = (unsigned short)rw;
	rect.height = (unsigned short)rh;
	ret = MI_VPE_SetPortCrop(0, 0, &rect);
	if (ret != 0) {
		static int warned;
		if (!warned) {
			warned = 1;
			fprintf(stderr, "[waybeam] stab HW SetPortCrop(0,0) "
				"x=%d y=%d %dx%d (pre %ux%u) failed %d\n", rx, ry,
				rw, rh, g_stab_pre_w, g_stab_pre_h, (int)ret);
		}
	}
}

static STAB_MI_PHY star6e_stab_uv_pa(const StabSysFrameData_t *f, uint32_t h)
{
	if (f->phyAddr[1])
		return f->phyAddr[1];
	return f->phyAddr[0] + (STAB_MI_PHY)f->u32Stride[0] * h;
}

static int star6e_stab_blit_nv12_crop(StabSysFrameData_t *dst,
	const StabSysFrameData_t *src, int src_x, int src_y,
	int width, int height)
{
	StabSysFrameData_t src_y_frame;
	StabSysFrameData_t dst_y_frame;
	StabSysFrameData_t src_uv_frame;
	StabSysFrameData_t dst_uv_frame;
	StabSysWindowRect_t src_y_rect;
	StabSysWindowRect_t dst_y_rect;
	StabSysWindowRect_t src_uv_rect;
	StabSysWindowRect_t dst_uv_rect;
	STAB_MI_PHY src_uv_pa;
	STAB_MI_PHY dst_uv_pa;
	int src_y_stride;
	int dst_y_stride;
	int src_uv_stride;
	int dst_uv_stride;
	MI_S32 ret;

	if (!dst || !src || !dst->phyAddr[0] || !src->phyAddr[0])
		return -1;

	src_x &= ~1;
	src_y &= ~1;
	width &= ~1;
	height &= ~1;

	if (src_x < 0) src_x = 0;
	if (src_y < 0) src_y = 0;
	if (src_x + width > (int)g_stab_src_w)
		src_x = (int)g_stab_src_w - width;
	if (src_y + height > (int)g_stab_src_h)
		src_y = (int)g_stab_src_h - height;

	src_y_stride = (int)src->u32Stride[0];
	dst_y_stride = (int)dst->u32Stride[0];
	src_uv_stride = src->u32Stride[1] ? (int)src->u32Stride[1] : src_y_stride;
	dst_uv_stride = dst->u32Stride[1] ? (int)dst->u32Stride[1] : dst_y_stride;
	src_uv_pa = star6e_stab_uv_pa(src, g_stab_src_h);
	dst_uv_pa = star6e_stab_uv_pa(dst, g_stab_enc_h);
	if (!src_uv_pa || !dst_uv_pa)
		return -1;

	memset(&src_y_frame, 0, sizeof(src_y_frame));
	memset(&dst_y_frame, 0, sizeof(dst_y_frame));
	memset(&src_y_rect, 0, sizeof(src_y_rect));
	memset(&dst_y_rect, 0, sizeof(dst_y_rect));

	src_y_frame.ePixelFormat = STAB_E_PIXEL_FRAME_I8;
	src_y_frame.phyAddr[0] = src->phyAddr[0];
	src_y_frame.u16Width = (MI_U16)g_stab_src_w;
	src_y_frame.u16Height = (MI_U16)g_stab_src_h;
	src_y_frame.u32Stride[0] = src_y_stride;
	dst_y_frame.ePixelFormat = STAB_E_PIXEL_FRAME_I8;
	dst_y_frame.phyAddr[0] = dst->phyAddr[0];
	dst_y_frame.u16Width = (MI_U16)g_stab_enc_w;
	dst_y_frame.u16Height = (MI_U16)g_stab_enc_h;
	dst_y_frame.u32Stride[0] = dst_y_stride;
	src_y_rect.u16X = (MI_U16)src_x;
	src_y_rect.u16Y = (MI_U16)src_y;
	src_y_rect.u16Width = (MI_U16)width;
	src_y_rect.u16Height = (MI_U16)height;
	dst_y_rect.u16Width = (MI_U16)width;
	dst_y_rect.u16Height = (MI_U16)height;

	ret = g_stab_sys_blit_pa(&dst_y_frame, &dst_y_rect,
		&src_y_frame, &src_y_rect);
	if (ret != 0)
		return ret;

	memset(&src_uv_frame, 0, sizeof(src_uv_frame));
	memset(&dst_uv_frame, 0, sizeof(dst_uv_frame));
	memset(&src_uv_rect, 0, sizeof(src_uv_rect));
	memset(&dst_uv_rect, 0, sizeof(dst_uv_rect));

	src_uv_frame.ePixelFormat = STAB_E_PIXEL_FRAME_I8;
	src_uv_frame.phyAddr[0] = src_uv_pa;
	src_uv_frame.u16Width = (MI_U16)g_stab_src_w;
	src_uv_frame.u16Height = (MI_U16)(g_stab_src_h / 2u);
	src_uv_frame.u32Stride[0] = src_uv_stride;
	dst_uv_frame.ePixelFormat = STAB_E_PIXEL_FRAME_I8;
	dst_uv_frame.phyAddr[0] = dst_uv_pa;
	dst_uv_frame.u16Width = (MI_U16)g_stab_enc_w;
	dst_uv_frame.u16Height = (MI_U16)(g_stab_enc_h / 2u);
	dst_uv_frame.u32Stride[0] = dst_uv_stride;
	src_uv_rect.u16X = (MI_U16)src_x;
	src_uv_rect.u16Y = (MI_U16)(src_y / 2);
	src_uv_rect.u16Width = (MI_U16)width;
	src_uv_rect.u16Height = (MI_U16)(height / 2);
	dst_uv_rect.u16Width = (MI_U16)width;
	dst_uv_rect.u16Height = (MI_U16)(height / 2);

	return g_stab_sys_blit_pa(&dst_uv_frame, &dst_uv_rect,
		&src_uv_frame, &src_uv_rect);
}

static int star6e_stab_make_center_y_crop(StabIveImage_t *image,
	const StabSysBufInfo_t *buf, int crop_w, int crop_h)
{
	int src_w;
	int src_h;
	int stride;
	int crop_x;
	int crop_y;

	if (!image || !buf || buf->eBufType != STAB_E_BUFDATA_FRAME ||
	    !buf->stFrameData.pVirAddr[0] || !buf->stFrameData.phyAddr[0])
		return -1;

	src_w = (int)buf->stFrameData.u16Width;
	src_h = (int)buf->stFrameData.u16Height;
	stride = (int)buf->stFrameData.u32Stride[0];
	if (crop_w > src_w) crop_w = src_w;
	if (crop_h > src_h) crop_h = src_h;
	crop_w &= ~15;
	crop_h &= ~1;
	crop_x = ((src_w - crop_w) / 2) & ~15;
	crop_y = ((src_h - crop_h) / 2) & ~1;
	if (crop_x < 0) crop_x = 0;
	if (crop_y < 0) crop_y = 0;

	memset(image, 0, sizeof(*image));
	image->eType = STAB_E_IVE_IMAGE_TYPE_U8C1;
	image->u16Width = (MI_U16)crop_w;
	image->u16Height = (MI_U16)crop_h;
	image->apu8VirAddr[0] = (MI_U8 *)buf->stFrameData.pVirAddr[0] +
		crop_y * stride + crop_x;
	image->aphyPhyAddr[0] = buf->stFrameData.phyAddr[0] +
		(STAB_MI_PHY)(crop_y * stride + crop_x);
	image->azu16Stride[0] = (MI_U16)stride;
	return 0;
}

static int star6e_stab_alloc_ive_image(StabIveImage_t *image,
	MI_U16 width, MI_U16 height, StabIveImageType_e type)
{
	MI_U32 align = 64;
	MI_U32 size;
	STAB_MI_PHY phy = 0;
	MI_S32 ret;

	memset(image, 0, sizeof(*image));
	image->eType = type;
	image->u16Width = width;
	image->u16Height = height;
	image->azu16Stride[0] = (width + (align - 1)) & ~(align - 1);
	size = image->azu16Stride[0] * height;
	if (posix_memalign((void **)&image->apu8VirAddr[0], align, size) != 0)
		return -1;
	memset(image->apu8VirAddr[0], 0, size);

	/* IVE needs a valid physical address even for 1×1 S8C1 result images;
	 * leaving aphyPhyAddr[0]=0 makes Shift_Detector return zero dx/dy. */
	ret = g_stab_sys_va2pa(image->apu8VirAddr[0], &phy);
	if (ret != 0 || !phy)
		phy = (STAB_MI_PHY)(uintptr_t)image->apu8VirAddr[0];
	image->aphyPhyAddr[0] = phy;
	return 0;
}

static int star6e_stab_send_frame_to_venc(const StabSysBufInfo_t *src_buf)
{
	StabSysBufConf_t conf;
	StabSysBufInfo_t venc_buf;
	StabSysBufHandle_t venc_handle = 0;
	int off_x;
	int off_y;
	int max_x;
	int max_y;
	int src_x;
	int src_y;
	MI_S32 ret;

	memset(&conf, 0, sizeof(conf));
	conf.eBufType = STAB_E_BUFDATA_FRAME;
	conf.u64TargetPts = star6e_stab_pts_us();
	conf.stFrameCfg.eFormat = I6_PIXFMT_YUV420SP;
	conf.stFrameCfg.eFrameScanMode = STAB_E_FRAME_SCAN_MODE_PROGRESSIVE;
	conf.stFrameCfg.u16Width = (MI_U16)g_stab_enc_w;
	conf.stFrameCfg.u16Height = (MI_U16)g_stab_enc_h;

	memset(&venc_buf, 0, sizeof(venc_buf));
	ret = g_stab_sys_in_get_buf(&g_stab_venc_port, &conf,
		&venc_buf, &venc_handle, 20);
	if (ret != 0)
		return ret;

	pthread_mutex_lock(&g_stab_lock);
	off_x = g_stab_off_x;
	off_y = g_stab_off_y;
	pthread_mutex_unlock(&g_stab_lock);

	/* User pan + stab shake-correction.  The crop window center is placed
	 * at (pan_x, pan_y) parts-per-thousand of the source frame; the IVE
	 * shift accumulator then shifts the window further to cancel camera
	 * motion.  Asymmetric clamps follow naturally from clipping src_x /
	 * src_y into [0, src - enc]: when the user pans hard toward an edge,
	 * stab loses headroom on that side first. */
	{
		int pan_x = g_stab_pan_x_mil;
		int pan_y = g_stab_pan_y_mil;
		int center_x = (int)((g_stab_src_w * (uint32_t)pan_x) / 1000u);
		int center_y = (int)((g_stab_src_h * (uint32_t)pan_y) / 1000u);
		src_x = center_x - (int)g_stab_enc_w / 2 + off_x;
		src_y = center_y - (int)g_stab_enc_h / 2 + off_y;
		max_x = (int)(g_stab_src_w - g_stab_enc_w);
		max_y = (int)(g_stab_src_h - g_stab_enc_h);
		if (src_x < 0) src_x = 0;
		if (src_x > max_x) src_x = max_x;
		if (src_y < 0) src_y = 0;
		if (src_y > max_y) src_y = max_y;
	}
	ret = star6e_stab_blit_nv12_crop(&venc_buf.stFrameData,
		&src_buf->stFrameData, src_x, src_y,
		(int)g_stab_enc_w, (int)g_stab_enc_h);
	if (ret != 0) {
		g_stab_sys_in_put_buf(venc_handle, &venc_buf, true);
		return ret;
	}

	return g_stab_sys_in_put_buf(venc_handle, &venc_buf, false);
}

/* Read the gyro samples captured during the frame interval [t0, t1] from the
 * shared IMU ring (CLOCK_MONOTONIC domain).  Returns the sample count and,
 * when non-zero, the mean angular rate (rad/s) on each axis.  This is the
 * frame-aligned data a gyro estimator integrates.  Returns 0 with zeroed
 * means when the IMU is disabled or no samples fell in the window — the
 * stabilizer then runs optical-only. */
static uint32_t star6e_stab_gyro_window(struct timespec t0, struct timespec t1,
	float *mean_gx, float *mean_gy, float *mean_gz)
{
	ImuRingSample s[64];
	uint32_t n, i;
	double sx = 0.0, sy = 0.0, sz = 0.0;

	*mean_gx = 0.0f;
	*mean_gy = 0.0f;
	*mean_gz = 0.0f;
	if (!g_stab_imu_ring_ready)
		return 0;
	n = imu_ring_read_range(&g_stab_imu_ring, t0, t1, s,
		(uint32_t)(sizeof(s) / sizeof(s[0])));
	for (i = 0; i < n; i++) {
		sx += s[i].gyro_x;
		sy += s[i].gyro_y;
		sz += s[i].gyro_z;
	}
	if (n > 0) {
		*mean_gx = (float)(sx / n);
		*mean_gy = (float)(sy / n);
		*mean_gz = (float)(sz / n);
	}
	return n;
}

/* Per-frame motion estimate.  On success returns 0 and fills out_dx/out_dy
 * with the measured inter-frame shift (signed pixels, source-frame domain;
 * pan/recenter applied by the caller); non-zero on detector failure.
 *
 * IMU-gyro fusion seam: today the estimate is purely optical
 * (MI_IVE_Shift_Detector).  To add gyro-assisted stabilization, read the
 * frame-aligned angular rates via star6e_stab_gyro_window(prev_ts, curr_ts,
 * ...), integrate yaw/pitch over the interval into a pixel shift using the
 * lens focal length (focal_px = (src_w/2) / tan(hfov/2)), and fuse it with
 * the optical shift here (e.g. a complementary filter: gyro for fast jitter,
 * optical to cancel gyro drift).  Nothing else in the pipeline changes. */
static int star6e_stab_estimate_shift(StabIveImage_t *prev_img,
	StabIveImage_t *curr_img, StabIveImage_t *dx, StabIveImage_t *dy,
	int *out_dx, int *out_dy)
{
	int img_w = (int)curr_img->u16Width;
	int img_h = (int)curr_img->u16Height;
	int box = STAB_BOX_SIZE;
	int left, top;
	MI_S32 ret;

	if (box > img_w) box = img_w;
	if (box > img_h) box = img_h;
	left = ((img_w - box) / 2) & ~1;
	top  = ((img_h - box) / 2) & ~1;

	{
		StabIveShiftDetectCtrl_t ctrl = {
			.enMode = STAB_E_IVE_SHIFT_DETECT_MODE_SINGLE,
			.pyramid_level = STAB_PYRAMID,
			.search_range = STAB_SEARCH_RANGE,
			.u16Left = (MI_U16)left,
			.u16Top = (MI_U16)top,
			.u16Width = (MI_U16)box,
			.u16Height = (MI_U16)box,
		};
		ret = g_stab_ive_shift(g_stab_ive_handle,
			prev_img, curr_img, dx, dy, &ctrl, true);
	}
	if (ret != 0)
		return ret;

	g_stab_sys_flush_inv_cache(dx->apu8VirAddr[0], dx->azu16Stride[0]);
	g_stab_sys_flush_inv_cache(dy->apu8VirAddr[0], dy->azu16Stride[0]);
	*out_dx = STAB_SHIFT_SIGN_X * (int)((int8_t *)dx->apu8VirAddr[0])[0];
	*out_dy = STAB_SHIFT_SIGN_Y * (int)((int8_t *)dy->apu8VirAddr[0])[0];
	return 0;
}

static void *star6e_stab_thread_main(void *arg)
{
	MI_S32 ret;
	MI_S32 fd = -1;
	StabSysBufHandle_t prev_handle = 0;
	StabIveImage_t prev_img;
	int have_prev = 0;
	/* Shake-correction offset accumulator.  Held in float (facc_*) so the
	 * return-to-center decays exact-proportionally and both axes converge
	 * along the true diagonal; acc_* are the rounded ints handed to the crop
	 * and OSD each detect. */
	double facc_x = 0.0;
	double facc_y = 0.0;
	int acc_x = 0;
	int acc_y = 0;
	int dbg_frame = 0;
	int loop_n = 0;            /* drained frames since have_prev; gates detect */
	int still_frames = STAB_RECENTER_STILL_FRAMES;  /* recenter cooldown; start settled */
	StabIveImage_t dx;
	StabIveImage_t dy;
	struct timespec prev_ts;
	struct timespec curr_ts;

	(void)arg;
	memset(&prev_ts, 0, sizeof(prev_ts));
	memset(&prev_img, 0, sizeof(prev_img));
	memset(&dx, 0, sizeof(dx));
	memset(&dy, 0, sizeof(dy));

	if (star6e_stab_alloc_ive_image(&dx, 1, 1,
	    STAB_E_IVE_IMAGE_TYPE_S8C1) != 0 ||
	    star6e_stab_alloc_ive_image(&dy, 1, 1,
	    STAB_E_IVE_IMAGE_TYPE_S8C1) != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab IVE result alloc failed — "
			"thread exiting, VENC ch0 will not receive frames\n");
		goto out;
	}

	if (g_stab_sys_get_fd)
		ret = g_stab_sys_get_fd(&g_stab_vpe_port, &fd);
	else
		ret = -1;
	if (ret != 0)
		fd = -1;

	while (g_stab_running) {
		StabSysBufInfo_t curr_buf;
		StabSysBufHandle_t curr_handle = 0;
		StabIveImage_t curr_img;
		int meas_dx = 0;
		int meas_dy = 0;
		float gyro_x = 0.0f;
		float gyro_y = 0.0f;
		float gyro_z = 0.0f;
		uint32_t gyro_n;

		memset(&curr_buf, 0, sizeof(curr_buf));

		/* Quiesce handshake: when teardown raises g_stab_pause, park here
		 * — not inside any MI_SYS GetBuf/PutBuf — so star6e_stab_stop can
		 * safely DisablePort(0,1) without racing the drain on the port. */
		if (g_stab_pause) {
			g_stab_parked = 1;
			usleep(2000);
			continue;
		}
		g_stab_parked = 0;

		if (fd >= 0) {
			fd_set rfds;
			struct timeval tv;

			FD_ZERO(&rfds);
			FD_SET(fd, &rfds);
			tv.tv_sec = 0;
			tv.tv_usec = 50000;
			ret = select(fd + 1, &rfds, NULL, NULL, &tv);
			if (ret <= 0 || !FD_ISSET(fd, &rfds))
				continue;
		}

		ret = g_stab_sys_out_get_buf(&g_stab_vpe_port,
			&curr_buf, &curr_handle);
		if (ret != 0) {
			if (fd < 0)
				usleep(1000);
			continue;
		}

		if (curr_buf.eBufType != STAB_E_BUFDATA_FRAME ||
		    !curr_buf.stFrameData.phyAddr[0] ||
		    !curr_buf.stFrameData.pVirAddr[0]) {
			g_stab_sys_out_put_buf(curr_handle);
			continue;
		}

		if (star6e_stab_make_center_y_crop(&curr_img, &curr_buf,
		    STAB_SHIFT_CROP_W, STAB_SHIFT_CROP_H) != 0) {
			g_stab_sys_out_put_buf(curr_handle);
			continue;
		}

		clock_gettime(CLOCK_MONOTONIC, &curr_ts);

		if (!have_prev) {
			/* HW mode: VENC is hardware-fed from port0; the detector tap
			 * (port1) only seeds the reference frame here.  Legacy mode:
			 * push the first frame into VENC's input. */
			if (!g_stab_hw_mode) {
				ret = star6e_stab_send_frame_to_venc(&curr_buf);
				if (ret != 0 && (dbg_frame++ % 60) == 0)
					fprintf(stderr, "[waybeam] stab first venc send "
						"failed ret=0x%x\n", ret);
			}
			prev_handle = curr_handle;
			prev_img = curr_img;
			prev_ts = curr_ts;
			have_prev = 1;
			continue;
		}

		/* Estimate motion only every STAB_DETECT_EVERY-th frame — the
		 * detector is the CPU bottleneck.  prev_img/prev_handle are held
		 * across skipped frames, so each detect spans the full interval
		 * since the last detect (no motion lost).  The crop blit + VENC
		 * send below run on every frame regardless, keeping output at
		 * sensor fps. */
		loop_n++;
		if ((loop_n % STAB_DETECT_EVERY) == 0) {
			/* Gyro samples for this interval (frame-aligned, optical-only
			 * today — see star6e_stab_estimate_shift for the fusion seam). */
			gyro_n = star6e_stab_gyro_window(prev_ts, curr_ts,
				&gyro_x, &gyro_y, &gyro_z);

			ret = star6e_stab_estimate_shift(&prev_img, &curr_img, &dx, &dy,
				&meas_dx, &meas_dy);

			if (ret == 0) {
				int max_x;
				int max_y;

				facc_x += meas_dx;
				facc_y += meas_dy;
				max_x = star6e_stab_max_off_x();
				max_y = star6e_stab_max_off_y();
				if (facc_x < -max_x) facc_x = -max_x;
				if (facc_x >  max_x) facc_x =  max_x;
				if (facc_y < -max_y) facc_y = -max_y;
				if (facc_y >  max_y) facc_y =  max_y;

				/* Gated return-to-center.  Recenter when the camera has
				 * settled (still for STILL_FRAMES), or while moving once the
				 * offset has pushed past EDGE_PCT of the dead-border on
				 * either axis (reclaim margin near saturation) — leaving the
				 * central zone untouched so it doesn't fight live
				 * stabilization.  When recentering, decay the (acc_x, acc_y)
				 * VECTOR's magnitude by (tau-1)/tau with its direction held
				 * constant, so both axes shrink proportionally and reach
				 * center together along a straight diagonal.  (Per-axis decay
				 * truncates the shorter axis to zero first, leaving a visible
				 * axis-aligned tail.)  nmag<1 snaps both axes to 0 together. */
				if (g_stab_recenter_period > 0) {
					uint32_t tau = g_stab_recenter_period;
					int settled;
					int edge_x = (max_x * STAB_RECENTER_EDGE_PCT) / 100;
					int edge_y = (max_y * STAB_RECENTER_EDGE_PCT) / 100;
					int do_recenter;
					if (tau < 2) tau = 2;

					if (abs(meas_dx) > STAB_MOTION_THRESH ||
					    abs(meas_dy) > STAB_MOTION_THRESH)
						still_frames = 0;
					else if (still_frames < STAB_RECENTER_STILL_FRAMES)
						still_frames++;
					settled = (still_frames >= STAB_RECENTER_STILL_FRAMES);

					do_recenter = settled ||
						facc_x > edge_x || facc_x < -edge_x ||
						facc_y > edge_y || facc_y < -edge_y;
					if (do_recenter) {
						/* Scale both axes by the same factor → direction
						 * held, straight diagonal to center.  Float math
						 * keeps the proportion exact at sub-pixel
						 * magnitudes; snap both to 0 together once the
						 * whole vector is under half a pixel. */
						double scale = (double)(tau - 1) / (double)tau;
						facc_x *= scale;
						facc_y *= scale;
						if (fabs(facc_x) < 0.5 && fabs(facc_y) < 0.5) {
							facc_x = 0.0;
							facc_y = 0.0;
						}
					}
				}

				acc_x = (int)lround(facc_x);
				acc_y = (int)lround(facc_y);
				pthread_mutex_lock(&g_stab_lock);
				g_stab_off_x = acc_x;
				g_stab_off_y = acc_y;
				pthread_mutex_unlock(&g_stab_lock);
				dbg_frame++;
				if ((dbg_frame % 120) == 0)
					fprintf(stderr, "[waybeam] stab tick %d: meas=(%d,%d) "
						"acc=(%d,%d) max=(%d,%d) pan=(%d,%d) still=%d "
						"gyro_n=%u gyro=(%.3f,%.3f,%.3f)\n",
						dbg_frame, meas_dx, meas_dy,
						acc_x, acc_y, max_x, max_y,
						g_stab_pan_x_mil, g_stab_pan_y_mil, still_frames,
						gyro_n, gyro_x, gyro_y, gyro_z);
			} else {
				if ((dbg_frame++ % 120) == 0)
					fprintf(stderr, "[waybeam] stab Shift_Detector "
						"ret=0x%x\n", (unsigned)ret);
			}

			/* Emit the new offset, then rotate prev to this frame.
			 * HW mode: reprogram port0's hardware crop (VENC is fed by
			 * the bind).  Legacy mode: BufBlitPa the crop into VENC. */
			if (g_stab_hw_mode) {
				star6e_stab_apply_port_crop(acc_x, acc_y);
			} else {
				ret = star6e_stab_send_frame_to_venc(&curr_buf);
				if (ret != 0 && (dbg_frame % 60) == 0)
					fprintf(stderr, "[waybeam] stab venc send failed "
						"ret=0x%x\n", ret);
			}
			if (prev_handle)
				g_stab_sys_out_put_buf(prev_handle);
			prev_handle = curr_handle;
			prev_img = curr_img;
			prev_ts = curr_ts;
		} else {
			/* Skipped-detect frame.  HW mode: nothing to emit (port0
			 * holds the last crop, VENC is hardware-fed).  Legacy mode:
			 * re-output with the current accumulator.  Keep prev as the
			 * last detected frame so the next detect spans the interval. */
			if (!g_stab_hw_mode) {
				ret = star6e_stab_send_frame_to_venc(&curr_buf);
				if (ret != 0 && (dbg_frame % 60) == 0)
					fprintf(stderr, "[waybeam] stab venc send failed "
						"ret=0x%x\n", ret);
			}
			g_stab_sys_out_put_buf(curr_handle);
		}
	}

	if (prev_handle)
		g_stab_sys_out_put_buf(prev_handle);
	if (fd >= 0 && g_stab_sys_close_fd)
		g_stab_sys_close_fd(fd);
out:
	free(dx.apu8VirAddr[0]);
	free(dy.apu8VirAddr[0]);
	return NULL;
}

/* Post-bind VPE port0 reapply.  When stab is enabled, port0 stays at the
 * full src dim (NV12, no scale); standalone DIS code re-sets the port
 * AFTER the VIF→VPE bind and bumps the output queue depth so the manual
 * drain thread keeps up.  Do NOT DisablePort first — on Star6E that can
 * leave port0 in a Pixel-MAX / 0×0 state when no VPE→VENC bind exists. */
static int star6e_stab_reapply_vpe_port(uint32_t src_w, uint32_t src_h)
{
	MI_VPE_PortAttr_t port = {0};
	MI_SYS_ChnPort_t vpe0 = {
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0, .port = 0 };
	MI_S32 ret;

	port.output.width = src_w;
	port.output.height = src_h;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;

	ret = MI_VPE_SetPortMode(0, 0, &port);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab VPE SetPortMode %ux%u "
			"failed %d\n", src_w, src_h, (int)ret);
		return ret;
	}
	ret = MI_VPE_EnablePort(0, 0);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab VPE EnablePort failed "
			"%d\n", (int)ret);
		return ret;
	}
	ret = MI_SYS_SetChnOutputPortDepth(&vpe0, 4, 8);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab SetChnOutputPortDepth "
			"failed %d\n", (int)ret);
		return ret;
	}
	return 0;
}

/* Set up VPE output ports for stabilization and choose the data path.
 *
 * Preferred (HW-crop): port0 outputs the encoded dim and is hardware-bound to
 * VENC ch0; the detector reads a tiny port1 256x256 centre tap.  Each detect
 * reprograms port0's SetPortCrop — no software blit, and port0 tears down via
 * the standard bound-port path (state->bound_vpe_venc=1).  Sets
 * g_stab_hw_mode=1, g_stab_vpe_port=port1.
 *
 * Fallback (legacy blit): if this BSP rejects a simultaneous port1, port0 is
 * reverted to full-src and manually drained + BufBlitPa'd into VENC's input
 * (the historic path).  Sets g_stab_hw_mode=0, g_stab_vpe_port=port0,
 * state->bound_vpe_venc=0.
 *
 * Returns 0 on success (either path) with the mode globals set, <0 on a hard
 * failure the caller must abort on. */
static int star6e_stab_setup_ports(Star6ePipelineState *state,
	uint32_t bind_src_fps, uint32_t bind_dst_fps)
{
	MI_VPE_PortAttr_t port = {0};
	MI_SYS_ChnPort_t vpe0 = {
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0, .port = 0 };
	MI_SYS_ChnPort_t vpe1 = {
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0, .port = 1 };
	i6_common_rect rect;
	int port1_enabled = 0;
	MI_S32 ret;

	g_stab_hw_mode = 0;
	g_stab_venc_port = state->venc_port;
	state->bound_vpe_venc = 0;

	/* VPE channel input (precrop) dim — the SetPortCrop coordinate domain.
	 * Set before any star6e_stab_apply_port_crop / img_to_pre call below. */
	g_stab_pre_w = state->active_precrop.w ? state->active_precrop.w
		: g_stab_src_w;
	g_stab_pre_h = state->active_precrop.h ? state->active_precrop.h
		: g_stab_src_h;

	/* port0: encoded-dim output (the SCL will crop the src into this). */
	port.output.width = g_stab_enc_w;
	port.output.height = g_stab_enc_h;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;
	ret = MI_VPE_SetPortMode(0, 0, &port);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab port0 SetPortMode %ux%u "
			"failed %d\n", g_stab_enc_w, g_stab_enc_h, (int)ret);
		return ret;   /* even the legacy path needs a working port0 */
	}
	ret = MI_VPE_EnablePort(0, 0);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab port0 EnablePort failed "
			"%d\n", (int)ret);
		return ret;
	}

	/* port1: 256x256 centre detector tap — the BSP-dependent step. */
	memset(&port, 0, sizeof(port));
	port.output.width = STAB_SHIFT_CROP_W;
	port.output.height = STAB_SHIFT_CROP_H;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;
	ret = MI_VPE_SetPortMode(0, 1, &port);
	if (ret == 0) {
		ret = MI_VPE_EnablePort(0, 1);
		if (ret == 0)
			port1_enabled = 1;
	}
	if (ret == 0) {
		/* Detector tap: crop a centre window of the precrop input that maps
		 * to a 256x256 IMAGE-domain patch, scaled to the 256x256 port1
		 * output.  The detector then measures motion in image pixels —
		 * identical to the legacy centre crop — so the accumulator and
		 * clamps stay mode-agnostic (image domain). */
		int dw = star6e_stab_img_to_pre_x(STAB_SHIFT_CROP_W) & ~1;
		int dh = star6e_stab_img_to_pre_y(STAB_SHIFT_CROP_H) & ~1;
		int dcx, dcy;
		if (dw < 2) dw = 2;
		if (dh < 2) dh = 2;
		if (dw > (int)g_stab_pre_w) dw = (int)g_stab_pre_w & ~1;
		if (dh > (int)g_stab_pre_h) dh = (int)g_stab_pre_h & ~1;
		dcx = (((int)g_stab_pre_w - dw) / 2) & ~1;
		dcy = (((int)g_stab_pre_h - dh) / 2) & ~1;
		if (dcx < 0) dcx = 0;
		if (dcy < 0) dcy = 0;
		rect.x = (unsigned short)dcx;
		rect.y = (unsigned short)dcy;
		rect.width = (unsigned short)dw;
		rect.height = (unsigned short)dh;
		ret = MI_VPE_SetPortCrop(0, 1, &rect);
	}
	if (ret == 0) {
		MI_SYS_SetChnOutputPortDepth(&vpe1, 2, 4);
		ret = MI_SYS_BindChnPort2(&state->vpe_port, &state->venc_port,
			bind_src_fps, bind_dst_fps, I6_SYS_LINK_FRAMEBASE, 0);
		if (ret == 0) {
			MI_SYS_SetChnOutputPortDepth(&state->venc_port, 1, 3);
			state->bound_vpe_venc = 1;
			g_stab_vpe_port = vpe1;   /* detector drains port1 */
			g_stab_hw_mode = 1;
			/* Centre the initial crop window now that port0 is bound
			 * (only after committing to HW mode, so a fallback never
			 * leaves a stray crop on port0). */
			star6e_stab_apply_port_crop(0, 0);
			fprintf(stderr, "[waybeam] stab: HW-crop mode "
				"(port0->VENC bind, port1 %dx%d detector tap)\n",
				STAB_SHIFT_CROP_W, STAB_SHIFT_CROP_H);
			return 0;
		}
		fprintf(stderr, "[waybeam] WARNING: stab port0->VENC bind "
			"failed %d; falling back to legacy blit\n", (int)ret);
	} else {
		fprintf(stderr, "[waybeam] WARNING: stab port1 tap unavailable "
			"(%d); falling back to legacy blit\n", (int)ret);
	}

	/* Fallback: tear down the tap and revert port0 to a full-src manual
	 * drain.  reapply re-SetPortModes/EnablePorts port0 (double EnablePort
	 * is the same pattern the original channel-start+reapply flow used). */
	if (port1_enabled)
		MI_VPE_DisablePort(0, 1);
	ret = star6e_stab_reapply_vpe_port(g_stab_src_w, g_stab_src_h);
	if (ret != 0)
		return ret;
	g_stab_vpe_port = vpe0;
	g_stab_hw_mode = 0;
	state->bound_vpe_venc = 0;
	fprintf(stderr, "[waybeam] stab: legacy blit mode "
		"(port0 full-src manual drain)\n");
	return 0;
}

static int star6e_stab_start(void)
{
	MI_S32 ret;

	g_stab_pause = 0;
	g_stab_parked = 0;

	if (star6e_stab_load_sys_extra_symbols() != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab cannot resolve required "
			"MI_SYS symbols\n");
		return -1;
	}

	g_stab_ive_lib = dlopen("libmi_ive.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!g_stab_ive_lib)
		g_stab_ive_lib = dlopen("libive.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!g_stab_ive_lib) {
		fprintf(stderr, "[waybeam] ERROR: stab cannot dlopen "
			"libmi_ive.so / libive.so\n");
		return -1;
	}

	g_stab_ive_create = (stab_ive_create_fn_t)dlsym(g_stab_ive_lib,
		"MI_IVE_Create");
	g_stab_ive_destroy = (stab_ive_destroy_fn_t)dlsym(g_stab_ive_lib,
		"MI_IVE_Destroy");
	g_stab_ive_shift = (stab_ive_shift_fn_t)dlsym(g_stab_ive_lib,
		"MI_IVE_Shift_Detector");
	if (!g_stab_ive_create || !g_stab_ive_destroy || !g_stab_ive_shift) {
		fprintf(stderr, "[waybeam] ERROR: stab missing IVE symbols\n");
		dlclose(g_stab_ive_lib);
		g_stab_ive_lib = NULL;
		return -1;
	}

	g_stab_ive_handle = 0;
	ret = g_stab_ive_create(g_stab_ive_handle);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: MI_IVE_Create failed %d\n",
			(int)ret);
		dlclose(g_stab_ive_lib);
		g_stab_ive_lib = NULL;
		return ret;
	}
	g_stab_ive_created = 1;

	g_stab_running = 1;
	if (pthread_create(&g_stab_thread, NULL,
	    star6e_stab_thread_main, NULL) != 0) {
		g_stab_running = 0;
		g_stab_ive_destroy(g_stab_ive_handle);
		g_stab_ive_created = 0;
		dlclose(g_stab_ive_lib);
		g_stab_ive_lib = NULL;
		fprintf(stderr, "[waybeam] ERROR: stab thread spawn failed\n");
		return -1;
	}

	fprintf(stderr, "[waybeam] stab: src=%ux%u out=%ux%u crop=%u%% "
		"recenter=%u (0=stick)\n", g_stab_src_w, g_stab_src_h,
		g_stab_enc_w, g_stab_enc_h, g_stab_crop_percent,
		g_stab_recenter_period);
	return 0;
}

static void star6e_stab_stop(void)
{
	if (g_stab_running) {
		/* Stop the detector thread and JOIN it before touching port1.
		 * In HW detect mode the thread keeps one port1 buffer checked out
		 * across iterations (prev_handle, the IVE reference frame); on loop
		 * exit it returns that buffer while the port is still enabled (safe)
		 * and can start no further IVE read.  Disabling port1 with the
		 * thread still alive — even "parked" — could free the ring under an
		 * in-flight IVE read: _MI_SYS_MMU_Callback Status=0x2 ClientId=0x15
		 * IsWrite=0 stormed the MMU into a hardware-watchdog reset.  The old
		 * 100ms park-spin was not a real barrier under the every-frame
		 * "high" detector load (it survived a few respawn cycles, then lost
		 * the race and reset the board).  pthread_join IS the barrier; only
		 * then disable the tap.  port0 stays bound feeding VENC throughout,
		 * so the VPE channel never backs up and there is no heavy manual
		 * drain to wedge [vpe0_P0_MAIN]. */
		g_stab_running = 0;
		pthread_join(g_stab_thread, NULL);
		memset(&g_stab_thread, 0, sizeof(g_stab_thread));
		if (g_stab_hw_mode)
			MI_VPE_DisablePort(0, 1);
		g_stab_pause = 0;
		g_stab_parked = 0;
	}
	if (g_stab_ive_created) {
		g_stab_ive_destroy(g_stab_ive_handle);
		g_stab_ive_created = 0;
	}
	if (g_stab_ive_lib) {
		dlclose(g_stab_ive_lib);
		g_stab_ive_lib = NULL;
	}
	g_stab_hw_mode = 0;
}

static int star6e_stab_enabled(const VencConfig *vcfg)
{
	return vcfg && vcfg->video0.stab_crop_pct >= 50 &&
		vcfg->video0.stab_crop_pct <= 100;
}

/* Compute the effective output dim for digital zoom.
 *
 * Approach C: zoom_pct shrinks BOTH crop and encoded output.  SCL output
 * dim == crop dim → 1:1 read/write, no upscale, no SCL bandwidth pressure.
 * The receiver sees the smaller frame in SPS/PPS and renders accordingly.
 *
 * Alignment 16 matches VENC and SCL.  Floor at 256 keeps the smallest
 * crop sane.  pct outside (0, 1) returns image dim unchanged. */
static void star6e_compute_zoom_dim(uint32_t image_w, uint32_t image_h,
	double pct, uint32_t *out_w, uint32_t *out_h)
{
	const uint32_t ALIGN = 16;
	const uint32_t MIN_DIM = 256;
	double dw, dh;
	uint32_t w, h;

	if (!isfinite(pct) || pct <= 0.0 || pct >= 1.0) {
		if (out_w) *out_w = image_w;
		if (out_h) *out_h = image_h;
		return;
	}

	/* Promote pct so neither dim drops under MIN_DIM — keeps the zoom
	 * AR-preserving instead of squishing the shorter axis when the
	 * floor kicks in. */
	if ((double)image_w * pct < (double)MIN_DIM)
		pct = (double)MIN_DIM / (double)image_w;
	if ((double)image_h * pct < (double)MIN_DIM)
		pct = (double)MIN_DIM / (double)image_h;
	if (pct > 1.0) pct = 1.0;

	dw = (double)image_w * pct;
	dh = (double)image_h * pct;
	w = (uint32_t)(dw + 0.5);
	h = (uint32_t)(dh + 0.5);
	w &= ~(ALIGN - 1);
	h &= ~(ALIGN - 1);
	if (w < MIN_DIM) w = MIN_DIM;
	if (h < MIN_DIM) h = MIN_DIM;
	if (w > image_w) w = image_w & ~(ALIGN - 1);
	if (h > image_h) h = image_h & ~(ALIGN - 1);
	if (out_w) *out_w = w;
	if (out_h) *out_h = h;
}

/* Build a VPE port-crop rect that places a (rect_w × rect_h) 1:1 window
 * inside (vpe_w × vpe_h) input, centred at fractional (x, y).  No scaling:
 * the VPE port output is set to the same (rect_w × rect_h) by SetPortMode,
 * so SCL reads the rect verbatim and emits it unchanged.  X/Y are 8-px
 * aligned (VPE requirement); rect_w/h are 16-px-aligned by the caller via
 * compute_zoom_dim. */
static i6_common_rect star6e_compute_zoom_rect(uint32_t vpe_w, uint32_t vpe_h,
	uint32_t rect_w, uint32_t rect_h, double x, double y)
{
	const uint32_t XY_ALIGN = 8;
	i6_common_rect r;
	double cx, cy;
	uint32_t rx, ry;

	if (!isfinite(x)) x = 0.5;
	if (!isfinite(y)) y = 0.5;
	if (x < 0.0)
		x = 0.0;
	if (x > 1.0)
		x = 1.0;
	if (y < 0.0)
		y = 0.0;
	if (y > 1.0)
		y = 1.0;
	if (rect_w > vpe_w) rect_w = vpe_w & ~(XY_ALIGN - 1);
	if (rect_h > vpe_h) rect_h = vpe_h & ~(XY_ALIGN - 1);

	cx = (double)vpe_w * x - (double)rect_w * 0.5;
	cy = (double)vpe_h * y - (double)rect_h * 0.5;
	if (cx < 0.0) cx = 0.0;
	if (cy < 0.0) cy = 0.0;
	if (cx + (double)rect_w > (double)vpe_w) cx = (double)(vpe_w - rect_w);
	if (cy + (double)rect_h > (double)vpe_h) cy = (double)(vpe_h - rect_h);

	rx = (uint32_t)(cx + 0.5) & ~(XY_ALIGN - 1);
	ry = (uint32_t)(cy + 0.5) & ~(XY_ALIGN - 1);
	if (rx + rect_w > vpe_w) rx = (vpe_w - rect_w) & ~(XY_ALIGN - 1);
	if (ry + rect_h > vpe_h) ry = (vpe_h - rect_h) & ~(XY_ALIGN - 1);

	r.x = (unsigned short)rx;
	r.y = (unsigned short)ry;
	r.width = (unsigned short)rect_w;
	r.height = (unsigned short)rect_h;
	return r;
}

static Star6eZoomStatus g_zoom_status;
static pthread_mutex_t g_zoom_status_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t star6e_zoom_level_x100(double pct)
{
	if (!isfinite(pct) || pct <= 0.0)
		return 0;
	return (uint32_t)(100.0 / pct + 0.5);
}

static void star6e_pipeline_set_zoom_status(double pct,
	uint32_t output_w, uint32_t output_h, const Star6ePrecropRect *base,
	const i6_common_rect *rect)
{
	Star6eZoomStatus snap;

	memset(&snap, 0, sizeof(snap));
	if (base && rect) {
		snap.active = 1;
		snap.level_x100 = star6e_zoom_level_x100(pct);
		snap.output_w = output_w;
		snap.output_h = output_h;
		snap.crop_x = (uint32_t)base->x + rect->x;
		snap.crop_y = (uint32_t)base->y + rect->y;
		snap.crop_w = rect->width;
		snap.crop_h = rect->height;
	}

	pthread_mutex_lock(&g_zoom_status_mutex);
	g_zoom_status = snap;
	pthread_mutex_unlock(&g_zoom_status_mutex);
}

static void star6e_pipeline_clear_zoom_status(void)
{
	pthread_mutex_lock(&g_zoom_status_mutex);
	memset(&g_zoom_status, 0, sizeof(g_zoom_status));
	pthread_mutex_unlock(&g_zoom_status_mutex);
}

void star6e_pipeline_zoom_status(Star6eZoomStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_zoom_status_mutex);
	*out = g_zoom_status;
	pthread_mutex_unlock(&g_zoom_status_mutex);
}

/* ----------------------------------------------------------------------- */
/*  Pan ramp: smooth live x/y panning via exponential decay.               */
/*                                                                          */
/*  apply_zoom records the target x/y; a dedicated tick thread runs at      */
/*  ~60Hz and steps `current` toward `target` each tick.  The crop rect     */
/*  the SDK sees is computed from `current`, not from the user input.      */
/*                                                                          */
/*  Per-step: current += (target - current) * alpha, where                 */
/*  alpha = 1 - exp(-tick_ms / PAN_RAMP_DEFAULT_MS).  Hardcoded — see       */
/*  PAN_RAMP_DEFAULT_MS rationale below.                                   */
/* ----------------------------------------------------------------------- */

#define PAN_RAMP_TICK_MS         16   /* ~60 Hz */
#define PAN_RAMP_SNAP_EPSILON    0.0005  /* fraction of frame */
/* Pan smoothing time constant.  150 ms ≈ 95 % settle in ~450 ms — fast
 * enough to feel responsive on a joystick, slow enough to mask the
 * step-quantization of an HTTP-driven SET stream.  Was a live config
 * field briefly; reverted to a hardcoded constant because the
 * additional knob added schema/UI/API surface without a use case
 * stronger than "user might want a different feel". */
#define PAN_RAMP_DEFAULT_MS      150

typedef struct {
	pthread_t        thread;
	pthread_mutex_t  lock;
	pthread_cond_t   cv;

	int       running;        /* 1 while pipeline is live */
	int       has_state;      /* 1 once pipeline_start populated `state` */

	double    target_pct;
	double    target_x, target_y;
	double    current_x, current_y;
	uint32_t  ramp_ms;        /* 0 = snap, else exponential decay τ */

	/* Borrowed; lifetime owned by pipeline_start/stop.  Always NULL when
	 * running==0. */
	Star6ePipelineState *state;
} Star6ePanRampState;

static Star6ePanRampState g_pan_ramp = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cv   = PTHREAD_COND_INITIALIZER,
	.ramp_ms = PAN_RAMP_DEFAULT_MS,
};

/* MI_ISP_CUS3A_SetAECropSize confines the AE meter to a sub-rect of the
 * ISP frame in 0..1023 normalized coords.  Resolved lazily; if absent or
 * libmi_isp didn't dlopen, every call is a silent no-op so the pipeline
 * still runs (master behaviour before AE-aware zoom was added). */
typedef struct {
	uint16_t crop_x;  /* 0..1023 */
	uint16_t crop_y;
	uint16_t crop_w;
	uint16_t crop_h;
} Star6eAeCropRect;

typedef int (*star6e_set_ae_crop_fn_t)(uint32_t channel,
	Star6eAeCropRect *data);

static star6e_set_ae_crop_fn_t g_star6e_set_ae_crop;
static int g_star6e_ae_crop_resolved;
static int g_star6e_ae_crop_ready;     /* 1 once CUS3A/ISP can accept calls */
static int g_star6e_ae_crop_disabled;  /* 1 if the SDK rejected a call — give up */
static Star6eAeCropRect g_star6e_ae_crop_last = { 0, 0, 1023, 1023 };

static void star6e_apply_ae_crop(double pct, double x, double y);

/* Compute and program the VPE port crop for (pct, x, y).  Caller must
 * already hold a valid pipeline state and image_width/height.  Side-
 * effect: updates the zoom_status snapshot. */
static int star6e_pan_apply_locked(Star6ePipelineState *state,
	double pct, double x, double y)
{
	i6_common_rect rect;
	MI_S32 ret;
	uint32_t in_w = state->active_precrop.w;
	uint32_t in_h = state->active_precrop.h;

	if (in_w == 0 || in_h == 0)
		return -1;

	rect = star6e_compute_zoom_rect(in_w, in_h,
		state->image_width, state->image_height, x, y);
	ret = MI_VPE_SetPortCrop(0, 0, &rect);
	if (ret != 0) {
		fprintf(stderr,
			"WARNING: MI_VPE_SetPortCrop(0,0) pan x=%.3f y=%.3f failed %d\n",
			x, y, (int)ret);
		return -1;
	}
	star6e_pipeline_set_zoom_status(pct, state->image_width,
		state->image_height, &state->active_precrop, &rect);
	return 0;
}

static void *star6e_pan_ramp_thread(void *arg)
{
	(void)arg;
	for (;;) {
		struct timespec ts;
		Star6ePipelineState *st;
		double tx, ty, pct, cx, cy, alpha;
		uint32_t ramp_ms;
		int has_state;

		pthread_mutex_lock(&g_pan_ramp.lock);
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += (long)PAN_RAMP_TICK_MS * 1000000L;
		while (ts.tv_nsec >= 1000000000L) {
			ts.tv_nsec -= 1000000000L;
			ts.tv_sec  += 1;
		}
		/* Idle when current already at target — wake on signal only. */
		while (g_pan_ramp.running &&
		    fabs(g_pan_ramp.target_x - g_pan_ramp.current_x) < PAN_RAMP_SNAP_EPSILON &&
		    fabs(g_pan_ramp.target_y - g_pan_ramp.current_y) < PAN_RAMP_SNAP_EPSILON) {
			int rc = pthread_cond_wait(&g_pan_ramp.cv, &g_pan_ramp.lock);
			if (rc != 0)
				break;
		}
		if (!g_pan_ramp.running) {
			pthread_mutex_unlock(&g_pan_ramp.lock);
			break;
		}
		/* Now we have a delta to step; pace the next iteration. */
		(void)pthread_cond_timedwait(&g_pan_ramp.cv, &g_pan_ramp.lock, &ts);
		if (!g_pan_ramp.running) {
			pthread_mutex_unlock(&g_pan_ramp.lock);
			break;
		}

		st        = g_pan_ramp.state;
		has_state = g_pan_ramp.has_state;
		ramp_ms   = g_pan_ramp.ramp_ms;
		tx        = g_pan_ramp.target_x;
		ty        = g_pan_ramp.target_y;
		pct       = g_pan_ramp.target_pct;

		if (!has_state || !st || pct <= 0.0 || pct >= 1.0) {
			g_pan_ramp.current_x = tx;
			g_pan_ramp.current_y = ty;
			pthread_mutex_unlock(&g_pan_ramp.lock);
			continue;
		}

		if (ramp_ms == 0) {
			alpha = 1.0;
		} else {
			alpha = 1.0 - exp(-(double)PAN_RAMP_TICK_MS / (double)ramp_ms);
			if (alpha > 1.0) alpha = 1.0;
			if (alpha < 0.0) alpha = 0.0;
		}

		g_pan_ramp.current_x += (tx - g_pan_ramp.current_x) * alpha;
		g_pan_ramp.current_y += (ty - g_pan_ramp.current_y) * alpha;
		if (fabs(tx - g_pan_ramp.current_x) < PAN_RAMP_SNAP_EPSILON)
			g_pan_ramp.current_x = tx;
		if (fabs(ty - g_pan_ramp.current_y) < PAN_RAMP_SNAP_EPSILON)
			g_pan_ramp.current_y = ty;

		cx = g_pan_ramp.current_x;
		cy = g_pan_ramp.current_y;
		pthread_mutex_unlock(&g_pan_ramp.lock);

		(void)star6e_pan_apply_locked(st, pct, cx, cy);
	}
	return NULL;
}

static int star6e_pan_ramp_start(Star6ePipelineState *state,
	double pct, double x, double y)
{
	int rc;

	pthread_mutex_lock(&g_pan_ramp.lock);
	if (g_pan_ramp.running) {
		/* Re-arm with the new state without recreating the thread. */
		g_pan_ramp.state        = state;
		g_pan_ramp.has_state    = 1;
		g_pan_ramp.target_pct   = pct;
		g_pan_ramp.target_x     = x;
		g_pan_ramp.target_y     = y;
		g_pan_ramp.current_x    = x;
		g_pan_ramp.current_y    = y;
		g_pan_ramp.ramp_ms      = PAN_RAMP_DEFAULT_MS;
		pthread_cond_signal(&g_pan_ramp.cv);
		pthread_mutex_unlock(&g_pan_ramp.lock);
		return 0;
	}
	g_pan_ramp.state        = state;
	g_pan_ramp.has_state    = 1;
	g_pan_ramp.target_pct   = pct;
	g_pan_ramp.target_x     = x;
	g_pan_ramp.target_y     = y;
	g_pan_ramp.current_x    = x;
	g_pan_ramp.current_y    = y;
	g_pan_ramp.ramp_ms      = PAN_RAMP_DEFAULT_MS;
	g_pan_ramp.running      = 1;
	pthread_mutex_unlock(&g_pan_ramp.lock);

	rc = pthread_create(&g_pan_ramp.thread, NULL,
		star6e_pan_ramp_thread, NULL);
	if (rc != 0) {
		fprintf(stderr,
			"WARNING: pthread_create(pan_ramp) failed %d (%s)\n",
			rc, strerror(rc));
		pthread_mutex_lock(&g_pan_ramp.lock);
		g_pan_ramp.running   = 0;
		g_pan_ramp.has_state = 0;
		g_pan_ramp.state     = NULL;
		pthread_mutex_unlock(&g_pan_ramp.lock);
		return -1;
	}

	/* Program the initial crop rect synchronously.  The ramp thread would
	 * otherwise idle because current==target on first start, leaving the
	 * VPE port at full-frame.  AE crop fires from the ISP-ready hook
	 * (star6e_ae_crop_mark_ready) — too early to call here. */
	if (pct > 0.0 && pct < 1.0)
		(void)star6e_pan_apply_locked(state, pct, x, y);
	return 0;
}

static void star6e_pan_ramp_stop(void)
{
	pthread_t th;
	int was_running;

	pthread_mutex_lock(&g_pan_ramp.lock);
	was_running          = g_pan_ramp.running;
	g_pan_ramp.running   = 0;
	g_pan_ramp.has_state = 0;
	g_pan_ramp.state     = NULL;
	th                   = g_pan_ramp.thread;
	pthread_cond_broadcast(&g_pan_ramp.cv);
	pthread_mutex_unlock(&g_pan_ramp.lock);

	if (was_running) {
		pthread_join(th, NULL);
	} else {
	}
}

/* Live pan: zoom_pct is MUT_RESTART (changing crop dim resizes VPE port and
 * VENC, which needs reinit), so the live path only handles x/y.  pct is
 * accepted just to short-circuit when zoom is off (rect dim == image dim).
 * Updates the *target*; the ramp thread tweens `current` toward it. */
int star6e_pipeline_apply_zoom(Star6ePipelineState *state,
	double pct, double x, double y)
{
	if (!state) return -1;
	if (!isfinite(pct) || !isfinite(x) || !isfinite(y))
		return -1;
	/* When stabilization is active the crop window is enforced CENTERED
	 * (0.5/0.5) so the accumulator has symmetric headroom — zoomX/zoomY are
	 * the zoom-mode pan and must not steer the stab window off-center (that
	 * would pin it against the frame edge and kill stabilization).  Ignore
	 * live x/y here; keep the window centered. */
	if (g_stab_running) {
		star6e_stab_set_pan(0.5, 0.5);
		return 0;
	}
	if (pct <= 0.0 || pct >= 1.0) {
		star6e_pipeline_clear_zoom_status();
		pthread_mutex_lock(&g_pan_ramp.lock);
		g_pan_ramp.target_pct = 0.0;
		g_pan_ramp.target_x   = x;
		g_pan_ramp.target_y   = y;
		g_pan_ramp.current_x  = x;
		g_pan_ramp.current_y  = y;
		pthread_cond_signal(&g_pan_ramp.cv);
		pthread_mutex_unlock(&g_pan_ramp.lock);
		star6e_apply_ae_crop(pct, x, y);
		return 0;  /* zoom off — nothing to pan */
	}

	if (state->active_precrop.w == 0 || state->active_precrop.h == 0)
		return -1;  /* VPE not started yet */

	pthread_mutex_lock(&g_pan_ramp.lock);
	g_pan_ramp.state      = state;
	g_pan_ramp.has_state  = 1;
	g_pan_ramp.target_pct = pct;
	g_pan_ramp.target_x   = x;
	g_pan_ramp.target_y   = y;
	if (g_pan_ramp.ramp_ms == 0) {
		g_pan_ramp.current_x = x;
		g_pan_ramp.current_y = y;
	}
	pthread_cond_signal(&g_pan_ramp.cv);
	pthread_mutex_unlock(&g_pan_ramp.lock);

	star6e_apply_ae_crop(pct, x, y);
	return 0;
}

/* Emit a normalized (0..1023) AE-meter crop rect to the ISP.  Shared by the
 * zoom and stabilization tracking paths.  Coords are normalized against the
 * ISP frame (the post-VIF input — same domain as `active_precrop`).
 *
 * Failure modes the bench taught us:
 *   - The SDK refuses calls before CUS3A handoff; we gate on
 *     g_star6e_ae_crop_ready and just queue the change in the cache.
 *   - Calling with full-frame (0,0,1023,1023) on this BSP returns -1
 *     during init.  Whether it works at all is uncertain, so callers never
 *     build a full-frame rect — the cache is pre-seeded full-frame and only
 *     sub-rects ever get pushed.  When the crop is removed live, the meter
 *     stays on the last rect; the ISP's own filtering re-equilibrates
 *     exposure over the next few seconds.
 *   - On any non-zero return from the SDK, we set
 *     g_star6e_ae_crop_disabled and stop calling for the rest of the
 *     process.  Next start cold-resets via pipeline_stop. */
static void star6e_emit_ae_crop(const Star6eAeCropRect *r)
{
	if (g_star6e_ae_crop_disabled)
		return;
	if (!g_star6e_ae_crop_resolved) {
		g_star6e_set_ae_crop = (star6e_set_ae_crop_fn_t)
			dlsym(RTLD_DEFAULT, "MI_ISP_CUS3A_SetAECropSize");
		g_star6e_ae_crop_resolved = 1;
		if (!g_star6e_set_ae_crop)
			fprintf(stderr,
				"WARNING: MI_ISP_CUS3A_SetAECropSize not present — "
				"AE will not track zoom/stab\n");
	}
	if (!g_star6e_set_ae_crop)
		return;

	if (r->crop_x == g_star6e_ae_crop_last.crop_x &&
	    r->crop_y == g_star6e_ae_crop_last.crop_y &&
	    r->crop_w == g_star6e_ae_crop_last.crop_w &&
	    r->crop_h == g_star6e_ae_crop_last.crop_h)
		return;

	g_star6e_ae_crop_last = *r;

	if (!g_star6e_ae_crop_ready)
		return;  /* queued — ISP not yet ready */

	if (g_star6e_set_ae_crop(0, (Star6eAeCropRect *)r) != 0) {
		fprintf(stderr,
			"WARNING: MI_ISP_CUS3A_SetAECropSize(%u,%u,%u,%u) failed — "
			"disabling AE crop-tracking for this run\n",
			r->crop_x, r->crop_y, r->crop_w, r->crop_h);
		g_star6e_ae_crop_disabled = 1;
	}
}

/* Zoom path: constrain the AE meter to the zoom rect.  The encoded dim
 * (state->image_width) is the crop size in precrop coords (Approach C SCL is
 * 1:1), so the meter fraction is image_width / active_precrop.w. */
static void star6e_apply_ae_crop(double pct, double x, double y)
{
	Star6eAeCropRect r;
	uint32_t in_w, in_h;
	uint32_t rect_w, rect_h;
	double cx, cy, rx, ry;
	Star6ePipelineState *state;

	/* Skip the "full frame" path entirely.  See star6e_emit_ae_crop. */
	if (!isfinite(pct) || pct <= 0.0 || pct >= 1.0)
		return;

	pthread_mutex_lock(&g_pan_ramp.lock);
	state = g_pan_ramp.has_state ? g_pan_ramp.state : NULL;
	pthread_mutex_unlock(&g_pan_ramp.lock);
	if (!state || state->active_precrop.w == 0 ||
	    state->active_precrop.h == 0)
		return;

	in_w   = state->active_precrop.w;
	in_h   = state->active_precrop.h;
	rect_w = state->image_width;
	rect_h = state->image_height;
	if (!isfinite(x)) x = 0.5;
	if (!isfinite(y)) y = 0.5;
	if (x < 0.0) x = 0.0;
	if (x > 1.0) x = 1.0;
	if (y < 0.0) y = 0.0;
	if (y > 1.0) y = 1.0;
	cx = (double)in_w * x - (double)rect_w * 0.5;
	cy = (double)in_h * y - (double)rect_h * 0.5;
	if (cx < 0.0) cx = 0.0;
	if (cy < 0.0) cy = 0.0;
	if (cx + (double)rect_w > (double)in_w)
		cx = (double)(in_w - rect_w);
	if (cy + (double)rect_h > (double)in_h)
		cy = (double)(in_h - rect_h);
	rx = cx * 1023.0 / (double)in_w;
	ry = cy * 1023.0 / (double)in_h;
	memset(&r, 0, sizeof(r));
	r.crop_x = (uint16_t)(rx + 0.5);
	r.crop_y = (uint16_t)(ry + 0.5);
	r.crop_w = (uint16_t)((double)rect_w * 1023.0 / (double)in_w + 0.5);
	r.crop_h = (uint16_t)((double)rect_h * 1023.0 / (double)in_h + 0.5);
	if (r.crop_w == 0) r.crop_w = 1;
	if (r.crop_h == 0) r.crop_h = 1;
	if (r.crop_x + r.crop_w > 1023)
		r.crop_x = (uint16_t)(1023 - r.crop_w);
	if (r.crop_y + r.crop_h > 1023)
		r.crop_y = (uint16_t)(1023 - r.crop_h);

	star6e_emit_ae_crop(&r);
}

/* Stabilization path: constrain the AE meter to the stabilized crop window.
 * The crop is taken from the VPE *output* (g_stab_src), not from precrop, so
 * the meter fraction is g_stab_enc / g_stab_src (= the crop %) regardless of
 * any sensor→stream downscale — unlike the zoom path's image_width/precrop.
 * Centre follows the live pan; the small per-frame stabilization offset is
 * intentionally not tracked (a few px; the meter window need not chase it). */
static void star6e_stab_apply_ae_crop(void)
{
	Star6eAeCropRect r;
	double fw, fh, x, y, rx, ry;

	if (g_stab_src_w == 0 || g_stab_src_h == 0)
		return;
	fw = (double)g_stab_enc_w / (double)g_stab_src_w;
	fh = (double)g_stab_enc_h / (double)g_stab_src_h;
	if (!isfinite(fw) || !isfinite(fh) ||
	    fw <= 0.0 || fw >= 1.0 || fh <= 0.0 || fh >= 1.0)
		return;  /* no crop → leave AE full-frame */

	x = (double)g_stab_pan_x_mil / 1000.0;
	y = (double)g_stab_pan_y_mil / 1000.0;
	rx = x - fw * 0.5;
	ry = y - fh * 0.5;
	if (rx < 0.0) rx = 0.0;
	if (ry < 0.0) ry = 0.0;
	if (rx + fw > 1.0) rx = 1.0 - fw;
	if (ry + fh > 1.0) ry = 1.0 - fh;

	memset(&r, 0, sizeof(r));
	r.crop_x = (uint16_t)(rx * 1023.0 + 0.5);
	r.crop_y = (uint16_t)(ry * 1023.0 + 0.5);
	r.crop_w = (uint16_t)(fw * 1023.0 + 0.5);
	r.crop_h = (uint16_t)(fh * 1023.0 + 0.5);
	if (r.crop_w == 0) r.crop_w = 1;
	if (r.crop_h == 0) r.crop_h = 1;
	if (r.crop_x + r.crop_w > 1023)
		r.crop_x = (uint16_t)(1023 - r.crop_w);
	if (r.crop_y + r.crop_h > 1023)
		r.crop_y = (uint16_t)(1023 - r.crop_h);

	star6e_emit_ae_crop(&r);
}

/* Called from the ISP-ready hook in pipeline_start.  If the user booted
 * with zoom active, this is the first chance the AE crop can actually
 * reach the SDK; flush the cached rect now. */
static void star6e_ae_crop_mark_ready(void)
{
	double pct, x, y;

	g_star6e_ae_crop_ready = 1;

	pthread_mutex_lock(&g_pan_ramp.lock);
	pct = g_pan_ramp.target_pct;
	x   = g_pan_ramp.target_x;
	y   = g_pan_ramp.target_y;
	pthread_mutex_unlock(&g_pan_ramp.lock);

	if (isfinite(pct) && pct > 0.0 && pct < 1.0) {
		/* Force re-emit: invalidate cache, then call. */
		g_star6e_ae_crop_last.crop_w = 0;
		star6e_apply_ae_crop(pct, x, y);
	}
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

static int star6e_pipeline_pre_start_apply_ref_pred(MI_VENC_CHN chn,
	const VencConfig *vcfg);

static int star6e_pipeline_start_venc(uint32_t width, uint32_t height,
	uint32_t bitrate, uint32_t framerate, uint32_t gop, PAYLOAD_TYPE_E codec,
	int rc_mode, bool frame_lost_enabled, const VencConfig *vcfg,
	MI_VENC_CHN *chn)
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

	/* SDK convention: SetRefParam must be called between CreateChn and
	 * StartRecvPic.  Star6E silently no-ops the call if invoked after
	 * StartRecvPic, producing a flat single-layer stream. */
	(void)star6e_pipeline_pre_start_apply_ref_pred(*chn, vcfg);

	ret = MI_VENC_StartRecvPic(*chn);
	if (ret != 0) {
		fprintf(stderr, "ERROR: MI_VENC_StartRecvPic failed %d\n", ret);
		MI_VENC_DestroyChn(*chn);
		return ret;
	}

	/* Frame lost strategy — see star6e_controls_apply_frame_lost_threshold. */
	if (star6e_controls_apply_frame_lost_threshold(*chn,
	    frame_lost_enabled, bitrate) != 0)
		fprintf(stderr, "[waybeam] WARNING: SetFrameLostStrategy"
			" failed\n");

	return 0;
}

static void star6e_pipeline_stop_venc(MI_VENC_CHN chn)
{
	MI_VENC_StopRecvPic(chn);
	MI_VENC_DestroyChn(chn);
}

static Star6eIntraRefreshStatus g_intra_status;
static pthread_mutex_t g_intra_status_mutex = PTHREAD_MUTEX_INITIALIZER;

void star6e_pipeline_intra_refresh_status(Star6eIntraRefreshStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_intra_status_mutex);
	*out = g_intra_status;
	pthread_mutex_unlock(&g_intra_status_mutex);
}

/* Compute IntraRefresh derived params from the current config.  Out param
 * is always populated; mode is also returned for caller convenience. */
static IntraRefreshMode star6e_pipeline_intra_refresh_derive(
	const VencConfig *vcfg, uint32_t height, uint32_t fps,
	PAYLOAD_TYPE_E codec, IntraRefreshDerived *out_ir)
{
	IntraRefreshMode mode = INTRA_MODE_OFF;

	memset(out_ir, 0, sizeof(*out_ir));
	if (vcfg) {
		mode = intra_refresh_parse_mode(vcfg->video0.intra_refresh_mode);
		(void)codec; /* H.265 only */
		intra_refresh_compute(mode, height, fps,
			vcfg->video0.intra_refresh_lines,
			vcfg->video0.intra_refresh_qp,
			vcfg->video0.gop_size, out_ir);
	}
	return mode;
}

static int star6e_pipeline_apply_intra_refresh(MI_VENC_CHN chn,
	const VencConfig *vcfg, uint32_t height, uint32_t fps,
	PAYLOAD_TYPE_E codec)
{
	MI_VENC_IntraRefresh_t cfg;
	Star6eIntraRefreshStatus snap;
	IntraRefreshDerived ir;
	IntraRefreshMode mode;
	const char *name;

	memset(&snap, 0, sizeof(snap));
	mode = star6e_pipeline_intra_refresh_derive(vcfg, height, fps, codec, &ir);
	name = intra_refresh_mode_name(mode);

	snprintf(snap.mode_name, sizeof(snap.mode_name), "%s", name);
	snap.mi_supported = g_mi_venc.fnSetIntraRefresh ? 1 : 0;
	if (vcfg) {
		snap.requested_lines  = vcfg->video0.intra_refresh_lines;
		snap.requested_qp     = vcfg->video0.intra_refresh_qp;
		snap.explicit_gop_sec = vcfg->video0.gop_size;
	}
	snap.target_ms             = ir.target_ms;
	snap.total_rows            = ir.total_rows;
	snap.effective_lines_per_p = ir.lines;
	snap.lines_clamped         = ir.lines_clamped;
	snap.effective_qp          = ir.req_iqp;
	snap.effective_gop_sec     = ir.gop_overridden ? snap.explicit_gop_sec : ir.gop_sec;
	snap.gop_auto              = ir.gop_overridden ? 0 : (ir.gop_sec > 0.0);

	if (mode == INTRA_MODE_OFF) {
		pthread_mutex_lock(&g_intra_status_mutex);
		g_intra_status = snap;
		pthread_mutex_unlock(&g_intra_status_mutex);
		return 0;
	}
	if (!g_mi_venc.fnSetIntraRefresh) {
		fprintf(stderr, "[waybeam] WARNING: intraRefreshMode=%s requested "
			"but libmi_venc.so does not export MI_VENC_SetIntraRefresh\n",
			name);
		pthread_mutex_lock(&g_intra_status_mutex);
		g_intra_status = snap;
		pthread_mutex_unlock(&g_intra_status_mutex);
		return -1;
	}
	if (ir.lines_clamped) {
		fprintf(stderr, "[waybeam] WARNING: intraRefreshLines exceeds picture "
			"LCU rows=%u, clamped\n", ir.total_rows);
	}
	if (ir.gop_overridden) {
		fprintf(stderr, "[waybeam] intra auto-GOP suppressed: explicit "
			"gopSize=%.2fs\n", snap.explicit_gop_sec);
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.bEnable = 1;
	cfg.u32RefreshLineNum = ir.lines;
	cfg.u32ReqIQp = ir.req_iqp;

	if (MI_VENC_SetIntraRefresh(chn, &cfg) != 0) {
		fprintf(stderr, "[waybeam] ERROR: MI_VENC_SetIntraRefresh(chn=%d, "
			"lines=%u, qp=%u) failed\n", chn,
			cfg.u32RefreshLineNum, cfg.u32ReqIQp);
		pthread_mutex_lock(&g_intra_status_mutex);
		g_intra_status = snap;
		pthread_mutex_unlock(&g_intra_status_mutex);
		return -1;
	}
	snap.apply_ok = 1;
	snap.active   = 1;
	pthread_mutex_lock(&g_intra_status_mutex);
	g_intra_status = snap;
	pthread_mutex_unlock(&g_intra_status_mutex);
	fprintf(stderr, "[waybeam] intraRefresh: mode=%s lines/P=%u qp=%u "
		"gop=%.2fs (%s)\n", name, cfg.u32RefreshLineNum, cfg.u32ReqIQp,
		snap.effective_gop_sec, snap.gop_auto ? "auto" : "explicit");
	return 0;
}

static Star6eRefPredStatus g_ref_pred_status;
static pthread_mutex_t g_ref_pred_status_mutex = PTHREAD_MUTEX_INITIALIZER;

void star6e_pipeline_ref_pred_status(Star6eRefPredStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_ref_pred_status_mutex);
	*out = g_ref_pred_status;
	pthread_mutex_unlock(&g_ref_pred_status_mutex);
}

/* SVC-T reference structure — opt-in via video0.refBase.  Disabled means
 * SDK default single-layer reference (one P references previous P).
 *
 * Star6E SDK requires SetRefParam to land between CreateChn and
 * StartRecvPic — calling it later silently no-ops (verified with the
 * test_ref_pred harness: bitstream identical at any post-Start value).
 * Therefore this helper is invoked from star6e_pipeline_start_venc()
 * immediately after CreateChn. */
static int star6e_pipeline_pre_start_apply_ref_pred(MI_VENC_CHN chn,
	const VencConfig *vcfg)
{
	MI_VENC_ParamRef_t ref;
	Star6eRefPredStatus snap;

	memset(&snap, 0, sizeof(snap));
	snap.mi_supported = g_mi_venc.fnSetRefParam ? 1 : 0;
	if (vcfg) {
		snap.base    = vcfg->video0.ref_base;
		snap.enhance = vcfg->video0.ref_enhance;
		snap.pred    = vcfg->video0.ref_pred ? 1 : 0;
	}

	if (!vcfg || vcfg->video0.ref_base == 0)
		goto publish;
	if (!g_mi_venc.fnSetRefParam) {
		fprintf(stderr, "[waybeam] WARNING: refBase=%u requested but "
			"libmi_venc.so does not export MI_VENC_SetRefParam\n",
			vcfg->video0.ref_base);
		goto publish;
	}

	memset(&ref, 0, sizeof(ref));
	ref.u32Base     = vcfg->video0.ref_base;
	ref.u32Enhance  = vcfg->video0.ref_enhance ? vcfg->video0.ref_enhance : 1;
	ref.bEnablePred = vcfg->video0.ref_pred ? 1 : 0;

	if (MI_VENC_SetRefParam(chn, &ref) != 0) {
		fprintf(stderr, "[waybeam] ERROR: MI_VENC_SetRefParam(chn=%d, "
			"base=%u, enhance=%u, pred=%u) failed\n", chn,
			ref.u32Base, ref.u32Enhance, ref.bEnablePred);
		goto publish;
	}
	snap.apply_ok = 1;
	snap.active   = 1;
	fprintf(stderr, "[waybeam] refPred: chn=%d base=%u enhance=%u pred=%u "
		"(applied pre-Start)\n", chn, ref.u32Base, ref.u32Enhance,
		ref.bEnablePred);
publish:
	pthread_mutex_lock(&g_ref_pred_status_mutex);
	g_ref_pred_status = snap;
	pthread_mutex_unlock(&g_ref_pred_status_mutex);
	return snap.active ? 0 : (vcfg && vcfg->video0.ref_base > 0 ? -1 : 0);
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
	char               isp_bin_path[256];   /* "" if no bin should be loaded;
	                                         * resolved by select_and_configure_sensor()
	                                         * after we know the live sensor name */
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

	if (codec_config_resolve_codec_rc(vcfg->video0.rc_mode,
	    &pconf->rc_codec, &pconf->rc_mode) != 0)
		return -1;

	state->output_enabled = vcfg->outgoing.enabled ? 1 : 0;
	if (!vcfg->outgoing.server[0] && vcfg->outgoing.enabled) {
		fprintf(stderr,
			"ERROR: outgoing.enabled=true but outgoing.server is empty\n");
		return -1;
	}
	if (star6e_output_prepare(&pconf->output_setup, vcfg->outgoing.server,
	    vcfg->outgoing.stream_mode,
	    vcfg->outgoing.connected_udp) != 0)
		return -1;

	/* Auto-cap exposure to frame period so the AE shutter never exceeds
	 * the frame period.  Without this, the AE converges on a long
	 * exposure that locks fps below the target. */
	pconf->exposure_cap_us = (pconf->sensor_framerate > 0) ?
		1000000 / pconf->sensor_framerate : 0;
	pconf->image_mirror    = vcfg->image.mirror ? 1 : 0;
	pconf->image_flip      = vcfg->image.flip   ? 1 : 0;
	pconf->vpe_level_3dnr  = vcfg->fpv.noise_level;
	pconf->oc_level        = vcfg->system.overclock_level;
	/* isp_bin_path is resolved later in bind_and_finalize_pipeline() once
	 * the live sensor name is known.  Resolved there (rather than in
	 * select_and_configure_sensor) so that reinit — which reuses the
	 * existing sensor and skips select_and_configure_sensor — also picks
	 * up SIGHUP-driven `isp.sensorBin` changes.  Leave empty here. */
	pconf->isp_bin_path[0] = '\0';

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
	/* Auto resolution: 0x0 means use sensor native dimensions */
	if (pconf->image_width == 0 || pconf->image_height == 0) {
		pconf->image_width = sensor_width;
		pconf->image_height = sensor_height;
	}
	pipeline_common_clamp_image_size("", sensor_width, sensor_height,
		&pconf->image_width, &pconf->image_height);

	/* Precrop is computed BEFORE the zoom override so the VIF→VPE window
	 * keeps the user-configured aspect ratio against the full sensor.
	 * Zoom is applied as a 1:1 sub-rect of the VPE output (not by shrinking
	 * the sensor read area). */
	pconf->precrop = star6e_pipeline_compute_precrop(sensor_width,
		sensor_height, pconf->image_width, pconf->image_height,
		vcfg->isp.keep_aspect);

	/* Approach C zoom: when zoom_pct ∈ (0, 1), shrink image_width/height
	 * to the crop dim.  VPE port output, SetPortCrop rect, and VENC channel
	 * all run at this smaller dim — no SCL upscale, no bandwidth pressure.
	 * The receiver sees the smaller resolution in SPS/PPS.  zoom_pct is
	 * MUT_RESTART so this only runs at start; live x/y pan stays in
	 * apply_zoom which only moves the rect inside VPE input. */
	if (vcfg->video0.zoom_pct > 0.0 && vcfg->video0.zoom_pct < 1.0) {
		uint32_t zw = pconf->image_width;
		uint32_t zh = pconf->image_height;
		star6e_compute_zoom_dim(pconf->image_width, pconf->image_height,
			vcfg->video0.zoom_pct, &zw, &zh);
		if (vcfg->system.verbose)
			printf("> Zoom: pct=%.3f → %ux%u (from image %ux%u)\n",
				vcfg->video0.zoom_pct, zw, zh,
				pconf->image_width, pconf->image_height);
		pconf->image_width  = zw;
		pconf->image_height = zh;
	}

	state->image_width  = pconf->image_width;
	state->image_height = pconf->image_height;
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

/* IMU push callback: route frame-synced 6-axis samples into the shared
 * gyro ring (g_stab_imu_ring).  The ring is the single home for IMU data —
 * the stabilization motion estimator reads it (see
 * star6e_stab_estimate_shift), and telemetry/sidecar consumers can read the
 * same ring.  Cheap (mutex + copy); a no-op until the ring is initialized in
 * the IMU bring-up block. */
static void star6e_pipeline_imu_push(void *ctx, const ImuSample *sample)
{
	(void)ctx;
	if (!g_stab_imu_ring_ready || !sample)
		return;
	ImuRingSample rs = {
		.ts      = sample->ts,
		.gyro_x  = sample->gyro_x,
		.gyro_y  = sample->gyro_y,
		.gyro_z  = sample->gyro_z,
		.accel_x = sample->accel_x,
		.accel_y = sample->accel_y,
		.accel_z = sample->accel_z,
	};
	imu_ring_push(&g_stab_imu_ring, &rs);
}

/* Tracks whether CUS3A has been enabled in this MI_SYS lifetime.  Cleared
 * by star6e_pipeline_stop(), which is always followed by MI_SYS_Exit in
 * runner_teardown — so the next process start runs a true cold sequence
 * including CUS3A enable. */
static int g_isp_initialized = 0;

/* Tracks the last-loaded ISP bin path within this MI_SYS lifetime so we
 * skip redundant reloads.  Cleared by star6e_pipeline_stop() since the
 * following MI_SYS_Exit releases ISP driver state and the next start
 * needs to reload the bin against the fresh kernel. */
static char g_last_isp_bin_path[256] = {0};

int star6e_pipeline_load_isp_bin_live(const char *configured_path,
	const VencConfig *vcfg, const char *sensor_name,
	MI_SNR_PAD_ID_e pad_id, uint32_t sensor_framerate)
{
	char resolved[256];
	const char *configured;
	int ret;

	if (!vcfg)
		return -1;

	configured = (configured_path && *configured_path) ? configured_path : NULL;
	if (!pipeline_common_resolve_isp_bin(configured, sensor_name,
	    resolved, sizeof(resolved))) {
		fprintf(stderr,
			"ERROR: ISP bin reload — no readable bin for '%s' "
			"(sensor '%s')\n",
			configured ? configured : "(unset)",
			sensor_name ? sensor_name : "(unknown)");
		return -1;
	}

	if (strcmp(resolved, g_last_isp_bin_path) == 0) {
		printf("> ISP bin reload: %s already loaded, skipping\n",
			resolved);
		return 0;
	}

	/* sdk_quiet is only used to silence noisy SDK chatter during boot.
	 * On a live reload we accept the extra logging — passing NULL keeps
	 * the wrapper free of cross-module quiet-state plumbing. */
	ret = star6e_pipeline_load_isp_bin(resolved, NULL);
	if (ret != 0)
		return ret;

	snprintf(g_last_isp_bin_path, sizeof(g_last_isp_bin_path), "%s",
		resolved);

	/* Bin can reset MI_ISP_AE_SetExposureLimit to its own defaults; reapply
	 * the FPS cap so AE doesn't relock to a shutter longer than the frame
	 * period.  Same logic as bind_and_finalize_pipeline. */
	if (vcfg->video0.fps > 0)
		star6e_pipeline_cap_exposure_for_fps(vcfg->video0.fps);

	/* Legacy-AE writes the bin's cold-boot shutter (often ~100 ms) directly
	 * to the sensor register, which the SetExposureLimit cap above doesn't
	 * touch.  Kick MI_SNR_SetFps to force the sensor driver to reconfigure
	 * timing — without this, swapping to a darker bin can lock the sensor
	 * at ~12 fps until reinit (cold_boot_fps_lock memory).  CUS3A mode
	 * handles this via its periodic fps_kick logic, so skip the kick when
	 * legacy_ae is off. */
	if (vcfg->isp.legacy_ae && sensor_framerate > 0)
		MI_SNR_SetFps(pad_id, sensor_framerate);

	return 0;
}

/* Phase 3: assign port structs, issue all MI_SYS bind calls, init output,
 * video, ISP bin, exposure cap, cus3a, clocks, and audio.
 * pconf is non-const because we resolve isp_bin_path in here (we need
 * the live sensor name from state->sensor, which is only populated
 * after Phase 2). */
static int bind_and_finalize_pipeline(Star6ePipelineState *state,
	const VencConfig *vcfg, Star6ePipelineConfig *pconf,
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
	star6e_pipeline_cap_exposure_for_fps(pconf->sensor_framerate);

	if (star6e_stab_enabled(vcfg)) {
		/* Stabilization path.  Preferred: port0 hardware-crops the stab
		 * window straight to a VENC bind (zero-copy) while a tiny port1
		 * tap feeds the detector — no per-frame blit, robust teardown.
		 * Falls back to the legacy full-src manual drain if this BSP can't
		 * run port1.  star6e_stab_setup_ports picks the mode and (in HW
		 * mode) sets state->bound_vpe_venc=1 for the standard teardown. */
		bind_src_fps = state->sensor.mode.maxFps ?
			state->sensor.mode.maxFps : pconf->sensor_framerate;
		bind_dst_fps = vcfg->video0.fps;
		if (bind_dst_fps == 0 || bind_dst_fps > bind_src_fps)
			bind_dst_fps = bind_src_fps;

		ret = star6e_stab_setup_ports(state, bind_src_fps, bind_dst_fps);
		if (ret != 0) {
			MI_SYS_UnBindChnPort(&state->vif_port, &state->vpe_port);
			state->bound_vif_vpe = 0;
			return ret;
		}
		ret = star6e_stab_start();
		if (ret != 0) {
			if (g_stab_hw_mode && state->bound_vpe_venc) {
				MI_SYS_UnBindChnPort(&state->vpe_port,
					&state->venc_port);
				state->bound_vpe_venc = 0;
				MI_VPE_DisablePort(0, 1);
			}
			MI_SYS_UnBindChnPort(&state->vif_port, &state->vpe_port);
			state->bound_vif_vpe = 0;
			return ret;
		}
		/* Seed the AE meter to the stabilized crop window.  The ISP-ready
		 * wait above has already fired star6e_ae_crop_mark_ready(), so
		 * this emits immediately rather than queueing. */
		star6e_stab_apply_ae_crop();
	} else {
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
	}

	/* Bring up the JPEG snapshot subsystem on the same VPE source port the
	 * main channel just bound to.  Failure is non-fatal — /api/v1/snapshot.jpg
	 * just serves 503 if init fails.  Config from venc.json snapshot.*
	 * section; width=0/height=0 inherits main stream dimensions. */
	{
		venc_jpeg_set_source(&state->vpe_port);
		const VencConfigSnapshot *snap = &vcfg->snapshot;
		VencJpegConfig jcfg = {
			.width   = snap->width  ? snap->width  : state->image_width,
			.height  = snap->height ? snap->height : state->image_height,
			.quality = snap->quality,
			.channel = snap->channel,
			.enabled = snap->enabled,
		};
		(void)venc_jpeg_init(&jcfg);
	}

	if (star6e_output_init(&state->output, &pconf->output_setup) != 0) {
		star6e_output_teardown(&state->output);
		MI_SYS_UnBindChnPort(&state->vpe_port, &state->venc_port);
		state->bound_vpe_venc = 0;
		MI_SYS_UnBindChnPort(&state->vif_port, &state->vpe_port);
		state->bound_vif_vpe = 0;
		return -1;
	}

	star6e_video_init(&state->video, vcfg, pconf->sensor_framerate,
		&state->output);

	/* Resolve isp.sensorBin: configured path takes precedence; if empty
	 * or unreadable, fall back to /etc/sensors/<sensor>.bin keyed off
	 * the live sensor name.  Resolved here (rather than in
	 * select_and_configure_sensor) so reinit also picks up SIGHUP-driven
	 * isp.sensorBin changes — reinit reuses the existing sensor and
	 * skips select_and_configure_sensor. */
	pipeline_common_resolve_isp_bin(
		vcfg->isp.sensor_bin[0] ? vcfg->isp.sensor_bin : NULL,
		state->sensor.plane.sensName,
		pconf->isp_bin_path, sizeof(pconf->isp_bin_path));

	/* Load ISP bin on first start, or when the bin path changes.  Skipping
	 * redundant reloads avoids the vendor AE resetting the sensor shutter
	 * register back to its default (~10000us on IMX335) on every SIGHUP /
	 * Save&Restart, which would otherwise lock the sensor VTS at ~100 fps.
	 * The kernel ISP driver accepts repeated loads but each one disturbs
	 * the running sensor timing. */
	if (pconf->isp_bin_path[0] &&
	    strcmp(pconf->isp_bin_path, g_last_isp_bin_path) != 0) {
		ret = star6e_pipeline_load_isp_bin(pconf->isp_bin_path, sdk_quiet);
		if (ret != 0) {
			fprintf(stderr, "WARNING: ISP bin load failed; continuing with default ISP settings\n");
		} else {
			snprintf(g_last_isp_bin_path, sizeof(g_last_isp_bin_path),
				"%s", pconf->isp_bin_path);
		}
	}
	if (!g_isp_initialized) {
		star6e_pipeline_enable_cus3a(sdk_quiet);
		g_isp_initialized = 1;
	}
	/* Reapply exposure cap after ISP bin load — the bin may reset AE
	 * limits to its own defaults which could exceed the frame period. */
	star6e_pipeline_cap_exposure_for_fps(pconf->sensor_framerate);

	/* Cold-boot fix: with legacyAe the ISP bin's AE may initialize the
	 * sensor at a shutter exceeding the frame period.  SetExposureLimit
	 * only constrains the AE algorithm, not the physical sensor register.
	 * MI_SNR_SetFps forces the sensor driver to reconfigure timing,
	 * resetting the shutter to fit the frame period.  When CUS3A is
	 * active it handles this via the fps_kick logic; when legacyAe is
	 * active we must do it here. */
	if (vcfg->isp.legacy_ae && pconf->exposure_cap_us > 0 &&
	    pconf->sensor_framerate > 0) {
		MI_SNR_SetFps(state->sensor.pad_id, pconf->sensor_framerate);
	}

	star6e_pipeline_set_hw_clocks(pconf->oc_level, vcfg->system.verbose);

	if (star6e_output_is_shm(&state->output) &&
	    vcfg->outgoing.audio_port == 0) {
		printf("[audio] Disabled in SHM mode (audioPort=0 has no socket to share)\n");
	} else {
		star6e_audio_init(&state->audio, vcfg, &state->output);
	}

	/* IMU */
	if (vcfg->imu.enabled) {
		if (!g_stab_imu_ring_ready) {
			imu_ring_init(&g_stab_imu_ring);
			g_stab_imu_ring_ready = 1;
		}
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

	/* Debug OSD.  Canvas dim is the encoded frame dim; on Star6E RGN
	 * attaches at the VPE port output (post-SCL crop), so the canvas is
	 * already 1:1 with the encoded frame and needs no zoom-time offset.
	 * HW-crop stab is no different: port0 outputs the cropped encoded dim,
	 * so the OSD is static 1:1 like the non-stab/zoom path.  ONLY the
	 * legacy blit fallback attaches at the full source dim (port0 is
	 * uncropped there) — seed the panel offset to the static centre-crop
	 * origin, then star6e_runtime tracks the live crop window per-frame via
	 * star6e_pipeline_stab_panel_anchor(). */
	if (vcfg->debug.show_osd) {
		int legacy_stab = star6e_stab_enabled(vcfg) && !g_stab_hw_mode;
		uint32_t osd_w = legacy_stab ? g_stab_src_w : state->image_width;
		uint32_t osd_h = legacy_stab ? g_stab_src_h : state->image_height;
		state->debug_osd = debug_osd_create(osd_w, osd_h, &state->vpe_port);
		if (!state->debug_osd) {
			fprintf(stderr, "WARNING: debug OSD requested but MI_RGN unavailable\n");
		} else if (legacy_stab) {
			int off_x = (int)((g_stab_src_w - g_stab_enc_w) / 2u);
			int off_y = (int)((g_stab_src_h - g_stab_enc_h) / 2u);
			debug_osd_set_panel_offset(state->debug_osd, off_x, off_y);
		}
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

	/* Clear userspace persist flags.  runner_teardown follows with
	 * MI_SYS_Exit, and the next pipeline_start always runs in a fresh
	 * process (SIGHUP-respawn forks a successor), so the kernel
	 * ISP/CUS3A state is genuinely cold on the next start.  Skipping
	 * these clears would leave stale "already initialised" flags that
	 * bypass the very work the fresh kernel state expects us to redo. */
	g_isp_initialized = 0;
	g_last_isp_bin_path[0] = '\0';
	g_cus3a_handoff_done = 0;
	venc_api_clear_active_precrop();
	star6e_pipeline_clear_zoom_status();
	star6e_pan_ramp_stop();
	/* AE crop: the SDK rejected our "restore full-frame" call on this BSP,
	 * so we never emit it.  Cycle the ready flag so the next start
	 * re-arms via the ISP-ready hook, and re-seed the cache so the next
	 * sub-rect (if any) is unconditionally re-emitted. */
	g_star6e_ae_crop_ready = 0;
	g_star6e_ae_crop_last.crop_x = 0;
	g_star6e_ae_crop_last.crop_y = 0;
	g_star6e_ae_crop_last.crop_w = 1023;
	g_star6e_ae_crop_last.crop_h = 1023;

	/* Clear IntraRefresh status snapshot — the channel it described is
	 * about to be destroyed, so /api/v1/intra/status should not keep
	 * reporting enabled=true until the next pipeline_start runs. */
	pthread_mutex_lock(&g_intra_status_mutex);
	memset(&g_intra_status, 0, sizeof(g_intra_status));
	pthread_mutex_unlock(&g_intra_status_mutex);

	/* The recording thread must keep consuming ch1 frames through
	 * the ENTIRE teardown sequence.  At 120fps, the 12-frame ch1
	 * buffer fills in ~100ms — any gap in consumption causes VPE
	 * backpressure → kernel D-state → StopRecvPic hangs.
	 *
	 * Sequence: teardown peripherals → drain both channels →
	 * StopRecvPic (thread still draining ch1) → signal thread
	 * to stop → join thread. */

	/* Stop + destroy IMU.  The push callback is now a stub (EIS was
	 * removed; see HISTORY 0.8.0) so order vs other teardown steps
	 * doesn't matter, but keeping the stop-then-destroy split lets a
	 * future telemetry consumer slot in without rework. */
	if (state->imu) {
		imu_stop(state->imu);
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

	/* Tear down JPEG snapshot channel first — it's bound to the same
	 * VPE port we're about to unbind, and its UnBindChnPort/DestroyChn
	 * must run while the SDK still holds a consistent view of the VPE
	 * source.  Idempotent; safe even if init was skipped or failed. */
	venc_jpeg_shutdown();

	/* Stop the stabilization drain thread.  In HW-crop mode this parks
	 * the detector, disables the tiny port1 tap, and joins the thread;
	 * port0 stays a normal VPE→VENC bind torn down by the standard unbind
	 * path below (state->bound_vpe_venc set).  In the legacy blit-fallback
	 * mode the thread is port0's only consumer, so this path inherits the
	 * historic teardown fragility (recovered by the watchdog) — that mode
	 * only runs when port1 is unsupported.  Idempotent: no-op when stab
	 * was not started. */
	star6e_stab_stop();

	/* MI teardown order: StopRecvPic each VENC consumer BEFORE unbinding
	 * its input port.  The previous Star6E order unbound VPE→VENC first and
	 * only then stopped VENC, leaving the kernel SDK still encoding/flushing
	 * a buffered frame out of a port userspace had just ripped out — VENC
	 * (MMU client 0x15) then reads a freed VPE buffer:
	 * `_MI_SYS_MMU_Callback Status=0x2 IsWrite=0` storms into a hardware
	 * watchdog reset on the ~2nd rapid respawn (reproduced on master from a
	 * cold boot, so it predates the stab/framing work).  Maruko hit the same
	 * root cause as a page fault in MI_SYS_IMPL_FlushInputPortTasks and was
	 * fixed to stop-first — see maruko_pipeline_teardown_graph(); Star6E was
	 * never given that fix until now.  StopRecvPic is a soft pause and does
	 * not deadlock while still bound.  Sequence: StopRecvPic → drain output →
	 * unbind VPE→VENC → unbind VIF→VPE → destroy VENC → stop VPE/VIF/sensor. */
	if (state->dual)
		MI_VENC_StopRecvPic(state->dual->channel);
	MI_VENC_StopRecvPic(state->venc_channel);

	/* Drain the last buffered frames that StopRecvPic let flow out. */
	drain_venc_channel(state->venc_channel, 150, "ch0");
	if (state->dual)
		drain_venc_channel(state->dual->channel, 150, "ch1-post");

	/* Unbind VPE→VENC now that the consumer is stopped. */
	if (state->dual && state->dual->bound) {
		MI_SYS_UnBindChnPort(&state->vpe_port, &state->dual->port);
		state->dual->bound = 0;
	}
	if (state->bound_vpe_venc) {
		MI_SYS_UnBindChnPort(&state->vpe_port, &state->venc_port);
		state->bound_vpe_venc = 0;
	}

	if (state->bound_vif_vpe) {
		MI_SYS_UnBindChnPort(&state->vif_port, &state->vpe_port);
		state->bound_vif_vpe = 0;
	}

	/* Destroy channels */
	if (state->dual) {
		venc_api_dual_unregister();
		MI_VENC_DestroyChn(state->dual->channel);
		/* Release the dual sidecar fd before freeing the struct.  Without
		 * this, every dual-stream stop leaks the ch1 listener; usually
		 * masked by fork+exec respawn but a clean process-exit path or
		 * future in-process reinit would accumulate it. */
		star6e_video_reset(&state->dual->video);
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
	star6e_pipeline_clear_zoom_status();

	if (prepare_pipeline_config(state, vcfg, &pconf) != 0)
		return -1;

	ret = select_and_configure_sensor(state, &pconf, vcfg, sdk_quiet);
	if (ret != 0)
		return ret;

	state->active_precrop = pconf.precrop;
	venc_api_set_active_precrop(pconf.precrop.x, pconf.precrop.y,
		pconf.precrop.w, pconf.precrop.h);

	ret = star6e_pipeline_start_vif(&state->sensor, &pconf.precrop);
	if (ret != 0)
		goto fail_sensor;

	ret = star6e_pipeline_start_vpe(&state->sensor, &pconf.precrop,
		pconf.image_width, pconf.image_height,
		pconf.image_mirror, pconf.image_flip,
		pconf.vpe_level_3dnr, sdk_quiet);
	if (ret != 0)
		goto fail_vif;

	/* image_width/height aren't stored on state until bind; pre-populate so
	 * the initial zoom apply (and subsequent live updates before first
	 * frame) sees correct port-output dims. */
	state->image_width = pconf.image_width;
	state->image_height = pconf.image_height;

	/* Framing mode: the video0.framing preset expands into either stab
	 * (stab_crop_pct/recenter) or zoom (zoom_pct).  They are mutually
	 * exclusive, so exactly one branch below runs.
	 *
	 * Stab path (HW-crop): VPE port0 hardware-crops the stab window into a
	 * VENC bind; a tiny port1 tap feeds IVE shift detection.  Legacy
	 * manual-drain+blit is the automatic fallback (see star6e_stab_setup_ports).
	 * VENC ch0 is created at the crop dim.  zoomX/zoomY pick the crop center so
	 * the stabilized stream pans live.  Zoom path: the pan-ramp thread owns
	 * x/y at the configured zoom_pct.  When framing is off, zoom_pct is 0 and
	 * the pan-ramp runs at full image. */
	if (star6e_stab_enabled(vcfg)) {
		/* Clamp the stab source to <=1920x1080 (preserve aspect, even-
		 * aligned) to avoid the high-res fps regression: above 1080p the
		 * VPE+VENC path cannot sustain 60/90/120 fps with stab on.  At
		 * <=1080p configs this is a no-op. */
		if (pconf.image_width > 1920 || pconf.image_height > 1080) {
			uint32_t sw = pconf.image_width, sh = pconf.image_height;
			uint64_t rw = (uint64_t)1920 * 1000 / sw;
			uint64_t rh = (uint64_t)1080 * 1000 / sh;
			uint64_t r = rw < rh ? rw : rh;
			uint32_t nw = (uint32_t)((uint64_t)sw * r / 1000) & ~1u;
			uint32_t nh = (uint32_t)((uint64_t)sh * r / 1000) & ~1u;
			if (nw < 2) nw = 2;
			if (nh < 2) nh = 2;
			fprintf(stderr, "[waybeam] WARNING: video0.framing '%s': source "
				"%ux%u exceeds 1920x1080; clamping to %ux%u to preserve "
				"fps\n", vcfg->video0.framing, sw, sh, nw, nh);
			pconf.image_width = nw;
			pconf.image_height = nh;
		}
		/* Stabilization is always CENTERED: the crop window must sit at the
		 * frame center so the accumulator has symmetric headroom (±max) on
		 * both axes.  zoomX/zoomY are the zoom-mode pan and do NOT apply here —
		 * an off-center pan (e.g. 0.7) would pin the window against the frame
		 * edge and leave no room to stabilize.  Pass 0.5/0.5 unconditionally;
		 * the saved zoomX/zoomY are preserved for when the user switches to a
		 * zoom preset. */
		star6e_stab_configure(pconf.image_width, pconf.image_height,
			vcfg->video0.stab_crop_pct,
			vcfg->video0.stab_recenter_speed,
			vcfg->video0.fps,
			0.5, 0.5);
		pconf.image_width = g_stab_enc_w;
		pconf.image_height = g_stab_enc_h;
		state->image_width = g_stab_enc_w;
		state->image_height = g_stab_enc_h;
	} else {
		(void)star6e_pan_ramp_start(state, vcfg->video0.zoom_pct,
			vcfg->video0.zoom_x, vcfg->video0.zoom_y);
	}

	state->venc_channel = 0;
	venc_fps = vcfg->video0.fps;
	if (venc_fps == 0 || venc_fps > pconf.sensor_framerate)
		venc_fps = pconf.sensor_framerate;

	/* IntraRefresh auto-GOP: when intraRefreshMode != off and the user did
	 * not pin gopSize, override the GOP frame count so each IDR aligns with
	 * one full GDR pass. */
	{
		IntraRefreshDerived ir;
		IntraRefreshMode mode = star6e_pipeline_intra_refresh_derive(
			vcfg, pconf.image_height, venc_fps, pconf.rc_codec, &ir);
		if (mode != INTRA_MODE_OFF && !ir.gop_overridden && ir.gop_frames > 0)
			pconf.venc_gop_size = ir.gop_frames;
	}

	ret = star6e_pipeline_start_venc(pconf.image_width, pconf.image_height,
		pconf.venc_max_rate, venc_fps, pconf.venc_gop_size,
		pconf.rc_codec, pconf.rc_mode,
		vcfg->video0.frame_lost, vcfg, &state->venc_channel);
	if (ret != 0)
		goto fail_vpe;

	/* IntraRefresh — opt-in via video0.intra_refresh.  Failure is logged
	 * but not fatal: stream still works without rolling refresh. */
	(void)star6e_pipeline_apply_intra_refresh(state->venc_channel, vcfg,
		pconf.image_height, venc_fps, pconf.rc_codec);

	/* SVC-T reference pyramid (refPred) is applied inside
	 * star6e_pipeline_start_venc() before StartRecvPic — the SDK
	 * requires that ordering or the call silently no-ops. */

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

	/* Stabilization owns VPE port0 via a manual GetBuf drain; a dual VENC
	 * bind on the same source port would contend with that drain (and dual
	 * would inherit the crop-overridden image dims).  The two are mutually
	 * exclusive for the *second* channel — so we skip the dual ch1 here but
	 * the caller still starts the ts_recorder on the stabilized main ch0
	 * (runtime's generic record block).  Net effect: recording is NOT
	 * disabled, it is downgraded to single-channel — one .ts of the
	 * stabilized main stream, at the main-stream bitrate rather than
	 * record.bitrate. */
	if (g_stab_running) {
		fprintf(stderr, "[waybeam] WARNING: dual recording downgraded to "
			"single-channel while image stabilization is active "
			"(video0.framing): no separate ch1 — recording the "
			"stabilized main stream (ch0) at its bitrate, not "
			"record.bitrate\n");
		return 0;
	}

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

	/* Dual ch1 does not see vcfg here — refPred not applied to ch1.  The
	 * secondary stream is typically a low-bandwidth recorder feed where
	 * SVC-T is less impactful; revisit if needed. */
	ret = star6e_pipeline_start_venc(state->image_width,
		state->image_height, bitrate, fps, gop,
		PT_H265, 3 /* CBR */, frame_lost, NULL, &d->channel);
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
	/* Mirrors the close-before-free in star6e_pipeline_stop above:
	 * free(d) without releasing d->video.sidecar.fd would leak the ch1
	 * sidecar listener.  star6e_pipeline_stop_dual is the live toggle-off
	 * path (no respawn), so the SOCK_CLOEXEC safety net does not apply
	 * here. */
	star6e_video_reset(&d->video);
	free(d->stream_packs);
	free(d);
	state->dual = NULL;
}
