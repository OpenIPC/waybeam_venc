/*
 * Maruko IPU detector host.
 *
 * SCL0/0/3 provides a raw NV12, model-sized tap.  A reader thread keeps the
 * user port drained and passes physical plane addresses to the ABI-3 detector
 * plugin.  Detection is best-effort: every failure unwinds port 3 while the
 * video path keeps running.
 */
#include "maruko_ipu_yolo.h"

#include "maruko_mi.h"
#include "rtp_sidecar.h"
#include "timing.h"

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#define MI_SYS_BUFDATA_FRAME 1
#define MARUKO_DETECT_PORT 3

typedef struct { MI_U16 x, y, w, h; } DetectRect;
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
	DetectRect crop;
} DetectFrameData;
typedef struct {
	MI_U64 pts, sideband;
	int type;
	MI_U8 eos, user_buf;
	MI_U32 seq;
	MI_U8 drop;
	union { DetectFrameData frame; MI_U8 pad[512]; };
	MI_U8 custom;
} DetectBufInfo;

typedef MI_S32 (*GetFdFn)(MI_SYS_ChnPort_t *, MI_S32 *);
typedef MI_S32 (*CloseFdFn)(MI_S32);
typedef MI_S32 (*GetBufFn)(MI_SYS_ChnPort_t *, DetectBufInfo *, MI_S32 *);
typedef MI_S32 (*PutBufFn)(MI_S32);

typedef struct {
	const DetectBackend *backend;
	void *plugin;
	MI_SYS_ChnPort_t port;
	int port_enabled;
	uint32_t net_w, net_h, model_id;
	int infer_interval;
	pthread_t reader;
	int reader_started;
	volatile sig_atomic_t running;
	volatile int drain_only;
	volatile int processing;
	pthread_mutex_t lock;
	DetectBox work[MARUKO_DETECT_SNAP_MAX];
	DetectBox published[MARUKO_DETECT_SNAP_MAX];
	int pub_count, pub_valid;
	uint32_t pub_seq;
	uint64_t pub_us;
	GetFdFn get_fd;
	CloseFdFn close_fd;
	GetBufFn get_buf;
	PutBufFn put_buf;
} MarukoIpuDetect;

static int load_sys_symbols(MarukoIpuDetect *d)
{
	void *h = g_mi_sys.handle;

	if (!h)
		return -1;
	d->get_fd = (GetFdFn)dlsym(h, "MI_SYS_GetFd");
	d->close_fd = (CloseFdFn)dlsym(h, "MI_SYS_CloseFd");
	d->get_buf = (GetBufFn)dlsym(h, "MI_SYS_ChnOutputPortGetBuf");
	d->put_buf = (PutBufFn)dlsym(h, "MI_SYS_ChnOutputPortPutBuf");
	return d->get_buf && d->put_buf ? 0 : -1;
}

static int resolve_dims(const VencConfig *vcfg, uint32_t *w, uint32_t *h)
{
	uint32_t rw = vcfg->detect.net_width ? vcfg->detect.net_width : 800;
	uint32_t rh = vcfg->detect.net_height ? vcfg->detect.net_height : 448;

	if (rw < 64 || rh < 64 || rw % 32 || rh % 32) {
		fprintf(stderr, "[maruko-ipu] tap %ux%u must be >=64 and "
			"multiples of 32\n", rw, rh);
		return -1;
	}
	*w = rw;
	*h = rh;
	return 0;
}

static int check_model_dims(MarukoIpuDetect *d)
{
	uint32_t w = 0, h = 0;

	if (!d->backend->model_dims ||
	    d->backend->model_dims(&w, &h) != 0 ||
	    w != d->net_w || h != d->net_h) {
		fprintf(stderr, "[maruko-ipu] model/tap geometry mismatch: "
			"model=%ux%u tap=%ux%u\n", w, h, d->net_w, d->net_h);
		return -1;
	}
	return 0;
}

