#include "maruko_controls.h"

#include "idr_rate_limit.h"
#include "maruko_bindings.h"
#include "maruko_iq.h"
#include "maruko_output.h"
#include "pipeline_common.h"
#include "venc_config.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── ISP type definitions (match SigmaStar SDK ABI) ──────────────────── */

typedef struct {
	unsigned int minShutterUs;
	unsigned int maxShutterUs;
	unsigned int minApertX10;
	unsigned int maxApertX10;
	unsigned int minSensorGain;
	unsigned int minIspGain;
	unsigned int maxSensorGain;
	unsigned int maxIspGain;
} MarukoIspExposureLimit;

typedef struct {
	uint32_t u32FNx10;
	uint32_t u32SensorGain;
	uint32_t u32ISPGain;
	uint32_t u32US;
} MarukoAeExpoValue;

typedef struct {
	uint32_t u32LumY;
	uint32_t u32AvgY;
	uint32_t u32Hits[128];
} MarukoAeHistWeightY;

typedef struct {
	int bIsStable;
	int bIsReachBoundary;
	MarukoAeExpoValue stExpoValueLong;
	MarukoAeExpoValue stExpoValueShort;
	MarukoAeHistWeightY stHistWeightY;
	uint32_t u32LVx10;
	int32_t s32BV;
	uint32_t u32SceneTarget;
} MarukoAeExpoInfo;

typedef struct {
	int plane_ret;
	int limit_ret;
	int info_ret;
	int state_ret;
	int mode_ret;
	MI_SNR_PAD_ID_e pad_id;
	MI_SNR_PlaneInfo_t plane;
	MarukoIspExposureLimit limit;
	MarukoAeExpoInfo info;
	int ae_state;
	int ae_mode_raw;
} MarukoAeDiagSnapshot;

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
} MarukoAwbQueryInfo;

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
	/* i6c adds sync 3A fields here — pad to avoid buffer overrun */
	uint8_t _sync3a_pad[32];
} __attribute__((packed, aligned(1))) MarukoAwbCus3aInfo;

typedef struct {
	int bEnable;
	uint16_t u16GlbGainThd;
	uint16_t u16CountThd;
	uint16_t u16ForceTriGainThd;
} MarukoAwbStabilizer;

typedef struct {
	uint32_t u32CT;
} MarukoAwbCtMwb;

typedef struct {
	int eState;
	int eOpType;
	uint16_t mwb_rgain;
	uint16_t mwb_grgain;
	uint16_t mwb_gbgain;
	uint16_t mwb_bgain;
	uint8_t _pad[8192];
} MarukoAwbAttr;

enum {
	MARUKO_AWB_MODE_AUTO = 0,
	MARUKO_AWB_MODE_MANUAL = 1,
	MARUKO_AWB_MODE_CT_MANUAL = 2,
};

enum {
	MARUKO_ISP_STATE_NORMAL = 0,
	MARUKO_ISP_STATE_PAUSE = 1,
	MARUKO_AE_EXPO_MODE_AUTO = 0,
};

/* ── Control context ─────────────────────────────────────────────────── */

typedef struct {
	MI_VENC_DEV venc_dev;
	MI_VENC_CHN venc_chn;
	int *verbose_ptr;
	MI_SNR_PAD_ID_e pad_id;
	uint32_t sensor_fps;
	uint32_t frame_width;
	uint32_t frame_height;
	VencConfig *vcfg;
	MarukoBackendConfig *backend_cfg;
	MarukoBackendContext *backend;   /* full backend handle for output/ring access */
	MI_SYS_ChnPort_t vpe_port;
	MI_SYS_ChnPort_t venc_port;
	volatile sig_atomic_t *output_enabled_ptr;
	volatile uint32_t *stored_fps_ptr;
} MarukoControlContext;

enum {
	MARUKO_CONTROLS_IDLE_FPS = 5,
};

static MarukoControlContext g_ctx;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static uint32_t align_down(uint32_t value, uint32_t align)
{
	return value / align * align;
}

static int maruko_apply_rc_qp_delta(const i6c_venc_chn *attr, MI_VENC_RcParam_t *param,
	int delta)
{
	if (!attr || !param || delta < -12 || delta > 12)
		return -1;

	switch (attr->rate.mode) {
	case MARUKO_VENC_RC_H265_CBR:
		param->stParamH265Cbr.s32IPQPDelta = delta;
		return 0;
	case MARUKO_VENC_RC_H264_CBR:
		param->stParamH264Cbr.s32IPQPDelta = delta;
		return 0;
	case MARUKO_VENC_RC_H265_VBR:
		param->stParamH265Vbr.s32IPQPDelta = delta;
		return 0;
	case MARUKO_VENC_RC_H264_VBR:
		param->stParamH264VBR.s32IPQPDelta = delta;
		return 0;
	case MARUKO_VENC_RC_H265_AVBR:
		param->stParamH265Avbr.s32IPQPDelta = delta;
		return 0;
	case MARUKO_VENC_RC_H264_AVBR:
		param->stParamH264Avbr.s32IPQPDelta = delta;
		return 0;
	default:
		return -1;
	}
}

