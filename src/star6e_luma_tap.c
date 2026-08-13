/*
 * star6e_luma_tap.c — read-only NV12 luma tap on VPE port1 (Star6E).
 *
 * Supplies overlay-free frames to a vision consumer.  MI_RGN composites per
 * scaler output port, and every overlay producer in the system (debug_osd here,
 * osd_render in waybeam-hub) attaches to port0 — which the H.265 encoder, the
 * MJPEG snapshot channel and the recorder all share 1:N.  port1 is a separate
 * scaler output and carries no overlay.
 *
 * Two rules shape this file, both learned from the retired /api/v1/snapshot.pgm
 * (PR #205):
 *
 *   1. The port is programmed once per bounded scan window, with a minimum
 *      open lifetime and reopen cooldown enforced by the daemon. snapshot.pgm
 *      cycled Enable/Disable per HTTP request with neither guard; DisablePort
 *      raced an in-flight mhal buffer and jammed the VPE input FIFO. Window
 *      closure always joins the reader and drains the port first.
 *
 *   2. The reader thread drains EVERY frame at line rate and copies the luma
 *      plane out only when a grab is pending.  An enabled-but-undrained port is
 *      dangerous on this BSP (the port2 probe stalled port0 with no consumer),
 *      and a slow consumer must never sit between GetBuf and PutBuf.
 *
 * Teardown order is load-bearing: park the reader outside the GetBuf/PutBuf
 * window, join it, reset the output depth, THEN DisablePort.  Leaving the depth
 * registered makes the kernel SCL keep queueing port1 output tasks for a
 * consumer that no longer exists, and a successor process inherits a stale
 * queue whose fence never completes.
 */

#include "star6e_luma_tap.h"
#include "star6e.h"
#include "star6e_vpe_ports.h"
#include "timing.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LT_OWNER      "qr"
#define LT_PORT       1
#define LT_MAX_DIM    4096u
/* How long a freshly enabled port gets to deliver its first frame before we
 * conclude the SCL will not drive that geometry.  Generous: it is only ever
 * waited out on the failure path, and a healthy port answers in one frame. */
#define LT_FIRST_FRAME_MS 1000u
#define LT_WINDOW_MS_MIN 1000u
#define LT_WINDOW_MS_MAX 60000u
/* Decoder duty cycle.  After each attempt the supervisor idles for as long as
 * that attempt took, so scanning never occupies more than half of one core.
 *
 * This is not a nicety.  Measured on the 2-core Star6E: back-to-back cascades
 * at 1080x1080 peg the CPU at 100% and drag the encoder from 60 fps to 23 --
 * the pipeline's ISP, AWB and frame-shm threads are ordinary SCHED_OTHER and
 * lose to a decoder that never yields.  (Only the encoder thread is SCHED_FIFO,
 * so it alone survives.)  A window with a marker in view is unaffected either
 * way: it decodes on the first frame and closes. */
#define LT_DUTY_CYCLE_NUM 1u   /* idle = busy * NUM/DEN                    */
#define LT_DUTY_CYCLE_DEN 1u
#define LT_PACE_MAX_MS    1000u
/* Floor on the interval between two port opens.
 *
 * Cycling port1 per scan window is safe at human cadence -- 200/200 and 100/100
 * open/close cycles about a second apart, encoder live, zero wedge signatures.
 * It is NOT safe without a floor: a client looping /api/v1/qr/scan drives
 * open/close every ~150 ms (a window that finds its marker closes in ~85 ms),
 * and that is the regime that retired /api/v1/snapshot.pgm in #205 --
 * MI_VPE_DisablePort racing an in-flight mhal buffer, jamming the VPE input
 * FIFO, kernel-side and unrecoverable from userspace.  A bench box hung hard
 * under exactly that loop.
 *
 * So the daemon, not the client, owns the cycle rate.  500 ms still allows two
 * scans a second, far beyond any real use. */
#define LT_REOPEN_COOLDOWN_MS 500u
/* Floor on how long the port stays open once enabled.
 *
 * The reopen cooldown gates the OPEN edge.  This gates the CLOSE edge, and that
 * is the one that actually panics the kernel: MI_VPE_DisablePort landing on a
 * port that only just came up, while the SCL still has buffers in flight,
 * jams the VPE input FIFO -- the #205 snapshot.pgm failure.  A bench box took a
 * hard kernel panic (panic=20 auto-reboot) under a loop of
 * /api/v1/qr/scan immediately followed by /api/v1/qr/stop, which closes a
 * ~200 ms-old port over and over.
 *
 * Every open/close soak that ever came back clean -- 200/200 and 100/100 -- let
 * windows close NATURALLY, on decode or on deadline, having been open long
 * enough to reach steady state.  That is the safe regime, and this constant is
 * what makes every close land in it, including an operator-triggered one.
 *
 * /qr/stop therefore requests a close rather than forcing one: it pulls the
 * deadline in to the earliest safe instant and waits. */
#define LT_MIN_WINDOW_MS 750u
/* Refuse a tap whose luma plane alone would dominate a 64-128 MB target. */
#define LT_MAX_PIXELS (8u * 1024u * 1024u)

/* ---- local MI_SYS output-buffer mirror.  Layout copied from the proven
 * definition in star6e_ipu_yolo.c; only the fields we read are named. ------ */
typedef MI_U64 LtPhy;

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
	LtPhy phyAddr[3];
	MI_U32 u32Stride[3];
	MI_U32 u32BufSize;
	MI_U16 u16RingBufStartLine;
	MI_U16 u16RingBufRealTotalHeight;
	struct {
		int eType;
		union { MI_U32 u32GlobalGradient; } uIspInfo;
	} stFrameIspInfo;
	MI_U8 reserved_rect[16];
} LtFrameData_t;

