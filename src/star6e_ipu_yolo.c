/*
 * star6e_ipu_yolo.c — host side of the IPU YOLO detector.
 *
 * Creates a VPE port1 tap (model-input NV12) and runs a reader thread that
 * hands each captured frame's physical address to a detector backend
 * (detect_plugin.h).  The backend owns the IPU inference and — in the Phase-A
 * "worker" backend — its own MI_RGN box overlay, so the host only pumps
 * frames and observes results.
 *
 * Port1 is shared with the stab framing tap (a single physical VPE output),
 * so the caller starts detection only when no port1-owning framing module is
 * active; see star6e_pipeline.c.
 *
 * Teardown obeys the same rule as the stab tap: the reader thread must be
 * joined (via a pause/park handshake) BEFORE MI_VPE_DisablePort(0,1), or an
 * in-flight IPU DMA read races the port free -> MMU fault -> watchdog reset.
 */

#include "star6e_ipu_yolo.h"
#include "star6e_pipeline.h"
#include "detect_plugin.h"
#include "rtp_sidecar.h"   /* RTP_SIDECAR_DETECT_MODEL_* registry */
#include "timing.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

/* ---- local MI_SYS output-buffer mirror (layout copied from the proven stab
 * definition; only the fields we read are named). ------------------------- */
typedef MI_U64 IY_PHY;

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
	IY_PHY phyAddr[3];
	MI_U32 u32Stride[3];
	MI_U32 u32BufSize;
	MI_U16 u16RingBufStartLine;
	MI_U16 u16RingBufRealTotalHeight;
	struct {
		int eType;
		union { MI_U32 u32GlobalGradient; } uIspInfo;
	} stFrameIspInfo;
	MI_U8 reserved_rect[16];
} IyFrameData_t;

typedef struct {
	MI_U64 u64Pts;
	MI_U64 u64SidebandMsg;
	int eBufType;
	MI_BOOL bEndOfStream;
	MI_BOOL bUsrBuf;
	MI_U32 u32SequenceNumber;
	MI_BOOL bDrop;
	union {
		IyFrameData_t stFrameData;
		MI_U8 reserved_union[512];
	};
	MI_U8 u8CusFlag;
} IyBufInfo_t;

typedef MI_S32 IyBufHandle_t;

#define IY_E_BUFDATA_FRAME 1

typedef MI_S32 (*iy_get_fd_fn)(MI_SYS_ChnPort_t *port, MI_S32 *fd);
typedef MI_S32 (*iy_close_fd_fn)(MI_S32 fd);
typedef MI_S32 (*iy_get_buf_fn)(MI_SYS_ChnPort_t *port, IyBufInfo_t *buf,
	IyBufHandle_t *handle);
typedef MI_S32 (*iy_put_buf_fn)(IyBufHandle_t handle);

/* Detector runtime context (heap-allocated, hung off Star6ePipelineState so it
 * does not grow VencConfig). */
typedef struct Star6eIpuDetect {
	const DetectBackend *backend;
	void            *plugin_handle;  /* dlopen'd detector plugin .so */
	MI_SYS_ChnPort_t vpe1_port;      /* {VPE,0,0,1} */
	int              port1_enabled;
	uint32_t         net_w;
	uint32_t         net_h;
	uint32_t         model_id;       /* class-table id of the loaded model;
	                                    set at load, stamped into the snapshot
	                                    so boxes + model_id stay latched */
	int              infer_interval; /* process 1 of every N captured frames */
	/* What is actually loaded, so a reload request can tell an .img change
	 * (needs the NPU graph rebuilt) from a label/threshold change (does not). */
	char             model_path[VENC_CONFIG_STRING_MAX];
	float            conf_thresh;
	float            nms_iou;

	pthread_t              reader;
	int                    reader_started;
	volatile sig_atomic_t  running;
	volatile int           pause;
	volatile int           parked;
	/* Consumer quiesce: set across a live model swap (star6e_ipu_yolo_reload)
	 * so a snapshot() query never sees a half-loaded graph.  The swap itself
	 * runs on the pipeline (encode) thread — the same thread that queries the
	 * snapshot — so this also covers any future off-thread consumer. */
	volatile int           paused;

	iy_get_fd_fn   get_fd;
	iy_close_fd_fn close_fd;
	iy_get_buf_fn  get_buf;
	iy_put_buf_fn  put_buf;

	DetectBox boxes[64];

	/* Latest-detection snapshot published by the reader for the encode
	 * thread (sidecar DETECT trailer).  The lock is held only for the
	 * ≤64-box memcpy on each side — never across the IPU invoke. */
	pthread_mutex_t pub_lock;
	DetectBox       pub_boxes[STAR6E_DETECT_SNAP_MAX];
	int             pub_count;
	uint32_t        pub_seq;
	uint64_t        pub_produced_us;
	int             pub_valid;
} Star6eIpuDetect;

