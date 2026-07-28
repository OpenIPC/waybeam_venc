/* maruko_jpeg.c — Maruko (Infinity6C) MJPEG snapshot backend.
 *
 * Architecture: dedicated MJPG VENC device 8 (I6C_VENC_DEV_MJPG_0)
 * channel 0, bound to a second SCL output port (SCL dev 0 / chn 0 /
 * port 1) configured by maruko_pipeline.c.  The channel stays idle
 * (StopRecvPic after init) and is pulse-encoded on each capture
 * request — same lifecycle pattern as Star6E in src/star6e_jpeg.c.
 *
 * Why a second SCL port rather than fan-out from the main VENC:
 *   1. SCL port 0 is held by the main H.265 channel in
 *      I6_SYS_LINK_RING mode, which is 1:1 — a second BindChnPort2
 *      attempt against port 0 returns SYS busy (0xA0092012).
 *   2. The earlier HW_RING fan-out attempt (main VENC dev 0 → MJPG
 *      VENC dev 8) leaked a [venc8_P0_MAIN] kernel thread on failure
 *      paths, which then blocked the next MI_SYS_Init.
 *
 * Tapping a fresh SCL port avoids both: there's no shared kthread
 * relationship between dev 0 and dev 8, so failed-init teardown is
 * clean.  Bind mode is I6_SYS_LINK_FRAMEBASE @ 5 fps destination
 * rate, so the SCL only forwards a trickle of frames into the JPEG
 * channel — the main stream on port 0 continues at full rate.
 */

#include "venc_jpeg.h"
#include "maruko_bindings.h"
#include "maruko_mi.h"
#include "sigmastar_types.h"
#include "star6e.h"
#include "maruko_scl_ports.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define JPEG_VENC_DEV   I6C_VENC_DEV_MJPG_0   /* 8 — dedicated MJPG VENC */
#define JPEG_VENC_CHN   0
#define MAX_PACKS       8                     /* APP/VDO/PIC/ECS splits */

static MI_SYS_ChnPort_t g_scl_port;
static int      g_have_scl_port = 0;
static int      g_dev_created   = 0;
static int      g_chn_created   = 0;
static int      g_bound         = 0;
static int      g_started       = 0;
static uint32_t g_quality       = 80;
/* Configured snapshot geometry, kept for the grayscale SCL tap (which does
 * not go through the MJPEG channel and so cannot read it back from VENC). */
static uint32_t g_snap_w, g_snap_h;

void venc_jpeg_set_source(const void *port_opaque)
{
	if (!port_opaque) {
		g_have_scl_port = 0;
		return;
	}
	g_scl_port = *(const MI_SYS_ChnPort_t *)port_opaque;
	g_have_scl_port = 1;
}