/* ── Basic controls (existing) ───────────────────────────────────────── */

static int maruko_apply_bitrate(uint32_t kbps)
{
	i6c_venc_chn attr = {0};
	if (maruko_mi_venc_get_chn_attr(g_ctx.venc_dev,
	    g_ctx.venc_chn, &attr) != 0)
		return -1;
	unsigned int bits = kbps * 1024;
	switch (attr.rate.mode) {
	case MARUKO_VENC_RC_H265_CBR:
		attr.rate.h265Cbr.bitrate = bits; break;
	case MARUKO_VENC_RC_H264_CBR:
		attr.rate.h264Cbr.bitrate = bits; break;
	case MARUKO_VENC_RC_H265_VBR:
		attr.rate.h265Vbr.maxBitrate = bits; break;
	case MARUKO_VENC_RC_H264_VBR:
		attr.rate.h264Vbr.maxBitrate = bits; break;
	case MARUKO_VENC_RC_H265_AVBR:
		attr.rate.h265Avbr.maxBitrate = bits; break;
	case MARUKO_VENC_RC_H264_AVBR:
		attr.rate.h264Avbr.maxBitrate = bits; break;
	default:
		return -1;
	}
	if (maruko_mi_venc_set_chn_attr(g_ctx.venc_dev,
	    g_ctx.venc_chn, &attr) != 0)
		return -1;
	/* Force an IDR after a bitrate change so the decoder resyncs against
	 * the new rate-control state.  Rate-limit gated to coalesce storms;
	 * see the matching note in src/star6e_controls.c apply_bitrate(). */
	if (idr_rate_limit_allow(g_ctx.venc_chn))
		maruko_mi_venc_request_idr(g_ctx.venc_dev,
			g_ctx.venc_chn, 1);
	return 0;
}

static int maruko_apply_gop(uint32_t gop_size)
{
	i6c_venc_chn attr = {0};
	if (maruko_mi_venc_get_chn_attr(g_ctx.venc_dev,
	    g_ctx.venc_chn, &attr) != 0)
		return -1;
	switch (attr.rate.mode) {
	case MARUKO_VENC_RC_H265_CBR: attr.rate.h265Cbr.gop = gop_size; break;
	case MARUKO_VENC_RC_H264_CBR: attr.rate.h264Cbr.gop = gop_size; break;
	case MARUKO_VENC_RC_H265_VBR: attr.rate.h265Vbr.gop = gop_size; break;
	case MARUKO_VENC_RC_H264_VBR: attr.rate.h264Vbr.gop = gop_size; break;
	case MARUKO_VENC_RC_H265_AVBR: attr.rate.h265Avbr.gop = gop_size; break;
	case MARUKO_VENC_RC_H264_AVBR: attr.rate.h264Avbr.gop = gop_size; break;
	default: return -1;
	}
	return maruko_mi_venc_set_chn_attr(g_ctx.venc_dev,
		g_ctx.venc_chn, &attr) == 0 ? 0 : -1;
}

static int maruko_apply_qp_delta(int delta)
{
	i6c_venc_chn attr = {0};
	MI_VENC_RcParam_t param = {0};

	if (maruko_mi_venc_get_chn_attr(g_ctx.venc_dev,
	    g_ctx.venc_chn, &attr) != 0)
		return -1;
	if (maruko_mi_venc_get_rc_param(g_ctx.venc_dev,
	    g_ctx.venc_chn, &param) != 0)
		return -1;
	if (maruko_apply_rc_qp_delta(&attr, &param, delta) != 0)
		return -1;
	if (maruko_mi_venc_set_rc_param(g_ctx.venc_dev,
	    g_ctx.venc_chn, &param) != 0)
		return -1;

	if (idr_rate_limit_allow(g_ctx.venc_chn))
		maruko_mi_venc_request_idr(g_ctx.venc_dev, g_ctx.venc_chn, 1);
	printf("> qpDelta changed to %d\n", delta);
	return 0;
}