#define IY_MAX_DETS ((int)(sizeof(((Star6eIpuDetect *)0)->boxes) / \
	sizeof(DetectBox)))

static int iy_load_sys_symbols(Star6eIpuDetect *d)
{
	/* libmi_sys stays resident for the whole run, so open it once and
	 * cache the handle: a fresh dlopen on every detect start (each SIGHUP
	 * reinit) would bump the refcount unboundedly and never dlclose. */
	static void *sys_h;
	if (!sys_h) {
		sys_h = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
		if (!sys_h)
			return -1;
	}
	d->get_fd = (iy_get_fd_fn)dlsym(sys_h, "MI_SYS_GetFd");
	d->close_fd = (iy_close_fd_fn)dlsym(sys_h, "MI_SYS_CloseFd");
	d->get_buf = (iy_get_buf_fn)dlsym(sys_h, "MI_SYS_ChnOutputPortGetBuf");
	d->put_buf = (iy_put_buf_fn)dlsym(sys_h, "MI_SYS_ChnOutputPortPutBuf");
	if (!d->get_buf || !d->put_buf)
		return -1;
	return 0;
}

static void *iy_reader_main(void *arg)
{
	Star6eIpuDetect *d = (Star6eIpuDetect *)arg;
	MI_S32 fd = -1;
	uint64_t frame_no = 0;

	if (d->get_fd && d->get_fd(&d->vpe1_port, &fd) != 0)
		fd = -1;

	while (d->running) {
		IyBufInfo_t buf;
		IyBufHandle_t handle = 0;
		uint64_t phy_y, phy_uv;
		int ndet = 0;
		DetectFrame frame;

		/* Quiesce handshake: park outside GetBuf/PutBuf so stop() can
		 * DisablePort without racing the drain. */
		if (d->pause) {
			d->parked = 1;
			usleep(2000);
			continue;
		}
		d->parked = 0;

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
		if (d->get_buf(&d->vpe1_port, &buf, &handle) != 0) {
			if (fd < 0)
				usleep(1000);
			continue;
		}

		if (buf.eBufType != IY_E_BUFDATA_FRAME ||
		    !buf.stFrameData.phyAddr[0]) {
			d->put_buf(handle);
			continue;
		}

		/* Decimate: cheap skip keeps the port draining without running
		 * inference on every frame. */
		if (d->infer_interval > 1 &&
		    (frame_no++ % (uint64_t)d->infer_interval) != 0) {
			d->put_buf(handle);
			continue;
		}

		phy_y = buf.stFrameData.phyAddr[0];
		/* NV12: UV plane follows Y.  Prefer the reported addr; fall back
		 * to Y + strideY*height when the SDK leaves plane 1 zero. */
		phy_uv = buf.stFrameData.phyAddr[1]
			? buf.stFrameData.phyAddr[1]
			: phy_y + (uint64_t)buf.stFrameData.u32Stride[0] *
			  buf.stFrameData.u16Height;

		frame.width = d->net_w;
		frame.height = d->net_h;
		frame.stride_y = buf.stFrameData.u32Stride[0]
			? buf.stFrameData.u32Stride[0] : d->net_w;
		frame.stride_uv = buf.stFrameData.u32Stride[1]
			? buf.stFrameData.u32Stride[1] : frame.stride_y;
		frame.phy_y = phy_y;
		frame.phy_uv = phy_uv;

		if (d->backend->process(&frame, d->boxes, IY_MAX_DETS,
		                        &ndet) == 0) {
			/* Publish the snapshot for the encode thread's sidecar
			 * DETECT trailer.  The worker backend also renders its
			 * own overlay; this just exposes the same boxes to the
			 * host without a second inference. */
			if (ndet < 0)
				ndet = 0;
			if (ndet > IY_MAX_DETS)
				ndet = IY_MAX_DETS;
			pthread_mutex_lock(&d->pub_lock);
			memcpy(d->pub_boxes, d->boxes,
			       (size_t)ndet * sizeof(DetectBox));
			d->pub_count = ndet;
			d->pub_seq++;
			d->pub_produced_us = wb_monotonic_us();
			d->pub_valid = 1;
			pthread_mutex_unlock(&d->pub_lock);
		}

		d->put_buf(handle);
	}

	if (fd >= 0 && d->close_fd)
		d->close_fd(fd);
	return NULL;
}

