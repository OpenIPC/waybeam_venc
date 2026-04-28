#include "star6e_controls.h"

#include "idr_rate_limit.h"
#include "output_socket.h"
#include "pipeline_common.h"
#include "star6e_audio.h"
#include "star6e_cus3a.h"
#include "star6e_iq.h"
#include "star6e_output.h"
#include "venc_api.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	MI_VENC_CHN venc_chn;
	MI_SYS_ChnPort_t vpe_port;
	MI_SYS_ChnPort_t venc_port;
	volatile uint32_t sensor_fps;
	uint32_t frame_width;
	uint32_t frame_height;
	Star6ePipelineState *pipeline;
	VencConfig *vcfg;
} Star6eControlContext;

typedef struct {
	int /*MI_ISP_BOOL_e*/ bIsStable;
	uint16_t u16Rgain;
	uint16_t u16Grgain;
	uint16_t u16Gbgain;
	uint16_t u16Bgain;
	uint16_t u16ColorTemp;
	uint8_t u8WPInd;
	int bMultiLSDetected;
	uint8_t u8FirstLSInd;
	uint8_t u8SecondLSInd;
} AwbQueryInfo_t;

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
} __attribute__((packed, aligned(1))) AwbCus3aInfo_t;

typedef struct {
	int bEnable;
	uint16_t u16GlbGainThd;
	uint16_t u16CountThd;
	uint16_t u16ForceTriGainThd;
} AwbStabilizer_t;

typedef struct {
	uint32_t u32CT;
} AwbCtMwb_t;

typedef struct {
	int eState;
	int eOpType;
	uint16_t mwb_rgain;
	uint16_t mwb_grgain;
	uint16_t mwb_gbgain;
	uint16_t mwb_bgain;
	uint8_t _pad[8192];
} AwbAttr_t;

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

typedef struct {
	uint32_t u32FNx10;
	uint32_t u32SensorGain;
	uint32_t u32ISPGain;
	uint32_t u32US;
} AeExpoValue_t;

typedef struct {
	uint32_t u32LumY;
	uint32_t u32AvgY;
	uint32_t u32Hits[128];
} AeHistWeightY_t;

typedef struct {
	int bIsStable;
	int bIsReachBoundary;
	AeExpoValue_t stExpoValueLong;
	AeExpoValue_t stExpoValueShort;
	AeHistWeightY_t stHistWeightY;
	uint32_t u32LVx10;
	int32_t s32BV;
	uint32_t u32SceneTarget;
} AeExpoInfo_t;

typedef struct {
	int plane_ret;
	int limit_ret;
	int info_ret;
	int state_ret;
	int mode_ret;
	MI_SNR_PAD_ID_e pad_id;
	MI_SNR_PlaneInfo_t plane;
	IspExposureLimit limit;
	AeExpoInfo_t info;
	int ae_state;
	int ae_mode_raw;
} AeDiagSnapshot;

enum {
	STAR6E_AWB_MODE_AUTO = 0,
	STAR6E_AWB_MODE_MANUAL = 1,
	STAR6E_AWB_MODE_CT_MANUAL = 2,
};

enum {
	STAR6E_ISP_STATE_NORMAL = 0,
	STAR6E_ISP_STATE_PAUSE = 1,
	STAR6E_AE_EXPO_MODE_AUTO = 0,
};

static Star6eControlContext g_star6e_control_ctx;

static int apply_encoder_gop(uint32_t gop_size);
static int request_idr(void);

static uint32_t align_down(uint32_t value, uint32_t align)
{
	return value / align * align;
}

static int apply_rc_qp_delta(const MI_VENC_ChnAttr_t *attr, MI_VENC_RcParam_t *param,
	int delta)
{
	if (!attr || !param || delta < -12 || delta > 12)
		return -1;

	switch (attr->rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		param->stParamH265Cbr.s32IPQPDelta = delta;
		return 0;
	case I6_VENC_RATEMODE_H264CBR:
		param->stParamH264Cbr.s32IPQPDelta = delta;
		return 0;
	case I6_VENC_RATEMODE_H265VBR:
		param->stParamH265Vbr.s32IPQPDelta = delta;
		return 0;
	case I6_VENC_RATEMODE_H264VBR:
		param->stParamH264VBR.s32IPQPDelta = delta;
		return 0;
	case I6_VENC_RATEMODE_H265AVBR:
		param->stParamH265Avbr.s32IPQPDelta = delta;
		return 0;
	case I6_VENC_RATEMODE_H264AVBR:
		param->stParamH264Avbr.s32IPQPDelta = delta;
		return 0;
	default:
		return -1;
	}
}

int star6e_controls_apply_frame_lost_threshold(MI_VENC_CHN chn, bool enabled,
	uint32_t kbps)
{
	MI_VENC_ParamFrameLost_t lost = {0};

	if (!enabled)
		return 0;

	lost.bFrmLostOpen = 1;
	lost.eFrmLostMode = E_MI_VENC_FRMLOST_NORMAL;
	lost.u32FrmLostBpsThr = pipeline_common_frame_lost_threshold(kbps);
	lost.u32EncFrmGaps = 0;

	return MI_VENC_SetFrameLostStrategy(chn, &lost) == 0 ? 0 : -1;
}

