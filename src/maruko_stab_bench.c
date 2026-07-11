/* maruko_stab_bench.c — Phase-1 SCL bench for the Maruko stab port.
 *
 * DEVELOPMENT-ONLY.  Compiled in only when the build sets STAB_BENCH=1
 * (-DHAVE_STAB_BENCH=1); the header degrades to no-op stubs otherwise, so a
 * production image carries none of this.  Even in a bench build it is inert
 * unless WAYBEAM_STAB_BENCH is set at launch.
 *
 * It answers the go/no-go questions in specs/2026-07-08-maruko-stab/plan.md
 * that can only be measured against the LIVE ISP->SCL(port0)->VENC pipeline —
 * which is why the bench lives inside waybeam rather than as a standalone tool
 * (a second process cannot attach to MI_SCL's per-process channel state).
 *
 * All four sub-benches reuse the running MI stack: MI_SYS is already
 * initialised by the main process, SCL device 0 / channel 0 already exists
 * with all four HW ports armed (scl_bind=0xF), port 0 is streaming to VENC and
 * port 1 to the MJPEG snapshot backend.  Ports 2 and 3 are free.  We tap port
 * 2, and (for 1b/1d) poke port 0 exactly the way maruko_pan_apply_locked does,
 * then restore it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/select.h>

#include "maruko_pipeline.h"   /* MarukoBackendContext, ctx->scl_crop_*, cfg */
#include "maruko_mi.h"         /* g_mi_scl (maruko_scl_impl) */
#include "maruko_bindings.h"   /* i6c_scl_port */
#include "maruko_stab_bench.h"

/* ── MI_SYS buffer ABI (mirrors src/star6e_framing_stab.c:40-103 verbatim) ── */
typedef struct {
	MI_U16 u16X, u16Y, u16Width, u16Height;
} SbWindowRect_t;

typedef struct {
	int eTileMode, ePixelFormat, eCompressMode, eFrameScanMode;
	int eFieldType, ePhylayoutType;
	MI_U16 u16Width, u16Height;
	void *pVirAddr[3];
	MI_U64 phyAddr[3];
	MI_U32 u32Stride[3];
	MI_U32 u32BufSize;
	MI_U16 u16RingBufStartLine, u16RingBufRealTotalHeight;
	struct {
		int eType;
		union { MI_U32 u32GlobalGradient; } uIspInfo;
	} stFrameIspInfo;
	SbWindowRect_t stContentCropWindow;
} SbFrameData_t;

typedef struct {
	void *pVirAddr; MI_U64 phyAddr;
	MI_U32 u32BufSize, u32ContentSize;
	MI_U8 bEndOfFrame; MI_U64 u64SeqNum;
} SbRawData_t;

typedef struct {
	void *pVirAddr; MI_U64 phyAddr;
	MI_U32 u32Size, u32ExtraData, eDataFromModule;
} SbMetaData_t;

typedef struct {
	MI_U64 u64Pts, u64SidebandMsg;
	int eBufType;
	MI_U8 bEndOfStream, bUsrBuf;
	MI_U32 u32SequenceNumber;
	MI_U8 bDrop;
	union {
		SbFrameData_t stFrameData;
		SbRawData_t stRawData;
		SbMetaData_t stMetaData;
		MI_U8 reserved_union[512];
	};
	MI_U8 u8CusFlag;
} SbBufInfo_t;

/* ── IVE types (verbatim from tools/ive_i6c_shift.c, device-proven) ───────── */
#define E_IVE_IMAGE_TYPE_U8C1 0x0
#define E_IVE_IMAGE_TYPE_S8C1 0x1
#define E_IVE_SHIFT_DETECT_MODE_SINGLE 0x00

typedef struct {
	int eType;
	MI_U64 aphyPhyAddr[3];
	MI_U8 *apu8VirAddr[3];
	MI_U16 azu16Stride[3];
	MI_U16 u16Width, u16Height, u16Reserved;
} IveImage_t;

