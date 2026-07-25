#include "star6e_framing_stab.h"
#include "star6e_framing_host.h"
#include "star6e_pipeline.h"
#include "framing_kalman.h"
#include "framing_stab_accuracy.h"
#include "imu_ring.h"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ── Image stabilization framing module — "stab" preset (Star6E) ──────────
 *
 * HW-crop DIS using MI_IVE_Shift_Detector.  Registered as a FramingModule
 * (see star6e_framing.h); the pipeline selects it when video0.framing="stab".
 *
 *   VIF → VPE → port0 (SCL-cropped to the encode dim) → bound zero-copy to
 *               VENC ch0.
 *   VIF → VPE → port1 (256x256 centre tap) → detector thread:
 *       1. drain the port1 tap frame
 *       2. run MI_IVE_Shift_Detector on a centre Y patch vs the previous frame
 *       3. accumulate dx/dy → off_x/off_y, clipped to half the dead border
 *       4. reprogram port0's MI_VPE_SetPortCrop to the shifted window — the
 *          bind feeds VENC the stabilized crop with no per-frame copy.
 *
 * If the BSP rejects the port1 tap, port0 stays bound at a static centre crop
 * (no detector).  Local types and dlsym-resolved symbols mirror the working
 * standalone star.c sample; pulling SigmaStar mi_sys.h / mi_ive.h here would
 * collide with waybeam's own MI compatibility layer in include/star6e.h. */

typedef MI_U64 STAB_MI_PHY;

typedef struct {
	MI_U16 u16X;
	MI_U16 u16Y;
	MI_U16 u16Width;
	MI_U16 u16Height;
} StabSysWindowRect_t;

typedef struct {
	int eTileMode;
	int ePixelFormat;
	int eCompressMode;
	int eFrameScanMode;
	int eFieldType;
	int ePhylayoutType;
	MI_U16 u16Width;
	MI_U16 u16Height;
	void *pVirAddr[3];
	STAB_MI_PHY phyAddr[3];
	MI_U32 u32Stride[3];
	MI_U32 u32BufSize;
	MI_U16 u16RingBufStartLine;
	MI_U16 u16RingBufRealTotalHeight;
	struct {
		int eType;
		union {
			MI_U32 u32GlobalGradient;
		} uIspInfo;
	} stFrameIspInfo;
	StabSysWindowRect_t stContentCropWindow;
} StabSysFrameData_t;

typedef struct {
	void *pVirAddr;
	STAB_MI_PHY phyAddr;
	MI_U32 u32BufSize;
	MI_U32 u32ContentSize;
	MI_BOOL bEndOfFrame;
	MI_U64 u64SeqNum;
} StabSysRawData_t;

typedef struct {
	void *pVirAddr;
	STAB_MI_PHY phyAddr;
	MI_U32 u32Size;
	MI_U32 u32ExtraData;
	MI_U32 eDataFromModule;
} StabSysMetaData_t;

typedef struct {
	MI_U64 u64Pts;
	MI_U64 u64SidebandMsg;
	int eBufType;
	MI_BOOL bEndOfStream;
	MI_BOOL bUsrBuf;
	MI_U32 u32SequenceNumber;
	MI_BOOL bDrop;
	union {
		StabSysFrameData_t stFrameData;
		StabSysRawData_t stRawData;
		StabSysMetaData_t stMetaData;
		MI_U8 reserved_union[512];
	};
	MI_U8 u8CusFlag;
} StabSysBufInfo_t;

typedef struct {
	MI_U16 u16BufHAlignment;
	MI_U16 u16BufVAlignment;
	MI_U16 u16BufChromaAlignment;
	MI_BOOL bClearPadding;
} StabSysFrameBufExtraConfig_t;

typedef struct {
	MI_U16 u16Width;
	MI_U16 u16Height;
	int eFrameScanMode;
	int eFormat;
	StabSysFrameBufExtraConfig_t stFrameBufExtraConf;
	int eCompressMode;
} StabSysBufFrameConfig_t;

typedef struct {
	int eBufType;
	MI_U32 u32Flags;
	MI_U64 u64TargetPts;
	union {
		StabSysBufFrameConfig_t stFrameCfg;
		struct { MI_U32 u32Size; } stRawCfg;
		struct { MI_U32 u32Size; } stMetaCfg;
	};
	MI_U8 u8CusFlag;
} StabSysBufConf_t;

typedef MI_S32 StabSysBufHandle_t;

#define STAB_E_BUFDATA_FRAME              1
#define STAB_E_FRAME_SCAN_MODE_PROGRESSIVE 0
#define STAB_E_PIXEL_FRAME_I8             9

typedef int StabIveImageType_e;
#define STAB_E_IVE_IMAGE_TYPE_U8C1 0x0
#define STAB_E_IVE_IMAGE_TYPE_S8C1 0x1

typedef struct {
	StabIveImageType_e eType;
	STAB_MI_PHY aphyPhyAddr[3];
	MI_U8 *apu8VirAddr[3];
	MI_U16 azu16Stride[3];
	MI_U16 u16Width;
	MI_U16 u16Height;
	MI_U16 u16Reserved;
} StabIveImage_t;

#define STAB_E_IVE_SHIFT_DETECT_MODE_SINGLE 0x00

typedef struct {
	int enMode;
	MI_U8 pyramid_level;
	MI_U8 search_range;
	MI_U16 u16Left;
	MI_U16 u16Top;
	MI_U16 u16Width;
	MI_U16 u16Height;
} StabIveShiftDetectCtrl_t;

typedef MI_S32 (*stab_sys_get_fd_fn_t)(MI_SYS_ChnPort_t *port, MI_S32 *fd);
typedef MI_S32 (*stab_sys_close_fd_fn_t)(MI_S32 fd);
typedef MI_S32 (*stab_sys_out_get_buf_fn_t)(MI_SYS_ChnPort_t *port,
	StabSysBufInfo_t *buf, StabSysBufHandle_t *handle);
typedef MI_S32 (*stab_sys_out_put_buf_fn_t)(StabSysBufHandle_t handle);
typedef MI_S32 (*stab_sys_flush_inv_cache_fn_t)(void *vir, MI_U32 size);
typedef MI_S32 (*stab_sys_va2pa_fn_t)(void *vir, STAB_MI_PHY *phy);
/* VENC-input + blit/fill symbols — used only by the "stab-fill" manual-compose
 * preset (the HW-crop "stab" preset is zero-copy and needs none of these). */
typedef MI_S32 (*stab_sys_in_get_buf_fn_t)(MI_SYS_ChnPort_t *port,
	StabSysBufConf_t *conf, StabSysBufInfo_t *buf,
	StabSysBufHandle_t *handle, MI_S32 timeout_ms);
typedef MI_S32 (*stab_sys_in_put_buf_fn_t)(StabSysBufHandle_t handle,
	StabSysBufInfo_t *buf, MI_BOOL drop);
typedef MI_S32 (*stab_sys_blit_pa_fn_t)(StabSysFrameData_t *dst,
	StabSysWindowRect_t *dst_rect, StabSysFrameData_t *src,
	StabSysWindowRect_t *src_rect);
typedef MI_S32 (*stab_sys_fill_pa_fn_t)(StabSysFrameData_t *plane,
	MI_U32 fill_val, StabSysWindowRect_t *rect);

typedef int StabIveHandle_t;
typedef MI_S32 (*stab_ive_create_fn_t)(StabIveHandle_t handle);
typedef MI_S32 (*stab_ive_destroy_fn_t)(StabIveHandle_t handle);
typedef MI_S32 (*stab_ive_shift_fn_t)(StabIveHandle_t handle,
	StabIveImage_t *src1, StabIveImage_t *src2,
	StabIveImage_t *dst_x, StabIveImage_t *dst_y,
	StabIveShiftDetectCtrl_t *ctrl, MI_BOOL instant);

/* Shift_Detector geometry is now the shared, user-selectable
 * `video0.stab_accuracy` level — resolved from the ONE table in
 * framing_stab_accuracy.h so Star6E and Maruko cannot drift.  On Star6E there
 * is no IVE kernel module, so MI_IVE_Shift_Detector runs as a userspace NEON
 * CPU fallback — the dominant per-frame stab cost (~19ms on the A7), roughly
 * independent of crop resolution.  "auto" maps to "high" (384/256/3): a larger
 * correlation box and 3-level pyramid give noticeably smoother estimates than
 * the cheaper configs (which produce visibly jittery stabilization — noisier
 * estimates, offset applied raw every frame).  Smoothness was chosen as the
 * Star6E default; a user can trade it for fps by picking "medium"/"low".
 * g_det is resolved once per prepare(); every geometry site reads
 * g_det.{crop,box,pyramid,search}.  margin = (crop-box)/2 = 64px at every
 * level. */
static FramingStabDetector g_det = { 384, 256, 3, 96 };  /* Star6E default = "high" */
#define STAB_SHIFT_SIGN_X   (-1)
#define STAB_SHIFT_SIGN_Y   (-1)
/* Run the (CPU-bound) detector every Nth drained frame.  N=1 (detect +
 * correct every frame) is required for smooth stabilization: sampling
 * motion below the frame rate aliases real jitter (Nyquist), so the
 * accumulator mis-corrects and the image visibly fights/shakes.  Keep
 * this at 1 unless a higher resolution makes the per-frame detector cost
 * exceed the frame budget — at 1152x864 the cheapened detector (256 crop,
 * 128 box, 2-level pyramid) fits 60fps every frame.  Bump only as a last
 * resort; it trades stabilization quality for fps. */
#define STAB_DETECT_EVERY   1

/* The Kalman trajectory smoother — the SINGLE control law for both "stab" and
 * "stab-fill" — now lives in the SoC-agnostic helper framing_kalman.{c,h}
 * (shared with the Maruko backend so the tuning constants can't drift).  The
 * state + defaults + bounds moved there; this file just seeds and steps it. */

static stab_sys_get_fd_fn_t g_stab_sys_get_fd;
static stab_sys_close_fd_fn_t g_stab_sys_close_fd;
static stab_sys_out_get_buf_fn_t g_stab_sys_out_get_buf;
static stab_sys_out_put_buf_fn_t g_stab_sys_out_put_buf;
static stab_sys_flush_inv_cache_fn_t g_stab_sys_flush_inv_cache;
static stab_sys_va2pa_fn_t g_stab_sys_va2pa;
/* "stab-fill" manual-compose symbols (resolved on demand in start()). */
static stab_sys_in_get_buf_fn_t g_stab_sys_in_get_buf;
static stab_sys_in_put_buf_fn_t g_stab_sys_in_put_buf;
static stab_sys_blit_pa_fn_t g_stab_sys_blit_pa;
static stab_sys_fill_pa_fn_t g_stab_sys_fill_pa;

static stab_ive_create_fn_t g_stab_ive_create;
static stab_ive_destroy_fn_t g_stab_ive_destroy;
static stab_ive_shift_fn_t g_stab_ive_shift;
static StabIveHandle_t g_stab_ive_handle;
static int g_stab_ive_created;
static void *g_stab_ive_lib;

static pthread_t g_stab_thread;
static volatile int g_stab_running;

/* Blit thread (stab-fill only).  IVE is CPU-bound, BufBlitPa is memory-
 * bandwidth-bound — different SoC resources.  Running them on separate threads
 * lets the next frame's IVE on core A overlap with the current frame's blit on
 * core B (dual-core A7 like SSC338Q).  Single-slot queue; deeper queues just
 * add latency without throughput. */
