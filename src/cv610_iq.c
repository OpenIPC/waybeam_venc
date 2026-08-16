/*
 * cv610_iq.c — runtime IQ control for the Hi3516CV610 ISP.
 *
 * The IMX662 sensor plugin seeds this ISP once, at pipe start, through
 * cmos_get_isp_default() (PR #229).  This module exposes the same blocks as
 * live knobs so a value can be corrected without a cross-compile, a plugin
 * deploy and a reboot.
 *
 * Two things differ from star6e_iq.c and make this the simpler backend:
 *
 *   - No dlopen.  The CV610 backend links ss_mpi directly, so the attribute
 *     structs are the SDK's own types.  Every field offset comes from
 *     offsetof(), not from a hand-computed constant that can drift when the
 *     SDK header changes.
 *   - The table is self-describing.  query() emits a "_schema" block so the
 *     WebUI renders the knob list this file declares, rather than carrying a
 *     second copy of it.
 *
 * Ranges in the field table are the "Range:" comments from the SDK's
 * ot_common_isp.h.  Values are clamped to them before the write.
 */

#include "cv610_iq.h"
#include "cv610_pipeline.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ot_type.h"
#include "ot_common.h"
#include "ot_common_video.h"
#include "ot_common_isp.h"
#include "ss_mpi_isp.h"
#include "ss_mpi_awb.h"

/* The backend runs a single VI pipe; cv610_pipeline.c calls it VI_PIPE 0. */
#define CV610_IQ_PIPE 0

/* td_bool and ot_op_mode are enums, so both are int-sized.  The field table
 * encodes them as FT_S32; assert that rather than trusting it. */
typedef char cv610_iq_bool_is_int[(sizeof(td_bool) == 4) ? 1 : -1];
typedef char cv610_iq_mode_is_int[(sizeof(ot_op_mode) == 4) ? 1 : -1];

typedef enum {
	FT_U8,
	FT_S8,
	FT_U16,
	FT_S16,
	FT_S32,   /* also carries td_bool and ot_op_mode */
} Cv610IqType;

typedef struct {
	const char *name;
	Cv610IqType type;
	uint16_t    offset;   /* offsetof() into the group's attr struct */
	uint16_t    count;    /* 1 = scalar, N = array */
	int32_t     min;
	int32_t     max;
	/* Writing a manual_attr field while the block runs its auto curve has
	 * no visible effect, which reads exactly like a broken setter.  Fields
	 * marked here force the group's op_type to manual on write. */
	uint8_t     forces_manual;
} Cv610IqField;

typedef struct {
	const char *name;
	int       (*get)(void *attr);
	int       (*set)(const void *attr);
	size_t      size;
	int32_t     op_type_offset;  /* -1 when the group has no op_type */
	const Cv610IqField *fields;
	uint16_t    field_count;
} Cv610IqGroup;

#define F_RO 0
#define F_MAN 1

/* ── MPI wrappers ───────────────────────────────────────────────────────
 * Typed one-liners rather than casting function pointers: converting a
 * function pointer to a different signature and calling through it is
 * undefined behaviour, and void * -> struct * is a plain implicit
 * conversion. */

#define IQ_ACCESSORS(group, type, getter, setter)                          \
	static int iq_get_##group(void *a) { return getter(CV610_IQ_PIPE, a); } \
	static int iq_set_##group(const void *a) { return setter(CV610_IQ_PIPE, a); }

IQ_ACCESSORS(saturation, ot_isp_saturation_attr,
	ss_mpi_isp_get_saturation_attr, ss_mpi_isp_set_saturation_attr)
IQ_ACCESSORS(color_tone, ot_isp_color_tone_attr,
	ss_mpi_isp_get_color_tone_attr, ss_mpi_isp_set_color_tone_attr)
IQ_ACCESSORS(csc, ot_isp_csc_attr,
	ss_mpi_isp_get_csc_attr, ss_mpi_isp_set_csc_attr)
