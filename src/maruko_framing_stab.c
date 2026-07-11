/* maruko_framing_stab.c — video0.framing="stab" for the Maruko (i6c) backend.
 *
 * The parallel of src/star6e_framing_stab.c's HW-crop "stab" path, ported to
 * the SigmaStar Infinity6C SCL pipeline.  Structurally SIMPLER than Star6E: the
 * SCL output-port crop and the detector tap both live in the SAME SCL-input
 * (ISP-output) domain, so there is no VPE precrop domain conversion — the
 * detector measures shake in SCL-input pixels and the emit applies it in the
 * same pixels.
 *
 * Data path (per frame, mirrors the Phase-1 bench_1d prototype, now with the
 * shared Kalman and real geometry):
 *   SCL port 2 (fixed 1:1 384x384 centre tap) --GetBuf--> MI_IVE_Shift_Detector
 *     --> framing_kalman_step --> shift the SCL port-0 crop window (SetPortConfig)
 *   port 0 stays bound to VENC via the ring the whole time (zero-copy).
 *
 * Geometry: src = the AR-precrop base surface (ctx->scl_crop_{x,y,w,h}); the
 * stab window is `pct`% of src (oversample margin = (src-win)/2 per axis),
 * scaled by the SCL to the fixed encode size (ctx->cfg.image_{width,height}).
 * The window's AR equals src's AR (keep_aspect precrop guarantees src AR ==
 * encode AR), so the SCL always does a uniform crop-then-scale — the only
 * stall-free shape on I6C.
 *
 * Teardown ordering is load-bearing (R6): pthread_join the detector BEFORE
 * disabling the tap port, else an in-flight IVE read races the freed ring and
 * storms _MI_SYS_MMU_Callback into a hardware-watchdog reset.  Never SIGKILL.
 */
#include "maruko_framing.h"
#include "maruko_framing_host.h"
#include "maruko_mi.h"          /* g_mi_scl */
#include "maruko_bindings.h"    /* i6c_scl_port */
#include "framing_kalman.h"
#include "framing_stab_accuracy.h"
#include "venc_config.h"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#if HAVE_FRAMING_STAB

/* ── detector geometry ─────────────────────────────────────────────────────
 * The Shift_Detector crop/box/pyramid/search geometry is now the shared,
 * user-selectable `video0.stab_accuracy` level — resolved from the ONE table in
 * framing_stab_accuracy.h so Star6E and Maruko cannot drift.  On Maruko
 * "auto" maps to "low" (256/128/2): the i6c is SINGLE-CORE and the detector is
 * NEON software, so the full "high" config (~17 ms/frame) would peg the one A7
 * core; "low" is ~8 ms/frame (~65% core) and copies less per frame (256x256 vs
 * 384x384 tap), holding every-frame detection at 50 fps with headroom, at a
 * documented slight-noise cost.  A user on a lighter sensor mode can pick
 * "medium"/"high".  g_det is resolved once in prepare(); every geometry site
 * below reads g_det.{crop,box,pyramid,search}. */
static FramingStabDetector g_det = { 256, 128, 2, 64 };  /* Maruko default = "low" */
#define STAB_TAP_PORT     2
#define STAB_DEFAULT_PCT  90   /* fallback framing window when cfg is out of range */

/* ── MI_SYS buffer ABI (i6c; mirrors star6e_framing_stab.c:40-103) ─────────── */
typedef struct { MI_U16 x, y, w, h; } StabRect_t;
typedef struct {
	int eTileMode, ePixelFormat, eCompressMode, eFrameScanMode;
	int eFieldType, ePhylayoutType;
	MI_U16 u16Width, u16Height;
	void *pVirAddr[3];
	MI_U64 phyAddr[3];
	MI_U32 u32Stride[3];
	MI_U32 u32BufSize;
	MI_U16 u16RingStart, u16RingTotal;
	struct { int eType; union { MI_U32 g; } u; } stIsp;
	StabRect_t stCrop;
} StabFrame_t;
typedef struct {
	MI_U64 u64Pts, u64Sideband;
	int eBufType;
	MI_U8 bEos, bUsrBuf;
	MI_U32 u32Seq;
	MI_U8 bDrop;
	union { StabFrame_t stFrameData; MI_U8 pad[512]; };
	MI_U8 u8CusFlag;
} StabBufInfo_t;

/* ── IVE types (device-proven; from tools/ive_i6c_shift.c) ─────────────────── */
#define E_IVE_IMAGE_TYPE_U8C1 0x0
#define E_IVE_IMAGE_TYPE_S8C1 0x1
#define E_IVE_SHIFT_MODE_SINGLE 0x00
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

/* ── vendor fn pointers ────────────────────────────────────────────────────── */
typedef MI_S32 (*fn_getfd)(MI_SYS_ChnPort_t *, MI_S32 *);
typedef MI_S32 (*fn_closefd)(MI_S32);
typedef MI_S32 (*fn_getbuf)(MI_SYS_ChnPort_t *, StabBufInfo_t *, MI_S32 *);
typedef MI_S32 (*fn_putbuf)(MI_S32);
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

static fn_getfd     SysGetFd;
static fn_closefd   SysCloseFd;
static fn_getbuf    SysGetBuf;
static fn_putbuf    SysPutBuf;
static fn_setdepth  SysSetDepth;
static fn_flush     SysFlush;
static fn_mma_alloc MmaAlloc;
static fn_mma_free  MmaFree;
static fn_mmap      SysMmap;
static fn_munmap    SysMunmap;
static fn_ive_create  IveCreate;
static fn_ive_destroy IveDestroy;
static fn_ive_shift   IveShift;
static void *g_sys_lib;
static void *g_ive_lib;

/* ── module state (mirrors the star6e stab globals; MUT_RESTART-scoped) ─────── */
static MarukoBackendContext *g_ctx;   /* borrowed; cleared on stop */
static FramingKalman g_kalman = {
	0.0, 0.0, 0.0, 0.0, 1.0, 1.0,
	FRAMING_KALMAN_Q_DEFAULT, FRAMING_KALMAN_R_DEFAULT
};
static uint32_t g_src_w, g_src_h;     /* AR-precrop base surface (SCL-input px) */
static uint32_t g_win_w, g_win_h;     /* stab framing window (< src, oversample) */
static uint32_t g_scl_crop_x, g_scl_crop_y;  /* base window offset in SCL input */
static uint32_t g_crop_pct;
static uint32_t g_recenter_period;
static double g_cfg_q = FRAMING_KALMAN_Q_DEFAULT;
static double g_cfg_r = FRAMING_KALMAN_R_DEFAULT;
static volatile int g_pan_x_mil = 500;
static volatile int g_pan_y_mil = 500;
static volatile int g_paused;

static pthread_t g_thread;
static volatile int g_running;
static volatile int g_drain_only;   /* teardown phase 1 (both presets): no IVE/
                                     * compose, pure GetBuf/PutBuf until the
                                     * phase-2 join (finish_stop) */
static void drain_user_port(int port);
static volatile int g_tap_active;
static int g_ive_created;
static int g_ive_handle;
static IveImage_t g_a, g_b, g_dx, g_dy;   /* detector ping-pong + result imgs */

/* ── stab-fill state (mirror of star6e g_stab_fill_mode) ──────────────────────
 * Full-FOV floating-on-black: SCL(0,0) port0 outputs the whole base crop at
 * encode dims RAW and UNBOUND; the fill thread drains it, detects on the
 * centre patch, and composes a shifted+black-bordered copy straight into the
 * (NORMAL_FRMBASE, unbound) VENC input port — the same shape as Star6E.
 * Phase 5a's "the i6c VENC cannot be manually pushed" was an ABI artifact
 * (eBufType=0 is BUFDATA_RAW on i6c + a Star6E-shaped BufConf union offset);
 * with the corrected MI_SYS_BufConf_t below the direct push encodes at the
 * full sensor rate.  Set by the stab-fill prepare(), cleared by the plain
 * stab prepare(). */
static volatile int g_fill_mode;
static uint32_t g_enc_w, g_enc_h;      /* encode dims == port0 output dims */
static MI_SYS_ChnPort_t g_fill_venc_in;  /* compose target: the VENC input port */

/* Debug-OSD snapshot — the detector/fill threads publish the last measurement
 * and Kalman correction here; the pipeline's debug-OSD loop renders a "stab"/
 * "sfil" row from it (see maruko_framing_stab_osd_status). */
static volatile int g_osd_acc_x, g_osd_acc_y;
static volatile int g_osd_meas_x, g_osd_meas_y;