static pthread_t       g_stab_blit_thread;
static volatile int    g_stab_blit_active;
static pthread_mutex_t g_stab_blit_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_stab_blit_cond_in  = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_stab_blit_cond_out = PTHREAD_COND_INITIALIZER;
static struct {
	StabSysBufInfo_t   buf;
	StabSysBufHandle_t handle;
	int                acc_x;
	int                acc_y;
	int                pending;
} g_stab_blit_slot;
/* Stabilization data path (single HW-crop mode): VPE port0 hardware-crops the
 * stab window straight to VENC (zero-copy bind); a tiny port1 256x256 tap feeds
 * the detector, which updates port0's SetPortCrop rect per detect.  No
 * per-frame BufBlitPa; port0 tears down via the standard bound path.
 *   g_stab_tap_active=1 when the port1 detector tap came up and the detector
 *   thread runs.  If the BSP rejects the tap, port0 still binds to VENC
 *   (static centre crop, no shake compensation) and the detector is skipped.
 *   g_stab_pause/parked are a quiesce handshake so teardown can disable port1
 *   while the detector is guaranteed not inside an MI_SYS call. */
static volatile int g_stab_tap_active;
/* "stab-fill" floating-image mode: port0 SCL-downscales the full sensor frame
 * to the configured encode size and the manual-drain compose shifts a window
 * inside that buffer, filling the exposed edge with black (BufFillPa strips +
 * BufBlitPa content).  Set by star6e_stab_configure (cleared) / the stab-fill
 * prepare (set).  Mutually exclusive with the HW-crop path. */
static volatile int g_stab_fill_mode;
/* 0 ⇒ port0 is bound/centred but the detector thread is not running (HW-crop
 * degrade: no port1 tap).  Always 1 in stab-fill. */
static volatile int g_stab_detector_enabled = 1;
static volatile int g_stab_pause;
static volatile int g_stab_parked;
static uint32_t g_stab_src_w;     /* image (port0-output) domain — detector + */
static uint32_t g_stab_src_h;     /* accumulator + recenter all use this dim   */
static uint32_t g_stab_enc_w;
static uint32_t g_stab_enc_h;
/* VPE channel input dim (precrop / VIF→VPE window).  MI_VPE_SetPortCrop is an
 * INPUT-domain crop, so HW mode scales the image-domain stab window into this
 * domain.  Equals g_stab_src_* when the sensor isn't aspect-cropped (ratio 1).
 * Only HW-crop mode uses these. */
static uint32_t g_stab_pre_w;
static uint32_t g_stab_pre_h;
static uint32_t g_stab_crop_percent;
static uint32_t g_stab_recenter_period;   /* pauseStab glide rate (frames); 0=default */
/* Kalman state (facc trajectory + per-axis estimate/uncertainty + q/r).  Reset
 * in star6e_stab_configure via framing_kalman_reset (MUT_RESTART, fixed for the
 * run) so q/r need no lock — never written after init.  Both "stab" and
 * "stab-fill" use this filter. */
static FramingKalman g_stab_kalman = {
	0.0, 0.0, 0.0, 0.0, 1.0, 1.0,
	FRAMING_KALMAN_Q_DEFAULT, FRAMING_KALMAN_R_DEFAULT
};
/* stab-fill runtime bypass (video0.pauseStab).  Software-only (D13): when set,
 * the detector glides the applied offset back to centre via the recenter ramp
 * and the compose feeds the unshifted full frame — no HW rebind, no thread
 * teardown, no MMU storm.  Flipped via set_live under g_stab_path_lock. */
static pthread_mutex_t g_stab_path_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_stab_paused;
/* Debug-OSD snapshot: last detector measurement + the Kalman correction
 * actually applied, both in stab pixels.  Written by the detector thread at
 * the framing_kalman_step() site, read at 1 Hz by the pipeline thread's OSD
 * refresh — plain volatiles, torn reads are cosmetic on a diagnostic row. */
static volatile int g_stab_osd_acc_x, g_stab_osd_acc_y;
static volatile int g_stab_osd_meas_x, g_stab_osd_meas_y;
/* User-controlled pan center as parts-per-thousand of (src_w, src_h).
 * 500/500 = exact center.  Updated live via star6e_stab_set_pan() so
 * the existing zoomX/zoomY HTTP controls steer the stabilized framing
 * without a pipeline restart. */
static volatile int g_stab_pan_x_mil = 500;
static volatile int g_stab_pan_y_mil = 500;
static MI_SYS_ChnPort_t g_stab_vpe_port;
/* VENC input port (stab-fill manual compose drains port0 and feeds VENC here). */
static MI_SYS_ChnPort_t g_stab_venc_port;

/* apply_ae_crop is defined after the detector block but called by set_pan. */
static void star6e_stab_apply_ae_crop(void);

static int star6e_stab_load_sys_extra_symbols(void)
{
	void *h;

	if (g_stab_sys_out_get_buf && g_stab_sys_out_put_buf &&
	    g_stab_sys_flush_inv_cache && g_stab_sys_va2pa &&
	    (!g_stab_fill_mode || (g_stab_sys_in_get_buf &&
	     g_stab_sys_in_put_buf && g_stab_sys_blit_pa && g_stab_sys_fill_pa)))
		return 0;

	h = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h)
		return -1;

	g_stab_sys_get_fd = (stab_sys_get_fd_fn_t)dlsym(h, "MI_SYS_GetFd");
	g_stab_sys_close_fd = (stab_sys_close_fd_fn_t)dlsym(h, "MI_SYS_CloseFd");
	g_stab_sys_out_get_buf = (stab_sys_out_get_buf_fn_t)dlsym(h,
		"MI_SYS_ChnOutputPortGetBuf");
	g_stab_sys_out_put_buf = (stab_sys_out_put_buf_fn_t)dlsym(h,
		"MI_SYS_ChnOutputPortPutBuf");
	g_stab_sys_flush_inv_cache = (stab_sys_flush_inv_cache_fn_t)dlsym(h,
		"MI_SYS_FlushInvCache");
	g_stab_sys_va2pa = (stab_sys_va2pa_fn_t)dlsym(h, "MI_SYS_Va2Pa");

	/* stab-fill manual-compose symbols. */
	g_stab_sys_in_get_buf = (stab_sys_in_get_buf_fn_t)dlsym(h,
		"MI_SYS_ChnInputPortGetBuf");
	g_stab_sys_in_put_buf = (stab_sys_in_put_buf_fn_t)dlsym(h,
		"MI_SYS_ChnInputPortPutBuf");
	g_stab_sys_blit_pa = (stab_sys_blit_pa_fn_t)dlsym(h, "MI_SYS_BufBlitPa");
	g_stab_sys_fill_pa = (stab_sys_fill_pa_fn_t)dlsym(h, "MI_SYS_BufFillPa");

	if (!(g_stab_sys_out_get_buf && g_stab_sys_out_put_buf &&
	      g_stab_sys_flush_inv_cache && g_stab_sys_va2pa))
		return -1;
	if (g_stab_fill_mode &&
	    !(g_stab_sys_in_get_buf && g_stab_sys_in_put_buf &&
	      g_stab_sys_blit_pa && g_stab_sys_fill_pa))
		return -1;
	return 0;
}

static uint64_t star6e_stab_pts_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* Compute encoded dims preserving the configured image_w:image_h aspect.
 * Integer math only — no floats per CONVENTIONS.md §1.  Width is 8-aligned
 * down (VENC + SCL); height is 2-aligned down (NV12). */
static void star6e_stab_compute_crop_dims(uint32_t src_w, uint32_t src_h,
	uint32_t pct, uint32_t *out_w, uint32_t *out_h)
{
	uint32_t w;
	uint32_t h;

	w = (src_w * pct) / 100u;
	w &= ~7u;
	if (w == 0) w = src_w & ~7u;
	h = (uint32_t)(((uint64_t)w * src_h) / src_w);
	h &= ~1u;

	if (h > src_h) {
		h = ((src_h * pct) / 100u) & ~1u;
		if (h == 0) h = src_h & ~1u;
		w = (uint32_t)(((uint64_t)h * src_w) / src_h);
		w &= ~7u;
	}

	if (w < 64 || h < 64 || w > src_w || h > src_h) {
		w = src_w & ~7u;
		h = src_h & ~1u;
	}

	*out_w = w;
	*out_h = h;
}

static int star6e_stab_pan_clamp_mil(double v)
{
	int mil;

	if (!isfinite(v) || v <= 0.0) return 0;
	if (v >= 1.0) return 1000;
	mil = (int)(v * 1000.0 + 0.5);
	if (mil < 0) mil = 0;
	if (mil > 1000) mil = 1000;
	return mil;
}

/* enc_w/enc_h: explicit encoded-window dims.  0 ⇒ derive from crop_pct
 * (crop+shrink "stab").  Non-zero ⇒ use directly ("stab-fill": port0
 * SCL-downscales the full sensor frame to the configured encode size, fixed
 * for the run; the per-frame manual-drain compose shifts a window inside it). */
static void star6e_stab_configure(uint32_t src_w, uint32_t src_h,
	uint32_t crop_pct, uint32_t enc_w, uint32_t enc_h,
	uint32_t recenter_speed, uint32_t venc_fps,
	double pan_x, double pan_y, double kalman_q, double kalman_r)
{
	/* Effective crop budget is always [60, 100] — config load already clamps,
	 * but guard the math here too (hand-edited config / future callers). */
	if (crop_pct < 60) crop_pct = 60;
	if (crop_pct > 100) crop_pct = 100;
	g_stab_src_w = src_w & ~1u;
	g_stab_src_h = src_h & ~1u;
	g_stab_crop_percent = crop_pct;
	g_stab_fill_mode = 0;          /* stab-fill prepare opts in after configure */
	g_stab_detector_enabled = 1;   /* HW setup_ports may clear on tap failure */
	if (enc_w && enc_h) {
		g_stab_enc_w = enc_w & ~1u;
		g_stab_enc_h = enc_h & ~1u;
	} else {
		star6e_stab_compute_crop_dims(g_stab_src_w, g_stab_src_h,
			crop_pct, &g_stab_enc_w, &g_stab_enc_h);
	}

	/* Seed + reset the shared Kalman (validates q/r into bounds, zeroes the
	 * trajectory/estimate — matches the Python "count == 0" init: facc=0, X=0,
	 * P=1).  A hand-edited config that bypasses the HTTP validator can never
	 * freeze or destabilize the loop; out-of-range q/r fall back to the
	 * compile-time default. */
	framing_kalman_reset(&g_stab_kalman, kalman_q, kalman_r);

	/* recenter_speed now only sets the pauseStab glide-home rate (frames):
	 * 0 = default ~30-frame ramp, lower = faster glide.  During normal
	 * stabilization the Kalman handles recentering, so this is inert. */
	(void)venc_fps;
	g_stab_recenter_period = recenter_speed;
	g_stab_pan_x_mil = star6e_stab_pan_clamp_mil(pan_x);
	g_stab_pan_y_mil = star6e_stab_pan_clamp_mil(pan_y);
	g_stab_paused = 0;
}

/* Live pan update — called from the LIVE_GROUP_ZOOM apply path so that
 * the existing zoomX/zoomY HTTP controls steer the stabilized framing
 * without a pipeline restart. */
