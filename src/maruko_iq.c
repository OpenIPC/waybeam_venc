/*
 * maruko_iq.c — IQ parameter interface for SigmaStar ISP (Infinity6C/Maruko)
 *
 * Adapted from star6e_iq.c. Key differences from Star6E:
 *   - ISP functions take (DevId, Channel, data*) — 3 args, not 2
 *   - Some symbol names differ (CamelCase vs UPPERCASE)
 *   - Struct layouts may differ — manual_offset values need validation
 *
 * First pass: use same offsets as star6e, validate on hardware.
 * Parameters that fail Get will show "available":false in the query.
 */

#include "maruko_iq.h"
#include "cJSON.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maruko ISP functions: (dev, channel, data*) */
typedef int (*iq_fn_t)(uint32_t dev, uint32_t channel, void *param);

/* Maruko (DUAL_OS firmware) IQ API path:
 * 1. MI_ISP_Alloc_IQDataBuf — allocate shared memory buffer
 * 2. memcpy data into shared buffer
 * 3. MI_SYS_FlushInvCache — flush CPU cache for DMA coherency
 * 4. MI_ISP_SetIQApiData(&header, buffer) — send to ISP RTOS
 *
 * The GENERAL_* and individual MI_ISP_IQ_Set* functions return
 * success but don't take effect because they don't use the
 * shared memory path required by DUAL_OS. */
typedef struct {
	uint32_t u32HeadSize;  /* sizeof(this struct) = 24 */
	uint32_t u32DataLen;   /* size of parameter struct */
	uint32_t u32CtrlID;    /* API ID (e.g. 4100 for ColorToGray) */
	uint32_t u32Channel;
	uint32_t u32DevId;
	int32_t  s32Ret;
} IspApiHeader;

typedef int (*isp_api_fn_t)(IspApiHeader *header, void *data);
typedef int (*isp_alloc_fn_t)(uint32_t size, void **buf);
typedef int (*sys_flush_fn_t)(void *buf, uint32_t size);

static isp_api_fn_t g_fn_api_set;
static isp_api_fn_t g_fn_api_get;
static isp_alloc_fn_t g_fn_alloc;
static sys_flush_fn_t g_fn_flush;

#define IQ_DEV  0
#define IQ_CHN  0
#define IQ_CALL(fn, buf)  (fn)(IQ_DEV, IQ_CHN, (buf))

#define IQ_OFFSET_ENABLE   0
#define IQ_OFFSET_OPTYPE   4
#define IQ_BUF_SIZE        32768

static pthread_mutex_t g_iq_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef enum {
	VT_BOOL,
	VT_U8,
	VT_U16,
	VT_U32,
} IqValueType;

typedef struct {
	const char *name;
	IqValueType vtype;
	uint16_t rel_offset;
	uint16_t count;
	uint32_t max_val;
} IqFieldDesc;

typedef struct {
	const char *name;
	const char *get_sym;
	const char *set_sym;
	iq_fn_t     fn_get;
	iq_fn_t     fn_set;
	IqValueType vtype;
	uint32_t    manual_offset;
	uint32_t    max_val;
	const IqFieldDesc *fields;
	uint16_t    field_count;
	uint16_t    api_id;  /* MI_ISP_IQ_SetAll/GetAll API ID (0=not mapped) */
} IqParamDesc;

/* ── Multi-field descriptors ──────────────────────────────────────── */

static const IqFieldDesc colortrans_fields[] = {
	{ "y_ofst",  VT_U16, 0,  1, 2047 },
	{ "u_ofst",  VT_U16, 2,  1, 2047 },
	{ "v_ofst",  VT_U16, 4,  1, 2047 },
	{ "matrix",  VT_U16, 6,  9, 65535 },
};

static const IqFieldDesc r2y_fields[] = {
	{ "matrix",  VT_U16, 0,  9, 1023 },
	{ "add_y16", VT_U8,  18, 1, 1 },
};

static const IqFieldDesc obc_fields[] = {
	{ "val_r",  VT_U16, 0, 1, 65535 },
	{ "val_gr", VT_U16, 2, 1, 65535 },
	{ "val_gb", VT_U16, 4, 1, 65535 },
	{ "val_b",  VT_U16, 6, 1, 65535 },
};

