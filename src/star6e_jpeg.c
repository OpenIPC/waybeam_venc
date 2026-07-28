/* star6e_jpeg.c — Star6E (Infinity6E) MJPEG snapshot backend.
 *
 * Creates one dedicated VENC channel (default ch7) bound to the same
 * VPE output port the main H.265 channel taps.  Channel stays
 * idle (StartRecvPic off) between requests; on each capture we flip
 * StartRecvPic on, poll MI_VENC_Query for ready packs, drain one frame
 * via MI_VENC_GetStream, copy the bytes, ReleaseStream, then turn
 * StartRecvPic back off.  All MI_VENC calls go through the dlopen
 * dispatch macros set up in include/star6e.h.
 *
 * Channel-id 7 is well clear of ch0 (main) and ch1 (dual/recorder).
 * The bind survives across captures, so steady-state cost is one VENC
 * channel slot — no encoder CPU when StartRecvPic is off.
 */

#include "venc_jpeg.h"
#include "star6e.h"
#include "star6e_vpe_ports.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_PACKS_PER_JPEG 8   /* MJPEG can split a frame into APP/VDO/PIC packs */

static MI_SYS_ChnPort_t g_vpe_port;
static int g_have_vpe_port = 0;
static MI_VENC_CHN g_chn = -1;
static int g_bound = 0;
static int g_chn_created = 0;
static uint32_t g_quality = 80;
/* Configured snapshot geometry, kept for the grayscale port1 tap (which does
 * not go through the MJPEG channel and so cannot read it back from VENC). */
static uint32_t g_snap_w, g_snap_h;

void venc_jpeg_set_source(const void *vpe_port_opaque)
{
	if (!vpe_port_opaque) {
		g_have_vpe_port = 0;
		return;
	}
	g_vpe_port = *(const MI_SYS_ChnPort_t *)vpe_port_opaque;
	g_have_vpe_port = 1;
}