int venc_jpeg_backend_init(const VencJpegConfig *cfg)
{
	if (!cfg)
		return -EINVAL;
	if (!g_have_scl_port) {
		fprintf(stderr, "[jpeg-maruko] no SCL source registered; "
			"snapshot disabled (SCL port-1 setup likely failed)\n");
		return -ENODEV;
	}
	if (cfg->width == 0 || cfg->height == 0) {
		fprintf(stderr, "[jpeg-maruko] width/height must be non-zero "
			"(got %ux%u)\n", cfg->width, cfg->height);
		return -EINVAL;
	}

	uint32_t w = cfg->width, h = cfg->height;
	uint32_t q = cfg->quality ? cfg->quality : 80;
	if (q > 99) q = 99;
	if (q < 1)  q = 1;
	g_quality = q;
	g_snap_w = w;
	g_snap_h = h;

	/* CreateDev for MJPG device 8 — separate from main H.26x dev 0. */
	i6c_venc_init init = {
		.maxWidth  = w,
		.maxHeight = h,
	};
	MI_S32 ret = maruko_mi_venc_create_dev(JPEG_VENC_DEV, &init);
	if (ret != 0) {
		fprintf(stderr,
			"[jpeg-maruko] MI_VENC_CreateDev(%d) failed %d\n",
			JPEG_VENC_DEV, ret);
		return -EIO;
	}
	g_dev_created = 1;

	/* CreateChn — MJPG codec at q-factor rate mode, low pull-fps so the
	 * channel idles cheaply when no client is requesting snapshots. */
	i6c_venc_chn attr = {0};
	attr.attrib.codec          = I6C_VENC_CODEC_MJPG;
	attr.attrib.mjpg.maxWidth  = w;
	attr.attrib.mjpg.maxHeight = h;
	attr.attrib.mjpg.bufSize   = w * h * 3 / 2;
	attr.attrib.mjpg.byFrame   = 1;
	attr.attrib.mjpg.width     = w;
	attr.attrib.mjpg.height    = h;

	/* Rate mode is the UBR-layout MJPEGFIXQP (= 9), NOT the
	 * I6C_VENC_RATEMODE_MJPGQP enum value (= 8).  Maruko firmware
	 * uses the UBR-shifted enum where 8 = MJPEGVBR — passing 8 here
	 * silently creates a VBR channel, and the quality field is then
	 * ignored (since VBR's struct doesn't have one).  The MjpegFixQp
	 * SDK struct layout matches our i6c_venc_rate_mjpgqp by accident
	 * (fpsNum/fpsDen/quality maps to u32SrcFrmRateNum/Den/Qfactor),
	 * so the existing fields below are byte-correct once the mode
	 * value is right.  See maruko_bindings.h MARUKO_VENC_RC_MJPG_*
	 * for the firmware enum. */
	attr.rate.mode             = MARUKO_VENC_RC_MJPG_FIXQP;
	attr.rate.mjpgQp.fpsNum    = 5;
	attr.rate.mjpgQp.fpsDen    = 1;
	attr.rate.mjpgQp.quality   = q;

	ret = maruko_mi_venc_create_chn(JPEG_VENC_DEV, JPEG_VENC_CHN, &attr);
	if (ret != 0) {
		fprintf(stderr,
			"[jpeg-maruko] MI_VENC_CreateChn(%d,%d) failed %d\n",
			JPEG_VENC_DEV, JPEG_VENC_CHN, ret);
		(void)maruko_mi_venc_destroy_dev(JPEG_VENC_DEV);
		g_dev_created = 0;
		return -EIO;
	}
	g_chn_created = 1;

	/* Bind SCL port 1 → MJPG VENC dev 8 chn 0.  FRAMEBASE with low
	 * dst fps so the main stream on port 0 is unaffected. */
	MI_SYS_ChnPort_t jpeg_port = {
		.module  = I6_SYS_MOD_VENC,
		.device  = JPEG_VENC_DEV,
		.channel = JPEG_VENC_CHN,
		.port    = 0,
	};
	ret = MI_SYS_BindChnPort2(&g_scl_port, &jpeg_port, 30, 5,
		I6_SYS_LINK_FRAMEBASE, 0);
	if (ret != 0) {
		fprintf(stderr,
			"[jpeg-maruko] BindChnPort2 SCL-port1→MJPG-VENC "
			"failed %d (snapshot disabled)\n", ret);
		(void)maruko_mi_venc_destroy_chn(JPEG_VENC_DEV, JPEG_VENC_CHN);
		g_chn_created = 0;
		(void)maruko_mi_venc_destroy_dev(JPEG_VENC_DEV);
		g_dev_created = 0;
		return -EIO;
	}
	g_bound = 1;

	/* CreateChn implicitly starts the channel on the I6C SDK.  Park it:
	 * we flip StartRecvPic on per capture, off again immediately after,
	 * so the JPEG channel only burns encoder cycles when serving a
	 * snapshot request. */
	(void)maruko_mi_venc_stop_recv(JPEG_VENC_DEV, JPEG_VENC_CHN);
	g_started = 0;

	fprintf(stderr,
		"[jpeg-maruko] init OK: dev=%d chn=%d %ux%u q=%u\n",
		JPEG_VENC_DEV, JPEG_VENC_CHN, w, h, q);
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

	MI_S32 ret = maruko_mi_venc_start_recv(JPEG_VENC_DEV, JPEG_VENC_CHN);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-maruko] StartRecvPic failed %d\n", ret);
		return -EIO;
	}
	g_started = 1;

	int rc = 0;
	int64_t deadline = now_ms() + (int64_t)timeout_ms;
	i6c_venc_strm stream = {0};
	i6c_venc_pack packs[MAX_PACKS] = {0};

	i6c_venc_stat stat = {0};
	for (;;) {
		if (maruko_mi_venc_query(JPEG_VENC_DEV, JPEG_VENC_CHN, &stat) == 0
			&& stat.curPacks > 0)
			break;
		if (now_ms() >= deadline) {
			rc = -ETIMEDOUT;
			goto stop;
		}
		usleep(5000);
	}

	uint32_t n = stat.curPacks;
	if (n > MAX_PACKS) n = MAX_PACKS;
	stream.count  = n;
	stream.packet = packs;

	ret = maruko_mi_venc_get_stream(JPEG_VENC_DEV, JPEG_VENC_CHN,
		&stream, 200);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-maruko] GetStream failed %d\n", ret);
		rc = -EIO;
		goto stop;
	}
	if (stream.count == 0) {
		fprintf(stderr, "[jpeg-maruko] GetStream returned 0 packs\n");
		(void)maruko_mi_venc_release_stream(JPEG_VENC_DEV, JPEG_VENC_CHN,
			&stream);
		rc = -EIO;
		goto stop;
	}

	/* Concatenate all packs into a single JPEG blob. */
	size_t total = 0;
	for (uint32_t i = 0; i < stream.count; ++i)
		total += stream.packet[i].length;
	if (total == 0) {
		(void)maruko_mi_venc_release_stream(JPEG_VENC_DEV, JPEG_VENC_CHN,
			&stream);
		rc = -EIO;
		goto stop;
	}

	uint8_t *copy = malloc(total);
	if (!copy) {
		(void)maruko_mi_venc_release_stream(JPEG_VENC_DEV, JPEG_VENC_CHN,
			&stream);
		rc = -ENOMEM;
		goto stop;
	}

	size_t off = 0;
	for (uint32_t i = 0; i < stream.count; ++i) {
		memcpy(copy + off, stream.packet[i].data,
			stream.packet[i].length);
		off += stream.packet[i].length;
	}
	(void)maruko_mi_venc_release_stream(JPEG_VENC_DEV, JPEG_VENC_CHN,
		&stream);

	*out_buf = copy;
	*out_len = total;