typedef struct {
	MI_U64 u64Pts;
	MI_U64 u64SidebandMsg;
	int eBufType;
	MI_BOOL bEndOfStream;
	MI_BOOL bUsrBuf;
	MI_U32 u32SequenceNumber;
	MI_BOOL bDrop;
	union {
		LtFrameData_t stFrameData;
		MI_U8 reserved_union[512];
	};
	MI_U8 u8CusFlag;
} LtBufInfo_t;

typedef MI_S32 LtBufHandle_t;

#define LT_E_BUFDATA_FRAME 1

typedef MI_S32 (*lt_get_fd_fn)(MI_SYS_ChnPort_t *port, MI_S32 *fd);
typedef MI_S32 (*lt_close_fd_fn)(MI_S32 fd);
typedef MI_S32 (*lt_get_buf_fn)(MI_SYS_ChnPort_t *port, LtBufInfo_t *buf,
	LtBufHandle_t *handle);
typedef MI_S32 (*lt_put_buf_fn)(LtBufHandle_t handle);
typedef MI_S32 (*lt_mmap_fn)(MI_U64 phy, MI_U32 size, void **vir, MI_U8 cached);
typedef MI_S32 (*lt_munmap_fn)(void *vir, MI_U32 size);

struct Star6eLumaTap {
	int              running;        /* tap started (port owned)            */
	int              port_enabled;
	int              claimed;
	MI_SYS_ChnPort_t port;           /* {VPE,0,0,1}                         */
	uint32_t         port_w, port_h; /* geometry programmed on the port     */
	uint32_t         w, h;           /* latch geometry = centre square      */

	pthread_t             reader;
	int                   reader_started;
	volatile sig_atomic_t reader_run;

	/* Grab handshake.  `lock` guards grab_pending/latch_valid and publishes
	 * the latch; `api_lock` serializes concurrent HTTP callers so only one
	 * grab is ever outstanding. */
	pthread_mutex_t lock;
	pthread_cond_t  cond;
	pthread_mutex_t api_lock;
	int             grab_pending;
	int             latch_valid;
	uint8_t        *latch;           /* w*h, tightly packed, stride removed */
	/* Set while the supervisor is running a cascade over the latch.  The
	 * reader keeps DRAINING (that is never optional) but stops overwriting
	 * the latch, and /qr/tap.pgm reports busy rather than handing out a
	 * frame that is being mutated.  Cheaper than a second W*H buffer, and
	 * the decode is what the window is for. */
	int             decoding;
	/* When the port was last released.  Guarded by `lock`, not `ctl_lock`:
	 * lt_port_close() runs both with and without ctl_lock held depending on
	 * the path, so ctl_lock could not be taken there without deadlocking. */
	uint64_t        last_close_us;

	lt_get_fd_fn   get_fd;
	lt_close_fd_fn close_fd;
	lt_get_buf_fn  get_buf;
	lt_put_buf_fn  put_buf;
	lt_mmap_fn     mmap_fn;
	lt_munmap_fn   munmap_fn;
	void          *sys_h;

	/* Settings captured at pipeline bring-up so open() can program the port
	 * later without a VencConfig in hand. */
	int      cfg_enabled;
	uint32_t cfg_w, cfg_h;
	uint32_t cfg_window_ms;

	/* Scan-window supervisor.  This thread is the ONLY one that opens or
	 * closes the port, so no SDK port call is ever concurrent.  `ctl_lock`
	 * guards the deadline and the supervisor flags; it is deliberately a
	 * different lock from `lock` (the latch handshake) so a slow grab cannot
	 * delay a window ending. */
	pthread_t       super;
	pthread_mutex_t ctl_lock;
	pthread_cond_t  ctl_cond;
	int             super_running;
	uint64_t        deadline_us;      /* CLOCK_MONOTONIC */
	uint64_t        open_us;          /* when the port came up */
	uint32_t        window_ms;
	uint64_t        frames, grabs;    /* per-window counters */

	/* Decode result, guarded by ctl_lock.  Reset at the start of each
	 * window and retained after it closes. */
	uint32_t        attempts;
	int             decoded;
	char            payload[STAR6E_QR_PAYLOAD_MAX];
	char            stage[32];
	uint64_t        decode_us;
	uint64_t        last_us;
};

Star6eLumaTap *star6e_luma_tap_create(void)
{
	Star6eLumaTap *tap = calloc(1, sizeof(*tap));

	if (!tap)
		return NULL;
	if (pthread_mutex_init(&tap->lock, NULL) != 0)
		goto fail;
	if (pthread_cond_init(&tap->cond, NULL) != 0)
		goto fail_lock;
	if (pthread_mutex_init(&tap->api_lock, NULL) != 0)
		goto fail_cond;
	if (pthread_mutex_init(&tap->ctl_lock, NULL) != 0)
		goto fail_api_lock;
	if (pthread_cond_init(&tap->ctl_cond, NULL) != 0)
		goto fail_ctl_lock;
	return tap;

fail_ctl_lock:
	pthread_mutex_destroy(&tap->ctl_lock);
fail_api_lock:
	pthread_mutex_destroy(&tap->api_lock);
fail_cond:
	pthread_cond_destroy(&tap->cond);
fail_lock:
	pthread_mutex_destroy(&tap->lock);
fail:
	free(tap);
	return NULL;
}

