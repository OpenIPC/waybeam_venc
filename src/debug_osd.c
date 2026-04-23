#include "debug_osd.h"

#ifdef PLATFORM_STAR6E

#include "debug_osd_draw.h"
#include "sigmastar_types.h"  /* i6_sys_bind */

#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── MI_RGN types ──────────────────────────────────────────────────────
 * Defined locally because the SDK headers (sdk/ssc338q/include/i6_rgn.h)
 * are not on the include path.  Layouts match the SigmaStar vendor SDK. */

typedef enum {
	I6_RGN_PIXFMT_ARGB1555,
	I6_RGN_PIXFMT_ARGB4444,
	I6_RGN_PIXFMT_I2,
	I6_RGN_PIXFMT_I4,
	I6_RGN_PIXFMT_I8,
	I6_RGN_PIXFMT_RGB565,
	I6_RGN_PIXFMT_ARGB888,
} i6_rgn_pixfmt;

typedef enum {
	I6_RGN_TYPE_OSD,
	I6_RGN_TYPE_COVER,
} i6_rgn_type;

typedef struct { unsigned int width; unsigned int height; } i6_rgn_size;

typedef struct {
	i6_rgn_type type;
	i6_rgn_pixfmt pixFmt;
	i6_rgn_size size;
} i6_rgn_cnf;

typedef struct {
	int invColOn;
	int lowThanThresh;
	unsigned int lumThresh;
	unsigned short divWidth;
	unsigned short divHeight;
} i6_rgn_inv;

typedef struct {
	unsigned int layer;
	int constAlphaOn;
	union {
		unsigned char bgFgAlpha[2];
		unsigned char constAlpha[2];
	};
	i6_rgn_inv invert;
} i6_rgn_osd;

typedef struct { unsigned int x; unsigned int y; } i6_rgn_pnt;

typedef struct {
	unsigned int layer;
	i6_rgn_size size;
	unsigned int color;
} i6_rgn_cov;

typedef struct {
	int show;
	i6_rgn_pnt point;
	union {
		i6_rgn_cov cover;
		i6_rgn_osd osd;
	};
} i6_rgn_chn;

typedef struct {
	unsigned char alpha, red, green, blue;
} i6_rgn_pale;

typedef struct {
	i6_rgn_pale element[256];
} i6_rgn_pal;

/* CanvasInfo ABI — not in SDK header.  Matches MI_RGN_CanvasInfo_t.
 * ARM32: MI_PHY=uint64_t (8B), MI_VIRT=unsigned long (4B). */
typedef struct {
	uint64_t phyAddr;
	unsigned long virtAddr;
	struct { uint32_t u32Width; uint32_t u32Height; } stSize;
	uint32_t u32Stride;
	int ePixelFmt;
} DebugOsdCanvasInfo;

#define RGN_HANDLE 0

/* ── State ─────────────────────────────────────────────────────────── */

struct DebugOsdState {
	void *lib;
	uint32_t width, height;
	DebugOsdCanvasInfo canvas;
	i6_sys_bind vpe_bind;
	OsdDirty dirty;           /* previous frame's drawn area */
	int font_scale;           /* pixel scaling factor for text */

	/* CPU usage sampler (from /proc/stat) */
	unsigned long long cpu_prev_total, cpu_prev_idle;
	int cpu_pct;               /* last sampled CPU% */
	struct timespec cpu_ts;    /* last sample time */

	int (*fnInit)(i6_rgn_pal *);
	int (*fnDeinit)(void);
	int (*fnCreateRegion)(unsigned int, i6_rgn_cnf *);
	int (*fnDestroyRegion)(unsigned int);
	int (*fnAttachChannel)(unsigned int, i6_sys_bind *, i6_rgn_chn *);
	int (*fnDetachChannel)(unsigned int, i6_sys_bind *);
	int (*fnGetCanvasInfo)(unsigned int, DebugOsdCanvasInfo *);
	int (*fnUpdateCanvas)(unsigned int);
};

static void osd_canvas_from_info(OsdCanvas *out, const DebugOsdCanvasInfo *info,
                                 uint32_t width, uint32_t height)
{
	out->pixels = (uint8_t *)(uintptr_t)info->virtAddr;
	out->stride_px = info->u32Stride;  /* I8: 1 byte/pixel */
	out->width = width;
	out->height = height;
}