static int apply_bitrate(uint32_t kbps)
{
	MI_VENC_ChnAttr_t attr = {0};
	MI_U32 bits;
	bool frame_lost_enabled = true;

	if (kbps > 200000)
		kbps = 200000;
	bits = kbps * 1024;

	if (g_star6e_control_ctx.vcfg)
		frame_lost_enabled = g_star6e_control_ctx.vcfg->video0.frame_lost;

	if (MI_VENC_GetChnAttr(g_star6e_control_ctx.venc_chn, &attr) != 0)
		return -1;

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

	if (MI_VENC_SetChnAttr(g_star6e_control_ctx.venc_chn, &attr) != 0)
		return -1;
	if (star6e_controls_apply_frame_lost_threshold(g_star6e_control_ctx.venc_chn,
	    frame_lost_enabled, kbps) != 0)
		return -1;
	/* Force an IDR after a bitrate change so the decoder resyncs against
	 * the new rate-control state.  Goes through the rate-limit gate so
	 * bitrate-storm calls can't DoS the stream.  Tight bitrate ramps
	 * (e.g. 100-step adaptive-link ladders inside the gate's min-spacing
	 * window, default 100 ms) will only IDR on the first step and again
	 * after the window expires — acceptable because the encoder rate
	 * controller absorbs small between-IDR changes without decoder
	 * resync. */
	if (idr_rate_limit_allow(g_star6e_control_ctx.venc_chn))
		(void)MI_VENC_RequestIdr(g_star6e_control_ctx.venc_chn, 1);
	return 0;
}

static int apply_encoder_gop(uint32_t gop_size)
{
	MI_VENC_ChnAttr_t attr = {0};

	if (MI_VENC_GetChnAttr(g_star6e_control_ctx.venc_chn, &attr) != 0)
		return -1;

	switch (attr.rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		attr.rate.h265Cbr.gop = gop_size;
		break;
	case I6_VENC_RATEMODE_H264CBR:
		attr.rate.h264Cbr.gop = gop_size;
		break;
	case I6_VENC_RATEMODE_H265VBR:
		attr.rate.h265Vbr.gop = gop_size;
		break;
	case I6_VENC_RATEMODE_H264VBR:
		attr.rate.h264Vbr.gop = gop_size;
		break;
	case I6_VENC_RATEMODE_H265AVBR:
		attr.rate.h265Avbr.gop = gop_size;
		break;
	case I6_VENC_RATEMODE_H264AVBR:
		attr.rate.h264Avbr.gop = gop_size;
		break;
	default:
		return -1;
	}

	return MI_VENC_SetChnAttr(g_star6e_control_ctx.venc_chn, &attr) == 0 ?
		0 : -1;
}

static int apply_gop(uint32_t gop_size)
{
	return apply_encoder_gop(gop_size);
}

static int apply_qp_delta(int delta)
{
	MI_VENC_ChnAttr_t attr = {0};
	MI_VENC_RcParam_t param = {0};

	if (MI_VENC_GetChnAttr(g_star6e_control_ctx.venc_chn, &attr) != 0)
		return -1;
	if (MI_VENC_GetRcParam(g_star6e_control_ctx.venc_chn, &param) != 0)
		return -1;
	if (apply_rc_qp_delta(&attr, &param, delta) != 0)
		return -1;
	if (MI_VENC_SetRcParam(g_star6e_control_ctx.venc_chn, &param) != 0)
		return -1;
	if (request_idr() != 0)
		return -1;
	printf("> qpDelta changed to %d\n", delta);
	return 0;
}

static int apply_encoder_fps(uint32_t fps)
{
	MI_VENC_ChnAttr_t attr = {0};
	if (MI_VENC_GetChnAttr(g_star6e_control_ctx.venc_chn, &attr) != 0)
		return -1;

	switch (attr.rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		attr.rate.h265Cbr.fpsNum = fps;
		break;
	case I6_VENC_RATEMODE_H264CBR:
		attr.rate.h264Cbr.fpsNum = fps;
		break;
	case I6_VENC_RATEMODE_H265VBR:
		attr.rate.h265Vbr.fpsNum = fps;
		break;
	case I6_VENC_RATEMODE_H264VBR:
		attr.rate.h264Vbr.fpsNum = fps;
		break;
	case I6_VENC_RATEMODE_H265AVBR:
		attr.rate.h265Avbr.fpsNum = fps;
		break;
	case I6_VENC_RATEMODE_H264AVBR:
		attr.rate.h264Avbr.fpsNum = fps;
		break;
	default:
		break;
	}

	return MI_VENC_SetChnAttr(g_star6e_control_ctx.venc_chn, &attr) == 0 ?
		0 : -1;
}

static int apply_scene_fps(uint32_t fps)
{
	(void)fps;
	return 0;
}

static int apply_fps(uint32_t fps)
{
	MI_S32 bind_ret;
	uint32_t sensor_fps;

	if (fps == 0 || fps > 120)
		return -1;

	sensor_fps = g_star6e_control_ctx.sensor_fps;
	if (fps > sensor_fps) {
		printf("> FPS %u exceeds sensor mode max %u, clamping\n", fps,
			sensor_fps);
		fps = sensor_fps;
	}

	MI_SYS_UnBindChnPort(&g_star6e_control_ctx.vpe_port,
		&g_star6e_control_ctx.venc_port);
	bind_ret = MI_SYS_BindChnPort2(&g_star6e_control_ctx.vpe_port,
		&g_star6e_control_ctx.venc_port, sensor_fps, fps,
		I6_SYS_LINK_FRAMEBASE, 0);
	if (bind_ret != 0) {
		printf("> Rebind VPE->VENC at %u:%u fps failed %d, restoring\n",
			sensor_fps, fps, bind_ret);
		MI_SYS_BindChnPort2(&g_star6e_control_ctx.vpe_port,
			&g_star6e_control_ctx.venc_port, sensor_fps, sensor_fps,
			I6_SYS_LINK_FRAMEBASE, 0);
		return -1;
	}

	if (apply_encoder_fps(fps) != 0)
		return -1;
	if (apply_scene_fps(fps) != 0)
		return -1;

	printf("> FPS changed to %u (bind %u:%u)\n", fps, sensor_fps, fps);
	return 0;
}