void star6e_luma_tap_destroy(Star6eLumaTap *tap)
{
	if (!tap)
		return;
	star6e_luma_tap_stop(tap);
	if (tap->sys_h)
		dlclose(tap->sys_h);
	pthread_cond_destroy(&tap->ctl_cond);
	pthread_mutex_destroy(&tap->ctl_lock);
	pthread_mutex_destroy(&tap->api_lock);
	pthread_cond_destroy(&tap->cond);
	pthread_mutex_destroy(&tap->lock);
	free(tap);
}

/* Port lifecycle, called ONLY from the supervisor thread or from scan()/stop()
 * under tap->ctl_lock — never concurrently. */
static int  lt_port_open(Star6eLumaTap *tap);
static void lt_port_close(Star6eLumaTap *tap);

static int lt_load_sys_symbols(Star6eLumaTap *tap)
{
	if (!tap->sys_h) {
		tap->sys_h = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
		if (!tap->sys_h)
			return -1;
	}
	tap->get_fd = (lt_get_fd_fn)dlsym(tap->sys_h, "MI_SYS_GetFd");
	tap->close_fd = (lt_close_fd_fn)dlsym(tap->sys_h, "MI_SYS_CloseFd");
	tap->get_buf = (lt_get_buf_fn)dlsym(tap->sys_h,
		"MI_SYS_ChnOutputPortGetBuf");
	tap->put_buf = (lt_put_buf_fn)dlsym(tap->sys_h,
		"MI_SYS_ChnOutputPortPutBuf");
	tap->mmap_fn = (lt_mmap_fn)dlsym(tap->sys_h, "MI_SYS_Mmap");
	tap->munmap_fn = (lt_munmap_fn)dlsym(tap->sys_h, "MI_SYS_Munmap");
	if (!tap->get_buf || !tap->put_buf || !tap->mmap_fn || !tap->munmap_fn)
		return -1;
	return 0;
}

/* Copy the luma plane of one frame into the latch, removing stride.  Runs on
 * the reader thread between GetBuf and PutBuf, so it is kept to a mapped
 * row-wise copy and nothing else. */
static int lt_latch_frame(Star6eLumaTap *tap, const LtFrameData_t *fr)
{
	uint32_t stride = fr->u32Stride[0] ? fr->u32Stride[0] : fr->u16Width;
	uint32_t fw = fr->u16Width, fh = fr->u16Height;
	uint32_t side = tap->w;            /* latch is square: tap->w == tap->h */
	uint32_t x0, y0;
	MI_U64 phy = fr->phyAddr[0];
	void *vir = NULL;

	if (!phy || fw == 0 || fh == 0 || side == 0)
		return -EIO;
	/* A frame smaller than the latch (shouldn't happen) is clamped rather
	 * than trusted. */
	if (side > fw) side = fw;
	if (side > fh) side = fh;

	/* Centre crop.  The SCL scales but does not crop, so taking the square
	 * here is what keeps the aspect undistorted — and it drops the outer
	 * frame where the fisheye is worst and where a marker never sits. */
	x0 = (fw - side) / 2;
	y0 = (fh - side) / 2;

	/* Map non-cached (flag 0) so reads see the latest DMA without an
	 * explicit invalidate.  The source is never written. */
	if (tap->mmap_fn(phy, stride * fh, &vir, 0) != 0 || !vir)
		return -EIO;

	{
		const uint8_t *src = (const uint8_t *)vir;
		for (uint32_t row = 0; row < side; ++row)
			memcpy(tap->latch + (size_t)row * tap->w,
			       src + (size_t)(y0 + row) * stride + x0, side);
	}
	tap->munmap_fn(vir, stride * fh);
	return 0;
}

static void *lt_reader_main(void *arg)
{
	Star6eLumaTap *tap = arg;
	MI_S32 fd = -1;
	if (tap->get_fd && tap->get_fd(&tap->port, &fd) != 0)
		fd = -1;

	while (tap->reader_run) {
		LtBufInfo_t buf;
		LtBufHandle_t handle = 0;
		int want;

		if (fd >= 0) {
			fd_set rfds;
			struct timeval tv;
			FD_ZERO(&rfds);
			FD_SET(fd, &rfds);
			tv.tv_sec = 0;
			tv.tv_usec = 50000;
			if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0 ||
			    !FD_ISSET(fd, &rfds))
				continue;
		}

		memset(&buf, 0, sizeof(buf));
		if (tap->get_buf(&tap->port, &buf, &handle) != 0) {
			if (fd < 0)
				usleep(1000);
			continue;
		}

		if (buf.eBufType != LT_E_BUFDATA_FRAME ||
		    !buf.stFrameData.phyAddr[0]) {
			tap->put_buf(handle);
			continue;
		}

		__atomic_fetch_add(&tap->frames, 1, __ATOMIC_RELAXED);

		pthread_mutex_lock(&tap->lock);
		want = tap->grab_pending && !tap->decoding;
		pthread_mutex_unlock(&tap->lock);

		if (want) {
			int rc = lt_latch_frame(tap, &buf.stFrameData);
			__atomic_fetch_add(&tap->grabs, 1, __ATOMIC_RELAXED);
			pthread_mutex_lock(&tap->lock);
			tap->grab_pending = 0;
			tap->latch_valid = (rc == 0);
			pthread_cond_broadcast(&tap->cond);
			pthread_mutex_unlock(&tap->lock);
		}

		/* Unconditional: every frame is returned immediately, whether or
		 * not it was copied.  This is what keeps the port drained. */
		tap->put_buf(handle);
	}

	if (fd >= 0 && tap->close_fd)
		tap->close_fd(fd);
	return NULL;
}