static void star6e_stab_set_pan(double pan_x, double pan_y)
{
	g_stab_pan_x_mil = star6e_stab_pan_clamp_mil(pan_x);
	g_stab_pan_y_mil = star6e_stab_pan_clamp_mil(pan_y);
	/* Keep the AE meter on the stabilized crop as it pans, mirroring the
	 * zoom path's AE tracking — the only intended runtime difference
	 * between the two modes is the pan ramp. */
	star6e_stab_apply_ae_crop();
}

/* Max per-axis shift (image domain).  HW-crop uses the oversample margin
 * (src-enc)/2.  Fill mode has no crop margin (enc==src-aspect window), so the
 * shift limit is the black-border budget set by stab_crop_pct:
 * src * (100 - pct) / 200 (e.g. pct=80 → 10% of src per side). */
static int star6e_stab_max_off_x(void)
{
	if (g_stab_fill_mode)
		return (int)((g_stab_src_w * (100u - g_stab_crop_percent)) / 200u);
	return (int)((g_stab_src_w - g_stab_enc_w) / 2u);
}

static int star6e_stab_max_off_y(void)
{
	if (g_stab_fill_mode)
		return (int)((g_stab_src_h * (100u - g_stab_crop_percent)) / 200u);
	return (int)((g_stab_src_h - g_stab_enc_h) / 2u);
}

/* Map an image-domain length to the VPE-input (precrop) domain.  When the
 * sensor isn't aspect-cropped pre==src and this is the identity. */
static int star6e_stab_img_to_pre_x(int v)
{
	return (int)((int64_t)v * (int64_t)g_stab_pre_w / (int64_t)g_stab_src_w);
}
static int star6e_stab_img_to_pre_y(int v)
{
	return (int)((int64_t)v * (int64_t)g_stab_pre_h / (int64_t)g_stab_src_h);
}

/* HW-crop output: program VPE port0's SetPortCrop to the stab window
 * (enc_w x enc_h positioned at pan-center + accumulated shake offset).  The
 * window is computed in image domain (src_x/src_y math drives the
 * accumulator/feel) and then scaled into the
 * VPE INPUT (precrop) domain, because MI_VPE_SetPortCrop crops the channel
 * input — the SCL then scales that window down to the encoded port output,
 * the same downscale ratio the non-stab full-frame path uses.  x/y/w/h
 * aligned to 2 for NV12 chroma; the SDK accepts 2-px granularity (zoom path
 * precedent), preserving fine stabilization steps. */
static void star6e_stab_apply_port_crop(int acc_x, int acc_y)
{
	int pan_x = g_stab_pan_x_mil;
	int pan_y = g_stab_pan_y_mil;
	int center_x = (int)((g_stab_src_w * (uint32_t)pan_x) / 1000u);
	int center_y = (int)((g_stab_src_h * (uint32_t)pan_y) / 1000u);
	int src_x = center_x - (int)g_stab_enc_w / 2 + acc_x;
	int src_y = center_y - (int)g_stab_enc_h / 2 + acc_y;
	int max_x = (int)(g_stab_src_w - g_stab_enc_w);
	int max_y = (int)(g_stab_src_h - g_stab_enc_h);
	int rx, ry, rw, rh, rmax_x, rmax_y;
	i6_common_rect rect;
	MI_S32 ret;

	if (src_x < 0) src_x = 0;
	if (src_x > max_x) src_x = max_x;
	if (src_y < 0) src_y = 0;
	if (src_y > max_y) src_y = max_y;

	/* Scale image window → precrop (input) domain. */
	rx = star6e_stab_img_to_pre_x(src_x) & ~1;
	ry = star6e_stab_img_to_pre_y(src_y) & ~1;
	rw = star6e_stab_img_to_pre_x((int)g_stab_enc_w) & ~1;
	rh = star6e_stab_img_to_pre_y((int)g_stab_enc_h) & ~1;
	if (rw < 2) rw = 2;
	if (rh < 2) rh = 2;
	rmax_x = (int)g_stab_pre_w - rw;
	rmax_y = (int)g_stab_pre_h - rh;
	if (rx < 0) rx = 0;
	if (rx > rmax_x) rx = rmax_x;
	if (ry < 0) ry = 0;
	if (ry > rmax_y) ry = rmax_y;

	rect.x = (unsigned short)rx;
	rect.y = (unsigned short)ry;
	rect.width = (unsigned short)rw;
	rect.height = (unsigned short)rh;
	ret = MI_VPE_SetPortCrop(0, 0, &rect);
	if (ret != 0) {
		static int warned;
		if (!warned) {
			warned = 1;
			fprintf(stderr, "[waybeam] stab HW SetPortCrop(0,0) "
				"x=%d y=%d %dx%d (pre %ux%u) failed %d\n", rx, ry,
				rw, rh, g_stab_pre_w, g_stab_pre_h, (int)ret);
		}
	}
}


static int star6e_stab_make_center_y_crop(StabIveImage_t *image,
	const StabSysBufInfo_t *buf, int crop_w, int crop_h)
{
	int src_w;
	int src_h;
	int stride;
	int crop_x;
	int crop_y;

	if (!image || !buf || buf->eBufType != STAB_E_BUFDATA_FRAME ||
	    !buf->stFrameData.pVirAddr[0] || !buf->stFrameData.phyAddr[0])
		return -1;

	src_w = (int)buf->stFrameData.u16Width;
	src_h = (int)buf->stFrameData.u16Height;
	stride = (int)buf->stFrameData.u32Stride[0];
	if (crop_w > src_w) crop_w = src_w;
	if (crop_h > src_h) crop_h = src_h;
	crop_w &= ~15;
	crop_h &= ~1;
	crop_x = ((src_w - crop_w) / 2) & ~15;
	crop_y = ((src_h - crop_h) / 2) & ~1;
	if (crop_x < 0) crop_x = 0;
	if (crop_y < 0) crop_y = 0;

	memset(image, 0, sizeof(*image));
	image->eType = STAB_E_IVE_IMAGE_TYPE_U8C1;
	image->u16Width = (MI_U16)crop_w;
	image->u16Height = (MI_U16)crop_h;
	image->apu8VirAddr[0] = (MI_U8 *)buf->stFrameData.pVirAddr[0] +
		crop_y * stride + crop_x;
	image->aphyPhyAddr[0] = buf->stFrameData.phyAddr[0] +
		(STAB_MI_PHY)(crop_y * stride + crop_x);
	image->azu16Stride[0] = (MI_U16)stride;
	return 0;
}

static int star6e_stab_alloc_ive_image(StabIveImage_t *image,
	MI_U16 width, MI_U16 height, StabIveImageType_e type)
{
	MI_U32 align = 64;
	MI_U32 size;
	STAB_MI_PHY phy = 0;
	MI_S32 ret;

	memset(image, 0, sizeof(*image));
	image->eType = type;
	image->u16Width = width;
	image->u16Height = height;
	image->azu16Stride[0] = (width + (align - 1)) & ~(align - 1);
	size = image->azu16Stride[0] * height;
	if (posix_memalign((void **)&image->apu8VirAddr[0], align, size) != 0)
		return -1;
	memset(image->apu8VirAddr[0], 0, size);

	/* IVE needs a valid physical address even for 1×1 S8C1 result images;
	 * leaving aphyPhyAddr[0]=0 makes Shift_Detector return zero dx/dy. */
	ret = g_stab_sys_va2pa(image->apu8VirAddr[0], &phy);
	if (ret != 0 || !phy)
		phy = (STAB_MI_PHY)(uintptr_t)image->apu8VirAddr[0];
	image->aphyPhyAddr[0] = phy;
	return 0;
}

/* ── stab-fill manual-compose helpers ─────────────────────────────────────
 * The "stab-fill" preset drains port0 (full encode-dim frame) and composes a
 * shifted, black-bordered copy into a fresh VENC input buffer.  These helpers
 * (UV addressing, i8 plane descriptors, clipped fill/blit, the threaded blit
 * worker) implement that path; the HW-crop "stab" preset uses none of them. */

static STAB_MI_PHY star6e_stab_uv_pa(const StabSysFrameData_t *f, uint32_t h)
{
	if (f->phyAddr[1])
		return f->phyAddr[1];
	return f->phyAddr[0] + (STAB_MI_PHY)f->u32Stride[0] * h;
}

static void *star6e_stab_uv_va(const StabSysFrameData_t *f, uint32_t h)
{
	if (f->pVirAddr[1])
		return f->pVirAddr[1];
	if (!f->pVirAddr[0])
		return NULL;
	return (void *)((uint8_t *)f->pVirAddr[0] + f->u32Stride[0] * h);
}

/* Build an i8 (mono-channel) plane descriptor for fill/blit calls. */
static void star6e_stab_make_i8_plane(StabSysFrameData_t *out,
	STAB_MI_PHY phy, void *vir, int width, int height, int stride)
{
	memset(out, 0, sizeof(*out));
	out->ePixelFormat = (int)I6_PIXFMT_I8;
	out->u16Width = (MI_U16)width;
	out->u16Height = (MI_U16)height;
	out->phyAddr[0] = phy;
	out->pVirAddr[0] = vir;
	out->u32Stride[0] = (MI_U32)stride;
}

/* Fill helper.  CRITICAL: BufFillPa writes 32-bit words, so the per-byte fill
 * value must be replicated across all four bytes of fill32 — otherwise the
 * plane gets striped (e.g. Y=16 passed raw produces 0x10000000 → green bars). */
static int star6e_stab_fill_i8_rect(StabSysFrameData_t *plane,
	int x, int y, int w, int h, MI_U32 value)
{
	StabSysWindowRect_t r;
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
	r.u16X = (MI_U16)x;
	r.u16Y = (MI_U16)y;
	r.u16Width = (MI_U16)w;
	r.u16Height = (MI_U16)h;

	v = value & 0xffu;
	fill32 = v | (v << 8) | (v << 16) | (v << 24);
	ret = g_stab_sys_fill_pa(plane, fill32, &r);
	if (ret != 0) {
		static int warned;
		if (!warned) {
			warned = 1;
			fprintf(stderr, "[waybeam] stab-fill MI_SYS_BufFillPa "
				"ret=0x%x rect=%d,%d %dx%d val=%u\n",
				(unsigned)ret, x, y, w, h, value);
		}
		return ret;
	}
	return 0;
}