static int apply_gain_max(uint32_t gain)
{
	if (star6e_cus3a_running())
		star6e_cus3a_set_gain_max(gain);
	return 0;
}

static int apply_verbose(bool on)
{
	printf("> Verbose %s via API\n", on ? "enabled" : "disabled");
	return 0;
}

static int request_idr(void)
{
	int chn = g_star6e_control_ctx.venc_chn;
	if (!idr_rate_limit_allow(chn))
		return 0;  /* coalesced — not an error */
	return MI_VENC_RequestIdr(chn, 1) == 0 ? 0 : -1;
}

/* Compute one horizontal ROI band for step index of steps.
 * Full-height bands centered horizontally, tapered QP toward edges.
 * Returns 0 if valid, -1 if region should be skipped. */
static int compute_horizontal_roi(uint32_t width, uint32_t height,
	float center_frac, int qp, int steps, int index,
	MI_VENC_RoiCfg_t *roi)
{
	float frac;
	uint32_t rw, rh, rx;
	int level;

	if (!roi || index < 0 || index >= steps)
		return -1;

	level = index + 1;
	frac = center_frac + (1.0f - center_frac) *
		(float)(steps - level) / (float)steps;
	rw = align_down((uint32_t)(frac * width), 32);
	rh = align_down(height, 32);
	rx = align_down((width - rw) / 2, 32);
	if (rw == 0 || rh == 0)
		return -1;

	roi->u32Index = (uint32_t)index;
	roi->bEnable = 1;
	roi->bAbsQp = 0;
	roi->s32Qp = pipeline_common_scale_roi_qp(qp, level, steps);
	roi->stRect.u32Left = rx;
	roi->stRect.u32Top = 0;
	roi->stRect.u32Width = rw;
	roi->stRect.u32Height = rh;
	return 0;
}


static int apply_roi_qp(int qp)
{
	uint32_t width = g_star6e_control_ctx.frame_width;
	uint32_t height = g_star6e_control_ctx.frame_height;
	int ok = 1;
	uint16_t steps;
	float center_frac;

	if (width == 0 || height == 0)
		return -1;

	for (int i = 0; i < PIPELINE_ROI_MAX_STEPS; i++) {
		MI_VENC_RoiCfg_t roi = {0};
		roi.u32Index = i;
		roi.bEnable = 0;
		MI_VENC_SetRoiCfg(g_star6e_control_ctx.venc_chn, &roi);
	}

	if (!g_star6e_control_ctx.vcfg ||
	    !g_star6e_control_ctx.vcfg->fpv.roi_enabled || qp == 0) {
		printf("> ROI disabled (all regions cleared)\n");
		return 0;
	}

	if (qp < -30) qp = -30;
	if (qp > 30) qp = 30;

	steps = g_star6e_control_ctx.vcfg->fpv.roi_steps;
	if (steps < 1) steps = 1;
	if (steps > PIPELINE_ROI_MAX_STEPS) steps = PIPELINE_ROI_MAX_STEPS;

	center_frac = (float)g_star6e_control_ctx.vcfg->fpv.roi_center;
	if (center_frac < 0.1f) center_frac = 0.1f;
	if (center_frac > 0.9f) center_frac = 0.9f;

	for (int i = 0; i < steps; i++) {
		MI_VENC_RoiCfg_t roi = {0};
		MI_S32 ret;

		if (compute_horizontal_roi(width, height, center_frac, qp,
		    steps, i, &roi) != 0)
			continue;

		ret = MI_VENC_SetRoiCfg(g_star6e_control_ctx.venc_chn, &roi);
		if (ret != 0) {
			printf("> ROI[%d] set failed (ret=0x%08x) rect=(%u,%u %ux%u) qp=%+d\n",
				i, (unsigned)ret,
				roi.stRect.u32Left, roi.stRect.u32Top,
				roi.stRect.u32Width, roi.stRect.u32Height,
				roi.s32Qp);
			ok = 0;
		}
	}

	if (ok) {
		printf("> ROI horizontal: %ux%u, %u steps, center=%.0f%%, qp=%+d\n",
			width, height, steps, center_frac * 100.0f, qp);
	}

	return ok ? 0 : -1;
}

static const char *ae_state_name(int state)
{
	return state == STAR6E_ISP_STATE_PAUSE ? "pause" : "normal";
}

static const char *ae_expo_mode_name(int mode)
{
	return mode == STAR6E_AE_EXPO_MODE_AUTO ? "auto" : "unknown";
}

static void ae_diag_snapshot_defaults(AeDiagSnapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->pad_id = 0;
	snapshot->limit_ret = -1;
	snapshot->info_ret = -1;
	snapshot->state_ret = -1;
	snapshot->mode_ret = -1;
	snapshot->ae_state = STAR6E_ISP_STATE_NORMAL;
	snapshot->ae_mode_raw = STAR6E_AE_EXPO_MODE_AUTO;
}