/* Release the port: depth reset THEN disable, in that order.  Every path that
 * enabled the port comes through here so the two calls cannot drift apart. */
static void lt_port_teardown(Star6eLumaTap *tap)
{
	MI_S32 dret, rret;

	if (!tap->port_enabled)
		return;
	rret = MI_SYS_SetChnOutputPortDepth(&tap->port, 0, 0);
	dret = MI_VPE_DisablePort(0, LT_PORT);
	if (rret != 0 || dret != 0)
		fprintf(stderr, "[luma-tap] port1 teardown: depth_reset=%d "
			"disable=%d\n", (int)rret, (int)dret);
	tap->port_enabled = 0;
}

void star6e_luma_tap_configure(Star6eLumaTap *tap, const VencConfig *cfg,
	uint32_t main_w,
	uint32_t main_h)
{
	uint32_t w, h;

	if (!tap)
		return;
	tap->cfg_enabled = 0;
	if (!cfg || !cfg->qr.tap_enabled)
		return;

	w = cfg->qr.tap_width ? cfg->qr.tap_width : main_w;
	h = cfg->qr.tap_height ? cfg->qr.tap_height : main_h;
	/* Even dimensions: the tap is NV12 and a 4:2:0 chroma plane needs them,
	 * even though only luma is read. */
	w &= ~1u;
	h &= ~1u;
	if (w == 0 || h == 0 || w > LT_MAX_DIM || h > LT_MAX_DIM ||
	    (uint64_t)w * h > LT_MAX_PIXELS) {
		fprintf(stderr, "[luma-tap] refusing geometry %ux%u\n",
			cfg->qr.tap_width, cfg->qr.tap_height);
		return;
	}
	tap->cfg_w = w;
	tap->cfg_h = h;
	tap->cfg_window_ms = cfg->qr.window_ms;
	tap->cfg_enabled = 1;
	fprintf(stderr, "[luma-tap] armed: port %ux%u, centre square %u "
		"(opens per scan window)\n", w, h, w < h ? w : h);
}

static int lt_port_open(Star6eLumaTap *tap)
{
	MI_VPE_PortAttr_t port;
	MI_S32 ret;
	uint32_t w = tap->cfg_w, h = tap->cfg_h;

	if (!tap->cfg_enabled)
		return -ENODEV;
	if (tap->running)
		return 0;

	/* Lowest-priority claimant: a scan window cannot evict stab or detect. */
	if (star6e_vpe_port1_claim(LT_OWNER) != 0) {
		fprintf(stderr, "[luma-tap] open refused — VPE port1 held by "
			"'%s'\n", star6e_vpe_port1_owner());
		return -EBUSY;
	}
	tap->claimed = 1;

	if (lt_load_sys_symbols(tap) != 0) {
		fprintf(stderr, "[luma-tap] MI_SYS symbols unavailable\n");
		goto fail;
	}
	tap->port_w = w;
	tap->port_h = h;
	/* Latch is the CENTRE SQUARE of the port output.  The SCL scales but does
	 * not crop, so the square has to be taken during the copy — which also
	 * discards the outer frame where fisheye distortion is worst and where a
	 * marker never sits.  See lt_latch_frame(). */
	tap->w = tap->h = (w < h) ? w : h;

	tap->latch = malloc((size_t)tap->w * tap->h);
	if (!tap->latch) {
		fprintf(stderr, "[luma-tap] latch alloc %ux%u failed\n",
			tap->w, tap->h);
		goto fail;
	}

	tap->port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0,
		.port = LT_PORT };

	/* Full-frame scale to the tap geometry.  MI_VPE_SetPortCrop is
	 * deliberately never called: it is sticky on i6e and a rect left behind
	 * poisons a later detect run on the same port. */
	memset(&port, 0, sizeof(port));
	port.output.width = (unsigned short)w;
	port.output.height = (unsigned short)h;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;
	ret = MI_VPE_SetPortMode(0, LT_PORT, &port);
	if (ret == 0) {
		ret = MI_VPE_EnablePort(0, LT_PORT);
		if (ret == 0)
			tap->port_enabled = 1;
	}
	if (!tap->port_enabled) {
		fprintf(stderr, "[luma-tap] VPE port1 %ux%u unavailable (%d)\n",
			w, h, (int)ret);
		goto fail;
	}
	/* Unbound output port needs a user frame queue or GetBuf sees 0 frames. */
	MI_SYS_SetChnOutputPortDepth(&tap->port, 2, 4);

	tap->grab_pending = 0;
	tap->latch_valid = 0;
	__atomic_store_n(&tap->frames, 0, __ATOMIC_RELAXED);
	/* The first-frame probe below counts from here. */
	tap->reader_run = 1;
	if (pthread_create(&tap->reader, NULL, lt_reader_main, tap) != 0) {
		fprintf(stderr, "[luma-tap] reader thread spawn failed\n");
		tap->reader_run = 0;
		goto fail;
	}
	tap->reader_started = 1;
	tap->running = 1;
	tap->open_us = wb_monotonic_us();

	/* Prove the port actually produces before reporting the window open.
	 *
	 * MI_VPE_SetPortMode and EnablePort both return success for geometries
	 * the SCL will not in fact drive -- measured: a 160x90 port claims
	 * port1, enables cleanly, and then delivers ZERO frames forever.  The
	 * scan window looked healthy, /qr/tap.pgm timed out, and detect and
	 * stab were locked out of port1 for the whole window for nothing.
	 *
	 * Deliberately a frame probe rather than a geometry rule: the SDK's real
	 * constraint is not documented and cannot be derived safely (the working
	 * 1920x1080 is not 16-aligned in height, so the obvious rule is wrong).
	 * Watching for a frame catches every cause -- too small, misaligned, or
	 * whatever else -- and costs one frame time on the success path. */
	{
		unsigned waited = 0;

		while (__atomic_load_n(&tap->frames, __ATOMIC_RELAXED) == 0 &&
		       waited < LT_FIRST_FRAME_MS) {
			usleep(10000);
			waited += 10;
		}
		if (__atomic_load_n(&tap->frames, __ATOMIC_RELAXED) == 0) {
			fprintf(stderr, "[luma-tap] VPE port1 %ux%u enabled but "
				"produced no frame in %u ms — geometry not "
				"driveable by the SCL; releasing port1\n",
				w, h, LT_FIRST_FRAME_MS);
			/* Full teardown: the reader is running, so the fail
			 * label's shorter sequence is not enough. */
			lt_port_close(tap);
			return -EIO;
		}
	}

	fprintf(stderr, "[luma-tap] VPE port1 tap up: port %ux%u, square %ux%u\n",
		w, h, tap->w, tap->h);
	return 0;

