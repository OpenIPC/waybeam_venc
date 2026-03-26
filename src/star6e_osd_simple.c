#include "star6e_osd_simple.h"

#include "star6e.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef MI_SUCCESS
#define MI_SUCCESS 0
#endif

typedef unsigned long long MI_PHY;
typedef unsigned long MI_VIRT;

#include "mi_rgn_datatype.h"

#define STAR6E_OSD_DOT_HANDLE 0u
#define STAR6E_OSD_DOT_LAYER 0u
#define STAR6E_OSD_DEFAULT_W 1920u
#define STAR6E_OSD_DEFAULT_H 1080u

typedef MI_U32 MI_RGN_HANDLE;

MI_S32 MI_RGN_Init(MI_RGN_PaletteTable_t *pstPaletteTable);
MI_S32 MI_RGN_DeInit(void);
MI_S32 MI_RGN_Create(MI_RGN_HANDLE hHandle, MI_RGN_Attr_t *pstRegion);
MI_S32 MI_RGN_Destroy(MI_RGN_HANDLE hHandle);
MI_S32 MI_RGN_AttachToChn(MI_RGN_HANDLE hHandle,
	MI_RGN_ChnPort_t *pstChnPort, MI_RGN_ChnPortParam_t *pstChnAttr);
MI_S32 MI_RGN_DetachFromChn(MI_RGN_HANDLE hHandle,
	MI_RGN_ChnPort_t *pstChnPort);
MI_S32 MI_RGN_GetCanvasInfo(MI_RGN_HANDLE hHandle,
	MI_RGN_CanvasInfo_t *pstCanvasInfo);
MI_S32 MI_RGN_UpdateCanvas(MI_RGN_HANDLE hHandle);

typedef struct {
	int initialized;
	int region_created;
	int region_attached;
	int target_configured;
	int marker_drawn;
	unsigned int trace_updates;
	unsigned int current_color;
	unsigned int current_x;
	unsigned int current_y;
	unsigned int track_point_count;
	unsigned int track_point_x[STAR6E_OSD_TRACK_POINT_COUNT];
	unsigned int track_point_y[STAR6E_OSD_TRACK_POINT_COUNT];
	unsigned int canvas_width;
	unsigned int canvas_height;
	MI_RGN_ChnPort_t chn_port;
} Star6eOsdState;

static Star6eOsdState g_star6e_osd;

static void init_palette(MI_RGN_PaletteTable_t *palette)
{
	static const MI_RGN_PaletteElement_t palette_entries[] = {
		{255, 0, 0, 0},
		{0xFF, 0xFF, 0x00, 0x00},
		{0xFF, 0x00, 0xFF, 0x00},
		{0xFF, 0x00, 0x00, 0xFF},
		{0xFF, 0xF8, 0xF8, 0x00},
		{0xFF, 0xF8, 0x00, 0xF8},
		{0xFF, 0x00, 0xF8, 0xF8},
		{0xFF, 0xFF, 0xFF, 0xFF},
		{0xFF, 0x00, 0x00, 0x00},
		{0x6F, 0x00, 0x00, 0x00},
		{0xFF, 0xA2, 0x08, 0x08},
		{0xFF, 0x63, 0x18, 0xC6},
		{0xFF, 0xAD, 0x52, 0xD6},
		{0xFF, 0xCC, 0xCC, 0xCC},
		{0xFF, 0x77, 0x77, 0x77},
		{0x00, 0x00, 0x00, 0x00},
		{0x00, 0x00, 0x00, 0x00},
		{0xFF, 0xF0, 0xF0, 0xF0},
	};

	memset(palette, 0, sizeof(*palette));
	memcpy(palette->astElement, palette_entries, sizeof(palette_entries));
}

static unsigned int normalize_color(unsigned int color)
{
	if (color == STAR6E_OSD_COLOR_RED)
		return STAR6E_OSD_COLOR_RED;
	if (color == STAR6E_OSD_COLOR_GREEN)
		return STAR6E_OSD_COLOR_GREEN;
	if (color == STAR6E_OSD_COLOR_WHITE)
		return STAR6E_OSD_COLOR_WHITE;
	if (color == STAR6E_OSD_COLOR_TRANSPARENT)
		return STAR6E_OSD_COLOR_TRANSPARENT;
	return STAR6E_OSD_COLOR_RED;
}

static unsigned int clamp_coord(unsigned int value, unsigned int max_value)
{
	if (value > max_value)
		return max_value;
	return value;
}