/* i6c MI_SYS_BufConf_t ABI (mi_sys_datatype.h:503 — VERIFIED against the SDK
 * headers, NOT the Star6E layout: i6c inserts bDirectBuf+bCrcCheck before the
 * union, its BufFrameConfig has NO embedded ExtraConf, and the union holds
 * MI_PHY (u64) members so it sits 8-aligned at offset 24 on ARM32.  Getting
 * this wrong is silent: GetBuf returns 0 with a zeroed degenerate buffer (the
 * SDK reads w=0/h=0) — the exact first-bring-up failure. */
typedef struct {                               /* == MI_SYS_BufFrameConfig_t */
	MI_U16 u16Width, u16Height;
	int eFrameScanMode;
	int eFormat;
	int eCompressMode;
} StabFillFrameCfg_t;
typedef struct {                               /* == MI_SYS_BufConf_t */
	int eBufType;                              /* offset 0 */
	MI_U32 u32Flags;                           /* offset 4 */
	MI_U64 u64TargetPts;                       /* offset 8 */
	MI_U8 bDirectBuf;                          /* offset 16 */
	MI_U8 bCrcCheck;                           /* offset 17 (pad → 24) */
	union {                                    /* offset 24, align 8 */
		StabFillFrameCfg_t stFrameCfg;
		struct { MI_U32 u32Size; } stRawCfg;
		MI_U64 align8_;                        /* union carries MI_PHY members */
		MI_U8 pad[64];                         /* >= sizeof largest member */
	};
} StabFillBufConf_t;

/* i6c blit/fill/inject symbols (fill mode only; leading u16SocId unlike i6e):
 *   MI_SYS_BufFillPa(soc, FrameData*, u32Val, WindowRect*)
 *   MI_SYS_BufBlitPa(soc, DstFrameData*, DstRect*, SrcFrameData*, SrcRect*) */
typedef MI_S32 (*fn_in_getbuf)(MI_SYS_ChnPort_t *, StabFillBufConf_t *,
	StabBufInfo_t *, MI_S32 *, MI_S32);
typedef MI_S32 (*fn_in_putbuf)(MI_S32, StabBufInfo_t *, MI_U8);
typedef MI_S32 (*fn_blit_pa)(MI_U16, StabFrame_t *, StabRect_t *,
	StabFrame_t *, StabRect_t *);
typedef MI_S32 (*fn_fill_pa)(MI_U16, StabFrame_t *, MI_U32, StabRect_t *);
/* Input-port frame-rate control.  An injected (unbound) input port comes up
 * with 0/0 FRC and the SCL scheduler never runs its queued tasks (proc shows
 * UsrInjectQ_cnt piling with workingTask_cnt=0 until NOBUF) — it must be told
 * its rate with eType=E_MI_SYS_FRC_TYPE_USERINJECT. */
typedef struct {                               /* == MI_SYS_ChnPortFrcAttr_t */
	int eType;                                 /* 1 = USERINJECT */
	int eStrategy;                             /* 0 = BY_RATIO */
	MI_U32 u32SrcFrameRate;
	MI_U32 u32DstFrameRate;
} StabFillFrc_t;
typedef MI_S32 (*fn_in_frc)(MI_SYS_ChnPort_t *, StabFillFrc_t *);
typedef MI_S32 (*fn_get_pts)(MI_U16, MI_U64 *);
static fn_in_getbuf SysInGetBuf;
static fn_in_putbuf SysInPutBuf;
static fn_blit_pa   SysBlitPa;
static fn_fill_pa   SysFillPa;
static fn_in_frc    SysInFrc;
static fn_get_pts   SysGetCurPts;

#define STABFILL_FRC_USERINJECT 1   /* E_MI_SYS_FRC_TYPE_USERINJECT */
#define STABFILL_FRC_BY_RATIO   0   /* E_MI_SYS_FRC_STRATEGY_BY_RATIO */

/* i6c enum: E_MI_SYS_BUFDATA_RAW=0, FRAME=1 (mi_sys_datatype.h:226) — NOT 0
 * as on Star6E.  Type 0 makes GetBuf attempt a giant RAW alloc (u32Size read
 * from the union = width|height<<16) and return a degenerate zero buffer. */
#define STABFILL_BUFDATA_FRAME 1   /* E_MI_SYS_BUFDATA_FRAME */

/* ── small helpers ─────────────────────────────────────────────────────────── */
static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static int pan_clamp_mil(double v)
{
	int mil;
	if (!isfinite(v) || v <= 0.0) return 0;
	if (v >= 1.0) return 1000;
	mil = (int)(v * 1000.0 + 0.5);
	if (mil < 0) mil = 0;
	if (mil > 1000) mil = 1000;
	return mil;
}
static uint32_t stab_pct(const VencConfig *vcfg)
{
	uint32_t p = vcfg->video0.stab_crop_pct;
	if (p < 60 || p > 99) p = STAB_DEFAULT_PCT;
	return p;
}

/* ── emit: shift the SCL port-0 crop window (parallel of
 * star6e_stab_apply_port_crop, but the SCL crop is already SCL-input domain so
 * no precrop scaling is needed). acc is in SCL-input pixels. ───────────────── */
static void maruko_stab_apply_crop(int acc_x, int acc_y)
{
	MarukoBackendContext *ctx = g_ctx;
	int src_w, src_h, win_w, win_h, center_x, center_y, off_x, off_y, mx, my;
	i6c_scl_port p;

	if (!ctx) return;
	src_w = (int)g_src_w; src_h = (int)g_src_h;
	win_w = (int)g_win_w; win_h = (int)g_win_h;
	if (win_w <= 0 || win_h <= 0 || win_w > src_w || win_h > src_h) return;

	center_x = (int)(((int64_t)src_w * g_pan_x_mil) / 1000);
	center_y = (int)(((int64_t)src_h * g_pan_y_mil) / 1000);
	off_x = center_x - win_w / 2 + acc_x;
	off_y = center_y - win_h / 2 + acc_y;
	mx = src_w - win_w; my = src_h - win_h;
	if (off_x < 0) off_x = 0;
	if (off_x > mx) off_x = mx;
	if (off_y < 0) off_y = 0;
	if (off_y > my) off_y = my;
	off_x &= ~1; off_y &= ~1;   /* keep the 2-px-even assumption (1c open) */

	memset(&p, 0, sizeof(p));
	p.crop.x = (unsigned short)((int)g_scl_crop_x + off_x);
	p.crop.y = (unsigned short)((int)g_scl_crop_y + off_y);
	p.crop.width = (unsigned short)win_w;
	p.crop.height = (unsigned short)win_h;
	p.output.width = (unsigned short)ctx->cfg.image_width;
	p.output.height = (unsigned short)ctx->cfg.image_height;
	p.pixFmt = I6_PIXFMT_YUV420SP;
	p.compress = (i6_common_compr)6;   /* IFC — required for the VENC ring */
	(void)g_mi_scl.fnSetPortConfig(0, 0, 0, &p);
}

/* ── vtable: enabled / prepare ─────────────────────────────────────────────── */
static int maruko_stab_enabled(const VencConfig *vcfg)
{
	return strcmp(vcfg->video0.framing, "stab") == 0;
}

/* On Maruko the encode size is fixed (video0.size); stab does NOT resize VENC —
 * it crops a `pct` window of the SCL base and the SCL scales it up to the
 * encode size.  So prepare() leaves enc dims unchanged and just latches the
 * tuning; the real geometry is computed in setup_ports once scl_crop is known. */
static int maruko_stab_prepare(const VencConfig *vcfg, uint32_t src_w,
	uint32_t src_h, uint32_t *enc_w, uint32_t *enc_h)
{
	(void)src_w; (void)src_h;
	/* Resolve the shared detector level once, before the tap / IVE images are
	 * sized in setup_ports.  "auto" -> "low" on single-core Maruko. */
	g_det = framing_stab_detector_params(vcfg->video0.stab_accuracy, "low");
	g_fill_mode = 0;   /* stab-fill prepare opts in (mirror star6e_stab_configure) */
	g_crop_pct = stab_pct(vcfg);
	g_recenter_period = vcfg->video0.stab_recenter_speed;
	g_cfg_q = vcfg->video0.stab_kalman_q;
	g_cfg_r = vcfg->video0.stab_kalman_r;
	g_pan_x_mil = 500;
	g_pan_y_mil = 500;
	g_paused = 0;
	/* enc dims unchanged: the SCL scales the crop window up to the configured
	 * encode size, so VENC stays at video0.width x video0.height. */
	if (enc_w) *enc_w = vcfg->video0.width;
	if (enc_h) *enc_h = vcfg->video0.height;
	return 0;
}

