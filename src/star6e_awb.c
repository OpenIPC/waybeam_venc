#include "star6e_awb.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Struct layouts are the i6e MI_ISP ABI, copied from the SDK header
 * mi_isp_datatype.h.  Every CUS3A entry point takes a POINTER to its struct —
 * passing by value silently misreads (the SetRcPriority class of bug). */

typedef struct {
	uint32_t Size;
	uint32_t AvgBlkX;
	uint32_t AvgBlkY;
	uint32_t CurRGain;
	uint32_t CurGGain;
	uint32_t CurBGain;
	void    *avgs;
	uint8_t  HDRMode;
	void   **pAwbStatisShort;
	uint32_t u4BVx16384;
	int32_t  WeightY;
} __attribute__((packed, aligned(1))) AwbCusInfo;

typedef struct {
	uint32_t Size;
	uint32_t Change;    /* 1 = apply to hw register */
	uint32_t R_gain;
	uint32_t G_gain;
	uint32_t B_gain;
	uint32_t ColorTmp;
} AwbCusResult;

typedef struct { uint8_t r, g, b; } AwbAvg;

#define AWB_MAX_BLOCKS (128 * 90)

typedef struct {
	uint32_t nBlkX;
	uint32_t nBlkY;
	AwbAvg   nAvg[AWB_MAX_BLOCKS];
} AwbHwStats;

/* Blocks outside this band are excluded from the average: near-saturated
 * blocks have clipped channels (their ratios are meaningless) and near-black
 * blocks are mostly read noise. */
#define AWB_BLOCK_MAX 250
#define AWB_BLOCK_MIN 16

/* Gains are in the SDK's fixed-point base (1024 = 1.0x on this platform).
 * The clamp keeps a degenerate scene — a lens cap, a single-colour wall —
 * from driving a wild correction that then takes many ticks to unwind. */
#define AWB_GAIN_MIN 256u
#define AWB_GAIN_MAX 8192u

/* Exponential damping: each tick moves 1/AWB_DAMP of the way to the target,
 * so a scene change eases in over ~1 s at 5 Hz instead of snapping. */
#define AWB_DAMP 4

static struct {
	void *lib;
	int (*fn_get_stats)(uint32_t, AwbHwStats *);
	int (*fn_get_status)(uint32_t, AwbCusInfo *);
	int (*fn_set_param)(uint32_t, AwbCusResult *);

	pthread_t thread;
	volatile int running;
	volatile int paused;
	volatile uint32_t hz;

	AwbHwStats *stats;          /* ~35 KB — allocated once, not per tick */
	uint32_t cur_r, cur_g, cur_b;
	volatile uint32_t ticks;
} g_awb;

/* Percent of the current gain within which a new target is treated as "no
 * change".  Wide enough to swallow block-average noise on a static scene,
 * narrow enough that a real light change still moves. */
#define AWB_DEADBAND_PCT 3

static int awb_within_deadband(uint32_t cur, uint32_t target)
{
	uint32_t diff = cur > target ? cur - target : target - cur;

	return diff * 100 <= cur * AWB_DEADBAND_PCT;
}

static uint32_t awb_clamp(uint32_t v)
{
	if (v < AWB_GAIN_MIN)
		return AWB_GAIN_MIN;
	if (v > AWB_GAIN_MAX)
		return AWB_GAIN_MAX;
	return v;
}

/* Grey-world over the accepted blocks: assume the scene averages to neutral,
 * so per-channel gains are inversely proportional to the channel means.  G is
 * held at whatever the ISP currently applies and R/B are solved against it,
 * which keeps overall exposure untouched (this loop must not fight AE).
 * Returns 0 when too little of the frame is usable to trust an estimate. */
static int awb_compute(const AwbHwStats *st, uint32_t g_gain,
	uint32_t *out_r, uint32_t *out_b)
{
	uint64_t sr = 0, sg = 0, sb = 0;
	uint32_t n = st->nBlkX * st->nBlkY;
	uint32_t used = 0;
	uint32_t i;

	if (n > AWB_MAX_BLOCKS)
		n = AWB_MAX_BLOCKS;
	if (n == 0 || g_gain == 0)
		return 0;

	for (i = 0; i < n; i++) {
		uint8_t r = st->nAvg[i].r, g = st->nAvg[i].g, b = st->nAvg[i].b;
		uint8_t hi = r > g ? r : g;

		if (b > hi)
			hi = b;
		if (hi >= AWB_BLOCK_MAX || hi < AWB_BLOCK_MIN)
			continue;
		sr += r;
		sg += g;
		sb += b;
		used++;
	}

	/* Under ~5% usable the estimate is noise — hold the last gains. */
	if (used < n / 20 || sr == 0 || sb == 0)
		return 0;

	*out_r = awb_clamp((uint32_t)((uint64_t)g_gain * sg / sr));
	*out_b = awb_clamp((uint32_t)((uint64_t)g_gain * sg / sb));
	return 1;
}