static int maruko_apply_fps(uint32_t fps)
{
	i6c_venc_chn attr = {0};
	MI_S32 bind_ret;
	uint32_t sensor_fps;

	if (fps == 0 || fps > 120)
		return -1;

	sensor_fps = g_ctx.sensor_fps;
	if (fps > sensor_fps) {
		printf("> FPS %u exceeds sensor mode max %u, clamping\n", fps,
			sensor_fps);
		fps = sensor_fps;
	}

	MI_SYS_UnBindChnPort(&g_ctx.vpe_port, &g_ctx.venc_port);
	bind_ret = MI_SYS_BindChnPort2(&g_ctx.vpe_port,
		&g_ctx.venc_port, sensor_fps, fps,
		I6_SYS_LINK_RING, 0);
	if (bind_ret != 0) {
		printf("> Rebind SCL->VENC at %u:%u fps failed %d, restoring\n",
			sensor_fps, fps, bind_ret);
		MI_SYS_BindChnPort2(&g_ctx.vpe_port,
			&g_ctx.venc_port, sensor_fps, sensor_fps,
			I6_SYS_LINK_RING, 0);
		return -1;
	}

	if (maruko_mi_venc_get_chn_attr(g_ctx.venc_dev,
	    g_ctx.venc_chn, &attr) == 0) {
		switch (attr.rate.mode) {
		case MARUKO_VENC_RC_H265_CBR:
			attr.rate.h265Cbr.fpsNum = fps; break;
		case MARUKO_VENC_RC_H264_CBR:
			attr.rate.h264Cbr.fpsNum = fps; break;
		case MARUKO_VENC_RC_H265_VBR:
			attr.rate.h265Vbr.fpsNum = fps; break;
		case MARUKO_VENC_RC_H264_VBR:
			attr.rate.h264Vbr.fpsNum = fps; break;
		case MARUKO_VENC_RC_H265_AVBR:
			attr.rate.h265Avbr.fpsNum = fps; break;
		case MARUKO_VENC_RC_H264_AVBR:
			attr.rate.h264Avbr.fpsNum = fps; break;
		default:
			break;
		}
		maruko_mi_venc_set_chn_attr(g_ctx.venc_dev,
			g_ctx.venc_chn, &attr);
	}

	printf("> FPS changed to %u (bind %u:%u)\n", fps, sensor_fps, fps);
	return 0;
}

static int maruko_apply_verbose(bool on)
{
	if (g_ctx.verbose_ptr)
		*g_ctx.verbose_ptr = on ? 1 : 0;
	printf("> Verbose %s via API\n", on ? "enabled" : "disabled");
	return 0;
}

static int maruko_request_idr(void)
{
	return maruko_mi_venc_request_idr(g_ctx.venc_dev,
		g_ctx.venc_chn, 1) == 0 ? 0 : -1;
}

static uint32_t maruko_query_live_fps(void)
{
	i6c_venc_chn attr = {0};

	if (maruko_mi_venc_get_chn_attr(g_ctx.venc_dev,
	    g_ctx.venc_chn, &attr) != 0)
		return 0;

	switch (attr.rate.mode) {
	case MARUKO_VENC_RC_H265_CBR: return attr.rate.h265Cbr.fpsNum;
	case MARUKO_VENC_RC_H264_CBR: return attr.rate.h264Cbr.fpsNum;
	case MARUKO_VENC_RC_H265_VBR: return attr.rate.h265Vbr.fpsNum;
	case MARUKO_VENC_RC_H264_VBR: return attr.rate.h264Vbr.fpsNum;
	case MARUKO_VENC_RC_H265_AVBR: return attr.rate.h265Avbr.fpsNum;
	case MARUKO_VENC_RC_H264_AVBR: return attr.rate.h264Avbr.fpsNum;
	default: return 0;
	}
}

/* ── AE diagnostics (Step 2A) ────────────────────────────────────────── */

static const char *ae_state_name(int state)
{
	return state == MARUKO_ISP_STATE_PAUSE ? "pause" : "normal";
}

static const char *ae_expo_mode_name(int mode)
{
	return mode == MARUKO_AE_EXPO_MODE_AUTO ? "auto" : "unknown";
}

static void ae_diag_defaults(MarukoAeDiagSnapshot *s)
{
	memset(s, 0, sizeof(*s));
	s->pad_id = g_ctx.pad_id;
	s->limit_ret = -1;
	s->info_ret = -1;
	s->state_ret = -1;
	s->mode_ret = -1;
	s->ae_state = MARUKO_ISP_STATE_NORMAL;
	s->ae_mode_raw = MARUKO_AE_EXPO_MODE_AUTO;
}

