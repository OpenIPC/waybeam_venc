#include "debug_osd.h"

/* ── CPU usage sampler (shared, /proc/stat) ────────────────────────────
 * Reports busy time as a percentage of TOTAL capacity across every core, so
 * 100% means every core saturated (unchanged from the original semantics).
 *
 * IMPORTANT — do NOT compute the denominator from the /proc/stat busy fields
 * on SigmaStar. On these 4.9 kernels `user` and `system` are credited late and
 * in batches, so over short windows they are wildly wrong while `idle` and the
 * per-task counters stay accurate. Measured on SSC338Q over 2s windows: the
 * field sum swung 287..605 jiffies for a constant load (`system` alone 6..236,
 * and 10..319 for a syscall-free busy loop that cannot produce system time),
 * and field-derived busy came to just 0.11x the true value. That is precisely
 * why busybox `top` shows this binary oscillating 2..25% when it is in fact
 * steady, and the OSD inherited the same bug by dividing by that sum.
 *
 * So: derive busy from IDLE against a WALL-CLOCK denominator.
 *   avail = elapsed_seconds * USER_HZ * ncores   (capacity, in jiffies)
 *   busy% = (avail - delta_idle) / avail * 100
 * Only `idle` (trustworthy) and CLOCK_MONOTONIC are used; the bursty fields
 * are never read. Details: documentation/STAR6E_CPU_PROFILE.md.
 *
 * CV610 ONLY — that idle derivation does not work there either, for the
 * opposite reason: /proc/stat's total is exactly right on the 5.10 kernel, so
 * idle-derived and field-derived are algebraically the same number, and it is
 * the idle/busy SPLIT that oscillates (tick sampling beating against venc's
 * 100 fps at HZ=100). Measured on .181: 1.9-95.3% for a box steady at ~33%.
 * There we read /proc/schedstat rq_cpu_time instead. This stays compile-time
 * CV610-only on purpose: on Star6E /proc/stat is sound and schedstat would be
 * WRONG -- measured on .232, idle-derived 60.5 vs an all-task sum of 52.7, and
 * rq_cpu_time counts only task execution, so it drops that 7.7-point
 * IRQ/softirq remainder. Maruko ships without CONFIG_SCHEDSTATS at all.
 *
 * Snapshot ring at 500ms cadence, span = OSD_CPU_RING * 500ms, so the readout
 * is a sliding ~2s average that still refreshes every 500ms. 2s is where
 * idle-derived busy measured stable (+/-2.5% on a constant load) while
 * remaining responsive enough to watch a mode change land. */
#if defined(PLATFORM_STAR6E) || defined(PLATFORM_MARUKO) || defined(PLATFORM_CV610)

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OSD_CPU_RING 4     /* 4 x 500ms cadence => ~2s sliding window */

typedef struct {
	struct {
		/* busy nanoseconds when use_sched, else idle+iowait jiffies */
		unsigned long long acc;
		struct timespec ts;      /* when this snapshot was taken */
	} ring[OSD_CPU_RING];
	int count;                 /* snapshots stored (saturates at RING) */
	int head;                  /* next write index; oldest when full */
	int pct;                   /* last computed CPU% */
	struct timespec ts;        /* last snapshot time (cadence gate) */
	long hz;                   /* USER_HZ for /proc/stat, always 100 */
	int ncores;                /* cpu lines in the source */
	int use_sched;             /* this ring holds ns, not jiffies */
} OsdCpuSampler;

/* /proc/schedstat cpuN row: "cpu%d %u 0 %u %u %u %u %llu %llu %lu" -- six
 * counters after the label, THEN rq_cpu_time in nanoseconds. That is real
 * task-execution time off the scheduler clock, so it does not inherit the
 * tick-sampling error that makes /proc/stat unusable on CV610: measured on
 * .181, /proc/stat read 1.9-95.3% (stdev 22.05) for a box steady at ~33%,
 * while schedstat read 30.1-38.3% (stdev 2.02). Returns 0 when the kernel
 * lacks CONFIG_SCHEDSTATS, which keeps the /proc/stat path below. */
#ifdef PLATFORM_CV610
static int osd_read_schedstat(unsigned long long *busy_ns, int *cores)
{
	char line[256];
	FILE *f = fopen("/proc/schedstat", "r");
	if (!f) return 0;

	*busy_ns = 0;
	*cores = 0;
	while (fgets(line, sizeof line, f)) {
		unsigned long long ns;
		if (strncmp(line, "cpu", 3) != 0) continue;
		if (line[3] < '0' || line[3] > '9') continue;
		if (sscanf(line, "cpu%*u %*u %*u %*u %*u %*u %*u %llu", &ns) != 1)
			continue;
		*busy_ns += ns;
		(*cores)++;
	}
	fclose(f);
	/* Insist the counter actually moved. Our fleet already spans schedstat v15
	 * and v17; if a future revision shifts the cpuN layout, field 8 lands on
	 * one of the leading zero counters and this would report 0% forever --
	 * silently wrong, which is the failure this path exists to avoid.
	 * Demanding a non-zero sum makes that self-correcting (fall back to
	 * /proc/stat) and cannot false-trigger: init has consumed task time long
	 * before venc starts. */
	return *cores > 0 && *busy_ns > 0;
}
#endif /* PLATFORM_CV610 */