static const IqFieldDesc demosaic_fields[] = {
	{ "dir_thrd",      VT_U8, 0, 1, 63 },
	{ "edge_smooth_y", VT_U8, 1, 1, 255 },
	{ "edge_smooth_c", VT_U8, 2, 1, 127 },
};

static const IqFieldDesc false_color_fields[] = {
	{ "freq_thrd",       VT_U8, 0, 1, 255 },
	{ "edge_score_thrd", VT_U8, 1, 1, 255 },
	{ "chroma_thrd_max", VT_U8, 2, 1, 127 },
	{ "chroma_thrd_mid", VT_U8, 3, 1, 127 },
	{ "chroma_thrd_min", VT_U8, 4, 1, 127 },
	{ "strength_mid",    VT_U8, 5, 1, 7 },
	{ "strength_min",    VT_U8, 6, 1, 7 },
};

static const IqFieldDesc crosstalk_fields[] = {
	{ "strength",      VT_U8,  0,  1,  31 },
	{ "strength_by_y", VT_U8,  1,  15, 127 },
	{ "threshold",     VT_U8,  16, 1,  255 },
	{ "offset",        VT_U16, 18, 1,  4095 },
};

#define FIELDS(a) a, (sizeof(a) / sizeof(a[0]))

/*
 * Parameter table for Maruko (Infinity6C).
 *
 * Symbol names: Maruko uses CamelCase where star6e uses UPPERCASE.
 * Offsets: initial pass uses star6e offsets — parameters whose structs
 * differ will fail Get and show available:false. These get fixed as
 * we validate on hardware.
 *
 * Maruko-only params (not in star6e) added at the end.
 */