int venc_jpeg_backend_init(const VencJpegConfig *cfg)
{
	if (!cfg)
		return -EINVAL;
	if (!g_have_vpe_port) {
		fprintf(stderr, "[jpeg-star6e] no VPE source registered; "
			"call venc_jpeg_set_source() before init\n");
		return -ENODEV;
	}
	if (cfg->width == 0 || cfg->height == 0) {
		fprintf(stderr, "[jpeg-star6e] width/height must be non-zero "
			"(got %ux%u)\n", cfg->width, cfg->height);
		return -EINVAL;
	}

	uint32_t w = cfg->width, h = cfg->height;
	uint32_t q = cfg->quality ? cfg->quality : 80;
	if (q > 99) q = 99;
	if (q < 1) q = 1;
	g_quality = q;
	g_snap_w = w;
	g_snap_h = h;
	g_chn = (MI_VENC_CHN)cfg->channel;

	MI_VENC_ChnAttr_t attr = {0};
	attr.attrib.codec = I6_VENC_CODEC_MJPG;
	attr.attrib.mjpg.maxWidth = w;
	attr.attrib.mjpg.maxHeight = h;
	attr.attrib.mjpg.bufSize = w * h * 3 / 2;
	attr.attrib.mjpg.byFrame = 1;
	attr.attrib.mjpg.width = w;
	attr.attrib.mjpg.height = h;

	attr.rate.mode = I6_VENC_RATEMODE_MJPGQP;
	attr.rate.mjpgQp.fpsNum = 5;   /* low — we'll only ever pull on demand */
	attr.rate.mjpgQp.fpsDen = 1;
	attr.rate.mjpgQp.quality = q;

	MI_S32 ret = MI_VENC_CreateChn(g_chn, &attr);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-star6e] MI_VENC_CreateChn(%d) failed %d\n",
			(int)g_chn, ret);
		return -EIO;
	}
	g_chn_created = 1;

	MI_U32 venc_dev = 0;
	if (MI_VENC_GetChnDevid(g_chn, &venc_dev) != 0) {
		fprintf(stderr, "[jpeg-star6e] MI_VENC_GetChnDevid(%d) failed\n", (int)g_chn);
		MI_VENC_DestroyChn(g_chn);
		g_chn_created = 0;
		return -EIO;
	}
	MI_SYS_ChnPort_t jpeg_port = {
		.module  = I6_SYS_MOD_VENC,
		.device  = venc_dev,
		.channel = (unsigned)g_chn,
		.port    = 0,
	};

	/* Bind VPE output → JPEG VENC input.  1:N from VPE is supported
	 * (the dual-stream path uses the same pattern at the main channel's
	 * source port).  FRAMEBASE link mode + low destination fps — the
	 * SDK uses dstFps to throttle which frames make it into the JPEG
	 * channel; 5 fps matches the rate-control attr above. */
	ret = MI_SYS_BindChnPort2(&g_vpe_port, &jpeg_port, 30, 5,
		I6_SYS_LINK_FRAMEBASE, 0);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-star6e] BindChnPort2 VPE→JPEG-VENC failed %d\n", ret);
		MI_VENC_DestroyChn(g_chn);
		g_chn_created = 0;
		return -EIO;
	}
	g_bound = 1;

	fprintf(stderr, "[jpeg-star6e] init OK: chn=%d %ux%u q=%u\n",
		(int)g_chn, w, h, q);
	return 0;
}

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int venc_jpeg_backend_capture(uint8_t **out_buf, size_t *out_len,
	uint32_t timeout_ms)
{
	if (!out_buf || !out_len)
		return -EINVAL;
	*out_buf = NULL;
	*out_len = 0;
	if (!g_chn_created || !g_bound)
		return -ENODEV;
	if (timeout_ms == 0)
		timeout_ms = 1500;

	MI_S32 ret = MI_VENC_StartRecvPic(g_chn);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-star6e] StartRecvPic failed %d\n", ret);
		return -EIO;
	}

	int rc = 0;
	int64_t deadline = now_ms() + (int64_t)timeout_ms;
	MI_VENC_Stream_t stream = {0};
	MI_VENC_Pack_t packs[MAX_PACKS_PER_JPEG] = {0};

	/* Wait for at least one pending pack.  Query is cheap and returns
	 * immediately; sleep 5 ms between polls so we don't burn CPU. */
	MI_VENC_Stat_t stat = {0};
	for (;;) {
		if (MI_VENC_Query(g_chn, &stat) == 0 && stat.curPacks > 0)
			break;
		if (now_ms() >= deadline) {
			rc = -ETIMEDOUT;
			goto stop;
		}
		usleep(5000);
	}

	uint32_t n = stat.curPacks;
	if (n > MAX_PACKS_PER_JPEG) n = MAX_PACKS_PER_JPEG;
	stream.count = n;
	stream.packet = packs;

	ret = MI_VENC_GetStream(g_chn, &stream, 200);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-star6e] GetStream failed %d\n", ret);
		rc = -EIO;
		goto stop;
	}
	if (stream.count == 0) {
		fprintf(stderr, "[jpeg-star6e] GetStream returned 0 packs\n");
		MI_VENC_ReleaseStream(g_chn, &stream);
		rc = -EIO;
		goto stop;
	}

	/* Concatenate packs into a single JPEG blob. */
	size_t total = 0;
	for (uint32_t i = 0; i < stream.count; ++i)
		total += stream.packet[i].length;
	if (total == 0) {
		MI_VENC_ReleaseStream(g_chn, &stream);
		rc = -EIO;
		goto stop;
	}

	uint8_t *copy = malloc(total);
	if (!copy) {
		MI_VENC_ReleaseStream(g_chn, &stream);
		rc = -ENOMEM;
		goto stop;
	}

	size_t off = 0;
	for (uint32_t i = 0; i < stream.count; ++i) {
		memcpy(copy + off, stream.packet[i].data, stream.packet[i].length);
		off += stream.packet[i].length;
	}
	MI_VENC_ReleaseStream(g_chn, &stream);

	*out_buf = copy;
	*out_len = total;

stop:
	(void)MI_VENC_StopRecvPic(g_chn);
	return rc;
}