/* ── symbol loading (libmi_sys already mapped by the main process) ─────────── */
static int load_syms(void)
{
	if (SysGetBuf && IveShift) return 0;
	g_sys_lib = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!g_sys_lib) {
		fprintf(stderr, "[maruko-stab] dlopen libmi_sys: %s\n", dlerror());
		return -1;
	}
	SysGetFd   = (fn_getfd)   dlsym(g_sys_lib, "MI_SYS_GetFd");
	SysCloseFd = (fn_closefd) dlsym(g_sys_lib, "MI_SYS_CloseFd");
	SysGetBuf  = (fn_getbuf)  dlsym(g_sys_lib, "MI_SYS_ChnOutputPortGetBuf");
	SysPutBuf  = (fn_putbuf)  dlsym(g_sys_lib, "MI_SYS_ChnOutputPortPutBuf");
	SysSetDepth = (fn_setdepth) dlsym(g_sys_lib, "MI_SYS_SetChnOutputPortDepth");
	SysFlush   = (fn_flush)   dlsym(g_sys_lib, "MI_SYS_FlushInvCache");
	MmaAlloc   = (fn_mma_alloc) dlsym(g_sys_lib, "MI_SYS_MMA_Alloc");
	MmaFree    = (fn_mma_free)  dlsym(g_sys_lib, "MI_SYS_MMA_Free");
	SysMmap    = (fn_mmap)    dlsym(g_sys_lib, "MI_SYS_Mmap");
	SysMunmap  = (fn_munmap)  dlsym(g_sys_lib, "MI_SYS_Munmap");
	if (!SysGetBuf || !SysPutBuf || !SysFlush || !SysSetDepth ||
	    !MmaAlloc || !SysMmap) {
		fprintf(stderr, "[maruko-stab] missing MI_SYS symbols\n");
		return -1;
	}
	g_ive_lib = dlopen("libmi_ive.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!g_ive_lib) {
		fprintf(stderr, "[maruko-stab] dlopen libmi_ive: %s\n", dlerror());
		return -1;
	}
	IveCreate  = (fn_ive_create)  dlsym(g_ive_lib, "MI_IVE_Create");
	IveDestroy = (fn_ive_destroy) dlsym(g_ive_lib, "MI_IVE_Destroy");
	IveShift   = (fn_ive_shift)   dlsym(g_ive_lib, "MI_IVE_Shift_Detector");
	if (!IveCreate || !IveDestroy || !IveShift) {
		fprintf(stderr, "[maruko-stab] missing MI_IVE symbols\n");
		return -1;
	}
	return 0;
}

static int ive_alloc(IveImage_t *img, MI_U16 w, MI_U16 h, int type)
{
	MI_U32 stride = (w + 63u) & ~63u;
	MI_U32 size = stride * h;
	MI_U64 phy = 0; void *vir = NULL;
	memset(img, 0, sizeof(*img));
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

/* ── the detector tap: fixed 1:1 384x384 window at the centre of the base
 * surface (compress=0 so the Y plane is CPU-readable).  1:1 means the shift the
 * detector measures is already in SCL-input pixels — no scaling to the emit. ─ */
static int stab_tap_enable(void)
{
	i6c_scl_port p;
	MI_SYS_ChnPort_t bind;
	int cx, cy;
	MI_S32 ret;

	cx = ((int)g_src_w - g_det.crop) / 2;
	cy = ((int)g_src_h - g_det.crop) / 2;
	if (cx < 0) cx = 0;
	if (cy < 0) cy = 0;
	cx &= ~1; cy &= ~1;

	memset(&p, 0, sizeof(p));
	p.crop.x = (unsigned short)((int)g_scl_crop_x + cx);
	p.crop.y = (unsigned short)((int)g_scl_crop_y + cy);
	p.crop.width = g_det.crop;
	p.crop.height = g_det.crop;
	p.output.width = g_det.crop;
	p.output.height = g_det.crop;
	p.pixFmt = I6_PIXFMT_YUV420SP;
	p.compress = (i6_common_compr)0;   /* RAW — CPU reads the Y plane */
	ret = g_mi_scl.fnSetPortConfig(0, 0, STAB_TAP_PORT, &p);
	if (ret != 0) {
		fprintf(stderr, "[maruko-stab] tap SetPortConfig -> %d\n", (int)ret);
		return -1;
	}
	ret = g_mi_scl.fnEnablePort(0, 0, STAB_TAP_PORT);
	if (ret != 0) {
		fprintf(stderr, "[maruko-stab] tap EnablePort -> %d\n", (int)ret);
		return -1;
	}
	/* Unbound out-port needs a user-frame queue or GetBuf sees 0 frames
	 * (Phase-1 1a finding). */
	memset(&bind, 0, sizeof(bind));
	bind.module = I6_SYS_MOD_SCL; bind.port = STAB_TAP_PORT;
	ret = SysSetDepth(0, &bind, 2, 4);
	if (ret != 0)
		fprintf(stderr, "[maruko-stab] tap SetChnOutputPortDepth -> %d\n",
			(int)ret);
	return 0;
}
static void stab_tap_disable(void)
{
	(void)g_mi_scl.fnDisablePort(0, 0, STAB_TAP_PORT);
}

/* ── stab-fill manual-compose helpers (port of star6e_framing_stab.c:625-959;
 * the i6c ABI differences: leading-SocId blit/fill signatures, the i6c
 * MI_SYS_BufConf_t layout, and BUFDATA_FRAME=1) ───────────────────────────── */

static int load_fill_syms(void)
{
	if (SysInGetBuf && SysBlitPa)
		return 0;
	if (load_syms() != 0 || !g_sys_lib)
		return -1;
	SysInGetBuf = (fn_in_getbuf)dlsym(g_sys_lib, "MI_SYS_ChnInputPortGetBuf");
	SysInPutBuf = (fn_in_putbuf)dlsym(g_sys_lib, "MI_SYS_ChnInputPortPutBuf");
	SysBlitPa   = (fn_blit_pa)  dlsym(g_sys_lib, "MI_SYS_BufBlitPa");
	SysFillPa   = (fn_fill_pa)  dlsym(g_sys_lib, "MI_SYS_BufFillPa");
	SysInFrc    = (fn_in_frc)   dlsym(g_sys_lib, "MI_SYS_SetChnInputPortFrc");
	SysGetCurPts = (fn_get_pts) dlsym(g_sys_lib, "MI_SYS_GetCurPts");
	if (!SysInGetBuf || !SysInPutBuf || !SysBlitPa || !SysFillPa) {
		fprintf(stderr, "[maruko-stab] fill: missing MI_SYS compose symbols "
			"(inget=%p input=%p blit=%p fill=%p)\n", (void *)SysInGetBuf,
			(void *)SysInPutBuf, (void *)SysBlitPa, (void *)SysFillPa);
		return -1;
	}
	return 0;
}

static MI_U64 fill_uv_pa(const StabFrame_t *f, uint32_t h)
{
	if (f->phyAddr[1])
		return f->phyAddr[1];
	return f->phyAddr[0] + (MI_U64)f->u32Stride[0] * h;
}
static void *fill_uv_va(const StabFrame_t *f, uint32_t h)
{
	if (f->pVirAddr[1])
		return f->pVirAddr[1];
	if (!f->pVirAddr[0])
		return NULL;
	return (void *)((uint8_t *)f->pVirAddr[0] + (size_t)f->u32Stride[0] * h);
}

/* Build an I8 (mono-plane) descriptor so fill/blit treat Y and UV separately. */
static void fill_make_i8_plane(StabFrame_t *out, MI_U64 phy, void *vir,
	int width, int height, int stride)
{
	memset(out, 0, sizeof(*out));
	out->ePixelFormat = (int)I6_PIXFMT_I8;
	out->u16Width = (MI_U16)width;
	out->u16Height = (MI_U16)height;
	out->phyAddr[0] = phy;
	out->pVirAddr[0] = vir;
	out->u32Stride[0] = (MI_U32)stride;
}

/* Clipped, even-aligned BufFillPa.  CRITICAL (star6e finding): BufFillPa
 * writes 32-bit words — replicate the byte across all four lanes or the plane
 * stripes (Y=16 raw → green bars). */
static int fill_i8_rect(StabFrame_t *plane, int x, int y, int w, int h,
	MI_U32 value)
{
	StabRect_t r;
	MI_U32 v, fill32;
	MI_S32 ret;

	if (!plane || !plane->phyAddr[0])
		return -1;
	if (w <= 0 || h <= 0)
		return 0;
	x &= ~1;
	w &= ~1;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > plane->u16Width)
		w = plane->u16Width - x;
	if (y + h > plane->u16Height)
		h = plane->u16Height - y;
	w &= ~1;
	if (w <= 0 || h <= 0)
		return 0;

	memset(&r, 0, sizeof(r));
	r.x = (MI_U16)x;
	r.y = (MI_U16)y;
	r.w = (MI_U16)w;
	r.h = (MI_U16)h;
	v = value & 0xffu;
	fill32 = v | (v << 8) | (v << 16) | (v << 24);
	ret = SysFillPa(0, plane, fill32, &r);
	if (ret != 0) {
		static int warned;
		if (!warned) {
			warned = 1;
			fprintf(stderr, "[maruko-stab] fill: MI_SYS_BufFillPa ret=0x%x "
				"rect=%d,%d %dx%d val=%u\n", (unsigned)ret, x, y, w, h,
				(unsigned)value);
		}
		return ret;
	}
	return 0;
}