/* Class count each registered model_id implies (protocols/rtp-sidecar.md).
 * Returns 0 for ids we do not know, which disables the cross-check rather
 * than guessing — a private model_id is legitimate. */
static int iy_model_id_classes(uint32_t model_id)
{
	switch (model_id) {
	case RTP_SIDECAR_DETECT_MODEL_VISDRONE: return 10;
	case RTP_SIDECAR_DETECT_MODEL_PERSON:   return 1;
	default:                                return 0;
	}
}

/* Warn (once per load) when the configured model_id's class count disagrees
 * with what the loaded model reports.  model_id is configured, not derived —
 * the plugin loads whatever .img it is pointed at — so swapping the model
 * without updating the config silently relabels every box against the wrong
 * class table.  The plugin knows its class count, so cross-check that much: it
 * catches the realistic mistake (a 1-class person model still announcing
 * itself as VisDrone-10) without pretending to validate the identity we cannot
 * actually verify. */
static void iy_check_model_id(const Star6eIpuDetect *d, uint32_t model_id)
{
	const char *const *names = NULL;
	int have, want;

	if (!d->backend || !d->backend->describe)
		return;
	have = d->backend->describe(&names);
	want = iy_model_id_classes(model_id);
	if (have > 0 && want > 0 && have != want)
		fprintf(stderr, "[ipu-yolo] WARNING: model_id=%u expects %d classes "
			"but the model has %d — consumers will mislabel every box; "
			"fix detect.modelId\n", model_id, want, have);
}

/* Verify the loaded model's REAL input geometry against the port1 tap that was
 * created for it.  This is the only check that can catch an operator pointing
 * detect.model_path at a .img compiled for different dims: config is not
 * evidence of what the model wants, and the failure is silent either way: a
 * backend that enforces its frame contract (as the native one does) rejects
 * every frame, and process() errors are not logged, so the detector reports
 * "active" and simply never detects; one that does not enforce it decodes in
 * the wrong coordinate space and misplaces every box.  Because model_path is
 * live-swappable but net_width/net_height are restart-scope, that skew is easy
 * to reach by accident.
 *
 * Returns 0 when the model agrees, -1 to refuse the load.  A backend that
 * cannot report its dims is refused too: "unverified" is exactly the state this
 * check exists to eliminate, and model_dims() is mandatory at this ABI. */
static int iy_check_model_dims(const Star6eIpuDetect *d, uint32_t net_w,
	uint32_t net_h)
{
	uint32_t mw = 0, mh = 0;

	if (d->backend->model_dims(&mw, &mh) != 0 || mw == 0 || mh == 0) {
		fprintf(stderr, "[ipu-yolo] backend '%s' could not report its model "
			"input dims — refusing, the %ux%u tap cannot be verified\n",
			d->backend->name, net_w, net_h);
		return -1;
	}
	if (mw != net_w || mh != net_h) {
		fprintf(stderr, "[ipu-yolo] model geometry mismatch: '%s' expects "
			"%ux%u but the tap is %ux%u — refusing, it would run without "
			"ever detecting.  Set detect.netWidth=%u and "
			"detect.netHeight=%u (both restart-scope) to use this model.\n",
			d->backend->name, mw, mh, net_w, net_h, mw, mh);
		return -1;
	}
	return 0;
}

