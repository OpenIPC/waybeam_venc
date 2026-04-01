#include "debug_osd.h"

#ifdef PLATFORM_STAR6E

#include "sigmastar_types.h"  /* i6_sys_bind */

#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <arm_neon.h>

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

typedef struct {
	uint16_t x0, y0, x1, y1;  /* inclusive bounding box */
} DirtyRect;

struct DebugOsdState {
	void *lib;
	uint32_t width, height;
	DebugOsdCanvasInfo canvas;
	i6_sys_bind vpe_bind;
	DirtyRect dirty;           /* previous frame's drawn area */
	int font_scale;            /* pixel scaling factor for text */

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

static void fill_row(uint16_t *row, int count, uint16_t color);

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

/* ── 8x8 bitmap font (CP437 printable ASCII, public domain) ────────
 * Each glyph: 8 rows, 5px wide, bit 4 = leftmost pixel. */
static const uint8_t g_font8x8[128][8] = {
	[0x20] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
	[0x21] = {0x04,0x04,0x04,0x04,0x04,0x00,0x04,0x00}, /* ! */
	[0x22] = {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, /* " */
	[0x23] = {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A,0x00}, /* # */
	[0x24] = {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04,0x00}, /* $ */
	[0x25] = {0x18,0x19,0x02,0x04,0x08,0x13,0x03,0x00}, /* % */
	[0x26] = {0x08,0x14,0x14,0x08,0x15,0x12,0x0D,0x00}, /* & */
	[0x27] = {0x04,0x04,0x08,0x00,0x00,0x00,0x00,0x00}, /* ' */
	[0x28] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02,0x00}, /* ( */
	[0x29] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08,0x00}, /* ) */
	[0x2A] = {0x00,0x04,0x15,0x0E,0x15,0x04,0x00,0x00}, /* * */
	[0x2B] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00,0x00}, /* + */
	[0x2C] = {0x00,0x00,0x00,0x00,0x00,0x04,0x04,0x08}, /* , */
	[0x2D] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00,0x00}, /* - */
	[0x2E] = {0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00}, /* . */
	[0x2F] = {0x00,0x01,0x02,0x04,0x08,0x10,0x00,0x00}, /* / */
	[0x30] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E,0x00}, /* 0 */
	[0x31] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E,0x00}, /* 1 */
	[0x32] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F,0x00}, /* 2 */
	[0x33] = {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E,0x00}, /* 3 */
	[0x34] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02,0x00}, /* 4 */
	[0x35] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,0x00}, /* 5 */
	[0x36] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E,0x00}, /* 6 */
	[0x37] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08,0x00}, /* 7 */
	[0x38] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,0x00}, /* 8 */
	[0x39] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C,0x00}, /* 9 */
	[0x3A] = {0x00,0x00,0x04,0x00,0x00,0x04,0x00,0x00}, /* : */
	[0x3B] = {0x00,0x00,0x04,0x00,0x00,0x04,0x04,0x08}, /* ; */
	[0x3C] = {0x02,0x04,0x08,0x10,0x08,0x04,0x02,0x00}, /* < */
	[0x3D] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00,0x00}, /* = */
	[0x3E] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08,0x00}, /* > */
	[0x3F] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04,0x00}, /* ? */
	[0x40] = {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E,0x00}, /* @ */
	[0x41] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11,0x00}, /* A */
	[0x42] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E,0x00}, /* B */
	[0x43] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E,0x00}, /* C */
	[0x44] = {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C,0x00}, /* D */
	[0x45] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F,0x00}, /* E */
	[0x46] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10,0x00}, /* F */
	[0x47] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F,0x00}, /* G */
	[0x48] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11,0x00}, /* H */
	[0x49] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E,0x00}, /* I */
	[0x4A] = {0x07,0x02,0x02,0x02,0x02,0x12,0x0C,0x00}, /* J */
	[0x4B] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11,0x00}, /* K */
	[0x4C] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F,0x00}, /* L */
	[0x4D] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11,0x00}, /* M */
	[0x4E] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11,0x00}, /* N */
	[0x4F] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E,0x00}, /* O */
	[0x50] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,0x00}, /* P */
	[0x51] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D,0x00}, /* Q */
	[0x52] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11,0x00}, /* R */
	[0x53] = {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E,0x00}, /* S */
	[0x54] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04,0x00}, /* T */
	[0x55] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E,0x00}, /* U */
	[0x56] = {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04,0x00}, /* V */
	[0x57] = {0x11,0x11,0x11,0x15,0x15,0x1B,0x11,0x00}, /* W */
	[0x58] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11,0x00}, /* X */
	[0x59] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04,0x00}, /* Y */
	[0x5A] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F,0x00}, /* Z */
	[0x5B] = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E,0x00}, /* [ */
	[0x5C] = {0x00,0x10,0x08,0x04,0x02,0x01,0x00,0x00}, /* \ */
	[0x5D] = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E,0x00}, /* ] */
	[0x5E] = {0x04,0x0A,0x11,0x00,0x00,0x00,0x00,0x00}, /* ^ */
	[0x5F] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0x00}, /* _ */
	[0x60] = {0x08,0x04,0x02,0x00,0x00,0x00,0x00,0x00}, /* ` */
	[0x61] = {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F,0x00}, /* a */
	[0x62] = {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E,0x00}, /* b */
	[0x63] = {0x00,0x00,0x0E,0x11,0x10,0x11,0x0E,0x00}, /* c */
	[0x64] = {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F,0x00}, /* d */
	[0x65] = {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E,0x00}, /* e */
	[0x66] = {0x06,0x08,0x1C,0x08,0x08,0x08,0x08,0x00}, /* f */
	[0x67] = {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E,0x00}, /* g */
	[0x68] = {0x10,0x10,0x1E,0x11,0x11,0x11,0x11,0x00}, /* h */
	[0x69] = {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E,0x00}, /* i */
	[0x6A] = {0x02,0x00,0x06,0x02,0x02,0x12,0x0C,0x00}, /* j */
	[0x6B] = {0x10,0x10,0x12,0x14,0x18,0x14,0x12,0x00}, /* k */
	[0x6C] = {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E,0x00}, /* l */
	[0x6D] = {0x00,0x00,0x1A,0x15,0x15,0x11,0x11,0x00}, /* m */
	[0x6E] = {0x00,0x00,0x1E,0x11,0x11,0x11,0x11,0x00}, /* n */
	[0x6F] = {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E,0x00}, /* o */
	[0x70] = {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10,0x00}, /* p */
	[0x71] = {0x00,0x00,0x0F,0x11,0x0F,0x01,0x01,0x00}, /* q */
	[0x72] = {0x00,0x00,0x16,0x19,0x10,0x10,0x10,0x00}, /* r */
	[0x73] = {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E,0x00}, /* s */
	[0x74] = {0x08,0x08,0x1C,0x08,0x08,0x09,0x06,0x00}, /* t */
	[0x75] = {0x00,0x00,0x11,0x11,0x11,0x13,0x0D,0x00}, /* u */
	[0x76] = {0x00,0x00,0x11,0x11,0x11,0x0A,0x04,0x00}, /* v */
	[0x77] = {0x00,0x00,0x11,0x11,0x15,0x15,0x0A,0x00}, /* w */
	[0x78] = {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11,0x00}, /* x */
	[0x79] = {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E,0x00}, /* y */
	[0x7A] = {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F,0x00}, /* z */
	[0x7B] = {0x02,0x04,0x04,0x08,0x04,0x04,0x02,0x00}, /* { */
	[0x7C] = {0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x00}, /* | */
	[0x7D] = {0x08,0x04,0x04,0x02,0x04,0x04,0x08,0x00}, /* } */
	[0x7E] = {0x00,0x00,0x08,0x15,0x02,0x00,0x00,0x00}, /* ~ */
};