fail:
	lt_port_teardown(tap);
	free(tap->latch);
	tap->latch = NULL;
	if (tap->claimed) {
		star6e_vpe_port1_release(LT_OWNER);
		tap->claimed = 0;
	}
	return -EIO;
}

/* True once the window's budget is spent.  /qr/stop expresses itself by moving
 * the deadline to 0, so there is no separate stop flag to keep in step. */
static int lt_expired(Star6eLumaTap *tap)
{
	int done;

	pthread_mutex_lock(&tap->ctl_lock);
	done = wb_monotonic_us() >= tap->deadline_us;
	pthread_mutex_unlock(&tap->ctl_lock);
	return done;
}

/* Poll the helper against the window deadline so /qr/stop can abort a decode
 * promptly instead of waiting for the whole attempt. */
static int lt_scan_abort(void *user)
{
	return lt_expired(user);
}

static int lt_write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = write(fd, p, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/* Keep the third-party decoder out of the daemon: feed one frozen luma frame
 * to the separately built qr_decode helper.  The helper is optional; without
 * it scan windows still provide /qr/tap.pgm for an external consumer. */
static int lt_decode_external(Star6eLumaTap *tap, char *payload, size_t cap)
{
	char path[] = "/tmp/waybeam_qr_XXXXXX";
	char hdr[32];
	char child_out[STAR6E_QR_PAYLOAD_MAX + 2];
	int pipefd[2] = { -1, -1 };
	int fd = -1, status = 0;
	pid_t pid;
	ssize_t n;

	fd = mkstemp(path);
	if (fd < 0)
		return -1;
	n = snprintf(hdr, sizeof(hdr), "P5\n%u %u\n255\n", tap->w, tap->h);
	if (n <= 0 || lt_write_all(fd, hdr, (size_t)n) != 0 ||
	    lt_write_all(fd, tap->latch, (size_t)tap->w * tap->h) != 0) {
		close(fd);
		unlink(path);
		return -1;
	}
	if (close(fd) != 0 || pipe(pipefd) != 0) {
		unlink(path);
		return -1;
	}
	fd = -1;
	pid = fork();
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execl("/usr/bin/qr_decode", "qr_decode", path, (char *)NULL);
		_exit(127);
	}
	close(pipefd[1]);
	pipefd[1] = -1;
	if (pid < 0) {
		close(pipefd[0]);
		unlink(path);
		return -1;
	}
	while (waitpid(pid, &status, WNOHANG) == 0) {
		if (lt_scan_abort(tap)) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			break;
		}
		usleep(20000);
	}
	n = read(pipefd[0], child_out, sizeof(child_out) - 1);
	close(pipefd[0]);
	unlink(path);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || n <= 0)
		return 0;
	child_out[n] = '\0';
	child_out[strcspn(child_out, "\r\n")] = '\0';
	snprintf(payload, cap, "%s", child_out);
	return payload[0] != '\0';
}

/* Wait for the reader to place a fresh frame in the latch.  Returns 0 on
 * success.  The reader only latches when a grab is pending, so this is also
 * what paces capture to the decoder: one frame per attempt, not one per
 * vsync. */
static int lt_await_frame(Star6eLumaTap *tap, uint32_t timeout_ms)
{
	struct timespec deadline;
	int rc = 0;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += (time_t)(timeout_ms / 1000u);
	deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec += 1;
		deadline.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&tap->lock);
	tap->latch_valid = 0;
	tap->grab_pending = 1;
	while (!tap->latch_valid && tap->running) {
		if (pthread_cond_timedwait(&tap->cond, &tap->lock, &deadline) != 0)
			break;
	}
	if (!tap->latch_valid)
		rc = tap->running ? -ETIMEDOUT : -ENODEV;
	else
		tap->decoding = 1;   /* freeze the latch for the cascade */
	tap->grab_pending = 0;
	pthread_mutex_unlock(&tap->lock);
	return rc;
}

/* Idle for `ms`, waking early if the window ends.  Used to hold the cascade to
 * its duty cycle without making /qr/stop wait it out. */
static void lt_pace(Star6eLumaTap *tap, uint32_t ms)
{
	struct timespec ts;

	if (ms == 0)
		return;
	if (ms > LT_PACE_MAX_MS)
		ms = LT_PACE_MAX_MS;

	pthread_mutex_lock(&tap->ctl_lock);
	if (wb_monotonic_us() < tap->deadline_us) {
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += (time_t)(ms / 1000u);
		ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec += 1;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&tap->ctl_cond, &tap->ctl_lock, &ts);
	}
	pthread_mutex_unlock(&tap->ctl_lock);
}