/* Clipped, even-aligned BufBlitPa of an I8 rect from src to dst. */
static int fill_blit_i8_rect(StabFrame_t *dst, int dst_x, int dst_y,
	StabFrame_t *src, int src_x, int src_y, int w, int h)
{
	StabRect_t sr, dr;
	MI_S32 ret;

	if (!dst || !src || !dst->phyAddr[0] || !src->phyAddr[0])
		return -1;
	if (w <= 0 || h <= 0)
		return 0;
	if (src_x < 0) { int d = -src_x; src_x = 0; dst_x += d; w -= d; }
	if (src_y < 0) { int d = -src_y; src_y = 0; dst_y += d; h -= d; }
	if (dst_x < 0) { int d = -dst_x; dst_x = 0; src_x += d; w -= d; }
	if (dst_y < 0) { int d = -dst_y; dst_y = 0; src_y += d; h -= d; }
	/* Even-align AFTER the negative clips — an odd clip delta must not leave
	 * odd (SDK-rejected) coordinates.  The 1px content shift this can cost is
	 * confined to the defensive out-of-bounds path (unreachable from the
	 * clamped compose caller). */
	dst_x &= ~1;
	src_x &= ~1;
	w &= ~1;
	if (src_x + w > src->u16Width)
		w = src->u16Width - src_x;
	if (dst_x + w > dst->u16Width)
		w = dst->u16Width - dst_x;
	if (src_y + h > src->u16Height)
		h = src->u16Height - src_y;
	if (dst_y + h > dst->u16Height)
		h = dst->u16Height - dst_y;
	w &= ~1;
	if (w <= 0 || h <= 0)
		return 0;

	memset(&sr, 0, sizeof(sr));
	memset(&dr, 0, sizeof(dr));
	sr.x = (MI_U16)src_x; sr.y = (MI_U16)src_y;
	sr.w = (MI_U16)w;     sr.h = (MI_U16)h;
	dr.x = (MI_U16)dst_x; dr.y = (MI_U16)dst_y;
	dr.w = (MI_U16)w;     dr.h = (MI_U16)h;
	ret = SysBlitPa(0, dst, &dr, src, &sr);
	if (ret != 0) {
		static int warned;
		if (!warned) {
			warned = 1;
			fprintf(stderr, "[maruko-stab] fill: MI_SYS_BufBlitPa ret=0x%x "
				"src=%d,%d %dx%d dst=%d,%d\n", (unsigned)ret,
				src_x, src_y, w, h, dst_x, dst_y);
		}
		return ret;
	}
	return 0;
}

/* Max per-axis float (encode-pixel domain): the black-border budget set by
 * stab_crop_pct — enc * (100-pct)/200 per side (pct=90 → 5% per side). */
static int fill_max_off_x(void)
{
	return (int)((g_enc_w * (100u - g_crop_pct)) / 200u);
}
static int fill_max_off_y(void)
{
	return (int)((g_enc_h * (100u - g_crop_pct)) / 200u);
}

/* PTS in the MI timebase (MI_SYS_GetCurPts) — the SCL FRC/scheduler compares
 * injected-frame PTS against it, so a wall-clock PTS can park frames forever.
 * Falls back to CLOCK_MONOTONIC µs if the symbol is absent. */