static void ae_diag_snapshot_collect(AeDiagSnapshot *snapshot)
{
	typedef MI_S32 (*fn_get_limit_t)(uint32_t, IspExposureLimit *);
	typedef MI_S32 (*fn_query_info_t)(uint32_t, AeExpoInfo_t *);
	typedef MI_S32 (*fn_get_state_t)(uint32_t, int *);
	typedef MI_S32 (*fn_get_mode_t)(uint32_t, int *);
	void *handle;

	ae_diag_snapshot_defaults(snapshot);
	if (g_star6e_control_ctx.pipeline)
		snapshot->pad_id = g_star6e_control_ctx.pipeline->sensor.pad_id;
	snapshot->plane_ret = MI_SNR_GetPlaneInfo(snapshot->pad_id, 0,
		&snapshot->plane);

	handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!handle)
		return;

	{
		fn_get_limit_t fn_get_limit = (fn_get_limit_t)dlsym(handle,
			"MI_ISP_AE_GetExposureLimit");
		fn_query_info_t fn_query_info = (fn_query_info_t)dlsym(handle,
			"MI_ISP_AE_QueryExposureInfo");
		fn_get_state_t fn_get_state = (fn_get_state_t)dlsym(handle,
			"MI_ISP_AE_GetState");
		fn_get_mode_t fn_get_mode = (fn_get_mode_t)dlsym(handle,
			"MI_ISP_AE_GetExpoMode");

		if (fn_get_limit)
			snapshot->limit_ret = fn_get_limit(0, &snapshot->limit);
		if (fn_query_info)
			snapshot->info_ret = fn_query_info(0, &snapshot->info);
		if (fn_get_state)
			snapshot->state_ret = fn_get_state(0, &snapshot->ae_state);
		if (fn_get_mode)
			snapshot->mode_ret = fn_get_mode(0, &snapshot->ae_mode_raw);
	}

	dlclose(handle);
}

static uint32_t ae_diag_exposure_us(const AeDiagSnapshot *snapshot)
{
	if (snapshot->info_ret == 0)
		return snapshot->info.stExpoValueLong.u32US;
	return snapshot->plane.shutter;
}

static uint32_t ae_diag_sensor_gain(const AeDiagSnapshot *snapshot)
{
	if (snapshot->info_ret == 0)
		return snapshot->info.stExpoValueLong.u32SensorGain;
	return snapshot->plane.sensGain;
}

static uint32_t ae_diag_isp_gain(const AeDiagSnapshot *snapshot)
{
	if (snapshot->info_ret == 0)
		return snapshot->info.stExpoValueLong.u32ISPGain;
	return snapshot->plane.compGain;
}

static uint32_t ae_diag_sensor_fps(void)
{
	if (g_star6e_control_ctx.sensor_fps != 0)
		return g_star6e_control_ctx.sensor_fps;
	if (g_star6e_control_ctx.vcfg)
		return g_star6e_control_ctx.vcfg->video0.fps;
	return 0;
}

static uint32_t query_live_fps(void)
{
	MI_VENC_ChnAttr_t attr = {0};

	if (MI_VENC_GetChnAttr(g_star6e_control_ctx.venc_chn, &attr) != 0)
		return 0;

	switch (attr.rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		return attr.rate.h265Cbr.fpsNum;
	case I6_VENC_RATEMODE_H264CBR:
		return attr.rate.h264Cbr.fpsNum;
	case I6_VENC_RATEMODE_H265VBR:
		return attr.rate.h265Vbr.fpsNum;
	case I6_VENC_RATEMODE_H264VBR:
		return attr.rate.h264Vbr.fpsNum;
	case I6_VENC_RATEMODE_H265AVBR:
		return attr.rate.h265Avbr.fpsNum;
	case I6_VENC_RATEMODE_H264AVBR:
		return attr.rate.h264Avbr.fpsNum;
	default:
		return 0;
	}
}