/* Copy the shared host-side palette into the SDK's i6_rgn_pal shape. */
static void palette_init(i6_rgn_pal *pal)
{
	const OsdPaletteEntry *src = osd_palette();
	for (unsigned i = 0; i < OSD_PALETTE_SIZE; i++) {
		pal->element[i].alpha = src[i].alpha;
		pal->element[i].red   = src[i].red;
		pal->element[i].green = src[i].green;
		pal->element[i].blue  = src[i].blue;
	}
}

/* ── dlopen ────────────────────────────────────────────────────────── */

static int rgn_load(DebugOsdState *ctx)
{
	ctx->lib = dlopen("libmi_rgn.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!ctx->lib) {
		fprintf(stderr, "[debug_osd] Cannot load libmi_rgn.so: %s\n",
			dlerror());
		return -1;
	}

#define LOAD_SYM(field, name) do { \
	ctx->field = dlsym(ctx->lib, name); \
	if (!ctx->field) { \
		fprintf(stderr, "[debug_osd] Missing symbol: %s\n", name); \
		dlclose(ctx->lib); \
		ctx->lib = NULL; \
		return -1; \
	} \
} while (0)

	LOAD_SYM(fnInit,           "MI_RGN_Init");
	LOAD_SYM(fnDeinit,         "MI_RGN_DeInit");
	LOAD_SYM(fnCreateRegion,   "MI_RGN_Create");
	LOAD_SYM(fnDestroyRegion,  "MI_RGN_Destroy");
	LOAD_SYM(fnAttachChannel,  "MI_RGN_AttachToChn");
	LOAD_SYM(fnDetachChannel,  "MI_RGN_DetachFromChn");
	LOAD_SYM(fnGetCanvasInfo,  "MI_RGN_GetCanvasInfo");
	LOAD_SYM(fnUpdateCanvas,   "MI_RGN_UpdateCanvas");

#undef LOAD_SYM
	return 0;
}

/* ── CPU usage sampler ─────────────────────────────────────────────── */

static void cpu_sample(DebugOsdState *osd)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long ms = (now.tv_sec - osd->cpu_ts.tv_sec) * 1000 +
	          (now.tv_nsec - osd->cpu_ts.tv_nsec) / 1000000;
	if (ms < 500) return;  /* sample at most 2 Hz */

	FILE *f = fopen("/proc/stat", "r");
	if (!f) return;

	unsigned long long user, nice, sys, idle, iowait, irq, softirq;
	if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu",
	           &user, &nice, &sys, &idle, &iowait, &irq, &softirq) != 7) {
		fclose(f);
		return;
	}
	fclose(f);

	unsigned long long total = user + nice + sys + idle + iowait + irq + softirq;
	unsigned long long idle_all = idle + iowait;

	if (osd->cpu_prev_total > 0) {
		unsigned long long dt = total - osd->cpu_prev_total;
		unsigned long long di = idle_all - osd->cpu_prev_idle;
		osd->cpu_pct = dt > 0 ? (int)((dt - di) * 100 / dt) : 0;
	}

	osd->cpu_prev_total = total;
	osd->cpu_prev_idle = idle_all;
	osd->cpu_ts = now;
}

/* ── Public API ────────────────────────────────────────────────────── */

