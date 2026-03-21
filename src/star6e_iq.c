/*
 * star6e_iq.c — Direct IQ parameter interface for SigmaStar ISP
 *
 * Exposes MI_ISP_IQ_* functions as HTTP-queryable/settable parameters.
 * Uses cached dlopen handle (opened once at init, closed at cleanup).
 *
 * Struct layouts from SDK mi_isp_iq_datatype.h (pudding/infinity6e):
 *
 *   Standard auto/manual (manual_offset > 4):
 *     { bEnable(4), enOpType(4), stAuto(N*16), stManual(...) }
 *
 *   Manual-only (manual_offset == 4):
 *     { bEnable(4), stManual(...) }
 *
 *   Bool-only (manual_offset == 0):
 *     { bEnable(4) }
 */

#include "star6e_iq.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int MI_S32;
typedef MI_S32 (*iq_fn_t)(uint32_t channel, void *param);

#define IQ_OFFSET_ENABLE   0
#define IQ_OFFSET_OPTYPE   4
#define IQ_BUF_SIZE        32768  /* must fit largest struct: DUMMY ~26KB */

typedef enum {
	VT_BOOL,    /* just bEnable (color_to_gray, defog) */
	VT_U8,      /* uint8_t */
	VT_U16,     /* uint16_t */
	VT_U32,     /* uint32_t */
} IqValueType;

typedef struct {
	const char *name;
	const char *get_sym;
	const char *set_sym;
	iq_fn_t     fn_get;
	iq_fn_t     fn_set;
	IqValueType vtype;
	uint32_t    manual_offset;  /* 0=bool-only, 4=manual-only, >4=auto/manual */
	uint32_t    max_val;
} IqParamDesc;

/*
 * Parameter table — offsets computed from SDK structs:
 *   auto/manual: manual_offset = 8 + (16 * sizeof(per_iso_param))
 *   manual-only: manual_offset = 4
 *   bool-only:   manual_offset = 0
 */