static uint64_t fill_pts_us(void)
{
	struct timespec ts;
	if (SysGetCurPts) {
		MI_U64 pts = 0;
		if (SysGetCurPts(0, &pts) == 0)
			return (uint64_t)pts;
	}
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* stab-fill compose: GetBuf a fresh VENC-input frame, BufFillPa the four
 * exposed black strips (Y=16, UV=128), BufBlitPa the in-bounds source content
 * at the shifted sub-rect, PutBuf (→ VENC encodes it).  src dim ==
 * dst dim == encode dim; off_x/off_y are the accumulated shake compensation in
 * encode pixels.  Port of star6e_stab_send_frame_to_venc_fill. */
static int fill_compose_to_venc(const StabBufInfo_t *src_buf,
	int off_x, int off_y)
{
	MI_SYS_ChnPort_t target;
	StabFillBufConf_t conf;
	StabBufInfo_t dst_buf;
	StabFrame_t *dst;
	const StabFrame_t *src;
	StabFrame_t src_y_plane, dst_y_plane, src_uv_plane, dst_uv_plane;
	MI_S32 handle = 0;
	int src_w, src_h, dst_w, dst_h;
	int src_x, src_y, dst_x, dst_y, copy_w, copy_h;
	int uv_dst_y, uv_copy_h, uv_h;
	MI_U64 src_uv, dst_uv;
	MI_S32 ret;

	src = &src_buf->stFrameData;
	src_w = src->u16Width ? src->u16Width : (int)g_enc_w;
	src_h = src->u16Height ? src->u16Height : (int)g_enc_h;
	dst_w = (int)g_enc_w;
	dst_h = (int)g_enc_h;
	if (dst_w != src_w || dst_h != src_h) {
		static int warned;
		if (!warned) {
			warned = 1;
			fprintf(stderr, "[maruko-stab] fill: size mismatch src=%dx%d "
				"dst=%dx%d\n", src_w, src_h, dst_w, dst_h);
		}
		return -1;
	}

	target = g_fill_venc_in;

	memset(&conf, 0, sizeof(conf));
	conf.eBufType = STABFILL_BUFDATA_FRAME;
	conf.u64TargetPts = fill_pts_us();
	conf.stFrameCfg.u16Width = (MI_U16)dst_w;
	conf.stFrameCfg.u16Height = (MI_U16)dst_h;
	conf.stFrameCfg.eFormat = I6_PIXFMT_YUV420SP;
	conf.stFrameCfg.eFrameScanMode = 0;   /* PROGRESSIVE */
	memset(&dst_buf, 0, sizeof(dst_buf));
	ret = SysInGetBuf(&target, &conf, &dst_buf, &handle, 20);
	if (ret != 0) {
		static int warned;
		if (!warned) {
			warned = 1;
			fprintf(stderr, "[maruko-stab] fill: VENC-input ChnInputPortGetBuf "
				"ret=%d (0x%x)\n", (int)ret, (unsigned)ret);
		}
		return ret;
	}
	dst = &dst_buf.stFrameData;
	/* SDK inject discipline: the pushed BufInfo carries the PTS explicitly. */
	dst_buf.u64Pts = conf.u64TargetPts;

	{
		static int dumped;
		if (!dumped) {
			dumped = 1;
			fprintf(stderr, "[maruko-stab] fill: first compose: src "
				"%ux%u str=%u/%u phy=%llx/%llx vir=%p  dst %ux%u "
				"str=%u/%u phy=%llx/%llx vir=%p fmt=%d\n",
				src->u16Width, src->u16Height, src->u32Stride[0],
				src->u32Stride[1], (unsigned long long)src->phyAddr[0],
				(unsigned long long)src->phyAddr[1], src->pVirAddr[0],
				dst->u16Width, dst->u16Height, dst->u32Stride[0],
				dst->u32Stride[1], (unsigned long long)dst->phyAddr[0],
				(unsigned long long)dst->phyAddr[1], dst->pVirAddr[0],
				dst->ePixelFormat);
		}
	}

	/* NV12 needs even offsets; clamp to the border budget. */
	off_x &= ~1;
	off_y &= ~1;
	{
		int max_x = fill_max_off_x(), max_y = fill_max_off_y();
		if (off_x < -max_x) off_x = -max_x;
		if (off_x >  max_x) off_x =  max_x;
		if (off_y < -max_y) off_y = -max_y;
		if (off_y >  max_y) off_y =  max_y;
	}
	if (off_x >= 0) { src_x = off_x; dst_x = 0; copy_w = src_w - off_x; }
	else            { src_x = 0; dst_x = -off_x; copy_w = src_w + off_x; }
	if (off_y >= 0) { src_y = off_y; dst_y = 0; copy_h = src_h - off_y; }
	else            { src_y = 0; dst_y = -off_y; copy_h = src_h + off_y; }
	src_x &= ~1; src_y &= ~1;
	dst_x &= ~1; dst_y &= ~1;
	copy_w &= ~1; copy_h &= ~1;
	if (copy_w <= 0 || copy_h <= 0) {
		SysInPutBuf(handle, &dst_buf, 1);
		return -1;
	}

	src_uv = fill_uv_pa(src, (uint32_t)src_h);
	dst_uv = fill_uv_pa(dst, (uint32_t)dst_h);
	if (!src_uv || !dst_uv) {
		static int warned;
		if (!warned) {
			warned = 1;
			fprintf(stderr, "[maruko-stab] fill: NULL uv pa "
				"(src=%llx dst=%llx)\n", (unsigned long long)src_uv,
				(unsigned long long)dst_uv);
		}
		SysInPutBuf(handle, &dst_buf, 1);
		return -1;
	}
	fill_make_i8_plane(&src_y_plane, src->phyAddr[0], src->pVirAddr[0],
		src_w, src_h, (int)src->u32Stride[0]);
	fill_make_i8_plane(&dst_y_plane, dst->phyAddr[0], dst->pVirAddr[0],
		dst_w, dst_h, (int)dst->u32Stride[0]);
	fill_make_i8_plane(&src_uv_plane, src_uv, fill_uv_va(src, (uint32_t)src_h),
		src_w, src_h / 2, (int)(src->u32Stride[1] ?
			src->u32Stride[1] : src->u32Stride[0]));
	fill_make_i8_plane(&dst_uv_plane, dst_uv, fill_uv_va(dst, (uint32_t)dst_h),
		dst_w, dst_h / 2, (int)(dst->u32Stride[1] ?
			dst->u32Stride[1] : dst->u32Stride[0]));

	/* Black strips around the shifted content (Y=16, UV=128). */
	uv_dst_y  = dst_y / 2;
	uv_copy_h = copy_h / 2;
	uv_h      = dst_h / 2;
	if (dst_x > 0) {
		fill_i8_rect(&dst_y_plane, 0, 0, dst_x, dst_h, 16);
		fill_i8_rect(&dst_uv_plane, 0, 0, dst_x, uv_h, 128);
	}
	if (dst_x + copy_w < dst_w) {
		fill_i8_rect(&dst_y_plane, dst_x + copy_w, 0,
			dst_w - (dst_x + copy_w), dst_h, 16);
		fill_i8_rect(&dst_uv_plane, dst_x + copy_w, 0,
			dst_w - (dst_x + copy_w), uv_h, 128);
	}
	if (dst_y > 0) {
		fill_i8_rect(&dst_y_plane, dst_x, 0, copy_w, dst_y, 16);
		fill_i8_rect(&dst_uv_plane, dst_x, 0, copy_w, uv_dst_y, 128);
	}
	if (dst_y + copy_h < dst_h) {
		fill_i8_rect(&dst_y_plane, dst_x, dst_y + copy_h,
			copy_w, dst_h - (dst_y + copy_h), 16);
		fill_i8_rect(&dst_uv_plane, dst_x, uv_dst_y + uv_copy_h,
			copy_w, uv_h - (uv_dst_y + uv_copy_h), 128);
	}

	/* Copy the in-bounds Y rect, then the UV rect (Y pos/height halved). */
	ret = fill_blit_i8_rect(&dst_y_plane, dst_x, dst_y,
		&src_y_plane, src_x, src_y, copy_w, copy_h);
	if (ret != 0)
		goto err;
	ret = fill_blit_i8_rect(&dst_uv_plane, dst_x, dst_y / 2,
		&src_uv_plane, src_x, src_y / 2, copy_w, copy_h / 2);
	if (ret != 0)
		goto err;

	ret = SysInPutBuf(handle, &dst_buf, 0);
	if (ret != 0) {
		static int warned;
		if (!warned) {
			warned = 1;
			fprintf(stderr, "[maruko-stab] fill: VENC-input ChnInputPortPutBuf "
				"ret=%d (0x%x)\n", (int)ret, (unsigned)ret);
		}
		/* Drop-release so a transient submit failure can't bleed the input
		 * pool dry (a leaked handle per failure would wedge GetBuf at
		 * NOBUF).  If the failed submit already consumed the handle, the
		 * drop call fails harmlessly on the handle lookup. */
		SysInPutBuf(handle, &dst_buf, 1);
	}
	return ret;

err:
	SysInPutBuf(handle, &dst_buf, 1);
	return ret;
}

/* Copy the centre Y crop of a drained port0 frame into an IVE image (clamped,
 * 16/2-aligned — port of star6e_stab_copy_y_to_sw). */
static int fill_copy_y_to_ive(IveImage_t *dst, const StabBufInfo_t *src_buf)
{
	int src_w, src_h, stride, crop_w, crop_h, crop_x, crop_y, row;
	const uint8_t *src_p;
	uint8_t *dst_p;

	if (!dst || !dst->apu8VirAddr[0] || !src_buf->stFrameData.pVirAddr[0])
		return -1;
	src_w = (int)src_buf->stFrameData.u16Width;
	src_h = (int)src_buf->stFrameData.u16Height;
	stride = (int)src_buf->stFrameData.u32Stride[0];
	crop_w = g_det.crop;
	crop_h = g_det.crop;
	if (crop_w > src_w) crop_w = src_w;
	if (crop_h > src_h) crop_h = src_h;
	crop_w &= ~15;
	crop_h &= ~1;
	crop_x = ((src_w - crop_w) / 2) & ~15;
	crop_y = ((src_h - crop_h) / 2) & ~1;
	if (crop_x < 0) crop_x = 0;
	if (crop_y < 0) crop_y = 0;

	src_p = (const uint8_t *)src_buf->stFrameData.pVirAddr[0] +
		(size_t)crop_y * stride + crop_x;
	dst_p = dst->apu8VirAddr[0];
	for (row = 0; row < crop_h; row++) {
		memcpy(dst_p, src_p, (size_t)crop_w);
		src_p += stride;
		dst_p += dst->azu16Stride[0];
	}
	SysFlush(dst->apu8VirAddr[0],
		(MI_U32)dst->azu16Stride[0] * (MI_U32)crop_h);
	return 0;
}

/* ── stab-fill vtable: prepare / setup_ports / drain thread ────────────────── */

static int maruko_stab_fill_enabled(const VencConfig *vcfg)
{
	return strcmp(vcfg->video0.framing, "stab-fill") == 0;
}

/* Encode dims unchanged (video0.size): port0 SCL-downscales the FULL base crop
 * to the encode size; stab_crop_pct bounds the float/black-border budget
 * rather than shrinking the output (mirror star6e_stab_fill_prepare). */
static int maruko_stab_fill_prepare(const VencConfig *vcfg, uint32_t src_w,
	uint32_t src_h, uint32_t *enc_w, uint32_t *enc_h)
{
	(void)src_w; (void)src_h;
	g_det = framing_stab_detector_params(vcfg->video0.stab_accuracy, "low");
	g_crop_pct = stab_pct(vcfg);
	g_recenter_period = vcfg->video0.stab_recenter_speed;
	g_cfg_q = vcfg->video0.stab_kalman_q;
	g_cfg_r = vcfg->video0.stab_kalman_r;
	g_pan_x_mil = 500;
	g_pan_y_mil = 500;
	g_paused = 0;
	g_fill_mode = 1;
	if (enc_w) *enc_w = vcfg->video0.width;
	if (enc_h) *enc_h = vcfg->video0.height;
	return 0;
}

/* Fill setup: NO detector tap (the detector reads the centre patch of the full
 * port0 frame) and NO port-0 crop writes.  Port0 was configured RAW + left
 * unbound by the pipeline (cfg.framing=="stab-fill"); give it a user-frame
 * queue and verify the graph is in manual-feed shape. */
static int maruko_stab_fill_setup_ports(MarukoBackendContext *ctx,
	uint32_t src_fps, uint32_t dst_fps)
{
	MI_SYS_ChnPort_t port0;
	MI_S32 ret;

	(void)src_fps; (void)dst_fps;
	if (!ctx)
		return -1;
	if (load_fill_syms() != 0)
		return -1;
	if (!maruko_framing_fill_graph_ready()) {
		fprintf(stderr, "[maruko-stab] fill: graph not configured for "
			"stab-fill — refusing to push into a RING-fed VENC\n");
		return -1;
	}
	g_ctx = ctx;
	/* Compose target: the (unbound, NORMAL_FRMBASE) VENC input port. */
	memset(&g_fill_venc_in, 0, sizeof(g_fill_venc_in));
	g_fill_venc_in.module  = I6_SYS_MOD_VENC;
	g_fill_venc_in.device  = (unsigned)ctx->venc_device;
	g_fill_venc_in.channel = (unsigned)ctx->venc_channel;
	g_fill_venc_in.port    = 0;
	g_enc_w = ctx->cfg.image_width;
	g_enc_h = ctx->cfg.image_height;
	/* AE metering + max-off reference: the whole encode frame. */
	g_src_w = g_enc_w;
	g_src_h = g_enc_h;
	g_win_w = g_enc_w;
	g_win_h = g_enc_h;
	if (g_enc_w < (uint32_t)g_det.crop + 2 ||
	    g_enc_h < (uint32_t)g_det.crop + 2) {
		fprintf(stderr, "[maruko-stab] fill: encode %ux%u too small for "
			"%dx%d detect crop\n", g_enc_w, g_enc_h, g_det.crop, g_det.crop);
		g_ctx = NULL;
		return -1;
	}
	g_tap_active = 0;   /* no tap port in fill mode */

	/* R2 still applies: nothing else may write the SCL port-0 config. */
	maruko_framing_pan_ramp_stop();

	/* Unbound port0 needs a user-frame queue or GetBuf sees 0 frames
	 * (Phase-1 1a finding; star6e fill uses (4,8)). */
	memset(&port0, 0, sizeof(port0));
	port0.module = I6_SYS_MOD_SCL;
	port0.port = 0;
	ret = SysSetDepth(0, &port0, 4, 8);
	if (ret != 0) {
		fprintf(stderr, "[maruko-stab] fill: port0 SetChnOutputPortDepth "
			"-> %d\n", (int)ret);
		g_ctx = NULL;
		return -1;
	}

	/* Tell the scheduler the VENC input port's frame rate — an injected
	 * (unbound) input port defaults to 0/0 FRC, and without USERINJECT the
	 * queued frames can park until NOBUF wedges the feed.  Hard setup error
	 * if the symbol is missing or the call fails: a silently-wedging feed
	 * is worse than a visible bring-up failure. */
	if (!SysInFrc) {
		fprintf(stderr, "[maruko-stab] fill: MI_SYS_SetChnInputPortFrc "
			"missing — injected frames would never schedule\n");
		g_ctx = NULL;
		return -1;
	}
	{
		StabFillFrc_t frc = {
			.eType = STABFILL_FRC_USERINJECT,
			.eStrategy = STABFILL_FRC_BY_RATIO,
			.u32SrcFrameRate = ctx->sensor.fps,
			.u32DstFrameRate = ctx->sensor.fps,
		};
		ret = SysInFrc(&g_fill_venc_in, &frc);
		fprintf(stderr, "[maruko-stab] fill: VENC-input SetChnInputPortFrc"
			"(USERINJECT %u/%u) ret=%d\n",
			ctx->sensor.fps, ctx->sensor.fps, (int)ret);
		if (ret != 0) {
			g_ctx = NULL;
			return -1;
		}
	}
	framing_kalman_reset(&g_kalman, g_cfg_q, g_cfg_r);
	fprintf(stderr, "[maruko-stab] fill: manual-drain mode (encode %ux%u, "
		"pct=%u, max_off %dx%d, detect crop %dx%d)\n", g_enc_w, g_enc_h,
		g_crop_pct, fill_max_off_x(), fill_max_off_y(),
		g_det.crop, g_det.crop);
	return 0;
}

/* Fill drain thread: drain port0 (full encode-dim RAW frame) → detect on the
 * centre patch → Kalman → compose shifted+bordered copy into the VENC input →
 * release the port0 buffer.  Every drained frame is composed and pushed (the
 * frame-base VENC has no other producer); detection needs a previous frame so
 * the first frame goes out with acc=(0,0).  Per-frame cost is instrumented —
 * the pivotal F0 number on the single A7. */
static void *maruko_stab_fill_thread_main(void *arg)
{
	MI_SYS_ChnPort_t port0;
	MI_S32 fd = -1;
	int have_prev = 0, cur = 0, frames = 0, send_fail = 0;
	int acc_x = 0, acc_y = 0;
	double det_ms = 0.0, comp_ms = 0.0, cpu_ms = 0.0;
	int left = ((g_det.crop - g_det.box) / 2) & ~1;
	int top  = ((g_det.crop - g_det.box) / 2) & ~1;
	IveShiftCtrl_t ctrl = {
		.enMode = E_IVE_SHIFT_MODE_SINGLE,
		.pyramid_level = g_det.pyramid, .search_range = g_det.search,
		.u16Left = (MI_U16)left, .u16Top = (MI_U16)top,
		.u16Width = g_det.box, .u16Height = g_det.box,
	};
	(void)arg;

	memset(&port0, 0, sizeof(port0));
	port0.module = I6_SYS_MOD_SCL;
	port0.port = 0;
	if (SysGetFd) (void)SysGetFd(&port0, &fd);

	while (g_running) {
		StabBufInfo_t buf;
		MI_S32 h = -1;
		IveImage_t *prev = (cur & 1) ? &g_b : &g_a;
		IveImage_t *curr = (cur & 1) ? &g_a : &g_b;
		double t0, t1, t2, c1;
		int copied;

		if (fd >= 0) {
			fd_set rf; struct timeval tv = {0, 100000};
			FD_ZERO(&rf); FD_SET(fd, &rf);
			if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0) continue;
		}
		memset(&buf, 0, sizeof(buf));
		if (SysGetBuf(&port0, &buf, &h) != 0) {
			if (fd < 0) usleep(1000);
			continue;
		}

		/* Teardown phase 1 (drain-only): keep releasing port0 frames so the
		 * SCL/ISP working queues can quiesce while the pipeline disables the
		 * port — but no IVE (buffers may be going away) and no compose (the
		 * VENC is being stopped).  Phase 2 (finish_stop) joins us. */
		if (g_drain_only) {
			SysPutBuf(h);
			continue;
		}

		{
			struct timespec cts;
			clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cts);
			c1 = cts.tv_sec * 1000.0 + cts.tv_nsec / 1e6;
		}
		t0 = now_ms();
		copied = fill_copy_y_to_ive(curr, &buf) == 0;
		if (copied && have_prev) {
			MI_S32 r = IveShift(g_ive_handle, prev, curr, &g_dx, &g_dy,
				&ctrl, 1);
			if (r == 0) {
				double meas_dx, meas_dy;
				uint32_t tau;
				SysFlush(g_dx.apu8VirAddr[0], g_dx.azu16Stride[0]);
				SysFlush(g_dy.apu8VirAddr[0], g_dy.azu16Stride[0]);
				/* STAB_SHIFT_SIGN_{X,Y} = -1 (star6e parity). */
				meas_dx = -(double)((signed char *)g_dx.apu8VirAddr[0])[0];
				meas_dy = -(double)((signed char *)g_dy.apu8VirAddr[0])[0];
				tau = g_recenter_period ? g_recenter_period : 30;
				framing_kalman_step(&g_kalman, meas_dx, meas_dy,
					g_paused, tau, fill_max_off_x(), fill_max_off_y(),
					&acc_x, &acc_y);
				g_osd_meas_x = (int)meas_dx;
				g_osd_meas_y = (int)meas_dy;
				g_osd_acc_x = acc_x;
				g_osd_acc_y = acc_y;
			}
		}
		t1 = now_ms();

		if (fill_compose_to_venc(&buf, acc_x, acc_y) != 0)
			send_fail++;
		t2 = now_ms();

		SysPutBuf(h);
		/* Only advance the ping-pong on a successful copy — else the next
		 * detect would correlate against a stale/uninitialized prev image
		 * and inject a bogus shift sample into the Kalman. */
		if (copied) {
			have_prev = 1;
			cur++;
		}

		det_ms += (t1 - t0);
		comp_ms += (t2 - t1);
		{
			struct timespec cts;
			clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cts);
			cpu_ms += (cts.tv_sec * 1000.0 + cts.tv_nsec / 1e6) - c1;
		}
		if (++frames % 250 == 0) {
			fprintf(stderr, "[maruko-stab] fill: %d frames | detect %.1f ms "
				"| compose %.1f ms | thread CPU %.1f ms/frame | acc=(%d,%d) "
				"send_fail=%d\n", frames, det_ms / 250.0, comp_ms / 250.0,
				cpu_ms / 250.0, acc_x, acc_y, send_fail);
			det_ms = comp_ms = cpu_ms = 0.0;
		}
	}
	if (fd >= 0 && SysCloseFd) SysCloseFd(fd);
	return NULL;
}