static char *query_ae_info(void)
{
	AeDiagSnapshot snapshot;
	char buf[2048];
	char precrop_field[96];
	uint32_t exposure_us;
	uint32_t sensor_gain;
	uint32_t isp_gain;
	uint16_t px = 0, py = 0, pw = 0, ph = 0;

	ae_diag_snapshot_collect(&snapshot);
	exposure_us = ae_diag_exposure_us(&snapshot);
	sensor_gain = ae_diag_sensor_gain(&snapshot);
	isp_gain = ae_diag_isp_gain(&snapshot);

	if (venc_api_get_active_precrop(&px, &py, &pw, &ph)) {
		snprintf(precrop_field, sizeof(precrop_field),
			",\"active_precrop\":{\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u}",
			px, py, pw, ph);
	} else {
		precrop_field[0] = '\0';
	}

	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{"
		"\"sensor_plane\":{\"ret\":%d,\"pad\":%d,\"shutter_us\":%u,"
		"\"sensor_gain_x1024\":%u,\"comp_gain_x1024\":%u},"
		"\"exposure_limit\":{\"ret\":%d,\"min_shutter_us\":%u,"
		"\"max_shutter_us\":%u,\"min_sensor_gain\":%u,\"max_sensor_gain\":%u,"
		"\"min_isp_gain\":%u,\"max_isp_gain\":%u},"
		"\"exposure_info\":{\"ret\":%d,\"stable\":%s,\"reach_boundary\":%s,"
		"\"long_us\":%u,\"long_sensor_gain_x1024\":%u,"
		"\"long_isp_gain_x1024\":%u,\"luma_y\":%u,\"avg_y\":%u,"
		"\"lv_x10\":%u,\"bv\":%d,\"scene_target\":%u},"
		"\"state\":{\"ret\":%d,\"raw\":%d,\"name\":\"%s\"},"
		"\"expo_mode\":{\"ret\":%d,\"raw\":%d,\"name\":\"%s\"},"
		"\"metrics\":{\"exposure_us\":%u,\"sensor_gain_x1024\":%u,"
		"\"isp_gain_x1024\":%u,\"fps\":%u},"
		"\"runtime\":{\"sensor_fps\":%u%s}}}",
		snapshot.plane_ret, snapshot.pad_id, snapshot.plane.shutter,
		snapshot.plane.sensGain, snapshot.plane.compGain,
		snapshot.limit_ret, snapshot.limit.minShutterUs,
		snapshot.limit.maxShutterUs, snapshot.limit.minSensorGain,
		snapshot.limit.maxSensorGain, snapshot.limit.minIspGain,
		snapshot.limit.maxIspGain,
		snapshot.info_ret,
		snapshot.info_ret == 0 && snapshot.info.bIsStable ? "true" : "false",
		snapshot.info_ret == 0 && snapshot.info.bIsReachBoundary ? "true" : "false",
		snapshot.info.stExpoValueLong.u32US,
		snapshot.info.stExpoValueLong.u32SensorGain,
		snapshot.info.stExpoValueLong.u32ISPGain,
		snapshot.info.stHistWeightY.u32LumY, snapshot.info.stHistWeightY.u32AvgY,
		snapshot.info.u32LVx10, snapshot.info.s32BV,
		snapshot.info.u32SceneTarget,
		snapshot.state_ret, snapshot.ae_state, ae_state_name(snapshot.ae_state),
		snapshot.mode_ret, snapshot.ae_mode_raw,
		ae_expo_mode_name(snapshot.ae_mode_raw),
		exposure_us, sensor_gain, isp_gain, ae_diag_sensor_fps(),
		ae_diag_sensor_fps(), precrop_field);
	return strdup(buf);
}

static char *query_isp_metrics(void)
{
	AeDiagSnapshot snapshot;
	char buf[512];

	ae_diag_snapshot_collect(&snapshot);
	snprintf(buf, sizeof(buf),
		"# HELP isp_again Analog Gain\n"
		"# TYPE isp_again gauge\n"
		"isp_again %u\n"
		"# HELP isp_dgain Digital Gain\n"
		"# TYPE isp_dgain gauge\n"
		"isp_dgain %u\n"
		"# HELP isp_exposure Exposure\n"
		"# TYPE isp_exposure gauge\n"
		"isp_exposure %u\n"
		"# HELP isp_fps Sensor fps\n"
		"# TYPE isp_fps gauge\n"
		"isp_fps %u\n"
		"# HELP isp_luma_y Scene luma Y\n"
		"# TYPE isp_luma_y gauge\n"
		"isp_luma_y %u\n"
		"# HELP isp_avg_y Scene average Y\n"
		"# TYPE isp_avg_y gauge\n"
		"isp_avg_y %u\n"
		"# HELP isp_ae_stable AE stability flag\n"
		"# TYPE isp_ae_stable gauge\n"
		"isp_ae_stable %u\n",
		ae_diag_sensor_gain(&snapshot),
		ae_diag_isp_gain(&snapshot),
		ae_diag_exposure_us(&snapshot) / 1000,
		ae_diag_sensor_fps(),
		snapshot.info.stHistWeightY.u32LumY,
		snapshot.info.stHistWeightY.u32AvgY,
		snapshot.info_ret == 0 && snapshot.info.bIsStable ? 1u : 0u);
	return strdup(buf);
}

