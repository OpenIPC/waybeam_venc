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
#include "cJSON.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int MI_S32;
typedef MI_S32 (*iq_fn_t)(uint32_t channel, void *param);

#define IQ_OFFSET_ENABLE   0
#define IQ_OFFSET_OPTYPE   4
#define IQ_BUF_SIZE        32768  /* must fit largest struct: DUMMY ~26KB */

/* Serializes query and set operations — the static iq_buf is shared. */
static pthread_mutex_t g_iq_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef enum {
	VT_BOOL,    /* just bEnable (color_to_gray, defog) */
	VT_U8,      /* uint8_t */
	VT_U16,     /* uint16_t */
	VT_U32,     /* uint32_t */
} IqValueType;

typedef struct {
	const char *name;       /* "y_ofst", "matrix" */
	IqValueType vtype;      /* VT_U8, VT_U16, VT_U32 */
	uint16_t rel_offset;    /* bytes from manual struct start */
	uint16_t count;         /* 1=scalar, N=array */
	uint32_t max_val;       /* per-element max */
} IqFieldDesc;

typedef struct {
	const char *name;
	const char *get_sym;
	const char *set_sym;
	iq_fn_t     fn_get;
	iq_fn_t     fn_set;
	IqValueType vtype;
	uint32_t    manual_offset;  /* 0=bool-only, 4=manual-only, >4=auto/manual */
	uint32_t    max_val;
	const IqFieldDesc *fields;  /* NULL = legacy single-value */
	uint16_t    field_count;    /* 0 when fields==NULL */
} IqParamDesc;

/* ── Multi-field descriptors (from SDK mi_isp_iq_datatype.h) ────────── */

/* COLORTRANS: manual-only@4 */
static const IqFieldDesc colortrans_fields[] = {
	{ "y_ofst",  VT_U16, 0,  1, 2047 },
	{ "u_ofst",  VT_U16, 2,  1, 2047 },
	{ "v_ofst",  VT_U16, 4,  1, 2047 },
	{ "matrix",  VT_U16, 6,  9, 65535 },
};

/* R2Y: manual-only@4 */
static const IqFieldDesc r2y_fields[] = {
	{ "matrix",  VT_U16, 0,  9, 1023 },
	{ "add_y16", VT_U8,  18, 1, 1 },
};

/* OBC: auto/manual, manual@136 */
static const IqFieldDesc obc_fields[] = {
	{ "val_r",  VT_U16, 0, 1, 65535 },
	{ "val_gr", VT_U16, 2, 1, 65535 },
	{ "val_gb", VT_U16, 4, 1, 65535 },
	{ "val_b",  VT_U16, 6, 1, 65535 },
};

/* DEMOSAIC: manual-only@4 */
static const IqFieldDesc demosaic_fields[] = {
	{ "dir_thrd",      VT_U8, 0, 1, 63 },
	{ "edge_smooth_y", VT_U8, 1, 1, 255 },
	{ "edge_smooth_c", VT_U8, 2, 1, 127 },
};

/* FALSECOLOR: auto/manual, manual@120 */
static const IqFieldDesc false_color_fields[] = {
	{ "freq_thrd",       VT_U8, 0, 1, 255 },
	{ "edge_score_thrd", VT_U8, 1, 1, 255 },
	{ "chroma_thrd_max", VT_U8, 2, 1, 127 },
	{ "chroma_thrd_mid", VT_U8, 3, 1, 127 },
	{ "chroma_thrd_min", VT_U8, 4, 1, 127 },
	{ "strength_mid",    VT_U8, 5, 1, 7 },
	{ "strength_min",    VT_U8, 6, 1, 7 },
};

/* CROSSTALK: auto/manual, manual@296 */
static const IqFieldDesc crosstalk_fields[] = {
	{ "strength",      VT_U8,  0,  1,  31 },
	{ "strength_by_y", VT_U8,  1,  15, 127 },
	{ "threshold",     VT_U8,  16, 1,  255 },
	{ "offset",        VT_U16, 18, 1,  4095 },
};