static IqParamDesc g_params[] = {
	/* ── Image quality (same symbol names on both platforms) ─────── */
	{ "lightness",    "MI_ISP_IQ_GetLightness",   "MI_ISP_IQ_SetLightness",
	  NULL, NULL, VT_U32, 72,   100, NULL, 0, 4103 },
	{ "contrast",     "MI_ISP_IQ_GetContrast",    "MI_ISP_IQ_SetContrast",
	  NULL, NULL, VT_U32, 72,   100, NULL, 0, 4101 },
	{ "brightness",   "MI_ISP_IQ_GetBrightness",  "MI_ISP_IQ_SetBrightness",
	  NULL, NULL, VT_U32, 72,   100, NULL, 0, 4102 },
	{ "saturation",   "MI_ISP_IQ_GetSaturation",  "MI_ISP_IQ_SetSaturation",
	  NULL, NULL, VT_U8,  392,  127, NULL, 0, 4106 },
	{ "sharpness",    "MI_ISP_IQ_GetSharpness",   "MI_ISP_IQ_SetSharpness",
	  NULL, NULL, VT_U8,  1192, 255, NULL, 0, 4114 },

	/* ── Noise reduction (Maruko symbol names) ─────────────────── */
	{ "nr3d",         "MI_ISP_IQ_GetNr3d",        "MI_ISP_IQ_SetNr3d",
	  NULL, NULL, VT_U8,  1288, 255, NULL, 0 },
	{ "nr_despike",   "MI_ISP_IQ_GetNrDeSpike",   "MI_ISP_IQ_SetNrDeSpike",
	  NULL, NULL, VT_U8,  552,  15,  NULL, 0 },
	{ "nr_luma_adv",  "MI_ISP_IQ_GetNrLumaAdv",   "MI_ISP_IQ_SetNrLumaAdv",
	  NULL, NULL, VT_U32, 1304, 1,   NULL, 0 },
	{ "nr_chroma_adv","MI_ISP_IQ_GetNrChromaAdv",  "MI_ISP_IQ_SetNrChromaAdv",
	  NULL, NULL, VT_U8,  488,  255, NULL, 0 },
	{ "nr_chroma_pre","MI_ISP_IQ_GetNrChromaPre",  "MI_ISP_IQ_SetNrChromaPre",
	  NULL, NULL, VT_U8,  4,    255, NULL, 0 },

	/* ── Corrections (same or CamelCase symbols) ───────────────── */
	{ "false_color",  "MI_ISP_IQ_GetFalseColor",  "MI_ISP_IQ_SetFalseColor",
	  NULL, NULL, VT_U8,  120,  255, FIELDS(false_color_fields) },
	{ "crosstalk",    "MI_ISP_IQ_GetCrossTalk",   "MI_ISP_IQ_SetCrossTalk",
	  NULL, NULL, VT_U8,  296,  31,  FIELDS(crosstalk_fields) },
	{ "demosaic",     "MI_ISP_IQ_GetDemosaic",    "MI_ISP_IQ_SetDemosaic",
	  NULL, NULL, VT_U8,  4,    63,  FIELDS(demosaic_fields) },
	{ "obc",          "MI_ISP_IQ_GetObc",          "MI_ISP_IQ_SetObc",
	  NULL, NULL, VT_U16, 136,  65535, FIELDS(obc_fields) },
	{ "dynamic_dp",   "MI_ISP_IQ_GetDynamicDp",   "MI_ISP_IQ_SetDynamicDp",
	  NULL, NULL, VT_U8,  488,  1,   NULL, 0 },
	{ "dp_cluster",   "MI_ISP_IQ_GetDynamicDpCluster", "MI_ISP_IQ_SetDynamicDpCluster",
	  NULL, NULL, VT_U32, 1080, 1,   NULL, 0 },
	{ "r2y",          "MI_ISP_IQ_GetR2Y",          "MI_ISP_IQ_SetR2Y",
	  NULL, NULL, VT_U16, 4,    1023, FIELDS(r2y_fields) },
	{ "colortrans",   "MI_ISP_IQ_GetColorTrans",  "MI_ISP_IQ_SetColorTrans",
	  NULL, NULL, VT_U16, 4,    2047, FIELDS(colortrans_fields) },
	{ "colortrans_ex","MI_ISP_IQ_GetColorTrans_EX","MI_ISP_IQ_SetColorTrans_EX",
	  NULL, NULL, VT_U16, 4,    65535, NULL, 0 },
	{ "rgb_matrix",   "MI_ISP_IQ_GetRgbMatrix",   "MI_ISP_IQ_SetRgbMatrix",
	  NULL, NULL, VT_U16, 444,  8191, NULL, 0 },
	{ "hsv",          "MI_ISP_IQ_GetHsv",          "MI_ISP_IQ_SetHsv",
	  NULL, NULL, VT_U8,  3096, 64,  NULL, 0 },

	/* ── Dynamic range ─────────────────────────────────────────── */
	{ "wdr",          "MI_ISP_IQ_GetWdr",          "MI_ISP_IQ_SetWdr",
	  NULL, NULL, VT_U8,  648,  4,    NULL, 0 },
	{ "wdr_curve_full","MI_ISP_IQ_GetWdrCurveFull","MI_ISP_IQ_SetWdrCurveFull",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "wdr_ltm",      "MI_ISP_IQ_GetWdrLtm",      "MI_ISP_IQ_SetWdrLtm",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "wdr_nr",       "MI_ISP_IQ_GetWdrNr",        "MI_ISP_IQ_SetWdrNr",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "pfc",          "MI_ISP_IQ_GetPfc",          "MI_ISP_IQ_SetPfc",
	  NULL, NULL, VT_U8,  376,  255, NULL, 0 },
	{ "pfc_ex",       "MI_ISP_IQ_GetPfcEx",        "MI_ISP_IQ_SetPfcEx",
	  NULL, NULL, VT_U32, 4,    1,   NULL, 0 },
	{ "hdr",          "MI_ISP_IQ_GetHdr",           "MI_ISP_IQ_SetHdr",
	  NULL, NULL, VT_U32, 904,  1,   NULL, 0 },
	{ "hdr_ex",       "MI_ISP_IQ_GetHdrEx",         "MI_ISP_IQ_SetHdrEx",
	  NULL, NULL, VT_U16, 4,    65535, NULL, 0 },
	{ "shp_ex",       "MI_ISP_IQ_GetShpEx",         "MI_ISP_IQ_SetShpEx",
	  NULL, NULL, VT_U32, 4,    1,   NULL, 0 },
	{ "nr3d_ex",      "MI_ISP_IQ_GetNr3dEx",        "MI_ISP_IQ_SetNr3dEx",
	  NULL, NULL, VT_U32, 4,    1,   NULL, 0 },
	{ "rgbir",        "MI_ISP_IQ_GetRgbir",         "MI_ISP_IQ_SetRgbir",
	  NULL, NULL, VT_U8,  600,  7,   NULL, 0 },

	/* ── Lens & sensor ─────────────────────────────────────────── */
	{ "lsc",          "MI_ISP_IQ_GetLsc",          "MI_ISP_IQ_SetLsc",
	  NULL, NULL, VT_U16, 4,    65535, NULL, 0 },
	{ "lsc_ctrl",     "MI_ISP_IQ_GetLscCtrl",      "MI_ISP_IQ_SetLscCtrl",
	  NULL, NULL, VT_U8,  4,    255, NULL, 0 },
	{ "alsc",         "MI_ISP_IQ_GetAlsc",          "MI_ISP_IQ_SetAlsc",
	  NULL, NULL, VT_U8,  4,    255, NULL, 0 },
	{ "alsc_ctrl",    "MI_ISP_IQ_GetAlscCtrl",      "MI_ISP_IQ_SetAlscCtrl",
	  NULL, NULL, VT_U8,  4,    255, NULL, 0 },
	{ "dark_shading", "MI_ISP_IQ_GetDarkShading",   "MI_ISP_IQ_SetDarkShading",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "obc_p1",       "MI_ISP_IQ_GetObcP1",         "MI_ISP_IQ_SetObcP1",
	  NULL, NULL, VT_U16, 136,  65535, FIELDS(obc_fields) },

	/* ── LUT-based ─────────────────────────────────────────────── */
	{ "rgb_gamma",    "MI_ISP_IQ_GetRgbGamma",    "MI_ISP_IQ_SetRgbGamma",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "yuv_gamma",    "MI_ISP_IQ_GetYuvGamma",    "MI_ISP_IQ_SetYuvGamma",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "adaptive_gamma","MI_ISP_IQ_GetAdaptiveGamma","MI_ISP_IQ_SetAdaptiveGamma",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },

	/* ── Toggle controls ───────────────────────────────────────── */
	{ "defog",        "MI_ISP_IQ_GetDefog",        "MI_ISP_IQ_SetDefog",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0, 4107 },
	{ "color_to_gray","MI_ISP_IQ_GetColorToGray",  "MI_ISP_IQ_SetColorToGray",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0, 4100 },
	{ "fpn",          "MI_ISP_IQ_GetFpn",           "MI_ISP_IQ_SetFpn",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },

	/* ── Maruko-only ───────────────────────────────────────────── */
	{ "temp",         "MI_ISP_IQ_GetTemp",          "MI_ISP_IQ_SetTemp",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "roi",          "MI_ISP_IQ_GetROI",           "MI_ISP_IQ_SetROI",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "day_night",    "MI_ISP_IQ_GetDayNightDetection","MI_ISP_IQ_SetDayNightDetection",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "alsc_adj",     "MI_ISP_IQ_GetALSC_ADJ",      "MI_ISP_IQ_SetALSC_ADJ",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "iq_mode",      "MI_ISP_IQ_GetIqMode",        "MI_ISP_IQ_SetIqMode",
	  NULL, NULL, VT_U32, 0,    1,   NULL, 0 },
	{ "dummy",        "MI_ISP_IQ_GetDummy",          "MI_ISP_IQ_SetDummy",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "dummy_ex",     "MI_ISP_IQ_GetDummyEx",        "MI_ISP_IQ_SetDummyEx",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },

	/* ── AE (auto-exposure) — may return 0xA0212209 with disabledBin ── */
	{ "ae_ev_comp",   "MI_ISP_AE_GetEvComp",      "MI_ISP_AE_SetEvComp",
	  NULL, NULL, VT_U32, 0,    200, NULL, 0 },
	{ "ae_mode",      "MI_ISP_AE_GetExpoMode",    "MI_ISP_AE_SetExpoMode",
	  NULL, NULL, VT_U32, 0,    4,   NULL, 0 },
	{ "ae_state",     "MI_ISP_AE_GetState",        "MI_ISP_AE_SetState",
	  NULL, NULL, VT_U32, 0,    1,   NULL, 0 },
	{ "ae_flicker",   "MI_ISP_AE_GetFlicker",      "MI_ISP_AE_SetFlicker",
	  NULL, NULL, VT_U32, 0,    3,   NULL, 0 },
	{ "ae_expo_limit","MI_ISP_AE_GetExposureLimit","MI_ISP_AE_SetExposureLimit",
	  NULL, NULL, VT_U32, 0,    65535, NULL, 0 },
	{ "ae_stabilizer","MI_ISP_AE_GetStabilizer",   "MI_ISP_AE_SetStabilizer",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },

	/* ── AWB (auto white balance) ──────────────────────────────── */
	{ "awb_attr_ex",  "MI_ISP_AWB_GetAttrEx",      "MI_ISP_AWB_SetAttrEx",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
	{ "awb_ct_weight","MI_ISP_AWB_GetCtWeight",    "MI_ISP_AWB_SetCtWeight",
	  NULL, NULL, VT_U16, 0,    65535, NULL, 0 },
	{ "awb_stabilizer","MI_ISP_AWB_GetStabilizer",  "MI_ISP_AWB_SetStabilizer",
	  NULL, NULL, VT_BOOL, 0,   1,   NULL, 0 },
};
#define NUM_PARAMS (sizeof(g_params) / sizeof(g_params[0]))

static void *g_isp_handle;

int maruko_iq_init(void)
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

	/* Resolve DUAL_OS IQ API path — shared memory + cache flush. */
	g_fn_api_set = (isp_api_fn_t)dlsym(g_isp_handle,
		"MI_ISP_SetIQApiData");
	g_fn_api_get = (isp_api_fn_t)dlsym(g_isp_handle,
		"MI_ISP_GetIQApiData");
	g_fn_alloc = (isp_alloc_fn_t)dlsym(g_isp_handle,
		"MI_ISP_Alloc_IQDataBuf");
	g_fn_flush = (sys_flush_fn_t)dlsym(RTLD_DEFAULT,
		"MI_SYS_FlushInvCache");
	printf("[iq] DUAL_OS API: Set=%s Get=%s Alloc=%s Flush=%s\n",
		g_fn_api_set ? "yes" : "NO",
		g_fn_api_get ? "yes" : "NO",
		g_fn_alloc ? "yes" : "NO",
		g_fn_flush ? "yes" : "NO");

	/* Ensure all IQ modules have API bypass OFF so that
	 * MI_ISP_IQ_Set* calls take effect on the image.
	 * ApiBypassMode struct: { bEnable(4), eAPIIndex(4) }
	 * bEnable=0 means bypass OFF (module active + API-writable). */
	typedef int (*bypass_fn_t)(uint32_t, uint32_t, void *);
	bypass_fn_t fn_bypass = (bypass_fn_t)dlsym(g_isp_handle,
		"MI_ISP_IQ_SetApiBypassMode");
	if (fn_bypass) {
		int bypass_ok = 0;
		/* Iterate all API module IDs (0..62) */
		for (uint32_t api_id = 0; api_id <= 62; api_id++) {
			struct { uint32_t enable; uint32_t index; } bp;
			bp.enable = 0;  /* E_SS_BYPASS_OFF */
			bp.index = api_id;
			int r = fn_bypass(0, 0, &bp);
			if (r == 0) bypass_ok++;
		}
		printf("[iq] API bypass OFF set for %d modules\n", bypass_ok);
	}

	printf("[iq] Maruko IQ parameter API ready (%d/%d params resolved)\n",
		resolved, (int)NUM_PARAMS);
	return 0;
}