typedef struct {
	int enMode;
	MI_U8 pyramid_level, search_range;
	MI_U16 u16Left, u16Top, u16Width, u16Height;
} IveShiftCtrl_t;

/* ── vendor fn pointers (dlsym'd from the already-loaded libs) ────────────── */
typedef MI_S32 (*fn_getfd)(MI_SYS_ChnPort_t *, MI_S32 *);
typedef MI_S32 (*fn_closefd)(MI_S32);
typedef MI_S32 (*fn_getbuf)(MI_SYS_ChnPort_t *, SbBufInfo_t *, MI_S32 *);
typedef MI_S32 (*fn_putbuf)(MI_S32);
/* i6c ABI: SetChnOutputPortDepth takes a leading u16 soc_id (BSP mi_sys.h:97);
 * GetBuf/GetFd/PutBuf do NOT (mi_sys.h:81,102,84). */
typedef MI_S32 (*fn_setdepth)(MI_U16, MI_SYS_ChnPort_t *, MI_U32, MI_U32);
typedef MI_S32 (*fn_flush)(void *, MI_U32);
typedef MI_S32 (*fn_mma_alloc)(MI_U16, MI_U8 *, MI_U32, MI_U64 *);
typedef MI_S32 (*fn_mma_free)(MI_U16, MI_U64);
typedef MI_S32 (*fn_mmap)(MI_U64, MI_U32, void **, MI_U8);
typedef MI_S32 (*fn_munmap)(void *, MI_U32);
typedef MI_S32 (*fn_ive_create)(int);
typedef MI_S32 (*fn_ive_destroy)(int);
typedef MI_S32 (*fn_ive_shift)(int, IveImage_t *, IveImage_t *,
	IveImage_t *, IveImage_t *, IveShiftCtrl_t *, MI_U8);

static fn_getfd    SysGetFd;
static fn_closefd  SysCloseFd;
static fn_getbuf   SysGetBuf;
static fn_putbuf   SysPutBuf;
static fn_setdepth SysSetDepth;
static fn_flush    SysFlush;
static fn_mma_alloc MmaAlloc;
static fn_mma_free  MmaFree;
static fn_mmap     SysMmap;
static fn_munmap   SysMunmap;
static fn_ive_create  IveCreate;
static fn_ive_destroy IveDestroy;
static fn_ive_shift   IveShift;

/* ── tap geometry — mirrors STAB_SHIFT_* in star6e_framing_stab.c ─────────── */
#define TAP_W 384
#define TAP_H 384
#define BOX   256
#define PYRAMID 3
#define SEARCH_RANGE 96
#define TAP_PORT 2

static pthread_t g_bench_thread;
static int g_bench_running;
static volatile int g_bench_stop;

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static double cpu_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* SCL frame-processing counter from the SigmaStar proc interface.  The CMDQ
 * "TotalKickoff" field increments once per frame the SCL channel processes, so
 * sampling it at two instants gives the SCL throughput between them — a true
 * in-device "did the channel stall?" signal, independent of our own drain. */
static unsigned long read_scl_kickoff(void)
{
	FILE *f = fopen("/proc/mi_modules/mi_scl/mi_scl0", "r");
	if (!f) return 0;
	char line[512];
	unsigned long val = 0;
	int hdr = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "TotalKickoff")) { hdr = 1; continue; }
		if (hdr) {
			unsigned long passid, cmdqid, total;
			if (sscanf(line, " %lu %lu %lu", &passid, &cmdqid, &total) == 3) {
				val = total;
				break;
			}
		}
	}
	fclose(f);
	return val;
}