static IqParamDesc g_params[] = {
	/* ── Image quality ─────────────────────────────────────────── */
	/* LEVEL_BASE_PARAM_t = 4B, auto=64, manual@72 */
	{ "lightness",    "MI_ISP_IQ_GetLightness",   "MI_ISP_IQ_SetLightness",
	  NULL, NULL, VT_U32, 72,   100 },
	{ "contrast",     "MI_ISP_IQ_GetContrast",    "MI_ISP_IQ_SetContrast",
	  NULL, NULL, VT_U32, 72,   100 },
	{ "brightness",   "MI_ISP_IQ_GetBrightness",  "MI_ISP_IQ_SetBrightness",
	  NULL, NULL, VT_U32, 72,   100 },
	/* SATURATION_PARAM_t = 24B, auto=384, manual@392 (u8SatAllStr 0-127, 32=1X) */
	{ "saturation",   "MI_ISP_IQ_GetSaturation",  "MI_ISP_IQ_SetSaturation",
	  NULL, NULL, VT_U8,  392,  127 },
	/* SHARPNESS_PARAM_t = 74B, auto=1184, manual@1192 (u8OverShootGain) */
	{ "sharpness",    "MI_ISP_IQ_GetSharpness",   "MI_ISP_IQ_SetSharpness",
	  NULL, NULL, VT_U8,  1192, 255 },
	/* HSV_PARAM_t = 193B, auto=3088, manual@3096 (s16HueLut[0]) */
	{ "hsv",          "MI_ISP_IQ_GetHSV",          "MI_ISP_IQ_SetHSV",
	  NULL, NULL, VT_U8,  3096, 64 },

	/* ── Noise reduction ───────────────────────────────────────── */
	/* NR3D_PARAM_t = 80B, auto=1280, manual@1288 (u8MdThd) */
	{ "nr3d",         "MI_ISP_IQ_GetNR3D",        "MI_ISP_IQ_SetNR3D",
	  NULL, NULL, VT_U8,  1288, 255 },
	/* NR3D_EX: manual-only@4, 24B (bAREn) */
	{ "nr3d_ex",      "MI_ISP_IQ_GetNR3D_EX",     "MI_ISP_IQ_SetNR3D_EX",
	  NULL, NULL, VT_U32, 4,    1 },
	/* NRDESPIKE_PARAM_t = 34B, auto=544, manual@552 (u8BlendRatio) */
	{ "nr_despike",   "MI_ISP_IQ_GetNRDeSpike",   "MI_ISP_IQ_SetNRDeSpike",
	  NULL, NULL, VT_U8,  552,  15 },
	/* NRLUMA_PARAM_t = 6B, auto=96, manual@104 (u8Strength) */
	{ "nr_luma",      "MI_ISP_IQ_GetNRLuma",      "MI_ISP_IQ_SetNRLuma",
	  NULL, NULL, VT_U8,  104,  255 },
	/* NRLUMA_ADV_PARAM_t = 81B, auto=1296, manual@1304 (bDbgEn+u8Strength@+5) */
	{ "nr_luma_adv",  "MI_ISP_IQ_GetNRLuma_Adv",  "MI_ISP_IQ_SetNRLuma_Adv",
	  NULL, NULL, VT_U32, 1304, 1 },
	/* NRCHROMA_PARAM_t = 13B, auto=208, manual@216 (u8MatchRatio) */
	{ "nr_chroma",    "MI_ISP_IQ_GetNRChroma",    "MI_ISP_IQ_SetNRChroma",
	  NULL, NULL, VT_U8,  216,  127 },
	/* NRCHROMA_ADV_PARAM_t = 30B, auto=480, manual@488 (u8StrengthByY[0]) */
	{ "nr_chroma_adv","MI_ISP_IQ_GetNRChroma_Adv","MI_ISP_IQ_SetNRChroma_Adv",
	  NULL, NULL, VT_U8,  488,  255 },

	/* ── Corrections ───────────────────────────────────────────── */
	/* FALSECOLOR_PARAM_t = 7B, auto=112, manual@120 (u8FreqThrd) */
	{ "false_color",  "MI_ISP_IQ_GetFalseColor",  "MI_ISP_IQ_SetFalseColor",
	  NULL, NULL, VT_U8,  120,  255 },
	/* CROSSTALK_PARAM_t = 18B, auto=288, manual@296 (u8Strength) */
	{ "crosstalk",    "MI_ISP_IQ_GetCrossTalk",   "MI_ISP_IQ_SetCrossTalk",
	  NULL, NULL, VT_U8,  296,  31 },
	/* DEMOSAIC: manual-only@4 (u8DirThrd) */
	{ "demosaic",     "MI_ISP_IQ_GetDEMOSAIC",    "MI_ISP_IQ_SetDEMOSAIC",
	  NULL, NULL, VT_U8,  4,    63 },
	/* OBC_PARAM_t = 8B, auto=128, manual@136 (u16ValR) */
	{ "obc",          "MI_ISP_IQ_GetOBC",          "MI_ISP_IQ_SetOBC",
	  NULL, NULL, VT_U16, 136,  255 },
	/* DYNAMIC_DP_PARAM_t = 30B, auto=480, manual@488 (bHotPixEn) */
	{ "dynamic_dp",   "MI_ISP_IQ_GetDynamicDP",   "MI_ISP_IQ_SetDynamicDP",
	  NULL, NULL, VT_U8,  488,  1 },
	/* DYNAMIC_DP_CLUSTER_PARAM_t = 67B, auto=1072, manual@1080 (bEdgeMode) */
	{ "dp_cluster",   "MI_ISP_IQ_GetDynamicDP_CLUSTER", "MI_ISP_IQ_SetDynamicDP_CLUSTER",
	  NULL, NULL, VT_U32, 1080, 1 },
	/* R2Y: manual-only@4 (u16Matrix[0]) */
	{ "r2y",          "MI_ISP_IQ_GetR2Y",          "MI_ISP_IQ_SetR2Y",
	  NULL, NULL, VT_U16, 4,    1023 },
	/* COLORTRANS: manual-only@4 (u16Y_OFST) */
	{ "colortrans",   "MI_ISP_IQ_GetCOLORTRANS",  "MI_ISP_IQ_SetCOLORTRANS",
	  NULL, NULL, VT_U16, 4,    2047 },
	/* RGBMATRIX_PARAM_t: auto with bISOActEn, manual@CCM offset */
	{ "rgb_matrix",   "MI_ISP_IQ_GetRGBMatrix",   "MI_ISP_IQ_SetRGBMatrix",
	  NULL, NULL, VT_U16, 444,  8191 },

	/* ── Dynamic range & special ───────────────────────────────── */
	/* WDR_PARAM_t = 40B, auto=640, manual@648 (u8BoxNum) */
	{ "wdr",          "MI_ISP_IQ_GetWDR",          "MI_ISP_IQ_SetWDR",
	  NULL, NULL, VT_U8,  648,  4 },
	/* WDRCurveAdv_PARAM_t = 6B, auto=96, manual@104 (u16Slope) */
	{ "wdr_curve_adv","MI_ISP_IQ_GetWDRCurveAdv", "MI_ISP_IQ_SetWDRCurveAdv",
	  NULL, NULL, VT_U16, 104,  16384 },
	/* PFC_PARAM_t = 23B, auto=368, manual@376 (u8Strength) */
	{ "pfc",          "MI_ISP_IQ_GetPFC",          "MI_ISP_IQ_SetPFC",
	  NULL, NULL, VT_U8,  376,  255 },
	/* PFC_EX: manual-only@4 (bDbgEn) */
	{ "pfc_ex",       "MI_ISP_IQ_GetPFC_EX",      "MI_ISP_IQ_SetPFC_EX",
	  NULL, NULL, VT_U32, 4,    1 },
	/* HDR_PARAM_t = 56B, auto=896, manual@904 (bNrEn as MI_ISP_BOOL_e) */
	{ "hdr",          "MI_ISP_IQ_GetHDR",          "MI_ISP_IQ_SetHDR",
	  NULL, NULL, VT_U32, 904,  1 },
	/* HDR_EX: manual-only@4 (u16SensorExpRatio) */
	{ "hdr_ex",       "MI_ISP_IQ_GetHDR_EX",      "MI_ISP_IQ_SetHDR_EX",
	  NULL, NULL, VT_U16, 4,    65535 },
	/* SHP_EX: manual-only@4 (bDbgEn) */
	{ "shp_ex",       "MI_ISP_IQ_GetSHP_EX",      "MI_ISP_IQ_SetSHP_EX",
	  NULL, NULL, VT_U32, 4,    1 },
	/* RGBIR_PARAM_t = 37B, auto=592, manual@600 (u8IrPosType) */
	{ "rgbir",        "MI_ISP_IQ_GetRGBIR",        "MI_ISP_IQ_SetRGBIR",
	  NULL, NULL, VT_U8,  600,  7 },
	/* IQMode: just an enum (0=day, 1=night) */
	{ "iq_mode",      "MI_ISP_IQ_GetIQMode",       "MI_ISP_IQ_SetIQMode",
	  NULL, NULL, VT_U32, 0,    1 },

	/* ── Lens & sensor calibration (flat structs) ─────────────── */
	/* LSC: flat, bEnable(4) + u16CenterX@4 */
	{ "lsc",          "MI_ISP_IQ_GetLSC",          "MI_ISP_IQ_SetLSC",
	  NULL, NULL, VT_U16, 4,    65535 },
	/* LSC_CTRL: flat, bEnable(4) + u8RRatioByCct[0]@4 */
	{ "lsc_ctrl",     "MI_ISP_IQ_GetLSC_CTRL",    "MI_ISP_IQ_SetLSC_CTRL",
	  NULL, NULL, VT_U8,  4,    255 },
	/* ALSC: flat, bEnable(4) + u8GridX@4 */
	{ "alsc",         "MI_ISP_IQ_GetALSC",         "MI_ISP_IQ_SetALSC",
	  NULL, NULL, VT_U8,  4,    255 },
	/* ALSC_CTRL: flat, bEnable(4) + u8RRatioByCct[0]@4 */
	{ "alsc_ctrl",    "MI_ISP_IQ_GetALSC_CTRL",   "MI_ISP_IQ_SetALSC_CTRL",
	  NULL, NULL, VT_U8,  4,    255 },
	/* OBC_P1: same struct as OBC, separate API ID */
	{ "obc_p1",       "MI_ISP_IQ_GetOBC_P1",       "MI_ISP_IQ_SetOBC_P1",
	  NULL, NULL, VT_U16, 136,  255 },
	/* STITCH_LPF: flat, bEnable(4) + u16Coeff[0]@4 */
	{ "stitch_lpf",   "MI_ISP_IQ_GetSTITCH_LPF",  "MI_ISP_IQ_SetSTITCH_LPF",
	  NULL, NULL, VT_U16, 4,    256 },

	/* ── LUT-based (enable/mode control, value = first LUT entry) ─ */
	/* RGBGAMMA: auto/manual, huge LUT struct */
	{ "rgb_gamma",    "MI_ISP_IQ_GetRGBGamma",    "MI_ISP_IQ_SetRGBGamma",
	  NULL, NULL, VT_BOOL, 0,   1 },
	/* YUVGAMMA: auto/manual, huge LUT struct */
	{ "yuv_gamma",    "MI_ISP_IQ_GetYUVGamma",    "MI_ISP_IQ_SetYUVGamma",
	  NULL, NULL, VT_BOOL, 0,   1 },
	/* WDRCurveFull: auto/manual, 256-entry curve */
	{ "wdr_curve_full","MI_ISP_IQ_GetWDRCurveFull","MI_ISP_IQ_SetWDRCurveFull",
	  NULL, NULL, VT_BOOL, 0,   1 },

	/* ── Debug/test ────────────────────────────────────────────── */
	{ "dummy",        "MI_ISP_IQ_GetDUMMY",        "MI_ISP_IQ_SetDUMMY",
	  NULL, NULL, VT_BOOL, 0,   1 },
	{ "dummy_ex",     "MI_ISP_IQ_GetDUMMY_EX",    "MI_ISP_IQ_SetDUMMY_EX",
	  NULL, NULL, VT_BOOL, 0,   1 },

	/* ── Toggle controls ───────────────────────────────────────── */
	{ "defog",        "MI_ISP_IQ_GetDefog",        "MI_ISP_IQ_SetDefog",
	  NULL, NULL, VT_BOOL, 0,   1 },
	{ "color_to_gray","MI_ISP_IQ_GetColorToGray",  "MI_ISP_IQ_SetColorToGray",
	  NULL, NULL, VT_BOOL, 0,   1 },
	{ "nr3d_p1",      "MI_ISP_IQ_GetNR3D_P1",      "MI_ISP_IQ_SetNR3D_P1",
	  NULL, NULL, VT_BOOL, 0,   1 },
	{ "fpn",          "MI_ISP_IQ_GetFPN",           "MI_ISP_IQ_SetFPN",
	  NULL, NULL, VT_BOOL, 0,   1 },

	/* ── AE (auto-exposure) ────────────────────────────────────── */
	/* EV_COMP: {s32EV(4), u32Grad(4)} = 8B, primary s32EV@0 */
	{ "ae_ev_comp",   "MI_ISP_AE_GetEVComp",      "MI_ISP_AE_SetEVComp",
	  NULL, NULL, VT_U32, 0,    200 },
	/* AE mode enum: 0=A, 1=AV, 2=SV, 3=TV, 4=M */
	{ "ae_mode",      "MI_ISP_AE_GetExpoMode",    "MI_ISP_AE_SetExpoMode",
	  NULL, NULL, VT_U32, 0,    4 },
	/* AE state enum: 0=normal, 1=pause */
	{ "ae_state",     "MI_ISP_AE_GetState",        "MI_ISP_AE_SetState",
	  NULL, NULL, VT_U32, 0,    1 },
	/* Flicker enum: 0=disable, 1=60Hz, 2=50Hz, 3=auto */
	{ "ae_flicker",   "MI_ISP_AE_GetFlicker",      "MI_ISP_AE_SetFlicker",
	  NULL, NULL, VT_U32, 0,    3 },
	/* FlickerEX: bEnable(4)+enOpType(4)+u8Sensitivity@8 */
	{ "ae_flicker_ex","MI_ISP_AE_GetFlickerEX",   "MI_ISP_AE_SetFlickerEX",
	  NULL, NULL, VT_BOOL, 0,   1 },
	/* Metering mode enum: 0=average, 1=center, 2=spot */
	{ "ae_win_wgt_type","MI_ISP_AE_GetWinWgtType", "MI_ISP_AE_SetWinWgtType",
	  NULL, NULL, VT_U32, 0,    2 },
	/* Manual exposure: {u32FNx10(4), u32SensorGain(4), u32ISPGain(4), u32ShutterUS(4)} = 16B */
	{ "ae_manual_expo","MI_ISP_AE_GetManualExpo",  "MI_ISP_AE_SetManualExpo",
	  NULL, NULL, VT_U32, 0,    65535 },
	/* Exposure limit: {u32MinShutterUS,...} = 32B, 8 x u32 */
	{ "ae_expo_limit", "MI_ISP_AE_GetExposureLimit","MI_ISP_AE_SetExposureLimit",
	  NULL, NULL, VT_U32, 0,    65535 },
	/* AE stabilizer: bEnable(4)+u16DiffThd(2)+u16Percent(2) = 8B */
	{ "ae_stabilizer","MI_ISP_AE_GetStabilizer",   "MI_ISP_AE_SetStabilizer",
	  NULL, NULL, VT_BOOL, 0,   1 },
	/* AE RGBIR: bEnable(4)+u16MaxYWithIR(2)+u16MinISPGainCompRatio(2) = 8B */
	{ "ae_rgbir",     "MI_ISP_AE_GetRGBIRAE",      "MI_ISP_AE_SetRGBIRAE",
	  NULL, NULL, VT_BOOL, 0,   1 },
	/* AE HDR: complex LUT struct = 140B */
	{ "ae_hdr",       "MI_ISP_AE_GetHDR",          "MI_ISP_AE_SetHDR",
	  NULL, NULL, VT_BOOL, 0,   1 },

	/* ── AWB (auto white balance) ──────────────────────────────── */
	/* AWB AttrEx: bExtraLightEn(4)+stLightInfo[4] = 20B */
	{ "awb_attr_ex",  "MI_ISP_AWB_GetAttrEx",      "MI_ISP_AWB_SetAttrEx",
	  NULL, NULL, VT_BOOL, 0,   1 },
	/* AWB MultiLS: bEnable(4)+u8Sensitive(1)+u8CaliStrength(1)+CCMs = 44B */
	{ "awb_multi_ls", "MI_ISP_AWB_GetMultiLSAttr", "MI_ISP_AWB_SetMultiLSAttr",
	  NULL, NULL, VT_BOOL, 0,   1 },
	/* AWB stabilizer: bEnable(4)+thresholds = 12B */
	{ "awb_stabilizer","MI_ISP_AWB_GetStabilizer",  "MI_ISP_AWB_SetStabilizer",
	  NULL, NULL, VT_BOOL, 0,   1 },
	/* AWB CT calibration: u16StartIdx(2)+... = 84B */
	{ "awb_ct_cali",  "MI_ISP_AWB_GetCTCaliAttr",  "MI_ISP_AWB_SetCTCaliAttr",
	  NULL, NULL, VT_U16, 0,    65535 },
	/* AWB CT weight: u16LvIndex(2)+stParaAPI = 22B */
	{ "awb_ct_weight","MI_ISP_AWB_GetCTWeight",    "MI_ISP_AWB_SetCTWeight",
	  NULL, NULL, VT_U16, 0,    65535 },
};
#define NUM_PARAMS (sizeof(g_params) / sizeof(g_params[0]))