static void ae_diag_collect(MarukoAeDiagSnapshot *s)
{
	/* i6c ISP functions take (dev, channel, data*) */
	typedef MI_S32 (*fn_get_limit_t)(uint32_t, uint32_t, MarukoIspExposureLimit *);
	typedef MI_S32 (*fn_query_info_t)(uint32_t, uint32_t, MarukoAeExpoInfo *);
	typedef MI_S32 (*fn_get_state_t)(uint32_t, uint32_t, int *);
	typedef MI_S32 (*fn_get_mode_t)(uint32_t, uint32_t, int *);
	void *handle;

	ae_diag_defaults(s);
	s->plane_ret = MI_SNR_GetPlaneInfo(s->pad_id, 0, &s->plane);

	handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!handle)
		return;

	{
		fn_get_limit_t fn = (fn_get_limit_t)dlsym(handle,
			"MI_ISP_AE_GetExposureLimit");
		if (fn)
			s->limit_ret = fn(0, 0, &s->limit);
	}
	{
		fn_query_info_t fn = (fn_query_info_t)dlsym(handle,
			"MI_ISP_AE_QueryExposureInfo");
		if (fn)
			s->info_ret = fn(0, 0, &s->info);
	}
	{
		fn_get_state_t fn = (fn_get_state_t)dlsym(handle,
			"MI_ISP_AE_GetState");
		if (fn)
			s->state_ret = fn(0, 0, &s->ae_state);
	}
	{
		fn_get_mode_t fn = (fn_get_mode_t)dlsym(handle,
			"MI_ISP_AE_GetExpoMode");
		if (fn)
			s->mode_ret = fn(0, 0, &s->ae_mode_raw);
	}

	dlclose(handle);
}

static uint32_t ae_diag_exposure_us(const MarukoAeDiagSnapshot *s)
{
	if (s->info_ret == 0)
		return s->info.stExpoValueLong.u32US;
	return s->plane.shutter;
}

static uint32_t ae_diag_sensor_gain(const MarukoAeDiagSnapshot *s)
{
	if (s->info_ret == 0)
		return s->info.stExpoValueLong.u32SensorGain;
	return s->plane.sensGain;
}

static uint32_t ae_diag_isp_gain(const MarukoAeDiagSnapshot *s)
{
	if (s->info_ret == 0)
		return s->info.stExpoValueLong.u32ISPGain;
	return s->plane.compGain;
}

static uint32_t ae_diag_sensor_fps(void)
{
	if (g_ctx.sensor_fps != 0)
		return g_ctx.sensor_fps;
	if (g_ctx.vcfg)
		return g_ctx.vcfg->video0.fps;
	return 0;
}

static char *maruko_query_ae_info(void)
{
	MarukoAeDiagSnapshot s;
	char buf[2048];
	uint32_t exposure_us, sensor_gain, isp_gain;

	ae_diag_collect(&s);
	exposure_us = ae_diag_exposure_us(&s);
	sensor_gain = ae_diag_sensor_gain(&s);
	isp_gain = ae_diag_isp_gain(&s);

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
		"\"runtime\":{\"sensor_fps\":%u}}}",
		s.plane_ret, s.pad_id, s.plane.shutter,
		s.plane.sensGain, s.plane.compGain,
		s.limit_ret, s.limit.minShutterUs, s.limit.maxShutterUs,
		s.limit.minSensorGain, s.limit.maxSensorGain,
		s.limit.minIspGain, s.limit.maxIspGain,
		s.info_ret,
		s.info_ret == 0 && s.info.bIsStable ? "true" : "false",
		s.info_ret == 0 && s.info.bIsReachBoundary ? "true" : "false",
		s.info.stExpoValueLong.u32US,
		s.info.stExpoValueLong.u32SensorGain,
		s.info.stExpoValueLong.u32ISPGain,
		s.info.stHistWeightY.u32LumY, s.info.stHistWeightY.u32AvgY,
		s.info.u32LVx10, s.info.s32BV, s.info.u32SceneTarget,
		s.state_ret, s.ae_state, ae_state_name(s.ae_state),
		s.mode_ret, s.ae_mode_raw, ae_expo_mode_name(s.ae_mode_raw),
		exposure_us, sensor_gain, isp_gain, ae_diag_sensor_fps(),
		ae_diag_sensor_fps());
	return strdup(buf);
}

static char *maruko_query_isp_metrics(void)
{
	MarukoAeDiagSnapshot s;
	char buf[512];

	ae_diag_collect(&s);
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
		ae_diag_sensor_gain(&s),
		ae_diag_isp_gain(&s),
		ae_diag_exposure_us(&s) / 1000,
		ae_diag_sensor_fps(),
		s.info.stHistWeightY.u32LumY,
		s.info.stHistWeightY.u32AvgY,
		s.info_ret == 0 && s.info.bIsStable ? 1u : 0u);
	return strdup(buf);
}