static void fill_osd_attr(MI_RGN_ChnPortParam_t *chn_attr)
{
	memset(chn_attr, 0, sizeof(*chn_attr));
	chn_attr->bShow = (MI_BOOL)1;
	chn_attr->stPoint.u32X = 0;
	chn_attr->stPoint.u32Y = 0;
	chn_attr->unPara.stOsdChnPort.u32Layer = STAR6E_OSD_DOT_LAYER;
	chn_attr->unPara.stOsdChnPort.stOsdAlphaAttr.eAlphaMode =
		E_MI_RGN_PIXEL_ALPHA;
	chn_attr->unPara.stOsdChnPort.stOsdAlphaAttr.stAlphaPara
		.stArgb1555Alpha.u8BgAlpha = 0;
	chn_attr->unPara.stOsdChnPort.stOsdAlphaAttr.stAlphaPara
		.stArgb1555Alpha.u8FgAlpha = 255;
}

static void log_rgn_info(const char *message)
{
	fprintf(stdout, "[star6e_osd_simple] %s\n", message);
	fflush(stdout);
}

static int should_trace_update(void)
{
	return g_star6e_osd.trace_updates < 12u;
}

static void log_rgn_error(const char *step, MI_S32 ret)
{
	fprintf(stderr, "[star6e_osd_simple] %s failed: ret=%d\n", step, ret);
	fflush(stderr);
}

static MI_S32 attach_region_port(const MI_RGN_ChnPort_t *chn_port,
	const MI_RGN_ChnPortParam_t *chn_attr)
{
	return MI_RGN_AttachToChn(STAR6E_OSD_DOT_HANDLE,
		(MI_RGN_ChnPort_t *)chn_port,
		(MI_RGN_ChnPortParam_t *)chn_attr);
}

static void draw_i4_point(uint8_t *base, MI_U32 stride,
	unsigned int x, unsigned int y, unsigned int color)
{
	uint8_t *pixel = base + (stride * y) + (x / 2u);
	uint8_t nibble = (uint8_t)(color & 0x0f);

	if (x & 1u)
		*pixel = (uint8_t)((*pixel & 0x0f) | (nibble << 4));
	else
		*pixel = (uint8_t)((*pixel & 0xf0) | nibble);
}

static void draw_rect(uint8_t *base, MI_U32 stride,
	unsigned int canvas_width, unsigned int canvas_height,
	unsigned int origin_x, unsigned int origin_y,
	unsigned int rect_width, unsigned int rect_height,
	unsigned int color)
{
	unsigned int x;
	unsigned int y;
	unsigned int max_x;
	unsigned int max_y;

	if (canvas_width == 0 || canvas_height == 0)
		return;
	if (canvas_width <= rect_width)
		origin_x = 0;
	else {
		max_x = canvas_width - rect_width;
		origin_x = clamp_coord(origin_x, max_x);
	}
	if (canvas_height <= rect_height)
		origin_y = 0;
	else {
		max_y = canvas_height - rect_height;
		origin_y = clamp_coord(origin_y, max_y);
	}

	for (y = 0; y < rect_height && (origin_y + y) < canvas_height; ++y) {
		for (x = 0; x < rect_width && (origin_x + x) < canvas_width; ++x)
			draw_i4_point(base, stride, origin_x + x, origin_y + y, color);
	}
}

static void draw_centered_rect(uint8_t *base, MI_U32 stride,
	unsigned int canvas_width, unsigned int canvas_height,
	unsigned int center_x, unsigned int center_y,
	unsigned int rect_width, unsigned int rect_height,
	unsigned int color)
{
	unsigned int origin_x = 0;
	unsigned int origin_y = 0;

	if (center_x > (rect_width / 2u))
		origin_x = center_x - (rect_width / 2u);
	if (center_y > (rect_height / 2u))
		origin_y = center_y - (rect_height / 2u);

	draw_rect(base, stride, canvas_width, canvas_height,
		origin_x, origin_y, rect_width, rect_height, color);
}

static void clear_canvas(uint8_t *base, MI_U32 stride,
	unsigned int canvas_height)
{
	memset(base, 0xFF, stride * canvas_height);
}

static void draw_track_points(uint8_t *base, MI_U32 stride,
	unsigned int canvas_width, unsigned int canvas_height)
{
	unsigned int index;

	for (index = 0; index < g_star6e_osd.track_point_count; ++index) {
		draw_centered_rect(base, stride, canvas_width, canvas_height,
			g_star6e_osd.track_point_x[index],
			g_star6e_osd.track_point_y[index],
			STAR6E_OSD_TRACK_POINT_W,
			STAR6E_OSD_TRACK_POINT_H,
			STAR6E_OSD_COLOR_GREEN);
	}
}