static int load_syms(void)
{
	/* libmi_sys / libmi_ive are already mapped by the main process; dlopen
	 * just bumps the refcount and hands back a usable handle. */
	void *sys = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
	void *ive = dlopen("libmi_ive.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!sys) { fprintf(stderr, "[bench] dlopen libmi_sys: %s\n", dlerror()); return -1; }
	SysGetFd   = (fn_getfd)   dlsym(sys, "MI_SYS_GetFd");
	SysCloseFd = (fn_closefd) dlsym(sys, "MI_SYS_CloseFd");
	SysGetBuf  = (fn_getbuf)  dlsym(sys, "MI_SYS_ChnOutputPortGetBuf");
	SysPutBuf  = (fn_putbuf)  dlsym(sys, "MI_SYS_ChnOutputPortPutBuf");
	SysSetDepth = (fn_setdepth) dlsym(sys, "MI_SYS_SetChnOutputPortDepth");
	SysFlush   = (fn_flush)   dlsym(sys, "MI_SYS_FlushInvCache");
	MmaAlloc   = (fn_mma_alloc) dlsym(sys, "MI_SYS_MMA_Alloc");
	MmaFree    = (fn_mma_free)  dlsym(sys, "MI_SYS_MMA_Free");
	SysMmap    = (fn_mmap)    dlsym(sys, "MI_SYS_Mmap");
	SysMunmap  = (fn_munmap)  dlsym(sys, "MI_SYS_Munmap");
	if (!SysGetBuf || !SysPutBuf || !SysFlush) {
		fprintf(stderr, "[bench] missing MI_SYS drain symbols\n");
		return -1;
	}
	if (ive) {
		IveCreate  = (fn_ive_create)  dlsym(ive, "MI_IVE_Create");
		IveDestroy = (fn_ive_destroy) dlsym(ive, "MI_IVE_Destroy");
		IveShift   = (fn_ive_shift)   dlsym(ive, "MI_IVE_Shift_Detector");
	}
	return 0;
}

/* Build the free-port-2 tap config: same source window as port 0, downscaled
 * to TAP_W x TAP_H, RAW (compress=0) so we can CPU-read the Y plane. */
static void build_tap_port(MarukoBackendContext *ctx, i6c_scl_port *p)
{
	memset(p, 0, sizeof(*p));
	p->crop.x = (unsigned short)ctx->scl_crop_x;
	p->crop.y = (unsigned short)ctx->scl_crop_y;
	p->crop.width = (unsigned short)ctx->scl_crop_w;
	p->crop.height = (unsigned short)ctx->scl_crop_h;
	p->output.width = TAP_W;
	p->output.height = TAP_H;
	p->pixFmt = I6_PIXFMT_YUV420SP;
	p->compress = (i6_common_compr)0; /* RAW */
}

/* Reconstruct port 0's live config the way maruko_pan_apply_locked does at
 * centre (x=y=0.5, no zoom) — used by 1b to poke and then restore. */
static void build_port0(MarukoBackendContext *ctx, i6c_scl_port *p,
	int dx)
{
	memset(p, 0, sizeof(*p));
	p->crop.x = (unsigned short)((int)ctx->scl_crop_x + dx);
	p->crop.y = (unsigned short)ctx->scl_crop_y;
	p->crop.width = (unsigned short)ctx->scl_crop_w;
	p->crop.height = (unsigned short)ctx->scl_crop_h;
	p->output.width = (unsigned short)ctx->cfg.image_width;
	p->output.height = (unsigned short)ctx->cfg.image_height;
	p->pixFmt = I6_PIXFMT_YUV420SP;
	p->compress = (i6_common_compr)6; /* IFC — required for the VENC ring */
}

/* ── 1a: port-2 tap cadence ───────────────────────────────────────────────
 * Enable port 2, drain for dur_s, report frame count / measured fps / jitter /
 * CPU cost.  Leaves the port ENABLED iff keep!=0 (1d reuses it). */
static int bench_tap_enable(MarukoBackendContext *ctx)
{
	i6c_scl_port p;
	MI_S32 ret;
	build_tap_port(ctx, &p);
	ret = g_mi_scl.fnSetPortConfig(0, 0, TAP_PORT, &p);
	if (ret != 0) {
		fprintf(stderr, "[bench] port2 SetPortConfig -> %d\n", (int)ret);
		return -1;
	}
	ret = g_mi_scl.fnEnablePort(0, 0, TAP_PORT);
	if (ret != 0) {
		fprintf(stderr, "[bench] port2 EnablePort -> %d\n", (int)ret);
		return -1;
	}
	/* An UNBOUND SCL output port passes frames only to a downstream bind;
	 * to make it accumulate frames for a userspace GetBuf it needs a
	 * user-frame queue.  Mirrors star6e_framing_stab.c:1624
	 * (MI_SYS_SetChnOutputPortDepth(&vpe1, 2, 4)).  Without this the port
	 * produces 0 frames (the 1a=0-frames symptom). */
	if (SysSetDepth) {
		MI_SYS_ChnPort_t bind;
		memset(&bind, 0, sizeof(bind));
		bind.module = I6_SYS_MOD_SCL;
		bind.device = 0; bind.channel = 0; bind.port = TAP_PORT;
		ret = SysSetDepth(0, &bind, 2, 4);
		if (ret != 0)
			fprintf(stderr, "[bench] port2 SetChnOutputPortDepth -> %d "
				"(tap may stay empty)\n", (int)ret);
	} else {
		fprintf(stderr, "[bench] WARN: no MI_SYS_SetChnOutputPortDepth — "
			"tap will likely produce 0 frames\n");
	}
	return 0;
}
static void bench_tap_disable(void)
{
	(void)g_mi_scl.fnDisablePort(0, 0, TAP_PORT);
}

static void bench_1a(MarukoBackendContext *ctx)
{
	MI_SYS_ChnPort_t bind;
	MI_S32 fd = -1;
	int frames = 0, getbuf_fail = 0;
	double t_start, t_last = 0, ivl_min = 1e9, ivl_max = 0, ivl_sum = 0;
	double c0, c1, wall_dur = 3000.0;

	printf("\n[bench] === 1a: SCL port-2 tap cadence (%dx%d raw) ===\n",
		TAP_W, TAP_H);
	if (bench_tap_enable(ctx) != 0) { printf("[bench] 1a: tap enable FAILED\n"); return; }

	memset(&bind, 0, sizeof(bind));
	bind.module = I6_SYS_MOD_SCL;
	bind.device = 0; bind.channel = 0; bind.port = TAP_PORT;
	if (SysGetFd) (void)SysGetFd(&bind, &fd);

	c0 = cpu_ms();
	t_start = now_ms();
	while (now_ms() - t_start < wall_dur && !g_bench_stop) {
		SbBufInfo_t buf;
		MI_S32 h = -1, r;
		if (fd >= 0) {
			fd_set rf; struct timeval tv = {0, 100000};
			FD_ZERO(&rf); FD_SET(fd, &rf);
			if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0) continue;
		}
		memset(&buf, 0, sizeof(buf));
		r = SysGetBuf(&bind, &buf, &h);
		if (r != 0) { getbuf_fail++; if (fd < 0) usleep(1000); continue; }
		double t = now_ms();
		if (frames > 0) {
			double ivl = t - t_last;
			if (ivl < ivl_min) ivl_min = ivl;
			if (ivl > ivl_max) ivl_max = ivl;
			ivl_sum += ivl;
		}
		t_last = t;
		frames++;
		SysPutBuf(h);
	}
	c1 = cpu_ms();
	if (fd >= 0 && SysCloseFd) SysCloseFd(fd);

	double elapsed = now_ms() - t_start;
	double fps = frames * 1000.0 / elapsed;
	double mean_ivl = (frames > 1) ? ivl_sum / (frames - 1) : 0;
	double drain_cpu_pct = (elapsed > 0) ? (c1 - c0) / elapsed * 100.0 : 0;
	printf("[bench] 1a: %d frames in %.0f ms -> %.1f fps; "
		"ivl mean/min/max = %.2f/%.2f/%.2f ms; getbuf_fail=%d; "
		"drain CPU=%.1f%%\n",
		frames, elapsed, fps, mean_ivl, ivl_min, ivl_max, getbuf_fail,
		drain_cpu_pct);
	printf("BENCH_1A_JSON {\"frames\":%d,\"fps\":%.1f,\"mean_ivl_ms\":%.2f,"
		"\"min_ivl_ms\":%.2f,\"max_ivl_ms\":%.2f,\"drain_cpu_pct\":%.1f}\n",
		frames, fps, mean_ivl, ivl_min, ivl_max, drain_cpu_pct);
	bench_tap_disable();
}