static int osd_read_procstat(unsigned long long *idle_all, int *cores)
{
	unsigned long long user, nice, sys, idle, iowait, irq, softirq;
	char line[256];
	int have = 0;
	FILE *f = fopen("/proc/stat", "r");
	if (!f) return 0;

	*cores = 0;
	/* The aggregate "cpu " line comes first, then one "cpuN" line per core,
	 * then non-cpu keys. One pass gets both the idle counter and the core
	 * count. Lines are ~110 bytes, well inside the buffer. */
	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, "cpu", 3) != 0) break;
		if (line[3] == ' ') {
			if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu",
			           &user, &nice, &sys, &idle, &iowait, &irq,
			           &softirq) == 7)
				have = 1;
		} else if (line[3] >= '0' && line[3] <= '9') {
			(*cores)++;
		}
	}
	fclose(f);
	if (!have) return 0;
	*idle_all = idle + iowait;
	return 1;
}

static void osd_cpu_sample(OsdCpuSampler *cs)
{
	struct timespec now;
	unsigned long long acc = 0;
	int cores = 0;

	clock_gettime(CLOCK_MONOTONIC, &now);
	long ms = (now.tv_sec - cs->ts.tv_sec) * 1000 +
	          (now.tv_nsec - cs->ts.tv_nsec) / 1000000;
	if (cs->count > 0 && ms < 500) return;

	/* Read the accumulator. Re-decided every sample rather than latched: a
	 * latch taken on a transient fopen failure at the first encoded frame
	 * would pin us to the wrong source for the whole process lifetime, and
	 * a latch that cannot fall back retries per FRAME (100 Hz) rather than
	 * per window, because the cadence gate above is only armed by cs->ts. */
	int used_sched = 0;
#ifdef PLATFORM_CV610
	used_sched = osd_read_schedstat(&acc, &cores);
#endif
	if (!used_sched && !osd_read_procstat(&acc, &cores)) {
		cs->ts = now;   /* arm the gate: do not retry every frame */
		return;
	}

	/* Nanoseconds and jiffies must never share a ring. If the source moved,
	 * drop the window and start a fresh one rather than differencing two
	 * different units. */
	if (cs->count > 0 && used_sched != cs->use_sched) {
		cs->count = 0;
		cs->head = 0;
		cs->ncores = 0;
	}
	cs->use_sched = used_sched;

	if (cs->hz <= 0) {
		cs->hz = sysconf(_SC_CLK_TCK);
		if (cs->hz <= 0) cs->hz = 100;
	}
	if (cs->ncores <= 0) cs->ncores = cores > 0 ? cores : 1;

	if (cs->count > 0) {
		/* When the ring is full, head (about to be overwritten) is the
		 * oldest retained snapshot; while filling, index 0 is. */
		int oldest = (cs->count == OSD_CPU_RING) ? cs->head : 0;
		long span_ms =
			(now.tv_sec - cs->ring[oldest].ts.tv_sec) * 1000L +
			(now.tv_nsec - cs->ring[oldest].ts.tv_nsec) / 1000000L;
		/* A counter that moved BACKWARDS is not a measurement -- keep the
		 * last good value instead of inventing one. Feeding a zero delta
		 * to either formula fabricates an opposite extreme: 0% on the
		 * schedstat path, and 100% on the /proc/stat path (avail - 0), the
		 * latter reading as an alarm on the overlay. Left unguarded it is
		 * worse still: the raw subtraction wraps to a huge unsigned. */
		if (span_ms > 0 && acc >= cs->ring[oldest].acc) {
			unsigned long long d = acc - cs->ring[oldest].acc;
			unsigned long long avail, busy;
			if (cs->use_sched) {
				avail = (unsigned long long)span_ms * 1000000ULL *
					(unsigned long long)cs->ncores;
				busy = d;
			} else {
				avail = (unsigned long long)span_ms *
					(unsigned long long)cs->hz *
					(unsigned long long)cs->ncores / 1000ULL;
				/* Clamp: idle is sampled a hair after the wall clock, so
				 * a fully idle box can round to d slightly over avail. */
				busy = (avail > d) ? avail - d : 0;
			}
			if (avail > 0) {
				cs->pct = (int)(busy * 100ULL / avail);
				if (cs->pct > 100) cs->pct = 100;
			}
		}
	}

	cs->ring[cs->head].acc = acc;
	cs->ring[cs->head].ts = now;
	cs->head = (cs->head + 1) % OSD_CPU_RING;
	if (cs->count < OSD_CPU_RING) cs->count++;
	cs->ts = now;
}

#endif /* platform CPU sampler */

/* Maruko build flags set BOTH PLATFORM_STAR6E and PLATFORM_MARUKO (see
 * Makefile:39 — the Star6E backend's MI shim headers are reused for
 * type compatibility on Maruko), so the Star6E branch must explicitly
 * exclude Maruko.  Star6E-only builds set only PLATFORM_STAR6E. */