static MI_S32 update_dot_canvas(unsigned int x, unsigned int y,
	unsigned int color)
{
	MI_RGN_CanvasInfo_t canvas_info;
	uint8_t *base;
	MI_S32 ret;

	memset(&canvas_info, 0, sizeof(canvas_info));
	if (should_trace_update()) {
		fprintf(stdout,
			"[star6e_osd_simple] trace update-begin x=%u y=%u color=%u\n",
			x, y, normalize_color(color));
		fflush(stdout);
	}
	ret = MI_RGN_GetCanvasInfo(STAR6E_OSD_DOT_HANDLE, &canvas_info);
	if (ret != MI_RGN_OK) {
		log_rgn_error("MI_RGN_GetCanvasInfo", ret);
		return ret;
	}
	if (should_trace_update()) {
		fprintf(stdout,
			"[star6e_osd_simple] trace got-canvas stride=%u size=%ux%u addr=0x%llx\n",
			canvas_info.u32Stride,
			canvas_info.stSize.u32Width,
			canvas_info.stSize.u32Height,
			(unsigned long long)canvas_info.virtAddr);
		fflush(stdout);
	}

	base = (uint8_t *)(uintptr_t)canvas_info.virtAddr;
	clear_canvas(base, canvas_info.u32Stride, canvas_info.stSize.u32Height);
	draw_track_points(base, canvas_info.u32Stride,
		canvas_info.stSize.u32Width, canvas_info.stSize.u32Height);
	draw_rect(base, canvas_info.u32Stride,
		canvas_info.stSize.u32Width, canvas_info.stSize.u32Height,
		x, y, STAR6E_OSD_DOT_W, STAR6E_OSD_DOT_H,
		normalize_color(color));

	ret = MI_RGN_UpdateCanvas(STAR6E_OSD_DOT_HANDLE);
	if (ret != MI_RGN_OK) {
		log_rgn_error("MI_RGN_UpdateCanvas", ret);
		return ret;
	}
	if (should_trace_update()) {
		fprintf(stdout,
			"[star6e_osd_simple] trace update-end x=%u y=%u\n",
			x, y);
		fflush(stdout);
	}

	g_star6e_osd.current_x = x;
	g_star6e_osd.current_y = y;
	g_star6e_osd.current_color = normalize_color(color);
	g_star6e_osd.marker_drawn = 1;
	g_star6e_osd.trace_updates++;
	return MI_RGN_OK;
}

int star6e_osd_set_track_points(const unsigned int *x_points,
	const unsigned int *y_points, unsigned int count)
{
	unsigned int index;

	if (!x_points || !y_points)
		count = 0;
	if (count > STAR6E_OSD_TRACK_POINT_COUNT)
		count = STAR6E_OSD_TRACK_POINT_COUNT;

	g_star6e_osd.track_point_count = count;
	for (index = 0; index < count; ++index) {
		g_star6e_osd.track_point_x[index] = x_points[index];
		g_star6e_osd.track_point_y[index] = y_points[index];
	}
	for (; index < STAR6E_OSD_TRACK_POINT_COUNT; ++index) {
		g_star6e_osd.track_point_x[index] = 0;
		g_star6e_osd.track_point_y[index] = 0;
	}

	return MI_RGN_OK;
}

static int fill_target_port(MI_RGN_ChnPort_t *chn_port,
	const MI_SYS_ChnPort_t *sys_port)
{
	if (!sys_port)
		return -1;
	if (sys_port->module != I6_SYS_MOD_VPE)
		return -1;

	memset(chn_port, 0, sizeof(*chn_port));
	chn_port->eModId = E_MI_RGN_MODID_VPE;
	chn_port->s32DevId = sys_port->device;
	chn_port->s32ChnId = sys_port->channel;
	chn_port->s32OutputPortId = sys_port->port;
	return 0;
}

int star6e_osd_set_vpe_target(const void *chn_port)
{
	const MI_SYS_ChnPort_t *sys_port = (const MI_SYS_ChnPort_t *)chn_port;

	if (g_star6e_osd.region_attached)
		return -1;
	if (fill_target_port(&g_star6e_osd.chn_port, sys_port) != 0)
		return -1;

	g_star6e_osd.target_configured = 1;
	fprintf(stdout,
		"[star6e_osd_simple] target module configured: mod=%d dev=%d chn=%d port=%d\n",
		g_star6e_osd.chn_port.eModId,
		g_star6e_osd.chn_port.s32DevId,
		g_star6e_osd.chn_port.s32ChnId,
		g_star6e_osd.chn_port.s32OutputPortId);
	fflush(stdout);
	return MI_RGN_OK;
}

int star6e_osd_set_canvas_size(unsigned int width, unsigned int height)
{
	if (g_star6e_osd.region_created || g_star6e_osd.region_attached)
		return -1;
	if (width == 0 || height == 0)
		return -1;

	g_star6e_osd.canvas_width = width;
	g_star6e_osd.canvas_height = height;
	fprintf(stdout,
		"[star6e_osd_simple] canvas size configured: %ux%u\n",
		g_star6e_osd.canvas_width,
		g_star6e_osd.canvas_height);
	fflush(stdout);
	return MI_RGN_OK;
}

