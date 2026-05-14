/* maruko_jpeg.c — Maruko (Infinity6C) MJPEG snapshot backend.
 *
 * Mirrors src/star6e_jpeg.c with three Maruko-specific twists:
 *   1. MJPEG channels live on a dedicated VENC *device* (`I6C_VENC_DEV_MJPG_0`
 *      = 8), separate from the H.26x device 0 used by the main stream.  We
 *      create our own device + channel pair on dev 8.
 *   2. All MI_VENC_* calls go through the `maruko_mi_venc_*` wrapper macros
 *      from maruko_bindings.h (these dispatch via the dlopen'd g_mi_venc).
 *   3. The upstream port is SCL (`I6_SYS_MOD_SCL`), not VPE — the main
 *      pipeline binds ISP→SCL→VENC, and we tap the same SCL output port.
 *
 * Channel lifecycle: created at pipeline start (StartRecvPic off), bound
 * to the same SCL port the main H.265/H.264 channel consumes (1:N from
 * SCL output port — same pattern the main pipeline already uses).  Each
 * capture flips StartRecvPic on, polls Query for a ready pack, drains
 * one MJPEG frame, then turns StartRecvPic back off.
 */

#include "venc_jpeg.h"
#include "maruko_bindings.h"
#include "maruko_mi.h"
#include "sigmastar_types.h"
#include "star6e.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MJPG_DEV       I6C_VENC_DEV_MJPG_0
#define MAX_PACKS_PER_JPEG 8

static MI_SYS_ChnPort_t g_scl_port;   /* source — registered by pipeline */
static int g_have_scl_port = 0;
static int g_chn = -1;                 /* channel index within MJPG_DEV */
static int g_dev_created = 0;
static int g_chn_created = 0;
static int g_bound = 0;
static uint32_t g_quality = 80;

void venc_jpeg_set_source(const void *scl_port_opaque)
{
	if (!scl_port_opaque) {
		g_have_scl_port = 0;
		return;
	}
	g_scl_port = *(const MI_SYS_ChnPort_t *)scl_port_opaque;
	g_have_scl_port = 1;
}