/* ── 1c: SCL crop.x/y granularity on port 2 ───────────────────────────────
 * Does the scaler accept an odd crop.x, or does it reject / silently round?
 * We only test acceptance (ret code) here; content-shift verification would
 * need a full drain+correlate, deferred. */
static void bench_1c(MarukoBackendContext *ctx)
{
	static const int deltas[] = {0, 1, 2, 3, 4};
	printf("\n[bench] === 1c: SCL crop.x granularity (port 2) ===\n");
	if (bench_tap_enable(ctx) != 0) { printf("[bench] 1c: tap enable FAILED\n"); return; }
	for (unsigned i = 0; i < sizeof(deltas)/sizeof(deltas[0]); i++) {
		i6c_scl_port p;
		build_tap_port(ctx, &p);
		p.crop.x = (unsigned short)((int)ctx->scl_crop_x + deltas[i]);
		MI_S32 r = g_mi_scl.fnSetPortConfig(0, 0, TAP_PORT, &p);
		printf("[bench] 1c: crop.x = base+%d (%u) -> SetPortConfig ret=%d %s\n",
			deltas[i], p.crop.x, (int)r,
			(deltas[i] & 1) ? "(odd)" : "(even)");
	}
	bench_tap_disable();
}

/* ── 1b: hammer MI_SCL_SetOutputPortParam on port 0 at target_hz ───────────
 * THE gate.  Reconstructs port0's live struct, then for dur_s alternates
 * crop.x by +/-1 at target_hz.  Frame-drop is measured OUT OF BAND from the
 * host RTP on :5600 — here we log per-call latency + any nonzero ret, then
 * RESTORE the base rect so the stream is left undisturbed. */