#if defined(PLATFORM_STAR6E) && !defined(PLATFORM_MARUKO)

#include "debug_osd_draw.h"
#include "sigmastar_types.h"  /* i6_sys_bind */

#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Settle between MI_RGN_DetachFromChn and MI_RGN_Destroy in
 * debug_osd_destroy().  The detach removes the region from the VPE
 * compositor's list, but a frame already mid-composite can still be reading
 * the RGN canvas; freeing it immediately races that read → MMU read-fault
 * (ClientId=0x15, IsWrite=0) that storms to a HW watchdog reset on rapid
 * respawn.  ~3 frame intervals at 60fps covers the in-flight composite.
 * Overridable at build time for tuning. */
#ifndef VENC_OSD_DESTROY_SETTLE_US
#define VENC_OSD_DESTROY_SETTLE_US 50000
#endif

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
	int panel_off_x;          /* added to PANEL_X for text positioning */
	int panel_off_y;          /* added to PANEL_Y — used when VPE port
	                           * dim > encoded dim (e.g. image stab) so
	                           * panel lands inside the encoded view */

	OsdCpuSampler cpu;         /* shared /proc/stat sampler (top of file) */

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
	out->stride_bytes = info->u32Stride;  /* I4: width/2 + alignment */
	out->width = width;
	out->height = height;
}

/* Copy the shared host-side palette into the SDK's i6_rgn_pal shape.
 * The SDK struct holds 256 entries; for I4 we only fill the first 16.
 * Zero the rest so the unused tail isn't stack garbage on the way down
 * to MI_RGN_Init — defensive even though the kernel almost certainly
 * ignores entries above the format's reach. */
static void palette_init(i6_rgn_pal *pal)
{
	const OsdPaletteEntry *src = osd_palette();
	memset(pal, 0, sizeof(*pal));
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

/* ── Public API ────────────────────────────────────────────────────── */

/* RGN module ids for Star6E libmi_rgn.so.  MI_RGN_AttachToChn takes an
 * i6_sys_bind whose module field uses RGN's private enum, NOT the
 * i6_sys_mod enum.  Standard SigmaStar Infinity6E layout puts VENC at 2,
 * but the exact value is determined by the device's libmi_rgn build —
 * if VENC attach starts failing after a firmware update, this is the
 * first place to check. */
#define RGN_MODID_VPE  0
#define RGN_MODID_VENC 2

static DebugOsdState *debug_osd_create_impl(uint32_t frame_w, uint32_t frame_h,
	int rgn_mod_id, int dev_id, int chn_id, int port_id)
{
	DebugOsdState *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) return NULL;

	ctx->width = frame_w;
	ctx->height = frame_h;
	ctx->font_scale = 3;
	ctx->vpe_bind.module = rgn_mod_id;
	ctx->vpe_bind.device = dev_id;
	ctx->vpe_bind.channel = chn_id;
	ctx->vpe_bind.port = port_id;

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

	/* Create OSD region: full frame, I4 (palette-indexed, 4 bpp) */
	i6_rgn_cnf cnf;
	memset(&cnf, 0, sizeof(cnf));
	cnf.type = I6_RGN_TYPE_OSD;
	cnf.pixFmt = I6_RGN_PIXFMT_I4;
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
		fprintf(stderr, "[debug_osd] MI_RGN_AttachToChn failed "
			"(module=%d dev=%d chn=%d port=%d)\n",
			ctx->vpe_bind.module, ctx->vpe_bind.device,
			ctx->vpe_bind.channel, ctx->vpe_bind.port);
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

	/* Clear canvas to transparent on first create.  I4 transparent is
	 * palette index 0; the packed nibble byte for it is 0x00, so a flat
	 * memset of stride_bytes per row clears both pixels in each byte. */
	{
		uint8_t *pixels = (uint8_t *)(uintptr_t)ctx->canvas.virtAddr;
		uint32_t stride = ctx->canvas.u32Stride;
		for (uint32_t y = 0; y < frame_h; y++)
			memset(pixels + y * stride, 0, stride);
		ctx->fnUpdateCanvas(RGN_HANDLE);
	}

	osd_dirty_reset(&ctx->dirty, frame_w, frame_h);

	fprintf(stderr, "[debug_osd] overlay %ux%u stride=%u virtAddr=%p "
		"attached to rgn_mod=%d dev=%d chn=%d\n",
		ctx->canvas.stSize.u32Width, ctx->canvas.stSize.u32Height,
		ctx->canvas.u32Stride, (void *)(uintptr_t)ctx->canvas.virtAddr,
		ctx->vpe_bind.module, ctx->vpe_bind.device,
		ctx->vpe_bind.channel);
	return ctx;
}

DebugOsdState *debug_osd_create(uint32_t frame_w, uint32_t frame_h,
                                const void *vpe_port)
{
	(void)vpe_port;
	return debug_osd_create_impl(frame_w, frame_h,
		RGN_MODID_VPE, 0, 0, 0);
}