static char *query_awb_info(void)
{
	typedef MI_S32 (*fn_query_t)(uint32_t, AwbQueryInfo_t *);
	typedef MI_S32 (*fn_cus3a_status_t)(uint32_t, AwbCus3aInfo_t *);
	typedef MI_S32 (*fn_stab_t)(uint32_t, AwbStabilizer_t *);
	typedef MI_S32 (*fn_attr_t)(uint32_t, AwbAttr_t *);
	typedef MI_S32 (*fn_ctmwb_get_t)(uint32_t, AwbCtMwb_t *);
	void *handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	char buf[2048];
	int pos = 0;

	if (!handle)
		return NULL;

	pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"ok\":true,\"data\":{");

	{
		fn_query_t fn_query = (fn_query_t)dlsym(handle, "MI_ISP_AWB_QueryInfo");

		if (fn_query) {
			AwbQueryInfo_t qi;
			MI_S32 ret;

			memset(&qi, 0, sizeof(qi));
			ret = fn_query(0, &qi);
			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"query_info\":{\"ret\":%d,\"stable\":%s,"
				"\"r_gain\":%u,\"gr_gain\":%u,\"gb_gain\":%u,\"b_gain\":%u,"
				"\"color_temp\":%u,\"wp_index\":%u,"
				"\"multi_ls\":%s,\"ls1_idx\":%u,\"ls2_idx\":%u},",
				ret, qi.bIsStable ? "true" : "false", qi.u16Rgain,
				qi.u16Grgain, qi.u16Gbgain, qi.u16Bgain,
				qi.u16ColorTemp, qi.u8WPInd,
				qi.bMultiLSDetected ? "true" : "false",
				qi.u8FirstLSInd, qi.u8SecondLSInd);
		}
	}

	{
		fn_cus3a_status_t fn_cus3a = (fn_cus3a_status_t)dlsym(handle,
			"MI_ISP_CUS3A_GetAwbStatus");

		if (fn_cus3a) {
			AwbCus3aInfo_t ci;
			MI_S32 ret;

			memset(&ci, 0, sizeof(ci));
			ci.Size = sizeof(ci);
			ret = fn_cus3a(0, &ci);
			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"cus3a_status\":{\"ret\":%d,"
				"\"avg_blk_x\":%u,\"avg_blk_y\":%u,"
				"\"cur_r_gain\":%u,\"cur_g_gain\":%u,\"cur_b_gain\":%u,"
				"\"hdr_mode\":%u,\"bv_x16384\":%u,\"weight_y\":%d},",
				ret, ci.AvgBlkX, ci.AvgBlkY, ci.CurRGain, ci.CurGGain,
				ci.CurBGain, ci.HDRMode, ci.u4BVx16384, ci.WeightY);
		}
	}

	{
		fn_stab_t fn_stab = (fn_stab_t)dlsym(handle,
			"MI_ISP_AWB_GetStabilizer");

		if (fn_stab) {
			AwbStabilizer_t st;
			MI_S32 ret;

			memset(&st, 0, sizeof(st));
			ret = fn_stab(0, &st);
			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"stabilizer\":{\"ret\":%d,\"enabled\":%s,"
				"\"glb_gain_thd\":%u,\"count_thd\":%u,\"force_tri_thd\":%u},",
				ret, st.bEnable ? "true" : "false", st.u16GlbGainThd,
				st.u16CountThd, st.u16ForceTriGainThd);
		}
	}

	{
		fn_attr_t fn_attr = (fn_attr_t)dlsym(handle, "MI_ISP_AWB_GetAttr");

		if (fn_attr) {
			AwbAttr_t attr;
			MI_S32 ret;
			const char *mode_str = "unknown";

			memset(&attr, 0, sizeof(attr));
			ret = fn_attr(0, &attr);
			if (attr.eOpType == STAR6E_AWB_MODE_AUTO)
				mode_str = "auto";
			else if (attr.eOpType == STAR6E_AWB_MODE_MANUAL)
				mode_str = "manual";
			else if (attr.eOpType == STAR6E_AWB_MODE_CT_MANUAL)
				mode_str = "ct_manual";

			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"attr\":{\"ret\":%d,\"state\":%d,\"mode\":\"%s\",\"mode_raw\":%d,"
				"\"mwb_r\":%u,\"mwb_gr\":%u,\"mwb_gb\":%u,\"mwb_b\":%u},",
				ret, attr.eState, mode_str, attr.eOpType, attr.mwb_rgain,
				attr.mwb_grgain, attr.mwb_gbgain, attr.mwb_bgain);
		}
	}

	{
		fn_ctmwb_get_t fn_ctmwb_get = (fn_ctmwb_get_t)dlsym(handle,
			"MI_ISP_AWB_GetCTMwbAttr");

		if (fn_ctmwb_get) {
			AwbCtMwb_t mwb;
			MI_S32 ret;

			memset(&mwb, 0, sizeof(mwb));
			ret = fn_ctmwb_get(0, &mwb);
			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"ct_mwb\":{\"ret\":%d,\"ct\":%u}", ret, mwb.u32CT);
		}
	}

	pos += snprintf(buf + pos, sizeof(buf) - pos, "}}");
	dlclose(handle);
	return strdup(buf);
}

static int apply_awb_mode(int mode, uint32_t ct)
{
	typedef MI_S32 (*fn_get_t)(uint32_t, AwbAttr_t *);
	typedef MI_S32 (*fn_set_t)(uint32_t, AwbAttr_t *);
	typedef MI_S32 (*fn_ctmwb_t)(uint32_t, AwbCtMwb_t *);
	void *handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	int ret = 0;

	if (!handle)
		return -1;

	if (mode == 0) {
		fn_get_t fn_get = (fn_get_t)dlsym(handle,
			"MI_ISP_AWB_GetAttr");
		fn_set_t fn_set = (fn_set_t)dlsym(handle,
			"MI_ISP_AWB_SetAttr");

		if (fn_get && fn_set) {
			AwbAttr_t attr;

			memset(&attr, 0, sizeof(attr));
			if (fn_get(0, &attr) == 0) {
				MI_S32 awb_ret;

				attr.eState = 0;
				attr.eOpType = STAR6E_AWB_MODE_AUTO;
				awb_ret = fn_set(0, &attr);
				if (awb_ret != 0) {
					fprintf(stderr, "WARNING: MI_ISP_AWB_SetAttr(auto) failed: 0x%08x\n",
						(unsigned)awb_ret);
					ret = -1;
				} else {
					printf("> AWB mode: auto\n");
				}
			} else {
				ret = -1;
			}
		} else {
			ret = -1;
		}
	} else {
		fn_ctmwb_t fn_ctmwb = (fn_ctmwb_t)dlsym(handle,
			"MI_ISP_AWB_SetCTMwbAttr");
		fn_get_t fn_get = (fn_get_t)dlsym(handle, "MI_ISP_AWB_GetAttr");
		fn_set_t fn_set = (fn_set_t)dlsym(handle, "MI_ISP_AWB_SetAttr");

		if (fn_ctmwb && fn_get && fn_set) {
			AwbCtMwb_t mwb = { .u32CT = ct };
			MI_S32 awb_ret = fn_ctmwb(0, &mwb);

			if (awb_ret != 0) {
				fprintf(stderr, "WARNING: MI_ISP_AWB_SetCTMwbAttr(%u) failed: 0x%08x\n",
					ct, (unsigned)awb_ret);
				ret = -1;
			} else {
				AwbAttr_t attr;

				memset(&attr, 0, sizeof(attr));
				if (fn_get(0, &attr) == 0) {
					attr.eState = 0;
					attr.eOpType = STAR6E_AWB_MODE_CT_MANUAL;
					awb_ret = fn_set(0, &attr);
					if (awb_ret != 0) {
						fprintf(stderr, "WARNING: MI_ISP_AWB_SetAttr(ct_manual) failed: 0x%08x\n",
							(unsigned)awb_ret);
						ret = -1;
					} else {
						printf("> AWB mode: ct_manual (%uK)\n", ct);
					}
				} else {
					ret = -1;
				}
			}
		} else {
			ret = -1;
		}
	}

	dlclose(handle);
	return ret;
}