stop:
	if (g_started) {
		(void)maruko_mi_venc_stop_recv(JPEG_VENC_DEV, JPEG_VENC_CHN);
		g_started = 0;
	}
	return rc;
}

/* ── Grayscale (P5 PGM) capture ──────────────────────────────────────────
 *
 * Mirrors the Star6E path in star6e_jpeg.c, on i6c primitives: program a
 * short-lived SCL tap, drain one uncompressed NV12 frame, copy its luma
 * plane, tear the tap down.
 *
 * Port choice: every SCL output already has an owner — port0 is the main
 * H.265 output (RING, 1:1), port1 carries the bound MJPEG channel, port2 is
 * the stab tap.  Only **port3** (the NPU detector's) is available, and a user
 * output depth may only ever be registered on a port with no downstream
 * hardware bind (registering one on a bound port faults the MI_SYS allocator
 * — the bug this endpoint shipped with on Star6E).  So port3 it is, taken
 * through maruko_scl_ports so a capture loses to a running detector instead
 * of reprogramming the tap underneath it.
 *
 * i6c ABI notes (these differ from i6e — see the SDK headers, never assume
 * the Star6E layout): MI_SYS entry points carry a leading u16 soc_id, and the
 * SCL port is configured through fnSetPortConfig(dev, chn, port, i6c_scl_port)
 * with an explicit crop window rather than a VPE port-mode struct. */

#define GRAY_TAP_PORT      3
#define GRAY_TAP_OWNER     "snapshot"
#define GRAY_BUFDATA_FRAME 1
#define GRAY_TAP_MAX_DIM   1280u   /* long-side cap; QR/vision consumers */
#define GRAY_MAX_DIM       8192u   /* sanity clamp on returned geometry */