void debug_osd_destroy(DebugOsdState *osd)
{
	if (!osd) return;
	osd->fnDetachChannel(RGN_HANDLE, &osd->vpe_bind);
	/* Let any in-flight VPE composite finish reading the canvas before the
	 * destroy frees it — closes the client-0x15 MMU read-fault race that
	 * wedges rapid respawn (see VENC_OSD_DESTROY_SETTLE_US above). */
	usleep(VENC_OSD_DESTROY_SETTLE_US);
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
	int panel_x = PANEL_X + osd->panel_off_x;
	int panel_y = PANEL_Y + osd->panel_off_y;
	uint16_t y = (uint16_t)(panel_y + row * row_h);
	if ((uint32_t)y + (uint32_t)char_h > osd->height) return;

	char line[LINE_MAX];
	int len = snprintf(line, sizeof(line), "%s: %s", label, value);
	if (len < 0) return;
	if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;

	OsdCanvas c;
	osd_canvas_from_info(&c, &osd->canvas, osd->width, osd->height);

	/* Semi-transparent background behind text */
	uint16_t bg_w = (uint16_t)(len * char_w + 4 * s);
	int bg_x = panel_x - 2;
	int bg_y = (int)y - s;
	if (bg_x < 0) bg_x = 0;
	if (bg_y < 0) bg_y = 0;
	osd_draw_rect(&c, &osd->dirty, (uint16_t)bg_x, (uint16_t)bg_y, bg_w,
		(uint16_t)(char_h + 2 * s), DEBUG_OSD_SEMITRANS_BLACK, 1);

	osd_draw_string(&c, &osd->dirty, (uint16_t)panel_x, y, line, s,
		DEBUG_OSD_WHITE);
}

void debug_osd_set_panel_offset(DebugOsdState *osd, int off_x, int off_y)
{
	if (!osd) return;
	osd->panel_off_x = off_x;
	osd->panel_off_y = off_y;
}

void debug_osd_sample_cpu(DebugOsdState *osd)
{
	if (!osd) return;
	osd_cpu_sample(&osd->cpu);
}