/* ── Drawing primitives ────────────────────────────────────────────── */

static inline void dirty_expand(DebugOsdState *osd, int x, int y)
{
	/* Clamp to canvas bounds before storing — negative values would
	 * wrap to huge uint16_t and corrupt the dirty rect. */
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= (int)osd->width) x = (int)osd->width - 1;
	if (y >= (int)osd->height) y = (int)osd->height - 1;
	if ((uint16_t)x < osd->dirty.x0) osd->dirty.x0 = (uint16_t)x;
	if ((uint16_t)y < osd->dirty.y0) osd->dirty.y0 = (uint16_t)y;
	if ((uint16_t)x > osd->dirty.x1) osd->dirty.x1 = (uint16_t)x;
	if ((uint16_t)y > osd->dirty.y1) osd->dirty.y1 = (uint16_t)y;
}

static inline void put_pixel(DebugOsdState *osd, int x, int y, uint16_t color)
{
	if (x < 0 || x >= (int)osd->width || y < 0 || y >= (int)osd->height)
		return;
	uint16_t *pixels = (uint16_t *)(uintptr_t)osd->canvas.virtAddr;
	uint32_t stride_px = osd->canvas.u32Stride / 2;
	pixels[y * stride_px + x] = color;
	/* dirty_expand not needed here — callers that use put_pixel in bulk
	 * (draw_char, point, line) expand dirty at a higher level. */
}