IQ_ACCESSORS(ccm, ot_isp_color_matrix_attr,
	ss_mpi_isp_get_ccm_attr, ss_mpi_isp_set_ccm_attr)
IQ_ACCESSORS(wb, ot_isp_wb_attr,
	ss_mpi_isp_get_wb_attr, ss_mpi_isp_set_wb_attr)
IQ_ACCESSORS(sharpen, ot_isp_sharpen_attr,
	ss_mpi_isp_get_sharpen_attr, ss_mpi_isp_set_sharpen_attr)
IQ_ACCESSORS(nr, ot_isp_nr_attr,
	ss_mpi_isp_get_nr_attr, ss_mpi_isp_set_nr_attr)
IQ_ACCESSORS(drc, ot_isp_drc_attr,
	ss_mpi_isp_get_drc_attr, ss_mpi_isp_set_drc_attr)
IQ_ACCESSORS(ldci, ot_isp_ldci_attr,
	ss_mpi_isp_get_ldci_attr, ss_mpi_isp_set_ldci_attr)
IQ_ACCESSORS(dehaze, ot_isp_dehaze_attr,
	ss_mpi_isp_get_dehaze_attr, ss_mpi_isp_set_dehaze_attr)
IQ_ACCESSORS(ca, ot_isp_ca_attr,
	ss_mpi_isp_get_ca_attr, ss_mpi_isp_set_ca_attr)

#undef IQ_ACCESSORS

/* ── Field tables ───────────────────────────────────────────────────────
 * Ranges are the SDK header's documented limits.  Fields the CV610 does not
 * support (motion sharpen, tnr, dering, cp LUT, radial crop) are omitted
 * rather than exposed as knobs that do nothing. */

#define OFS(t, m) ((uint16_t)offsetof(t, m))

static const Cv610IqField f_saturation[] = {
	{ "op_type",           FT_S32, OFS(ot_isp_saturation_attr, op_type),
		1, 0, 1, F_RO },
	{ "manual.saturation", FT_U8,  OFS(ot_isp_saturation_attr, manual_attr.saturation),
		1, 0, 255, F_MAN },
	/* Indexed by AGC bucket; 128 == nominal.  This is the curve the sensor
	 * plugin's g_imx662_awb_agc_table seeds. */
	{ "auto.sat",          FT_U8,  OFS(ot_isp_saturation_attr, auto_attr.sat),
		OT_ISP_AUTO_ISO_NUM, 0, 255, F_RO },
};

static const Cv610IqField f_color_tone[] = {
	{ "red_cast_gain",   FT_U16, OFS(ot_isp_color_tone_attr, red_cast_gain),
		1, 256, 384, F_RO },
	{ "green_cast_gain", FT_U16, OFS(ot_isp_color_tone_attr, green_cast_gain),
		1, 256, 384, F_RO },
	{ "blue_cast_gain",  FT_U16, OFS(ot_isp_color_tone_attr, blue_cast_gain),
		1, 256, 384, F_RO },
};

static const Cv610IqField f_csc[] = {
	{ "enable",           FT_S32, OFS(ot_isp_csc_attr, enable), 1, 0, 1, F_RO },
	{ "hue",              FT_U8,  OFS(ot_isp_csc_attr, hue),    1, 0, 100, F_RO },
	{ "luma",             FT_U8,  OFS(ot_isp_csc_attr, luma),   1, 0, 100, F_RO },
	{ "contr",            FT_U8,  OFS(ot_isp_csc_attr, contr),  1, 0, 100, F_RO },
	{ "satu",             FT_U8,  OFS(ot_isp_csc_attr, satu),   1, 0, 100, F_RO },
	{ "limited_range_en", FT_S32, OFS(ot_isp_csc_attr, limited_range_en),
		1, 0, 1, F_RO },
};