static int load_backend(MarukoIpuDetect *d, const VencConfig *vcfg,
	uint32_t display_w, uint32_t display_h)
{
	const char *path = vcfg->detect.plugin[0] ? vcfg->detect.plugin
		: "/root/libwaybeam_detect.so";
	WaybeamDetectEntryFn entry;
	DetectBackendConfig cfg;

	d->plugin = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
	if (!d->plugin) {
		fprintf(stderr, "[maruko-ipu] dlopen '%s': %s\n", path, dlerror());
		return -1;
	}
	entry = (WaybeamDetectEntryFn)dlsym(d->plugin, WAYBEAM_DETECT_ENTRY);
	d->backend = entry ? entry() : NULL;
	if (!d->backend || d->backend->abi != DETECT_PLUGIN_ABI ||
	    !d->backend->init || !d->backend->process ||
	    !d->backend->deinit || !d->backend->model_dims) {
		fprintf(stderr, "[maruko-ipu] plugin ABI %u required\n",
			DETECT_PLUGIN_ABI);
		return -1;
	}
	memset(&cfg, 0, sizeof(cfg));
	cfg.model_path = vcfg->detect.model_path;
	cfg.firmware_path = NULL; /* Maruko firmware is embedded. */
	cfg.display_w = (int)display_w;
	cfg.display_h = (int)display_h;
	cfg.conf_thresh = vcfg->detect.conf_thresh;
	cfg.nms_iou = vcfg->detect.nms_iou;
	cfg.net_width = d->net_w;
	cfg.net_height = d->net_h;
	if (d->backend->init(&cfg) != 0 || check_model_dims(d) != 0) {
		if (d->backend)
			d->backend->deinit();
		d->backend = NULL;
		return -1;
	}
	return 0;
}

static int enable_tap(MarukoBackendContext *ctx, MarukoIpuDetect *d)
{
	i6c_scl_port p;
	MI_S32 rc;

	memset(&p, 0, sizeof(p));
	p.crop.x = (MI_U16)ctx->scl_crop_x;
	p.crop.y = (MI_U16)ctx->scl_crop_y;
	p.crop.width = (MI_U16)ctx->scl_crop_w;
	p.crop.height = (MI_U16)ctx->scl_crop_h;
	p.output.width = (MI_U16)d->net_w;
	p.output.height = (MI_U16)d->net_h;
	p.pixFmt = I6_PIXFMT_YUV420SP;
	p.compress = (i6_common_compr)0;
	rc = g_mi_scl.fnSetPortConfig(0, 0, MARUKO_DETECT_PORT, &p);
	if (rc == 0)
		rc = g_mi_scl.fnEnablePort(0, 0, MARUKO_DETECT_PORT);
	if (rc != 0) {
		fprintf(stderr, "[maruko-ipu] SCL port3 enable failed: %d\n",
			(int)rc);
		return -1;
	}
	d->port_enabled = 1;
	rc = g_mi_sys.fnSetChnOutputPortDepth(0, &d->port, 2, 4);
	if (rc != 0) {
		fprintf(stderr, "[maruko-ipu] port3 depth failed: %d\n", (int)rc);
		return -1;
	}
	return 0;
}

static void publish(MarukoIpuDetect *d, int count)
{
	if (count < 0)
		count = 0;
	if (count > MARUKO_DETECT_SNAP_MAX)
		count = MARUKO_DETECT_SNAP_MAX;
	pthread_mutex_lock(&d->lock);
	memcpy(d->published, d->work, (size_t)count * sizeof(d->work[0]));
	d->pub_count = count;
	d->pub_seq++;
	d->pub_us = wb_monotonic_us();
	d->pub_valid = 1;
	pthread_mutex_unlock(&d->lock);
}

static void *reader_main(void *arg)
{
	MarukoIpuDetect *d = (MarukoIpuDetect *)arg;
	MI_S32 fd = -1;
	uint64_t frame_no = 0;

	if (d->get_fd && d->get_fd(&d->port, &fd) != 0)
		fd = -1;
	while (d->running) {
		DetectBufInfo buf;
		DetectFrame frame;
		MI_S32 handle = -1;
		int count = 0;

		if (fd >= 0) {
			fd_set rfds;
			struct timeval tv = { 0, 50000 };
			FD_ZERO(&rfds);
			FD_SET(fd, &rfds);
			if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0)
				continue;
		}
		memset(&buf, 0, sizeof(buf));
		if (d->get_buf(&d->port, &buf, &handle) != 0) {
			if (fd < 0)
				usleep(1000);
			continue;
		}
		if (d->drain_only || buf.type != MI_SYS_BUFDATA_FRAME ||
		    !buf.frame.phy_addr[0]) {
			d->put_buf(handle);
			continue;
		}
		if (d->infer_interval > 1 &&
		    frame_no++ % (uint64_t)d->infer_interval != 0) {
			d->put_buf(handle);
			continue;
		}
		memset(&frame, 0, sizeof(frame));
		frame.width = d->net_w;
		frame.height = d->net_h;
		frame.stride_y = buf.frame.stride[0] ?
			buf.frame.stride[0] : d->net_w;
		frame.stride_uv = buf.frame.stride[1] ?
			buf.frame.stride[1] : frame.stride_y;
		frame.phy_y = buf.frame.phy_addr[0];
		frame.phy_uv = buf.frame.phy_addr[1] ?
			buf.frame.phy_addr[1] :
			frame.phy_y + (uint64_t)frame.stride_y * frame.height;
		d->processing = 1;
		if (d->backend->process(&frame, d->work,
		    MARUKO_DETECT_SNAP_MAX, &count) == 0)
			publish(d, count);
		d->processing = 0;
		d->put_buf(handle);
	}
	if (fd >= 0 && d->close_fd)
		d->close_fd(fd);
	return NULL;
}