static void draw_char(DebugOsdState *osd, int px, int py,
                      char ch, uint16_t color)
{
	if (ch < 0x20 || ch > 0x7e) ch = '?';
	const uint8_t *glyph = g_font8x8[(unsigned char)ch];
	int s = osd->font_scale;
	uint16_t *pixels = (uint16_t *)(uintptr_t)osd->canvas.virtAddr;
	uint32_t stride_px = osd->canvas.u32Stride / 2;

	/* Expand dirty rect once for the whole character */
	dirty_expand(osd, px, py);
	dirty_expand(osd, px + 5 * s - 1, py + 8 * s - 1);

	for (int gy = 0; gy < 8; gy++) {
		uint8_t bits = glyph[gy];
		for (int gx = 0; gx < 5; gx++) {
			if (!(bits & (0x10 >> gx)))
				continue;
			int cx = px + gx * s;
			int cy = py + gy * s;
			/* Write scaled block — use fill_row for each row of the block */
			for (int sy = 0; sy < s; sy++) {
				int row_y = cy + sy;
				if (row_y < 0 || row_y >= (int)osd->height)
					continue;
				if (cx >= 0 && cx + s <= (int)osd->width)
					fill_row(pixels + row_y * stride_px + cx, s, color);
				else
					for (int sx = 0; sx < s; sx++)
						put_pixel(osd, cx + sx, row_y, color);
			}
		}
	}
}

static void draw_string(DebugOsdState *osd, int x, int y,
                        const char *str, uint16_t color)
{
	int cw = 6 * osd->font_scale;  /* 5px glyph + 1px gap, scaled */
	for (int i = 0; str[i]; i++)
		draw_char(osd, x + i * cw, y, str[i], color);
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

	/* Init RGN subsystem with empty palette (pixel-alpha mode) */
	i6_rgn_pal pal;
	memset(&pal, 0, sizeof(pal));
	if (ctx->fnInit(&pal) != 0) {
		fprintf(stderr, "[debug_osd] MI_RGN_Init failed\n");
		dlclose(ctx->lib);
		free(ctx);
		return NULL;
	}

	/* Create OSD region: full frame, ARGB4444 */
	i6_rgn_cnf cnf;
	memset(&cnf, 0, sizeof(cnf));
	cnf.type = I6_RGN_TYPE_OSD;
	cnf.pixFmt = I6_RGN_PIXFMT_ARGB4444;
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
		uint16_t *pixels = (uint16_t *)(uintptr_t)ctx->canvas.virtAddr;
		uint32_t stride_px = ctx->canvas.u32Stride / 2;
		for (uint32_t y = 0; y < frame_h; y++)
			memset(pixels + y * stride_px, 0, frame_w * 2);
		ctx->fnUpdateCanvas(RGN_HANDLE);
	}

	/* Init dirty rect to empty */
	ctx->dirty.x0 = frame_w;
	ctx->dirty.y0 = frame_h;
	ctx->dirty.x1 = 0;
	ctx->dirty.y1 = 0;

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

	/* Clear only the previous frame's dirty region */
	if (osd->dirty.x1 >= osd->dirty.x0 && osd->dirty.y1 >= osd->dirty.y0) {
		uint16_t *pixels = (uint16_t *)(uintptr_t)osd->canvas.virtAddr;
		uint32_t stride_px = osd->canvas.u32Stride / 2;
		int clear_w = osd->dirty.x1 - osd->dirty.x0 + 1;
		for (uint32_t y = osd->dirty.y0; y <= osd->dirty.y1 && y < osd->height; y++)
			fill_row(pixels + y * stride_px + osd->dirty.x0, clear_w, 0);
	}

	/* Reset dirty rect to empty (max/min sentinel) */
	osd->dirty.x0 = osd->width;
	osd->dirty.y0 = osd->height;
	osd->dirty.x1 = 0;
	osd->dirty.y1 = 0;
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

	/* Semi-transparent background behind text */
	uint16_t bg_w = (uint16_t)(len * char_w + 4 * s);
	int bg_x = PANEL_X - 2;
	int bg_y = (int)y - s;
	if (bg_x < 0) bg_x = 0;
	if (bg_y < 0) bg_y = 0;
	debug_osd_rect(osd, (uint16_t)bg_x, (uint16_t)bg_y, bg_w,
		(uint16_t)(char_h + 2 * s), DEBUG_OSD_SEMITRANS_BLACK, 1);

	draw_string(osd, PANEL_X, y, line, DEBUG_OSD_WHITE);
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

