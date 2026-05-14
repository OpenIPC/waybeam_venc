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
	 * for the firmware enum and the H264/H265 precedent. */
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
	if (n > MAX_PACKS) {
		fprintf(stderr, "[jpeg-maruko] WARN: %u packs > max %d, "
			"JPEG may be truncated\n",
			(unsigned)stat.curPacks, MAX_PACKS);
		n = MAX_PACKS;
	}
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