static int apply_output_enabled(bool on)
{
	uint32_t restored_fps;

	if (!g_star6e_control_ctx.pipeline)
		return -1;

	if (on) {
		if (!g_star6e_control_ctx.vcfg ||
		    !g_star6e_control_ctx.vcfg->outgoing.server[0]) {
			fprintf(stderr, "> Cannot enable output: no server configured\n");
			return -1;
		}
		g_star6e_control_ctx.pipeline->output_enabled = 1;
		restored_fps = g_star6e_control_ctx.pipeline->stored_fps ?
			g_star6e_control_ctx.pipeline->stored_fps :
			g_star6e_control_ctx.vcfg->video0.fps;
		if (apply_fps(restored_fps) != 0) {
			g_star6e_control_ctx.pipeline->output_enabled = 0;
			return -1;
		}
		if (request_idr() != 0) {
			g_star6e_control_ctx.pipeline->output_enabled = 0;
			return -1;
		}
		printf("> Output enabled, FPS restored to %u\n", restored_fps);
	} else {
		g_star6e_control_ctx.pipeline->output_enabled = 0;
		g_star6e_control_ctx.pipeline->stored_fps = g_star6e_control_ctx.vcfg ?
			g_star6e_control_ctx.vcfg->video0.fps : 30;
		if (apply_fps(STAR6E_CONTROLS_IDLE_FPS) != 0) {
			g_star6e_control_ctx.pipeline->output_enabled = 1;
			return -1;
		}
		printf("> Output disabled, FPS reduced to %u (idle)\n",
			STAR6E_CONTROLS_IDLE_FPS);
	}

	return 0;
}

static int apply_server(const char *uri)
{
	if (!g_star6e_control_ctx.pipeline)
		return -1;
	if (star6e_output_apply_server(&g_star6e_control_ctx.pipeline->output,
	    uri) != 0) {
		return -1;
	}
	if (request_idr() != 0)
		return -1;
	printf("> Destination changed to %s\n", uri);
	return 0;
}

static int apply_max_payload_size(uint16_t size)
{
	Star6ePipelineState *ps = g_star6e_control_ctx.pipeline;

	if (!ps)
		return -1;

	/* Validation enforces [VENC_OUTPUT_PAYLOAD_MIN_BYTES,
	 * VENC_OUTPUT_PAYLOAD_CEILING_BYTES] and the SHM ring is sized to
	 * the ceiling at startup, so any value reaching here fits every
	 * transport. Plain uint16_t stores are atomic on ARM; readers
	 * (encoder thread, audio compact thread) re-read once per frame.
	 * The next frame uses the new value; in-flight fragmentation for
	 * the current frame keeps the old. */
	ps->video.rtp_payload_size = size;
	ps->video.max_frame_size = size;
	if (ps->dual) {
		ps->dual->video.rtp_payload_size = size;
		ps->dual->video.max_frame_size = size;
	}
	/* Audio compact-mode chunking reads this on every audio frame
	 * (star6e_audio_output_send_compact). RTP audio doesn't fragment so
	 * the field is unused there, but we keep both modes in sync. */
	ps->audio.output.max_payload_size = size;

	/* Use stderr (unbuffered, bypasses the audio stdout filter pipe in
	 * star6e_audio.c) so the live-apply trace lands in the log even
	 * when stdout is captured by the filter. */
	fprintf(stderr, "> max_payload_size set to %u (live)\n", (unsigned)size);
	return 0;
}

static int apply_mute(bool on)
{
	if (!g_star6e_control_ctx.pipeline)
		return -1;
	if (!g_star6e_control_ctx.pipeline->audio.channel_enabled) {
		printf("[audio] %s (no-op, audio not active)\n",
			on ? "Muted" : "Unmuted");
		return 0;
	}
	if (star6e_audio_apply_mute(&g_star6e_control_ctx.pipeline->audio,
	    on ? 1 : 0) != 0) {
		return -1;
	}

	printf("[audio] %s\n", on ? "Muted" : "Unmuted");
	return 0;
}

static const char *output_transport_name(const Star6eOutput *o)
{
	if (!o)
		return "none";
	if (o->ring)
		return "shm";
	switch (o->transport) {
	case VENC_OUTPUT_URI_UDP:  return "udp";
	case VENC_OUTPUT_URI_UNIX: return "unix";
	case VENC_OUTPUT_URI_SHM:  return "shm";
	default:                   return "none";
	}
}