/* ── vtable: setup_ports — compute geometry (scl_crop now known), enable tap ─ */
static int maruko_stab_setup_ports(MarukoBackendContext *ctx,
	uint32_t src_fps, uint32_t dst_fps)
{
	(void)src_fps; (void)dst_fps;
	if (!ctx) return -1;
	/* Resolve MI_SYS/IVE symbols before the tap setup — stab_tap_enable needs
	 * SysSetDepth, and the detector thread needs the rest. Idempotent. */
	if (load_syms() != 0)
		return -1;
	g_ctx = ctx;
	g_scl_crop_x = ctx->scl_crop_x;
	g_scl_crop_y = ctx->scl_crop_y;
	g_src_w = ctx->scl_crop_w;
	g_src_h = ctx->scl_crop_h;
	if (g_src_w == 0 || g_src_h == 0) {
		fprintf(stderr, "[maruko-stab] no scl_crop base — stab off\n");
		g_ctx = NULL;
		return -1;
	}
	if (g_src_w < (uint32_t)g_det.crop + 2 || g_src_h < (uint32_t)g_det.crop + 2) {
		fprintf(stderr, "[maruko-stab] base %ux%u too small for %dx%d tap\n",
			g_src_w, g_src_h, g_det.crop, g_det.crop);
		g_ctx = NULL;
		return -1;
	}
	/* Stab framing window: pct of the base, 2-aligned, AR == base AR (== encode
	 * AR under keep_aspect), so the SCL always crop-then-uniform-scales. */
	g_win_w = ((g_src_w * g_crop_pct) / 100u) & ~1u;
	g_win_h = ((g_src_h * g_crop_pct) / 100u) & ~1u;
	if (g_win_w >= g_src_w) g_win_w = (g_src_w - 2u) & ~1u;
	if (g_win_h >= g_src_h) g_win_h = (g_src_h - 2u) & ~1u;

	/* R2: stab is the sole writer of the SCL port-0 crop — stop the pan ramp. */
	maruko_framing_pan_ramp_stop();

	if (stab_tap_enable() != 0) {
		g_tap_active = 0;
		g_ctx = NULL;
		return -1;
	}
	g_tap_active = 1;
	/* Centre the initial stab window (acc=0). */
	framing_kalman_reset(&g_kalman, g_cfg_q, g_cfg_r);
	maruko_stab_apply_crop(0, 0);
	fprintf(stderr, "[maruko-stab] base %ux%u @(%u,%u) win %ux%u (pct=%u) "
		"tap %dx%d\n", g_src_w, g_src_h, g_scl_crop_x, g_scl_crop_y,
		g_win_w, g_win_h, g_crop_pct, g_det.crop, g_det.crop);
	return 0;
}