int debug_osd_get_cpu(DebugOsdState *osd)
{
	return osd ? osd->cpu.pct : 0;
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

#elif defined(PLATFORM_MARUKO)

#include "debug_osd_draw.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* MI_RGN ABI — Maruko (OpenIPC libmi_rgn.so) v3 API.
 *
 * The OpenIPC build of libmi_rgn uses the older datatype layout
 * (msposd vintage): MI_RGN_ModId_e enum + s32OutputPortId field name,
 * NOT v5 SDK's MI_ModuleId_e + s32PortId.  Layouts mirrored from
 * waybeam-hub/vendor/sigmastar/maruko/include/mi_rgn_datatype.h.
 *
 * mod_id 34 (SCL) is accepted by the lib even though the
 * MI_RGN_ModId_e enum only defines VPE/DIVP/LDC — msposd does the
 * same trick and waybeam-hub's mod_osd_render verifies it works on
 * this kernel/lib pair. */

typedef enum {
	MR_RGN_PIXFMT_ARGB1555 = 0,
	MR_RGN_PIXFMT_ARGB4444,
	MR_RGN_PIXFMT_I2,
	MR_RGN_PIXFMT_I4,
	MR_RGN_PIXFMT_I8,
	MR_RGN_PIXFMT_RGB565,
	MR_RGN_PIXFMT_ARGB8888,
} mr_rgn_pixfmt;

typedef enum {
	MR_RGN_TYPE_OSD = 0,
	MR_RGN_TYPE_COVER,
} mr_rgn_type;

typedef struct { uint32_t u32Width; uint32_t u32Height; } mr_rgn_size;

typedef struct {
	int32_t  ePixelFmt;          /* mr_rgn_pixfmt */
	mr_rgn_size stSize;
} mr_rgn_osd_init_param;

typedef struct {
	int32_t  eType;              /* mr_rgn_type */
	mr_rgn_osd_init_param stOsdInitParam;
} mr_rgn_attr;

typedef struct {
	int32_t  eModId;             /* MI_RGN_ModId_e — raw 34 = SCL */
	int32_t  s32DevId;
	int32_t  s32ChnId;
	int32_t  s32OutputPortId;
} mr_rgn_chnport;

typedef struct { uint32_t u32X; uint32_t u32Y; } mr_rgn_point;

typedef struct {
	uint8_t  u8BgAlpha;
	uint8_t  u8FgAlpha;
} mr_rgn_argb1555_alpha;

typedef union {
	mr_rgn_argb1555_alpha stArgb1555Alpha;
	uint8_t  u8ConstantAlpha;
	/* pad to widest alpha mode shape */
	uint16_t pad;
} mr_rgn_alpha_para;

typedef struct {
	int32_t  eAlphaMode;         /* 0 = pixel, 1 = constant */
	mr_rgn_alpha_para stAlphaPara;
} mr_rgn_alpha_attr;

typedef struct {
	int32_t  bEnableColorInv;    /* MI_BOOL is 32-bit on this SDK */
	int32_t  eInvertColorMode;
	uint16_t u16LumaThreshold;
	uint16_t u16WDivNum;
	uint16_t u16HDivNum;
} mr_rgn_invert;

typedef struct {
	uint32_t u32Layer;
	mr_rgn_alpha_attr stOsdAlphaAttr;
	mr_rgn_invert stColorInvertAttr;
} mr_rgn_osd_chnport;

typedef struct {
	mr_rgn_size stSize;
	uint32_t u32Color;
	uint32_t u32Layer;
} mr_rgn_cover_chnport;

typedef union {
	mr_rgn_cover_chnport stCoverChnPort;
	mr_rgn_osd_chnport   stOsdChnPort;
} mr_rgn_chnport_param_u;

typedef struct {
	int32_t  bShow;              /* MI_BOOL */
	mr_rgn_point stPoint;
	mr_rgn_chnport_param_u unPara;
} mr_rgn_chnport_param;

typedef struct {
	uint64_t phyAddr;            /* MI_PHY = 64-bit on Maruko */
	unsigned long virtAddr;      /* MI_VIRT = pointer-width */
	mr_rgn_size stSize;
	uint32_t u32Stride;
	int32_t  ePixelFmt;
} mr_rgn_canvas_info;

typedef struct {
	uint8_t u8Alpha;
	uint8_t u8Red;
	uint8_t u8Green;
	uint8_t u8Blue;
} mr_rgn_palette_element;

typedef struct {
	mr_rgn_palette_element astElement[256];
} mr_rgn_palette_table;

#define RGN_HANDLE      0
#define RGN_SOC_ID      0
#define RGN_MODID_SCL   34

struct DebugOsdState {
	void *lib;
	uint32_t width, height;
	mr_rgn_canvas_info canvas;
	mr_rgn_chnport chnport;
	OsdDirty dirty;
	int font_scale;

	OsdCpuSampler cpu;         /* shared /proc/stat sampler (top of file) */

	/* Maruko v3 API.  The vendor's official IPC demo
	 * (common/osd/osd.cpp) uses MI_RGN_Init/DeInit (palette as
	 * direct arg, soc_id first) — NOT MI_RGN_InitDev/DeInitDev (with
	 * the InitParam wrapper).  Both symbols exist in the OpenIPC
	 * libmi_rgn.so on this device, but only MI_RGN_Init's kernel
	 * ioctl path is stable; MI_RGN_InitDev triggers a kernel oops in
	 * MI_DEVICE_Ioctl → kfree on this kernel/lib pair. */
	int (*fnInit)(uint16_t, mr_rgn_palette_table *);
	int (*fnDeInit)(uint16_t);
	int (*fnCreate)(uint16_t, int, mr_rgn_attr *);
	int (*fnDestroy)(uint16_t, int);
	int (*fnAttach)(uint16_t, int, mr_rgn_chnport *, mr_rgn_chnport_param *);
	int (*fnDetach)(uint16_t, int, mr_rgn_chnport *);
	int (*fnGetCanvas)(uint16_t, int, mr_rgn_canvas_info *);
	int (*fnUpdateCanvas)(uint16_t, int);
};

static void osd_canvas_from_info(OsdCanvas *out, const mr_rgn_canvas_info *info,
                                 uint32_t width, uint32_t height)
{
	out->pixels = (uint8_t *)(uintptr_t)info->virtAddr;
	out->stride_bytes = info->u32Stride;
	out->width = width;
	out->height = height;
}

static void palette_init(mr_rgn_palette_table *pal)
{
	const OsdPaletteEntry *src = osd_palette();
	memset(pal, 0, sizeof(*pal));
	for (unsigned i = 0; i < OSD_PALETTE_SIZE; i++) {
		pal->astElement[i].u8Alpha = src[i].alpha;
		pal->astElement[i].u8Red   = src[i].red;
		pal->astElement[i].u8Green = src[i].green;
		pal->astElement[i].u8Blue  = src[i].blue;
	}
}

static int rgn_load(DebugOsdState *ctx)
{
	/* maruko_mi_init() already preloaded libmi_rgn.so with RTLD_GLOBAL,
	 * so this dlopen is effectively a refcount bump that returns the
	 * existing handle. */
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
	LOAD_SYM(fnDeInit,         "MI_RGN_DeInit");
	LOAD_SYM(fnCreate,         "MI_RGN_Create");
	LOAD_SYM(fnDestroy,        "MI_RGN_Destroy");
	LOAD_SYM(fnAttach,         "MI_RGN_AttachToChn");
	LOAD_SYM(fnDetach,         "MI_RGN_DetachFromChn");
	LOAD_SYM(fnGetCanvas,      "MI_RGN_GetCanvasInfo");
	LOAD_SYM(fnUpdateCanvas,   "MI_RGN_UpdateCanvas");

#undef LOAD_SYM
	return 0;
}

DebugOsdState *debug_osd_create(uint32_t frame_w, uint32_t frame_h,
                                const void *vpe_port)
{
	(void)vpe_port;  /* Maruko channel coords are fixed (SCL/0/0/0) */

	DebugOsdState *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) return NULL;

	ctx->width = frame_w;
	ctx->height = frame_h;
	ctx->font_scale = 3;
	ctx->chnport.eModId          = RGN_MODID_SCL;
	ctx->chnport.s32DevId        = 0;
	ctx->chnport.s32ChnId        = 0;
	ctx->chnport.s32OutputPortId = 0;

	if (rgn_load(ctx) != 0) {
		free(ctx);
		return NULL;
	}

	/* MI_SYS_Init already called by maruko_pipeline_init().  Initialise
	 * the RGN device here on the main thread (kernel mi_rgn driver's
	 * singlethread workqueue must be created from the main task). */
	mr_rgn_palette_table pal;
	palette_init(&pal);
	if (ctx->fnInit(RGN_SOC_ID, &pal) != 0) {
		fprintf(stderr, "[debug_osd] MI_RGN_Init failed\n");
		dlclose(ctx->lib);
		free(ctx);
		return NULL;
	}

	mr_rgn_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.eType = MR_RGN_TYPE_OSD;
	attr.stOsdInitParam.ePixelFmt = MR_RGN_PIXFMT_I4;
	attr.stOsdInitParam.stSize.u32Width  = frame_w;
	attr.stOsdInitParam.stSize.u32Height = frame_h;

	if (ctx->fnCreate(RGN_SOC_ID, RGN_HANDLE, &attr) != 0) {
		fprintf(stderr, "[debug_osd] MI_RGN_Create failed (%ux%u)\n",
			frame_w, frame_h);
		ctx->fnDeInit(RGN_SOC_ID);
		dlclose(ctx->lib);
		free(ctx);
		return NULL;
	}

	mr_rgn_chnport_param chn_param;
	memset(&chn_param, 0, sizeof(chn_param));
	chn_param.bShow = 1;
	chn_param.stPoint.u32X = 0;
	chn_param.stPoint.u32Y = 0;
	chn_param.unPara.stOsdChnPort.u32Layer = 0;
	chn_param.unPara.stOsdChnPort.stOsdAlphaAttr.eAlphaMode = 0; /* pixel */

	if (ctx->fnAttach(RGN_SOC_ID, RGN_HANDLE, &ctx->chnport, &chn_param) != 0) {
		fprintf(stderr, "[debug_osd] MI_RGN_AttachToChn failed\n");
		ctx->fnDestroy(RGN_SOC_ID, RGN_HANDLE);
		ctx->fnDeInit(RGN_SOC_ID);
		dlclose(ctx->lib);
		free(ctx);
		return NULL;
	}

	if (ctx->fnGetCanvas(RGN_SOC_ID, RGN_HANDLE, &ctx->canvas) != 0) {
		fprintf(stderr, "[debug_osd] MI_RGN_GetCanvasInfo failed\n");
		ctx->fnDetach(RGN_SOC_ID, RGN_HANDLE, &ctx->chnport);
		ctx->fnDestroy(RGN_SOC_ID, RGN_HANDLE);
		ctx->fnDeInit(RGN_SOC_ID);
		dlclose(ctx->lib);
		free(ctx);
		return NULL;
	}

	{
		uint8_t *pixels = (uint8_t *)(uintptr_t)ctx->canvas.virtAddr;
		uint32_t stride = ctx->canvas.u32Stride;
		for (uint32_t y = 0; y < frame_h; y++)
			memset(pixels + y * stride, 0, stride);
		ctx->fnUpdateCanvas(RGN_SOC_ID, RGN_HANDLE);
	}

	osd_dirty_reset(&ctx->dirty, frame_w, frame_h);

	fprintf(stderr, "[debug_osd] overlay %ux%u stride=%u virtAddr=%p (Maruko SCL)\n",
		ctx->canvas.stSize.u32Width, ctx->canvas.stSize.u32Height,
		ctx->canvas.u32Stride, (void *)(uintptr_t)ctx->canvas.virtAddr);
	return ctx;
}