static char *query_transport_status(void)
{
	Star6ePipelineState *ps = g_star6e_control_ctx.pipeline;
	const VencConfig *vcfg = g_star6e_control_ctx.vcfg;
	char buf[768];
	const char *transport;
	int pos;

	if (!ps || !vcfg)
		return NULL;
	transport = output_transport_name(&ps->output);

	if (ps->output.ring) {
		venc_ring_fill_t fill;
		if (venc_ring_get_fill(ps->output.ring, &fill) != 0)
			return NULL;
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"active\":true,"
			"\"transport\":\"%s\","
			"\"fillPct\":%u,"
			"\"inPressure\":%s,"
			"\"transportDrops\":%llu,"
			"\"pressureDrops\":%llu,"
			"\"packetsSent\":%llu,"
			"\"oversizeDrops\":%llu,"
			"\"backpressure\":%s,"
			"\"highWaterPct\":%u,"
			"\"lowWaterPct\":%u,"
			"\"slotCount\":%u,"
			"\"usedSlots\":%u}}",
			transport,
			(unsigned)fill.fill_pct,
			ps->output.in_pressure ? "true" : "false",
			(unsigned long long)fill.full_drops,
			(unsigned long long)ps->output.pressure_drops,
			(unsigned long long)fill.writes,
			(unsigned long long)fill.oversize_drops,
			vcfg->outgoing.backpressure ? "true" : "false",
			(unsigned)vcfg->outgoing.high_water_pct,
			(unsigned)vcfg->outgoing.low_water_pct,
			(unsigned)fill.slot_count,
			(unsigned)fill.used_slots);
	} else if ((ps->output.transport == VENC_OUTPUT_URI_UNIX ||
	            ps->output.transport == VENC_OUTPUT_URI_UDP) &&
	           ps->output.socket_handle >= 0) {
		uint8_t fill_pct = 0;
		if (output_socket_get_fill_pct(ps->output.socket_handle,
		    ps->output.send_buf_capacity, &fill_pct) != 0)
			fill_pct = 0;
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"active\":true,"
			"\"transport\":\"%s\","
			"\"fillPct\":%u,"
			"\"inPressure\":%s,"
			"\"pressureDrops\":%llu,"
			"\"backpressure\":%s,"
			"\"highWaterPct\":%u,"
			"\"lowWaterPct\":%u}}",
			transport,
			(unsigned)fill_pct,
			ps->output.in_pressure ? "true" : "false",
			(unsigned long long)ps->output.pressure_drops,
			vcfg->outgoing.backpressure ? "true" : "false",
			(unsigned)vcfg->outgoing.high_water_pct,
			(unsigned)vcfg->outgoing.low_water_pct);
	} else {
		/* No transport configured — active=false, just surface the
		 * cfg state so consumers can read defaults. */
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"active\":false,"
			"\"transport\":\"%s\","
			"\"backpressure\":%s,"
			"\"highWaterPct\":%u,"
			"\"lowWaterPct\":%u}}",
			transport,
			vcfg->outgoing.backpressure ? "true" : "false",
			(unsigned)vcfg->outgoing.high_water_pct,
			(unsigned)vcfg->outgoing.low_water_pct);
	}
	if (pos < 0 || pos >= (int)sizeof(buf))
		return NULL;
	return strdup(buf);
}

static const VencApplyCallbacks g_star6e_apply_callbacks = {
	.apply_bitrate = apply_bitrate,
	.apply_fps = apply_fps,
	.apply_gop = apply_gop,
	.apply_qp_delta = apply_qp_delta,
	.apply_roi_qp = apply_roi_qp,
	.apply_gain_max = apply_gain_max,
	.apply_verbose = apply_verbose,
	.apply_output_enabled = apply_output_enabled,
	.apply_server = apply_server,
	.apply_mute = apply_mute,
	.request_idr = request_idr,
	.query_live_fps = query_live_fps,
	.query_ae_info = query_ae_info,
	.query_awb_info = query_awb_info,
	.query_isp_metrics = query_isp_metrics,
	.apply_awb_mode = apply_awb_mode,
	.query_iq_info = star6e_iq_query,
	.apply_iq_param = star6e_iq_set,
	.apply_max_payload_size = apply_max_payload_size,
	.query_transport_status = query_transport_status,
};

void star6e_controls_bind(Star6ePipelineState *pipeline, VencConfig *vcfg)
{
	memset(&g_star6e_control_ctx, 0, sizeof(g_star6e_control_ctx));
	if (!pipeline || !vcfg)
		return;

	g_star6e_control_ctx.venc_chn = pipeline->venc_channel;
	g_star6e_control_ctx.vpe_port = pipeline->vpe_port;
	g_star6e_control_ctx.venc_port = pipeline->venc_port;
	g_star6e_control_ctx.sensor_fps = pipeline->sensor.mode.maxFps ?
		pipeline->sensor.mode.maxFps : pipeline->video.sensor_framerate;
	g_star6e_control_ctx.frame_width = pipeline->image_width;
	g_star6e_control_ctx.frame_height = pipeline->image_height;
	g_star6e_control_ctx.pipeline = pipeline;
	g_star6e_control_ctx.vcfg = vcfg;
}

void star6e_controls_reset(void)
{
	memset(&g_star6e_control_ctx, 0, sizeof(g_star6e_control_ctx));
}

const VencApplyCallbacks *star6e_controls_callbacks(void)
{
	return &g_star6e_apply_callbacks;
}

int star6e_controls_apply_fps(uint32_t fps)
{
	return apply_fps(fps);
}

int star6e_controls_apply_roi_qp(int qp)
{
	return apply_roi_qp(qp);
}

int star6e_controls_apply_qp_delta(int delta)
{
	return apply_qp_delta(delta);
}