/* ── detector thread ───────────────────────────────────────────────────────── */
static void *maruko_stab_thread_main(void *arg)
{
	MI_SYS_ChnPort_t bind;
	MI_S32 fd = -1;
	int have_prev = 0, cur = 0, dbg = 0;
	int max_x = (int)(g_src_w - g_win_w) / 2;
	int max_y = (int)(g_src_h - g_win_h) / 2;
	int left = ((g_det.crop - g_det.box) / 2) & ~1;
	int top  = ((g_det.crop - g_det.box) / 2) & ~1;
	IveShiftCtrl_t ctrl = {
		.enMode = E_IVE_SHIFT_MODE_SINGLE,
		.pyramid_level = g_det.pyramid, .search_range = g_det.search,
		.u16Left = (MI_U16)left, .u16Top = (MI_U16)top,
		.u16Width = g_det.box, .u16Height = g_det.box,
	};
	(void)arg;

	memset(&bind, 0, sizeof(bind));
	bind.module = I6_SYS_MOD_SCL; bind.port = STAB_TAP_PORT;
	if (SysGetFd) (void)SysGetFd(&bind, &fd);

	while (g_running) {
		StabBufInfo_t buf;
		MI_S32 h = -1;
		IveImage_t *prev = (cur & 1) ? &g_b : &g_a;
		IveImage_t *curr = (cur & 1) ? &g_a : &g_b;
		MI_U8 *y; MI_U32 stride; int copied = 0;

		if (fd >= 0) {
			fd_set rf; struct timeval tv = {0, 100000};
			FD_ZERO(&rf); FD_SET(fd, &rf);
			if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0) continue;
		}
		memset(&buf, 0, sizeof(buf));
		if (SysGetBuf(&bind, &buf, &h) != 0) {
			if (fd < 0) usleep(1000);
			continue;
		}
		/* Teardown phase 1 (drain-only): keep releasing tap frames so the
		 * SCL/ISP working queues can quiesce — no IVE.  Same hazard as the
		 * fill port0: an unconsumed user port pins the ISP→SCL REALTIME
		 * unbind flush (device-reproduced from plain stab on a size-change
		 * reinit, 2026-07-09). */
		if (g_drain_only) {
			SysPutBuf(h);
			continue;
		}
		y = (MI_U8 *)buf.stFrameData.pVirAddr[0];
		stride = buf.stFrameData.u32Stride[0];
		if (y && stride >= (MI_U32)g_det.crop) {
			for (int r = 0; r < g_det.crop; r++)
				memcpy(curr->apu8VirAddr[0] + (size_t)r * curr->azu16Stride[0],
				       y + (size_t)r * stride, g_det.crop);
			SysFlush(curr->apu8VirAddr[0],
				(MI_U32)curr->azu16Stride[0] * g_det.crop);
			copied = 1;
		}
		SysPutBuf(h);

		/* Skip an un-mapped / short tap frame entirely: correlating a
		 * stale (two-frames-old) or freshly-memset curr against prev
		 * yields a bogus large shift and a one-frame crop jump. Retry
		 * without advancing cur/have_prev (matches the fill thread). */
		if (!copied)
			continue;

		if (!have_prev) { have_prev = 1; cur++; continue; }

		{
			MI_S32 r = IveShift(g_ive_handle, prev, curr, &g_dx, &g_dy, &ctrl, 1);
			if (r == 0) {
				int acc_x = 0, acc_y = 0;
				double meas_dx, meas_dy;
				uint32_t tau;
				SysFlush(g_dx.apu8VirAddr[0], g_dx.azu16Stride[0]);
				SysFlush(g_dy.apu8VirAddr[0], g_dy.azu16Stride[0]);
				/* STAB_SHIFT_SIGN_{X,Y} = -1 (star6e_framing_stab.c). */
				meas_dx = -(double)((signed char *)g_dx.apu8VirAddr[0])[0];
				meas_dy = -(double)((signed char *)g_dy.apu8VirAddr[0])[0];
				tau = g_recenter_period ? g_recenter_period : 30;
				framing_kalman_step(&g_kalman, meas_dx, meas_dy,
					g_paused, tau, max_x, max_y, &acc_x, &acc_y);
				g_osd_meas_x = (int)meas_dx;
				g_osd_meas_y = (int)meas_dy;
				g_osd_acc_x = acc_x;
				g_osd_acc_y = acc_y;
				maruko_stab_apply_crop(acc_x, acc_y);
				if ((++dbg % 120) == 0)
					fprintf(stderr, "[maruko-stab] meas=(%.0f,%.0f) "
						"acc=(%d,%d) max=(%d,%d) pan=(%d,%d) "
						"q=%.4f r=%.2f paused=%d\n",
						meas_dx, meas_dy, acc_x, acc_y, max_x, max_y,
						g_pan_x_mil, g_pan_y_mil, g_kalman.q,
						g_kalman.r, g_paused);
			}
		}
		cur++;
	}
	if (fd >= 0 && SysCloseFd) SysCloseFd(fd);
	return NULL;
}

/* ── vtable: start / stop ──────────────────────────────────────────────────── */
static int maruko_stab_start(void)
{
	/* Fill mode has no tap — the detector reads the drained port0 frame — and
	 * the thread is mandatory (it is the frame-base VENC's only producer). */
	if (!g_fill_mode && !g_tap_active) {
		fprintf(stderr, "[maruko-stab] no tap — detector disabled\n");
		return 0;
	}
	if (load_syms() != 0)
		return -1;
	g_ive_handle = 0;
	if (IveCreate(g_ive_handle) != 0) {
		fprintf(stderr, "[maruko-stab] MI_IVE_Create failed\n");
		return -1;
	}
	g_ive_created = 1;
	if (ive_alloc(&g_a, g_det.crop, g_det.crop, E_IVE_IMAGE_TYPE_U8C1) ||
	    ive_alloc(&g_b, g_det.crop, g_det.crop, E_IVE_IMAGE_TYPE_U8C1) ||
	    ive_alloc(&g_dx, 1, 1, E_IVE_IMAGE_TYPE_S8C1) ||
	    ive_alloc(&g_dy, 1, 1, E_IVE_IMAGE_TYPE_S8C1)) {
		fprintf(stderr, "[maruko-stab] IVE image alloc failed\n");
		ive_free(&g_a); ive_free(&g_b); ive_free(&g_dx); ive_free(&g_dy);
		IveDestroy(g_ive_handle); g_ive_created = 0;
		return -1;
	}
	g_running = 1;
	if (pthread_create(&g_thread, NULL, g_fill_mode ?
	    maruko_stab_fill_thread_main : maruko_stab_thread_main, NULL) != 0) {
		g_running = 0;
		ive_free(&g_a); ive_free(&g_b); ive_free(&g_dx); ive_free(&g_dy);
		IveDestroy(g_ive_handle); g_ive_created = 0;
		fprintf(stderr, "[maruko-stab] thread spawn failed\n");
		return -1;
	}
	if (g_fill_mode)
		fprintf(stderr, "[maruko-stab] fill drain+compose running "
			"(enc %ux%u, max_off %d/%d)\n", g_enc_w, g_enc_h,
			fill_max_off_x(), fill_max_off_y());
	else
		fprintf(stderr, "[maruko-stab] detector running (win %ux%u, max_off "
			"%d/%d)\n", g_win_w, g_win_h, (int)(g_src_w - g_win_w) / 2,
			(int)(g_src_h - g_win_h) / 2);
	return 0;
}