static void bench_1b_at(MarukoBackendContext *ctx, int target_hz,
	double dur_s)
{
	int period_us = 1000000 / target_hz;
	int calls = 0, errs = 0, tick = 0;
	double lat_max = 0, lat_sum = 0, t_start;
	unsigned long k0, k1;

	printf("\n[bench] === 1b: port-0 poke @ %d Hz for %.0fs ===\n",
		target_hz, dur_s);
	k0 = read_scl_kickoff();
	t_start = now_ms();
	while (now_ms() - t_start < dur_s * 1000.0 && !g_bench_stop) {
		i6c_scl_port p;
		double a, b;
		build_port0(ctx, &p, (tick & 1) ? 1 : 0);
		a = now_ms();
		MI_S32 r = g_mi_scl.fnSetPortConfig(0, 0, 0, &p);
		b = now_ms();
		double lat = b - a;
		if (lat > lat_max) lat_max = lat;
		lat_sum += lat;
		if (r != 0) errs++;
		calls++; tick++;
		usleep(period_us);
	}
	/* restore exact base rect (dx=0) */
	{
		i6c_scl_port p;
		build_port0(ctx, &p, 0);
		(void)g_mi_scl.fnSetPortConfig(0, 0, 0, &p);
	}
	double elapsed = now_ms() - t_start;
	k1 = read_scl_kickoff();
	double eff_hz = calls * 1000.0 / elapsed;
	/* SCL frames processed DURING the hammer -> the channel's real fps while
	 * we thrashed port0.  If this holds ~50 (== undisturbed delivery) the
	 * poke did NOT stall the SCL; a drop means renegotiation is stalling. */
	double scl_fps = (k1 - k0) * 1000.0 / elapsed;
	printf("[bench] 1b@%dHz: %d calls (%.1f Hz eff), errs=%d, "
		"lat mean/max = %.3f/%.3f ms; SCL processed %lu frames -> "
		"%.1f fps DURING poke\n",
		target_hz, calls, eff_hz, errs,
		calls ? lat_sum / calls : 0, lat_max, k1 - k0, scl_fps);
	printf("BENCH_1B_JSON {\"target_hz\":%d,\"calls\":%d,\"eff_hz\":%.1f,"
		"\"errs\":%d,\"lat_mean_ms\":%.3f,\"lat_max_ms\":%.3f,"
		"\"scl_frames\":%lu,\"scl_fps\":%.1f}\n",
		target_hz, calls, eff_hz, errs,
		calls ? lat_sum / calls : 0, lat_max, k1 - k0, scl_fps);
}
static void bench_1b(MarukoBackendContext *ctx)
{
	int rates[] = {30, 60, 90, 144};
	for (unsigned i = 0; i < sizeof(rates)/sizeof(rates[0]) && !g_bench_stop; i++) {
		bench_1b_at(ctx, rates[i], 4.0);
		usleep(1000000); /* 1s settle between rungs — watch host RTP recover */
	}
}

