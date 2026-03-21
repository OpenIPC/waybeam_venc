/*
 * star6e_iq.c — Direct IQ parameter interface for SigmaStar ISP
 *
 * Exposes MI_ISP_IQ_* functions as HTTP-queryable/settable parameters.
 * Uses cached dlopen handle (opened once at init, closed at cleanup).
 *
 * All IQ structs follow the pattern:
 *   offset 0: bEnable   (uint32_t, 0=off 1=on)
 *   offset 4: enOpType  (uint32_t, 0=auto 1=manual)
 *   offset 8+: auto attrs, then manual attrs
 *
 * For simple level params (contrast, brightness, saturation, sharpness),
 * the manual value is expected at a fixed offset within the struct.
 * Since exact struct layouts are not in SDK headers, we use oversized
 * 8KB buffers and include raw hex in query output for on-device
 * verification of offsets.
 */

#include "star6e_iq.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int MI_S32;
typedef MI_S32 (*iq_fn_t)(uint32_t channel, void *param);

/* IQ struct field offsets (from SDK mi_isp_iq_datatype.h):
 *   All level structs: { bEnable(4), enOpType(4), stAuto(N*16), stManual }
 *   color_to_gray: just { bEnable(4) }
 *
 * Struct sizes and manual value offsets:
 *   contrast:    76 bytes, manual u32Lev at offset 72 (0-100)
 *   brightness:  76 bytes, manual u32Lev at offset 72 (0-100)
 *   saturation: 416 bytes, manual u8SatAllStr at offset 392 (0-127, 32=1X)
 *   sharpness: 1268 bytes, manual u8OverShootGain at offset 1192 (0-255, complex)
 */
#define IQ_OFFSET_ENABLE   0
#define IQ_OFFSET_OPTYPE   4
#define IQ_BUF_SIZE        8192

typedef enum {
	VT_U32,     /* uint32_t level (contrast, brightness) */
	VT_U8,      /* uint8_t level (saturation, sharpness) */
	VT_BOOL,    /* color_to_gray: just bEnable */
} IqValueType;

typedef struct {
	const char *name;
	const char *get_sym;
	const char *set_sym;
	iq_fn_t     fn_get;
	iq_fn_t     fn_set;
	IqValueType vtype;
	uint32_t    manual_offset;  /* byte offset of primary manual value */
	uint32_t    max_val;        /* max valid value */
} IqParamDesc;

static IqParamDesc g_params[] = {
	{ "contrast",      "MI_ISP_IQ_GetContrast",    "MI_ISP_IQ_SetContrast",
	  NULL, NULL, VT_U32, 72,   100 },
	{ "brightness",    "MI_ISP_IQ_GetBrightness",  "MI_ISP_IQ_SetBrightness",
	  NULL, NULL, VT_U32, 72,   100 },
	{ "saturation",    "MI_ISP_IQ_GetSaturation",  "MI_ISP_IQ_SetSaturation",
	  NULL, NULL, VT_U8,  392,  127 },
	{ "sharpness",     "MI_ISP_IQ_GetSharpness",   "MI_ISP_IQ_SetSharpness",
	  NULL, NULL, VT_U8,  1192, 255 },
	{ "color_to_gray", "MI_ISP_IQ_GetColorToGray", "MI_ISP_IQ_SetColorToGray",
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

/* Format first N bytes as hex string for debugging struct layout */
static void hex_dump(const uint8_t *data, size_t len, char *out, size_t out_sz)
{
	size_t pos = 0;
	for (size_t i = 0; i < len && pos + 3 < out_sz; i++) {
		pos += (size_t)snprintf(out + pos, out_sz - pos,
			"%02x", data[i]);
	}
}

char *star6e_iq_query(void)
{
	if (!g_isp_handle)
		return NULL;

	char buf[4096];
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

		uint32_t enable = 0, optype = 0;
		memcpy(&enable, iq_buf + IQ_OFFSET_ENABLE, sizeof(enable));

		char hex[65];
		hex_dump(iq_buf, 32, hex, sizeof(hex));

		if (p->vtype == VT_BOOL) {
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"%s\":{\"ret\":%d,\"value\":%s}%s",
				p->name, ret,
				enable ? "true" : "false",
				(i + 1 < NUM_PARAMS) ? "," : "");
		} else {
			memcpy(&optype, iq_buf + IQ_OFFSET_OPTYPE,
				sizeof(optype));
			/* Read the manual value at the known offset */
			uint32_t manual_val = 0;
			if (p->vtype == VT_U32)
				memcpy(&manual_val, iq_buf + p->manual_offset,
					sizeof(uint32_t));
			else
				manual_val = iq_buf[p->manual_offset];
			pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
				"\"%s\":{\"ret\":%d,\"enabled\":%s,"
				"\"op_type\":\"%s\",\"value\":%u}%s",
				p->name, ret,
				enable ? "true" : "false",
				optype == 1 ? "manual" : "auto",
				manual_val,
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
		/* color_to_gray: struct is just { bEnable } */
		uint32_t v = level ? 1 : 0;
		memcpy(iq_buf + IQ_OFFSET_ENABLE, &v, sizeof(v));
	} else {
		/* Level params: set bEnable=1, enOpType=1 (manual),
		 * write level at the SDK-defined manual value offset */
		uint32_t enable = 1;
		uint32_t optype = 1; /* SS_OP_TYP_MANUAL */
		memcpy(iq_buf + IQ_OFFSET_ENABLE, &enable, sizeof(enable));
		memcpy(iq_buf + IQ_OFFSET_OPTYPE, &optype, sizeof(optype));

		if (target->vtype == VT_U32)
			memcpy(iq_buf + target->manual_offset, &level,
				sizeof(uint32_t));
		else
			iq_buf[target->manual_offset] = (uint8_t)level;
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
