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
#define IQ_BUF_SIZE        8192

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

	/* ── Noise reduction ───────────────────────────────────────── */
	/* NR3D_PARAM_t = 80B, auto=1280, manual@1288 (u8MdThd) */
	{ "nr3d",         "MI_ISP_IQ_GetNR3D",        "MI_ISP_IQ_SetNR3D",
	  NULL, NULL, VT_U8,  1288, 255 },
	/* NRDESPIKE_PARAM_t = 39B, auto=624, manual@632 (u8BlendRatio) */
	{ "nr_despike",   "MI_ISP_IQ_GetNRDeSpike",   "MI_ISP_IQ_SetNRDeSpike",
	  NULL, NULL, VT_U8,  632,  15 },
	/* NRLUMA_PARAM_t = 6B, auto=96, manual@104 (u8Strength) */
	{ "nr_luma",      "MI_ISP_IQ_GetNRLuma",      "MI_ISP_IQ_SetNRLuma",
	  NULL, NULL, VT_U8,  104,  255 },
	/* NRCHROMA_PARAM_t = 13B, auto=208, manual@216 (u8MatchRatio) */
	{ "nr_chroma",    "MI_ISP_IQ_GetNRChroma",    "MI_ISP_IQ_SetNRChroma",
	  NULL, NULL, VT_U8,  216,  127 },

	/* ── Corrections ───────────────────────────────────────────── */
	/* FALSECOLOR_PARAM_t = 7B, auto=112, manual@120 (u8FreqThrd) */
	{ "false_color",  "MI_ISP_IQ_GetFalseColor",  "MI_ISP_IQ_SetFalseColor",
	  NULL, NULL, VT_U8,  120,  255 },
	/* CROSSTALK_PARAM_t = 18B, auto=288, manual@296 (u8Strength) */
	{ "crosstalk",    "MI_ISP_IQ_GetCrossTalk",   "MI_ISP_IQ_SetCrossTalk",
	  NULL, NULL, VT_U8,  296,  31 },
	/* DEMOSAIC: manual-only, bEnable(4) + stManual@4 (u8DirThrd) */
	{ "demosaic",     "MI_ISP_IQ_GetDEMOSAIC",    "MI_ISP_IQ_SetDEMOSAIC",
	  NULL, NULL, VT_U8,  4,    63 },
	/* OBC_PARAM_t = 8B, auto=128, manual@136 (u16ValR) */
	{ "obc",          "MI_ISP_IQ_GetOBC",          "MI_ISP_IQ_SetOBC",
	  NULL, NULL, VT_U16, 136,  255 },
	/* DYNAMIC_DP_PARAM_t = 30B, auto=480, manual@488 (bHotPixEn) */
	{ "dynamic_dp",   "MI_ISP_IQ_GetDynamicDP",   "MI_ISP_IQ_SetDynamicDP",
	  NULL, NULL, VT_U8,  488,  1 },

	/* ── Dynamic range & special ───────────────────────────────── */
	/* WDR_PARAM_t = 40B, auto=640, manual@648 (u8BoxNum) */
	{ "wdr",          "MI_ISP_IQ_GetWDR",          "MI_ISP_IQ_SetWDR",
	  NULL, NULL, VT_U8,  648,  4 },
	/* PFC_PARAM_t = 28B, auto=448, manual@456 (u8Strength) */
	{ "pfc",          "MI_ISP_IQ_GetPFC",          "MI_ISP_IQ_SetPFC",
	  NULL, NULL, VT_U8,  456,  255 },
	/* HDR_PARAM_t = 68B, auto=1088, manual@1096 (bNrEn) */
	{ "hdr",          "MI_ISP_IQ_GetHDR",          "MI_ISP_IQ_SetHDR",
	  NULL, NULL, VT_U8,  1096, 1 },
	/* RGBIR_PARAM_t = 37B, auto=592, manual@600 (u8IrPosType) */
	{ "rgbir",        "MI_ISP_IQ_GetRGBIR",        "MI_ISP_IQ_SetRGBIR",
	  NULL, NULL, VT_U8,  600,  7 },

	/* ── Toggle controls ───────────────────────────────────────── */
	{ "defog",        "MI_ISP_IQ_GetDefog",        "MI_ISP_IQ_SetDefog",
	  NULL, NULL, VT_BOOL, 0,   1 },
	{ "color_to_gray","MI_ISP_IQ_GetColorToGray",  "MI_ISP_IQ_SetColorToGray",
	  NULL, NULL, VT_BOOL, 0,   1 },
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

	char buf[8192];
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

		uint8_t iq_buf[IQ_BUF_SIZE];
		memset(iq_buf, 0, sizeof(iq_buf));
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

	pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "}}");
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

	uint8_t iq_buf[IQ_BUF_SIZE];
	memset(iq_buf, 0, sizeof(iq_buf));

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