static void sweep(MarukoIpuDetect *d)
{
	int i;

	for (i = 0; i < 32; i++) {
		DetectBufInfo buf;
		MI_S32 handle = -1;
		memset(&buf, 0, sizeof(buf));
		if (d->get_buf(&d->port, &buf, &handle) != 0)
			break;
		d->put_buf(handle);
	}
}

int maruko_ipu_yolo_start(MarukoBackendContext *ctx,
	const VencConfig *vcfg)
{
	MarukoIpuDetect *d;
	uint32_t w, h;

	if (!ctx || !vcfg || !vcfg->detect.enabled || ctx->detect)
		return 0;
	if (resolve_dims(vcfg, &w, &h) != 0)
		return 0;
	d = (MarukoIpuDetect *)calloc(1, sizeof(*d));
	if (!d)
		return 0;
	d->port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_SCL, .device = 0, .channel = 0,
		.port = MARUKO_DETECT_PORT };
	d->net_w = w;
	d->net_h = h;
	d->model_id = vcfg->detect.model_id;
	d->infer_interval = vcfg->detect.infer_interval > 0 ?
		vcfg->detect.infer_interval : 1;
	if (load_sys_symbols(d) != 0 ||
	    load_backend(d, vcfg, vcfg->video0.width, vcfg->video0.height) != 0 ||
	    enable_tap(ctx, d) != 0)
		goto fail;
	pthread_mutex_init(&d->lock, NULL);
	d->running = 1;
	if (pthread_create(&d->reader, NULL, reader_main, d) != 0) {
		pthread_mutex_destroy(&d->lock);
		d->running = 0;
		goto fail;
	}
	d->reader_started = 1;
	ctx->detect = d;
	fprintf(stderr, "[maruko-ipu] active: backend=%s tap=SCL0P3 %ux%u "
		"interval=%d model_id=%u\n", d->backend->name, w, h,
		d->infer_interval, d->model_id);
	return 0;

fail:
	if (d->port_enabled) {
		g_mi_sys.fnSetChnOutputPortDepth(0, &d->port, 0, 0);
		g_mi_scl.fnDisablePort(0, 0, MARUKO_DETECT_PORT);
	}
	if (d->backend)
		d->backend->deinit();
	if (d->plugin)
		dlclose(d->plugin);
	free(d);
	fprintf(stderr, "[maruko-ipu] detection disabled\n");
	return 0;
}

void maruko_ipu_yolo_stop(MarukoBackendContext *ctx)
{
	MarukoIpuDetect *d;
	int i;

	if (!ctx || !ctx->detect)
		return;
	d = (MarukoIpuDetect *)ctx->detect;
	ctx->detect = NULL;
	d->drain_only = 1;
	for (i = 0; i < 200 && d->processing; i++)
		usleep(2000);
	if (d->port_enabled) {
		g_mi_sys.fnSetChnOutputPortDepth(0, &d->port, 0, 0);
		g_mi_scl.fnDisablePort(0, 0, MARUKO_DETECT_PORT);
		d->port_enabled = 0;
	}
	usleep(30000);
	d->running = 0;
	if (d->reader_started)
		pthread_join(d->reader, NULL);
	sweep(d);
	if (d->backend)
		d->backend->deinit();
	if (d->plugin)
		dlclose(d->plugin);
	pthread_mutex_destroy(&d->lock);
	free(d);
}

int maruko_ipu_yolo_reload(MarukoBackendContext *ctx,
	const VencConfig *vcfg)
{
	if (!ctx || !vcfg)
		return -1;
	maruko_ipu_yolo_stop(ctx);
	return maruko_ipu_yolo_start(ctx, vcfg);
}

int maruko_ipu_yolo_snapshot(MarukoBackendContext *ctx,
	MarukoDetectSnapshot *out)
{
	MarukoIpuDetect *d;
	int have = 0;

	if (!ctx || !out || !ctx->detect)
		return 0;
	d = (MarukoIpuDetect *)ctx->detect;
	pthread_mutex_lock(&d->lock);
	if (d->pub_valid) {
		int n = d->pub_count;
		memcpy(out->boxes, d->published, (size_t)n * sizeof(out->boxes[0]));
		out->count = n;
		out->seq = d->pub_seq;
		out->produced_us = d->pub_us;
		out->net_w = (uint16_t)d->net_w;
		out->net_h = (uint16_t)d->net_h;
		out->model_id = (uint16_t)d->model_id;
		have = 1;
	}
	pthread_mutex_unlock(&d->lock);
	return have;
}