/* WDR_CURVE_ADV: auto/manual, manual@104 */
static const IqFieldDesc wdr_curve_adv_fields[] = {
	{ "slope",           VT_U16, 0, 1, 16384 },
	{ "trans_point_0",   VT_U8,  2, 1, 255 },
	{ "trans_point_1",   VT_U8,  3, 1, 255 },
	{ "saturated_point", VT_U8,  4, 1, 255 },
	{ "curve_mode_sel",  VT_U8,  5, 1, 1 },
};

#define FIELDS(a) a, (sizeof(a) / sizeof(a[0]))

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
	  NULL, NULL, VT_U32, 72,   100, NULL, 0 },
	{ "contrast",     "MI_ISP_IQ_GetContrast",    "MI_ISP_IQ_SetContrast",
	  NULL, NULL, VT_U32, 72,   100, NULL, 0 },
	{ "brightness",   "MI_ISP_IQ_GetBrightness",  "MI_ISP_IQ_SetBrightness",
	  NULL, NULL, VT_U32, 72,   100, NULL, 0 },
	/* SATURATION_PARAM_t = 24B, auto=384, manual@392 (u8SatAllStr 0-127, 32=1X) */
	{ "saturation",   "MI_ISP_IQ_GetSaturation",  "MI_ISP_IQ_SetSaturation",
	  NULL, NULL, VT_U8,  392,  127, NULL, 0 },
	/* SHARPNESS_PARAM_t = 74B, auto=1184, manual@1192 (u8OverShootGain) */
	{ "sharpness",    "MI_ISP_IQ_GetSharpness",   "MI_ISP_IQ_SetSharpness",
	  NULL, NULL, VT_U8,  1192, 255, NULL, 0 },
	/* HSV_PARAM_t = 193B, auto=3088, manual@3096 (s16HueLut[0]) */
	{ "hsv",          "MI_ISP_IQ_GetHSV",          "MI_ISP_IQ_SetHSV",
	  NULL, NULL, VT_U8,  3096, 64,  NULL, 0 },

	/* ── Noise reduction ───────────────────────────────────────── */
	{ "nr3d",         "MI_ISP_IQ_GetNR3D",        "MI_ISP_IQ_SetNR3D",
	  NULL, NULL, VT_U8,  1288, 255, NULL, 0 },
	{ "nr3d_ex",      "MI_ISP_IQ_GetNR3D_EX",     "MI_ISP_IQ_SetNR3D_EX",
	  NULL, NULL, VT_U32, 4,    1,   NULL, 0 },
	{ "nr_despike",   "MI_ISP_IQ_GetNRDeSpike",   "MI_ISP_IQ_SetNRDeSpike",
	  NULL, NULL, VT_U8,  552,  15,  NULL, 0 },
	{ "nr_luma",      "MI_ISP_IQ_GetNRLuma",      "MI_ISP_IQ_SetNRLuma",
	  NULL, NULL, VT_U8,  104,  255, NULL, 0 },
	{ "nr_luma_adv",  "MI_ISP_IQ_GetNRLuma_Adv",  "MI_ISP_IQ_SetNRLuma_Adv",
	  NULL, NULL, VT_U32, 1304, 1,   NULL, 0 },
	{ "nr_chroma",    "MI_ISP_IQ_GetNRChroma",    "MI_ISP_IQ_SetNRChroma",
	  NULL, NULL, VT_U8,  216,  127, NULL, 0 },
	{ "nr_chroma_adv","MI_ISP_IQ_GetNRChroma_Adv","MI_ISP_IQ_SetNRChroma_Adv",
	  NULL, NULL, VT_U8,  488,  255, NULL, 0 },

	/* ── Corrections ───────────────────────────────────────────── */
	{ "false_color",  "MI_ISP_IQ_GetFalseColor",  "MI_ISP_IQ_SetFalseColor",
	  NULL, NULL, VT_U8,  120,  255, FIELDS(false_color_fields) },
	{ "crosstalk",    "MI_ISP_IQ_GetCrossTalk",   "MI_ISP_IQ_SetCrossTalk",
	  NULL, NULL, VT_U8,  296,  31,  FIELDS(crosstalk_fields) },
	{ "demosaic",     "MI_ISP_IQ_GetDEMOSAIC",    "MI_ISP_IQ_SetDEMOSAIC",
	  NULL, NULL, VT_U8,  4,    63,  FIELDS(demosaic_fields) },
	{ "obc",          "MI_ISP_IQ_GetOBC",          "MI_ISP_IQ_SetOBC",
	  NULL, NULL, VT_U16, 136,  65535, FIELDS(obc_fields) },
	{ "dynamic_dp",   "MI_ISP_IQ_GetDynamicDP",   "MI_ISP_IQ_SetDynamicDP",
	  NULL, NULL, VT_U8,  488,  1,   NULL, 0 },
	{ "dp_cluster",   "MI_ISP_IQ_GetDynamicDP_CLUSTER", "MI_ISP_IQ_SetDynamicDP_CLUSTER",
	  NULL, NULL, VT_U32, 1080, 1,   NULL, 0 },
	{ "r2y",          "MI_ISP_IQ_GetR2Y",          "MI_ISP_IQ_SetR2Y",
	  NULL, NULL, VT_U16, 4,    1023, FIELDS(r2y_fields) },
	{ "colortrans",   "MI_ISP_IQ_GetCOLORTRANS",  "MI_ISP_IQ_SetCOLORTRANS",
	  NULL, NULL, VT_U16, 4,    2047, FIELDS(colortrans_fields) },
	{ "rgb_matrix",   "MI_ISP_IQ_GetRGBMatrix",   "MI_ISP_IQ_SetRGBMatrix",
	  NULL, NULL, VT_U16, 444,  8191, NULL, 0 },

	/* ── Dynamic range & special ───────────────────────────────── */
	{ "wdr",          "MI_ISP_IQ_GetWDR",          "MI_ISP_IQ_SetWDR",
	  NULL, NULL, VT_U8,  648,  4,    NULL, 0 },
	{ "wdr_curve_adv","MI_ISP_IQ_GetWDRCurveAdv", "MI_ISP_IQ_SetWDRCurveAdv",
	  NULL, NULL, VT_U16, 104,  16384, FIELDS(wdr_curve_adv_fields) },
	{ "pfc",          "MI_ISP_IQ_GetPFC",          "MI_ISP_IQ_SetPFC",
	  NULL, NULL, VT_U8,  376,  255, NULL, 0 },
	{ "pfc_ex",       "MI_ISP_IQ_GetPFC_EX",      "MI_ISP_IQ_SetPFC_EX",
	  NULL, NULL, VT_U32, 4,    1,   NULL, 0 },
	{ "hdr",          "MI_ISP_IQ_GetHDR",          "MI_ISP_IQ_SetHDR",
	  NULL, NULL, VT_U32, 904,  1,   NULL, 0 },
	{ "hdr_ex",       "MI_ISP_IQ_GetHDR_EX",      "MI_ISP_IQ_SetHDR_EX",
	  NULL, NULL, VT_U16, 4,    65535, NULL, 0 },
	{ "shp_ex",       "MI_ISP_IQ_GetSHP_EX",      "MI_ISP_IQ_SetSHP_EX",
	  NULL, NULL, VT_U32, 4,    1,   NULL, 0 },
	{ "rgbir",        "MI_ISP_IQ_GetRGBIR",        "MI_ISP_IQ_SetRGBIR",
	  NULL, NULL, VT_U8,  600,  7,   NULL, 0 },
	{ "iq_mode",      "MI_ISP_IQ_GetIQMode",       "MI_ISP_IQ_SetIQMode",
	  NULL, NULL, VT_U32, 0,    1,   NULL, 0 },

	/* ── Lens & sensor calibration (flat structs) ─────────────── */
	{ "lsc",          "MI_ISP_IQ_GetLSC",          "MI_ISP_IQ_SetLSC",
	  NULL, NULL, VT_U16, 4,    65535, NULL, 0 },
	{ "lsc_ctrl",     "MI_ISP_IQ_GetLSC_CTRL",    "MI_ISP_IQ_SetLSC_CTRL",
	  NULL, NULL, VT_U8,  4,    255, NULL, 0 },
	{ "alsc",         "MI_ISP_IQ_GetALSC",         "MI_ISP_IQ_SetALSC",
	  NULL, NULL, VT_U8,  4,    255, NULL, 0 },
	{ "alsc_ctrl",    "MI_ISP_IQ_GetALSC_CTRL",   "MI_ISP_IQ_SetALSC_CTRL",
	  NULL, NULL, VT_U8,  4,    255, NULL, 0 },
	{ "obc_p1",       "MI_ISP_IQ_GetOBC_P1",       "MI_ISP_IQ_SetOBC_P1",
	  NULL, NULL, VT_U16, 136,  65535, FIELDS(obc_fields) },
	{ "stitch_lpf",   "MI_ISP_IQ_GetSTITCH_LPF",  "MI_ISP_IQ_SetSTITCH_LPF",
	  NULL, NULL, VT_U16, 4,    256, NULL, 0 },

	/* ── LUT-based (enable/mode control only) ─────────────────── */
	{ "rgb_gamma",    "MI_ISP_IQ_GetRGBGamma",    "MI_ISP_IQ_SetRGBGamma",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "yuv_gamma",    "MI_ISP_IQ_GetYUVGamma",    "MI_ISP_IQ_SetYUVGamma",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "wdr_curve_full","MI_ISP_IQ_GetWDRCurveFull","MI_ISP_IQ_SetWDRCurveFull",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },

	/* ── Debug/test ────────────────────────────────────────────── */
	{ "dummy",        "MI_ISP_IQ_GetDUMMY",        "MI_ISP_IQ_SetDUMMY",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "dummy_ex",     "MI_ISP_IQ_GetDUMMY_EX",    "MI_ISP_IQ_SetDUMMY_EX",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },

	/* ── Toggle controls ───────────────────────────────────────── */
	{ "defog",        "MI_ISP_IQ_GetDefog",        "MI_ISP_IQ_SetDefog",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "color_to_gray","MI_ISP_IQ_GetColorToGray",  "MI_ISP_IQ_SetColorToGray",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "nr3d_p1",      "MI_ISP_IQ_GetNR3D_P1",      "MI_ISP_IQ_SetNR3D_P1",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "fpn",          "MI_ISP_IQ_GetFPN",           "MI_ISP_IQ_SetFPN",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },

	/* ── AE (auto-exposure) ────────────────────────────────────── */
	{ "ae_ev_comp",   "MI_ISP_AE_GetEVComp",      "MI_ISP_AE_SetEVComp",
	  NULL, NULL, VT_U32, 0,    200, NULL, 0 },
	{ "ae_mode",      "MI_ISP_AE_GetExpoMode",    "MI_ISP_AE_SetExpoMode",
	  NULL, NULL, VT_U32, 0,    4,   NULL, 0 },
	{ "ae_state",     "MI_ISP_AE_GetState",        "MI_ISP_AE_SetState",
	  NULL, NULL, VT_U32, 0,    1,   NULL, 0 },
	{ "ae_flicker",   "MI_ISP_AE_GetFlicker",      "MI_ISP_AE_SetFlicker",
	  NULL, NULL, VT_U32, 0,    3,   NULL, 0 },
	{ "ae_flicker_ex","MI_ISP_AE_GetFlickerEX",   "MI_ISP_AE_SetFlickerEX",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "ae_win_wgt_type","MI_ISP_AE_GetWinWgtType", "MI_ISP_AE_SetWinWgtType",
	  NULL, NULL, VT_U32, 0,    2,   NULL, 0 },
	{ "ae_manual_expo","MI_ISP_AE_GetManualExpo",  "MI_ISP_AE_SetManualExpo",
	  NULL, NULL, VT_U32, 0,    65535, NULL, 0 },
	{ "ae_expo_limit", "MI_ISP_AE_GetExposureLimit","MI_ISP_AE_SetExposureLimit",
	  NULL, NULL, VT_U32, 0,    65535, NULL, 0 },
	{ "ae_stabilizer","MI_ISP_AE_GetStabilizer",   "MI_ISP_AE_SetStabilizer",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "ae_rgbir",     "MI_ISP_AE_GetRGBIRAE",      "MI_ISP_AE_SetRGBIRAE",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "ae_hdr",       "MI_ISP_AE_GetHDR",          "MI_ISP_AE_SetHDR",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },

	/* ── AWB (auto white balance) ──────────────────────────────── */
	{ "awb_attr_ex",  "MI_ISP_AWB_GetAttrEx",      "MI_ISP_AWB_SetAttrEx",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "awb_multi_ls", "MI_ISP_AWB_GetMultiLSAttr", "MI_ISP_AWB_SetMultiLSAttr",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "awb_stabilizer","MI_ISP_AWB_GetStabilizer",  "MI_ISP_AWB_SetStabilizer",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "awb_ct_cali",  "MI_ISP_AWB_GetCTCaliAttr",  "MI_ISP_AWB_SetCTCaliAttr",
	  NULL, NULL, VT_U16, 0,    65535, NULL, 0 },
	{ "awb_ct_weight","MI_ISP_AWB_GetCTWeight",    "MI_ISP_AWB_SetCTWeight",
	  NULL, NULL, VT_U16, 0,    65535, NULL, 0 },
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