static const Cv610IqField f_ccm[] = {
	{ "op_type",          FT_S32, OFS(ot_isp_color_matrix_attr, op_type),
		1, 0, 1, F_RO },
	{ "manual.sat_en",    FT_S32, OFS(ot_isp_color_matrix_attr, manual_attr.sat_en),
		1, 0, 1, F_MAN },
	{ "manual.ccm",       FT_U16, OFS(ot_isp_color_matrix_attr, manual_attr.ccm),
		OT_ISP_CCM_MATRIX_SIZE, 0, 65535, F_MAN },
	/* cv610_pipeline.c's enable_sensor_ccm() clears both of these so the
	 * auto CCM never bypasses; they are surfaced to make that visible. */
	{ "auto.iso_act_en",  FT_S32, OFS(ot_isp_color_matrix_attr, auto_attr.iso_act_en),
		1, 0, 1, F_RO },
	{ "auto.temp_act_en", FT_S32, OFS(ot_isp_color_matrix_attr, auto_attr.temp_act_en),
		1, 0, 1, F_RO },
};

static const Cv610IqField f_wb[] = {
	{ "bypass",         FT_S32, OFS(ot_isp_wb_attr, bypass),  1, 0, 1, F_RO },
	{ "op_type",        FT_S32, OFS(ot_isp_wb_attr, op_type), 1, 0, 1, F_RO },
	{ "manual.r_gain",  FT_U16, OFS(ot_isp_wb_attr, manual_attr.r_gain),
		1, 0, 4095, F_MAN },
	{ "manual.gr_gain", FT_U16, OFS(ot_isp_wb_attr, manual_attr.gr_gain),
		1, 0, 4095, F_MAN },
	{ "manual.gb_gain", FT_U16, OFS(ot_isp_wb_attr, manual_attr.gb_gain),
		1, 0, 4095, F_MAN },
	{ "manual.b_gain",  FT_U16, OFS(ot_isp_wb_attr, manual_attr.b_gain),
		1, 0, 4095, F_MAN },
};

static const Cv610IqField f_sharpen[] = {
	{ "enable",                  FT_S32, OFS(ot_isp_sharpen_attr, enable),
		1, 0, 1, F_RO },
	{ "op_type",                 FT_S32, OFS(ot_isp_sharpen_attr, op_type),
		1, 0, 1, F_RO },
	{ "manual.texture_strength", FT_U16, OFS(ot_isp_sharpen_attr, manual_attr.texture_strength),
		OT_ISP_SHARPEN_GAIN_NUM, 0, 4095, F_MAN },
	{ "manual.edge_strength",    FT_U16, OFS(ot_isp_sharpen_attr, manual_attr.edge_strength),
		OT_ISP_SHARPEN_GAIN_NUM, 0, 4095, F_MAN },
	{ "manual.texture_freq",     FT_U16, OFS(ot_isp_sharpen_attr, manual_attr.texture_freq),
		1, 0, 4095, F_MAN },
	{ "manual.edge_freq",        FT_U16, OFS(ot_isp_sharpen_attr, manual_attr.edge_freq),
		1, 0, 4095, F_MAN },
	{ "manual.over_shoot",       FT_U8,  OFS(ot_isp_sharpen_attr, manual_attr.over_shoot),
		1, 0, 127, F_MAN },
	{ "manual.under_shoot",      FT_U8,  OFS(ot_isp_sharpen_attr, manual_attr.under_shoot),
		1, 0, 127, F_MAN },
	{ "manual.shoot_sup_strength", FT_U8, OFS(ot_isp_sharpen_attr, manual_attr.shoot_sup_strength),
		1, 0, 255, F_MAN },
	{ "manual.detail_ctrl",      FT_U8,  OFS(ot_isp_sharpen_attr, manual_attr.detail_ctrl),
		1, 0, 255, F_MAN },
	{ "manual.edge_filt_strength", FT_U8, OFS(ot_isp_sharpen_attr, manual_attr.edge_filt_strength),
		1, 0, 63, F_MAN },
	{ "manual.max_sharp_gain",   FT_U16, OFS(ot_isp_sharpen_attr, manual_attr.max_sharp_gain),
		1, 0, 2047, F_MAN },
	{ "manual.luma_wgt",         FT_U8,  OFS(ot_isp_sharpen_attr, manual_attr.luma_wgt),
		OT_ISP_SHARPEN_LUMA_NUM, 0, 31, F_MAN },
};