/* Layout copied from the proven detector definition in maruko_ipu_yolo.c. */
typedef struct { MI_U16 x, y, w, h; } GrayRect;
typedef struct {
	int tile_mode, pixel_format, compress_mode, scan_mode;
	int field_type, phy_layout_type;
	MI_U16 width, height;
	void *vir_addr[3];
	MI_U64 phy_addr[3];
	MI_U32 stride[3];
	MI_U32 buf_size;
	MI_U16 ring_start, ring_total;
	struct { int type; union { MI_U32 global; } u; } isp;
	GrayRect crop;
} GrayFrameData;
typedef struct {
	MI_U64 pts, sideband;
	int type;
	MI_U8 eos, user_buf;
	MI_U32 seq;
	MI_U8 drop;
	union { GrayFrameData frame; MI_U8 pad[512]; };
	MI_U8 custom;
} GrayBufInfo;

typedef MI_S32 (*GrayGetBufFn)(MI_SYS_ChnPort_t *, GrayBufInfo *, MI_S32 *);
typedef MI_S32 (*GrayPutBufFn)(MI_S32);
typedef MI_S32 (*GrayMmapFn)(MI_U64 phy, MI_U32 size, void **vir, MI_U8 cache);
typedef MI_S32 (*GrayMunmapFn)(void *vir, MI_U32 size);

static GrayGetBufFn  g_gray_get_buf;
static GrayPutBufFn  g_gray_put_buf;
static GrayMmapFn    g_gray_mmap;
static GrayMunmapFn  g_gray_munmap;
static uint32_t g_gray_crop_x, g_gray_crop_y, g_gray_crop_w, g_gray_crop_h;

void venc_jpeg_set_gray_crop(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	g_gray_crop_x = x;
	g_gray_crop_y = y;
	g_gray_crop_w = w;
	g_gray_crop_h = h;
}

static int gray_load_syms(void)
{
	void *h = g_mi_sys.handle;

	if (g_gray_get_buf && g_gray_put_buf && g_gray_mmap)
		return 0;
	if (!h)
		return -1;
	g_gray_get_buf = (GrayGetBufFn)dlsym(h, "MI_SYS_ChnOutputPortGetBuf");
	g_gray_put_buf = (GrayPutBufFn)dlsym(h, "MI_SYS_ChnOutputPortPutBuf");
	g_gray_mmap    = (GrayMmapFn)dlsym(h, "MI_SYS_Mmap");
	g_gray_munmap  = (GrayMunmapFn)dlsym(h, "MI_SYS_Munmap");
	if (!g_gray_get_buf || !g_gray_put_buf || !g_gray_mmap)
		return -1;
	return 0;
}

/* Tap geometry: the configured snapshot size with the long side capped.
 * PGM is self-describing, so consumers read the real dims from the header. */
static void gray_tap_dims(uint32_t *out_w, uint32_t *out_h)
{
	uint32_t w = g_snap_w ? g_snap_w : 1280;
	uint32_t h = g_snap_h ? g_snap_h : 720;
	uint32_t longest = (w > h) ? w : h;

	if (longest > GRAY_TAP_MAX_DIM) {
		w = (uint32_t)((uint64_t)w * GRAY_TAP_MAX_DIM / longest);
		h = (uint32_t)((uint64_t)h * GRAY_TAP_MAX_DIM / longest);
	}
	w &= ~15u;
	h &= ~1u;
	if (w < 64) w = 64;
	if (h < 64) h = 64;
	*out_w = w;
	*out_h = h;
}