/* Bounds-checked single-char append for JSON building. */
/* Bounds-checked single-char append and snprintf clamp for JSON building. */
#define JSON_CHR(b, p, sz, c) do { \
	if ((size_t)(p) < (sz) - 1) (b)[(p)++] = (c); } while (0)
#define JSON_CLAMP(p, sz) do { \
	if ((p) >= (int)(sz)) (p) = (int)(sz) - 1; } while (0)

static int emit_fields_json(char *buf, size_t buf_size,
	const IqParamDesc *p, const uint8_t *iq_buf)
{
	int pos = 0;
	uint32_t elem_size;

	pos += snprintf(buf + pos, buf_size - (size_t)pos, "\"fields\":{");
	JSON_CLAMP(pos, buf_size);
	for (uint16_t f = 0; f < p->field_count; f++) {
		const IqFieldDesc *fd = &p->fields[f];
		uint32_t foff = p->manual_offset + fd->rel_offset;

		if (f > 0)
			JSON_CHR(buf, pos, buf_size, ',');
		if (fd->count == 1) {
			uint32_t val = read_value(iq_buf, foff, fd->vtype);
			pos += snprintf(buf + pos, buf_size - (size_t)pos,
				"\"%s\":%u", fd->name, val);
		} else {
			pos += snprintf(buf + pos, buf_size - (size_t)pos,
				"\"%s\":[", fd->name);
			JSON_CLAMP(pos, buf_size);
			elem_size = (fd->vtype == VT_U8) ? 1 :
				(fd->vtype == VT_U16) ? 2 : 4;
			for (uint16_t e = 0; e < fd->count; e++) {
				uint32_t val = read_value(iq_buf,
					foff + e * elem_size, fd->vtype);
				if (e > 0) JSON_CHR(buf, pos, buf_size, ',');
				pos += snprintf(buf + pos,
					buf_size - (size_t)pos, "%u", val);
				JSON_CLAMP(pos, buf_size);
			}
			JSON_CHR(buf, pos, buf_size, ']');
		}
		JSON_CLAMP(pos, buf_size);
	}
	JSON_CHR(buf, pos, buf_size, '}');
	buf[pos < (int)buf_size ? pos : (int)buf_size - 1] = '\0';
	return pos;
}

