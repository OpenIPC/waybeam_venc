/* cv610_jpeg.c — CV610 (Hi3516CV610) JPEG snapshot backend.
 *
 * Creates one dedicated VENC channel (default ch7, OT_PT_JPEG) bound as a
 * SECOND destination on the same VPSS channel the main H.265 encoder already
 * consumes.  ot_defines.h caps a source at OT_MAX_BIND_DST_NUM (4) bind
 * targets, so this needs no VPSS channel of its own: no extra scaling, no
 * extra VB pool, and nothing running while nobody is asking for a snapshot.
 *
 * The bind is permanent; only the channel's receive state pulses.  Each
 * capture does start_chn(recv_pic_num = 1) -> poll query_status ->
 * get_stream -> copy -> release_stream -> stop_chn.  A stopped bound channel
 * discards frames the source keeps delivering, so the live H.265 path is
 * untouched between requests.
 *
 * That ordering is deliberate.  Cycling the *source* port per request is the
 * shape that has bitten this project before (a VPE port that enabled but then
 * never delivered); here the source is never reconfigured, so there is no
 * enable/deliver race to lose.
 *
 * Geometry: the snapshot inherits the main stream's size, because it shares
 * the main stream's VPSS channel.  snapshot.width/height are advertised
 * unsupported on this backend rather than accepted and ignored.
 */

#include "venc_jpeg.h"

#include "ot_common.h"
#include "ot_common_video.h"
#include "ot_common_venc.h"
#include "ot_common_sys.h"
#include "ss_mpi_venc.h"
#include "ss_mpi_sys_bind.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static ot_mpp_chn g_src;          /* VPSS grp/chn feeding the main encoder */
static int g_have_src;
static ot_venc_chn g_chn = -1;
static int g_chn_created;
static int g_bound;

static ot_mpp_chn jpeg_dst(void)
{
	ot_mpp_chn dst;

	dst.mod_id = OT_ID_VENC;
	dst.dev_id = 0;
	dst.chn_id = g_chn;
	return dst;
}

/* Called by cv610_venc_start() once the main channel's own bind has
 * succeeded, so the source is known good before a second consumer joins it. */
void venc_jpeg_set_source(const void *src_chn_opaque)
{
	if (!src_chn_opaque) {
		g_have_src = 0;
		return;
	}
	g_src = *(const ot_mpp_chn *)src_chn_opaque;
	g_have_src = 1;
}

static int apply_qfactor(uint32_t q)
{
	ot_venc_jpeg_param param;

	memset(&param, 0, sizeof(param));
	if (ss_mpi_venc_get_jpeg_param(g_chn, &param) != TD_SUCCESS) {
		fprintf(stderr, "[jpeg-cv610] get_jpeg_param(%d) failed\n",
			(int)g_chn);
		return -EIO;
	}
	param.qfactor = q;
	if (ss_mpi_venc_set_jpeg_param(g_chn, &param) != TD_SUCCESS) {
		fprintf(stderr, "[jpeg-cv610] set_jpeg_param(q=%u) failed\n",
			(unsigned)q);
		return -EIO;
	}
	return 0;
}