/* Clipped, even-aligned BufBlitPa of an i8 rect from src to dst. */
static int star6e_stab_blit_i8_rect(StabSysFrameData_t *dst,
	int dst_x, int dst_y, StabSysFrameData_t *src,
	int src_x, int src_y, int w, int h)
{
	StabSysWindowRect_t sr;
	StabSysWindowRect_t dr;
	MI_S32 ret;

	if (!dst || !src || !dst->phyAddr[0] || !src->phyAddr[0])
		return -1;
	if (w <= 0 || h <= 0)
		return 0;

	dst_x &= ~1;
	src_x &= ~1;
	w &= ~1;

	if (src_x < 0) { int d = -src_x; src_x = 0; dst_x += d; w -= d; }
	if (src_y < 0) { int d = -src_y; src_y = 0; dst_y += d; h -= d; }
	if (dst_x < 0) { int d = -dst_x; dst_x = 0; src_x += d; w -= d; }
	if (dst_y < 0) { int d = -dst_y; dst_y = 0; src_y += d; h -= d; }
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
	sr.u16X = (MI_U16)src_x;
	sr.u16Y = (MI_U16)src_y;
	sr.u16Width = (MI_U16)w;
	sr.u16Height = (MI_U16)h;
	dr.u16X = (MI_U16)dst_x;
	dr.u16Y = (MI_U16)dst_y;
	dr.u16Width = (MI_U16)w;
	dr.u16Height = (MI_U16)h;

	ret = g_stab_sys_blit_pa(dst, &dr, src, &sr);
	if (ret != 0) {
		static int warned;
		if (!warned) {
			warned = 1;
			fprintf(stderr, "[waybeam] stab-fill MI_SYS_BufBlitPa "
				"ret=0x%x src=%d,%d %dx%d dst=%d,%d\n",
				(unsigned)ret, src_x, src_y, w, h, dst_x, dst_y);
		}
		return ret;
	}
	return 0;
}

/* Copy the centre Y crop of `src_buf` into an already-allocated IVE image
 * (sw_detect[]).  Independent of port0 — once copied, the source buffer can be
 * released without invalidating the IVE input, so the blit thread can return
 * the port0 handle to VPE as soon as its blit is done. */
static int star6e_stab_copy_y_to_sw(StabIveImage_t *dst,
	const StabSysBufInfo_t *src_buf, int crop_w, int crop_h)
{
	int src_w, src_h, stride;
	int crop_x, crop_y;
	const uint8_t *src_p;
	uint8_t *dst_p;
	int row;

	if (!dst || !src_buf || !dst->apu8VirAddr[0] ||
	    src_buf->eBufType != STAB_E_BUFDATA_FRAME ||
	    !src_buf->stFrameData.pVirAddr[0])
		return -1;

	src_w = (int)src_buf->stFrameData.u16Width;
	src_h = (int)src_buf->stFrameData.u16Height;
	stride = (int)src_buf->stFrameData.u32Stride[0];

	if (crop_w > src_w) crop_w = src_w;
	if (crop_h > src_h) crop_h = src_h;
	crop_w &= ~15;
	crop_h &= ~1;
	crop_x = ((src_w - crop_w) / 2) & ~15;
	crop_y = ((src_h - crop_h) / 2) & ~1;
	if (crop_x < 0) crop_x = 0;
	if (crop_y < 0) crop_y = 0;

	src_p = (const uint8_t *)src_buf->stFrameData.pVirAddr[0] +
		crop_y * stride + crop_x;
	dst_p = dst->apu8VirAddr[0];

	for (row = 0; row < crop_h; row++) {
		memcpy(dst_p, src_p, (size_t)crop_w);
		src_p += stride;
		dst_p += dst->azu16Stride[0];
	}
	return 0;
}

/* stab-fill compose: GetBuf a fresh VENC-input frame, BufFillPa the four
 * exposed black strips (Y=16, UV=128), BufBlitPa the in-bounds source content
 * at the shifted sub-rect, PutBuf.  src dim == dst dim == encode dim — the only
 * scaling (full sensor → encode) is the fixed SCL on port0.  off_x/off_y are
 * the high-pass shake compensation in output-pixel domain. */
static int star6e_stab_send_frame_to_venc_fill(const StabSysBufInfo_t *src_buf,
	int off_x, int off_y)
{
	StabSysBufConf_t conf;
	StabSysBufInfo_t venc_buf;
	StabSysBufHandle_t venc_handle = 0;
	const StabSysFrameData_t *src;
	StabSysFrameData_t *dst;
	StabSysFrameData_t src_y_plane, dst_y_plane;
	StabSysFrameData_t src_uv_plane, dst_uv_plane;
	int src_w, src_h, dst_w, dst_h;
	int src_x, src_y, dst_x, dst_y, copy_w, copy_h;
	int src_y_stride, dst_y_stride, src_uv_stride, dst_uv_stride;
	int uv_dst_y, uv_copy_h, uv_h;
	STAB_MI_PHY src_uv, dst_uv;
	MI_S32 ret;

	if (!g_stab_sys_blit_pa || !g_stab_sys_fill_pa ||
	    !g_stab_sys_in_get_buf || !g_stab_sys_in_put_buf)
		return -1;

	src = &src_buf->stFrameData;
	src_w = src->u16Width ? src->u16Width : (int)g_stab_src_w;
	src_h = src->u16Height ? src->u16Height : (int)g_stab_src_h;
	dst_w = (int)g_stab_enc_w;
	dst_h = (int)g_stab_enc_h;

	if (dst_w != src_w || dst_h != src_h) {
		static int warned;
		if (!warned) {
			warned = 1;
			fprintf(stderr, "[waybeam] stab-fill PAD size mismatch "
				"src=%dx%d dst=%dx%d\n", src_w, src_h, dst_w, dst_h);
		}
		return -1;
	}

	memset(&conf, 0, sizeof(conf));
	conf.eBufType = STAB_E_BUFDATA_FRAME;
	conf.u64TargetPts = star6e_stab_pts_us();
	conf.stFrameCfg.eFormat = I6_PIXFMT_YUV420SP;
	conf.stFrameCfg.eFrameScanMode = STAB_E_FRAME_SCAN_MODE_PROGRESSIVE;
	conf.stFrameCfg.u16Width = (MI_U16)dst_w;
	conf.stFrameCfg.u16Height = (MI_U16)dst_h;
	memset(&venc_buf, 0, sizeof(venc_buf));
	ret = g_stab_sys_in_get_buf(&g_stab_venc_port, &conf,
		&venc_buf, &venc_handle, 20);
	if (ret != 0)
		return ret;
	dst = &venc_buf.stFrameData;

	/* NV12 requires even offsets. */
	off_x &= ~1;
	off_y &= ~1;

	/* Clamp to the max pad (border budget). */
	{
		int max_x = star6e_stab_max_off_x();
		int max_y = star6e_stab_max_off_y();
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
		g_stab_sys_in_put_buf(venc_handle, &venc_buf, true);
		return -1;
	}

	src_y_stride = (int)src->u32Stride[0];
	dst_y_stride = (int)dst->u32Stride[0];
	src_uv_stride = src->u32Stride[1] ?
		(int)src->u32Stride[1] : src_y_stride;
	dst_uv_stride = dst->u32Stride[1] ?
		(int)dst->u32Stride[1] : dst_y_stride;
	src_uv = star6e_stab_uv_pa(src, (uint32_t)src_h);
	dst_uv = star6e_stab_uv_pa(dst, (uint32_t)dst_h);
	if (!src_uv || !dst_uv) {
		g_stab_sys_in_put_buf(venc_handle, &venc_buf, true);
		return -1;
	}

	star6e_stab_make_i8_plane(&src_y_plane,
		src->phyAddr[0], src->pVirAddr[0], src_w, src_h, src_y_stride);
	star6e_stab_make_i8_plane(&dst_y_plane,
		dst->phyAddr[0], dst->pVirAddr[0], dst_w, dst_h, dst_y_stride);
	star6e_stab_make_i8_plane(&src_uv_plane, src_uv,
		star6e_stab_uv_va(src, (uint32_t)src_h),
		src_w, src_h / 2, src_uv_stride);
	star6e_stab_make_i8_plane(&dst_uv_plane, dst_uv,
		star6e_stab_uv_va(dst, (uint32_t)dst_h),
		dst_w, dst_h / 2, dst_uv_stride);

	/* Black strips around the shifted content (Y=16, UV=128). */
	uv_dst_y  = dst_y / 2;
	uv_copy_h = copy_h / 2;
	uv_h      = dst_h / 2;
	if (dst_x > 0) {
		star6e_stab_fill_i8_rect(&dst_y_plane, 0, 0,
			dst_x, dst_h, 16);
		star6e_stab_fill_i8_rect(&dst_uv_plane, 0, 0,
			dst_x, uv_h, 128);
	}
	if (dst_x + copy_w < dst_w) {
		star6e_stab_fill_i8_rect(&dst_y_plane, dst_x + copy_w, 0,
			dst_w - (dst_x + copy_w), dst_h, 16);
		star6e_stab_fill_i8_rect(&dst_uv_plane, dst_x + copy_w, 0,
			dst_w - (dst_x + copy_w), uv_h, 128);
	}
	if (dst_y > 0) {
		star6e_stab_fill_i8_rect(&dst_y_plane, dst_x, 0,
			copy_w, dst_y, 16);
		star6e_stab_fill_i8_rect(&dst_uv_plane, dst_x, 0,
			copy_w, uv_dst_y, 128);
	}
	if (dst_y + copy_h < dst_h) {
		star6e_stab_fill_i8_rect(&dst_y_plane, dst_x, dst_y + copy_h,
			copy_w, dst_h - (dst_y + copy_h), 16);
		star6e_stab_fill_i8_rect(&dst_uv_plane, dst_x, uv_dst_y + uv_copy_h,
			copy_w, uv_h - (uv_dst_y + uv_copy_h), 128);
	}

	/* Copy the in-bounds Y rect, then the UV rect (Y pos/height halved). */
	ret = star6e_stab_blit_i8_rect(&dst_y_plane, dst_x, dst_y,
		&src_y_plane, src_x, src_y, copy_w, copy_h);
	if (ret != 0) goto err;
	ret = star6e_stab_blit_i8_rect(&dst_uv_plane, dst_x, dst_y / 2,
		&src_uv_plane, src_x, src_y / 2, copy_w, copy_h / 2);
	if (ret != 0) goto err;

	return g_stab_sys_in_put_buf(venc_handle, &venc_buf, false);

err:
	g_stab_sys_in_put_buf(venc_handle, &venc_buf, true);
	return ret;
}

/* Blit-worker loop (stab-fill): consume the single-slot queue, compose, then
 * release the port0 handle back to VPE. */
static void *star6e_stab_blit_thread_main(void *arg)
{
	(void)arg;
	while (g_stab_running) {
		StabSysBufInfo_t  buf;
		StabSysBufHandle_t handle = 0;
		int acc_x = 0, acc_y = 0;
		int have_work = 0;

		pthread_mutex_lock(&g_stab_blit_lock);
		while (g_stab_running && !g_stab_blit_slot.pending)
			pthread_cond_wait(&g_stab_blit_cond_in, &g_stab_blit_lock);
		if (g_stab_blit_slot.pending) {
			buf = g_stab_blit_slot.buf;
			handle = g_stab_blit_slot.handle;
			acc_x = g_stab_blit_slot.acc_x;
			acc_y = g_stab_blit_slot.acc_y;
			g_stab_blit_slot.pending = 0;
			have_work = 1;
			pthread_cond_signal(&g_stab_blit_cond_out);
		}
		pthread_mutex_unlock(&g_stab_blit_lock);

		if (have_work) {
			(void)star6e_stab_send_frame_to_venc_fill(&buf, acc_x, acc_y);
			if (handle && g_stab_sys_out_put_buf)
				g_stab_sys_out_put_buf(handle);
		}
	}

	/* Shutdown: drain anything still in the slot so its handle isn't leaked
	 * back to VPE (otherwise the port pool sits on a missing buffer). */
	pthread_mutex_lock(&g_stab_blit_lock);
	if (g_stab_blit_slot.pending) {
		if (g_stab_blit_slot.handle && g_stab_sys_out_put_buf)
			g_stab_sys_out_put_buf(g_stab_blit_slot.handle);
		g_stab_blit_slot.pending = 0;
	}
	pthread_mutex_unlock(&g_stab_blit_lock);
	return NULL;
}

