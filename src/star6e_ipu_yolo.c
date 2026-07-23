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
	int              infer_interval; /* process 1 of every N captured frames */

	pthread_t              reader;
	int                    reader_started;
	volatile sig_atomic_t  running;
	volatile int           pause;
	volatile int           parked;

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
	void *h = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h)
		return -1;
	d->get_fd = (iy_get_fd_fn)dlsym(h, "MI_SYS_GetFd");
	d->close_fd = (iy_close_fd_fn)dlsym(h, "MI_SYS_CloseFd");
	d->get_buf = (iy_get_buf_fn)dlsym(h, "MI_SYS_ChnOutputPortGetBuf");
	d->put_buf = (iy_put_buf_fn)dlsym(h, "MI_SYS_ChnOutputPortPutBuf");
	/* handle intentionally leaked: libmi_sys stays resident for the run */
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

int star6e_ipu_yolo_start(Star6ePipelineState *state, const VencConfig *vcfg)
{
	Star6eIpuDetect *d;
	MI_VPE_PortAttr_t port;
	MI_SYS_ChnPort_t vpe1 = {
		.module = I6_SYS_MOD_VPE, .device = 0, .channel = 0, .port = 1 };
	uint32_t net_w, net_h;
	MI_S32 ret;

	if (!state || !vcfg || !vcfg->detect.enabled)
		return 0;

	/* Tap geometry follows the compiled model, so a higher-resolution
	 * .img needs no code change.  Both dims must be a multiple of 32:
	 * the YOLO head strides 8/16/32, and a ragged grid would not match
	 * the anchor count the decode derives. */
	net_w = vcfg->detect.net_width  ? vcfg->detect.net_width  : 640;
	net_h = vcfg->detect.net_height ? vcfg->detect.net_height : 352;
	if (net_w % 32 || net_h % 32 || net_w < 64 || net_h < 64) {
		fprintf(stderr, "[ipu-yolo] detect.netWidth/netHeight %ux%u "
			"must be multiples of 32 (>=64); detection disabled\n",
			net_w, net_h);
		return 0;
	}
	if (state->detect)
		return 0;   /* already running */

	d = (Star6eIpuDetect *)calloc(1, sizeof(*d));
	if (!d) {
		fprintf(stderr, "[ipu-yolo] alloc failed\n");
		return 0;   /* best-effort: never fatal to the stream */
	}
	d->vpe1_port = vpe1;
	d->net_w = net_w;
	d->net_h = net_h;
	d->infer_interval = vcfg->detect.infer_interval > 0
		? vcfg->detect.infer_interval : 1;

	{
		const char *plugin = vcfg->detect.plugin[0]
			? vcfg->detect.plugin : "/root/libwaybeam_detect.so";
		WaybeamDetectEntryFn entry;

		d->plugin_handle = dlopen(plugin, RTLD_NOW | RTLD_GLOBAL);
		if (!d->plugin_handle) {
			fprintf(stderr, "[ipu-yolo] dlopen plugin '%s' failed: "
				"%s; detection disabled\n", plugin, dlerror());
			free(d);
			return 0;
		}
		entry = (WaybeamDetectEntryFn)dlsym(d->plugin_handle,
			WAYBEAM_DETECT_ENTRY);
		if (!entry) {
			fprintf(stderr, "[ipu-yolo] plugin '%s' missing entry "
				"'%s'; detection disabled\n", plugin,
				WAYBEAM_DETECT_ENTRY);
			dlclose(d->plugin_handle);
			free(d);
			return 0;
		}
		d->backend = entry();
		if (!d->backend || d->backend->abi != DETECT_PLUGIN_ABI) {
			fprintf(stderr, "[ipu-yolo] plugin '%s' ABI mismatch "
				"(got %u, want %u); detection disabled\n", plugin,
				d->backend ? d->backend->abi : 0u,
				DETECT_PLUGIN_ABI);
			dlclose(d->plugin_handle);
			free(d);
			return 0;
		}
	}

	if (iy_load_sys_symbols(d) != 0) {
		fprintf(stderr, "[ipu-yolo] MI_SYS symbols unavailable\n");
		dlclose(d->plugin_handle);
		free(d);
		return 0;
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
		fprintf(stderr, "[ipu-yolo] VPE port1 %ux%u tap unavailable "
			"(%d); detection disabled\n", net_w, net_h, (int)ret);
		dlclose(d->plugin_handle);
		free(d);
		return 0;
	}
	MI_SYS_SetChnOutputPortDepth(&vpe1, 2, 4);

	{
		DetectBackendConfig cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.model_path = vcfg->detect.model_path;
		cfg.firmware_path = vcfg->detect.firmware_path;
		cfg.display_w = (int)state->image_width;
		cfg.display_h = (int)state->image_height;
		/* 0 leaves the plugin on its own defaults (0.40 / 0.45). */
		cfg.conf_thresh = vcfg->detect.conf_thresh;
		cfg.nms_iou = vcfg->detect.nms_iou;
		if (d->backend->init(&cfg) != 0) {
			fprintf(stderr, "[ipu-yolo] backend '%s' init failed; "
				"detection disabled\n", d->backend->name);
			MI_VPE_DisablePort(0, 1);
			dlclose(d->plugin_handle);
			free(d);
			return 0;
		}
	}

	pthread_mutex_init(&d->pub_lock, NULL);

	d->running = 1;
	if (pthread_create(&d->reader, NULL, iy_reader_main, d) != 0) {
		fprintf(stderr, "[ipu-yolo] reader thread create failed\n");
		d->running = 0;
		pthread_mutex_destroy(&d->pub_lock);
		d->backend->deinit();
		MI_VPE_DisablePort(0, 1);
		dlclose(d->plugin_handle);
		free(d);
		return 0;
	}
	d->reader_started = 1;

	state->detect = d;
	fprintf(stderr, "[ipu-yolo] detection active: backend=%s tap=VPE0P1 "
		"%ux%u interval=%d conf=%.2f iou=%.2f\n", d->backend->name,
		net_w, net_h, d->infer_interval,
		(double)vcfg->detect.conf_thresh,
		(double)vcfg->detect.nms_iou);
	return 0;
}

void star6e_ipu_yolo_stop(Star6ePipelineState *state)
{
	Star6eIpuDetect *d;
	int i;

	if (!state || !state->detect)
		return;
	d = (Star6eIpuDetect *)state->detect;
	state->detect = NULL;

	if (d->reader_started) {
		/* Park the reader out of any GetBuf/PutBuf, then join before
		 * touching the port (MMU-safe ordering). */
		d->pause = 1;
		for (i = 0; i < 100 && !d->parked; i++)
			usleep(2000);
		d->running = 0;
		pthread_join(d->reader, NULL);
	}

	if (d->backend)
		d->backend->deinit();

	if (d->port1_enabled)
		MI_VPE_DisablePort(0, 1);

	if (d->plugin_handle)
		dlclose(d->plugin_handle);

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
	if (!d)
		return 0;

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
		have = 1;
	}
	pthread_mutex_unlock(&d->pub_lock);
	return have;
}