void maruko_iq_cleanup(void)
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

/* ── Helpers (same as star6e_iq.c) ──────────────────────────────── */

static uint32_t read_value(const uint8_t *buf, uint32_t offset, IqValueType vt)
{
	switch (vt) {
	case VT_U32:
	case VT_BOOL: {
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

#define JSON_CHR(b, p, sz, c) do { \
	if ((size_t)(p) < (sz) - 1) (b)[(p)++] = (c); } while (0)
#define JSON_CLAMP(p, sz) do { \
	if ((p) >= (int)(sz)) (p) = (int)(sz) - 1; } while (0)

static int emit_fields_json(char *buf, size_t buf_size,
	const IqParamDesc *p, const uint8_t *iq_buf)
{
	int pos = 0;
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
			uint32_t elem_size = (fd->vtype == VT_U8) ? 1 :
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

/* ── Query ──────────────────────────────────────────────────────── */

char *maruko_iq_query(void)
{
	char *result;
	if (!g_isp_handle)
		return NULL;

	pthread_mutex_lock(&g_iq_mutex);

	char buf[16384];
	int pos = 0;
	static uint8_t iq_buf[IQ_BUF_SIZE];

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

		memset(iq_buf, 0, IQ_BUF_SIZE);
		int ret;
		if (p->api_id && g_fn_api_get && g_fn_alloc) {
			/* DUAL_OS Get: alloc shared buf, get, copy back */
			uint32_t sz = (p->vtype == VT_BOOL) ? 4 :
				(p->manual_offset > 0 ?
				 p->manual_offset + 4 : 256);
			void *shm_buf = NULL;
			if (g_fn_alloc(sz, &shm_buf) == 0 && shm_buf) {
				memset(shm_buf, 0, sz);
				IspApiHeader hdr = {
					.u32HeadSize = sizeof(IspApiHeader),
					.u32DataLen = sz,
					.u32CtrlID = p->api_id,
					.u32Channel = IQ_CHN,
					.u32DevId = IQ_DEV,
					.s32Ret = 0,
				};
				if (g_fn_flush)
					g_fn_flush(shm_buf, sz);
				ret = g_fn_api_get(&hdr, shm_buf);
				memcpy(iq_buf, shm_buf, sz);
			} else {
				ret = IQ_CALL(p->fn_get, iq_buf);
			}
		} else {
			ret = IQ_CALL(p->fn_get, iq_buf);
		}
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
		} else if (p->manual_offset > 4) {
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
		} else {
			/* manual_offset == 0 but not bool — raw value */
			uint32_t val = read_value(iq_buf, 0, p->vtype);
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"%s\":{\"ret\":%d,\"value\":%u}",
				p->name, ret, val);
		}
		if (i + 1 < NUM_PARAMS)
			JSON_CHR(buf, pos, sizeof(buf), ',');
	}

	/* ── Diagnostics ──────────────────────────────────────────── */
	pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ",\"_diag\":{");
	JSON_CLAMP(pos, sizeof(buf));
	{
		typedef int (*fn_ver_t)(uint32_t, uint32_t, void *);
		fn_ver_t fn_ver = (fn_ver_t)dlsym(g_isp_handle,
			"MI_ISP_IQ_GetVersionInfo");
		if (fn_ver) {
			uint32_t ver[3] = {0};
			int r = fn_ver(0, 0, ver);
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"version\":{\"ret\":%d,\"vendor\":%u,"
				"\"major\":%u,\"minor\":%u}",
				r, ver[0], ver[1], ver[2]);
			JSON_CLAMP(pos, sizeof(buf));
		}
	}
	pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "}}}");
	JSON_CLAMP(pos, sizeof(buf));

	result = strdup(buf);
	pthread_mutex_unlock(&g_iq_mutex);
	return result;
}