/* Push a drained port0 buffer to the blit thread (transfers handle ownership). */
static int star6e_stab_fill_queue_blit(StabSysBufHandle_t *handle_inout,
	const StabSysBufInfo_t *buf, int acc_x, int acc_y)
{
	if (!handle_inout || !buf)
		return -1;
	pthread_mutex_lock(&g_stab_blit_lock);
	while (g_stab_running && g_stab_blit_slot.pending)
		pthread_cond_wait(&g_stab_blit_cond_out, &g_stab_blit_lock);
	if (!g_stab_running) {
		pthread_mutex_unlock(&g_stab_blit_lock);
		return -1;
	}
	g_stab_blit_slot.buf     = *buf;
	g_stab_blit_slot.handle  = *handle_inout;
	g_stab_blit_slot.acc_x   = acc_x;
	g_stab_blit_slot.acc_y   = acc_y;
	g_stab_blit_slot.pending = 1;
	pthread_cond_signal(&g_stab_blit_cond_in);
	pthread_mutex_unlock(&g_stab_blit_lock);
	*handle_inout = 0;   /* ownership transferred */
	return 0;
}


/* Read the gyro samples captured during the frame interval [t0, t1] from the
 * shared IMU ring (CLOCK_MONOTONIC domain).  Returns the sample count and,
 * when non-zero, the mean angular rate (rad/s) on each axis.  This is the
 * frame-aligned data a gyro estimator integrates.  Returns 0 with zeroed
 * means when the IMU is disabled or no samples fell in the window — the
 * stabilizer then runs optical-only. */
static uint32_t star6e_stab_gyro_window(struct timespec t0, struct timespec t1,
	float *mean_gx, float *mean_gy, float *mean_gz)
{
	ImuRingSample s[64];
	uint32_t n, i;
	double sx = 0.0, sy = 0.0, sz = 0.0;

	*mean_gx = 0.0f;
	*mean_gy = 0.0f;
	*mean_gz = 0.0f;
	{
		ImuRing *ring = star6e_pipeline_imu_ring();
		if (!ring)
			return 0;
		n = imu_ring_read_range(ring, t0, t1, s,
			(uint32_t)(sizeof(s) / sizeof(s[0])));
	}
	for (i = 0; i < n; i++) {
		sx += s[i].gyro_x;
		sy += s[i].gyro_y;
		sz += s[i].gyro_z;
	}
	if (n > 0) {
		*mean_gx = (float)(sx / n);
		*mean_gy = (float)(sy / n);
		*mean_gz = (float)(sz / n);
	}
	return n;
}

/* Per-frame motion estimate.  On success returns 0 and fills out_dx/out_dy
 * with the measured inter-frame shift (signed pixels, source-frame domain;
 * pan/recenter applied by the caller); non-zero on detector failure.
 *
 * IMU-gyro fusion seam: today the estimate is purely optical
 * (MI_IVE_Shift_Detector).  To add gyro-assisted stabilization, read the
 * frame-aligned angular rates via star6e_stab_gyro_window(prev_ts, curr_ts,
 * ...), integrate yaw/pitch over the interval into a pixel shift using the
 * lens focal length (focal_px = (src_w/2) / tan(hfov/2)), and fuse it with
 * the optical shift here (e.g. a complementary filter: gyro for fast jitter,
 * optical to cancel gyro drift).  Nothing else in the pipeline changes. */
static int star6e_stab_estimate_shift(StabIveImage_t *prev_img,
	StabIveImage_t *curr_img, StabIveImage_t *dx, StabIveImage_t *dy,
	int *out_dx, int *out_dy)
{
	int img_w = (int)curr_img->u16Width;
	int img_h = (int)curr_img->u16Height;
	int box = g_det.box;
	int left, top;
	MI_S32 ret;

	if (box > img_w) box = img_w;
	if (box > img_h) box = img_h;
	left = ((img_w - box) / 2) & ~1;
	top  = ((img_h - box) / 2) & ~1;

	{
		StabIveShiftDetectCtrl_t ctrl = {
			.enMode = STAB_E_IVE_SHIFT_DETECT_MODE_SINGLE,
			.pyramid_level = g_det.pyramid,
			.search_range = g_det.search,
			.u16Left = (MI_U16)left,
			.u16Top = (MI_U16)top,
			.u16Width = (MI_U16)box,
			.u16Height = (MI_U16)box,
		};
		ret = g_stab_ive_shift(g_stab_ive_handle,
			prev_img, curr_img, dx, dy, &ctrl, true);
	}
	if (ret != 0)
		return ret;

	g_stab_sys_flush_inv_cache(dx->apu8VirAddr[0], dx->azu16Stride[0]);
	g_stab_sys_flush_inv_cache(dy->apu8VirAddr[0], dy->azu16Stride[0]);
	*out_dx = STAB_SHIFT_SIGN_X * (int)((int8_t *)dx->apu8VirAddr[0])[0];
	*out_dy = STAB_SHIFT_SIGN_Y * (int)((int8_t *)dy->apu8VirAddr[0])[0];
	return 0;
}