/* Pack the Y plane of one NV12 frame into a malloc'd P5 PGM blob. */
static int gray_frame_to_pgm(const GrayFrameData *fr, uint8_t **out_buf,
	size_t *out_len)
{
	uint32_t w = fr->width, h = fr->height;
	uint32_t stride = fr->stride[0] ? fr->stride[0] : w;
	MI_U64 phy = fr->phy_addr[0];
	void *vir = NULL;
	char hdr[32];
	int hlen;
	size_t total;
	uint8_t *out;
	uint32_t row;

	if (w == 0 || h == 0 || w > GRAY_MAX_DIM || h > GRAY_MAX_DIM || !phy)
		return -EIO;
	/* Non-cached (flag 0): reads see the latest DMA with no invalidate. */
	if (g_gray_mmap(phy, stride * h, &vir, 0) != 0 || !vir)
		return -EIO;

	hlen = snprintf(hdr, sizeof(hdr), "P5\n%u %u\n255\n", w, h);
	total = (size_t)hlen + (size_t)w * h;
	out = malloc(total);
	if (!out) {
		g_gray_munmap(vir, stride * h);
		return -ENOMEM;
	}
	memcpy(out, hdr, (size_t)hlen);
	for (row = 0; row < h; ++row)
		memcpy(out + hlen + (size_t)row * w,
			(const uint8_t *)vir + (size_t)row * stride, w);
	g_gray_munmap(vir, stride * h);

	*out_buf = out;
	*out_len = total;
	return 0;
}

static int gray_drain_one(MI_SYS_ChnPort_t *tap, uint8_t **out_buf,
	size_t *out_len, uint32_t timeout_ms)
{
	int64_t deadline = now_ms() + (int64_t)timeout_ms;

	for (;;) {
		GrayBufInfo buf;
		MI_S32 handle = 0;

		memset(&buf, 0, sizeof(buf));
		if (g_gray_get_buf(tap, &buf, &handle) != 0) {
			if (now_ms() >= deadline)
				return -ETIMEDOUT;
			usleep(2000);
			continue;
		}
		if (buf.type != GRAY_BUFDATA_FRAME || !buf.frame.phy_addr[0]) {
			g_gray_put_buf(handle);
			if (now_ms() >= deadline)
				return -ETIMEDOUT;
			usleep(2000);
			continue;
		}
		{
			int rc = gray_frame_to_pgm(&buf.frame, out_buf, out_len);
			g_gray_put_buf(handle);
			return rc;
		}
	}
}

int venc_jpeg_backend_capture_gray(uint8_t **out_buf, size_t *out_len,
	uint32_t timeout_ms)
{
	MI_SYS_ChnPort_t tap;
	i6c_scl_port p;
	uint32_t tap_w = 0, tap_h = 0;
	int port_enabled = 0, depth_set = 0, claimed = 0;
	int rc;
	MI_S32 ret;

	if (!out_buf || !out_len)
		return -EINVAL;
	*out_buf = NULL;
	*out_len = 0;
	if (!g_have_scl_port || !g_mi_scl.fnSetPortConfig)
		return -ENODEV;
	if (g_gray_crop_w == 0 || g_gray_crop_h == 0)
		return -ENODEV;   /* pipeline has not published a crop window yet */
	if (gray_load_syms() != 0)
		return -EIO;
	if (timeout_ms == 0)
		timeout_ms = 1500;

	if (maruko_scl_tap_claim(GRAY_TAP_OWNER) != 0) {
		char owner[16];
		maruko_scl_tap_owner_copy(owner, sizeof(owner));
		fprintf(stderr, "[jpeg-maruko] gray: SCL tap busy (owner=%s)\n",
			owner[0] ? owner : "?");
		return -EBUSY;
	}
	claimed = 1;

	gray_tap_dims(&tap_w, &tap_h);

	tap = (MI_SYS_ChnPort_t){ .module = I6_SYS_MOD_SCL, .device = 0,
		.channel = 0, .port = GRAY_TAP_PORT };

	memset(&p, 0, sizeof(p));
	p.crop.x = (MI_U16)g_gray_crop_x;
	p.crop.y = (MI_U16)g_gray_crop_y;
	p.crop.width = (MI_U16)g_gray_crop_w;
	p.crop.height = (MI_U16)g_gray_crop_h;
	p.output.width = (MI_U16)tap_w;
	p.output.height = (MI_U16)tap_h;
	p.pixFmt = I6_PIXFMT_YUV420SP;
	p.compress = (i6_common_compr)0;

	ret = g_mi_scl.fnSetPortConfig(0, 0, GRAY_TAP_PORT, &p);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-maruko] gray: SCL port%d config %ux%u failed "
			"%d\n", GRAY_TAP_PORT, tap_w, tap_h, (int)ret);
		rc = -EIO;
		goto out;
	}
	ret = g_mi_scl.fnEnablePort(0, 0, GRAY_TAP_PORT);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-maruko] gray: SCL port%d enable failed %d\n",
			GRAY_TAP_PORT, (int)ret);
		rc = -EIO;
		goto out;
	}
	port_enabled = 1;

	/* Legal here and only here: port3 carries no downstream bind. */
	ret = g_mi_sys.fnSetChnOutputPortDepth(0, &tap, 2, 4);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-maruko] gray: port%d depth failed %d\n",
			GRAY_TAP_PORT, (int)ret);
		rc = -EIO;
		goto out;
	}
	depth_set = 1;

	rc = gray_drain_one(&tap, out_buf, out_len, timeout_ms);