/* Supervisor: owns the port for exactly one scan window, and drives the decode.
 *
 * One thread does both deliberately.  The port lifecycle rule -- only ever
 * opened and closed here -- is what removes the whole concurrency race class
 * that retired snapshot.pgm, and a separate decode thread would need a second
 * handshake with this one for no gain.
 *
 * Waits are against a CLOCK_MONOTONIC deadline rather than an absolute
 * wall-clock time, so a clock step cannot strand an open window. */
static void *lt_super_main(void *arg)
{
	Star6eLumaTap *tap = arg;

	/* Scanning is best-effort work behind a live video pipeline: it must
	 * lose every scheduling contest with the ISP, AWB and frame-shm threads,
	 * which are ordinary SCHED_OTHER and would otherwise be starved by a
	 * decoder that runs flat out.  Per-thread nice (Linux extends
	 * PRIO_PROCESS to a tid) rather than a policy change, so this stays a
	 * hint and can never deadlock the pipeline waiting on us. */
	if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10) != 0)
		fprintf(stderr, "[luma-tap] could not lower scan thread priority\n");

	while (!lt_expired(tap)) {
		char payload[STAR6E_QR_PAYLOAD_MAX];
		uint64_t started;
		int ok;

		if (lt_await_frame(tap, 500) != 0)
			continue;   /* re-tests the deadline at the top */

		payload[0] = '\0';
		started = wb_monotonic_us();
		ok = lt_decode_external(tap, payload, sizeof(payload));

		pthread_mutex_lock(&tap->lock);
		tap->decoding = 0;
		pthread_mutex_unlock(&tap->lock);

		pthread_mutex_lock(&tap->ctl_lock);
		tap->attempts++;
		tap->last_us = wb_monotonic_us() - started;
		if (ok > 0) {
			size_t n = strlen(payload);

			if (n >= sizeof(tap->payload))
				n = sizeof(tap->payload) - 1;
			/* The envelope already restricts this to the QR
			 * alphanumeric set, but scrub anyway: the payload is
			 * whatever someone held in front of the camera, and it
			 * is about to be pasted into JSON. */
			for (size_t i = 0; i < n; i++) {
				unsigned char c = payload[i];

				tap->payload[i] = (c >= 0x20 && c < 0x7F &&
						c != '"' && c != '\\')
					? (char)c : '?';
			}
			tap->payload[n] = '\0';
			snprintf(tap->stage, sizeof(tap->stage), "qr_decode");
			tap->decode_us = tap->last_us;
			tap->decoded = 1;
			/* A window exists to find one code.  Ending it here
			 * hands port1 back to detect/stab seconds earlier than
			 * waiting out the budget would.  lt_port_close() holds
			 * the port for its minimum lifetime regardless. */
			tap->deadline_us = 0;
		}
		pthread_mutex_unlock(&tap->ctl_lock);

		if (ok > 0) {
			fprintf(stderr, "[luma-tap] decoded \"%s\" at stage=%s "
				"in %llu ms (attempt %u)\n", tap->payload, tap->stage,
				(unsigned long long)(tap->decode_us / 1000),
				tap->attempts);
			break;
		}

		/* Hold the duty cycle.  Idling for as long as the attempt took
		 * keeps scanning under half a core whatever the geometry: an
		 * 85 ms decode barely pauses, a 431 ms full cascade at
		 * 1080x1080 pauses 431 ms. */
		lt_pace(tap, (uint32_t)((tap->last_us / 1000) *
			LT_DUTY_CYCLE_NUM / LT_DUTY_CYCLE_DEN));
	}

	/* Clear before the port goes away: lt_port_close() frees the latch, and
	 * a stale `decoding` would wedge the next window's reader. */
	pthread_mutex_lock(&tap->lock);
	tap->decoding = 0;
	pthread_mutex_unlock(&tap->lock);

	lt_port_close(tap);

	pthread_mutex_lock(&tap->ctl_lock);
	tap->super_running = 0;
	pthread_cond_broadcast(&tap->ctl_cond);
	pthread_mutex_unlock(&tap->ctl_lock);
	return NULL;
}