static void *star6e_stab_thread_main(void *arg)
{
	MI_S32 ret;
	MI_S32 fd = -1;
	StabSysBufHandle_t prev_handle = 0;
	StabIveImage_t prev_img;
	int have_prev = 0;
	/* Shake-correction offset accumulator now lives in g_stab_kalman (facc_*),
	 * so the return-to-center decays exact-proportionally and both axes
	 * converge along the true diagonal; acc_* are the rounded ints handed to
	 * the crop and OSD each detect. */
	/* Final low-pass on the APPLIED offset.  The accumulator/recenter math
	 * can carry per-frame jitter (detector noise, 2-px crop quantization);
	 * applied raw it shows as judder, and the geometry magnifies it
	 * differently per preset (low: small motion vs quantization; high: large
	 * upscale) so only the mid preset looked smooth.  The Kalman trajectory
	 * filter (applied below) absorbs that jitter — no separate EMA needed. */
	int acc_x = 0;
	int acc_y = 0;
	int dbg_frame = 0;
	int loop_n = 0;            /* drained frames since have_prev; gates detect */
	StabIveImage_t dx;
	StabIveImage_t dy;
	struct timespec prev_ts;
	struct timespec curr_ts;
	/* Fill-mode: independent Y copies for IVE so the port0 buffer can be
	 * handed to the blit thread immediately (and released back to VPE by the
	 * blit thread) without affecting next frame's IVE input.  Two-slot ring,
	 * toggled each detect: sw_detect[sw_idx] is current, [sw_idx^1] previous. */
	StabIveImage_t sw_detect[2];
	int sw_detect_ok = 0;
	int sw_idx = 0;

	(void)arg;
	memset(&prev_ts, 0, sizeof(prev_ts));
	memset(&prev_img, 0, sizeof(prev_img));
	memset(&dx, 0, sizeof(dx));
	memset(&dy, 0, sizeof(dy));
	memset(sw_detect, 0, sizeof(sw_detect));

	if (star6e_stab_alloc_ive_image(&dx, 1, 1,
	    STAB_E_IVE_IMAGE_TYPE_S8C1) != 0 ||
	    star6e_stab_alloc_ive_image(&dy, 1, 1,
	    STAB_E_IVE_IMAGE_TYPE_S8C1) != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab IVE result alloc failed — "
			"thread exiting, VENC ch0 will not receive frames\n");
		goto out;
	}

	if (g_stab_fill_mode) {
		if (star6e_stab_alloc_ive_image(&sw_detect[0], g_det.crop,
		    g_det.crop, STAB_E_IVE_IMAGE_TYPE_U8C1) == 0 &&
		    star6e_stab_alloc_ive_image(&sw_detect[1], g_det.crop,
		    g_det.crop, STAB_E_IVE_IMAGE_TYPE_U8C1) == 0) {
			sw_detect_ok = 1;
			fprintf(stderr, "[waybeam] stab-fill: sw_detect active "
				"(%dx%d x2) — blit thread decoupled from IVE\n",
				g_det.crop, g_det.crop);
		} else {
			fprintf(stderr, "[waybeam] WARNING: stab-fill sw_detect "
				"alloc failed — falling back to single-threaded blit\n");
		}
	}

	if (g_stab_sys_get_fd)
		ret = g_stab_sys_get_fd(&g_stab_vpe_port, &fd);
	else
		ret = -1;
	if (ret != 0)
		fd = -1;

	while (g_stab_running) {
		StabSysBufInfo_t curr_buf;
		StabSysBufHandle_t curr_handle = 0;
		StabIveImage_t curr_img;
		int meas_dx = 0;
		int meas_dy = 0;
		float gyro_x = 0.0f;
		float gyro_y = 0.0f;
		float gyro_z = 0.0f;
		uint32_t gyro_n;

		memset(&curr_buf, 0, sizeof(curr_buf));

		/* Quiesce handshake: when teardown raises g_stab_pause, park here
		 * — not inside any MI_SYS GetBuf/PutBuf — so star6e_stab_stop can
		 * safely DisablePort(0,1) without racing the drain on the port. */
		if (g_stab_pause) {
			g_stab_parked = 1;
			usleep(2000);
			continue;
		}
		g_stab_parked = 0;

		if (fd >= 0) {
			fd_set rfds;
			struct timeval tv;

			FD_ZERO(&rfds);
			FD_SET(fd, &rfds);
			tv.tv_sec = 0;
			tv.tv_usec = 50000;
			ret = select(fd + 1, &rfds, NULL, NULL, &tv);
			if (ret <= 0 || !FD_ISSET(fd, &rfds))
				continue;
		}

		ret = g_stab_sys_out_get_buf(&g_stab_vpe_port,
			&curr_buf, &curr_handle);
		if (ret != 0) {
			if (fd < 0)
				usleep(1000);
			continue;
		}

		if (curr_buf.eBufType != STAB_E_BUFDATA_FRAME ||
		    !curr_buf.stFrameData.phyAddr[0] ||
		    !curr_buf.stFrameData.pVirAddr[0]) {
			g_stab_sys_out_put_buf(curr_handle);
			continue;
		}

		if (star6e_stab_make_center_y_crop(&curr_img, &curr_buf,
		    g_det.crop, g_det.crop) != 0) {
			g_stab_sys_out_put_buf(curr_handle);
			continue;
		}

		/* Fill mode (threaded): copy Y into the sw_detect ring so the port0
		 * buffer can be released by the blit thread without invalidating the
		 * IVE reference frame.  Re-point curr_img at the sw copy. */
		if (g_stab_fill_mode && sw_detect_ok) {
			star6e_stab_copy_y_to_sw(&sw_detect[sw_idx], &curr_buf,
				g_det.crop, g_det.crop);
			curr_img.apu8VirAddr[0]  = sw_detect[sw_idx].apu8VirAddr[0];
			curr_img.aphyPhyAddr[0]  = sw_detect[sw_idx].aphyPhyAddr[0];
			curr_img.azu16Stride[0]  = sw_detect[sw_idx].azu16Stride[0];
		}

		clock_gettime(CLOCK_MONOTONIC, &curr_ts);

		if (!have_prev) {
			/* HW mode: VENC is hardware-fed from port0; the detector tap
			 * (port1) only seeds the reference frame here.  Fill mode: hand
			 * the first frame to the compose with acc=(0,0). */
			if (g_stab_fill_mode) {
				if (sw_detect_ok && g_stab_blit_active)
					ret = star6e_stab_fill_queue_blit(&curr_handle,
						&curr_buf, 0, 0);
				else
					ret = star6e_stab_send_frame_to_venc_fill(
						&curr_buf, 0, 0);
				if (ret != 0 && (dbg_frame++ % 60) == 0)
					fprintf(stderr, "[waybeam] stab-fill first venc "
						"send failed ret=0x%x\n", ret);
			}
			prev_handle = curr_handle;   /* 0 if ownership transferred */
			prev_img = curr_img;
			prev_ts = curr_ts;
			if (g_stab_fill_mode && sw_detect_ok)
				sw_idx ^= 1;
			have_prev = 1;
			continue;
		}

		/* Estimate motion only every STAB_DETECT_EVERY-th frame — the
		 * detector is the CPU bottleneck.  prev_img/prev_handle are held
		 * across skipped frames, so each detect spans the full interval
		 * since the last detect (no motion lost).  The crop blit + VENC
		 * send below run on every frame regardless, keeping output at
		 * sensor fps. */
		loop_n++;
		if ((loop_n % STAB_DETECT_EVERY) == 0) {
			/* Gyro samples for this interval (frame-aligned, optical-only
			 * today — see star6e_stab_estimate_shift for the fusion seam). */
			gyro_n = star6e_stab_gyro_window(prev_ts, curr_ts,
				&gyro_x, &gyro_y, &gyro_z);

			ret = star6e_stab_estimate_shift(&prev_img, &curr_img, &dx, &dy,
				&meas_dx, &meas_dy);

			if (ret == 0) {
				int max_x;
				int max_y;

					max_x = star6e_stab_max_off_x();
					max_y = star6e_stab_max_off_y();

					/* Unified trajectory smoother (Kalman) for BOTH "stab" and
					 * "stab-fill" — the SINGLE control law in framing_kalman.c,
					 * so identical settings give identical return-to-centre feel;
					 * the only per-preset difference is the emit step below (HW
					 * crop reprogram vs software compose).  The applied
					 * compensation is the high-pass component (facc - estimate):
					 * fast shake removed, slow pans pass through, the estimate
					 * eases the offset back to centre on its own.  Pause (D13)
					 * glides home via the recenter ramp; g_stab_recenter_period
					 * sets the rate (0 = default 30-frame ramp). */
					{
						uint32_t tau = g_stab_recenter_period ?
							g_stab_recenter_period : 30;
						framing_kalman_step(&g_stab_kalman, meas_dx, meas_dy,
							g_stab_paused, tau, max_x, max_y,
							&acc_x, &acc_y);
						g_stab_osd_meas_x = (int)meas_dx;
						g_stab_osd_meas_y = (int)meas_dy;
						g_stab_osd_acc_x = acc_x;
						g_stab_osd_acc_y = acc_y;
					}
				dbg_frame++;
				if ((dbg_frame % 120) == 0)
					fprintf(stderr, "[waybeam] stab tick %d: meas=(%d,%d) "
						"acc=(%d,%d) max=(%d,%d) pan=(%d,%d) "
						"kalman(q=%.4f,r=%.2f) paused=%d "
						"gyro_n=%u gyro=(%.3f,%.3f,%.3f)\n",
						dbg_frame, meas_dx, meas_dy,
						acc_x, acc_y, max_x, max_y,
						g_stab_pan_x_mil, g_stab_pan_y_mil,
						g_stab_kalman.q, g_stab_kalman.r, g_stab_paused,
						gyro_n, gyro_x, gyro_y, gyro_z);
			} else {
				if ((dbg_frame++ % 120) == 0)
					fprintf(stderr, "[waybeam] stab Shift_Detector "
						"ret=0x%x\n", (unsigned)ret);
			}

			/* Emit the new offset, then rotate prev to this frame.  HW mode:
			 * reprogram port0's hardware crop (VENC is fed by the bind).
			 * Fill mode: BufFillPa strips + BufBlitPa content into a fresh
			 * VENC input (threaded if the blit thread is up, else inline). */
			if (g_stab_fill_mode && sw_detect_ok && g_stab_blit_active) {
				ret = star6e_stab_fill_queue_blit(&curr_handle,
					&curr_buf, acc_x, acc_y);
				if (ret != 0 && (dbg_frame % 60) == 0)
					fprintf(stderr, "[waybeam] stab-fill queue blit "
						"failed ret=0x%x\n", ret);
			} else if (g_stab_fill_mode) {
				ret = star6e_stab_send_frame_to_venc_fill(&curr_buf,
					acc_x, acc_y);
				if (ret != 0 && (dbg_frame % 60) == 0)
					fprintf(stderr, "[waybeam] stab-fill venc send "
						"failed ret=0x%x\n", ret);
			} else {
				star6e_stab_apply_port_crop(acc_x, acc_y);
			}
			if (prev_handle)
				g_stab_sys_out_put_buf(prev_handle);
			prev_handle = curr_handle;   /* 0 if the blit thread owns it */
			prev_img = curr_img;
			prev_ts = curr_ts;
			if (g_stab_fill_mode && sw_detect_ok)
				sw_idx ^= 1;
		} else {
			/* Skipped-detect frame.  HW mode: nothing to emit (port0 holds
			 * the last crop, VENC is hardware-fed).  Fill mode: re-output
			 * with the current accumulator so VENC keeps receiving at sensor
			 * fps.  Keep prev as the last detected frame. */
			if (g_stab_fill_mode && sw_detect_ok && g_stab_blit_active) {
				ret = star6e_stab_fill_queue_blit(&curr_handle,
					&curr_buf, acc_x, acc_y);
			} else if (g_stab_fill_mode) {
				ret = star6e_stab_send_frame_to_venc_fill(&curr_buf,
					acc_x, acc_y);
				if (ret != 0 && (dbg_frame % 60) == 0)
					fprintf(stderr, "[waybeam] stab-fill venc send "
						"failed ret=0x%x\n", ret);
			}
			if (curr_handle)
				g_stab_sys_out_put_buf(curr_handle);
		}
	}

	if (prev_handle)
		g_stab_sys_out_put_buf(prev_handle);
	if (fd >= 0 && g_stab_sys_close_fd)
		g_stab_sys_close_fd(fd);
out:
	free(dx.apu8VirAddr[0]);
	free(dy.apu8VirAddr[0]);
	if (sw_detect[0].apu8VirAddr[0])
		free(sw_detect[0].apu8VirAddr[0]);
	if (sw_detect[1].apu8VirAddr[0])
		free(sw_detect[1].apu8VirAddr[0]);
	return NULL;
}


/* stab-fill port setup: VPE port0 SCL-downscales the full sensor input to the
 * configured encode dim (fixed for the run) and is MANUALLY drained by the
 * detector thread (no HW bind to VENC).  Per frame the thread composes a fresh
 * VENC input by BufFillPa-ing the four exposed black strips and BufBlitPa-ing
 * the in-bounds source content at a shifted sub-rect.  No port1 tap — the
 * detector reads the centre patch from the full port0 frame. */
static int star6e_stab_setup_ports_fill(Star6ePipelineState *state)
{
	MI_VPE_PortAttr_t port = {0};
	MI_SYS_ChnPort_t vpe0 = {
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0, .port = 0 };
	MI_S32 ret;

	g_stab_tap_active = 0;
	g_stab_detector_enabled = 1;
	g_stab_venc_port = state->venc_port;
	state->bound_vpe_venc = 0;
	g_stab_pre_w = state->active_precrop.w ? state->active_precrop.w
		: g_stab_src_w;
	g_stab_pre_h = state->active_precrop.h ? state->active_precrop.h
		: g_stab_src_h;

	port.output.width = g_stab_enc_w;
	port.output.height = g_stab_enc_h;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;
	ret = MI_VPE_SetPortMode(0, 0, &port);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab-fill port0 SetPortMode "
			"%ux%u failed %d\n",
			g_stab_enc_w, g_stab_enc_h, (int)ret);
		return ret;
	}
	ret = MI_VPE_EnablePort(0, 0);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab-fill port0 EnablePort "
			"failed %d\n", (int)ret);
		return ret;
	}
	ret = MI_SYS_SetChnOutputPortDepth(&vpe0, 4, 8);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab-fill port0 "
			"SetChnOutputPortDepth failed %d\n", (int)ret);
		return ret;
	}
	g_stab_vpe_port = vpe0;   /* detector drains port0 directly */
	fprintf(stderr, "[waybeam] stab-fill: manual-drain mode (precrop %ux%u, "
		"encode %ux%u, max_off %dx%d)\n",
		g_stab_pre_w, g_stab_pre_h, g_stab_enc_w, g_stab_enc_h,
		star6e_stab_max_off_x(), star6e_stab_max_off_y());
	return 0;
}

/* Set up VPE output ports for stabilization (single HW-crop path).
 *
 * port0 outputs the encoded dim and is hardware-bound to VENC ch0; the
 * detector reads a tiny port1 256x256 centre tap and reprograms port0's
 * SetPortCrop per detect (no software blit; port0 tears down via the standard
 * bound-port path, state->bound_vpe_venc=1).  Sets g_stab_tap_active=1,
 * g_stab_vpe_port=port1.
 *
 * If this BSP rejects the port1 tap, port0 is still bound to VENC and held at
 * a static centre crop (g_stab_tap_active=0, no detector, no shake
 * compensation) — the stream stays up at the configured framing.  Returns 0 on
 * success, <0 only on a hard port0/bind failure the caller must abort on. */