/* Resolve + validate the tap/model dims from config.  Tap geometry follows the
 * compiled model, so a higher-resolution .img needs no code change.  Both dims
 * must be a multiple of 32 (>=64): the YOLO head strides 8/16/32, and a ragged
 * grid would not match the anchor count the decode derives.  Returns 0 and
 * writes the resolved dims on success, -1 (with a diag) when the geometry is
 * rejected. */
static int iy_resolve_net_dims(const VencConfig *vcfg, uint32_t *net_w,
	uint32_t *net_h)
{
	uint32_t w = vcfg->detect.net_width  ? vcfg->detect.net_width  : 640;
	uint32_t h = vcfg->detect.net_height ? vcfg->detect.net_height : 352;

	if (w % 32 || h % 32 || w < 64 || h < 64) {
		fprintf(stderr, "[ipu-yolo] detect.netWidth/netHeight %ux%u must be "
			"multiples of 32 (>=64); detection disabled\n", w, h);
		return -1;
	}
	*net_w = w;
	*net_h = h;
	return 0;
}

/* Load the model-specific half of the detector: dlopen the plugin, resolve +
 * ABI-check the backend, create the VPE port1 tap, and init the backend on the
 * configured model.  On any failure everything this call opened is torn back
 * down (so the context is left with backend/plugin_handle/port1_enabled clear)
 * and -1 is returned.  net_w/net_h are the already-validated tap dims. */
static int iy_load_graph(Star6eIpuDetect *d, const VencConfig *vcfg,
	uint32_t net_w, uint32_t net_h, uint32_t disp_w, uint32_t disp_h)
{
	const char *plugin = vcfg->detect.plugin[0]
		? vcfg->detect.plugin : "/root/libwaybeam_detect.so";
	WaybeamDetectEntryFn entry;
	MI_VPE_PortAttr_t port;
	MI_S32 ret;

	d->plugin_handle = dlopen(plugin, RTLD_NOW | RTLD_GLOBAL);
	if (!d->plugin_handle) {
		fprintf(stderr, "[ipu-yolo] dlopen plugin '%s' failed: %s\n",
			plugin, dlerror());
		return -1;
	}
	entry = (WaybeamDetectEntryFn)dlsym(d->plugin_handle,
		WAYBEAM_DETECT_ENTRY);
	if (!entry) {
		fprintf(stderr, "[ipu-yolo] plugin '%s' missing entry '%s'\n",
			plugin, WAYBEAM_DETECT_ENTRY);
		goto fail_plugin;
	}
	d->backend = entry();
	if (!d->backend || d->backend->abi != DETECT_PLUGIN_ABI) {
		fprintf(stderr, "[ipu-yolo] plugin '%s' ABI mismatch (got %u, "
			"want %u)\n", plugin, d->backend ? d->backend->abi : 0u,
			DETECT_PLUGIN_ABI);
		d->backend = NULL;
		goto fail_plugin;
	}
	/* model_dims() is mandatory at this ABI: without it the tap geometry
	 * cannot be verified against the model, and a mismatch is silent. */
	if (!d->backend->model_dims) {
		fprintf(stderr, "[ipu-yolo] plugin '%s' claims ABI %u but has no "
			"model_dims()\n", plugin, DETECT_PLUGIN_ABI);
		d->backend = NULL;
		goto fail_plugin;
	}

	if (iy_load_sys_symbols(d) != 0) {
		fprintf(stderr, "[ipu-yolo] MI_SYS symbols unavailable\n");
		d->backend = NULL;
		goto fail_plugin;
	}

	/* port1: full-frame scaled to the model input, NV12. */
	memset(&port, 0, sizeof(port));
	port.output.width = (unsigned short)net_w;
	port.output.height = (unsigned short)net_h;
	port.pixFmt = I6_PIXFMT_YUV420SP;
	port.compress = I6_COMPR_NONE;
	ret = MI_VPE_SetPortMode(0, 1, &port);
	if (ret == 0) {
		ret = MI_VPE_EnablePort(0, 1);
		if (ret == 0)
			d->port1_enabled = 1;
	}
	if (!d->port1_enabled) {
		fprintf(stderr, "[ipu-yolo] VPE port1 %ux%u tap unavailable (%d)\n",
			net_w, net_h, (int)ret);
		d->backend = NULL;
		goto fail_plugin;
	}
	MI_SYS_SetChnOutputPortDepth(&d->vpe1_port, 2, 4);

	{
		DetectBackendConfig cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.model_path = vcfg->detect.model_path;
		cfg.firmware_path = vcfg->detect.firmware_path;
		cfg.display_w = (int)disp_w;
		cfg.display_h = (int)disp_h;
		/* 0 leaves the plugin on its own defaults (0.40 / 0.45). */
		cfg.conf_thresh = vcfg->detect.conf_thresh;
		cfg.nms_iou = vcfg->detect.nms_iou;
		/* ABI 2: the tap geometry, so a backend can refuse a model it
		 * cannot be fed (the host checks independently below anyway). */
		cfg.net_width = net_w;
		cfg.net_height = net_h;
		if (d->backend->init(&cfg) != 0) {
			fprintf(stderr, "[ipu-yolo] backend '%s' init failed\n",
				d->backend->name);
			MI_VPE_DisablePort(0, 1);
			d->port1_enabled = 0;
			d->backend = NULL;
			goto fail_plugin;
		}
		if (iy_check_model_dims(d, net_w, net_h) != 0) {
			d->backend->deinit();
			MI_VPE_DisablePort(0, 1);
			d->port1_enabled = 0;
			d->backend = NULL;
			goto fail_plugin;
		}
		/* Record what is now loaded so a later reload request can tell an
		 * .img change from a label/threshold change. */
		snprintf(d->model_path, sizeof(d->model_path), "%s",
			vcfg->detect.model_path);
		d->conf_thresh = vcfg->detect.conf_thresh;
		d->nms_iou = vcfg->detect.nms_iou;
	}
	return 0;

fail_plugin:
	dlclose(d->plugin_handle);
	d->plugin_handle = NULL;
	return -1;
}