int star6e_luma_tap_scan(Star6eLumaTap *tap, uint32_t window_ms)
{
	int rc = 0;

	if (!tap)
		return -ENODEV;
	if (!tap->cfg_enabled)
		return -ENODEV;
	if (window_ms == 0)
		window_ms = tap->cfg_window_ms;
	if (window_ms < LT_WINDOW_MS_MIN) window_ms = LT_WINDOW_MS_MIN;
	if (window_ms > LT_WINDOW_MS_MAX) window_ms = LT_WINDOW_MS_MAX;

	/* Rate-limit port cycling.  This is the one guard between a
	 * scan-in-a-loop client and the kernel VPE wedge -- see
	 * LT_REOPEN_COOLDOWN_MS.  Done BEFORE ctl_lock is taken: sleeping while
	 * holding it would stall /qr/status and /qr/stop for the cooldown.
	 *
	 * Racing an extend here is harmless: the worst outcome is a needless
	 * sleep, after which the locked section below takes the extend path. */
	{
		uint64_t last;

		pthread_mutex_lock(&tap->lock);
		last = tap->last_close_us;
		pthread_mutex_unlock(&tap->lock);

		if (last && !tap->running) {
			uint64_t since = wb_monotonic_us() - last;
			uint64_t need = (uint64_t)LT_REOPEN_COOLDOWN_MS * 1000;

			if (since < need)
				usleep((useconds_t)(need - since));
		}
	}

	pthread_mutex_lock(&tap->ctl_lock);

	if (tap->super_running) {
		/* Extend only.  Deliberately no port work: re-opening per scan
		 * request is what wedged snapshot.pgm. */
		tap->deadline_us = wb_monotonic_us() + (uint64_t)window_ms * 1000;
		tap->window_ms = window_ms;
		pthread_cond_broadcast(&tap->ctl_cond);
		pthread_mutex_unlock(&tap->ctl_lock);
		return 0;
	}

	/* Open on the caller's thread before the supervisor exists so the caller
	 * gets the real error (409/503) instead of a window that dies silently. */
	rc = lt_port_open(tap);
	if (rc != 0) {
		pthread_mutex_unlock(&tap->ctl_lock);
		return rc;
	}

	__atomic_store_n(&tap->frames, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&tap->grabs, 0, __ATOMIC_RELAXED);
	tap->attempts = 0;
	tap->decoded = 0;
	tap->payload[0] = '\0';
	tap->stage[0] = '\0';
	tap->decode_us = 0;
	tap->last_us = 0;
	tap->window_ms = window_ms;
	tap->deadline_us = wb_monotonic_us() + (uint64_t)window_ms * 1000;
	tap->super_running = 1;
	if (pthread_create(&tap->super, NULL, lt_super_main, tap) != 0) {
		tap->super_running = 0;
		pthread_mutex_unlock(&tap->ctl_lock);
		fprintf(stderr, "[luma-tap] supervisor spawn failed\n");
		lt_port_close(tap);
		return -EIO;
	}
	pthread_detach(tap->super);
	pthread_mutex_unlock(&tap->ctl_lock);
	fprintf(stderr, "[luma-tap] scan window %u ms open\n", window_ms);
	return 0;
}

void star6e_luma_tap_scan_stop(Star6eLumaTap *tap)
{
	if (!tap)
		return;
	pthread_mutex_lock(&tap->ctl_lock);
	if (!tap->super_running) {
		pthread_mutex_unlock(&tap->ctl_lock);
		/* No supervisor: close directly in case a window failed to spawn
		 * one after opening the port. */
		lt_port_close(tap);
		return;
	}
	/* Ending the window is just moving the deadline; lt_port_close() is what
	 * holds the port for its minimum lifetime, so a stop against a
	 * brand-new window blocks there rather than closing early. */
	tap->deadline_us = 0;
	pthread_cond_broadcast(&tap->ctl_cond);
	/* The supervisor is detached, so wait on the flag rather than joining —
	 * on return the port is closed and port1 is free. */
	while (tap->super_running)
		pthread_cond_wait(&tap->ctl_cond, &tap->ctl_lock);
	pthread_mutex_unlock(&tap->ctl_lock);
}

void star6e_luma_tap_status(Star6eLumaTap *tap, Star6eLumaTapStatus *out)
{
	uint64_t now;

	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (!tap)
		return;
	out->armed = tap->cfg_enabled;

	pthread_mutex_lock(&tap->ctl_lock);
	out->scanning = tap->super_running;
	out->window_ms = tap->window_ms;
	now = wb_monotonic_us();
	out->remaining_ms = (tap->super_running && tap->deadline_us > now)
		? (int64_t)((tap->deadline_us - now) / 1000) : 0;
	out->attempts = tap->attempts;
	out->decoded = tap->decoded;
	snprintf(out->payload, sizeof(out->payload), "%s", tap->payload);
	snprintf(out->stage, sizeof(out->stage), "%s", tap->stage);
	out->decode_us = tap->decode_us;
	out->last_us = tap->last_us;
	pthread_mutex_unlock(&tap->ctl_lock);

	out->width = tap->w ? tap->w : (tap->cfg_w < tap->cfg_h ? tap->cfg_w : tap->cfg_h);
	out->height = tap->h ? tap->h : out->width;
	out->frames = __atomic_load_n(&tap->frames, __ATOMIC_RELAXED);
	out->grabs = __atomic_load_n(&tap->grabs, __ATOMIC_RELAXED);
	star6e_vpe_port1_owner_copy(out->port1_owner,
		sizeof(out->port1_owner));
}

/* Drain whatever the port still holds, from the CLOSING thread, after the
 * reader has been joined.  Disabling a port that still has queued buffers is
 * what races an in-flight mhal buffer — the failure that retired snapshot.pgm.
 * Bounded so a misbehaving port cannot spin here forever. */
static void lt_drain_quiescent(Star6eLumaTap *tap)
{
	int empty_polls = 0, drained = 0;

	if (!tap->port_enabled || !tap->get_buf || !tap->put_buf)
		return;
	/* Two consecutive empty GetBufs, or 64 buffers, whichever comes first. */
	while (empty_polls < 2 && drained < 64) {
		LtBufInfo_t buf;
		LtBufHandle_t handle = 0;

		memset(&buf, 0, sizeof(buf));
		if (tap->get_buf(&tap->port, &buf, &handle) != 0) {
			empty_polls++;
			usleep(2000);
			continue;
		}
		empty_polls = 0;
		drained++;
		tap->put_buf(handle);
	}
	if (drained)
		fprintf(stderr, "[luma-tap] drained %d buffered frame(s) before "
			"disable\n", drained);
}