static int star6e_stab_setup_ports(Star6ePipelineState *state,
	uint32_t bind_src_fps, uint32_t bind_dst_fps)
{
	MI_VPE_PortAttr_t port = {0};
	MI_SYS_ChnPort_t vpe0 = {
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0, .port = 0 };
	MI_SYS_ChnPort_t vpe1 = {
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0, .port = 1 };
	i6_common_rect rect;
	int port1_enabled = 0;
	int tap_ok;
	MI_S32 ret;

	if (g_stab_fill_mode) {
		(void)bind_src_fps;
		(void)bind_dst_fps;
		return star6e_stab_setup_ports_fill(state);
	}

	g_stab_tap_active = 0;
	g_stab_venc_port = state->venc_port;
	state->bound_vpe_venc = 0;

	/* VPE channel input (precrop) dim — the SetPortCrop coordinate domain.
	 * Set before any star6e_stab_apply_port_crop / img_to_pre call below. */
	g_stab_pre_w = state->active_precrop.w ? state->active_precrop.w
		: g_stab_src_w;
	g_stab_pre_h = state->active_precrop.h ? state->active_precrop.h
		: g_stab_src_h;

	/* port0: encoded-dim output (the SCL will crop the src into this). */
	port.output.width = g_stab_enc_w;
	port.output.height = g_stab_enc_h;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;
	ret = MI_VPE_SetPortMode(0, 0, &port);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab port0 SetPortMode %ux%u "
			"failed %d\n", g_stab_enc_w, g_stab_enc_h, (int)ret);
		return ret;   /* even the legacy path needs a working port0 */
	}
	ret = MI_VPE_EnablePort(0, 0);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab port0 EnablePort failed "
			"%d\n", (int)ret);
		return ret;
	}

	/* port1: 256x256 centre detector tap — the BSP-dependent step. */
	memset(&port, 0, sizeof(port));
	port.output.width = g_det.crop;
	port.output.height = g_det.crop;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;
	ret = MI_VPE_SetPortMode(0, 1, &port);
	if (ret == 0) {
		ret = MI_VPE_EnablePort(0, 1);
		if (ret == 0)
			port1_enabled = 1;
	}
	if (ret == 0) {
		/* Detector tap: crop a centre window of the precrop input that maps
		 * to a 256x256 IMAGE-domain patch, scaled to the 256x256 port1
		 * output.  The detector then measures motion in image pixels —
		 * identical to the legacy centre crop — so the accumulator and
		 * clamps stay mode-agnostic (image domain). */
		int dw = star6e_stab_img_to_pre_x(g_det.crop) & ~1;
		int dh = star6e_stab_img_to_pre_y(g_det.crop) & ~1;
		int dcx, dcy;
		if (dw < 2) dw = 2;
		if (dh < 2) dh = 2;
		if (dw > (int)g_stab_pre_w) dw = (int)g_stab_pre_w & ~1;
		if (dh > (int)g_stab_pre_h) dh = (int)g_stab_pre_h & ~1;
		dcx = (((int)g_stab_pre_w - dw) / 2) & ~1;
		dcy = (((int)g_stab_pre_h - dh) / 2) & ~1;
		if (dcx < 0) dcx = 0;
		if (dcy < 0) dcy = 0;
		rect.x = (unsigned short)dcx;
		rect.y = (unsigned short)dcy;
		rect.width = (unsigned short)dw;
		rect.height = (unsigned short)dh;
		ret = MI_VPE_SetPortCrop(0, 1, &rect);
	}
	tap_ok = (ret == 0 && port1_enabled);

	/* Bind port0 -> VENC zero-copy regardless of the tap: the stabilized (or,
	 * if the tap is unavailable, static centre) crop is fed by this bind. */
	ret = MI_SYS_BindChnPort2(&state->vpe_port, &state->venc_port,
		bind_src_fps, bind_dst_fps, I6_SYS_LINK_FRAMEBASE, 0);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab port0->VENC bind failed "
			"%d\n", (int)ret);
		if (port1_enabled)
			MI_VPE_DisablePort(0, 1);
		return ret;
	}
	MI_SYS_SetChnOutputPortDepth(&state->venc_port, 1, 3);
	state->bound_vpe_venc = 1;
	star6e_stab_apply_port_crop(0, 0);   /* centre the initial crop window */

	if (tap_ok) {
		MI_SYS_SetChnOutputPortDepth(&vpe1, 2, 4);
		g_stab_vpe_port = vpe1;          /* detector drains port1 */
		g_stab_tap_active = 1;
		fprintf(stderr, "[waybeam] stab: HW-crop mode (port0->VENC bind, "
			"port1 %dx%d detector tap)\n",
			g_det.crop, g_det.crop);
	} else {
		/* No detector tap on this BSP: keep the bound static centre crop,
		 * no shake compensation (never the legacy manual drain). */
		if (port1_enabled)
			MI_VPE_DisablePort(0, 1);
		g_stab_vpe_port = vpe0;
		g_stab_tap_active = 0;
		fprintf(stderr, "[waybeam] WARNING: stab port1 tap unavailable "
			"(%d); static centre crop, no stabilization\n", (int)ret);
	}
	return 0;
}