/* Tear down the model-specific half loaded by iy_load_graph.  The reader
 * thread MUST already be stopped/joined (no in-flight port1 DMA) before this
 * runs, or DisablePort races the IPU read -> MMU fault. */
static void iy_unload_graph(Star6eIpuDetect *d)
{
	if (d->backend) {
		d->backend->deinit();
		d->backend = NULL;
	}
	if (d->port1_enabled) {
		MI_VPE_DisablePort(0, 1);
		d->port1_enabled = 0;
	}
	if (d->plugin_handle) {
		dlclose(d->plugin_handle);
		d->plugin_handle = NULL;
	}
}

/* Park the reader out of any GetBuf/PutBuf, then join it — MMU-safe ordering:
 * the reader is guaranteed out of the port1 GetBuf/PutBuf window before the
 * caller unloads the graph (DisablePort). */
static void iy_stop_reader(Star6eIpuDetect *d)
{
	int i;

	if (!d->reader_started)
		return;
	d->pause = 1;
	for (i = 0; i < 100 && !d->parked; i++)
		usleep(2000);
	d->running = 0;
	pthread_join(d->reader, NULL);
	d->reader_started = 0;
}

int star6e_ipu_yolo_start(Star6ePipelineState *state, const VencConfig *vcfg)
{
	Star6eIpuDetect *d;
	uint32_t net_w, net_h;

	if (!state || !vcfg || !vcfg->detect.enabled)
		return 0;
	if (iy_resolve_net_dims(vcfg, &net_w, &net_h) != 0)
		return 0;
	if (state->detect)
		return 0;   /* already running */

	d = (Star6eIpuDetect *)calloc(1, sizeof(*d));
	if (!d) {
		fprintf(stderr, "[ipu-yolo] alloc failed\n");
		return 0;   /* best-effort: never fatal to the stream */
	}
	d->vpe1_port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0, .port = 1 };
	d->net_w = net_w;
	d->net_h = net_h;
	d->model_id = vcfg->detect.model_id;
	d->infer_interval = vcfg->detect.infer_interval > 0
		? vcfg->detect.infer_interval : 1;

	if (iy_load_graph(d, vcfg, net_w, net_h, state->image_width,
	    state->image_height) != 0) {
		fprintf(stderr, "[ipu-yolo] detection disabled\n");
		free(d);
		return 0;
	}

	pthread_mutex_init(&d->pub_lock, NULL);

	d->running = 1;
	if (pthread_create(&d->reader, NULL, iy_reader_main, d) != 0) {
		fprintf(stderr, "[ipu-yolo] reader thread create failed\n");
		d->running = 0;
		pthread_mutex_destroy(&d->pub_lock);
		iy_unload_graph(d);
		free(d);
		return 0;
	}
	d->reader_started = 1;

	state->detect = d;
	fprintf(stderr, "[ipu-yolo] detection active: backend=%s tap=VPE0P1 "
		"%ux%u interval=%d conf=%.2f iou=%.2f model_id=%u\n",
		d->backend->name, net_w, net_h, d->infer_interval,
		(double)vcfg->detect.conf_thresh,
		(double)vcfg->detect.nms_iou, vcfg->detect.model_id);
	iy_check_model_id(d, vcfg->detect.model_id);
	return 0;
}