DebugOsdState *debug_osd_create(uint32_t frame_w, uint32_t frame_h,
                                const void *vpe_port)
{
	DebugOsdState *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) return NULL;

	ctx->width = frame_w;
	ctx->height = frame_h;
	ctx->font_scale = 3;
	/* MI_RGN uses its own module ID enum (VPE=0), not the system
	 * i6_sys_mod enum (where VPE=11).  Build the RGN ChnPort manually. */
	ctx->vpe_bind.module = 0;  /* E_MI_RGN_MODID_VPE */
	ctx->vpe_bind.device = 0;
	ctx->vpe_bind.channel = 0;
	ctx->vpe_bind.port = 0;

	if (rgn_load(ctx) != 0) {
		free(ctx);
		return NULL;
	}

	/* Verify CanvasInfo ABI assumptions */
	fprintf(stderr, "[debug_osd] CanvasInfo sizeof=%zu stride_off=%zu\n",
		sizeof(DebugOsdCanvasInfo),
		offsetof(DebugOsdCanvasInfo, u32Stride));

	/* Init RGN subsystem with our fixed palette */
	i6_rgn_pal pal;
	palette_init(&pal);
	if (ctx->fnInit(&pal) != 0) {
		fprintf(stderr, "[debug_osd] MI_RGN_Init failed\n");
		dlclose(ctx->lib);
		free(ctx);
		return NULL;
	}

	/* Create OSD region: full frame, I8 (palette-indexed, 8 bpp) */
	i6_rgn_cnf cnf;
	memset(&cnf, 0, sizeof(cnf));
	cnf.type = I6_RGN_TYPE_OSD;
	cnf.pixFmt = I6_RGN_PIXFMT_I8;
	cnf.size.width = frame_w;
	cnf.size.height = frame_h;

	if (ctx->fnCreateRegion(RGN_HANDLE, &cnf) != 0) {
		fprintf(stderr, "[debug_osd] MI_RGN_Create failed (%ux%u)\n",
			frame_w, frame_h);
		ctx->fnDeinit();
		dlclose(ctx->lib);
		free(ctx);
		return NULL;
	}

	/* Attach to VPE channel — pixel-alpha, layer 0 */
	i6_rgn_chn chn;
	memset(&chn, 0, sizeof(chn));
	chn.show = 1;
	chn.point.x = 0;
	chn.point.y = 0;
	chn.osd.layer = 0;
	chn.osd.constAlphaOn = 0;

	if (ctx->fnAttachChannel(RGN_HANDLE, &ctx->vpe_bind, &chn) != 0) {
		fprintf(stderr, "[debug_osd] MI_RGN_AttachToChn failed\n");
		ctx->fnDestroyRegion(RGN_HANDLE);
		ctx->fnDeinit();
		dlclose(ctx->lib);
		free(ctx);
		return NULL;
	}

	/* Get canvas memory mapping */
	if (ctx->fnGetCanvasInfo(RGN_HANDLE, &ctx->canvas) != 0) {
		fprintf(stderr, "[debug_osd] MI_RGN_GetCanvasInfo failed\n");
		ctx->fnDetachChannel(RGN_HANDLE, &ctx->vpe_bind);
		ctx->fnDestroyRegion(RGN_HANDLE);
		ctx->fnDeinit();
		dlclose(ctx->lib);
		free(ctx);
		return NULL;
	}

	/* Clear canvas to transparent on first create */
	{
		uint8_t *pixels = (uint8_t *)(uintptr_t)ctx->canvas.virtAddr;
		uint32_t stride = ctx->canvas.u32Stride;
		for (uint32_t y = 0; y < frame_h; y++)
			memset(pixels + y * stride, 0, frame_w);
		ctx->fnUpdateCanvas(RGN_HANDLE);
	}

	osd_dirty_reset(&ctx->dirty, frame_w, frame_h);

	fprintf(stderr, "[debug_osd] overlay %ux%u stride=%u virtAddr=%p\n",
		ctx->canvas.stSize.u32Width, ctx->canvas.stSize.u32Height,
		ctx->canvas.u32Stride, (void *)(uintptr_t)ctx->canvas.virtAddr);
	return ctx;
}

void debug_osd_destroy(DebugOsdState *osd)
{
	if (!osd) return;
	osd->fnDetachChannel(RGN_HANDLE, &osd->vpe_bind);
	osd->fnDestroyRegion(RGN_HANDLE);
	osd->fnDeinit();
	if (osd->lib)
		dlclose(osd->lib);
	free(osd);
}

void debug_osd_begin_frame(DebugOsdState *osd)
{
	if (!osd) return;

	/* Re-acquire canvas info every frame — SDK double-buffers the canvas,
	 * so virtAddr can change after UpdateCanvas. */
	if (osd->fnGetCanvasInfo(RGN_HANDLE, &osd->canvas) != 0)
		return;

	OsdCanvas c;
	osd_canvas_from_info(&c, &osd->canvas, osd->width, osd->height);
	osd_clear_dirty(&c, &osd->dirty);
	osd_dirty_reset(&osd->dirty, osd->width, osd->height);
}

void debug_osd_end_frame(DebugOsdState *osd)
{
	if (!osd) return;
	osd->fnUpdateCanvas(RGN_HANDLE);
}

#define PANEL_X     8
#define PANEL_Y     8
#define LINE_MAX    64