/* MMA-backed IVE image (from ive_i6c_shift.c). */
static int ive_alloc(IveImage_t *img, MI_U16 w, MI_U16 h, int type)
{
	MI_U32 stride = (w + 63u) & ~63u;
	MI_U32 size = stride * h;
	MI_U64 phy = 0; void *vir = NULL;
	memset(img, 0, sizeof(*img));
	if (!MmaAlloc || !SysMmap) return -1;
	if (MmaAlloc(0, NULL, size, &phy) != 0 || !phy) {
		if (MmaAlloc(0, (MI_U8 *)"mma_heap_name0", size, &phy) != 0 || !phy)
			return -1;
	}
	if (SysMmap(phy, size, &vir, 0) != 0 || !vir) { MmaFree(0, phy); return -1; }
	memset(vir, 0, size);
	img->eType = type; img->u16Width = w; img->u16Height = h;
	img->azu16Stride[0] = (MI_U16)stride;
	img->aphyPhyAddr[0] = phy; img->apu8VirAddr[0] = (MI_U8 *)vir;
	return 0;
}
static void ive_free(IveImage_t *img)
{
	MI_U32 size = (MI_U32)img->azu16Stride[0] * img->u16Height;
	if (img->apu8VirAddr[0] && SysMunmap) SysMunmap(img->apu8VirAddr[0], size);
	if (img->aphyPhyAddr[0] && MmaFree) MmaFree(0, img->aphyPhyAddr[0]);
	memset(img, 0, sizeof(*img));
}

/* ── 1d: full loop — tap -> detector -> port-0 emit — sustained fps proof ──
 * The integration prototype: drains port 2, copies the Y patch into an MMA
 * IVE buffer, runs Shift_Detector against the previous frame, applies the
 * accumulated shift to port 0, and measures the achievable steady-state fps
 * plus the per-frame detector cost.  No Kalman — raw accumulate, clamped —
 * this is a throughput probe, not a quality one. */