void debug_osd_destroy(DebugOsdState *osd)
{
	if (!osd) return;
	osd->fnDetach(RGN_SOC_ID, RGN_HANDLE, &osd->chnport);
	osd->fnDestroy(RGN_SOC_ID, RGN_HANDLE);
	osd->fnDeInit(RGN_SOC_ID);
	if (osd->lib)
		dlclose(osd->lib);
	free(osd);
}

void debug_osd_begin_frame(DebugOsdState *osd)
{
	if (!osd) return;
	if (osd->fnGetCanvas(RGN_SOC_ID, RGN_HANDLE, &osd->canvas) != 0)
		return;
	OsdCanvas c;
	osd_canvas_from_info(&c, &osd->canvas, osd->width, osd->height);
	osd_clear_dirty(&c, &osd->dirty);
	osd_dirty_reset(&osd->dirty, osd->width, osd->height);
}

void debug_osd_end_frame(DebugOsdState *osd)
{
	if (!osd) return;
	osd->fnUpdateCanvas(RGN_SOC_ID, RGN_HANDLE);
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
	int row_h = char_h + 2 * s;
	int char_w = 6 * s;
	uint16_t y = (uint16_t)(PANEL_Y + row * row_h);
	if ((uint32_t)y + (uint32_t)char_h > osd->height) return;

	char line[LINE_MAX];
	int len = snprintf(line, sizeof(line), "%s: %s", label, value);
	if (len < 0) return;
	if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;

	OsdCanvas c;
	osd_canvas_from_info(&c, &osd->canvas, osd->width, osd->height);

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
	osd_cpu_sample(&osd->cpu);
}