int venc_jpeg_backend_init(const VencJpegConfig *cfg)
{
	ot_venc_chn_attr attr;
	ot_mpp_chn dst;
	uint32_t w, h, q;
	td_s32 ret;

	if (!cfg)
		return -EINVAL;
	if (!g_have_src) {
		fprintf(stderr, "[jpeg-cv610] no VPSS source registered; "
			"call venc_jpeg_set_source() before init\n");
		return -ENODEV;
	}
	if (cfg->width == 0 || cfg->height == 0) {
		fprintf(stderr, "[jpeg-cv610] width/height must be non-zero "
			"(got %ux%u)\n", cfg->width, cfg->height);
		return -EINVAL;
	}

	w = cfg->width;
	h = cfg->height;
	q = cfg->quality ? cfg->quality : 80;
	if (q > 99)
		q = 99;
	if (q < 1)
		q = 1;
	g_chn = (ot_venc_chn)cfg->channel;

	memset(&attr, 0, sizeof(attr));
	attr.venc_attr.type = OT_PT_JPEG;
	attr.venc_attr.max_pic_width = w;
	attr.venc_attr.max_pic_height = h;
	/* 3/4, matching the main H.265 channel — this buffer is reserved out
	 * of a 64 MB MMZ for the life of the process whether or not a snapshot
	 * is ever taken, and 3/2 would hold ~3 MB at 1080p, double what the
	 * video channel itself takes.  Device-measured worst case is 379.5 KB
	 * (q=95, 720p), so 3/4 is still ample headroom. */
	attr.venc_attr.buf_size = ((w * h * 3 / 4) + 63) & ~63u;
	attr.venc_attr.is_by_frame = TD_TRUE;
	attr.venc_attr.pic_width = w;
	attr.venc_attr.pic_height = h;
	attr.venc_attr.jpeg_attr.recv_mode = OT_VENC_PIC_RECV_SINGLE;
	/* rc_attr stays zeroed: ot_common_rc.h declares no JPEG rate-control
	 * family — a still JPEG's rate is its qfactor, set below. */

	ret = ss_mpi_venc_create_chn(g_chn, &attr);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "[jpeg-cv610] create_chn(%d)=0x%x\n",
			(int)g_chn, (unsigned)ret);
		/* Name the one cause that is not a code bug.  JPEG encode is its
		 * own kernel module on this SoC and the H.265 path comes up
		 * without it, so a device missing open_jpege.ko fails exactly
		 * here and nowhere else. */
		if ((unsigned)ret == 0xa0088018u)
			fprintf(stderr, "[jpeg-cv610] OT_ERR_NOT_READY: the JPEG "
				"codec module is probably not loaded — check "
				"`lsmod | grep jpege`\n");
		return -EIO;
	}
	g_chn_created = 1;

	if (apply_qfactor(q) != 0) {
		(void)ss_mpi_venc_destroy_chn(g_chn);
		g_chn_created = 0;
		return -EIO;
	}

	/* Second destination on the main encoder's source.  The channel is
	 * left stopped, so it draws no encode bandwidth until a capture. */
	dst = jpeg_dst();
	ret = ss_mpi_sys_bind(&g_src, &dst);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "[jpeg-cv610] sys_bind VPSS(%d,%d)->VENC(%d)=0x%x\n",
			(int)g_src.dev_id, (int)g_src.chn_id, (int)g_chn,
			(unsigned)ret);
		(void)ss_mpi_venc_destroy_chn(g_chn);
		g_chn_created = 0;
		return -EIO;
	}
	g_bound = 1;

	fprintf(stderr, "[jpeg-cv610] init OK: chn=%d %ux%u q=%u\n",
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
	ot_venc_start_param start;
	ot_venc_chn_status status;
	ot_venc_stream stream;
	int64_t deadline;
	size_t total = 0;
	size_t off = 0;
	uint8_t *copy;
	int rc = 0;
	td_u32 i;

	if (!out_buf || !out_len)
		return -EINVAL;
	*out_buf = NULL;
	*out_len = 0;
	if (!g_chn_created || !g_bound)
		return -ENODEV;
	if (timeout_ms == 0)
		timeout_ms = 1500;

	memset(&start, 0, sizeof(start));
	start.recv_pic_num = 1;
	if (ss_mpi_venc_start_chn(g_chn, &start) != TD_SUCCESS) {
		fprintf(stderr, "[jpeg-cv610] start_chn(%d) failed\n", (int)g_chn);
		return -EIO;
	}

	/* Poll rather than select(): the fd's readability is tied to the
	 * channel's receive state, which this function is toggling. */
	deadline = now_ms() + (int64_t)timeout_ms;
	for (;;) {
		memset(&status, 0, sizeof(status));
		if (ss_mpi_venc_query_status(g_chn, &status) == TD_SUCCESS &&
		    status.cur_packs > 0)
			break;
		if (now_ms() >= deadline) {
			rc = -ETIMEDOUT;
			goto stop;
		}
		usleep(5000);
	}

	memset(&stream, 0, sizeof(stream));
	stream.pack = calloc(status.cur_packs, sizeof(*stream.pack));
	if (!stream.pack) {
		rc = -ENOMEM;
		goto stop;
	}
	stream.pack_cnt = status.cur_packs;

	/* Clamp what the SDK writes back.  The buffer is sized from
	 * status.cur_packs, and iterating a larger pack_cnt would read past
	 * it — star6e_jpeg.c already clamps for the same reason. */
	if (ss_mpi_venc_get_stream(g_chn, &stream, 200) != TD_SUCCESS) {
		fprintf(stderr, "[jpeg-cv610] get_stream(%d) failed\n", (int)g_chn);
		free(stream.pack);
		rc = -EIO;
		goto stop;
	}

	if (stream.pack_cnt > status.cur_packs)
		stream.pack_cnt = status.cur_packs;

	for (i = 0; i < stream.pack_cnt; ++i) {
		if (stream.pack[i].addr && stream.pack[i].len > stream.pack[i].offset)
			total += stream.pack[i].len - stream.pack[i].offset;
	}
	if (total == 0) {
		(void)ss_mpi_venc_release_stream(g_chn, &stream);
		free(stream.pack);
		rc = -EIO;
		goto stop;
	}

	copy = malloc(total);
	if (!copy) {
		(void)ss_mpi_venc_release_stream(g_chn, &stream);
		free(stream.pack);
		rc = -ENOMEM;
		goto stop;
	}
	for (i = 0; i < stream.pack_cnt; ++i) {
		size_t chunk;

		if (!stream.pack[i].addr ||
		    stream.pack[i].len <= stream.pack[i].offset)
			continue;
		chunk = stream.pack[i].len - stream.pack[i].offset;
		memcpy(copy + off, stream.pack[i].addr + stream.pack[i].offset,
			chunk);
		off += chunk;
	}
	(void)ss_mpi_venc_release_stream(g_chn, &stream);
	free(stream.pack);

	*out_buf = copy;
	*out_len = total;

stop:
	(void)ss_mpi_venc_stop_chn(g_chn);
	return rc;
}

int venc_jpeg_backend_set_quality(uint32_t q)
{
	if (!g_chn_created)
		return -ENODEV;
	if (q == 0)
		q = 1;
	if (q > 99)
		q = 99;
	return apply_qfactor(q);
}

void venc_jpeg_backend_shutdown(void)
{
	if (g_bound) {
		ot_mpp_chn dst = jpeg_dst();

		(void)ss_mpi_sys_unbind(&g_src, &dst);
		g_bound = 0;
	}
	if (g_chn_created) {
		/* Defensive: a capture that died between start_chn and stop_chn
		 * would otherwise leave the channel receiving into a destroy. */
		(void)ss_mpi_venc_stop_chn(g_chn);
		(void)ss_mpi_venc_destroy_chn(g_chn);
		g_chn_created = 0;
	}
	g_chn = -1;
	g_have_src = 0;
}