/* ── Set ────────────────────────────────────────────────────────── */

int maruko_iq_set(const char *param, const char *value)
{
	int rc;
	if (!g_isp_handle || !param || !value)
		return -1;

	pthread_mutex_lock(&g_iq_mutex);

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

	int ret;
	uint32_t data_len;
	if (target->vtype == VT_BOOL)
		data_len = 4;
	else if (target->manual_offset > 0)
		data_len = target->manual_offset + 4;
	else
		data_len = 256;

	if (target->api_id && g_fn_api_get && g_fn_alloc) {
		void *shm_buf = NULL;
		if (g_fn_alloc(data_len, &shm_buf) == 0 && shm_buf) {
			memcpy(shm_buf, iq_buf, data_len);
			IspApiHeader hdr = {
				.u32HeadSize = sizeof(IspApiHeader),
				.u32DataLen = data_len,
				.u32CtrlID = target->api_id,
				.u32Channel = IQ_CHN,
				.u32DevId = IQ_DEV,
				.s32Ret = 0,
			};
			if (g_fn_flush)
				g_fn_flush(shm_buf, data_len);
			ret = g_fn_api_get(&hdr, shm_buf);
			memcpy(iq_buf, shm_buf, data_len);
		} else {
			ret = IQ_CALL(target->fn_get, iq_buf);
		}
	} else {
		ret = IQ_CALL(target->fn_get, iq_buf);
	}
	if (ret != 0) {
		fprintf(stderr, "[iq] %s: Get failed: 0x%08x\n",
			param_name, (unsigned)ret);
		rc = -1;
		goto out;
	}

	if (field_name && strcmp(field_name, "enabled") == 0) {
		uint32_t en = (uint32_t)atoi(value);
		write_value(iq_buf, IQ_OFFSET_ENABLE, VT_U32, en ? 1 : 0);
		goto apply;
	}

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
		write_value(iq_buf, IQ_OFFSET_ENABLE, VT_U32, 1);
		if (target->manual_offset > 4)
			write_value(iq_buf, IQ_OFFSET_OPTYPE, VT_U32, 1);
		if (fd->count == 1) {
			uint32_t level = (uint32_t)atoi(value);
			if (level > fd->max_val) level = fd->max_val;
			write_value(iq_buf, abs_offset, fd->vtype, level);
		} else {
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
		} else if (target->manual_offset > 4) {
			write_value(iq_buf, IQ_OFFSET_ENABLE, VT_U32, 1);
			write_value(iq_buf, IQ_OFFSET_OPTYPE, VT_U32, 1);
			write_value(iq_buf, target->manual_offset,
				target->vtype, level);
		}
	}