char *star6e_iq_query(void)
{
	char *result;

	if (!g_isp_handle)
		return NULL;

	pthread_mutex_lock(&g_iq_mutex);

	char buf[16384];
	int pos = 0;

	pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
		"{\"ok\":true,\"data\":{");
	JSON_CLAMP(pos, sizeof(buf));

	for (size_t i = 0; i < NUM_PARAMS; i++) {
		IqParamDesc *p = &g_params[i];
		if (!p->fn_get) {
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"%s\":{\"available\":false}%s",
				p->name, (i + 1 < NUM_PARAMS) ? "," : "");
			JSON_CLAMP(pos, sizeof(buf));
			continue;
		}

		static uint8_t iq_buf[IQ_BUF_SIZE];
		memset(iq_buf, 0, IQ_BUF_SIZE);
		MI_S32 ret = p->fn_get(0, iq_buf);

		uint32_t enable = read_value(iq_buf, IQ_OFFSET_ENABLE, VT_U32);

		if (p->vtype == VT_BOOL) {
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"%s\":{\"ret\":%d,\"value\":%s}",
				p->name, ret,
				enable ? "true" : "false");
		} else if (p->manual_offset == 4) {
			uint32_t val = read_value(iq_buf, p->manual_offset,
				p->vtype);
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"%s\":{\"ret\":%d,\"enabled\":%s,"
				"\"value\":%u",
				p->name, ret,
				enable ? "true" : "false", val);
			JSON_CLAMP(pos, sizeof(buf));
			if (p->fields) {
				JSON_CHR(buf, pos, sizeof(buf), ',');
				pos += emit_fields_json(buf + pos,
					sizeof(buf) - (size_t)pos, p, iq_buf);
			}
			JSON_CHR(buf, pos, sizeof(buf), '}');
		} else {
			uint32_t optype = read_value(iq_buf,
				IQ_OFFSET_OPTYPE, VT_U32);
			uint32_t val = read_value(iq_buf, p->manual_offset,
				p->vtype);
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"%s\":{\"ret\":%d,\"enabled\":%s,"
				"\"op_type\":\"%s\",\"value\":%u",
				p->name, ret,
				enable ? "true" : "false",
				optype == 1 ? "manual" : "auto", val);
			JSON_CLAMP(pos, sizeof(buf));
			if (p->fields) {
				JSON_CHR(buf, pos, sizeof(buf), ',');
				pos += emit_fields_json(buf + pos,
					sizeof(buf) - (size_t)pos, p, iq_buf);
			}
			JSON_CHR(buf, pos, sizeof(buf), '}');
		}
		if (i + 1 < NUM_PARAMS)
			JSON_CHR(buf, pos, sizeof(buf), ',');
	}

	/* ── Read-only ISP diagnostics ─────────────────────────────── */
	pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ",\"_diag\":{");
	JSON_CLAMP(pos, sizeof(buf));
	{
		typedef int (*fn_ver_t)(uint32_t, void *);
		typedef int (*fn_ind_t)(uint32_t, void *);
		typedef int (*fn_ccm_t)(uint32_t, void *);
		int diag_first = 1;

		fn_ver_t fn_ver = (fn_ver_t)dlsym(g_isp_handle,
			"MI_ISP_IQ_GetVersionInfo");
		fn_ind_t fn_ind = (fn_ind_t)dlsym(g_isp_handle,
			"MI_ISP_IQ_GetIQind");
		fn_ccm_t fn_ccm = (fn_ccm_t)dlsym(g_isp_handle,
			"MI_ISP_IQ_QueryCCMInfo");

		if (fn_ver) {
			uint32_t ver[3] = {0, 0, 0};
			int r = fn_ver(0, ver);
			if (!diag_first) JSON_CHR(buf, pos, sizeof(buf), ',');
			diag_first = 0;
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"version\":{\"ret\":%d,\"vendor\":%u,"
				"\"major\":%u,\"minor\":%u}",
				r, ver[0], ver[1], ver[2]);
			JSON_CLAMP(pos, sizeof(buf));
		}

		if (fn_ind) {
			uint32_t idx = 0;
			int r = fn_ind(0, &idx);
			if (!diag_first) JSON_CHR(buf, pos, sizeof(buf), ',');
			diag_first = 0;
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"iq_index\":{\"ret\":%d,\"value\":%u}",
				r, idx);
			JSON_CLAMP(pos, sizeof(buf));
		}

		if (fn_ccm) {
			static uint8_t ccm_buf[64];
			memset(ccm_buf, 0, sizeof(ccm_buf));
			int r = fn_ccm(0, ccm_buf);
			uint16_t cct = 0;
			memcpy(&cct, ccm_buf + 24, sizeof(cct));
			if (!diag_first) JSON_CHR(buf, pos, sizeof(buf), ',');
			diag_first = 0;
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"ccm\":{\"ret\":%d,\"color_temp\":%u}",
				r, cct);
			JSON_CLAMP(pos, sizeof(buf));
		}
	}
	pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "}}}");
	JSON_CLAMP(pos, sizeof(buf));
	result = strdup(buf);
	pthread_mutex_unlock(&g_iq_mutex);
	return result;
}