static void bench_1d(MarukoBackendContext *ctx)
{
	MI_SYS_ChnPort_t bind;
	IveImage_t a, b, dx, dy;
	MI_S32 fd = -1;
	int frames = 0, det = 0, emit = 0, acc_x = 0, acc_y = 0, cur = 0;
	double t_start, det_cpu = 0, det_wall = 0;
	int max_off = 32;

	printf("\n[bench] === 1d: tap + detector + emit, sustained fps ===\n");
	if (!IveCreate || !IveShift) { printf("[bench] 1d: no IVE symbols\n"); return; }
	if (IveCreate(0) != 0) { printf("[bench] 1d: MI_IVE_Create FAILED\n"); return; }
	if (ive_alloc(&a, TAP_W, TAP_H, E_IVE_IMAGE_TYPE_U8C1) ||
	    ive_alloc(&b, TAP_W, TAP_H, E_IVE_IMAGE_TYPE_U8C1) ||
	    ive_alloc(&dx, 1, 1, E_IVE_IMAGE_TYPE_S8C1) ||
	    ive_alloc(&dy, 1, 1, E_IVE_IMAGE_TYPE_S8C1)) {
		printf("[bench] 1d: IVE alloc FAILED\n");
		if (IveDestroy) IveDestroy(0);
		return;
	}
	if (bench_tap_enable(ctx) != 0) {
		printf("[bench] 1d: tap enable FAILED\n");
		ive_free(&a); ive_free(&b); ive_free(&dx); ive_free(&dy);
		if (IveDestroy) IveDestroy(0);
		return;
	}
	memset(&bind, 0, sizeof(bind));
	bind.module = I6_SYS_MOD_SCL; bind.port = TAP_PORT;
	if (SysGetFd) (void)SysGetFd(&bind, &fd);

	int left = ((TAP_W - BOX) / 2) & ~1, top = ((TAP_H - BOX) / 2) & ~1;
	IveShiftCtrl_t ctrl = {
		.enMode = E_IVE_SHIFT_DETECT_MODE_SINGLE,
		.pyramid_level = PYRAMID, .search_range = SEARCH_RANGE,
		.u16Left = (MI_U16)left, .u16Top = (MI_U16)top,
		.u16Width = BOX, .u16Height = BOX,
	};

	t_start = now_ms();
	while (now_ms() - t_start < 6000.0 && !g_bench_stop) {
		SbBufInfo_t buf; MI_S32 h = -1;
		IveImage_t *prev = (cur & 1) ? &b : &a;
		IveImage_t *curr = (cur & 1) ? &a : &b;
		if (fd >= 0) {
			fd_set rf; struct timeval tv = {0, 100000};
			FD_ZERO(&rf); FD_SET(fd, &rf);
			if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0) continue;
		}
		memset(&buf, 0, sizeof(buf));
		if (SysGetBuf(&bind, &buf, &h) != 0) { if (fd < 0) usleep(1000); continue; }
		/* zero-copy read: Y plane of the drained NV12 frame. */
		MI_U8 *y = (MI_U8 *)buf.stFrameData.pVirAddr[0];
		MI_U32 stride = buf.stFrameData.u32Stride[0];
		if (y && stride >= TAP_W) {
			for (int r = 0; r < TAP_H; r++)
				memcpy(curr->apu8VirAddr[0] + (size_t)r * curr->azu16Stride[0],
				       y + (size_t)r * stride, TAP_W);
			SysFlush(curr->apu8VirAddr[0],
				(MI_U32)curr->azu16Stride[0] * TAP_H);
		}
		SysPutBuf(h);
		frames++;
		if (frames < 2) { cur++; continue; }

		double dw0 = now_ms(), dc0 = cpu_ms();
		MI_S32 r = IveShift(0, prev, curr, &dx, &dy, &ctrl, 1);
		det_wall += now_ms() - dw0; det_cpu += cpu_ms() - dc0;
		det++;
		if (r == 0) {
			SysFlush(dx.apu8VirAddr[0], dx.azu16Stride[0]);
			SysFlush(dy.apu8VirAddr[0], dy.azu16Stride[0]);
			int mdx = -((signed char *)dx.apu8VirAddr[0])[0];
			int mdy = -((signed char *)dy.apu8VirAddr[0])[0];
			acc_x += mdx; acc_y += mdy;
			if (acc_x > max_off) acc_x = max_off;
			if (acc_x < -max_off) acc_x = -max_off;
			if (acc_y > max_off) acc_y = max_off;
			if (acc_y < -max_off) acc_y = -max_off;
			i6c_scl_port p;
			build_port0(ctx, &p, 0);
			p.crop.x = (unsigned short)((int)ctx->scl_crop_x + acc_x);
			p.crop.y = (unsigned short)((int)ctx->scl_crop_y + acc_y);
			if (g_mi_scl.fnSetPortConfig(0, 0, 0, &p) == 0) emit++;
		}
		cur++;
	}
	/* restore base rect */
	{ i6c_scl_port p; build_port0(ctx, &p, 0); (void)g_mi_scl.fnSetPortConfig(0, 0, 0, &p); }

	double elapsed = now_ms() - t_start;
	double loop_fps = frames * 1000.0 / elapsed;
	double det_per = det ? det_wall / det : 0;
	double det_cpu_per = det ? det_cpu / det : 0;
	printf("[bench] 1d: %d frames, %d detections, %d emits in %.0f ms -> "
		"%.1f fps; detector %.2f ms/call wall, %.2f ms CPU\n",
		frames, det, emit, elapsed, loop_fps, det_per, det_cpu_per);
	printf("BENCH_1D_JSON {\"loop_fps\":%.1f,\"frames\":%d,\"det\":%d,"
		"\"emit\":%d,\"det_wall_ms\":%.2f,\"det_cpu_ms\":%.2f}\n",
		loop_fps, frames, det, emit, det_per, det_cpu_per);

	if (fd >= 0 && SysCloseFd) SysCloseFd(fd);
	bench_tap_disable();
	ive_free(&a); ive_free(&b); ive_free(&dx); ive_free(&dy);
	if (IveDestroy) IveDestroy(0);
}