int debug_osd_get_cpu(DebugOsdState *osd)
{
	return osd ? osd->cpu.pct : 0;
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

/* Maruko OSD attaches at fixed SCL/0/0/0 dim = encoded dim, so no panel
 * offset is ever needed. Provided to satisfy the shared header. */
void debug_osd_set_panel_offset(DebugOsdState *osd, int off_x, int off_y)
{ (void)osd; (void)off_x; (void)off_y; }

#elif defined(PLATFORM_CV610)

#include "debug_osd_draw.h"

#include "ot_common_region.h"
#include "ss_mpi_region.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RGN_HANDLE 0
#define PANEL_X 8
#define PANEL_Y 8
#define LINE_MAX 64

struct DebugOsdState {
	uint32_t width, height;
	ot_rgn_canvas_info canvas;
	ot_mpp_chn channel;
	OsdDirty dirty;
	int font_scale;
	int panel_off_x;
	int panel_off_y;
	int created;
	int attached;
	OsdCpuSampler cpu;
};

static void cv610_canvas(OsdCanvas *out, const DebugOsdState *ctx)
{
	out->pixels = ctx->canvas.virt_addr;
	out->stride_bytes = ctx->canvas.stride;
	out->width = ctx->width;
	out->height = ctx->height;
}

static uint32_t cv610_clut_entry(const OsdPaletteEntry *entry)
{
	return ((uint32_t)entry->alpha << 24) |
		((uint32_t)entry->red << 16) |
		((uint32_t)entry->green << 8) | entry->blue;
}

DebugOsdState *debug_osd_create(uint32_t frame_w, uint32_t frame_h,
	const void *vpe_port)
{
	DebugOsdState *ctx;
	ot_rgn_attr attr;
	ot_rgn_chn_attr chn_attr;
	const OsdPaletteEntry *palette;
	td_s32 ret;
	unsigned int i;

	(void)vpe_port;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;
	ctx->width = frame_w;
	ctx->height = frame_h;
	ctx->font_scale = 3;
	ctx->channel.mod_id = OT_ID_VENC;
	ctx->channel.dev_id = 0;
	ctx->channel.chn_id = 0;

	memset(&attr, 0, sizeof(attr));
	attr.type = OT_RGN_OVERLAY;
	attr.attr.overlay.pixel_format = OT_PIXEL_FORMAT_ARGB_CLUT4;
	attr.attr.overlay.bg_color = DEBUG_OSD_TRANSPARENT;
	attr.attr.overlay.size.width = frame_w;
	attr.attr.overlay.size.height = frame_h;
	attr.attr.overlay.canvas_num = 2;
	palette = osd_palette();
	for (i = 0; i < OSD_PALETTE_SIZE; ++i)
		attr.attr.overlay.clut[i] = cv610_clut_entry(&palette[i]);
	ret = ss_mpi_rgn_create(RGN_HANDLE, &attr);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "[debug_osd] CV610 RGN create failed: 0x%x\n", ret);
		free(ctx);
		return NULL;
	}
	ctx->created = 1;

	memset(&chn_attr, 0, sizeof(chn_attr));
	chn_attr.is_show = TD_TRUE;
	chn_attr.type = OT_RGN_OVERLAY;
	chn_attr.attr.overlay_chn.point.x = 0;
	chn_attr.attr.overlay_chn.point.y = 0;
	chn_attr.attr.overlay_chn.fg_alpha = 255;
	chn_attr.attr.overlay_chn.bg_alpha = 0;
	chn_attr.attr.overlay_chn.layer = 0;
	chn_attr.attr.overlay_chn.qp_info.enable = TD_FALSE;
	chn_attr.attr.overlay_chn.dst = OT_RGN_ATTACH_JPEG_MAIN;
	ret = ss_mpi_rgn_attach_to_chn(RGN_HANDLE, &ctx->channel, &chn_attr);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "[debug_osd] CV610 RGN attach failed: 0x%x\n", ret);
		debug_osd_destroy(ctx);
		return NULL;
	}
	ctx->attached = 1;
	ret = ss_mpi_rgn_get_canvas_info(RGN_HANDLE, &ctx->canvas);
	if (ret != TD_SUCCESS || !ctx->canvas.virt_addr ||
		ctx->canvas.stride < (frame_w + 1u) / 2u) {
		fprintf(stderr, "[debug_osd] CV610 canvas invalid: 0x%x\n", ret);
		debug_osd_destroy(ctx);
		return NULL;
	}
	for (i = 0; i < frame_h; ++i)
		memset((uint8_t *)ctx->canvas.virt_addr + i * ctx->canvas.stride,
			0, ctx->canvas.stride);
	if (ss_mpi_rgn_update_canvas(RGN_HANDLE) != TD_SUCCESS) {
		debug_osd_destroy(ctx);
		return NULL;
	}
	osd_dirty_reset(&ctx->dirty, frame_w, frame_h);
	fprintf(stdout, "[debug_osd] CV610 CLUT4 overlay %ux%u stride=%u\n",
		frame_w, frame_h, ctx->canvas.stride);
	return ctx;
}