static void *g_isp_handle;

int star6e_iq_init(void)
{
	if (g_isp_handle)
		return 0;

	g_isp_handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!g_isp_handle) {
		fprintf(stderr, "[iq] ERROR: dlopen libmi_isp.so: %s\n",
			dlerror());
		return -1;
	}

	int resolved = 0;
	for (size_t i = 0; i < NUM_PARAMS; i++) {
		g_params[i].fn_get = (iq_fn_t)dlsym(g_isp_handle,
			g_params[i].get_sym);
		g_params[i].fn_set = (iq_fn_t)dlsym(g_isp_handle,
			g_params[i].set_sym);
		if (g_params[i].fn_get && g_params[i].fn_set)
			resolved++;
	}

	printf("[iq] IQ parameter API ready (%d/%d params resolved)\n",
		resolved, (int)NUM_PARAMS);
	return 0;
}

void star6e_iq_cleanup(void)
{
	if (g_isp_handle) {
		dlclose(g_isp_handle);
		g_isp_handle = NULL;
	}
	for (size_t i = 0; i < NUM_PARAMS; i++) {
		g_params[i].fn_get = NULL;
		g_params[i].fn_set = NULL;
	}
}

static uint32_t read_value(const uint8_t *buf, uint32_t offset, IqValueType vt)
{
	switch (vt) {
	case VT_U32: {
		uint32_t v;
		memcpy(&v, buf + offset, sizeof(v));
		return v;
	}
	case VT_U16: {
		uint16_t v;
		memcpy(&v, buf + offset, sizeof(v));
		return v;
	}
	case VT_U8:
		return buf[offset];
	case VT_BOOL: {
		uint32_t v;
		memcpy(&v, buf + offset, sizeof(v));
		return v;
	}
	}
	return 0;
}