/* ── Grayscale (P5 PGM) capture ──────────────────────────────────────────
 *
 * The MJPEG path feeds NV12 frames straight into a hardware VENC channel, so
 * the host never sees raw pixels.  To hand a consumer (e.g. a boot-time QR
 * scan) plain grayscale we open a short-lived VPE **port1** tap and copy the
 * luma plane out of one uncompressed NV12 frame.
 *
 * WHY NOT port0 (the JPEG/encoder source): a user output depth may only be
 * registered on a port with NO downstream hardware bind.  port0 is bound to
 * the main H.265 VENC (star6e_pipeline.c) — and 1:N to the MJPEG channel —
 * so registering a user queue on it makes the kernel SCL run user output
 * tasks alongside the bind and the allocator trips a hard BUG in
 * _MI_SYS_IMPL_AllocBufDefaultPolicy (seen on the vpe0_P0_MAIN worker,
 * followed by an EnsureInputPortFifoEmpty stall on the live encode path and,
 * on some runs, a board reset).  The rule holds across the tree: every
 * working user drain — the stab detector tap, the NPU detector, stab-fill's
 * manual port0 drain — registers its depth on an UNBOUND port.
 *
 * port1 is the single second scaler output, so it is taken through the
 * star6e_vpe_ports arbiter: stab or the NPU detector own it for a whole run,
 * and a snapshot must lose that race rather than program the tap underneath
 * them (-EBUSY -> HTTP 409).  When free we program it, pull exactly one
 * frame, and hand it back — the encode path on port0 is never touched.
 *
 * Teardown order is load-bearing: reset the user depth BEFORE DisablePort,
 * on every exit path (see iy_port1_teardown in star6e_ipu_yolo.c — a depth
 * left registered leaves the kernel queueing output tasks for a consumer
 * that no longer exists, and a later process inherits a queue whose fence
 * never completes).  The single `out:` epilogue below exists for that. */

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
	MI_U64 phyAddr[3];
	MI_U32 u32Stride[3];
	MI_U32 u32BufSize;
	MI_U16 u16RingBufStartLine;
	MI_U16 u16RingBufRealTotalHeight;
	struct {
		int eType;
		union { MI_U32 u32GlobalGradient; } uIspInfo;
	} stFrameIspInfo;
	MI_U8 reserved_rect[16];
} GrayFrameData_t;

typedef struct {
	MI_U64 u64Pts;
	MI_U64 u64SidebandMsg;
	int eBufType;
	MI_BOOL bEndOfStream;
	MI_BOOL bUsrBuf;
	MI_U32 u32SequenceNumber;
	MI_BOOL bDrop;
	union {
		GrayFrameData_t stFrameData;
		MI_U8 reserved_union[512];
	};
	MI_U8 u8CusFlag;
} GrayBufInfo_t;

#define GRAY_E_BUFDATA_FRAME 1
#define GRAY_MAX_DIM         8192u   /* sanity clamp on frame geometry */
#define GRAY_TAP_MAX_DIM     1280u   /* long-side cap for the port1 tap */
#define GRAY_TAP_OWNER       "snapshot"   /* star6e_vpe_ports claim label */
/* The tap port.  There is no third scaler output to escape to on i6e: a
 * device probe of port2 (SetPortMode+EnablePort) did not fail cleanly — it
 * stalled port0 with "EnsureInputPortFifoEmpty ... no response in 1000ms" and
 * wedged the encode path, so port1 really is the only second output and the
 * arbiter below is the way to share it. */
#define GRAY_TAP_PORT        1

typedef MI_S32 (*gray_get_buf_fn)(MI_SYS_ChnPort_t *port, GrayBufInfo_t *buf,
	MI_S32 *handle);
typedef MI_S32 (*gray_put_buf_fn)(MI_S32 handle);
typedef MI_S32 (*gray_mmap_fn)(MI_U64 phy, MI_U32 size, void **vir, MI_U8 cache);
typedef MI_S32 (*gray_munmap_fn)(void *vir, MI_U32 size);

static gray_get_buf_fn g_gray_get_buf;
static gray_put_buf_fn g_gray_put_buf;
static gray_mmap_fn    g_gray_mmap;
static gray_munmap_fn  g_gray_munmap;

static int gray_load_syms(void)
{
	/* libmi_sys stays resident for the whole run; open once and cache. */
	static void *sys_h;
	if (g_gray_get_buf && g_gray_put_buf && g_gray_mmap)
		return 0;
	if (!sys_h) {
		sys_h = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
		if (!sys_h)
			return -1;
	}
	g_gray_get_buf = (gray_get_buf_fn)dlsym(sys_h,
		"MI_SYS_ChnOutputPortGetBuf");
	g_gray_put_buf = (gray_put_buf_fn)dlsym(sys_h,
		"MI_SYS_ChnOutputPortPutBuf");
	g_gray_mmap = (gray_mmap_fn)dlsym(sys_h, "MI_SYS_Mmap");
	g_gray_munmap = (gray_munmap_fn)dlsym(sys_h, "MI_SYS_Munmap");
	if (!g_gray_get_buf || !g_gray_put_buf || !g_gray_mmap)
		return -1;
	return 0;
}