static int star6e_stab_start(void)
{
	MI_S32 ret;

	g_stab_pause = 0;
	g_stab_parked = 0;

	/* HW-crop degrade path: port0 is bound at a static centre crop, no port1
	 * tap — run no detector thread.  Fill mode has no port1 tap (it drains
	 * port0), so this guard must not fire there. */
	if (!g_stab_fill_mode && !g_stab_tap_active) {
		fprintf(stderr, "[waybeam] stab: detector disabled "
			"(static centre crop, no shake compensation)\n");
		return 0;
	}

	if (star6e_stab_load_sys_extra_symbols() != 0) {
		fprintf(stderr, "[waybeam] ERROR: stab cannot resolve required "
			"MI_SYS symbols\n");
		return -1;
	}

	g_stab_ive_lib = dlopen("libmi_ive.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!g_stab_ive_lib)
		g_stab_ive_lib = dlopen("libive.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!g_stab_ive_lib) {
		fprintf(stderr, "[waybeam] ERROR: stab cannot dlopen "
			"libmi_ive.so / libive.so\n");
		return -1;
	}

	g_stab_ive_create = (stab_ive_create_fn_t)dlsym(g_stab_ive_lib,
		"MI_IVE_Create");
	g_stab_ive_destroy = (stab_ive_destroy_fn_t)dlsym(g_stab_ive_lib,
		"MI_IVE_Destroy");
	g_stab_ive_shift = (stab_ive_shift_fn_t)dlsym(g_stab_ive_lib,
		"MI_IVE_Shift_Detector");
	if (!g_stab_ive_create || !g_stab_ive_destroy || !g_stab_ive_shift) {
		fprintf(stderr, "[waybeam] ERROR: stab missing IVE symbols\n");
		dlclose(g_stab_ive_lib);
		g_stab_ive_lib = NULL;
		return -1;
	}

	g_stab_ive_handle = 0;
	ret = g_stab_ive_create(g_stab_ive_handle);
	if (ret != 0) {
		fprintf(stderr, "[waybeam] ERROR: MI_IVE_Create failed %d\n",
			(int)ret);
		dlclose(g_stab_ive_lib);
		g_stab_ive_lib = NULL;
		return ret;
	}
	g_stab_ive_created = 1;

	g_stab_running = 1;

	/* Fill-mode only: spawn the blit thread that consumes the single-slot
	 * queue.  Resetting the slot defends against a previous start that exited
	 * dirty.  If the spawn fails the detector falls back to the inline compose
	 * (the g_stab_blit_active gate). */
	g_stab_blit_active = 0;
	memset(&g_stab_blit_slot, 0, sizeof(g_stab_blit_slot));
	if (g_stab_fill_mode) {
		if (pthread_create(&g_stab_blit_thread, NULL,
		    star6e_stab_blit_thread_main, NULL) == 0) {
			g_stab_blit_active = 1;
			/* Under stab-fill the blit thread composes the output
			 * frame and feeds it to VENC, so it sits on the
			 * frame-delivery critical path alongside the encoder
			 * thread.  Give it the same elevated SCHED_FIFO priority
			 * (VENC_RT_PRIO, default 50, clamped 1..80; see
			 * star6e_runner_run) so it is not preempted mid-compose
			 * by SCHED_OTHER work — otherwise the scheduling jitter
			 * the encoder fix removes would re-enter via this stage. */
			{
				int rt_prio = 50;
				const char *env = getenv("VENC_RT_PRIO");
				if (env && *env) {
					int v = atoi(env);
					if (v < 1)
						v = 1;
					else if (v > 80)
						v = 80;
					rt_prio = v;
				}
				struct sched_param sp;
				sp.sched_priority = rt_prio;
				if (pthread_setschedparam(g_stab_blit_thread,
				    SCHED_FIFO, &sp) != 0)
					fprintf(stderr, "[waybeam] note: stab-fill "
						"blit RT priority unavailable\n");
			}
			fprintf(stderr, "[waybeam] stab-fill: blit thread spawned "
				"(IVE/blit pipelined)\n");
		} else {
			fprintf(stderr, "[waybeam] WARNING: stab-fill blit thread "
				"spawn failed — running blit inline\n");
		}
	}

	if (pthread_create(&g_stab_thread, NULL,
	    star6e_stab_thread_main, NULL) != 0) {
		g_stab_running = 0;
		if (g_stab_blit_active) {
			pthread_mutex_lock(&g_stab_blit_lock);
			pthread_cond_broadcast(&g_stab_blit_cond_in);
			pthread_cond_broadcast(&g_stab_blit_cond_out);
			pthread_mutex_unlock(&g_stab_blit_lock);
			pthread_join(g_stab_blit_thread, NULL);
			g_stab_blit_active = 0;
		}
		g_stab_ive_destroy(g_stab_ive_handle);
		g_stab_ive_created = 0;
		dlclose(g_stab_ive_lib);
		g_stab_ive_lib = NULL;
		fprintf(stderr, "[waybeam] ERROR: stab thread spawn failed\n");
		return -1;
	}

	fprintf(stderr, "[waybeam] stab: src=%ux%u out=%ux%u crop=%u%% "
		"kalman(q=%.4f,r=%.2f) pauseGlide=%u\n",
		g_stab_src_w, g_stab_src_h,
		g_stab_enc_w, g_stab_enc_h, g_stab_crop_percent,
		g_stab_kalman.q, g_stab_kalman.r, g_stab_recenter_period);
	return 0;
}

static void star6e_stab_stop(void)
{
	if (g_stab_running) {
		/* Stop the detector thread and JOIN it before touching port1.
		 * In HW detect mode the thread keeps one port1 buffer checked out
		 * across iterations (prev_handle, the IVE reference frame); on loop
		 * exit it returns that buffer while the port is still enabled (safe)
		 * and can start no further IVE read.  Disabling port1 with the
		 * thread still alive — even "parked" — could free the ring under an
		 * in-flight IVE read: _MI_SYS_MMU_Callback Status=0x2 ClientId=0x15
		 * IsWrite=0 stormed the MMU into a hardware-watchdog reset.  The old
		 * 100ms park-spin was not a real barrier under the every-frame
		 * "high" detector load (it survived a few respawn cycles, then lost
		 * the race and reset the board).  pthread_join IS the barrier; only
		 * then disable the tap.  port0 stays bound feeding VENC throughout,
		 * so the VPE channel never backs up and there is no heavy manual
		 * drain to wedge [vpe0_P0_MAIN]. */
		g_stab_running = 0;
		/* Wake any threads waiting on the blit queue before joining the
		 * detector — otherwise the detector could be blocked inside
		 * star6e_stab_fill_queue_blit (waiting for slot empty) and never see
		 * g_stab_running=0.  Broadcast both condvars; join the detector
		 * first, then the blit thread (which drains its leftover slot). */
		if (g_stab_blit_active) {
			pthread_mutex_lock(&g_stab_blit_lock);
			pthread_cond_broadcast(&g_stab_blit_cond_in);
			pthread_cond_broadcast(&g_stab_blit_cond_out);
			pthread_mutex_unlock(&g_stab_blit_lock);
		}
		pthread_join(g_stab_thread, NULL);
		memset(&g_stab_thread, 0, sizeof(g_stab_thread));
		if (g_stab_blit_active) {
			pthread_join(g_stab_blit_thread, NULL);
			memset(&g_stab_blit_thread, 0, sizeof(g_stab_blit_thread));
			g_stab_blit_active = 0;
		}
		g_stab_pause = 0;
		g_stab_parked = 0;
	}
	/* Disable the tap whether or not the detector thread ran (the start-fail
	 * path sets up the tap but never spawns the thread).  Always after the
	 * join above, never before — see the MMU-safety note. */
	if (g_stab_tap_active) {
		MI_VPE_DisablePort(0, 1);
		g_stab_tap_active = 0;
	}
	if (g_stab_ive_created) {
		g_stab_ive_destroy(g_stab_ive_handle);
		g_stab_ive_created = 0;
	}
	if (g_stab_ive_lib) {
		dlclose(g_stab_ive_lib);
		g_stab_ive_lib = NULL;
	}
}

/* Shared gate: a stab preset is active when stab_crop_pct is in range.  The two
 * presets ("stab", "stab-fill") then disambiguate on the framing string so the
 * registry selects exactly one. */
static int star6e_stab_crop_active(const VencConfig *vcfg)
{
	return vcfg && vcfg->video0.stab_crop_pct >= 50 &&
		vcfg->video0.stab_crop_pct <= 100;
}

static int star6e_stab_enabled(const VencConfig *vcfg)
{
	return star6e_stab_crop_active(vcfg) &&
		strcmp(vcfg->video0.framing, "stab") == 0;
}

static int star6e_stab_fill_enabled(const VencConfig *vcfg)
{
	return star6e_stab_crop_active(vcfg) &&
		strcmp(vcfg->video0.framing, "stab-fill") == 0;
}

/* Stab live bypass (D13 software ramp), mode-agnostic.  Returns 0 if applied,
 * -1 if no stab detector is running (the detector glides the offset to centre
 * on the next tick — no HW rebind).  Works for both framing=stab (the crop
 * window glides home) and framing=stab-fill (the floating image glides home). */
static int star6e_stab_set_paused(int paused)
{
	if (!g_stab_running)
		return -1;
	pthread_mutex_lock(&g_stab_path_lock);
	g_stab_paused = paused ? 1 : 0;
	pthread_mutex_unlock(&g_stab_path_lock);
	fprintf(stderr, "[waybeam] stab%s: %s (software ramp)\n",
		g_stab_fill_mode ? "-fill" : "",
		paused ? "PAUSED — gliding to centre" : "RESUMED — compose active");
	return 0;
}

/* Vtable set_live for both stab presets.  Carries the live pauseStab toggle
 * from the API layer without the pipeline knowing its semantics. */
static int star6e_stab_set_live(const char *key, const char *val)
{
	if (!key)
		return -1;
	if (strcmp(key, "pause") == 0 || strcmp(key, "pauseStab") == 0 ||
	    strcmp(key, "video0.pause_stab") == 0) {
		int paused = val && (val[0] == '1' || val[0] == 't' ||
			val[0] == 'T' || val[0] == 'y' || val[0] == 'Y');
		return star6e_stab_set_paused(paused);
	}
	return -1;
}

/* Stabilization path: constrain the AE meter to the stabilized crop window.
 * The crop is taken from the VPE *output* (g_stab_src), not from precrop, so
 * the meter fraction is g_stab_enc / g_stab_src (= the crop %) regardless of
 * any sensor→stream downscale — unlike the zoom path's image_width/precrop.
 * Centre follows the live pan; the small per-frame stabilization offset is
 * intentionally not tracked (a few px; the meter window need not chase it). */
static void star6e_stab_apply_ae_crop(void)
{
	Star6eAeCropRect r;
	double fw, fh, x, y, rx, ry;

	if (g_stab_src_w == 0 || g_stab_src_h == 0)
		return;
	fw = (double)g_stab_enc_w / (double)g_stab_src_w;
	fh = (double)g_stab_enc_h / (double)g_stab_src_h;
	if (!isfinite(fw) || !isfinite(fh) ||
	    fw <= 0.0 || fw >= 1.0 || fh <= 0.0 || fh >= 1.0)
		return;  /* no crop → leave AE full-frame */

	x = (double)g_stab_pan_x_mil / 1000.0;
	y = (double)g_stab_pan_y_mil / 1000.0;
	rx = x - fw * 0.5;
	ry = y - fh * 0.5;
	if (rx < 0.0) rx = 0.0;
	if (ry < 0.0) ry = 0.0;
	if (rx + fw > 1.0) rx = 1.0 - fw;
	if (ry + fh > 1.0) ry = 1.0 - fh;

	memset(&r, 0, sizeof(r));
	r.crop_x = (uint16_t)(rx * 1023.0 + 0.5);
	r.crop_y = (uint16_t)(ry * 1023.0 + 0.5);
	r.crop_w = (uint16_t)(fw * 1023.0 + 0.5);
	r.crop_h = (uint16_t)(fh * 1023.0 + 0.5);
	if (r.crop_w == 0) r.crop_w = 1;
	if (r.crop_h == 0) r.crop_h = 1;
	if (r.crop_x + r.crop_w > 1023)
		r.crop_x = (uint16_t)(1023 - r.crop_w);
	if (r.crop_y + r.crop_h > 1023)
		r.crop_y = (uint16_t)(1023 - r.crop_h);

	star6e_emit_ae_crop(&r);
}

static int star6e_stab_active(void)
{
	return g_stab_running;
}

/* Compute stabilization geometry from vcfg and the source dims; clamp the
 * source to <=1920x1080 (above it the VPE+VENC path cannot sustain 60/90/120
 * fps with stab on) and return the encode dims the pipeline sizes VENC to.
 * Stabilization is always CENTERED (0.5/0.5) so the accumulator has symmetric
 * headroom — zoomX/zoomY are the zoom-mode pan and do not apply here. */
static int star6e_stab_prepare(const VencConfig *vcfg, uint32_t src_w,
	uint32_t src_h, uint32_t *enc_w, uint32_t *enc_h)
{
	/* Resolve the shared detector level before the tap/IVE images are sized.
	 * "auto" -> "high" on Star6E (its historical full config). */
	g_det = framing_stab_detector_params(vcfg->video0.stab_accuracy, "high");
	if (src_w > 1920 || src_h > 1080) {
		uint64_t rw = (uint64_t)1920 * 1000 / src_w;
		uint64_t rh = (uint64_t)1080 * 1000 / src_h;
		uint64_t r = rw < rh ? rw : rh;
		uint32_t nw = (uint32_t)((uint64_t)src_w * r / 1000) & ~1u;
		uint32_t nh = (uint32_t)((uint64_t)src_h * r / 1000) & ~1u;
		if (nw < 2) nw = 2;
		if (nh < 2) nh = 2;
		fprintf(stderr, "[waybeam] WARNING: video0.framing '%s': source "
			"%ux%u exceeds 1920x1080; clamping to %ux%u to preserve fps\n",
			vcfg->video0.framing, src_w, src_h, nw, nh);
		src_w = nw;
		src_h = nh;
	}
	star6e_stab_configure(src_w, src_h,
		vcfg->video0.stab_crop_pct,
		0, 0,                       /* derive enc dims from crop_pct (shrink) */
		vcfg->video0.stab_recenter_speed,
		vcfg->video0.fps,
		0.5, 0.5,
		vcfg->video0.stab_kalman_q,
		vcfg->video0.stab_kalman_r);
	*enc_w = g_stab_enc_w;
	*enc_h = g_stab_enc_h;
	return 0;
}

/* stab-fill prepare: the encode resolution stays at the (clamped) full-frame
 * size — the floating image is the full sensor frame SCL-downscaled to encode
 * dim, and stab_crop_pct bounds the shift/black-border budget rather than
 * shrinking the output.  Clamp to <=1920x1080 like "stab" for fps. */
static int star6e_stab_fill_prepare(const VencConfig *vcfg, uint32_t src_w,
	uint32_t src_h, uint32_t *enc_w, uint32_t *enc_h)
{
	/* stab-fill shares the same detector; resolve its level too. */
	g_det = framing_stab_detector_params(vcfg->video0.stab_accuracy, "high");
	if (src_w > 1920 || src_h > 1080) {
		uint64_t rw = (uint64_t)1920 * 1000 / src_w;
		uint64_t rh = (uint64_t)1080 * 1000 / src_h;
		uint64_t r = rw < rh ? rw : rh;
		uint32_t nw = (uint32_t)((uint64_t)src_w * r / 1000) & ~1u;
		uint32_t nh = (uint32_t)((uint64_t)src_h * r / 1000) & ~1u;
		if (nw < 2) nw = 2;
		if (nh < 2) nh = 2;
		fprintf(stderr, "[waybeam] WARNING: video0.framing '%s': source "
			"%ux%u exceeds 1920x1080; clamping to %ux%u to preserve fps\n",
			vcfg->video0.framing, src_w, src_h, nw, nh);
		src_w = nw;
		src_h = nh;
	}
	/* enc dims = full (clamped) frame: no shrink, the shift fills with black. */
	star6e_stab_configure(src_w, src_h,
		vcfg->video0.stab_crop_pct,
		src_w, src_h,               /* explicit encode dims (no shrink) */
		vcfg->video0.stab_recenter_speed,
		vcfg->video0.fps,
		0.5, 0.5,
		vcfg->video0.stab_kalman_q,
		vcfg->video0.stab_kalman_r);
	g_stab_fill_mode = 1;           /* opt in after configure cleared it */
	*enc_w = g_stab_enc_w;
	*enc_h = g_stab_enc_h;
	return 0;
}

/* Debug-OSD snapshot: last detector measurement + Kalman correction (both in
 * stab pixels — SCL-input px for "stab", encode px for "stab-fill").  Returns
 * 0 when no stab thread is running (row hidden).  Called from the pipeline's
 * 1 Hz debug-OSD refresh (see star6e_framing_stab.h). */
int star6e_framing_stab_osd_status(int *acc_x, int *acc_y,
	int *meas_x, int *meas_y, int *paused, int *fill)
{
	if (!g_stab_running)
		return 0;
	if (acc_x)  *acc_x = g_stab_osd_acc_x;
	if (acc_y)  *acc_y = g_stab_osd_acc_y;
	if (meas_x) *meas_x = g_stab_osd_meas_x;
	if (meas_y) *meas_y = g_stab_osd_meas_y;
	if (paused) *paused = g_stab_paused;
	if (fill)   *fill = g_stab_fill_mode;
	return 1;
}

const FramingModule star6e_framing_stab = {
	.preset_name   = "stab",
	.uses_vpe_port1 = true,   /* HW-crop stab: 256x256 motion tap on port1 */
	.enabled       = star6e_stab_enabled,
	.prepare       = star6e_stab_prepare,
	.setup_ports   = star6e_stab_setup_ports,
	.start         = star6e_stab_start,
	.stop          = star6e_stab_stop,
	.apply_ae_crop = star6e_stab_apply_ae_crop,
	.set_pan       = star6e_stab_set_pan,
	.active        = star6e_stab_active,
	.set_live      = star6e_stab_set_live,
};

/* "stab-fill" shares setup_ports/start/stop/apply_ae_crop/set_live with "stab" —
 * those branch internally on g_stab_fill_mode (set by this preset's prepare).
 * Both presets carry the live pauseStab toggle (D13 software ramp). */
const FramingModule star6e_framing_stab_fill = {
	.preset_name   = "stab-fill",
	.uses_vpe_port1 = false,  /* drains the full port0 frame; no port1 tap */
	.enabled       = star6e_stab_fill_enabled,
	.prepare       = star6e_stab_fill_prepare,
	.setup_ports   = star6e_stab_setup_ports,
	.start         = star6e_stab_start,
	.stop          = star6e_stab_stop,
	.apply_ae_crop = star6e_stab_apply_ae_crop,
	.set_pan       = star6e_stab_set_pan,
	.active        = star6e_stab_active,
	.set_live      = star6e_stab_set_live,
};