/* ── Row fill ──────────────────────────────────────────────────────── */

static void fill_row(uint16_t *row, int count, uint16_t color)
{
	uint16x8_t v = vdupq_n_u16(color);
	int i = 0;

	while (i < count && ((uintptr_t)(row + i) & 15)) {
		row[i] = color;
		i++;
	}
	int chunks = (count - i) / 8;
	uint16_t *p = row + i;
	for (int j = 0; j < chunks; j++)
		vst1q_u16(p + j * 8, v);
	i += chunks * 8;
	while (i < count) {
		row[i] = color;
		i++;
	}
}

/* ── Rectangles ────────────────────────────────────────────────────── */

void debug_osd_rect(DebugOsdState *osd, uint16_t x, uint16_t y,
                    uint16_t w, uint16_t h, uint16_t color, int filled)
{
	if (!osd || !w || !h) return;

	/* Clamp to canvas bounds */
	int x0 = x, y0 = y;
	int x1 = x + w - 1, y1 = y + h - 1;
	if (x0 >= (int)osd->width || y0 >= (int)osd->height) return;
	if (x1 >= (int)osd->width) x1 = (int)osd->width - 1;
	if (y1 >= (int)osd->height) y1 = (int)osd->height - 1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;

	/* Expand dirty rect once for the whole rectangle */
	dirty_expand(osd, x0, y0);
	dirty_expand(osd, x1, y1);

	uint16_t *pixels = (uint16_t *)(uintptr_t)osd->canvas.virtAddr;
	uint32_t stride_px = osd->canvas.u32Stride / 2;
	int span = x1 - x0 + 1;

	if (filled) {
		for (int row = y0; row <= y1; row++)
			fill_row(pixels + row * stride_px + x0, span, color);
	} else {
		fill_row(pixels + y0 * stride_px + x0, span, color);
		fill_row(pixels + y1 * stride_px + x0, span, color);
		for (int row = y0; row <= y1; row++) {
			pixels[row * stride_px + x0] = color;
			pixels[row * stride_px + x1] = color;
		}
	}
}

void debug_osd_point(DebugOsdState *osd, uint16_t x, uint16_t y,
                     uint16_t color, int size)
{
	if (!osd) return;
	dirty_expand(osd, (int)x - size, (int)y - size);
	dirty_expand(osd, (int)x + size, (int)y + size);
	for (int d = -size; d <= size; d++) {
		put_pixel(osd, (int)x + d, (int)y, color);
		put_pixel(osd, (int)x, (int)y + d, color);
	}
}

void debug_osd_line(DebugOsdState *osd, uint16_t x0, uint16_t y0,
                    uint16_t x1, uint16_t y1, uint16_t color)
{
	if (!osd) return;
	dirty_expand(osd, x0, y0);
	dirty_expand(osd, x1, y1);

	int dx = abs((int)x1 - (int)x0);
	int dy = -abs((int)y1 - (int)y0);
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	int cx = x0, cy = y0;

	for (;;) {
		put_pixel(osd, cx, cy, color);
		if (cx == (int)x1 && cy == (int)y1) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; cx += sx; }
		if (e2 <= dx) { err += dx; cy += sy; }
	}
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
