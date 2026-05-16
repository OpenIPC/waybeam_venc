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