int star6e_iq_set(const char *param, const char *value)
{
	int rc;

	if (!g_isp_handle || !param || !value)
		return -1;

	pthread_mutex_lock(&g_iq_mutex);

	/* Check for dot-notation: "colortrans.y_ofst" */
	const char *dot = strchr(param, '.');
	char param_name[64];
	const char *field_name = NULL;

	if (dot) {
		size_t plen = (size_t)(dot - param);
		if (plen >= sizeof(param_name)) plen = sizeof(param_name) - 1;
		memcpy(param_name, param, plen);
		param_name[plen] = '\0';
		field_name = dot + 1;
	} else {
		snprintf(param_name, sizeof(param_name), "%s", param);
	}

	IqParamDesc *target = NULL;
	for (size_t i = 0; i < NUM_PARAMS; i++) {
		if (strcmp(g_params[i].name, param_name) == 0) {
			target = &g_params[i];
			break;
		}
	}

	if (!target) {
		fprintf(stderr, "[iq] unknown parameter: %s\n", param_name);
		rc = -1;
		goto out;
	}

	if (!target->fn_get || !target->fn_set) {
		fprintf(stderr, "[iq] %s: symbols not resolved\n", param_name);
		rc = -1;
		goto out;
	}

	static uint8_t iq_buf[IQ_BUF_SIZE];
	memset(iq_buf, 0, IQ_BUF_SIZE);

	/* Get current state first to preserve unknown fields */
	MI_S32 ret = target->fn_get(0, iq_buf);
	if (ret != 0) {
		fprintf(stderr, "[iq] %s: Get failed: 0x%08x\n",
			param_name, (unsigned)ret);
		rc = -1;
		goto out;
	}

	/* Field-level set via dot-notation */
	if (field_name && target->fields) {
		const IqFieldDesc *fd = NULL;
		for (uint16_t f = 0; f < target->field_count; f++) {
			if (strcmp(target->fields[f].name, field_name) == 0) {
				fd = &target->fields[f];
				break;
			}
		}
		if (!fd) {
			fprintf(stderr, "[iq] %s: unknown field: %s\n",
				param_name, field_name);
			rc = -1;
			goto out;
		}

		uint32_t abs_offset = target->manual_offset + fd->rel_offset;

		/* Enable + set manual mode */
		write_value(iq_buf, IQ_OFFSET_ENABLE, VT_U32, 1);
		if (target->manual_offset > 4)
			write_value(iq_buf, IQ_OFFSET_OPTYPE, VT_U32, 1);

		if (fd->count == 1) {
			uint32_t level = (uint32_t)atoi(value);
			if (level > fd->max_val) level = fd->max_val;
			write_value(iq_buf, abs_offset, fd->vtype, level);
		} else {
			/* Comma-separated array */
			uint32_t elem_size = (fd->vtype == VT_U8) ? 1 :
				(fd->vtype == VT_U16) ? 2 : 4;
			const char *p = value;
			for (uint16_t e = 0; e < fd->count && *p; e++) {
				uint32_t v = (uint32_t)atoi(p);
				if (v > fd->max_val) v = fd->max_val;
				write_value(iq_buf, abs_offset + e * elem_size,
					fd->vtype, v);
				while (*p && *p != ',') p++;
				if (*p == ',') p++;
			}
		}
	} else {
		/* Legacy single-value set */
		uint32_t level = (uint32_t)atoi(value);
		if (level > target->max_val)
			level = target->max_val;

		if (target->vtype == VT_BOOL) {
			write_value(iq_buf, IQ_OFFSET_ENABLE, VT_U32,
				level ? 1 : 0);
		} else if (target->manual_offset == 4) {
			write_value(iq_buf, IQ_OFFSET_ENABLE, VT_U32, 1);
			write_value(iq_buf, target->manual_offset,
				target->vtype, level);
		} else {
			write_value(iq_buf, IQ_OFFSET_ENABLE, VT_U32, 1);
			write_value(iq_buf, IQ_OFFSET_OPTYPE, VT_U32, 1);
			write_value(iq_buf, target->manual_offset,
				target->vtype, level);
		}
	}

	ret = target->fn_set(0, iq_buf);
	if (ret != 0) {
		fprintf(stderr, "[iq] %s: Set failed: 0x%08x\n",
			param, (unsigned)ret);
		rc = -1;
		goto out;
	}

	printf("[iq] %s = %s (set OK)\n", param, value);
	rc = 0;

out:
	pthread_mutex_unlock(&g_iq_mutex);
	return rc;
}