/* Bayer NR.  Only the switches are exposed: the strength LUTs are the
 * sc450ai-derived tables PR #229 borrowed, and re-tuning those is a
 * calibration exercise, not a slider. */
static const Cv610IqField f_nr[] = {
	{ "enable",  FT_S32, OFS(ot_isp_nr_attr, enable),  1, 0, 1, F_RO },
	{ "op_type", FT_S32, OFS(ot_isp_nr_attr, op_type), 1, 0, 1, F_RO },
};

static const Cv610IqField f_drc[] = {
	{ "enable",             FT_S32, OFS(ot_isp_drc_attr, enable),  1, 0, 1, F_RO },
	{ "op_type",            FT_S32, OFS(ot_isp_drc_attr, op_type), 1, 0, 1, F_RO },
	{ "manual.strength",    FT_U16, OFS(ot_isp_drc_attr, manual_attr.strength),
		1, 0, 1023, F_MAN },
	{ "auto.strength",      FT_U16, OFS(ot_isp_drc_attr, auto_attr.strength),
		1, 0, 1023, F_RO },
	{ "auto.strength_max",  FT_U16, OFS(ot_isp_drc_attr, auto_attr.strength_max),
		1, 0, 1023, F_RO },
	{ "auto.strength_min",  FT_U16, OFS(ot_isp_drc_attr, auto_attr.strength_min),
		1, 0, 1023, F_RO },
	{ "contrast_ctrl",      FT_U8,  OFS(ot_isp_drc_attr, contrast_ctrl),
		1, 0, 15, F_RO },
	{ "detail_adjust_coef", FT_U8,  OFS(ot_isp_drc_attr, detail_adjust_coef),
		1, 0, 15, F_RO },
	{ "bright_gain_limit",  FT_U8,  OFS(ot_isp_drc_attr, bright_gain_limit),
		1, 0, 15, F_RO },
	{ "dark_gain_limit_luma", FT_U8, OFS(ot_isp_drc_attr, dark_gain_limit_luma),
		1, 0, 133, F_RO },
};

static const Cv610IqField f_ldci[] = {
	{ "enable",           FT_S32, OFS(ot_isp_ldci_attr, enable),  1, 0, 1, F_RO },
	{ "op_type",          FT_S32, OFS(ot_isp_ldci_attr, op_type), 1, 0, 1, F_RO },
	{ "gauss_lpf_sigma",  FT_U8,  OFS(ot_isp_ldci_attr, gauss_lpf_sigma),
		1, 1, 255, F_RO },
	{ "manual.blc_ctrl",  FT_U16, OFS(ot_isp_ldci_attr, manual_attr.blc_ctrl),
		1, 0, 511, F_MAN },
	{ "tpr_incr_coef",    FT_U16, OFS(ot_isp_ldci_attr, tpr_incr_coef),
		1, 0, 256, F_RO },
	{ "tpr_decr_coef",    FT_U16, OFS(ot_isp_ldci_attr, tpr_decr_coef),
		1, 0, 256, F_RO },
};

static const Cv610IqField f_dehaze[] = {
	{ "enable",          FT_S32, OFS(ot_isp_dehaze_attr, enable),  1, 0, 1, F_RO },
	{ "op_type",         FT_S32, OFS(ot_isp_dehaze_attr, op_type), 1, 0, 1, F_RO },
	{ "manual.strength", FT_U8,  OFS(ot_isp_dehaze_attr, manual_attr.strength),
		1, 0, 255, F_MAN },
	{ "auto.strength",   FT_U8,  OFS(ot_isp_dehaze_attr, auto_attr.strength),
		1, 0, 255, F_RO },
};