int star6e_ipu_yolo_reload(Star6ePipelineState *state, const VencConfig *vcfg)
{
	Star6eIpuDetect *d;
	uint32_t net_w, net_h;

	if (!state || !vcfg)
		return -1;
	d = (Star6eIpuDetect *)state->detect;
	if (!d)
		return -1;   /* nothing running to swap — caller keeps config */

	if (iy_resolve_net_dims(vcfg, &net_w, &net_h) != 0)
		return -1;
	/* Geometry guard: the port output w/h is fixed at create, so a dims
	 * change needs the port recreated under a full MUT_RESTART, not this
	 * live path.  (netWidth/netHeight are MUT_RESTART fields, so this is
	 * defence-in-depth — a live model swap never carries a dims change.) */
	if (net_w != d->net_w || net_h != d->net_h) {
		fprintf(stderr, "[ipu-yolo] live reload skipped: net geometry "
			"%ux%u -> %ux%u needs restart\n",
			d->net_w, d->net_h, net_w, net_h);
		return -1;
	}

	/* Only a different .img actually needs the NPU graph rebuilt.  model_id is
	 * just a label stamped into the snapshot, and conf/iou are decode-time
	 * knobs, so rebuilding the graph for those would cost hundreds of ms to
	 * seconds of frame output (the load runs on this, the pipeline, thread) for
	 * no reason — and threshold tuning is inherently interactive.  Apply them
	 * in place instead, leaving the graph and the tap untouched. */
	if (strcmp(vcfg->detect.model_path, d->model_path) == 0) {
		int th_changed = (vcfg->detect.conf_thresh != d->conf_thresh) ||
			(vcfg->detect.nms_iou != d->nms_iou);

		if (!th_changed || d->backend->set_thresholds) {
			if (th_changed) {
				/* Park the reader so the decode is not reading the
				 * thresholds while they are written.  It parks between
				 * frames, so this costs at most one inference — and the
				 * graph is never torn down, so there is no half-loaded
				 * state for snapshot() to guard against (hence no
				 * d->paused / pub_valid reset here). */
				int i;
				if (d->reader_started) {
					d->pause = 1;
					for (i = 0; i < 100 && !d->parked; i++)
						usleep(2000);
				}
				d->backend->set_thresholds(vcfg->detect.conf_thresh,
					vcfg->detect.nms_iou);
				d->conf_thresh = vcfg->detect.conf_thresh;
				d->nms_iou = vcfg->detect.nms_iou;
				if (d->reader_started)
					d->pause = 0;
			}
			/* Same model, so the boxes in flight are already this model's:
			 * stamping the new id needs no latch window.  Written from the
			 * pipeline thread, which is also the only snapshot() caller. */
			d->model_id = vcfg->detect.model_id;
			fprintf(stderr, "[ipu-yolo] applied in place (no model reload): "
				"conf=%.2f iou=%.2f model_id=%u\n",
				(double)vcfg->detect.conf_thresh,
				(double)vcfg->detect.nms_iou, vcfg->detect.model_id);
			iy_check_model_id(d, vcfg->detect.model_id);
			return 0;
		}
		/* Thresholds changed but the backend cannot set them live — fall
		 * through to the full reload, which picks them up via init(). */
	}

	/* Quiesce the consumer and drop the stale snapshot so no old-model boxes
	 * (or a half-loaded graph) are ever queried during the swap.  The DETECT
	 * sidecar trailer is simply absent for the swap window — consumers
	 * already tolerate "no DETECT". */
	d->paused = 1;
	pthread_mutex_lock(&d->pub_lock);
	d->pub_valid = 0;
	pthread_mutex_unlock(&d->pub_lock);

	iy_stop_reader(d);
	iy_unload_graph(d);

	d->infer_interval = vcfg->detect.infer_interval > 0
		? vcfg->detect.infer_interval : 1;

	if (iy_load_graph(d, vcfg, net_w, net_h, state->image_width,
	    state->image_height) != 0) {
		/* New model would not load — leave detection cleanly OFF (the
		 * encode path stops emitting DETECT) rather than half-up.  This
		 * runs on the pipeline thread, so clearing state->detect + freeing
		 * here cannot race the same thread's next snapshot() read. */
		fprintf(stderr, "[ipu-yolo] live reload failed; detection disabled\n");
		state->detect = NULL;
		pthread_mutex_destroy(&d->pub_lock);
		free(d);
		return -1;
	}

	d->running = 1;
	d->pause = 0;
	d->parked = 0;
	if (pthread_create(&d->reader, NULL, iy_reader_main, d) != 0) {
		fprintf(stderr, "[ipu-yolo] reload: reader thread create failed; "
			"detection disabled\n");
		d->running = 0;
		iy_unload_graph(d);
		state->detect = NULL;
		pthread_mutex_destroy(&d->pub_lock);
		free(d);
		return -1;
	}
	d->reader_started = 1;
	/* Latch the new model_id before unpausing the consumer: paused was held
	 * across the whole swap and pub_valid was cleared, so the first snapshot
	 * the encode thread reads after this carries the new boxes AND this id. */
	d->model_id = vcfg->detect.model_id;
	d->paused = 0;

	fprintf(stderr, "[ipu-yolo] model reloaded live: backend=%s %ux%u "
		"conf=%.2f iou=%.2f model_id=%u\n", d->backend->name, net_w, net_h,
		(double)vcfg->detect.conf_thresh, (double)vcfg->detect.nms_iou,
		vcfg->detect.model_id);
	iy_check_model_id(d, vcfg->detect.model_id);
	return 0;
}