/* ── AWB diagnostics and control (Steps 2A, 2C) ─────────────────────── */

static char *maruko_query_awb_info(void)
{
	/* i6c ISP functions take (dev, channel, data*) */
	typedef MI_S32 (*fn_query_t)(uint32_t, uint32_t, MarukoAwbQueryInfo *);
	typedef MI_S32 (*fn_cus3a_status_t)(uint32_t, uint32_t, MarukoAwbCus3aInfo *);
	typedef MI_S32 (*fn_stab_t)(uint32_t, uint32_t, MarukoAwbStabilizer *);
	typedef MI_S32 (*fn_attr_t)(uint32_t, uint32_t, MarukoAwbAttr *);
	typedef MI_S32 (*fn_ctmwb_get_t)(uint32_t, uint32_t, MarukoAwbCtMwb *);
	void *handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	char buf[2048];
	int pos = 0;

	if (!handle)
		return NULL;

	pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"ok\":true,\"data\":{");

	{
		fn_query_t fn = (fn_query_t)dlsym(handle, "MI_ISP_AWB_QueryInfo");
		if (fn) {
			MarukoAwbQueryInfo qi;
			MI_S32 ret;
			memset(&qi, 0, sizeof(qi));
			ret = fn(0, 0, &qi);
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
		fn_cus3a_status_t fn = (fn_cus3a_status_t)dlsym(handle,
			"MI_ISP_CUS3A_GetAwbStatus");
		if (fn) {
			MarukoAwbCus3aInfo ci;
			MI_S32 ret;
			memset(&ci, 0, sizeof(ci));
			ci.Size = sizeof(ci);
			ret = fn(0, 0, &ci);
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
		fn_stab_t fn = (fn_stab_t)dlsym(handle, "MI_ISP_AWB_GetStabilizer");
		if (fn) {
			MarukoAwbStabilizer st;
			MI_S32 ret;
			memset(&st, 0, sizeof(st));
			ret = fn(0, 0, &st);
			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"stabilizer\":{\"ret\":%d,\"enabled\":%s,"
				"\"glb_gain_thd\":%u,\"count_thd\":%u,\"force_tri_thd\":%u},",
				ret, st.bEnable ? "true" : "false", st.u16GlbGainThd,
				st.u16CountThd, st.u16ForceTriGainThd);
		}
	}

	{
		fn_attr_t fn = (fn_attr_t)dlsym(handle, "MI_ISP_AWB_GetAttr");
		if (fn) {
			MarukoAwbAttr attr;
			MI_S32 ret;
			const char *mode_str = "unknown";
			memset(&attr, 0, sizeof(attr));
			ret = fn(0, 0, &attr);
			if (attr.eOpType == MARUKO_AWB_MODE_AUTO)
				mode_str = "auto";
			else if (attr.eOpType == MARUKO_AWB_MODE_MANUAL)
				mode_str = "manual";
			else if (attr.eOpType == MARUKO_AWB_MODE_CT_MANUAL)
				mode_str = "ct_manual";
			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"attr\":{\"ret\":%d,\"state\":%d,\"mode\":\"%s\",\"mode_raw\":%d,"
				"\"mwb_r\":%u,\"mwb_gr\":%u,\"mwb_gb\":%u,\"mwb_b\":%u},",
				ret, attr.eState, mode_str, attr.eOpType, attr.mwb_rgain,
				attr.mwb_grgain, attr.mwb_gbgain, attr.mwb_bgain);
		}
	}

	{
		fn_ctmwb_get_t fn = (fn_ctmwb_get_t)dlsym(handle,
			"MI_ISP_AWB_GetCTMwbAttr");
		if (!fn)
			fn = (fn_ctmwb_get_t)dlsym(handle,
				"MI_ISP_AWB_GetCtMwbAttr");
		if (fn) {
			MarukoAwbCtMwb mwb;
			MI_S32 ret;
			memset(&mwb, 0, sizeof(mwb));
			ret = fn(0, 0, &mwb);
			pos += snprintf(buf + pos, sizeof(buf) - pos,
				"\"ct_mwb\":{\"ret\":%d,\"ct\":%u}", ret, mwb.u32CT);
		}
	}

	pos += snprintf(buf + pos, sizeof(buf) - pos, "}}");
	dlclose(handle);
	return strdup(buf);
}