static const Cv610IqField f_ca[] = {
	{ "enable", FT_S32, OFS(ot_isp_ca_attr, enable), 1, 0, 1, F_RO },
};

#undef OFS

#define GROUP(n, t, ops) \
	{ #n, iq_get_##n, iq_set_##n, sizeof(t), ops, f_##n, \
	  (uint16_t)(sizeof(f_##n) / sizeof(f_##n[0])) }

static const Cv610IqGroup g_groups[] = {
	GROUP(saturation, ot_isp_saturation_attr,
		(int32_t)offsetof(ot_isp_saturation_attr, op_type)),
	GROUP(color_tone, ot_isp_color_tone_attr, -1),
	GROUP(csc,        ot_isp_csc_attr,        -1),
	GROUP(ccm,        ot_isp_color_matrix_attr,
		(int32_t)offsetof(ot_isp_color_matrix_attr, op_type)),
	GROUP(wb,         ot_isp_wb_attr,
		(int32_t)offsetof(ot_isp_wb_attr, op_type)),
	GROUP(sharpen,    ot_isp_sharpen_attr,
		(int32_t)offsetof(ot_isp_sharpen_attr, op_type)),
	GROUP(nr,         ot_isp_nr_attr,
		(int32_t)offsetof(ot_isp_nr_attr, op_type)),
	GROUP(drc,        ot_isp_drc_attr,
		(int32_t)offsetof(ot_isp_drc_attr, op_type)),
	GROUP(ldci,       ot_isp_ldci_attr,
		(int32_t)offsetof(ot_isp_ldci_attr, op_type)),
	GROUP(dehaze,     ot_isp_dehaze_attr,
		(int32_t)offsetof(ot_isp_dehaze_attr, op_type)),
	GROUP(ca,         ot_isp_ca_attr,         -1),
};

#undef GROUP

#define NUM_GROUPS (sizeof(g_groups) / sizeof(g_groups[0]))

/* Largest attribute struct, with the right alignment for every member. */
typedef union {
	ot_isp_saturation_attr   saturation;
	ot_isp_color_tone_attr   color_tone;
	ot_isp_csc_attr          csc;
	ot_isp_color_matrix_attr ccm;
	ot_isp_wb_attr           wb;
	ot_isp_sharpen_attr      sharpen;
	ot_isp_nr_attr           nr;
	ot_isp_drc_attr          drc;
	ot_isp_ldci_attr         ldci;
	ot_isp_dehaze_attr       dehaze;
	ot_isp_ca_attr           ca;
} Cv610IqAttr;

/* The scratch attribute is shared by query and set; both run on the httpd
 * thread today, but the ISP thread is a second writer of the same MPI. */
static pthread_mutex_t g_iq_mutex = PTHREAD_MUTEX_INITIALIZER;
static Cv610IqAttr     g_attr;

/* ── Field access ───────────────────────────────────────────────────────── */

static size_t field_elem_size(Cv610IqType t)
{
	switch (t) {
	case FT_U8:
	case FT_S8:  return 1;
	case FT_U16:
	case FT_S16: return 2;
	case FT_S32: return 4;
	}
	return 4;
}

static int32_t field_read(const void *attr, const Cv610IqField *f, uint16_t idx)
{
	const uint8_t *p = (const uint8_t *)attr + f->offset +
		idx * field_elem_size(f->type);

	switch (f->type) {
	case FT_U8:  return (int32_t)*p;
	case FT_S8:  return (int32_t)*(const int8_t *)p;
	case FT_U16: { uint16_t v; memcpy(&v, p, sizeof(v)); return (int32_t)v; }
	case FT_S16: { int16_t  v; memcpy(&v, p, sizeof(v)); return (int32_t)v; }
	case FT_S32: { int32_t  v; memcpy(&v, p, sizeof(v)); return v; }
	}
	return 0;
}

static void field_write(void *attr, const Cv610IqField *f, uint16_t idx, int32_t val)
{
	uint8_t *p = (uint8_t *)attr + f->offset + idx * field_elem_size(f->type);

	if (val < f->min)
		val = f->min;
	if (val > f->max)
		val = f->max;

	switch (f->type) {
	case FT_U8:  *p = (uint8_t)val; return;
	case FT_S8:  *(int8_t *)p = (int8_t)val; return;
	case FT_U16: { uint16_t v = (uint16_t)val; memcpy(p, &v, sizeof(v)); return; }
	case FT_S16: { int16_t  v = (int16_t)val;  memcpy(p, &v, sizeof(v)); return; }
	case FT_S32: { int32_t  v = val;           memcpy(p, &v, sizeof(v)); return; }
	}
}

/* ── JSON emission ──────────────────────────────────────────────────────── */

#define JSON_CLAMP(p, sz) do { \
	if ((p) >= (int)(sz)) (p) = (int)(sz) - 1; } while (0)

#define JSON_PUT(buf, pos, sz, ...) do { \
	(pos) += snprintf((buf) + (pos), (sz) - (size_t)(pos), __VA_ARGS__); \
	JSON_CLAMP(pos, sz); } while (0)

static int emit_group_values(char *buf, size_t sz, int pos,
	const Cv610IqGroup *g, const void *attr)
{
	for (uint16_t i = 0; i < g->field_count; i++) {
		const Cv610IqField *f = &g->fields[i];

		JSON_PUT(buf, pos, sz, "%s\"%s\":", i ? "," : "", f->name);
		if (f->count == 1) {
			JSON_PUT(buf, pos, sz, "%d", field_read(attr, f, 0));
			continue;
		}
		JSON_PUT(buf, pos, sz, "[");
		for (uint16_t e = 0; e < f->count; e++)
			JSON_PUT(buf, pos, sz, "%s%d",
				e ? "," : "", field_read(attr, f, e));
		JSON_PUT(buf, pos, sz, "]");
	}
	return pos;
}

/* The WebUI builds its knob list from this, so the table here stays the only
 * copy of the field set. */
static int emit_schema(char *buf, size_t sz, int pos)
{
	JSON_PUT(buf, pos, sz, "\"_schema\":[");
	for (size_t i = 0; i < NUM_GROUPS; i++) {
		const Cv610IqGroup *g = &g_groups[i];

		JSON_PUT(buf, pos, sz, "%s{\"name\":\"%s\",\"fields\":[",
			i ? "," : "", g->name);
		for (uint16_t f = 0; f < g->field_count; f++) {
			const Cv610IqField *fd = &g->fields[f];
			JSON_PUT(buf, pos, sz,
				"%s{\"name\":\"%s\",\"count\":%u,"
				"\"min\":%d,\"max\":%d,\"forces_manual\":%s}",
				f ? "," : "", fd->name,
				(unsigned)fd->count, fd->min, fd->max,
				fd->forces_manual ? "true" : "false");
		}
		JSON_PUT(buf, pos, sz, "]}");
	}
	JSON_PUT(buf, pos, sz, "]");
	return pos;
}

/* Read-only: which ISP blocks the hardware is bypassing right now.  This is
 * the control for "did enabling that block actually reach silicon" — a
 * seeded-but-bypassed block looks identical to one that was never seeded. */
static int emit_module_ctrl(char *buf, size_t sz, int pos)
{
	ot_isp_module_ctrl mod;
	int ret;

	memset(&mod, 0, sizeof(mod));
	ret = ss_mpi_isp_get_module_ctrl(CV610_IQ_PIPE, &mod);
	JSON_PUT(buf, pos, sz, "\"module_ctrl\":{\"ret\":%d", ret);
	if (ret == 0) {
		JSON_PUT(buf, pos, sz,
			",\"bypass\":{\"anti_false_color\":%u,\"crosstalk\":%u,"
			"\"dpc\":%u,\"nr\":%u,\"dehaze\":%u,\"wb_gain\":%u,"
			"\"mesh_shading\":%u,\"drc\":%u,\"demosaic\":%u,"
			"\"color_matrix\":%u,\"gamma\":%u,\"ca\":%u,\"csc\":%u,"
			"\"sharpen\":%u,\"cac\":%u,\"ldci\":%u}",
			(unsigned)mod.bit_bypass_anti_false_color,
			(unsigned)mod.bit_bypass_crosstalk_removal,
			(unsigned)mod.bit_bypass_dpc,
			(unsigned)mod.bit_bypass_nr,
			(unsigned)mod.bit_bypass_dehaze,
			(unsigned)mod.bit_bypass_wb_gain,
			(unsigned)mod.bit_bypass_mesh_shading,
			(unsigned)mod.bit_bypass_drc,
			(unsigned)mod.bit_bypass_demosaic,
			(unsigned)mod.bit_bypass_color_matrix,
			(unsigned)mod.bit_bypass_gamma,
			(unsigned)mod.bit_bypass_ca,
			(unsigned)mod.bit_bypass_csc,
			(unsigned)mod.bit_bypass_sharpen,
			(unsigned)mod.bit_bypass_cac,
			(unsigned)mod.bit_bypass_ldci);
	}
	JSON_PUT(buf, pos, sz, "}");
	return pos;
}

char *cv610_iq_query(void)
{
	static char buf[24576];
	int pos = 0;
	char *result;

	if (!cv610_pipeline_isp_ready())
		return NULL;

	pthread_mutex_lock(&g_iq_mutex);

	JSON_PUT(buf, pos, sizeof(buf), "{\"ok\":true,\"data\":{");
	pos = emit_schema(buf, sizeof(buf), pos);

	for (size_t i = 0; i < NUM_GROUPS; i++) {
		const Cv610IqGroup *g = &g_groups[i];
		int ret;

		memset(&g_attr, 0, sizeof(g_attr));
		ret = g->get(&g_attr);

		JSON_PUT(buf, pos, sizeof(buf), ",\"%s\":{\"ret\":%d",
			g->name, ret);
		if (ret == 0) {
			JSON_PUT(buf, pos, sizeof(buf), ",\"fields\":{");
			pos = emit_group_values(buf, sizeof(buf), pos, g, &g_attr);
			JSON_PUT(buf, pos, sizeof(buf), "}");
		}
		JSON_PUT(buf, pos, sizeof(buf), "}");
	}

	JSON_PUT(buf, pos, sizeof(buf), ",");
	pos = emit_module_ctrl(buf, sizeof(buf), pos);
	JSON_PUT(buf, pos, sizeof(buf), "}}");

	result = strdup(buf);
	pthread_mutex_unlock(&g_iq_mutex);
	return result;
}

/* ── Set ────────────────────────────────────────────────────────────────── */

/* strtol with the whole-token check the caller needs: "12abc" and "" are
 * rejected rather than silently read as 12 and 0. */
static int parse_i32(const char *s, const char **end, int32_t *out)
{
	char *stop = NULL;
	long v;

	while (*s == ' ')
		s++;
	if (*s == '\0')
		return -1;
	v = strtol(s, &stop, 10);
	if (stop == s)
		return -1;
	if (v < INT32_MIN || v > INT32_MAX)
		return -1;
	while (*stop == ' ')
		stop++;
	if (*stop != '\0' && *stop != ',')
		return -1;
	*out = (int32_t)v;
	*end = stop;
	return 0;
}

int cv610_iq_set(const char *param, const char *value)
{
	const Cv610IqGroup *group = NULL;
	const Cv610IqField *field = NULL;
	char group_name[32];
	const char *field_name;
	const char *dot;
	size_t glen;
	int ret;
	int rc = -1;

	if (!param || !value)
		return -1;
	if (!cv610_pipeline_isp_ready()) {
		fprintf(stderr, "[cv610-iq] ISP is not running\n");
		return -1;
	}

	/* Group and field split on the FIRST dot: field names carry their own
	 * dot ("manual.saturation"), which is what makes the auto/manual half
	 * of the struct visible in the name. */
	dot = strchr(param, '.');
	if (!dot) {
		fprintf(stderr, "[cv610-iq] %s: expected <group>.<field>\n", param);
		return -1;
	}
	glen = (size_t)(dot - param);
	if (glen == 0 || glen >= sizeof(group_name)) {
		fprintf(stderr, "[cv610-iq] %s: bad group name\n", param);
		return -1;
	}
	memcpy(group_name, param, glen);
	group_name[glen] = '\0';
	field_name = dot + 1;

	for (size_t i = 0; i < NUM_GROUPS; i++) {
		if (strcmp(g_groups[i].name, group_name) == 0) {
			group = &g_groups[i];
			break;
		}
	}
	if (!group) {
		fprintf(stderr, "[cv610-iq] unknown group: %s\n", group_name);
		return -1;
	}
	for (uint16_t i = 0; i < group->field_count; i++) {
		if (strcmp(group->fields[i].name, field_name) == 0) {
			field = &group->fields[i];
			break;
		}
	}
	if (!field) {
		fprintf(stderr, "[cv610-iq] %s: unknown field: %s\n",
			group_name, field_name);
		return -1;
	}

	pthread_mutex_lock(&g_iq_mutex);

	/* Read-modify-write: the attribute structs carry far more state than
	 * this table describes, and a zeroed struct would wipe the sensor
	 * plugin's seeds. */
	memset(&g_attr, 0, sizeof(g_attr));
	ret = group->get(&g_attr);
	if (ret != 0) {
		fprintf(stderr, "[cv610-iq] %s: get failed: 0x%x\n",
			group_name, (unsigned)ret);
		goto out;
	}

	if (field->count == 1) {
		int32_t v;
		const char *end;
		if (parse_i32(value, &end, &v) != 0 || *end == ',') {
			fprintf(stderr, "[cv610-iq] %s: expected one integer\n",
				param);
			goto out;
		}
		field_write(&g_attr, field, 0, v);
	} else {
		const char *p = value;
		uint16_t n = 0;
		while (n < field->count) {
			int32_t v;
			const char *end;
			if (parse_i32(p, &end, &v) != 0)
				break;
			field_write(&g_attr, field, n, v);
			n++;
			if (*end != ',')
				break;
			p = end + 1;
		}
		/* A short list would leave the tail at whatever the ISP held,
		 * which reads as a partly-applied curve.  Require all of it. */
		if (n != field->count) {
			fprintf(stderr, "[cv610-iq] %s: expected %u values, got %u\n",
				param, (unsigned)field->count, (unsigned)n);
			goto out;
		}
	}

	/* Manual fields are inert while the block runs its auto curve. */
	if (field->forces_manual && group->op_type_offset >= 0) {
		int32_t manual = OT_OP_MODE_MANUAL;
		memcpy((uint8_t *)&g_attr + group->op_type_offset,
			&manual, sizeof(manual));
	}

	ret = group->set(&g_attr);
	if (ret != 0) {
		fprintf(stderr, "[cv610-iq] %s: set failed: 0x%x\n",
			param, (unsigned)ret);
		goto out;
	}

	printf("[cv610-iq] %s = %s\n", param, value);
	rc = 0;

out:
	pthread_mutex_unlock(&g_iq_mutex);
	return rc;
}