static void awb_tick(void)
{
	AwbCusInfo info;
	AwbCusResult res;
	uint32_t tgt_r = 0, tgt_b = 0;
	uint32_t new_r, new_b;

	memset(&info, 0, sizeof(info));
	info.Size = sizeof(info);
	if (g_awb.fn_get_status(0, &info) != 0)
		return;
	if (g_awb.fn_get_stats(0, g_awb.stats) != 0)
		return;

	/* Track the ISP's own view of the applied gains so an external write
	 * (a manual-mode excursion, a bin reload) is picked up rather than
	 * fought. */
	g_awb.cur_r = info.CurRGain;
	g_awb.cur_g = info.CurGGain;
	g_awb.cur_b = info.CurBGain;

	if (!awb_compute(g_awb.stats, info.CurGGain, &tgt_r, &tgt_b))
		return;

	/* Deadband: a settled scene still jitters the block averages by a few
	 * counts, which would otherwise have us rewriting gains every tick and
	 * hunting visibly.  Only chase a target that is meaningfully different. */
	if (awb_within_deadband(info.CurRGain, tgt_r) &&
	    awb_within_deadband(info.CurBGain, tgt_b))
		return;

	new_r = (uint32_t)((int32_t)info.CurRGain +
		((int32_t)tgt_r - (int32_t)info.CurRGain) / AWB_DAMP);
	new_b = (uint32_t)((int32_t)info.CurBGain +
		((int32_t)tgt_b - (int32_t)info.CurBGain) / AWB_DAMP);
	/* Integer damping stalls one step short; snap the final unit. */
	if (new_r == info.CurRGain)
		new_r = tgt_r;
	if (new_b == info.CurBGain)
		new_b = tgt_b;

	if (new_r == info.CurRGain && new_b == info.CurBGain)
		return;

	memset(&res, 0, sizeof(res));
	res.Size = sizeof(res);
	res.Change = 1;
	res.R_gain = awb_clamp(new_r);
	res.G_gain = info.CurGGain;
	res.B_gain = awb_clamp(new_b);
	res.ColorTmp = 0;   /* estimate not modelled; the gains are the output */
	(void)g_awb.fn_set_param(0, &res);

	g_awb.cur_r = res.R_gain;
	g_awb.cur_b = res.B_gain;
	g_awb.ticks++;
}

static void *awb_thread_main(void *arg)
{
	(void)arg;
	while (g_awb.running) {
		uint32_t hz = g_awb.hz;
		uint32_t sleep_ms = hz ? 1000u / hz : 200u;

		if (!g_awb.paused && hz)
			awb_tick();

		/* Sleep in <=100 ms slices so stop() joins promptly — respawn
		 * latency is fragile on this SoC. */
		while (sleep_ms && g_awb.running) {
			uint32_t slice = sleep_ms > 100 ? 100 : sleep_ms;
			struct timespec ts = {
				.tv_sec = 0,
				.tv_nsec = (long)slice * 1000000L,
			};
			nanosleep(&ts, NULL);
			sleep_ms -= slice;
		}
	}
	return NULL;
}

int star6e_awb_start(uint32_t hz)
{
	if (g_awb.running) {
		star6e_awb_set_rate(hz);
		return 0;
	}

	if (!g_awb.lib) {
		g_awb.lib = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
		if (!g_awb.lib) {
			fprintf(stderr, "WARNING: [awb] dlopen libmi_isp.so "
				"failed: %s\n", dlerror());
			return -1;
		}
		g_awb.fn_get_stats = (int (*)(uint32_t, AwbHwStats *))
			dlsym(g_awb.lib, "MI_ISP_AWB_GetAwbHwAvgStats");
		g_awb.fn_get_status = (int (*)(uint32_t, AwbCusInfo *))
			dlsym(g_awb.lib, "MI_ISP_CUS3A_GetAwbStatus");
		g_awb.fn_set_param = (int (*)(uint32_t, AwbCusResult *))
			dlsym(g_awb.lib, "MI_ISP_CUS3A_SetAwbParam");
	}
	if (!g_awb.fn_get_stats || !g_awb.fn_get_status || !g_awb.fn_set_param) {
		fprintf(stderr, "WARNING: [awb] CUS3A AWB entry points missing "
			"— userspace AWB disabled\n");
		return -1;
	}

	if (!g_awb.stats) {
		g_awb.stats = calloc(1, sizeof(*g_awb.stats));
		if (!g_awb.stats) {
			fprintf(stderr, "WARNING: [awb] stats alloc failed\n");
			return -1;
		}
	}

	g_awb.hz = hz;
	g_awb.paused = 0;
	g_awb.ticks = 0;
	g_awb.running = 1;
	if (pthread_create(&g_awb.thread, NULL, awb_thread_main, NULL) != 0) {
		g_awb.running = 0;
		fprintf(stderr, "WARNING: [awb] thread create failed\n");
		return -1;
	}
	printf("> [awb] userspace AWB loop @%u Hz (frame-rate independent)\n", hz);
	return 0;
}

void star6e_awb_stop(void)
{
	if (!g_awb.running)
		return;
	g_awb.running = 0;
	pthread_join(g_awb.thread, NULL);
	free(g_awb.stats);
	g_awb.stats = NULL;
	/* g_awb.lib is deliberately left open: it is RTLD_GLOBAL and shared
	 * with the rest of the ISP paths, and closing it across a pipeline
	 * restart has no benefit. */
}

void star6e_awb_set_rate(uint32_t hz)
{
	g_awb.hz = hz;
}

void star6e_awb_set_paused(int paused)
{
	g_awb.paused = paused ? 1 : 0;
}

int star6e_awb_status(uint32_t *r_gain, uint32_t *g_gain, uint32_t *b_gain,
	uint32_t *ticks, int *paused)
{
	if (!g_awb.running)
		return 0;
	if (r_gain) *r_gain = g_awb.cur_r;
	if (g_gain) *g_gain = g_awb.cur_g;
	if (b_gain) *b_gain = g_awb.cur_b;
	if (ticks)  *ticks  = g_awb.ticks;
	if (paused) *paused = g_awb.paused;
	return 1;
}