/* Pack the Y plane of one NV12 frame into a freshly malloc'd P5 PGM blob
 * (header + tightly-packed rows, stride removed).  Returns 0 on success. */
static int gray_frame_to_pgm(const GrayFrameData_t *fr, uint8_t **out_buf,
	size_t *out_len)
{
	uint32_t w = fr->u16Width, h = fr->u16Height;
	uint32_t stride = fr->u32Stride[0] ? fr->u32Stride[0] : w;
	MI_U64 phy = fr->phyAddr[0];
	void *vir = NULL;

	if (w == 0 || h == 0 || w > GRAY_MAX_DIM || h > GRAY_MAX_DIM || !phy)
		return -EIO;

	/* Map the Y plane non-cached (flag 0) so reads see the latest DMA
	 * without a cache invalidate; the source stays untouched. */
	if (g_gray_mmap(phy, stride * h, &vir, 0) != 0 || !vir)
		return -EIO;

	char hdr[32];
	int hlen = snprintf(hdr, sizeof(hdr), "P5\n%u %u\n255\n", w, h);
	size_t total = (size_t)hlen + (size_t)w * h;
	uint8_t *out = malloc(total);
	if (!out) {
		g_gray_munmap(vir, stride * h);
		return -ENOMEM;
	}

	memcpy(out, hdr, (size_t)hlen);
	const uint8_t *src = (const uint8_t *)vir;
	uint8_t *dst = out + hlen;
	for (uint32_t row = 0; row < h; ++row)
		memcpy(dst + (size_t)row * w, src + (size_t)row * stride, w);

	g_gray_munmap(vir, stride * h);

	*out_buf = out;
	*out_len = total;
	return 0;
}

/* Geometry for the port1 tap.  Starts from the configured snapshot size (the
 * same dims the MJPEG channel uses) and caps the long side: port1 is the small
 * second scaler output, the consumers of this endpoint are code (QR/vision),
 * not viewers, and a capped frame keeps both the SCL request and the per-row
 * copy cheap.  PGM is self-describing, so a consumer reads the real dims from
 * the header rather than assuming the main-stream size. */
static void gray_tap_dims(uint32_t *out_w, uint32_t *out_h)
{
	uint32_t w = g_snap_w ? g_snap_w : 1280;
	uint32_t h = g_snap_h ? g_snap_h : 720;
	uint32_t longest = (w > h) ? w : h;

	if (longest > GRAY_TAP_MAX_DIM) {
		w = (uint32_t)((uint64_t)w * GRAY_TAP_MAX_DIM / longest);
		h = (uint32_t)((uint64_t)h * GRAY_TAP_MAX_DIM / longest);
	}
	w &= ~15u;            /* 16-align the width so the SCL stride is sane */
	h &= ~1u;             /* NV12 needs an even height */
	if (w < 64) w = 64;
	if (h < 64) h = 64;
	*out_w = w;
	*out_h = h;
}

/* Drain one frame off an already-programmed tap port.  Split out so the
 * caller's teardown epilogue stays the single exit for the port lifecycle. */
static int gray_drain_one(MI_SYS_ChnPort_t *tap, uint8_t **out_buf,
	size_t *out_len, uint32_t timeout_ms)
{
	int64_t deadline = now_ms() + (int64_t)timeout_ms;

	for (;;) {
		GrayBufInfo_t buf;
		MI_S32 handle = 0;

		memset(&buf, 0, sizeof(buf));
		if (g_gray_get_buf(tap, &buf, &handle) != 0) {
			if (now_ms() >= deadline)
				return -ETIMEDOUT;
			usleep(2000);
			continue;
		}
		if (buf.eBufType != GRAY_E_BUFDATA_FRAME ||
		    !buf.stFrameData.phyAddr[0]) {
			g_gray_put_buf(handle);
			if (now_ms() >= deadline)
				return -ETIMEDOUT;
			usleep(2000);
			continue;
		}
		{
			int rc = gray_frame_to_pgm(&buf.stFrameData, out_buf, out_len);
			g_gray_put_buf(handle);
			return rc;
		}
	}
}