static MI_S32 ensure_rgn_ready(void)
{
	MI_RGN_PaletteTable_t palette;
	MI_S32 ret;

	if (g_star6e_osd.initialized)
		return MI_RGN_OK;
	if (!g_star6e_osd.target_configured) {
		fprintf(stderr,
			"[star6e_osd_simple] target channel is not configured\n");
		fflush(stderr);
		return -1;
	}

	init_palette(&palette);
	ret = MI_RGN_Init(&palette);
	if (ret != MI_RGN_OK) {
		log_rgn_error("MI_RGN_Init", ret);
		return ret;
	}

	g_star6e_osd.initialized = 1;
	if (g_star6e_osd.canvas_width == 0 || g_star6e_osd.canvas_height == 0) {
		g_star6e_osd.canvas_width = STAR6E_OSD_DEFAULT_W;
		g_star6e_osd.canvas_height = STAR6E_OSD_DEFAULT_H;
	}
	log_rgn_info("region runtime initialized");
	return MI_RGN_OK;
}

int star6e_osd_add_dot_region(unsigned int x, unsigned int y,
	unsigned int color)
{
	MI_RGN_Attr_t region_attr;
	MI_RGN_ChnPortParam_t chn_attr;
	MI_S32 ret;

	if (g_star6e_osd.region_attached)
		return MI_RGN_OK;

	ret = ensure_rgn_ready();
	if (ret != MI_RGN_OK)
		return ret;

	memset(&region_attr, 0, sizeof(region_attr));
	region_attr.eType = E_MI_RGN_TYPE_OSD;
	region_attr.stOsdInitParam.ePixelFmt = E_MI_RGN_PIXEL_FORMAT_I4;
	region_attr.stOsdInitParam.stSize.u32Width = g_star6e_osd.canvas_width;
	region_attr.stOsdInitParam.stSize.u32Height = g_star6e_osd.canvas_height;
	ret = MI_RGN_Create(STAR6E_OSD_DOT_HANDLE, &region_attr);
	if (ret != MI_RGN_OK) {
		log_rgn_error("MI_RGN_Create", ret);
		(void)MI_RGN_DeInit();
		g_star6e_osd.initialized = 0;
		return ret;
	}
	g_star6e_osd.region_created = 1;

	fill_osd_attr(&chn_attr);

	ret = attach_region_port(&g_star6e_osd.chn_port, &chn_attr);
	if (ret != MI_RGN_OK) {
		log_rgn_error("MI_RGN_AttachToChn", ret);
		(void)MI_RGN_Destroy(STAR6E_OSD_DOT_HANDLE);
		g_star6e_osd.region_created = 0;
		(void)MI_RGN_DeInit();
		g_star6e_osd.initialized = 0;
		return ret;
	}

	g_star6e_osd.region_attached = 1;
	g_star6e_osd.marker_drawn = 0;
	ret = update_dot_canvas(x, y, color);
	if (ret != MI_RGN_OK) {
		(void)MI_RGN_DetachFromChn(STAR6E_OSD_DOT_HANDLE,
			&g_star6e_osd.chn_port);
		(void)MI_RGN_Destroy(STAR6E_OSD_DOT_HANDLE);
		(void)MI_RGN_DeInit();
		g_star6e_osd.region_attached = 0;
		g_star6e_osd.region_created = 0;
		g_star6e_osd.initialized = 0;
		return ret;
	}

	log_rgn_info("osd region attached to target channel");
	return MI_RGN_OK;
}

int star6e_osd_move_dot_region(unsigned int x, unsigned int y,
	unsigned int color)
{
	MI_S32 ret;

	ret = ensure_rgn_ready();
	if (ret != MI_RGN_OK)
		return ret;
	if (!g_star6e_osd.region_attached)
		return -1;

	ret = update_dot_canvas(x, y, color);
	if (ret != MI_RGN_OK)
		return ret;

	return MI_RGN_OK;
}

int star6e_osd_remove_dot_region(void)
{
	if (!g_star6e_osd.initialized)
		return MI_RGN_OK;

	if (g_star6e_osd.region_attached) {
		(void)MI_RGN_DetachFromChn(STAR6E_OSD_DOT_HANDLE,
			&g_star6e_osd.chn_port);
		g_star6e_osd.region_attached = 0;
		log_rgn_info("osd region detached");
	}
	if (g_star6e_osd.region_created) {
		(void)MI_RGN_Destroy(STAR6E_OSD_DOT_HANDLE);
		g_star6e_osd.region_created = 0;
		log_rgn_info("osd region destroyed");
	}
	(void)MI_RGN_DeInit();
	memset(&g_star6e_osd, 0, sizeof(g_star6e_osd));
	log_rgn_info("region runtime deinitialized");
	return MI_RGN_OK;
}