void debug_osd_destroy(DebugOsdState *osd)
{
	if (!osd)
		return;
	if (osd->attached) {
		(void)ss_mpi_rgn_detach_from_chn(RGN_HANDLE, &osd->channel);
		osd->attached = 0;
		usleep(50000);
	}
	if (osd->created)
		(void)ss_mpi_rgn_destroy(RGN_HANDLE);
	free(osd);
}

void debug_osd_begin_frame(DebugOsdState *osd)
{
	OsdCanvas canvas;

	if (!osd || ss_mpi_rgn_get_canvas_info(RGN_HANDLE, &osd->canvas) != TD_SUCCESS)
		return;
	cv610_canvas(&canvas, osd);
	osd_clear_dirty(&canvas, &osd->dirty);
	osd_dirty_reset(&osd->dirty, osd->width, osd->height);
}

void debug_osd_end_frame(DebugOsdState *osd)
{
	if (osd)
		(void)ss_mpi_rgn_update_canvas(RGN_HANDLE);
}

void debug_osd_text(DebugOsdState *osd, int row, const char *label,
	const char *fmt, ...)
{
	char value[48];
	char line[LINE_MAX];
	va_list ap;
	OsdCanvas canvas;
	int s, char_h, row_h, char_w, panel_x, panel_y, len, bg_x, bg_y;
	uint16_t y, bg_w;

	if (!osd)
		return;
	va_start(ap, fmt);
	vsnprintf(value, sizeof(value), fmt, ap);
	va_end(ap);
	s = osd->font_scale;
	char_h = 8 * s;
	row_h = char_h + 2 * s;
	char_w = 6 * s;
	panel_x = PANEL_X + osd->panel_off_x;
	panel_y = PANEL_Y + osd->panel_off_y;
	y = (uint16_t)(panel_y + row * row_h);
	if ((uint32_t)y + (uint32_t)char_h > osd->height)
		return;
	len = snprintf(line, sizeof(line), "%s: %s", label, value);
	if (len < 0)
		return;
	if (len >= (int)sizeof(line))
		len = (int)sizeof(line) - 1;
	cv610_canvas(&canvas, osd);
	bg_w = (uint16_t)(len * char_w + 4 * s);
	bg_x = panel_x - 2;
	bg_y = (int)y - s;
	if (bg_x < 0) bg_x = 0;
	if (bg_y < 0) bg_y = 0;
	osd_draw_rect(&canvas, &osd->dirty, (uint16_t)bg_x, (uint16_t)bg_y,
		bg_w, (uint16_t)(char_h + 2 * s), DEBUG_OSD_SEMITRANS_BLACK, 1);
	osd_draw_string(&canvas, &osd->dirty, panel_x, y, line, s,
		DEBUG_OSD_WHITE);
}

void debug_osd_set_panel_offset(DebugOsdState *osd, int off_x, int off_y)
{
	if (!osd) return;
	osd->panel_off_x = off_x;
	osd->panel_off_y = off_y;
}

void debug_osd_sample_cpu(DebugOsdState *osd)
{ if (osd) osd_cpu_sample(&osd->cpu); }

int debug_osd_get_cpu(DebugOsdState *osd)
{ return osd ? osd->cpu.pct : 0; }

void debug_osd_rect(DebugOsdState *osd, uint16_t x, uint16_t y,
	uint16_t w, uint16_t h, uint16_t color, int filled)
{
	OsdCanvas canvas;
	if (!osd) return;
	cv610_canvas(&canvas, osd);
	osd_draw_rect(&canvas, &osd->dirty, x, y, w, h, color, filled);
}

void debug_osd_point(DebugOsdState *osd, uint16_t x, uint16_t y,
	uint16_t color, int size)
{
	OsdCanvas canvas;
	if (!osd) return;
	cv610_canvas(&canvas, osd);
	osd_draw_point(&canvas, &osd->dirty, x, y, color, size);
}

void debug_osd_line(DebugOsdState *osd, uint16_t x0, uint16_t y0,
	uint16_t x1, uint16_t y1, uint16_t color)
{
	OsdCanvas canvas;
	if (!osd) return;
	cv610_canvas(&canvas, osd);
	osd_draw_line(&canvas, &osd->dirty, x0, y0, x1, y1, color);
}

#else /* !PLATFORM_STAR6E && !PLATFORM_MARUKO */

DebugOsdState *debug_osd_create(uint32_t frame_w, uint32_t frame_h,
                                const void *vpe_port)
{ (void)frame_w; (void)frame_h; (void)vpe_port; return NULL; }

void debug_osd_set_panel_offset(DebugOsdState *osd, int off_x, int off_y)
{ (void)osd; (void)off_x; (void)off_y; }

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

#endif /* PLATFORM_STAR6E / PLATFORM_MARUKO */