void debug_osd_text(DebugOsdState *osd, int row, const char *label,
                    const char *fmt, ...)
{
	if (!osd) return;

	char value[48];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(value, sizeof(value), fmt, ap);
	va_end(ap);

	int s = osd->font_scale;
	int char_h = 8 * s;
	int row_h = char_h + 2 * s;  /* glyph height + gap */
	int char_w = 6 * s;          /* 5px glyph + 1px gap, scaled */
	uint16_t y = (uint16_t)(PANEL_Y + row * row_h);
	if ((uint32_t)y + (uint32_t)char_h > osd->height) return;

	char line[LINE_MAX];
	int len = snprintf(line, sizeof(line), "%s: %s", label, value);
	if (len < 0) return;
	if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;

	OsdCanvas c;
	osd_canvas_from_info(&c, &osd->canvas, osd->width, osd->height);

	/* Semi-transparent background behind text */
	uint16_t bg_w = (uint16_t)(len * char_w + 4 * s);
	int bg_x = PANEL_X - 2;
	int bg_y = (int)y - s;
	if (bg_x < 0) bg_x = 0;
	if (bg_y < 0) bg_y = 0;
	osd_draw_rect(&c, &osd->dirty, (uint16_t)bg_x, (uint16_t)bg_y, bg_w,
		(uint16_t)(char_h + 2 * s), DEBUG_OSD_SEMITRANS_BLACK, 1);

	osd_draw_string(&c, &osd->dirty, PANEL_X, y, line, s, DEBUG_OSD_WHITE);
}

void debug_osd_sample_cpu(DebugOsdState *osd)
{
	if (!osd) return;
	cpu_sample(osd);
}

int debug_osd_get_cpu(DebugOsdState *osd)
{
	return osd ? osd->cpu_pct : 0;
}

void debug_osd_rect(DebugOsdState *osd, uint16_t x, uint16_t y,
                    uint16_t w, uint16_t h, uint16_t color, int filled)
{
	if (!osd) return;
	OsdCanvas c;
	osd_canvas_from_info(&c, &osd->canvas, osd->width, osd->height);
	osd_draw_rect(&c, &osd->dirty, x, y, w, h, color, filled);
}

void debug_osd_point(DebugOsdState *osd, uint16_t x, uint16_t y,
                     uint16_t color, int size)
{
	if (!osd) return;
	OsdCanvas c;
	osd_canvas_from_info(&c, &osd->canvas, osd->width, osd->height);
	osd_draw_point(&c, &osd->dirty, x, y, color, size);
}

void debug_osd_line(DebugOsdState *osd, uint16_t x0, uint16_t y0,
                    uint16_t x1, uint16_t y1, uint16_t color)
{
	if (!osd) return;
	OsdCanvas c;
	osd_canvas_from_info(&c, &osd->canvas, osd->width, osd->height);
	osd_draw_line(&c, &osd->dirty, x0, y0, x1, y1, color);
}

#else /* !PLATFORM_STAR6E */

DebugOsdState *debug_osd_create(uint32_t frame_w, uint32_t frame_h,
                                const void *vpe_port)
{ (void)frame_w; (void)frame_h; (void)vpe_port; return NULL; }

void debug_osd_destroy(DebugOsdState *osd) { (void)osd; }
void debug_osd_begin_frame(DebugOsdState *osd) { (void)osd; }
void debug_osd_end_frame(DebugOsdState *osd) { (void)osd; }

void debug_osd_text(DebugOsdState *osd, int row, const char *label,
                    const char *fmt, ...)
{ (void)osd; (void)row; (void)label; (void)fmt; }

void debug_osd_sample_cpu(DebugOsdState *osd) { (void)osd; }
int debug_osd_get_cpu(DebugOsdState *osd) { (void)osd; return 0; }

void debug_osd_rect(DebugOsdState *osd, uint16_t x, uint16_t y,
                    uint16_t w, uint16_t h, uint16_t color, int filled)
{ (void)osd; (void)x; (void)y; (void)w; (void)h; (void)color; (void)filled; }

void debug_osd_point(DebugOsdState *osd, uint16_t x, uint16_t y,
                     uint16_t color, int size)
{ (void)osd; (void)x; (void)y; (void)color; (void)size; }

void debug_osd_line(DebugOsdState *osd, uint16_t x0, uint16_t y0,
                    uint16_t x1, uint16_t y1, uint16_t color)
{ (void)osd; (void)x0; (void)y0; (void)x1; (void)y1; (void)color; }

#endif /* PLATFORM_STAR6E */