apply:
	if (target->api_id) {
		/* Use MI_ISP_IQ_SetAll — the ONLY path majestic uses.
		 * Majestic does NOT use GetAll, GENERAL_*, SetIQApiData,
		 * or individual MI_ISP_IQ_Set* functions. */
		typedef int (*setall_fn_t)(uint32_t, uint32_t,
			uint16_t, uint32_t, uint8_t *);
		setall_fn_t fn_sa = (setall_fn_t)dlsym(g_isp_handle,
			"MI_ISP_IQ_SetAll");
		if (fn_sa) {
			ret = fn_sa(IQ_DEV, IQ_CHN, target->api_id,
				data_len, iq_buf);
			printf("[iq] %s: IQ_SetAll(id=%u len=%u) "
				"ret=%d (0x%x)\n",
				param, target->api_id, data_len,
				ret, (unsigned)ret);
		} else {
			ret = IQ_CALL(target->fn_set, iq_buf);
		}
	} else {
		ret = IQ_CALL(target->fn_set, iq_buf);
	}
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

/* ── Import ─────────────────────────────────────────────────────── */

int maruko_iq_import(const char *json_str)
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

	data = cJSON_GetObjectItemCaseSensitive(root, "data");
	if (!data)
		data = root;

	cJSON *item = NULL;
	cJSON_ArrayForEach(item, data) {
		const char *pname = item->string;
		if (!pname || pname[0] == '_')
			continue;

		cJSON *en_item = cJSON_GetObjectItemCaseSensitive(item, "enabled");
		if (en_item && cJSON_IsBool(en_item)) {
			char en_key[128];
			snprintf(en_key, sizeof(en_key), "%s.enabled", pname);
			const char *en_val = cJSON_IsTrue(en_item) ? "1" : "0";
			if (maruko_iq_set(en_key, en_val) == 0)
				applied++;
			else
				failed++;
		}

		fields_obj = cJSON_GetObjectItemCaseSensitive(item, "fields");
		if (fields_obj && cJSON_IsObject(fields_obj)) {
			cJSON *fld = NULL;
			cJSON_ArrayForEach(fld, fields_obj) {
				const char *fname = fld->string;
				if (!fname) continue;
				char key[128];
				snprintf(key, sizeof(key), "%s.%s", pname, fname);
				if (cJSON_IsArray(fld)) {
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
				if (maruko_iq_set(key, val_buf) == 0)
					applied++;
				else
					failed++;
			}
			continue;
		}

		cJSON *val_item = cJSON_GetObjectItemCaseSensitive(item, "value");
		if (!val_item) continue;

		if (cJSON_IsBool(val_item))
			snprintf(val_buf, sizeof(val_buf), "%d",
				cJSON_IsTrue(val_item) ? 1 : 0);
		else if (cJSON_IsNumber(val_item))
			snprintf(val_buf, sizeof(val_buf), "%d",
				val_item->valueint);
		else
			continue;

		if (maruko_iq_set(pname, val_buf) == 0)
			applied++;
		else
			failed++;
	}

	cJSON_Delete(root);
	printf("[iq] import: %d applied, %d failed\n", applied, failed);
	return failed > 0 ? -1 : 0;
}