static void write_value(uint8_t *buf, uint32_t offset, IqValueType vt,
	uint32_t val)
{
	switch (vt) {
	case VT_U32:
	case VT_BOOL:
		memcpy(buf + offset, &val, sizeof(uint32_t));
		break;
	case VT_U16: {
		uint16_t v = (uint16_t)val;
		memcpy(buf + offset, &v, sizeof(v));
		break;
	}
	case VT_U8:
		buf[offset] = (uint8_t)val;
		break;
	}
}

char *star6e_iq_query(void)
{
	if (!g_isp_handle)
		return NULL;

	char buf[16384];
	int pos = 0;

	pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
		"{\"ok\":true,\"data\":{");

	for (size_t i = 0; i < NUM_PARAMS; i++) {
		IqParamDesc *p = &g_params[i];
		if (!p->fn_get) {
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"%s\":{\"available\":false}%s",
				p->name, (i + 1 < NUM_PARAMS) ? "," : "");
			continue;
		}

		static uint8_t iq_buf[IQ_BUF_SIZE];
		memset(iq_buf, 0, IQ_BUF_SIZE);
		MI_S32 ret = p->fn_get(0, iq_buf);

		uint32_t enable = read_value(iq_buf, IQ_OFFSET_ENABLE, VT_U32);

		if (p->vtype == VT_BOOL) {
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"%s\":{\"ret\":%d,\"value\":%s}%s",
				p->name, ret,
				enable ? "true" : "false",
				(i + 1 < NUM_PARAMS) ? "," : "");
		} else if (p->manual_offset == 4) {
			/* Manual-only struct: no enOpType */
			uint32_t val = read_value(iq_buf, p->manual_offset,
				p->vtype);
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"%s\":{\"ret\":%d,\"enabled\":%s,"
				"\"value\":%u}%s",
				p->name, ret,
				enable ? "true" : "false",
				val,
				(i + 1 < NUM_PARAMS) ? "," : "");
		} else {
			/* Standard auto/manual struct */
			uint32_t optype = read_value(iq_buf,
				IQ_OFFSET_OPTYPE, VT_U32);
			uint32_t val = read_value(iq_buf, p->manual_offset,
				p->vtype);
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"%s\":{\"ret\":%d,\"enabled\":%s,"
				"\"op_type\":\"%s\",\"value\":%u}%s",
				p->name, ret,
				enable ? "true" : "false",
				optype == 1 ? "manual" : "auto",
				val,
				(i + 1 < NUM_PARAMS) ? "," : "");
		}
	}

	/* ── Read-only ISP diagnostics ─────────────────────────────── */
	pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ",\"_diag\":{");
	{
		typedef int (*fn_ver_t)(uint32_t, void *);
		typedef int (*fn_ind_t)(uint32_t, void *);
		typedef int (*fn_ccm_t)(uint32_t, void *);

		fn_ver_t fn_ver = (fn_ver_t)dlsym(g_isp_handle,
			"MI_ISP_IQ_GetVersionInfo");
		fn_ind_t fn_ind = (fn_ind_t)dlsym(g_isp_handle,
			"MI_ISP_IQ_GetIQind");
		fn_ccm_t fn_ccm = (fn_ccm_t)dlsym(g_isp_handle,
			"MI_ISP_IQ_QueryCCMInfo");

		/* IQ bin version */
		if (fn_ver) {
			uint32_t ver[3] = {0, 0, 0};
			int r = fn_ver(0, ver);
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"version\":{\"ret\":%d,\"vendor\":%u,"
				"\"major\":%u,\"minor\":%u},",
				r, ver[0], ver[1], ver[2]);
		}

		/* Current IQ/ISO index */
		if (fn_ind) {
			uint32_t idx = 0;
			int r = fn_ind(0, &idx);
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"iq_index\":{\"ret\":%d,\"value\":%u},",
				r, idx);
		}

		/* Current CCM info */
		if (fn_ccm) {
			static uint8_t ccm_buf[64];
			memset(ccm_buf, 0, sizeof(ccm_buf));
			int r = fn_ccm(0, ccm_buf);
			uint16_t cct = 0;
			memcpy(&cct, ccm_buf + 24, sizeof(cct));
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"ccm\":{\"ret\":%d,\"color_temp\":%u}",
				r, cct);
		}
	}
	pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "}}}");
	return strdup(buf);
}