int venc_jpeg_backend_init(const VencJpegConfig *cfg)
{
	if (!cfg)
		return -EINVAL;
	if (!g_have_scl_port) {
		fprintf(stderr, "[jpeg-maruko] no SCL source registered; "
			"call venc_jpeg_set_source() before init\n");
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
	if (q < 1) q = 1;
	g_quality = q;
	g_chn = cfg->channel > 0 ? cfg->channel : 0;
	/* Maruko: channel id is scoped per device.  We use a fresh device
	 * (MJPG_DEV=8) so chn 0 there is always free regardless of how many
	 * H.26x channels the main pipeline allocates on dev 0. */
	if (g_chn > 0) g_chn = 0;

	i6c_venc_init dev_init = {
		.maxWidth  = w,
		.maxHeight = h,
	};
	MI_S32 ret = maruko_mi_venc_create_dev(MJPG_DEV, &dev_init);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-maruko] CreateDev(MJPG=%d) failed %d\n",
			MJPG_DEV, ret);
		return -EIO;
	}
	g_dev_created = 1;

	i6c_venc_chn attr = {0};
	attr.attrib.codec = I6C_VENC_CODEC_MJPG;
	attr.attrib.mjpg.maxWidth  = w;
	attr.attrib.mjpg.maxHeight = h;
	attr.attrib.mjpg.bufSize   = w * h * 3 / 2;
	attr.attrib.mjpg.byFrame   = 1;
	attr.attrib.mjpg.width     = w;
	attr.attrib.mjpg.height    = h;

	attr.rate.mode = I6C_VENC_RATEMODE_MJPGQP;
	attr.rate.mjpgQp.fpsNum  = 5;   /* low — pulled on demand only */
	attr.rate.mjpgQp.fpsDen  = 1;
	attr.rate.mjpgQp.quality = q;

	ret = maruko_mi_venc_create_chn(MJPG_DEV, g_chn, &attr);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-maruko] CreateChn(dev=%d chn=%d) failed %d\n",
			MJPG_DEV, g_chn, ret);
		(void)maruko_mi_venc_destroy_dev(MJPG_DEV);
		g_dev_created = 0;
		return -EIO;
	}
	g_chn_created = 1;

	/* Match the main pipeline's SCL→VENC source-config (RING_DMA) so
	 * the JPEG channel ingests frames the same way. */
	i6c_venc_src_conf input_mode = I6C_VENC_SRC_CONF_RING_DMA;
	(void)maruko_mi_venc_set_input_source(MJPG_DEV, g_chn, &input_mode);

	MI_SYS_ChnPort_t jpeg_port = {
		.module  = I6_SYS_MOD_VENC,
		.device  = MJPG_DEV,
		.channel = (unsigned)g_chn,
		.port    = 0,
	};
	ret = MI_SYS_BindChnPort2(&g_scl_port, &jpeg_port, 30, 5,
		I6_SYS_LINK_RING, 0);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-maruko] BindChnPort2 SCL→MJPG-VENC failed %d\n",
			ret);
		(void)maruko_mi_venc_destroy_chn(MJPG_DEV, g_chn);
		g_chn_created = 0;
		(void)maruko_mi_venc_destroy_dev(MJPG_DEV);
		g_dev_created = 0;
		return -EIO;
	}
	g_bound = 1;

	fprintf(stderr, "[jpeg-maruko] init OK: dev=%d chn=%d %ux%u q=%u\n",
		MJPG_DEV, g_chn, w, h, q);
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

	MI_S32 ret = maruko_mi_venc_start_recv(MJPG_DEV, g_chn);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-maruko] StartRecvPic failed %d\n", ret);
		return -EIO;
	}

	int rc = 0;
	int64_t deadline = now_ms() + (int64_t)timeout_ms;
	i6c_venc_stat stat = {0};
	i6c_venc_strm stream = {0};
	i6c_venc_pack packs[MAX_PACKS_PER_JPEG] = {0};

	for (;;) {
		if (maruko_mi_venc_query(MJPG_DEV, g_chn, &stat) == 0 &&
		    stat.curPacks > 0)
			break;
		if (now_ms() >= deadline) {
			rc = -ETIMEDOUT;
			goto stop;
		}
		usleep(5000);
	}

	uint32_t n = stat.curPacks;
	if (n > MAX_PACKS_PER_JPEG) n = MAX_PACKS_PER_JPEG;
	stream.count  = n;
	stream.packet = packs;

	ret = maruko_mi_venc_get_stream(MJPG_DEV, g_chn, &stream, 200);
	if (ret != 0) {
		fprintf(stderr, "[jpeg-maruko] GetStream failed %d\n", ret);
		rc = -EIO;
		goto stop;
	}
	if (stream.count == 0) {
		fprintf(stderr, "[jpeg-maruko] GetStream returned 0 packs\n");
		(void)maruko_mi_venc_release_stream(MJPG_DEV, g_chn, &stream);
		rc = -EIO;
		goto stop;
	}

	size_t total = 0;
	for (uint32_t i = 0; i < stream.count; ++i)
		total += stream.packet[i].length;
	if (total == 0) {
		(void)maruko_mi_venc_release_stream(MJPG_DEV, g_chn, &stream);
		rc = -EIO;
		goto stop;
	}

	uint8_t *copy = malloc(total);
	if (!copy) {
		(void)maruko_mi_venc_release_stream(MJPG_DEV, g_chn, &stream);
		rc = -ENOMEM;
		goto stop;
	}

	size_t off = 0;
	for (uint32_t i = 0; i < stream.count; ++i) {
		memcpy(copy + off, stream.packet[i].data,
			stream.packet[i].length);
		off += stream.packet[i].length;
	}
	(void)maruko_mi_venc_release_stream(MJPG_DEV, g_chn, &stream);

	*out_buf = copy;
	*out_len = total;

stop:
	(void)maruko_mi_venc_stop_recv(MJPG_DEV, g_chn);
	return rc;
}

void venc_jpeg_backend_shutdown(void)
{
	if (g_bound) {
		MI_SYS_ChnPort_t jpeg_port = {
			.module  = I6_SYS_MOD_VENC,
			.device  = MJPG_DEV,
			.channel = (unsigned)g_chn,
			.port    = 0,
		};
		(void)MI_SYS_UnBindChnPort(&g_scl_port, &jpeg_port);
		g_bound = 0;
	}
	if (g_chn_created) {
		(void)maruko_mi_venc_destroy_chn(MJPG_DEV, g_chn);
		g_chn_created = 0;
	}
	if (g_dev_created) {
		(void)maruko_mi_venc_destroy_dev(MJPG_DEV);
		g_dev_created = 0;
	}
	g_chn = -1;
}