out:
	/* Depth reset BEFORE DisablePort, always — a stranded depth leaves the
	 * kernel queueing output tasks for a consumer that is gone. */
	if (depth_set)
		(void)g_mi_sys.fnSetChnOutputPortDepth(0, &tap, 0, 0);
	if (port_enabled)
		(void)g_mi_scl.fnDisablePort(0, 0, GRAY_TAP_PORT);
	if (claimed)
		maruko_scl_tap_release(GRAY_TAP_OWNER);
	return rc;
}

int venc_jpeg_backend_set_quality(uint32_t q)
{
	if (!g_chn_created)
		return -ENODEV;
	if (!g_mi_venc.fnGetChnAttr || !g_mi_venc.fnSetChnAttr)
		return -ENOSYS;
	if (q == 0) q = 1;
	if (q > 99) q = 99;

	/* Get → modify quality → Set on the running channel.  The MJPEG
	 * channel is currently parked (StopRecvPic) between captures, but
	 * holding g_jpeg_mutex in the front end means we can't race a
	 * capture in progress.  Rate mode is MJPEGFIXQP (= 9), and the
	 * mjpgQp struct's `quality` field maps to MI_VENC_AttrMjpegFixQp_t's
	 * u32Qfactor — see commit "MJPG snapshot quality wires through" for
	 * the byte-layout discussion. */
	i6c_venc_chn attr = {0};
	if (g_mi_venc.fnGetChnAttr(JPEG_VENC_DEV, JPEG_VENC_CHN, &attr) != 0) {
		fprintf(stderr,
			"[jpeg-maruko] GetChnAttr(dev=%d,chn=%d) failed during "
			"live quality update\n", JPEG_VENC_DEV, JPEG_VENC_CHN);
		return -EIO;
	}
	attr.rate.mjpgQp.quality = q;
	if (g_mi_venc.fnSetChnAttr(JPEG_VENC_DEV, JPEG_VENC_CHN, &attr) != 0) {
		fprintf(stderr,
			"[jpeg-maruko] SetChnAttr(q=%u) failed\n", q);
		return -EIO;
	}
	g_quality = q;
	return 0;
}

void venc_jpeg_backend_shutdown(void)
{
	if (g_started) {
		(void)maruko_mi_venc_stop_recv(JPEG_VENC_DEV, JPEG_VENC_CHN);
		g_started = 0;
	}
	if (g_bound) {
		MI_SYS_ChnPort_t jpeg_port = {
			.module  = I6_SYS_MOD_VENC,
			.device  = JPEG_VENC_DEV,
			.channel = JPEG_VENC_CHN,
			.port    = 0,
		};
		(void)MI_SYS_UnBindChnPort(&g_scl_port, &jpeg_port);
		g_bound = 0;
	}
	if (g_chn_created) {
		(void)maruko_mi_venc_destroy_chn(JPEG_VENC_DEV, JPEG_VENC_CHN);
		g_chn_created = 0;
	}
	if (g_dev_created) {
		(void)maruko_mi_venc_destroy_dev(JPEG_VENC_DEV);
		g_dev_created = 0;
	}
	g_have_scl_port = 0;
}