static void maruko_stab_stop(void)
{
	/* Teardown is TWO-PHASE for BOTH presets.  The camera keeps producing
	 * into the user-drained SCL port (fill: port0; stab: the port-2 tap)
	 * until that port is disabled — if the consumer thread is joined first,
	 * the port's in-flight SCL working tasks lose their consumer, which pins
	 * the ISP's REALTIME deliveries, and the ISP→SCL unbind's UNBOUNDED
	 * kernel flush spins forever (MI_SYS_IMPL_FlushRealTimeOutputBuf D-state
	 * → process zombie or hardware-watchdog reset; device-reproduced from
	 * BOTH presets on reinit switches, 2026-07-09).  Phase 1 (here): flip
	 * the thread to drain-only — no IVE, no compose, pure GetBuf/PutBuf — so
	 * consumption continues WHILE the ports are disabled and SCL/ISP
	 * quiesce.  Phase 2 (maruko_framing_stab_finish_stop, called from
	 * teardown Step-0 after DisablePort(0,0,0)): disable the tap (fill has
	 * no tap), join, sweep the residue, free the IVE state. */
	if (g_running) {
		g_drain_only = 1;
		return;
	}

	/* Never-started (setup/start failure): plain cleanup. */
	if (g_tap_active) {
		stab_tap_disable();
		g_tap_active = 0;
	}
	ive_free(&g_a); ive_free(&g_b); ive_free(&g_dx); ive_free(&g_dy);
	if (g_ive_created) {
		IveDestroy(g_ive_handle);
		g_ive_created = 0;
	}
	g_ctx = NULL;
}

/* Teardown phase 2 — called from the pipeline teardown right after port0 is
 * disabled: disable the stab tap (its drain thread is still consuming, and in
 * drain-only mode there is no in-flight IVE read to race — the original R6
 * concern), give the last in-flight frame one period to land, join the
 * thread, sweep any residual FIFO entries, free the IVE state.  Idempotent;
 * no-op unless phase 1 armed. */
void maruko_framing_stab_finish_stop(void)
{
	if (!g_drain_only)
		return;
	if (g_tap_active) {
		stab_tap_disable();
		g_tap_active = 0;
	}
	usleep(30000);   /* one frame period: let final in-flight tasks land */
	if (g_running) {
		g_running = 0;
		pthread_join(g_thread, NULL);
		memset(&g_thread, 0, sizeof(g_thread));
	}
	drain_user_port(g_fill_mode ? 0 : STAB_TAP_PORT);
	ive_free(&g_a); ive_free(&g_b); ive_free(&g_dx); ive_free(&g_dy);
	if (g_ive_created) {
		IveDestroy(g_ive_handle);
		g_ive_created = 0;
	}
	g_drain_only = 0;
	g_ctx = NULL;
}

/* ── vtable: apply_ae_crop / set_pan / active / set_live ───────────────────── */
static void maruko_stab_apply_ae_crop(void)
{
	if (!g_ctx || g_src_w == 0 || g_src_h == 0) return;
	/* pct = window/base fraction; keep the AE meter on the stab crop. */
	maruko_framing_apply_ae_crop(g_ctx,
		(double)g_win_w / (double)g_src_w,
		(double)g_pan_x_mil / 1000.0,
		(double)g_pan_y_mil / 1000.0);
}
static void maruko_stab_set_pan(double x, double y)
{
	g_pan_x_mil = pan_clamp_mil(x);
	g_pan_y_mil = pan_clamp_mil(y);
	maruko_stab_apply_ae_crop();
}
static int maruko_stab_active(void)
{
	return g_running;
}

/* Fill-mode live-fps divider: re-issue the VENC input port's USERINJECT FRC
 * with dst < src so the scheduler drops the excess pushed frames — the
 * frame-base equivalent of the RING bind's src:dst fps trick.  Returns -1
 * when fill isn't running (caller falls back / errors). */
int maruko_framing_stab_fill_set_fps(uint32_t src_fps, uint32_t dst_fps)
{
	StabFillFrc_t frc = {
		.eType = STABFILL_FRC_USERINJECT,
		.eStrategy = STABFILL_FRC_BY_RATIO,
		.u32SrcFrameRate = src_fps,
		.u32DstFrameRate = dst_fps,
	};
	MI_S32 ret;

	if (!g_fill_mode || !g_running || !SysInFrc)
		return -1;
	ret = SysInFrc(&g_fill_venc_in, &frc);
	fprintf(stderr, "[maruko-stab] fill: live fps FRC %u:%u ret=%d\n",
		src_fps, dst_fps, (int)ret);
	return ret == 0 ? 0 : -1;
}

/* Release every frame still queued on a user-drained SCL chn-0 output port
 * (fill: port0; stab: the port-2 tap).  Called with the consumer thread
 * already joined and the port disabled — a residual queue pins SCL/ISP
 * working tasks and wedges the ISP→SCL REALTIME unbind flush in unbounded
 * D-state.  Bounded, idempotent, safe when nothing is queued. */
static void drain_user_port(int port)
{
	MI_SYS_ChnPort_t p;
	int drained = 0, i;

	if (!SysGetBuf || !SysPutBuf)
		return;
	memset(&p, 0, sizeof(p));
	p.module = I6_SYS_MOD_SCL;
	p.port = (unsigned)port;
	for (i = 0; i < 32; i++) {
		StabBufInfo_t buf;
		MI_S32 h = -1;
		memset(&buf, 0, sizeof(buf));
		if (SysGetBuf(&p, &buf, &h) != 0)
			break;
		SysPutBuf(h);
		drained++;
	}
	if (drained)
		fprintf(stderr, "[maruko-stab] drained %d residual frame(s) from "
			"port %d\n", drained, port);
}

/* Debug-OSD snapshot: last detector measurement + Kalman correction (both in
 * stab pixels — SCL-input px for "stab", encode px for "stab-fill").  Returns
 * 0 when no stab thread is running (row hidden).  Called from the pipeline's
 * 1 Hz debug-OSD refresh (see maruko_framing.h). */
int maruko_framing_stab_osd_status(int *acc_x, int *acc_y,
	int *meas_x, int *meas_y, int *paused, int *fill)
{
	if (!g_running)
		return 0;
	if (acc_x)  *acc_x = g_osd_acc_x;
	if (acc_y)  *acc_y = g_osd_acc_y;
	if (meas_x) *meas_x = g_osd_meas_x;
	if (meas_y) *meas_y = g_osd_meas_y;
	if (paused) *paused = g_paused;
	if (fill)   *fill = g_fill_mode;
	return 1;
}
static int maruko_stab_set_live(const char *key, const char *val)
{
	if (!key) return -1;
	if (strcmp(key, "pause") == 0 || strcmp(key, "pauseStab") == 0 ||
	    strcmp(key, "video0.pause_stab") == 0) {
		g_paused = val && (val[0] == '1' || val[0] == 't' || val[0] == 'T' ||
			val[0] == 'y' || val[0] == 'Y');
		return 0;
	}
	return -1;
}

const MarukoFramingModule maruko_framing_stab = {
	.preset_name   = "stab",
	.enabled       = maruko_stab_enabled,
	.prepare       = maruko_stab_prepare,
	.setup_ports   = maruko_stab_setup_ports,
	.start         = maruko_stab_start,
	.stop          = maruko_stab_stop,
	.apply_ae_crop = maruko_stab_apply_ae_crop,
	.set_pan       = maruko_stab_set_pan,
	.active        = maruko_stab_active,
	.set_live      = maruko_stab_set_live,
};

/* "stab-fill" shares start/stop/set_live with "stab" (they branch on
 * g_fill_mode); prepare/setup_ports are fill-specific.  apply_ae_crop is
 * shared too — fill sets win==src so the AE meter covers the full frame. */
const MarukoFramingModule maruko_framing_stab_fill = {
	.preset_name   = "stab-fill",
	.enabled       = maruko_stab_fill_enabled,
	.prepare       = maruko_stab_fill_prepare,
	.setup_ports   = maruko_stab_fill_setup_ports,
	.start         = maruko_stab_start,
	.stop          = maruko_stab_stop,
	.apply_ae_crop = maruko_stab_apply_ae_crop,
	.set_pan       = maruko_stab_set_pan,
	.active        = maruko_stab_active,
	.set_live      = maruko_stab_set_live,
};

#endif /* HAVE_FRAMING_STAB */