int star6e_iq_import(const char *json_str)
{
	cJSON *root, *data, *fields_obj;
	int applied = 0, failed = 0;
	char val_buf[512];

	if (!json_str)
		return -1;

	root = cJSON_Parse(json_str);
	if (!root) {
		fprintf(stderr, "[iq] import: JSON parse failed\n");
		return -1;
	}

	/* Accept both raw data object and wrapped {"ok":true,"data":{...}} */
	data = cJSON_GetObjectItemCaseSensitive(root, "data");
	if (!data)
		data = root;

	/* Iterate all params in the JSON */
	cJSON *item = NULL;
	cJSON_ArrayForEach(item, data) {
		const char *pname = item->string;
		if (!pname || pname[0] == '_')
			continue;  /* skip _diag */

		/* Handle fields object for multi-field params */
		fields_obj = cJSON_GetObjectItemCaseSensitive(item, "fields");
		if (fields_obj && cJSON_IsObject(fields_obj)) {
			cJSON *fld = NULL;
			cJSON_ArrayForEach(fld, fields_obj) {
				const char *fname = fld->string;
				if (!fname) continue;

				char key[128];
				snprintf(key, sizeof(key), "%s.%s", pname, fname);

				if (cJSON_IsArray(fld)) {
					/* Build comma-separated value */
					int pos = 0;
					cJSON *elem = NULL;
					cJSON_ArrayForEach(elem, fld) {
						if (pos > 0 && pos < (int)sizeof(val_buf) - 1)
							val_buf[pos++] = ',';
						pos += snprintf(val_buf + pos,
							sizeof(val_buf) - (size_t)pos,
							"%d", elem->valueint);
					}
					val_buf[pos] = '\0';
				} else if (cJSON_IsNumber(fld)) {
					snprintf(val_buf, sizeof(val_buf), "%d",
						fld->valueint);
				} else {
					continue;
				}

				if (star6e_iq_set(key, val_buf) == 0)
					applied++;
				else
					failed++;
			}
			continue;
		}

		/* Handle simple value or bool */
		cJSON *val_item = cJSON_GetObjectItemCaseSensitive(item, "value");
		if (!val_item) continue;

		if (cJSON_IsBool(val_item)) {
			snprintf(val_buf, sizeof(val_buf), "%d",
				cJSON_IsTrue(val_item) ? 1 : 0);
		} else if (cJSON_IsNumber(val_item)) {
			snprintf(val_buf, sizeof(val_buf), "%d",
				val_item->valueint);
		} else {
			continue;
		}

		if (star6e_iq_set(pname, val_buf) == 0)
			applied++;
		else
			failed++;
	}

	cJSON_Delete(root);
	printf("[iq] import: %d applied, %d failed\n", applied, failed);
	return failed > 0 ? -1 : 0;
}