/* Live close: the encoder keeps running, so this is the risky ordering.  Stop
 * the reader and JOIN it first — the loop tests reader_run at the top, so the
 * thread can only exit outside a GetBuf/PutBuf pair — then drain, then depth
 * reset, then disable. */
static void lt_port_close(Star6eLumaTap *tap)
{
	if (!tap->running && !tap->port_enabled && !tap->claimed)
		return;

	/* Never disable a port that only just came up.  MI_VPE_DisablePort
	 * landing on an in-flight mhal buffer jams the VPE input FIFO (#205);
	 * two bench boxes took hard kernel panics from exactly that, via
	 * /api/v1/qr/scan followed immediately by /api/v1/qr/stop.
	 *
	 * The wait lives here, at the single choke point every close path
	 * funnels through -- deadline, /qr/stop, decode-triggered early close,
	 * out-of-memory exit, failure unwind -- so no caller can bypass it.  An
	 * earlier version floored the deadline instead, which the decode path
	 * silently skipped by breaking out of the supervisor loop.
	 *
	 * It also has to come BEFORE the reader is parked: the port must keep
	 * being drained while we wait, or the wait itself creates the
	 * enabled-but-undrained state that is dangerous on this BSP. */
	if (tap->port_enabled) {
		uint64_t now = wb_monotonic_us();
		uint64_t safe = tap->open_us + (uint64_t)LT_MIN_WINDOW_MS * 1000;

		if (tap->open_us && now < safe)
			usleep((useconds_t)(safe - now));
	}

	tap->running = 0;

	if (tap->reader_started) {
		tap->reader_run = 0;
		pthread_mutex_lock(&tap->lock);
		tap->grab_pending = 0;
		tap->latch_valid = 0;
		pthread_cond_broadcast(&tap->cond);
		pthread_mutex_unlock(&tap->lock);
		pthread_join(tap->reader, NULL);
		tap->reader_started = 0;
	}

	lt_drain_quiescent(tap);
	lt_port_teardown(tap);

	pthread_mutex_lock(&tap->lock);
	free(tap->latch);
	tap->latch = NULL;
	tap->latch_valid = 0;
	tap->last_close_us = wb_monotonic_us();
	pthread_mutex_unlock(&tap->lock);

	if (tap->claimed) {
		star6e_vpe_port1_release(LT_OWNER);
		tap->claimed = 0;
	}
	tap->w = tap->h = 0;
	tap->port_w = tap->port_h = 0;
}

void star6e_luma_tap_stop(Star6eLumaTap *tap)
{
	if (!tap)
		return;
	/* Pipeline teardown: end any open window (which closes the port on the
	 * supervisor thread and waits for it), then disarm so a later scan
	 * cannot resurrect the tap against a torn-down graph. */
	star6e_luma_tap_scan_stop(tap);
	tap->cfg_enabled = 0;
	tap->cfg_w = tap->cfg_h = 0;
}

int star6e_luma_tap_running(const Star6eLumaTap *tap)
{
	return tap && tap->running;
}

int star6e_luma_tap_grab_pgm(Star6eLumaTap *tap, uint8_t **out_buf,
	size_t *out_len,
	uint32_t timeout_ms)
{
	struct timespec deadline;
	char hdr[32];
	int hlen;
	size_t total;
	uint8_t *out;
	int rc = 0;

	if (!tap || !out_buf || !out_len)
		return -EINVAL;
	*out_buf = NULL;
	*out_len = 0;
	if (!tap->running)
		return -ENODEV;
	if (timeout_ms == 0)
		timeout_ms = 1000;

	/* One outstanding grab at a time: the latch is single-buffered. */
	pthread_mutex_lock(&tap->api_lock);

	/* The decoder helper owns the latch for its duration. Refusing here is better
	 * than blocking an httpd worker for a second and a half, or handing
	 * back a frame the reader is free to overwrite. */
	pthread_mutex_lock(&tap->lock);
	if (tap->decoding) {
		pthread_mutex_unlock(&tap->lock);
		pthread_mutex_unlock(&tap->api_lock);
		return -EBUSY;
	}
	pthread_mutex_unlock(&tap->lock);

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += (time_t)(timeout_ms / 1000u);
	deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec += 1;
		deadline.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&tap->lock);
	tap->latch_valid = 0;
	tap->grab_pending = 1;
	while (!tap->latch_valid && tap->running) {
		if (pthread_cond_timedwait(&tap->cond, &tap->lock, &deadline) != 0)
			break;
	}
	if (!tap->latch_valid)
		rc = tap->running ? -ETIMEDOUT : -ENODEV;
	if (rc != 0)
		tap->grab_pending = 0;
	pthread_mutex_unlock(&tap->lock);

	if (rc == 0) {
		hlen = snprintf(hdr, sizeof(hdr), "P5\n%u %u\n255\n", tap->w, tap->h);
		total = (size_t)hlen + (size_t)tap->w * tap->h;
		out = malloc(total);
		if (!out) {
			rc = -ENOMEM;
		} else {
			memcpy(out, hdr, (size_t)hlen);
			pthread_mutex_lock(&tap->lock);
			if (tap->latch && tap->latch_valid) {
				memcpy(out + hlen, tap->latch,
				       (size_t)tap->w * tap->h);
			} else {
				rc = -ENODEV;   /* torn down under us */
			}
			pthread_mutex_unlock(&tap->lock);
			if (rc == 0) {
				*out_buf = out;
				*out_len = total;
			} else {
				free(out);
			}
		}
	}

	pthread_mutex_unlock(&tap->api_lock);
	return rc;
}

void star6e_luma_tap_free(uint8_t *buf)
{
	free(buf);
}