/* Temporarily disable CUS3A AWB to allow MI_ISP_AWB_SetAttr on i6c.
 * The ISP rejects attr changes while CUS3A AWB is active. */
static void cus3a_awb_set(void *handle, int enable)
{
	typedef int (*fn_t)(MI_U32, MI_U32, void *);
	fn_t fn = (fn_t)dlsym(handle, "MI_ISP_CUS3A_Enable");
	if (fn) {
		MI_BOOL p[3] = {1, enable ? 1 : 0, 1};
		fn(0, 0, p);
	}
}

static int maruko_apply_awb_mode(int mode, uint32_t ct)
{
	/* i6c ISP functions take (dev, channel, data*) */
	typedef MI_S32 (*fn_get_t)(uint32_t, uint32_t, MarukoAwbAttr *);
	typedef MI_S32 (*fn_set_t)(uint32_t, uint32_t, MarukoAwbAttr *);
	typedef MI_S32 (*fn_ctmwb_t)(uint32_t, uint32_t, MarukoAwbCtMwb *);
	void *handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	int ret = 0;

	if (!handle)
		return -1;

	/* Disable CUS3A AWB before changing attributes */
	cus3a_awb_set(handle, 0);

	if (mode == 0) {
		fn_get_t fn_get = (fn_get_t)dlsym(handle, "MI_ISP_AWB_GetAttr");
		fn_set_t fn_set = (fn_set_t)dlsym(handle, "MI_ISP_AWB_SetAttr");

		if (fn_get && fn_set) {
			MarukoAwbAttr attr;
			memset(&attr, 0, sizeof(attr));
			if (fn_get(0, 0, &attr) == 0) {
				MI_S32 awb_ret;
				attr.eState = 0;
				attr.eOpType = MARUKO_AWB_MODE_AUTO;
				awb_ret = fn_set(0, 0, &attr);
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
		if (!fn_ctmwb)
			fn_ctmwb = (fn_ctmwb_t)dlsym(handle,
				"MI_ISP_AWB_SetCtMwbAttr");
		fn_get_t fn_get = (fn_get_t)dlsym(handle, "MI_ISP_AWB_GetAttr");
		fn_set_t fn_set = (fn_set_t)dlsym(handle, "MI_ISP_AWB_SetAttr");

		if (fn_ctmwb && fn_get && fn_set) {
			MarukoAwbCtMwb mwb = { .u32CT = ct };
			MI_S32 awb_ret = fn_ctmwb(0, 0, &mwb);

			if (awb_ret != 0) {
				fprintf(stderr, "WARNING: MI_ISP_AWB_SetCTMwbAttr(%u) failed: 0x%08x\n",
					ct, (unsigned)awb_ret);
				ret = -1;
			} else {
				MarukoAwbAttr attr;
				memset(&attr, 0, sizeof(attr));
				if (fn_get(0, 0, &attr) == 0) {
					attr.eState = 0;
					attr.eOpType = MARUKO_AWB_MODE_CT_MANUAL;
					awb_ret = fn_set(0, 0, &attr);
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

	/* Re-enable CUS3A AWB */
	cus3a_awb_set(handle, 1);

	dlclose(handle);
	return ret;
}

static int maruko_apply_gain_max(uint32_t gain)
{
	/* Set max sensor gain via SetExposureLimit. */
	typedef int (*ae_get_fn)(uint32_t, uint32_t, MarukoIspExposureLimit *);
	typedef int (*ae_set_fn)(uint32_t, uint32_t, MarukoIspExposureLimit *);
	void *h = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h) return -1;

	ae_get_fn fn_get = (ae_get_fn)dlsym(h, "MI_ISP_AE_GetExposureLimit");
	ae_set_fn fn_set = (ae_set_fn)dlsym(h, "MI_ISP_AE_SetExposureLimit");
	if (!fn_get || !fn_set) { dlclose(h); return -1; }

	MarukoIspExposureLimit limit = {0};
	int ret = fn_get(0, 0, &limit);
	if (ret != 0) { dlclose(h); return ret; }

	printf("> [maruko] Gain max: %u -> %u\n",
		limit.maxSensorGain, gain);
	limit.maxSensorGain = gain;
	ret = fn_set(0, 0, &limit);
	dlclose(h);
	return ret;
}

/* ── ROI horizontal bands ────────────────────────────────────────────── */

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


static int maruko_apply_roi_qp(int qp)
{
	uint32_t width = g_ctx.frame_width;
	uint32_t height = g_ctx.frame_height;
	int ok = 1;
	uint16_t steps;
	float center_frac;

	if (width == 0 || height == 0)
		return -1;

	for (int i = 0; i < PIPELINE_ROI_MAX_STEPS; i++) {
		MI_VENC_RoiCfg_t roi = {0};
		roi.u32Index = i;
		roi.bEnable = 0;
		MI_VENC_SetRoiCfg(g_ctx.venc_chn, &roi);
	}

	if (!g_ctx.vcfg || !g_ctx.vcfg->fpv.roi_enabled || qp == 0) {
		printf("> ROI disabled (all regions cleared)\n");
		return 0;
	}

	if (qp < -30) qp = -30;
	if (qp > 30) qp = 30;

	steps = g_ctx.vcfg->fpv.roi_steps;
	if (steps < 1) steps = 1;
	if (steps > PIPELINE_ROI_MAX_STEPS) steps = PIPELINE_ROI_MAX_STEPS;

	center_frac = (float)g_ctx.vcfg->fpv.roi_center;
	if (center_frac < 0.1f) center_frac = 0.1f;
	if (center_frac > 0.9f) center_frac = 0.9f;

	for (int i = 0; i < steps; i++) {
		MI_VENC_RoiCfg_t roi = {0};
		MI_S32 ret;

		if (compute_horizontal_roi(width, height, center_frac, qp,
		    steps, i, &roi) != 0)
			continue;

		ret = MI_VENC_SetRoiCfg(g_ctx.venc_chn, &roi);
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

/* ── Output enable/disable (Step 3B) ─────────────────────────────────── */

static int maruko_apply_output_enabled(bool on)
{
	uint32_t restored_fps;

	if (!g_ctx.output_enabled_ptr)
		return -1;

	if (on) {
		if (!g_ctx.vcfg || !g_ctx.vcfg->outgoing.server[0]) {
			fprintf(stderr, "> Cannot enable output: no server configured\n");
			return -1;
		}
		*g_ctx.output_enabled_ptr = 1;
		restored_fps = (g_ctx.stored_fps_ptr && *g_ctx.stored_fps_ptr) ?
			*g_ctx.stored_fps_ptr :
			(g_ctx.vcfg ? g_ctx.vcfg->video0.fps : 30);
		maruko_apply_fps(restored_fps);
		maruko_mi_venc_request_idr(g_ctx.venc_dev, g_ctx.venc_chn, 1);
		printf("> Output enabled, FPS restored to %u\n", restored_fps);
	} else {
		*g_ctx.output_enabled_ptr = 0;
		if (g_ctx.stored_fps_ptr)
			*g_ctx.stored_fps_ptr = g_ctx.vcfg ?
				g_ctx.vcfg->video0.fps : 30;
		maruko_apply_fps(MARUKO_CONTROLS_IDLE_FPS);
		printf("> Output disabled, FPS reduced to %u (idle)\n",
			MARUKO_CONTROLS_IDLE_FPS);
	}

	return 0;
}

/* ── Live server change (Step 3C) ────────────────────────────────────── */

static MarukoOutput *g_maruko_output_ptr;

static int maruko_apply_server(const char *uri)
{
	if (!g_maruko_output_ptr)
		return -1;
	if (maruko_output_apply_server(g_maruko_output_ptr, uri) != 0)
		return -1;

	maruko_mi_venc_request_idr(g_ctx.venc_dev, g_ctx.venc_chn, 1);
	printf("> Destination changed to %s\n", uri);
	return 0;
}

static int maruko_apply_max_payload_size(uint16_t size)
{
	if (!g_ctx.backend_cfg)
		return -1;

	/* Validation enforces [VENC_OUTPUT_PAYLOAD_MIN_BYTES,
	 * VENC_OUTPUT_PAYLOAD_CEILING_BYTES] and the SHM ring is sized to
	 * the ceiling at startup, so any value reaching here fits every
	 * transport. Plain uint16_t stores are atomic on ARM; the encoder
	 * thread re-reads cfg->rtp_payload_size / cfg->max_frame_size once
	 * per frame. */
	g_ctx.backend_cfg->rtp_payload_size = size;
	g_ctx.backend_cfg->max_frame_size = size;

	/* stderr (unbuffered) so the live trace lands in the log even when
	 * stdout is buffered or captured by the audio filter. */
	fprintf(stderr, "> max_payload_size set to %u (live)\n", (unsigned)size);
	return 0;
}

static char *maruko_query_shm_status(void)
{
	MarukoBackendContext *backend = g_ctx.backend;
	const VencConfig *vcfg = g_ctx.vcfg;
	venc_ring_fill_t fill;
	char buf[768];
	int pos;

	if (!backend || !vcfg)
		return NULL;
	if (!backend->output.ring) {
		pos = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{"
			"\"active\":false,"
			"\"backpressure\":%s,"
			"\"highWaterPct\":%u,"
			"\"lowWaterPct\":%u}}",
			vcfg->outgoing.shm_backpressure ? "true" : "false",
			(unsigned)vcfg->outgoing.shm_high_water_pct,
			(unsigned)vcfg->outgoing.shm_low_water_pct);
		if (pos < 0 || pos >= (int)sizeof(buf))
			return NULL;
		return strdup(buf);
	}
	if (venc_ring_get_fill(backend->output.ring, &fill) != 0)
		return NULL;

	pos = snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{"
		"\"active\":true,"
		"\"slotCount\":%u,"
		"\"usedSlots\":%u,"
		"\"fillPct\":%u,"
		"\"writes\":%llu,"
		"\"fullDrops\":%llu,"
		"\"oversizeDrops\":%llu,"
		"\"backpressure\":%s,"
		"\"highWaterPct\":%u,"
		"\"lowWaterPct\":%u,"
		"\"inPressure\":%s,"
		"\"pressureDrops\":%llu}}",
		(unsigned)fill.slot_count,
		(unsigned)fill.used_slots,
		(unsigned)fill.fill_pct,
		(unsigned long long)fill.writes,
		(unsigned long long)fill.full_drops,
		(unsigned long long)fill.oversize_drops,
		vcfg->outgoing.shm_backpressure ? "true" : "false",
		(unsigned)vcfg->outgoing.shm_high_water_pct,
		(unsigned)vcfg->outgoing.shm_low_water_pct,
		backend->output.in_pressure ? "true" : "false",
		(unsigned long long)backend->output.pressure_drops);
	if (pos < 0 || pos >= (int)sizeof(buf))
		return NULL;
	return strdup(buf);
}

/* ── Callback table ──────────────────────────────────────────────────── */

static const VencApplyCallbacks g_maruko_apply_cb = {
	.apply_bitrate = maruko_apply_bitrate,
	.apply_fps = maruko_apply_fps,
	.apply_gop = maruko_apply_gop,
	.apply_qp_delta = maruko_apply_qp_delta,
	.apply_roi_qp = maruko_apply_roi_qp,
	.apply_verbose = maruko_apply_verbose,
	.apply_output_enabled = maruko_apply_output_enabled,
	.apply_server = maruko_apply_server,
	.apply_gain_max = maruko_apply_gain_max,
	.apply_mute = NULL,
	.request_idr = maruko_request_idr,
	.query_live_fps = maruko_query_live_fps,
	.query_ae_info = maruko_query_ae_info,
	.query_awb_info = maruko_query_awb_info,
	.query_isp_metrics = maruko_query_isp_metrics,
	.apply_awb_mode = maruko_apply_awb_mode,
	.query_iq_info = maruko_iq_query,
	.apply_iq_param = maruko_iq_set,
	.apply_max_payload_size = maruko_apply_max_payload_size,
	.query_shm_status = maruko_query_shm_status,
};

void maruko_controls_bind(MarukoBackendContext *backend, VencConfig *vcfg)
{
	memset(&g_ctx, 0, sizeof(g_ctx));
	if (!backend)
		return;

	g_ctx.venc_dev = backend->venc_device;
	g_ctx.venc_chn = backend->venc_channel;
	g_ctx.verbose_ptr = &backend->cfg.verbose;
	g_ctx.pad_id = backend->sensor.pad_id;
	g_ctx.sensor_fps = backend->sensor.fps;
	g_ctx.frame_width = backend->cfg.image_width;
	g_ctx.frame_height = backend->cfg.image_height;
	g_ctx.vcfg = vcfg;
	/* backend_cfg points into MarukoBackendContext, which lives in the
	 * runner context for the entire process lifetime. Reinit re-runs
	 * maruko_config_from_venc and rebinds, so the pointer remains
	 * valid and the snapshot stays in sync with vcfg. */
	g_ctx.backend_cfg = &backend->cfg;
	g_ctx.backend = backend;
	g_ctx.vpe_port = backend->vpe_port;
	g_ctx.venc_port = backend->venc_port;
	g_ctx.output_enabled_ptr = &backend->output_enabled;
	g_ctx.stored_fps_ptr = &backend->stored_fps;
	g_maruko_output_ptr = &backend->output;
}

const VencApplyCallbacks *maruko_controls_callbacks(void)
{
	return &g_maruko_apply_cb;
}

const VencConfig *maruko_controls_vcfg(void)
{
	return g_ctx.vcfg;
}