static int want(const char *sel, const char *tok)
{
	return strstr(sel, tok) != NULL;
}

static void *bench_thread_main(void *arg)
{
	MarukoBackendContext *ctx = (MarukoBackendContext *)arg;
	const char *sel = getenv("WAYBEAM_STAB_BENCH");
	if (!sel || !*sel) return NULL;

	/* let the pipeline settle to steady-state fps first */
	for (int i = 0; i < 20 && !g_bench_stop; i++) usleep(100000);
	if (g_bench_stop) return NULL;

	printf("\n[bench] ===== Maruko stab Phase-1 bench (sel=\"%s\") =====\n", sel);
	printf("[bench] src crop base = (%u,%u %ux%u), encode = %ux%u\n",
		ctx->scl_crop_x, ctx->scl_crop_y, ctx->scl_crop_w, ctx->scl_crop_h,
		ctx->cfg.image_width, ctx->cfg.image_height);

	int all = want(sel, "all");
	int preset_tap = all || want(sel, "tap") || (strcmp(sel, "1") == 0);

	if (all || preset_tap || want(sel, "1a")) bench_1a(ctx);
	if (all || preset_tap || want(sel, "1c")) bench_1c(ctx);
	if (all || want(sel, "1b")) bench_1b(ctx);
	if (all || want(sel, "1d")) bench_1d(ctx);

	printf("[bench] ===== Phase-1 bench complete =====\n\n");
	return NULL;
}

void maruko_stab_bench_maybe_start(MarukoBackendContext *ctx)
{
	static int started;
	const char *sel = getenv("WAYBEAM_STAB_BENCH");
	if (!ctx || !sel || !*sel || started) return;
	if (load_syms() != 0) {
		fprintf(stderr, "[bench] symbol load failed — bench disabled\n");
		return;
	}
	started = 1;
	g_bench_stop = 0;
	if (pthread_create(&g_bench_thread, NULL, bench_thread_main, ctx) == 0)
		g_bench_running = 1;
	else
		fprintf(stderr, "[bench] pthread_create failed\n");
}

void maruko_stab_bench_stop(void)
{
	if (!g_bench_running) return;
	g_bench_stop = 1;
	pthread_join(g_bench_thread, NULL);
	g_bench_running = 0;
}