void star6e_ipu_yolo_stop(Star6ePipelineState *state)
{
	Star6eIpuDetect *d;

	if (!state || !state->detect)
		return;
	d = (Star6eIpuDetect *)state->detect;
	state->detect = NULL;

	iy_stop_reader(d);
	iy_unload_graph(d);

	/* Reader is joined; no other user of pub_lock (state->detect is already
	 * NULL, so the encode-thread accessor bails before touching it). */
	pthread_mutex_destroy(&d->pub_lock);

	free(d);
}

int star6e_ipu_yolo_snapshot(Star6ePipelineState *state,
	Star6eDetectSnapshot *out)
{
	Star6eIpuDetect *d;
	int have = 0;

	if (!state || !out)
		return 0;
	d = (Star6eIpuDetect *)state->detect;
	if (!d || d->paused)
		return 0;   /* mid model swap: no half-loaded graph is queried */

	pthread_mutex_lock(&d->pub_lock);
	if (d->pub_valid) {
		int c = d->pub_count;
		if (c < 0)
			c = 0;
		if (c > STAR6E_DETECT_SNAP_MAX)
			c = STAR6E_DETECT_SNAP_MAX;
		memcpy(out->boxes, d->pub_boxes, (size_t)c * sizeof(DetectBox));
		out->count = c;
		out->seq = d->pub_seq;
		out->produced_us = d->pub_produced_us;
		out->net_w = (uint16_t)d->net_w;
		out->net_h = (uint16_t)d->net_h;
		out->model_id = (uint16_t)d->model_id;
		have = 1;
	}
	pthread_mutex_unlock(&d->pub_lock);
	return have;
}