int venc_jpeg_backend_capture_gray(uint8_t **out_buf, size_t *out_len,
	uint32_t timeout_ms)
{
	MI_VPE_PortAttr_t attr;
	MI_SYS_ChnPort_t tap;
	MI_VPE_CHANNEL vpe_chn;
	uint32_t tap_w = 0, tap_h = 0;
	int port_enabled = 0, depth_set = 0, claimed = 0;
	int rc;
	MI_S32 ret;

	if (!out_buf || !out_len)
		return -EINVAL;
	*out_buf = NULL;
	*out_len = 0;
	if (!g_have_vpe_port)
		return -ENODEV;
	if (gray_load_syms() != 0)
		return -EIO;
	if (timeout_ms == 0)
		timeout_ms = 1500;

	/* port1 has exactly one owner.  Losing to stab/detect is a normal
	 * outcome, not an error — the endpoint turns it into a 409. */
	if (star6e_vpe_port1_claim(GRAY_TAP_OWNER) != 0) {
		char owner[16];
		star6e_vpe_port1_owner_copy(owner, sizeof(owner));
		fprintf(stderr, "[jpeg-star6e] gray: VPE port1 tap busy (owner=%s)\n",
			owner[0] ? owner : "?");
		return -EBUSY;
	}
	claimed = 1;

	gray_tap_dims(&tap_w, &tap_h);
	vpe_chn = (MI_VPE_CHANNEL)g_vpe_port.channel;
	tap = g_vpe_port;
	tap.port = GRAY_TAP_PORT;

	memset(&attr, 0, sizeof(attr));
	attr.output.width  = (unsigned short)tap_w;
	attr.output.height = (unsigned short)tap_h;
	attr.pixFmt   = I6_PIXFMT_YUV420SP;
	attr.compress = I6_COMPR_NONE;

	ret = MI_VPE_SetPortMode(vpe_chn, GRAY_TAP_PORT, &attr);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-star6e] gray: port1 SetPortMode %ux%u failed "
			"%d\n", tap_w, tap_h, (int)ret);
		rc = -EIO;
		goto out;
	}
	ret = MI_VPE_EnablePort(vpe_chn, GRAY_TAP_PORT);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-star6e] gray: port1 EnablePort failed %d\n",
			(int)ret);
		rc = -EIO;
		goto out;
	}
	port_enabled = 1;

	/* Legal here and only here: port1 carries no downstream bind, so this
	 * user queue is the port's only consumer. */
	ret = MI_SYS_SetChnOutputPortDepth(&tap, 2, 4);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-star6e] gray: port1 SetChnOutputPortDepth "
			"failed %d\n", (int)ret);
		rc = -EIO;
		goto out;
	}
	depth_set = 1;

	rc = gray_drain_one(&tap, out_buf, out_len, timeout_ms);

out:
	/* Depth reset BEFORE DisablePort, always — see the header comment. */
	if (depth_set)
		(void)MI_SYS_SetChnOutputPortDepth(&tap, 0, 0);
	if (port_enabled)
		(void)MI_VPE_DisablePort(vpe_chn, GRAY_TAP_PORT);
	if (claimed)
		star6e_vpe_port1_release(GRAY_TAP_OWNER);
	return rc;
}

int venc_jpeg_backend_set_quality(uint32_t q)
{
	if (!g_chn_created)
		return -ENODEV;
	if (q == 0) q = 1;
	if (q > 99) q = 99;

	MI_VENC_ChnAttr_t attr = {0};
	MI_S32 gret = MI_VENC_GetChnAttr(g_chn, &attr);
	if (gret != 0) {
		fprintf(stderr,
			"[jpeg-star6e] GetChnAttr(%d) failed %d during live "
			"quality update\n", (int)g_chn, gret);
		return -EIO;
	}
	attr.rate.mjpgQp.quality = q;
	MI_S32 sret = MI_VENC_SetChnAttr(g_chn, &attr);
	if (sret != 0) {
		fprintf(stderr,
			"[jpeg-star6e] SetChnAttr(q=%u) failed %d\n", q, sret);
		return -EIO;
	}
	g_quality = q;
	return 0;
}

void venc_jpeg_backend_shutdown(void)
{
	if (g_bound) {
		MI_U32 venc_dev = 0;
		(void)MI_VENC_GetChnDevid(g_chn, &venc_dev);
		MI_SYS_ChnPort_t jpeg_port = {
			.module  = I6_SYS_MOD_VENC,
			.device  = venc_dev,
			.channel = (unsigned)g_chn,
			.port    = 0,
		};
		(void)MI_SYS_UnBindChnPort(&g_vpe_port, &jpeg_port);
		g_bound = 0;
	}
	if (g_chn_created) {
		(void)MI_VENC_DestroyChn(g_chn);
		g_chn_created = 0;
	}
	g_chn = -1;
}