int star6e_iq_set(const char *param, const char *value)
{
	if (!g_isp_handle || !param || !value)
		return -1;

	IqParamDesc *target = NULL;
	for (size_t i = 0; i < NUM_PARAMS; i++) {
		if (strcmp(g_params[i].name, param) == 0) {
			target = &g_params[i];
			break;
		}
	}

	if (!target) {
		fprintf(stderr, "[iq] unknown parameter: %s\n", param);
		return -1;
	}

	if (!target->fn_get || !target->fn_set) {
		fprintf(stderr, "[iq] %s: symbols not resolved\n", param);
		return -1;
	}

	static uint8_t iq_buf[IQ_BUF_SIZE];
	memset(iq_buf, 0, IQ_BUF_SIZE);

	/* Get current state first to preserve unknown fields */
	MI_S32 ret = target->fn_get(0, iq_buf);
	if (ret != 0) {
		fprintf(stderr, "[iq] %s: Get failed: 0x%08x\n",
			param, (unsigned)ret);
		return -1;
	}

	uint32_t level = (uint32_t)atoi(value);
	if (level > target->max_val)
		level = target->max_val;

	if (target->vtype == VT_BOOL) {
		/* Bool-only: just set bEnable */
		write_value(iq_buf, IQ_OFFSET_ENABLE, VT_U32,
			level ? 1 : 0);
	} else if (target->manual_offset == 4) {
		/* Manual-only: set bEnable=1, write value (no enOpType) */
		write_value(iq_buf, IQ_OFFSET_ENABLE, VT_U32, 1);
		write_value(iq_buf, target->manual_offset, target->vtype,
			level);
	} else {
		/* Standard auto/manual: set bEnable=1, enOpType=1 (manual),
		 * write value at the SDK-defined manual offset */
		write_value(iq_buf, IQ_OFFSET_ENABLE, VT_U32, 1);
		write_value(iq_buf, IQ_OFFSET_OPTYPE, VT_U32, 1);
		write_value(iq_buf, target->manual_offset, target->vtype,
			level);
	}

	ret = target->fn_set(0, iq_buf);
	if (ret != 0) {
		fprintf(stderr, "[iq] %s: Set failed: 0x%08x\n",
			param, (unsigned)ret);
		return -1;
	}

	printf("[iq] %s = %s (set OK)\n", param, value);
	return 0;
}
