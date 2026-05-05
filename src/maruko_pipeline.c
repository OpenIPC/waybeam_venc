#include "maruko_pipeline.h"

#include "audio_codec.h"
#include "debug_osd.h"
#include "hevc_rtp.h"
#include "idr_rate_limit.h"
#include "intra_refresh.h"
#include "isp_runtime.h"
#include "maruko_bindings.h"
#include "maruko_config.h"
#include "maruko_controls.h"
#include "maruko_cus3a.h"
#include "maruko_output.h"
#include "maruko_recorder.h"
#include "maruko_ts_recorder.h"
#include "maruko_video.h"
#include "output_socket.h"
#include "pipeline_common.h"
#include "rtp_sidecar.h"
#include "sensor_select.h"
#include "stream_metrics.h"
#include "venc_api.h"
#include "venc_httpd.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "h26x_param_sets.h"
#include "maruko_video.h"
#include "rtp_session.h"
#include "timing.h"

static void idle_wait(RtpSidecarSender *sc, int timeout_ms)
{
	if (!sc || sc->fd < 0) {
		usleep((unsigned)(timeout_ms * 1000));
		return;
	}
	struct pollfd pfd = { .fd = sc->fd, .events = POLLIN };
	if (poll(&pfd, 1, timeout_ms) > 0)
		rtp_sidecar_poll(sc);
}

volatile sig_atomic_t g_maruko_running = 1;
static volatile sig_atomic_t g_maruko_reinit = 0;
static int g_mi_isp_initialized = 0;
/* Last ISP bin path successfully loaded (Star6E parity).  Lets reinit
 * pick up SIGHUP-driven `isp.sensorBin` changes without re-running the
 * one-shot CUS3A enable below (which deadlocks the vendor mutex on a
 * second invocation).  Empty until the first successful load. */
static char g_last_isp_bin_path[256] = {0};
static int g_mi_isp_dev_created = 0;
static int g_mi_scl_dev_created = 0;
static int g_mi_isp_chn_created = 0;
static int g_mi_scl_chn_created = 0;

static int maruko_config_dev_ring_pool(i6c_sys_mod module, MI_U32 device,
	MI_U16 max_width, MI_U16 max_height, MI_U16 ring_line)
{
	if (max_width == 0 || max_height == 0 || ring_line == 0)
		return 0;

	i6c_sys_pool pool;
	memset(&pool, 0, sizeof(pool));
	pool.type = I6C_SYS_POOL_DEVICE_RING;
	pool.create = 1;
	pool.config.ring.module = module;
	pool.config.ring.device = device;
	pool.config.ring.maxWidth = max_width;
	pool.config.ring.maxHeight = max_height;
	pool.config.ring.ringLine = ring_line;

	MI_S32 ret = maruko_mi_sys_config_private_pool(0, &pool);
	if (ret != 0) {
		fprintf(stderr,
			"WARNING: [maruko] MI_SYS_ConfigPrivateMMAPool failed %d"
			" (module=%d dev=%u size=%ux%u ring=%u)\n",
			ret, module, device, max_width, max_height, ring_line);
	} else {
		printf("> [maruko] private ring pool configured"
			" (module=%d dev=%u size=%ux%u ring=%u)\n",
			module, device, max_width, max_height, ring_line);
	}
	return ret;
}

/* Enable CUS3A framework — required for ISP frame processing (without it
 * the ISP FIFO stalls at >=60fps).
 *
 * Always calls:
 *   - MI_ISP_CUS3A_Enable({1,1,0})  — bring up AE+AWB algos in the engine.
 *   - MI_ISP_EnableUserspace3A      — spawns the SDK's 3A_Proc_0 thread,
 *                                     which is what pumps IQ buffer writes
 *                                     (saturation/sharpness/brightness) into
 *                                     ISP HW.  Without this the IQ knobs
 *                                     accept writes but never reach the
 *                                     pipeline.
 *
 * The actual algorithm-throttle decision is made later in pipeline init
 * based on isp.aeMode: in "throttle" mode we additionally install a no-op
 * AE adaptor (see maruko_cus3a_install_noop_adaptor) so 3A_Proc_0's
 * algorithm step becomes free, and the supervisory thread drives AE via
 * SetAeParam at ae_fps Hz.  See HISTORY 0.9.12. */
static void maruko_enable_cus3a(void)
{
	void *h = g_mi_isp.handle;
	if (!h)
		return;

	/* Step 1: enable AE+AWB algos (AF disabled for fixed-focus IMX415).
	 * Engine must be ENABLED so MI_ISP_CUS3A_SetAeParam is honored. */
	typedef int (*cus3a_enable_fn)(MI_U32 dev, MI_U32 chn, void *params);
	cus3a_enable_fn fn_enable = (cus3a_enable_fn)dlsym(h,
		"MI_ISP_CUS3A_Enable");
	if (!fn_enable)
		return;
	MI_BOOL p100[3] = {1, 0, 0};
	MI_BOOL p110[3] = {1, 1, 0};
	fn_enable(0, 0, p100);
	MI_S32 ret = fn_enable(0, 0, p110);
	printf("> [maruko] CUS3A_Enable(1,1,0) ret=%d\n", ret);

	/* Step 2: spawn 3A_Proc_0 thread (drives IQ→HW pump). */
	typedef int (*enable_us3a_fn)(MI_U32 dev, MI_U32 chn);
	enable_us3a_fn fn_enable_us3a = (enable_us3a_fn)dlsym(h,
		"MI_ISP_EnableUserspace3A");
	if (fn_enable_us3a) {
		int us3a_ret = fn_enable_us3a(0, 0);
		printf("> [maruko] EnableUserspace3A ret=%d\n", us3a_ret);
	}

	/* Step 3 (no-op AE adaptor) is conditional on isp.aeMode and runs
	 * later from pipeline init — see the throttle branch there. */
}

static int maruko_disable_userspace3a(const IspRuntimeLib *lib, void *ctx)
{
	maruko_isp_disable_userspace3a_fn_t fn;

	(void)ctx;
	fn = (maruko_isp_disable_userspace3a_fn_t)lib->disable_userspace3a;
	return fn ? fn(0, 0) : 0;
}

static int maruko_call_load_bin(const IspRuntimeLib *lib,
	const char *path, unsigned int load_key, void *ctx)
{
	maruko_isp_load_bin_fn_t fn_api;
	maruko_isp_load_bin_fn_t fn_api_alt;
	int ret;

	(void)ctx;
	fn_api = (maruko_isp_load_bin_fn_t)lib->load_bin_api;
	fn_api_alt = (maruko_isp_load_bin_fn_t)lib->load_bin_api_alt;
	ret = -1;
	if (fn_api)
		ret = fn_api(0, 0, (char *)path, load_key);
	if (ret != 0 && fn_api_alt && fn_api_alt != fn_api)
		ret = fn_api_alt(0, 0, (char *)path, load_key);
	return ret;
}

static void maruko_post_load_cus3a(const IspRuntimeLib *lib, void *ctx)
{
	typedef int (*cus3a_fn_t)(MI_U32 dev_id, MI_U32 channel, void *params);
	cus3a_fn_t fn_cus3a;
	MI_BOOL p100[3] = {1, 0, 0};
	MI_BOOL p110[3] = {1, 1, 0};

	(void)ctx;
	fn_cus3a = (cus3a_fn_t)lib->cus3a_enable;
	if (!fn_cus3a)
		return;

	fn_cus3a(0, 0, p100);
	fn_cus3a(0, 0, p110);
}

static int maruko_load_isp_bin(const char *isp_bin_path)
{
	IspRuntimeLoadHooks hooks;

	memset(&hooks, 0, sizeof(hooks));
	hooks.log_prefix = "[maruko] ";
	hooks.load_key = 1234;
	hooks.disable_userspace3a = maruko_disable_userspace3a;
	hooks.load_bin = maruko_call_load_bin;
	hooks.post_load = maruko_post_load_cus3a;
	int ret = isp_runtime_load_bin_file(isp_bin_path, &hooks);

	/* Also load via MI_ISP_IQ_ApiCmdLoadBinFile to initialize the
	 * IQ parameter subsystem. Without this, MI_ISP_IQ_Set* calls
	 * are accepted but have no effect on the image. The IQ variant
	 * takes raw bin data (not a file path).
	 * NOTE: this second load may reset AE parameters from the API
	 * bin — testing if skipping it fixes dark image issue. */
	if (ret == 0) {
		FILE *f = fopen(isp_bin_path, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long sz = ftell(f);
			fseek(f, 0, SEEK_SET);
			uint8_t *buf = malloc((size_t)sz);
			if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
				typedef int (*iq_load_fn_t)(uint32_t, uint32_t,
					uint8_t *, uint32_t);
				iq_load_fn_t fn = (iq_load_fn_t)dlsym(
					RTLD_DEFAULT,
					"MI_ISP_IQ_ApiCmdLoadBinFile");
				if (fn) {
					/* DISABLED: IQ bin reload may reset
					 * AE params from API bin load above.
					 * Testing if this fixes dark image. */
					printf("> [maruko] IQ bin load: "
						"SKIPPED (testing AE fix)\n");
					(void)fn;
				} else {
					printf("> [maruko] IQ bin load: "
						"symbol not found (skipped)\n");
				}
			}
			free(buf);
			fclose(f);
		}
	}

	return ret;
}

/* maruko_load_symbol, i6c_isp_load/unload, i6c_scl_load/unload moved to
 * maruko_mi.c — ISP/SCL are loaded centrally via maruko_mi_init(). */

static void maruko_handle_signal(int sig)
{
	(void)sig;
	g_maruko_running = 0;
}

static void maruko_handle_sighup(int sig)
{
	(void)sig;
	g_maruko_reinit = 1;
}

void maruko_pipeline_install_signal_handlers(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = maruko_handle_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = maruko_handle_sighup;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGHUP, &sa, NULL);
}

static int maruko_start_vif(const SensorSelectResult *sensor)
{
	MI_S32 ret = 0;
	int group_created = 0;
	int dev_enabled = 0;
	int port_enabled = 0;

	MI_VIF_GroupAttr_t group = {0};
	group.eIntfMode = (MI_VIF_IntfMode_e)sensor->pad.intf;
	group.eWorkMode = E_MI_VIF_WORK_MODE_1MULTIPLEX;
	group.eHDRType = E_MI_VIF_HDR_TYPE_OFF;
	group.u32GroupStitchMask = E_MI_VIF_GROUPMASK_ID0;
	if (sensor->pad.intf == I6_INTF_BT656) {
		group.eClkEdge =
			(MI_VIF_ClkEdge_e)sensor->pad.intfAttr.bt656.edge;
	} else {
		group.eClkEdge = E_MI_VIF_CLK_EDGE_DOUBLE;
	}

	ret = MI_VIF_CreateDevGroup(0, &group);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VIF_CreateDevGroup failed %d\n",
			ret);
		return ret;
	}
	group_created = 1;

	MI_VIF_DevAttr_t dev = {0};
	dev.stInputRect = sensor->plane.capt;
	dev.eField = 0;
	dev.bEnH2T1PMode = 0;
	if (sensor->plane.bayer > I6_BAYER_END) {
		dev.eInputPixel = sensor->plane.pixFmt;
	} else {
		dev.eInputPixel = (i6_common_pixfmt)
			(I6_PIXFMT_RGB_BAYER +
			 sensor->plane.precision * I6_BAYER_END +
			 sensor->plane.bayer);
	}

	printf("> [maruko] VIF dev: inputRect(%u,%u %ux%u) pixel=%d\n",
		dev.stInputRect.x, dev.stInputRect.y,
		dev.stInputRect.width, dev.stInputRect.height,
		dev.eInputPixel);
	ret = MI_VIF_SetDevAttr(0, &dev);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VIF_SetDevAttr failed %d\n", ret);
		goto fail;
	}

	ret = MI_VIF_EnableDev(0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VIF_EnableDev failed %d\n", ret);
		goto fail;
	}
	dev_enabled = 1;

	MI_VIF_OutputPortAttr_t port = {0};
	port.stCapRect = dev.stInputRect;
	port.stDestSize.width = dev.stInputRect.width;
	port.stDestSize.height = dev.stInputRect.height;
	port.ePixFormat = dev.eInputPixel;
	port.eFrameRate = E_MI_VIF_FRAMERATE_FULL;
	port.eCompressMode = E_MI_SYS_COMPRESS_MODE_NONE;

	printf("> [maruko] VIF port: capRect(%u,%u %ux%u) dest(%ux%u) "
		"pixel=%d compress=%d\n",
		port.stCapRect.x, port.stCapRect.y,
		port.stCapRect.width, port.stCapRect.height,
		port.stDestSize.width, port.stDestSize.height,
		port.ePixFormat, port.eCompressMode);
	ret = MI_VIF_SetOutputPortAttr(0, 0, &port);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VIF_SetOutputPortAttr failed %d\n",
			ret);
		goto fail;
	}

	ret = MI_VIF_EnableOutputPort(0, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VIF_EnableOutputPort failed %d\n",
			ret);
		goto fail;
	}
	port_enabled = 1;

	return 0;

fail:
	if (port_enabled)
		(void)MI_VIF_DisableOutputPort(0, 0);
	if (dev_enabled)
		(void)MI_VIF_DisableDev(0);
	if (group_created)
		(void)MI_VIF_DestroyDevGroup(0);
	return ret;
}

static void maruko_stop_vif(void)
{
	(void)MI_VIF_DisableOutputPort(0, 0);
	(void)MI_VIF_DisableDev(0);
	(void)MI_VIF_DestroyDevGroup(0);
}

static int configure_maruko_isp(const SensorSelectResult *sensor,
	int vpe_level_3dnr)
{
	MI_S32 ret = 0;
	int dev = 0, chn = 0, started = 0, port = 0;


	if (!g_mi_isp_dev_created) {
		unsigned int sensor_mask = (1u << (unsigned int)sensor->pad_id);
		ret = g_mi_isp.fnCreateDevice(0, &sensor_mask);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] MI_ISP_CreateDevice failed %d\n", ret);
			goto fail;
		}
		g_mi_isp_dev_created = 1;
	}
	dev = 1;

	if (!g_mi_isp_chn_created) {
		i6c_isp_chn isp_chn = {0};
		/* SigmaStar ISP sensorId is 1-based (pad 0 -> sensorId 1),
		 * matching majestic behavior. pad_id=0 with sensorId=0
		 * causes ISP frame processing to stall at larger resolutions. */
		isp_chn.sensorId = (unsigned int)(sensor->pad_id + 1);
		ret = g_mi_isp.fnCreateChannel(0, 0, &isp_chn);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] MI_ISP_CreateChannel failed %d\n",
				ret);
			goto fail;
		}
		g_mi_isp_chn_created = 1;
	}
	chn = 1;

	{
		i6c_isp_para isp_para;
		memset(&isp_para, 0, sizeof(isp_para));
		isp_para.hdr = I6_HDR_OFF;
		isp_para.level3DNR = vpe_level_3dnr;
		/* Match majestic: set yuv2BayerOn based on sensor bayer type.
		 * Bayer sensors (bayer <= I6_BAYER_END) feed raw Bayer data;
		 * YUV sensors have bayer > I6_BAYER_END. */
		isp_para.yuv2BayerOn =
			(sensor->plane.bayer > I6_BAYER_END) ? 1 : 0;
		printf("> [maruko] ISP params: 3DNR=%d hdr=%d yuv2Bayer=%d\n",
			isp_para.level3DNR, isp_para.hdr,
			isp_para.yuv2BayerOn);
		ret = g_mi_isp.fnSetChannelParam(0, 0, &isp_para);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] MI_ISP_SetChnParam failed %d\n",
				ret);
			goto fail;
		}
	}

	ret = g_mi_isp.fnStartChannel(0, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_ISP_StartChannel failed %d\n",
			ret);
		goto fail;
	}
	started = 1;

	i6c_isp_port isp_port;
	memset(&isp_port, 0, sizeof(isp_port));
	/* Match majestic: ISP output port uses YUV422_YUYV with zero crop
	 * (let SCL handle crop/scale). Setting crop to sensor dimensions
	 * caused ISP frame processing to stall at higher resolutions. */
	isp_port.pixFmt = I6_PIXFMT_YUV422_YUYV;
	isp_port.compress = I6_COMPR_NONE;
	printf("> [maruko] ISP port: crop(%u,%u %ux%u) fmt=%d compress=%d\n",
		isp_port.crop.x, isp_port.crop.y,
		isp_port.crop.width, isp_port.crop.height,
		isp_port.pixFmt, isp_port.compress);
	ret = g_mi_isp.fnSetPortConfig(0, 0, 0, &isp_port);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_ISP_SetOutputPortParam failed %d\n",
			ret);
		goto fail;
	}

	ret = g_mi_isp.fnEnablePort(0, 0, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_ISP_EnableOutputPort failed %d\n",
			ret);
		goto fail;
	}
	port = 1;

	return 0;

fail:
	if (port)
		(void)g_mi_isp.fnDisablePort(0, 0, 0);
	if (started)
		(void)g_mi_isp.fnStopChannel(0, 0);
	if (chn)
		(void)g_mi_isp.fnDestroyChannel(0, 0);
	if (dev)
		(void)g_mi_isp.fnDestroyDevice(0);
	return ret ? ret : -1;
}

static int configure_maruko_scl(const SensorSelectResult *sensor,
	uint32_t out_width, uint32_t out_height,
	const PipelinePrecropRect *precrop)
{
	MI_S32 ret = 0;
	int dev = 0, chn = 0, started = 0, port = 0;

	(void)sensor;

	if (!g_mi_scl_dev_created) {
		/* Match majestic: enable all 4 HW scaler ports (bits 0-3). */
		unsigned int scl_bind = 0xF;
		ret = g_mi_scl.fnCreateDevice(0, &scl_bind);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] MI_SCL_CreateDevice failed %d\n",
				ret);
			goto fail;
		}
		g_mi_scl_dev_created = 1;
	}
	dev = 1;

	if (!g_mi_scl_chn_created) {
		unsigned int scl_reserved = 0;
		ret = g_mi_scl.fnCreateChannel(0, 0, &scl_reserved);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] MI_SCL_CreateChannel failed %d\n",
				ret);
			goto fail;
		}
		g_mi_scl_chn_created = 1;
	}
	chn = 1;

	int rotation = 0;
	ret = g_mi_scl.fnAdjustChannelRotation(0, 0, &rotation);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SCL_SetChnParam failed %d\n", ret);
		goto fail;
	}

	ret = g_mi_scl.fnStartChannel(0, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SCL_StartChannel failed %d\n",
			ret);
		goto fail;
	}
	started = 1;

	/* SCL port crop: when keep_aspect=true and source AR != encode AR,
	 * pipeline_common_compute_precrop() returns a centered rect that
	 * matches the encode aspect ratio (zero offsets + full source dims
	 * otherwise).  Writing it into scl_port.crop avoids non-uniform
	 * scaling in the SCL stage.  Output = target dimensions; IFC
	 * compress required for HW_RING binding to VENC. */
	i6c_scl_port scl_port;
	memset(&scl_port, 0, sizeof(scl_port));
	if (precrop) {
		scl_port.crop.x = precrop->x;
		scl_port.crop.y = precrop->y;
		scl_port.crop.width = precrop->w;
		scl_port.crop.height = precrop->h;
	}
	scl_port.output.width = (unsigned short)out_width;
	scl_port.output.height = (unsigned short)out_height;
	scl_port.pixFmt = I6_PIXFMT_YUV420SP;
	scl_port.compress = (i6_common_compr)6; /* IFC */
	printf("> [maruko] SCL port: crop(%u,%u %ux%u) out(%ux%u) "
		"fmt=%d compress=%d\n",
		scl_port.crop.x, scl_port.crop.y,
		scl_port.crop.width, scl_port.crop.height,
		scl_port.output.width, scl_port.output.height,
		scl_port.pixFmt, scl_port.compress);
	ret = g_mi_scl.fnSetPortConfig(0, 0, 0, &scl_port);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SCL_SetOutputPortParam failed %d\n",
			ret);
		goto fail;
	}

	ret = g_mi_scl.fnEnablePort(0, 0, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SCL_EnableOutputPort failed %d\n",
			ret);
		goto fail;
	}
	port = 1;

	if (precrop)
		venc_api_set_active_precrop(precrop->x, precrop->y,
			precrop->w, precrop->h);
	return 0;

fail:
	if (port)
		(void)g_mi_scl.fnDisablePort(0, 0, 0);
	if (started)
		(void)g_mi_scl.fnStopChannel(0, 0);
	if (chn)
		(void)g_mi_scl.fnDestroyChannel(0, 0);
	if (dev)
		(void)g_mi_scl.fnDestroyDevice(0);
	return ret ? ret : -1;
}

static int maruko_start_vpe(const SensorSelectResult *sensor,
	uint32_t out_width, uint32_t out_height, int vpe_level_3dnr,
	const PipelinePrecropRect *precrop)
{
	int isp_started = 0;

	if (configure_maruko_isp(sensor, vpe_level_3dnr) != 0)
		return -1;
	isp_started = 1;

	if (configure_maruko_scl(sensor, out_width, out_height, precrop) != 0)
		goto fail_scl;

	return 0;

fail_scl:
	if (isp_started) {
		(void)g_mi_isp.fnDisablePort(0, 0, 0);
		(void)g_mi_isp.fnStopChannel(0, 0);
		(void)g_mi_isp.fnDestroyChannel(0, 0);
		(void)g_mi_isp.fnDestroyDevice(0);
	}
	return -1;
}

/* Stop VPE channels only — keep devices and dlopen handles alive.
 * Used during reinit to avoid kernel mutex destruction. */
/* Stop VPE channels only — skip ISP DestroyChannel which crashes
 * with "Mutex not initialized" when CUS3A state persists in kernel. */
static void maruko_stop_vpe_channels(void)
{
	if (g_mi_scl_chn_created) {
		(void)g_mi_scl.fnDisablePort(0, 0, 0);
		(void)g_mi_scl.fnStopChannel(0, 0);
		(void)g_mi_scl.fnDestroyChannel(0, 0);
		g_mi_scl_chn_created = 0;
	}
	if (g_mi_isp_chn_created) {
		(void)g_mi_isp.fnDisablePort(0, 0, 0);
		(void)g_mi_isp.fnStopChannel(0, 0);
		/* Skip DestroyChannel — kernel ISP retains CUS3A mutex
		 * state that crashes on destroy+recreate cycle. */
	}
}

/* Full VPE stop — destroy devices and unload libs.
 * Used during final shutdown only. */
static void maruko_stop_vpe(void)
{
	maruko_stop_vpe_channels();
	g_mi_isp_chn_created = 0;
	g_mi_scl_chn_created = 0;
	if (g_mi_scl_dev_created) {
		(void)g_mi_scl.fnDestroyDevice(0);
		g_mi_scl_dev_created = 0;
	}
	if (g_mi_isp_dev_created) {
		(void)g_mi_isp.fnDestroyDevice(0);
		g_mi_isp_dev_created = 0;
	}
}

static void maruko_fill_h26x_attr(i6c_venc_attr_h26x *attr,
	uint32_t width, uint32_t height)
{
	attr->maxWidth = width;
	attr->maxHeight = height;
	attr->bufSize = width * height * 3 / 2;
	attr->profile = 0;
	attr->byFrame = 1;
	attr->width = width;
	attr->height = height;
	attr->bFrameNum = 0;
	attr->refNum = 1;
}

static void fill_maruko_rc_attr(i6c_venc_chn *attr,
	const MarukoBackendConfig *cfg, uint32_t gop, MI_U32 bit_rate_bits,
	uint32_t framerate)
{
	/* rc_mode values come from codec_config_resolve_codec_rc():
	 *   cbr=3, h265 vbr=4 avbr=5 qvbr=6, h264 vbr=2 avbr=0 qvbr=1. */
	if (cfg->rc_codec == PT_H265) {
		switch (cfg->rc_mode) {
		case 4: /* VBR */
			attr->rate.mode = MARUKO_VENC_RC_H265_VBR;
			attr->rate.h265Vbr = (i6c_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 5: /* AVBR */
			attr->rate.mode = MARUKO_VENC_RC_H265_AVBR;
			attr->rate.h265Avbr = (i6c_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 6: /* QVBR: VBR with tighter QP range */
			attr->rate.mode = MARUKO_VENC_RC_H265_VBR;
			attr->rate.h265Vbr = (i6c_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 40, .minQual = 28,
			};
			break;
		case 3: /* CBR */
		default:
			attr->rate.mode = MARUKO_VENC_RC_H265_CBR;
			attr->rate.h265Cbr = (i6c_venc_rate_h26xcbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.bitrate = bit_rate_bits, .avgLvl = 1,
			};
			break;
		}
	} else {
		switch (cfg->rc_mode) {
		case 2: /* VBR */
			attr->rate.mode = MARUKO_VENC_RC_H264_VBR;
			attr->rate.h264Vbr = (i6c_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 0: /* AVBR */
			attr->rate.mode = MARUKO_VENC_RC_H264_AVBR;
			attr->rate.h264Avbr = (i6c_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 45, .minQual = 20,
			};
			break;
		case 1: /* QVBR */
			attr->rate.mode = MARUKO_VENC_RC_H264_VBR;
			attr->rate.h264Vbr = (i6c_venc_rate_h26xvbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.maxBitrate = bit_rate_bits,
				.maxQual = 40, .minQual = 28,
			};
			break;
		case 3: /* CBR */
		default:
			attr->rate.mode = MARUKO_VENC_RC_H264_CBR;
			attr->rate.h264Cbr = (i6c_venc_rate_h26xcbr){
				.gop = gop, .statTime = 1,
				.fpsNum = framerate, .fpsDen = 1,
				.bitrate = bit_rate_bits, .avgLvl = 1,
			};
			break;
		}
	}
}

/* ── Digital zoom (Approach C: SCL crop, no upscale) ──────────────── */
/*
 * Same model as Star6E: zoom_pct shrinks BOTH the SCL output dim AND the
 * crop window — SCL reads the rect at 1:1 and emits it verbatim, no
 * upscale, no SCL bandwidth pressure.  The encoded resolution drops with
 * zoom (receiver sees the smaller dim in SPS/PPS).  Maruko's SCL has a
 * single SetPortConfig API that programs crop + output dim atomically;
 * live pan re-issues SetPortConfig with new crop offsets while keeping
 * the same output dim (ISP / VENC channels never resize).
 *
 * MaruWindowRect_t mirrors MI_SYS_WindowRect_t.  Compute helpers live
 * here so they're reusable by both initial setup and live pan. */
typedef struct {
	uint16_t u16X, u16Y, u16Width, u16Height;
} MaruWindowRect_t;

/* Effective output dim for a given zoom_pct.  16-px alignment matches
 * SCL output and VENC create requirements; floor at 256 keeps the
 * encoded dim above VENC_CreateChn's minimum. */
static void maruko_compute_zoom_dim(uint32_t image_w, uint32_t image_h,
	double pct, uint32_t *out_w, uint32_t *out_h)
{
	const uint32_t ALIGN = 16;
	const uint32_t MIN_DIM = 256;
	double dw, dh;
	uint32_t w, h;

	if (!isfinite(pct) || pct <= 0.0 || pct >= 1.0) {
		if (out_w) *out_w = image_w;
		if (out_h) *out_h = image_h;
		return;
	}

	/* Promote pct so neither dim drops under MIN_DIM — keeps the zoom
	 * AR-preserving instead of squishing the shorter axis when the
	 * floor kicks in. */
	if ((double)image_w * pct < (double)MIN_DIM)
		pct = (double)MIN_DIM / (double)image_w;
	if ((double)image_h * pct < (double)MIN_DIM)
		pct = (double)MIN_DIM / (double)image_h;
	if (pct > 1.0) pct = 1.0;

	dw = (double)image_w * pct;
	dh = (double)image_h * pct;
	w = (uint32_t)(dw + 0.5);
	h = (uint32_t)(dh + 0.5);
	w &= ~(ALIGN - 1);
	h &= ~(ALIGN - 1);
	if (w < MIN_DIM) w = MIN_DIM;
	if (h < MIN_DIM) h = MIN_DIM;
	if (w > image_w) w = image_w & ~(ALIGN - 1);
	if (h > image_h) h = image_h & ~(ALIGN - 1);
	if (out_w) *out_w = w;
	if (out_h) *out_h = h;
}

/* Place a (rect_w × rect_h) 1:1 window inside (in_w × in_h) at fractional
 * (x, y) — typically the SCL input dim (post-precrop).  2-px aligned per
 * Maruko ISP/SCL spec.  Caller guarantees rect_w/h are 16-px-aligned via
 * compute_zoom_dim. */
static MaruWindowRect_t maruko_compute_zoom_rect(
	uint32_t in_w, uint32_t in_h, uint32_t rect_w, uint32_t rect_h,
	double x, double y)
{
	const uint32_t XY_ALIGN = 2;
	MaruWindowRect_t r;
	double cx, cy;
	uint32_t rx, ry;

	if (!isfinite(x)) x = 0.5;
	if (!isfinite(y)) y = 0.5;
	if (x < 0.0)
		x = 0.0;
	if (x > 1.0)
		x = 1.0;
	if (y < 0.0)
		y = 0.0;
	if (y > 1.0)
		y = 1.0;
	if (rect_w > in_w) rect_w = in_w & ~(XY_ALIGN - 1);
	if (rect_h > in_h) rect_h = in_h & ~(XY_ALIGN - 1);

	cx = (double)in_w * x - (double)rect_w * 0.5;
	cy = (double)in_h * y - (double)rect_h * 0.5;
	if (cx < 0.0) cx = 0.0;
	if (cy < 0.0) cy = 0.0;
	if (cx + (double)rect_w > (double)in_w) cx = (double)(in_w - rect_w);
	if (cy + (double)rect_h > (double)in_h) cy = (double)(in_h - rect_h);

	rx = (uint32_t)(cx + 0.5) & ~(XY_ALIGN - 1);
	ry = (uint32_t)(cy + 0.5) & ~(XY_ALIGN - 1);
	if (rx + rect_w > in_w) rx = (in_w - rect_w) & ~(XY_ALIGN - 1);
	if (ry + rect_h > in_h) ry = (in_h - rect_h) & ~(XY_ALIGN - 1);

	r.u16X = (uint16_t)rx;
	r.u16Y = (uint16_t)ry;
	r.u16Width  = (uint16_t)rect_w;
	r.u16Height = (uint16_t)rect_h;
	return r;
}

static MarukoZoomStatus g_zoom_status;
static pthread_mutex_t g_zoom_status_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t maruko_zoom_level_x100(double pct)
{
	if (!isfinite(pct) || pct <= 0.0)
		return 0;
	return (uint32_t)(100.0 / pct + 0.5);
}

static void maruko_pipeline_set_zoom_status(double pct,
	uint32_t output_w, uint32_t output_h,
	uint32_t crop_x, uint32_t crop_y, uint32_t crop_w, uint32_t crop_h)
{
	MarukoZoomStatus snap;

	memset(&snap, 0, sizeof(snap));
	snap.active = 1;
	snap.level_x100 = maruko_zoom_level_x100(pct);
	snap.output_w = output_w;
	snap.output_h = output_h;
	snap.crop_x = crop_x;
	snap.crop_y = crop_y;
	snap.crop_w = crop_w;
	snap.crop_h = crop_h;

	pthread_mutex_lock(&g_zoom_status_mutex);
	g_zoom_status = snap;
	pthread_mutex_unlock(&g_zoom_status_mutex);
}

static void maruko_pipeline_clear_zoom_status(void)
{
	pthread_mutex_lock(&g_zoom_status_mutex);
	memset(&g_zoom_status, 0, sizeof(g_zoom_status));
	pthread_mutex_unlock(&g_zoom_status_mutex);
}

void maruko_pipeline_zoom_status(MarukoZoomStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_zoom_status_mutex);
	*out = g_zoom_status;
	pthread_mutex_unlock(&g_zoom_status_mutex);
}

/* Live pan: zoom_pct is MUT_RESTART (encoder dim change), so the live path
 * only updates x/y.  Re-issues SCL SetPortConfig with the same output dim
 * but new crop offsets.  pct is accepted to short-circuit when zoom is
 * off (no rect to pan). */
int maruko_pipeline_apply_zoom(MarukoBackendContext *ctx,
	double pct, double x, double y)
{
	MaruWindowRect_t rect;
	i6c_scl_port scl_port;
	uint32_t scl_crop_x, scl_crop_y, scl_crop_w, scl_crop_h;
	uint32_t crop_x, crop_y;
	MI_S32 ret;

	if (!ctx) return -1;
	if (!isfinite(pct) || !isfinite(x) || !isfinite(y))
		return -1;
	if (pct <= 0.0 || pct >= 1.0) {
		maruko_pipeline_clear_zoom_status();
		return 0;  /* zoom off — nothing to pan */
	}

	/* SCL channel must be created; otherwise SetPortConfig has nothing
	 * to update. */
	if (!g_mi_scl_chn_created)
		return -1;

	scl_crop_x = ctx->scl_crop_x;
	scl_crop_y = ctx->scl_crop_y;
	scl_crop_w = ctx->scl_crop_w;
	scl_crop_h = ctx->scl_crop_h;
	if (scl_crop_w == 0 || scl_crop_h == 0)
		return -1;

	rect = maruko_compute_zoom_rect(scl_crop_w, scl_crop_h,
		ctx->cfg.image_width, ctx->cfg.image_height, x, y);
	crop_x = scl_crop_x + rect.u16X;
	crop_y = scl_crop_y + rect.u16Y;

	memset(&scl_port, 0, sizeof(scl_port));
	scl_port.crop.x = (unsigned short)crop_x;
	scl_port.crop.y = (unsigned short)crop_y;
	scl_port.crop.width = rect.u16Width;
	scl_port.crop.height = rect.u16Height;
	scl_port.output.width = (unsigned short)ctx->cfg.image_width;
	scl_port.output.height = (unsigned short)ctx->cfg.image_height;
	scl_port.pixFmt = I6_PIXFMT_YUV420SP;
	scl_port.compress = (i6_common_compr)6;  /* IFC */

	ret = g_mi_scl.fnSetPortConfig(0, 0, 0, &scl_port);
	if (ret != 0) {
		fprintf(stderr,
			"[maruko] WARNING: SCL pan SetPortConfig failed %d "
			"(rect %ux%u+%u+%u out %ux%u)\n",
			(int)ret, rect.u16Width, rect.u16Height,
			crop_x, crop_y,
			ctx->cfg.image_width, ctx->cfg.image_height);
		return -1;
	}
	maruko_pipeline_set_zoom_status(pct, ctx->cfg.image_width,
		ctx->cfg.image_height, crop_x, crop_y,
		rect.u16Width, rect.u16Height);
	return 0;
}

/* IntraRefresh status snapshot — populated by
 * maruko_pipeline_apply_intra_refresh() at every pipeline_start, cleared by
 * maruko_stop_venc().  Read by venc_api's /api/v1/intra/status handler. */
static MarukoIntraRefreshStatus g_intra_status;
static pthread_mutex_t g_intra_status_mutex = PTHREAD_MUTEX_INITIALIZER;

void maruko_pipeline_intra_refresh_status(MarukoIntraRefreshStatus *out)
{
	if (!out)
		return;
	pthread_mutex_lock(&g_intra_status_mutex);
	*out = g_intra_status;
	pthread_mutex_unlock(&g_intra_status_mutex);
}

/* Compute IntraRefresh derived params from the maruko config snapshot. */
static IntraRefreshMode maruko_intra_refresh_derive(
	const MarukoBackendConfig *cfg, uint32_t height, uint32_t fps,
	PAYLOAD_TYPE_E codec, IntraRefreshDerived *out_ir)
{
	IntraRefreshMode mode = INTRA_MODE_OFF;

	memset(out_ir, 0, sizeof(*out_ir));
	if (cfg) {
		mode = intra_refresh_parse_mode(cfg->intra_refresh_mode);
		intra_refresh_compute(mode, height, fps, codec == PT_H265,
			cfg->intra_refresh_lines, cfg->intra_refresh_qp,
			cfg->gop_size_sec, out_ir);
	}
	return mode;
}

static int maruko_apply_intra_refresh(MI_VENC_DEV dev, MI_VENC_CHN chn,
	const MarukoBackendConfig *cfg, uint32_t height, uint32_t fps,
	PAYLOAD_TYPE_E codec)
{
	MI_VENC_IntraRefresh_t ir_sdk;
	MarukoIntraRefreshStatus snap;
	IntraRefreshDerived ir;
	IntraRefreshMode mode;
	const char *name;

	memset(&snap, 0, sizeof(snap));
	mode = maruko_intra_refresh_derive(cfg, height, fps, codec, &ir);
	name = intra_refresh_mode_name(mode);

	snprintf(snap.mode_name, sizeof(snap.mode_name), "%s", name);
	snap.mi_supported = g_mi_venc.fnSetIntraRefresh ? 1 : 0;
	if (cfg) {
		snap.requested_lines  = cfg->intra_refresh_lines;
		snap.requested_qp     = cfg->intra_refresh_qp;
		snap.explicit_gop_sec = cfg->gop_size_sec;
	}
	snap.target_ms             = ir.target_ms;
	snap.total_rows            = ir.total_rows;
	snap.effective_lines_per_p = ir.lines;
	snap.lines_clamped         = ir.lines_clamped;
	snap.effective_qp          = ir.req_iqp;
	snap.effective_gop_sec     = ir.gop_overridden ? snap.explicit_gop_sec : ir.gop_sec;
	snap.gop_auto              = ir.gop_overridden ? 0 : (ir.gop_sec > 0.0);

	if (mode == INTRA_MODE_OFF) {
		pthread_mutex_lock(&g_intra_status_mutex);
		g_intra_status = snap;
		pthread_mutex_unlock(&g_intra_status_mutex);
		return 0;
	}
	if (!g_mi_venc.fnSetIntraRefresh) {
		fprintf(stderr, "[venc] WARNING: intraRefreshMode=%s requested "
			"but libmi_venc.so does not export "
			"MI_VENC_SetIntraRefresh\n", name);
		pthread_mutex_lock(&g_intra_status_mutex);
		g_intra_status = snap;
		pthread_mutex_unlock(&g_intra_status_mutex);
		return -1;
	}
	if (ir.lines_clamped) {
		fprintf(stderr, "[venc] WARNING: intraRefreshLines exceeds picture "
			"LCU rows=%u, clamped\n", ir.total_rows);
	}
	if (ir.gop_overridden) {
		fprintf(stderr, "[venc] intra auto-GOP suppressed: explicit "
			"gopSize=%.2fs\n", snap.explicit_gop_sec);
	}

	memset(&ir_sdk, 0, sizeof(ir_sdk));
	ir_sdk.bEnable = 1;
	ir_sdk.u32RefreshLineNum = ir.lines;
	ir_sdk.u32ReqIQp = ir.req_iqp;

	if (maruko_mi_venc_set_intra_refresh(dev, chn, &ir_sdk) != 0) {
		fprintf(stderr, "[venc] ERROR: MI_VENC_SetIntraRefresh(dev=%d, "
			"chn=%d, lines=%u, qp=%u) failed\n", dev, chn,
			ir_sdk.u32RefreshLineNum, ir_sdk.u32ReqIQp);
		pthread_mutex_lock(&g_intra_status_mutex);
		g_intra_status = snap;
		pthread_mutex_unlock(&g_intra_status_mutex);
		return -1;
	}
	snap.apply_ok = 1;
	snap.active   = 1;
	pthread_mutex_lock(&g_intra_status_mutex);
	g_intra_status = snap;
	pthread_mutex_unlock(&g_intra_status_mutex);
	fprintf(stderr, "[venc] intraRefresh: mode=%s dev=%d chn=%d lines/P=%u "
		"qp=%u gop=%.2fs (%s)\n", name, dev, chn, ir_sdk.u32RefreshLineNum,
		ir_sdk.u32ReqIQp, snap.effective_gop_sec,
		snap.gop_auto ? "auto" : "explicit");
	return 0;
}

static int maruko_start_venc(const MarukoBackendConfig *cfg,
	uint32_t width, uint32_t height, uint32_t framerate,
	MI_VENC_DEV venc_dev, MI_VENC_CHN *chn, int *dev_created)
{
	if (dev_created)
		*dev_created = 0;

	i6c_venc_init init = {
		.maxWidth = 4096,
		.maxHeight = 2176,
	};
	MI_S32 ret = maruko_mi_venc_create_dev(venc_dev, &init);
	if (ret == 0) {
		if (dev_created)
			*dev_created = 1;
	} else {
		fprintf(stderr,
			"WARNING: [maruko] MI_VENC_CreateDev failed %d"
			" (continuing)\n", ret);
	}

	/* Ring pool MUST be configured before CreateChannel (matching
	 * majestic i6c_hal.c order: pool → CreateChn → SetSource → Start) */
	MI_U16 venc_ring = (MI_U16)height;
	if (venc_ring == 0)
		venc_ring = 1;
	MI_S32 pool_ret = maruko_config_dev_ring_pool(I6C_SYS_MOD_VENC,
		(MI_U32)venc_dev, (MI_U16)width, (MI_U16)height, venc_ring);
	printf("> [maruko] VENC ring pool: %ux%u ring=%u ret=%d\n",
		width, height, venc_ring, pool_ret);

	i6c_venc_chn attr = {0};
	if (cfg->rc_codec == PT_H265) {
		attr.attrib.codec = I6C_VENC_CODEC_H265;
		maruko_fill_h26x_attr(&attr.attrib.h265, width, height);
	} else {
		attr.attrib.codec = I6C_VENC_CODEC_H264;
		maruko_fill_h26x_attr(&attr.attrib.h264, width, height);
	}

	uint32_t gop = cfg->venc_gop_size;
	if (gop == 0)
		gop = 1;
	MI_U32 bit_rate_bits = cfg->venc_max_rate * 1024;

	fill_maruko_rc_attr(&attr, cfg, gop, bit_rate_bits, framerate);

	*chn = 0;
	ret = maruko_mi_venc_create_chn(venc_dev, *chn, &attr);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VENC_CreateChn failed %d\n", ret);
		if (dev_created && *dev_created) {
			(void)maruko_mi_venc_destroy_dev(venc_dev);
			*dev_created = 0;
		}
		return ret;
	}

	i6c_venc_src_conf input_mode = I6C_VENC_SRC_CONF_RING_DMA;
	ret = maruko_mi_venc_set_input_source(venc_dev, *chn, &input_mode);
	if (ret != 0) {
		fprintf(stderr,
			"WARNING: [maruko] MI_VENC_SetInputSourceConfig"
			" failed %d\n", ret);
	}

	ret = maruko_mi_venc_start_recv(venc_dev, *chn);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VENC_StartRecvPic failed %d\n",
			ret);
		(void)maruko_mi_venc_destroy_chn(venc_dev, *chn);
		if (dev_created && *dev_created) {
			(void)maruko_mi_venc_destroy_dev(venc_dev);
			*dev_created = 0;
		}
		return ret;
	}

	/* Frame-lost safety net — must be after StartRecvPic. */
	if (cfg->frame_lost) {
		MI_VENC_ParamFrameLost_t lost = {0};
		lost.bFrmLostOpen = 1;
		lost.eFrmLostMode = E_MI_VENC_FRMLOST_NORMAL;
		lost.u32FrmLostBpsThr =
			pipeline_common_frame_lost_threshold(cfg->venc_max_rate);
		lost.u32EncFrmGaps = 0;
		MI_S32 fl_ret = maruko_mi_venc_set_frame_lost(venc_dev, *chn,
			&lost);
		if (fl_ret != 0) {
			fprintf(stderr,
				"WARNING: [maruko] SetFrameLostStrategy"
				" thr=%u ret=%d (overshoot protection disabled)\n",
				lost.u32FrmLostBpsThr, fl_ret);
		} else {
			printf("> [maruko] SetFrameLostStrategy: thr=%u ret=0\n",
				lost.u32FrmLostBpsThr);
		}
	}

	/* IntraRefresh — opt-in via video0.intra_refresh.  Ch0 only; the
	 * dual ch1 path is intentionally skipped since TS containers need
	 * IDRs.  Failure is logged and snapshotted but never aborts the
	 * pipeline (matches Star6E behavior). */
	(void)maruko_apply_intra_refresh(venc_dev, *chn, cfg, height,
		framerate, cfg->rc_codec);

	/* Phase 7 dual-VENC SDK probe (debug-only, env-gated).
	 *
	 * Set MARUKO_DUAL_VENC_PROBE=1 to attempt CreateChn(dev, 1, ...)
	 * after channel 0 is fully started.  Reports a 3-stage signal:
	 *   (a) CreateChn ret  — does SDK accept a 2nd channel on dev 0?
	 *   (b) SetInputSourceConfig ret — RING_DMA path accepts ch1?
	 *   (c) StartRecvPic ret — channel actually starts?
	 *
	 * Channel 1 is torn down immediately; this does NOT bind VPE -> ch1.
	 * Use the result to decide whether to commit to a full Phase 7 port
	 * (Gemini-style mirror or per-channel resolution) on Maruko.
	 */
	if (getenv("MARUKO_DUAL_VENC_PROBE")) {
		MI_VENC_CHN probe_ch = 1;
		i6c_venc_chn probe_attr = attr;  /* mirror chn 0 attrs */
		MI_S32 probe_ret;

		printf("> [maruko][probe] dual-VENC SDK probe BEGIN "
			"(dev=%d, probe_chn=%d)\n",
			(int)venc_dev, (int)probe_ch);

		probe_ret = maruko_mi_venc_create_chn(venc_dev, probe_ch,
			&probe_attr);
		printf("> [maruko][probe] CreateChn(dev=%d, chn=%d) ret=%d\n",
			(int)venc_dev, (int)probe_ch, (int)probe_ret);

		if (probe_ret == 0) {
			i6c_venc_src_conf src = I6C_VENC_SRC_CONF_RING_DMA;
			MI_S32 src_ret = maruko_mi_venc_set_input_source(
				venc_dev, probe_ch, &src);
			printf("> [maruko][probe] SetInputSourceConfig(chn=%d,"
				" RING_DMA) ret=%d\n",
				(int)probe_ch, (int)src_ret);

			MI_S32 start_ret = maruko_mi_venc_start_recv(venc_dev,
				probe_ch);
			printf("> [maruko][probe] StartRecvPic(chn=%d) ret=%d\n",
				(int)probe_ch, (int)start_ret);
			if (start_ret == 0)
				(void)maruko_mi_venc_stop_recv(venc_dev, probe_ch);

			(void)maruko_mi_venc_destroy_chn(venc_dev, probe_ch);
			printf("> [maruko][probe] DestroyChn(chn=%d) — torn down\n",
				(int)probe_ch);
		}

		printf("> [maruko][probe] dual-VENC SDK probe END\n");
	}

	return 0;
}

static void maruko_stop_venc(MI_VENC_DEV venc_dev, MI_VENC_CHN chn,
	int destroy_dev)
{
	(void)maruko_mi_venc_stop_recv(venc_dev, chn);
	(void)maruko_mi_venc_destroy_chn(venc_dev, chn);
	if (destroy_dev)
		(void)maruko_mi_venc_destroy_dev(venc_dev);

	/* Clear IntraRefresh status snapshot — the channel is gone, so
	 * /api/v1/intra/status should not keep reporting enabled=true
	 * until the next pipeline_start runs. */
	pthread_mutex_lock(&g_intra_status_mutex);
	memset(&g_intra_status, 0, sizeof(g_intra_status));
	pthread_mutex_unlock(&g_intra_status_mutex);
	maruko_pipeline_clear_zoom_status();
}

static void maruko_sysfs_write(const char *path, const char *value)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "%s\n", value);
	fclose(f);
}

static void maruko_set_hw_clocks(int oc_level, int verbose)
{
	/* ISP clock: 384 MHz */
	maruko_sysfs_write("/sys/devices/virtual/mstar/isp0/isp_clk",
		"384000000");

	/* SCL clock: index 1 = 533 MHz (max available).
	 * Must be set before SCL device creation (locks clock). */
	maruko_sysfs_write("/sys/devices/virtual/mstar/mscl/clk", "1");

	printf("> [maruko] ISP clock -> 384 MHz, SCL clock -> 533 MHz\n");

	if (oc_level >= 1) {
		/* VENC secondary + AXI clocks: try to boost from 320→288
		 * (writes may be ignored while streaming). */
		maruko_sysfs_write(
			"/sys/devices/virtual/mstar/venc/ven_clock_2nd",
			"288000000");
		maruko_sysfs_write(
			"/sys/devices/virtual/mstar/venc/ven_clock_axi",
			"288000000");
		printf("> [maruko] VENC 2nd/AXI -> 288 MHz (oc-level %d)\n",
			oc_level);
	}

	if (oc_level >= 2) {
		maruko_sysfs_write(
			"/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
			"performance");
		maruko_sysfs_write(
			"/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq",
			"1200000");
		if (verbose)
			printf("> [maruko] CPU -> performance @ 1200 MHz "
				"(oc-level %d)\n", oc_level);
	}
}

int maruko_pipeline_init(MarukoBackendContext *ctx)
{
	if (maruko_mi_init() != 0) {
		fprintf(stderr, "ERROR: [maruko] MI library load failed\n");
		return -1;
	}

	MI_S32 ret = MI_SYS_Init();
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SYS_Init failed %d\n", ret);
		maruko_mi_deinit();
		return ret;
	}
	ctx->system_initialized = 1;

	/* Set HW clocks AFTER MI_SYS_Init (kernel modules now loaded)
	 * but BEFORE ISP/SCL device creation (which locks clocks). */
	maruko_set_hw_clocks(ctx->cfg.oc_level, 1);

	printf("> [maruko] stage init: MI_SYS_Init ok\n");
	return 0;
}

static int setup_maruko_graph_dimensions(MarukoBackendContext *ctx)
{
	SensorSelectConfig sel_cfg = pipeline_common_build_sensor_select_config(
		(int)ctx->cfg.forced_sensor_pad, ctx->cfg.forced_sensor_mode,
		ctx->cfg.sensor_width, ctx->cfg.sensor_height,
		ctx->cfg.sensor_fps);
	SensorStrategy strategy;
	if (ctx->cfg.sensor_unlock.enabled) {
		strategy = sensor_unlock_strategy(&ctx->cfg.sensor_unlock);
		printf("> [maruko] sensor unlock enabled (reg=0x%04x val=0x%04x)\n",
			ctx->cfg.sensor_unlock.reg, ctx->cfg.sensor_unlock.value);
	} else {
		strategy = sensor_default_strategy();
	}
	if (sensor_select(&sel_cfg, &strategy, &ctx->sensor) != 0)
		return -1;
	ctx->sensor_enabled = 1;

	pipeline_common_report_selected_fps("[maruko] ", ctx->cfg.sensor_fps,
		&ctx->sensor);

	/* Overscan detection: capture rect > crop rect can cause pipeline
	 * hangs (e.g. binning modes report pre-binning capture).
	 * Clamp to crop and zero offsets — the VIF inputRect must
	 * match the actual MIPI output, not the pre-binning window. */
	if (ctx->sensor.mode.crop.width > 0 &&
	    ctx->sensor.plane.capt.width > ctx->sensor.mode.crop.width) {
		fprintf(stderr, "WARNING: [maruko] sensor overscan detected: "
			"capture (%u,%u %ux%u) > crop %ux%u — clamping\n",
			ctx->sensor.plane.capt.x, ctx->sensor.plane.capt.y,
			ctx->sensor.plane.capt.width, ctx->sensor.plane.capt.height,
			ctx->sensor.mode.crop.width, ctx->sensor.mode.crop.height);
		ctx->sensor.plane.capt.x = 0;
		ctx->sensor.plane.capt.y = 0;
		ctx->sensor.plane.capt.width = ctx->sensor.mode.crop.width;
		ctx->sensor.plane.capt.height = ctx->sensor.mode.crop.height;
	}

	/* Effective output dimensions: when mode.output < crop, the sensor
	 * does internal binning and the actual frame is smaller than crop.
	 * Use output dimensions for image clamping and ring pool sizing. */
	uint32_t eff_w = ctx->sensor.plane.capt.width;
	uint32_t eff_h = ctx->sensor.plane.capt.height;
	if (ctx->sensor.mode.output.width > 0 &&
	    ctx->sensor.mode.output.width < eff_w) {
		printf("> [maruko] sensor output binning: %ux%u -> %ux%u\n",
			eff_w, eff_h,
			ctx->sensor.mode.output.width,
			ctx->sensor.mode.output.height);
		eff_w = ctx->sensor.mode.output.width;
		eff_h = ctx->sensor.mode.output.height;
	}
	uint32_t out_w = ctx->cfg.image_width;
	uint32_t out_h = ctx->cfg.image_height;
	if (out_w == 0 || out_h == 0) {
		out_w = eff_w;
		out_h = eff_h;
	}
	pipeline_common_clamp_image_size("[maruko] ", eff_w, eff_h,
		&out_w, &out_h);
	ctx->cfg.image_width = out_w;
	ctx->cfg.image_height = out_h;
	ctx->cfg.venc_gop_size = pipeline_common_gop_frames(
		ctx->cfg.venc_gop_seconds, ctx->sensor.fps);

	/* IntraRefresh auto-GOP override: when intraRefreshMode != off and the
	 * user did not pin gopSize, align IDR period with one full GDR pass. */
	{
		IntraRefreshDerived ir;
		IntraRefreshMode mode = maruko_intra_refresh_derive(
			&ctx->cfg, ctx->cfg.image_height, ctx->sensor.fps,
			ctx->cfg.rc_codec, &ir);
		if (mode != INTRA_MODE_OFF && !ir.gop_overridden && ir.gop_frames > 0)
			ctx->cfg.venc_gop_size = ir.gop_frames;
	}

	/* ISP throughput note: the Maruko ISP stalls when the sensor
	 * pixel throughput exceeds ~144M pix/s.  Mode 3 (1472x816@120fps
	 * = 144M) works; modes 0-2 (>=1920x1080) stall regardless of
	 * FPS because the MIPI data rate is set by the sensor mode, not
	 * the target FPS.  VIF sub-window crop is NOT supported on I6C.
	 * To enable lower-FPS modes at 1080p, the sensor driver needs
	 * custom mode entries with appropriate binning/MIPI settings. */

	/* Configure SCL ring pool using sensor capture dimensions.
	 * Note: majestic skips this, but the SDK sample_venc.c uses it.
	 * Use capture (ISP output) size, not effective/binned size. */
	uint32_t capt_w = ctx->sensor.plane.capt.width;
	uint32_t capt_h = ctx->sensor.plane.capt.height;
	MI_U16 scl_ring = (MI_U16)(capt_h / 4);
	if (scl_ring == 0)
		scl_ring = 1;
	MI_S32 pool_ret = maruko_config_dev_ring_pool(I6C_SYS_MOD_SCL, 0,
		(MI_U16)capt_w, (MI_U16)capt_h, scl_ring);
	printf("> [maruko] SCL ring pool: %ux%u ring=%u ret=%d\n",
		capt_w, capt_h, scl_ring, pool_ret);
	printf("> [maruko] sensor capt: %ux%u  eff: %ux%u  out: %ux%u\n",
		ctx->sensor.plane.capt.width, ctx->sensor.plane.capt.height,
		eff_w, eff_h, out_w, out_h);

	return 0;
}

static void assign_maruko_ports(MarukoBackendContext *ctx,
	MI_U32 venc_device)
{
	ctx->vif_port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VIF, .device = 0,
		.channel = 0, .port = 0,
	};
	ctx->isp_port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_ISP, .device = 0,
		.channel = 0, .port = 0,
	};
	ctx->vpe_port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_SCL, .device = 0,
		.channel = 0, .port = 0,
	};
	ctx->venc_port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VENC, .device = venc_device,
		.channel = ctx->venc_channel, .port = 0,
	};
}

static int bind_maruko_pipeline(MarukoBackendContext *ctx)
{
	uint32_t out_w = ctx->cfg.image_width;
	uint32_t out_h = ctx->cfg.image_height;

	ctx->venc_device = 0;
	if (maruko_start_venc(&ctx->cfg, out_w, out_h, ctx->sensor.fps,
	    ctx->venc_device, &ctx->venc_channel,
	    &ctx->venc_dev_created) != 0)
		return -1;
	ctx->venc_started = 1;

	MI_U32 venc_device = (MI_U32)ctx->venc_device;
	assign_maruko_ports(ctx, venc_device);

	MI_S32 ret = MI_SYS_BindChnPort2(&ctx->vif_port, &ctx->isp_port,
		ctx->sensor.fps, ctx->sensor.fps, I6_SYS_LINK_REALTIME, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] bind VIF->ISP failed %d\n", ret);
		return -1;
	}
	ctx->bound_vif_vpe = 1;

	ret = MI_SYS_BindChnPort2(&ctx->isp_port, &ctx->vpe_port,
		ctx->sensor.fps, ctx->sensor.fps, I6_SYS_LINK_REALTIME, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] bind ISP->SCL failed %d\n", ret);
		return -1;
	}
	ctx->bound_isp_vpe = 1;

	ret = MI_SYS_BindChnPort2(&ctx->vpe_port, &ctx->venc_port,
		ctx->sensor.fps, ctx->sensor.fps, I6_SYS_LINK_RING, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] bind SCL->VENC failed %d\n", ret);
		return -1;
	}
	ctx->bound_vpe_venc = 1;

	/* Set output port buffer depths to allow pipelining between
	 * stages.  Without this, the pipeline has zero frame buffering
	 * and any processing jitter causes frame drops, capping FPS
	 * well below the sensor's output rate.
	 * Star6E uses (1, 3) on the VENC port; SDK samples use (2, 4). */
	(void)MI_SYS_SetChnOutputPortDepth(&ctx->isp_port, 1, 3);
	(void)MI_SYS_SetChnOutputPortDepth(&ctx->vpe_port, 1, 3);
	(void)MI_SYS_SetChnOutputPortDepth(&ctx->venc_port, 1, 3);

	/* ISP bin: resolve every configure (so SIGHUP / `/api/v1/restart`
	 * changes to `isp.sensorBin` and the auto-detect fallback are picked
	 * up on reinit), but skip the actual reload when the resolved path
	 * matches the last-loaded path.  Avoids redundant vendor reloads
	 * which can disturb running AE.  Star6E parity. */
	{
		char isp_bin_resolved[256];
		const char *configured = ctx->cfg.isp_bin_path[0] ?
			ctx->cfg.isp_bin_path : NULL;

		if (pipeline_common_resolve_isp_bin(configured,
			ctx->sensor.plane.sensName,
			isp_bin_resolved, sizeof(isp_bin_resolved)) &&
		    strcmp(isp_bin_resolved, g_last_isp_bin_path) != 0) {
			ret = maruko_load_isp_bin(isp_bin_resolved);
			if (ret != 0)
				return -1;
			snprintf(g_last_isp_bin_path,
				sizeof(g_last_isp_bin_path), "%s",
				isp_bin_resolved);
		}
	}

	/* CUS3A enable + cold-boot exposure cap: must only run once per
	 * process lifetime.  Re-running CUS3A enable causes a vendor mutex
	 * deadlock; the exposure cap and `MI_SNR_SetFps` kick are documented
	 * cold-boot fixups, not reinit operations. */
	if (!g_mi_isp_initialized) {
		/* Initialize CUS3A framework + enable userspace 3A (keeps
		 * IQ→HW pump alive).  The optional no-op AE adaptor for
		 * throttle mode is installed below, after the cold-boot
		 * exposure cap and SetFps kick. */
		maruko_enable_cus3a();

		typedef struct {
			unsigned int minShutterUs, maxShutterUs;
			unsigned int minApertX10, maxApertX10;
			unsigned int minSensorGain, minIspGain;
			unsigned int maxSensorGain, maxIspGain;
		} MarukoIspExposureLimit;
		/* Auto-cap exposure to frame period for max FPS.
		 * 120fps sensor → 8333us cap → 118fps. */
		if (ctx->sensor.fps > 0) {
			uint32_t frame_period_us = 1000000 / ctx->sensor.fps;
			typedef int (*ae_get_fn)(uint32_t, uint32_t,
				MarukoIspExposureLimit *);
			typedef int (*ae_set_fn)(uint32_t, uint32_t,
				MarukoIspExposureLimit *);
			ae_get_fn fn_get = (ae_get_fn)dlsym(RTLD_DEFAULT,
				"MI_ISP_AE_GetExposureLimit");
			ae_set_fn fn_set = (ae_set_fn)dlsym(RTLD_DEFAULT,
				"MI_ISP_AE_SetExposureLimit");
			if (fn_get && fn_set) {
				MarukoIspExposureLimit lim = {0};
				if (fn_get(0, 0, &lim) == 0) {
					printf("> [maruko] Exposure cap: "
						"%uus -> %uus (for %u fps, "
						"frame period %uus)\n",
						lim.maxShutterUs,
						frame_period_us,
						ctx->sensor.fps,
						frame_period_us);
					lim.maxShutterUs = frame_period_us;
					fn_set(0, 0, &lim);
				}
			}

			/* Force sensor timing reconfiguration after AE
			 * init.  The vendor AE may have extended VTS
			 * based on ISP bin defaults; MI_SNR_SetFps
			 * forces the sensor driver to reset VTS to the
			 * mode's native value. */
			MI_SNR_SetFps(ctx->sensor.pad_id, ctx->sensor.fps);
			printf("> [maruko] MI_SNR_SetFps kick: "
				"pad %d fps %u\n",
				(int)ctx->sensor.pad_id,
				ctx->sensor.fps);
		}

		/* AE-mode dispatch.
		 *   native    — SDK's NATIVE AE+AWB run inside 3A_Proc_0 at
		 *               sensor rate (default; matches Star6E).
		 *   throttle  — no-op AE adaptor replaces NATIVE AE algo;
		 *               supervisory thread drives AE via SetAeParam
		 *               at ae_fps Hz.  Saves ~24% of one core. */
		int throttle = ctx->cfg.ae_mode[0] &&
			strcmp(ctx->cfg.ae_mode, "throttle") == 0;

		/* Start supervisory thread FIRST so it captures the bin's
		 * calibrated AE limits while the SDK's NATIVE algo is still
		 * live.  Installing the no-op adaptor clears AE init state
		 * (state goes to -1, GetExposureLimit returns zeros), so any
		 * baseline we want must be read before the swap. */
		if (ctx->cfg.ae_fps > 0) {
			MarukoCus3aConfig ae_cfg;
			maruko_cus3a_config_defaults(&ae_cfg);
			ae_cfg.sensor_fps    = ctx->sensor.fps;
			ae_cfg.ae_fps        = ctx->cfg.ae_fps;
			ae_cfg.gain_max      = ctx->cfg.isp_gain_max;
			ae_cfg.verbose       = ctx->cfg.verbose;
			ae_cfg.throttle_mode = throttle;
			(void)maruko_cus3a_start(&ae_cfg);
		} else if (ctx->cfg.verbose) {
			printf("> [maruko] supervisory 3A disabled "
				"(isp.aeFps=0)\n");
		}

		if (throttle) {
			maruko_cus3a_install_noop_adaptor();
			printf("> [maruko] AE mode: throttle "
				"(no-op AE adaptor + manual SetAeParam)\n");
		} else {
			printf("> [maruko] AE mode: native "
				"(SDK AE/AWB at sensor rate)\n");
		}

		g_mi_isp_initialized = 1;
	}

	if (ctx->cfg.output_uri.type == VENC_OUTPUT_URI_SHM) {
		if (ctx->cfg.stream_mode != MARUKO_STREAM_RTP) {
			fprintf(stderr, "ERROR: [maruko] shm:// requires RTP mode\n");
			return -1;
		}
		if (maruko_output_init_shm(&ctx->output,
		    ctx->cfg.output_uri.endpoint) != 0)
			return -1;
	} else {
		if (maruko_output_init(&ctx->output, &ctx->cfg.output_uri,
		    ctx->cfg.connected_udp) != 0)
			return -1;
	}

	return 0;
}

/* IMU push callback: stub.  Drained samples are discarded for now —
 * the callback exists so a future telemetry / sidecar consumer can
 * slot in without rewiring imu_init.  Mirrors star6e_pipeline.c's
 * star6e_pipeline_imu_push. */
static void maruko_pipeline_imu_push(void *ctx, const ImuSample *sample)
{
	(void)ctx;
	(void)sample;
}

/* ── Dual VENC (Phase 7) ─────────────────────────────────────────────────
 *
 * Mirrors src/star6e_pipeline.c's Star6eDualVenc lifecycle and
 * src/star6e_runtime.c's dual_rec_thread_fn frame-drain.  Currently
 * only the "dual-stream" mode is wired (chn 1 → UDP); the "dual" /
 * TS-record variant lives behind Phase 6 (no MI_AI / TS mux on
 * Maruko yet).
 *
 * The chn 1 stream is sourced from the same VPE port as chn 0 — both
 * channels see identical pre-encoder frames.  Bitrate / FPS / GOP can
 * differ per channel via the cfg.record.* knobs.
 */

struct MarukoDualVenc {
	MarukoBackendContext *ctx;          /* back-pointer for cfg lookup in thread */
	MI_VENC_CHN channel;
	MI_SYS_ChnPort_t port;              /* dst port for chn 1 (VENC/dev/1/0) */
	MI_SYS_ChnPort_t bind_src;          /* src port used at bind time
	                                     * (VENC chn 0 output) — captured so
	                                     * UnBind can pair with the same args
	                                     * even if the source port shape ever
	                                     * changes upstream. */
	int bound;
	int started;                        /* StartRecvPic succeeded */
	MarukoOutput output;
	MarukoRtpState rtp_state;
	H26xParamSets params;
	char mode[16];
	uint32_t bitrate;
	uint32_t fps;
	uint32_t gop;
	char server[128];
	int frame_lost;
	int is_dual_stream;
	/* Non-NULL when mode == "dual": chn 1 frames are written here.
	 * Exactly one of ts_recorder / recorder is non-NULL based on
	 * record.format ("ts" or "hevc"), set when the dual struct is
	 * built.  Both owned by MarukoBackendContext, weak ptrs here. */
	Star6eTsRecorderState *ts_recorder;
	Star6eRecorderState *recorder;
	pthread_t thread;
	volatile sig_atomic_t running;
	int started_thread;
	/* Pre-allocated stream packs to avoid alloc-per-frame in the drain
	 * loop.  Grown on demand by dual_ensure_packs() if a frame exceeds
	 * the cached cap. */
	i6c_venc_pack *stream_packs;
	uint32_t stream_packs_cap;
};

static i6c_venc_pack *dual_ensure_packs(i6c_venc_pack **packs,
	uint32_t *cap, uint32_t need)
{
	if (need > *cap) {
		i6c_venc_pack *p = realloc(*packs, need * sizeof(*p));
		if (!p)
			return NULL;
		*packs = p;
		*cap = need;
	}
	return *packs;
}

/* Reduce chn 1 bitrate by 10% on sustained pressure.  Mirrors
 * star6e_runtime.c:dual_rec_reduce_bitrate(). */
static int dual_reduce_bitrate(MI_VENC_DEV venc_dev, MI_VENC_CHN chn,
	uint32_t *current_kbps, uint32_t min_kbps)
{
	i6c_venc_chn attr = {0};
	uint32_t new_kbps;
	MI_U32 bits;

	if (maruko_mi_venc_get_chn_attr(venc_dev, chn, &attr) != 0)
		return -1;

	new_kbps = *current_kbps * 9 / 10;
	if (new_kbps < min_kbps)
		new_kbps = min_kbps;
	if (new_kbps == *current_kbps)
		return 0;

	bits = new_kbps * 1024;
	switch (attr.rate.mode) {
	case MARUKO_VENC_RC_H265_CBR:
		attr.rate.h265Cbr.bitrate = bits;
		break;
	case MARUKO_VENC_RC_H264_CBR:
		attr.rate.h264Cbr.bitrate = bits;
		break;
	case MARUKO_VENC_RC_H265_VBR:
		attr.rate.h265Vbr.maxBitrate = bits;
		break;
	case MARUKO_VENC_RC_H264_VBR:
		attr.rate.h264Vbr.maxBitrate = bits;
		break;
	case MARUKO_VENC_RC_H265_AVBR:
		attr.rate.h265Avbr.maxBitrate = bits;
		break;
	case MARUKO_VENC_RC_H264_AVBR:
		attr.rate.h264Avbr.maxBitrate = bits;
		break;
	default:
		return -1;
	}

	if (maruko_mi_venc_set_chn_attr(venc_dev, chn, &attr) != 0)
		return -1;

	printf("[maruko][dual] backpressure: bitrate %u -> %u kbps\n",
		*current_kbps, new_kbps);
	*current_kbps = new_kbps;
	return 0;
}

/* Drain frames from chn 1 and forward them via UDP.  Mirrors the
 * Star6E dual_rec_thread_fn but with the SD-card writer replaced by
 * maruko_video_send_frame (UDP only — dual-stream variant). */
static void *maruko_dual_stream_thread(void *arg)
{
	struct MarukoDualVenc *d = arg;
	MarukoBackendContext *ctx = d->ctx;
	uint32_t current_kbps = d->bitrate;
	uint32_t min_kbps = d->bitrate / 4;
	struct timespec interval_start;
	unsigned int behind_count = 0;
	unsigned int total_count = 0;
	unsigned int pressure_seconds = 0;

	if (min_kbps < 1000)
		min_kbps = 1000;

	clock_gettime(CLOCK_MONOTONIC, &interval_start);

	int venc_fd = maruko_mi_venc_get_fd(ctx->venc_device, d->channel);
	if (ctx->cfg.verbose)
		printf("> [maruko][dual] drain thread up (fd=%d)\n", venc_fd);

	while (d->running) {
		i6c_venc_stat stat = {0};
		i6c_venc_strm stream = {0};
		int ret;

		if (venc_fd >= 0) {
			struct pollfd pfd = { .fd = venc_fd, .events = POLLIN };
			(void)poll(&pfd, 1, 1000);
			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				maruko_mi_venc_close_fd(ctx->venc_device,
					d->channel);
				venc_fd = -1;
				usleep(1000);
				continue;
			}
			if (!(pfd.revents & POLLIN))
				continue;
		}

		ret = maruko_mi_venc_query(ctx->venc_device, d->channel, &stat);
		if (ret != 0 || stat.curPacks == 0) {
			usleep(venc_fd >= 0 ? 100 : 1000);
			continue;
		}

		stream.count = stat.curPacks;
		stream.packet = dual_ensure_packs(&d->stream_packs,
			&d->stream_packs_cap, stat.curPacks);
		if (!stream.packet) {
			usleep(1000);
			continue;
		}

		ret = maruko_mi_venc_get_stream(ctx->venc_device, d->channel,
			&stream, g_maruko_running ? 40 : 0);
		if (ret != 0) {
			if (ret == -EAGAIN || ret == EAGAIN)
				usleep(1000);
			continue;
		}

		if (g_maruko_running) {
			if (d->is_dual_stream) {
				(void)maruko_video_send_frame(&stream,
					&d->output, &d->rtp_state, &d->params,
					&ctx->cfg, NULL);
			} else if (d->ts_recorder) {
				/* Skip slow SD writes during shutdown — keep
				 * draining to prevent VPE backpressure while
				 * pipeline tears down. */
				(void)maruko_ts_recorder_write_stream(
					d->ts_recorder, &stream);
			} else if (d->recorder) {
				(void)maruko_recorder_write_frame(
					d->recorder, &stream);
			}
		}

		(void)maruko_mi_venc_release_stream(ctx->venc_device,
			d->channel, &stream);

		total_count++;
		if (stat.curPacks >= 2)
			behind_count++;

		{
			struct timespec now;
			long long elapsed_ms;

			clock_gettime(CLOCK_MONOTONIC, &now);
			elapsed_ms = (long long)(now.tv_sec -
				interval_start.tv_sec) * 1000LL +
				(long long)(now.tv_nsec -
				interval_start.tv_nsec) / 1000000LL;

			if (elapsed_ms >= 1000) {
				if (total_count > 0 &&
				    behind_count > total_count * 4 / 5) {
					pressure_seconds++;
					if (pressure_seconds >= 3) {
						dual_reduce_bitrate(
							ctx->venc_device,
							d->channel,
							&current_kbps,
							min_kbps);
						pressure_seconds = 0;
					}
				} else {
					pressure_seconds = 0;
				}
				behind_count = 0;
				total_count = 0;
				interval_start = now;
			}
		}
	}

	if (venc_fd >= 0)
		maruko_mi_venc_close_fd(ctx->venc_device, d->channel);

	return NULL;
}

/* Compose the chn 1 attribute block.  Mirrors the chn 0 path in
 * maruko_start_venc (codec branch + fill helper) but lets the caller
 * override bitrate / fps / gop independently. */
static void dual_fill_attr(i6c_venc_chn *attr,
	const MarukoBackendConfig *base_cfg,
	uint32_t width, uint32_t height,
	uint32_t bitrate_kbps, uint32_t framerate, uint32_t gop)
{
	MarukoBackendConfig dual_cfg = *base_cfg;
	dual_cfg.venc_max_rate = bitrate_kbps;
	dual_cfg.venc_gop_size = gop;

	if (base_cfg->rc_codec == PT_H265) {
		attr->attrib.codec = I6C_VENC_CODEC_H265;
		maruko_fill_h26x_attr(&attr->attrib.h265, width, height);
	} else {
		attr->attrib.codec = I6C_VENC_CODEC_H264;
		maruko_fill_h26x_attr(&attr->attrib.h264, width, height);
	}

	uint32_t safe_gop = gop ? gop : 1;
	MI_U32 bit_rate_bits = bitrate_kbps * 1024;
	fill_maruko_rc_attr(attr, &dual_cfg, safe_gop, bit_rate_bits, framerate);
}

int maruko_pipeline_start_dual(MarukoBackendContext *ctx,
	uint32_t bitrate, uint32_t fps, double gop_sec,
	const char *mode, const char *server, int frame_lost)
{
	struct MarukoDualVenc *d;
	MI_VENC_DEV dev = ctx->venc_device;
	MI_VENC_CHN chn = 1;
	uint32_t sensor_fps = ctx->sensor.fps;
	uint32_t gop_frames;
	int ret;

	if (!mode)
		return -1;

	if (sensor_fps == 0)
		sensor_fps = 30;
	if (fps == 0)
		fps = sensor_fps;
	if (fps > sensor_fps)
		fps = sensor_fps;
	if (bitrate == 0)
		bitrate = ctx->cfg.venc_max_rate;
	if (bitrate == 0)
		bitrate = 8000;
	gop_frames = (uint32_t)(gop_sec * fps + 0.5);
	if (gop_frames < 1)
		gop_frames = fps;

	d = calloc(1, sizeof(*d));
	if (!d)
		return -1;

	d->ctx = ctx;
	d->channel = chn;
	d->bitrate = bitrate;
	d->fps = fps;
	d->gop = gop_frames;
	d->frame_lost = frame_lost;
	d->is_dual_stream = (strcmp(mode, "dual-stream") == 0);
	if (d->is_dual_stream) {
		d->ts_recorder = NULL;
		d->recorder = NULL;
	} else if (strcmp(ctx->cfg.record.format, "hevc") == 0) {
		d->ts_recorder = NULL;
		d->recorder = &ctx->recorder;
	} else {
		d->ts_recorder = &ctx->ts_recorder;
		d->recorder = NULL;
	}
	snprintf(d->mode, sizeof(d->mode), "%s", mode);
	if (server)
		snprintf(d->server, sizeof(d->server), "%s", server);
	d->output.socket_handle = -1;

	/* SDK pattern: both VENC channels must exist before SCL -> chn 0
	 * fan-out is established.  bind_maruko_pipeline already bound
	 * SCL -> chn 0 (RING) — temporarily unbind so chn 1 can be
	 * created, then re-bind.  Skipping this leaves chn 0's encoder
	 * in a degraded ~5 Mbps mode (verified empirically: 25 Mbps ->
	 * 5 Mbps when chn 1 is added without rebind).  Safe here because
	 * configure_graph runs before the encoder loop, so no in-flight
	 * frames are lost during the unbind window. */
	int rebind_main = 0;
	if (ctx->bound_vpe_venc) {
		(void)MI_SYS_UnBindChnPort(&ctx->vpe_port, &ctx->venc_port);
		ctx->bound_vpe_venc = 0;
		rebind_main = 1;
	}

	/* CreateChn(dev, 1, &attr) — Phase 7 probe confirmed this works
	 * after chn 0 is fully started.  Ring pool is per-dev and was
	 * already provisioned for chn 0 in maruko_start_venc; no
	 * second pool reservation is required for chn 1. */
	i6c_venc_chn attr = {0};
	dual_fill_attr(&attr, &ctx->cfg, ctx->cfg.image_width,
		ctx->cfg.image_height, bitrate, fps, gop_frames);
	ret = maruko_mi_venc_create_chn(dev, chn, &attr);
	if (ret != 0) {
		fprintf(stderr,
			"WARNING: [maruko][dual] CreateChn(dev=%d, chn=%d)"
			" failed %d\n", (int)dev, (int)chn, ret);
		if (rebind_main) {
			(void)MI_SYS_BindChnPort2(&ctx->vpe_port, &ctx->venc_port,
				ctx->sensor.fps, ctx->sensor.fps,
				I6_SYS_LINK_RING, 0);
			ctx->bound_vpe_venc = 1;
		}
		free(d);
		return -1;
	}

	/* Maruko dual-VENC topology (per Maruko SDK sample_venc.c):
	 * chn 1 is sourced from chn 0's VENC output port (NOT from SCL).
	 * The VENC hardware exposes a chn 0 -> chn 1 HW_RING fan-out so
	 * the second encoder sees the same input frames.  Confirmed
	 * empirically: binding SCL/0/0/0 -> VENC/0/1/0 directly returns
	 * 0xA0092012 (SYS busy) because chn 0 already holds the SCL
	 * output port in RING mode, and the SCL port cannot multi-consume.
	 *
	 * The SDK sample does NOT call SetInputSourceConfig on chn 1 —
	 * sub-channels default to NORMAL_FRMBASE (handshake by ~3-buffer
	 * frame mode) which is what chn 0 -> chn 1 HW_RING expects on
	 * the destination side.  Setting RING_UNIFIED_DMA on chn 1
	 * starves chn 1 (the encoder waits for ring DMA frames that
	 * never arrive). */

	ret = maruko_mi_venc_start_recv(dev, chn);
	if (ret != 0) {
		fprintf(stderr,
			"WARNING: [maruko][dual] StartRecvPic(chn=%d)"
			" failed %d\n", (int)chn, ret);
		(void)maruko_mi_venc_destroy_chn(dev, chn);
		if (rebind_main) {
			(void)MI_SYS_BindChnPort2(&ctx->vpe_port, &ctx->venc_port,
				ctx->sensor.fps, ctx->sensor.fps,
				I6_SYS_LINK_RING, 0);
			ctx->bound_vpe_venc = 1;
		}
		free(d);
		return -1;
	}
	d->started = 1;

	/* Re-establish SCL -> VENC chn 0.  Both channels now exist, so
	 * the encoder enters dual-channel mode correctly. */
	if (rebind_main) {
		ret = MI_SYS_BindChnPort2(&ctx->vpe_port, &ctx->venc_port,
			ctx->sensor.fps, ctx->sensor.fps,
			I6_SYS_LINK_RING, 0);
		if (ret != 0) {
			fprintf(stderr,
				"WARNING: [maruko][dual] re-bind SCL->chn0"
				" failed %d\n", ret);
			(void)maruko_mi_venc_stop_recv(dev, chn);
			(void)maruko_mi_venc_destroy_chn(dev, chn);
			free(d);
			return -1;
		}
		ctx->bound_vpe_venc = 1;
	}

	if (frame_lost) {
		MI_VENC_ParamFrameLost_t lost = {0};
		lost.bFrmLostOpen = 1;
		lost.eFrmLostMode = E_MI_VENC_FRMLOST_NORMAL;
		lost.u32FrmLostBpsThr =
			pipeline_common_frame_lost_threshold(bitrate);
		lost.u32EncFrmGaps = 0;
		(void)maruko_mi_venc_set_frame_lost(dev, chn, &lost);
	}

	/* Source: chn 0's VENC output port.  Dst: chn 1 input.
	 * Maruko SDK sample_venc.c:
	 *   src = VENC/dev/MainChn/0  (chn 0 output)
	 *   dst = VENC/dev/SubChn/0   (chn 1)
	 *   bind type = HW_RING
	 */
	MI_SYS_ChnPort_t src_port = {
		.module = I6_SYS_MOD_VENC, .device = (MI_U32)dev,
		.channel = ctx->venc_channel, .port = 0,
	};
	d->port = (MI_SYS_ChnPort_t){
		.module = I6_SYS_MOD_VENC, .device = (MI_U32)dev,
		.channel = chn, .port = 0,
	};

	ret = MI_SYS_BindChnPort2(&src_port, &d->port,
		sensor_fps, fps, I6_SYS_LINK_RING, 0);
	if (ret != 0) {
		fprintf(stderr,
			"WARNING: [maruko][dual] bind VENC chn0->chn%d"
			" failed %d\n", (int)chn, ret);
		(void)maruko_mi_venc_stop_recv(dev, chn);
		(void)maruko_mi_venc_destroy_chn(dev, chn);
		free(d);
		return -1;
	}
	d->bound = 1;
	d->bind_src = src_port;  /* remember for unbind */

	(void)MI_SYS_SetChnOutputPortDepth(&d->port, 1, 3);

	if (d->is_dual_stream && d->server[0]) {
		VencOutputUri uri;
		if (venc_config_parse_output_uri(d->server, &uri) == 0) {
			if (maruko_output_init(&d->output, &uri,
				ctx->cfg.connected_udp) == 0) {
				maruko_video_init_rtp_state(&d->rtp_state,
					ctx->cfg.rc_codec, sensor_fps);
				printf("> [maruko][dual] dual-stream chn=%d ->"
					" %s (%u kbps, %u fps, gop=%u)\n",
					(int)chn, d->server, bitrate, fps,
					gop_frames);
			} else {
				fprintf(stderr,
					"WARNING: [maruko][dual] output_init"
					" failed for %s\n", d->server);
			}
		} else {
			fprintf(stderr,
				"WARNING: [maruko][dual] cannot parse"
				" record.server '%s'\n", d->server);
		}
	}

	d->running = 1;
	if (pthread_create(&d->thread, NULL, maruko_dual_stream_thread, d)
	    != 0) {
		/* Spawn failure: tear down everything we just allocated,
		 * leaving ctx->dual NULL so the caller continues with
		 * chn 0 only. */
		fprintf(stderr,
			"ERROR: [maruko][dual] pthread_create failed\n");
		d->running = 0;
		if (d->bound) {
			(void)MI_SYS_UnBindChnPort(&d->bind_src, &d->port);
			d->bound = 0;
		}
		if (d->started)
			(void)maruko_mi_venc_stop_recv(dev, chn);
		(void)maruko_mi_venc_destroy_chn(dev, chn);
		maruko_output_teardown(&d->output);
		free(d->stream_packs);
		free(d);
		return -1;
	}
	d->started_thread = 1;

	ctx->dual = d;
	venc_api_dual_register(d->channel, d->bitrate, d->fps, d->gop,
		d->frame_lost);
	return 0;
}

void maruko_pipeline_stop_dual(MarukoBackendContext *ctx)
{
	if (!ctx || !ctx->dual)
		return;

	venc_api_dual_unregister();

	struct MarukoDualVenc *d = ctx->dual;
	MI_VENC_DEV dev = ctx->venc_device;

	if (d->started_thread) {
		d->running = 0;
		pthread_join(d->thread, NULL);
		d->started_thread = 0;
	}

	if (d->started) {
		(void)maruko_mi_venc_stop_recv(dev, d->channel);
		d->started = 0;
	}
	if (d->bound) {
		(void)MI_SYS_UnBindChnPort(&d->bind_src, &d->port);
		d->bound = 0;
	}
	(void)maruko_mi_venc_destroy_chn(dev, d->channel);

	maruko_output_teardown(&d->output);
	free(d->stream_packs);
	free(d);
	ctx->dual = NULL;
}

int maruko_pipeline_configure_graph(MarukoBackendContext *ctx)
{

	if (setup_maruko_graph_dimensions(ctx) != 0)
		return -1;

	uint32_t out_w = ctx->cfg.image_width;
	uint32_t out_h = ctx->cfg.image_height;

	/* Effective SCL input dims: sensor capt after overscan clamp, then
	 * sensor binning (mode.output < capt).  Mirrors the effective-dim
	 * derivation in setup_maruko_graph_dimensions() — re-derived here
	 * so the precrop rect is computed against the same surface that
	 * actually feeds the SCL stage. */
	uint32_t scl_in_w = ctx->sensor.plane.capt.width;
	uint32_t scl_in_h = ctx->sensor.plane.capt.height;
	if (ctx->sensor.mode.output.width > 0 &&
	    ctx->sensor.mode.output.width < scl_in_w) {
		scl_in_w = ctx->sensor.mode.output.width;
		scl_in_h = ctx->sensor.mode.output.height;
	}
	PipelinePrecropRect precrop = pipeline_common_compute_precrop(
		scl_in_w, scl_in_h, out_w, out_h, ctx->cfg.keep_aspect ? true : false);
	PipelinePrecropRect base_precrop = precrop;
	if (ctx->cfg.verbose) {
		if (precrop.x || precrop.y ||
		    precrop.w != scl_in_w || precrop.h != scl_in_h) {
			printf("  - Precrop: %ux%u -> %ux%u (offset %u,%u)\n",
				scl_in_w, scl_in_h, precrop.w, precrop.h,
				precrop.x, precrop.y);
		}
	}

	/* Approach C zoom: when zoom_pct ∈ (0, 1), shrink BOTH the SCL
	 * output dim AND the SCL crop window — SCL reads exactly the rect
	 * and emits it 1:1, no upscale, no bandwidth pressure.  The encoded
	 * resolution drops to the crop dim; SPS/PPS carries it to the
	 * receiver.  zoom_pct is MUT_RESTART so this only runs at start;
	 * live x/y pan is handled by maruko_pipeline_apply_zoom which
	 * re-issues SetPortConfig with new offsets at the same dim. */
	if (ctx->cfg.zoom_pct > 0.0 && ctx->cfg.zoom_pct < 1.0) {
		uint32_t zw = out_w;
		uint32_t zh = out_h;
		MaruWindowRect_t zrect;
		maruko_compute_zoom_dim(out_w, out_h, ctx->cfg.zoom_pct, &zw, &zh);
		zrect = maruko_compute_zoom_rect(precrop.w, precrop.h, zw, zh,
			ctx->cfg.zoom_x, ctx->cfg.zoom_y);
		if (ctx->cfg.verbose)
			printf("> [maruko] Zoom: pct=%.3f → %ux%u "
				"(rect %ux%u@%u,%u within precrop %ux%u)\n",
				ctx->cfg.zoom_pct, zw, zh,
				zrect.u16Width, zrect.u16Height,
				zrect.u16X, zrect.u16Y, precrop.w, precrop.h);
		ctx->cfg.image_width = zw;
		ctx->cfg.image_height = zh;
		out_w = zw;
		out_h = zh;
		/* Replace the AR-precrop with the zoom rect (offset within
		 * precrop area).  Position in scl_in coordinates so the SCL
		 * crop is absolute. */
		precrop.x = (uint16_t)(precrop.x + zrect.u16X);
		precrop.y = (uint16_t)(precrop.y + zrect.u16Y);
		precrop.w = zrect.u16Width;
		precrop.h = zrect.u16Height;
	}
	/* Stash the AR-matched base crop for live pan.  The zoom rect is
	 * always positioned inside this surface, not inside the full SCL
	 * input, or panning can leak outside keep-aspect framing. */
	ctx->scl_crop_x = base_precrop.x;
	ctx->scl_crop_y = base_precrop.y;
	ctx->scl_crop_w = base_precrop.w;
	ctx->scl_crop_h = base_precrop.h;

	if (maruko_start_vif(&ctx->sensor) != 0)
		return -1;
	ctx->vif_started = 1;

	if (maruko_start_vpe(&ctx->sensor, out_w, out_h,
	    ctx->cfg.vpe_level_3dnr, &precrop) != 0)
		return -1;
	ctx->vpe_started = 1;
	if (ctx->cfg.zoom_pct > 0.0 && ctx->cfg.zoom_pct < 1.0) {
		maruko_pipeline_set_zoom_status(ctx->cfg.zoom_pct,
			out_w, out_h, precrop.x, precrop.y,
			precrop.w, precrop.h);
	} else {
		maruko_pipeline_clear_zoom_status();
	}

	/* Debug OSD must initialise on the main thread BEFORE the VENC
	 * kthread spawns: the kernel mi_rgn driver creates a singlethread
	 * workqueue inside MI_RGN_InitDev, and the v5.10 OpenIPC kernel
	 * rejects that allocation when invoked from a non-main thread.
	 * SCL device/channel exist by this point (maruko_start_vpe), so
	 * AttachToChn can resolve to SCL/0/0/0 immediately. */
	if (ctx->cfg.show_osd && !ctx->debug_osd) {
		ctx->debug_osd = debug_osd_create(out_w, out_h, NULL);
		if (!ctx->debug_osd)
			fprintf(stderr,
				"WARNING: [maruko] debug OSD init failed — "
				"continuing without overlay\n");
	}

	/* IMU: opt-in BMI270 reader, FIFO mode (frame-synced via imu_drain
	 * in the per-frame loop).  The push callback is a stub right now —
	 * samples are read and discarded; future telemetry/sidecar export
	 * can replace the stub without touching the lifecycle.
	 *
	 * Init MUST run BEFORE bind_maruko_pipeline() because that path
	 * calls MI_VENC_StartRecvPic, and the IMU's auto-bias calibration
	 * is a blocking ~2 s loop (400 samples @ 200 Hz default).  On
	 * Maruko, blocking the main thread for 2 s after StartRecvPic
	 * leaves the VENC fd in a state where poll() never returns
	 * POLLIN and the stream loop never progresses (verified empirically
	 * on 192.168.2.12: drops to 0 fps with 2 s cal, recovers cleanly
	 * with 250 ms cal — the issue is the blocking duration vs the
	 * VENC's internal queue, not the IMU code itself).  Star6E does
	 * not exhibit this; pre-init only on Maruko. */
	if (ctx->cfg.imu.enabled && !ctx->imu) {
		ImuConfig imu_cfg = {
			.i2c_device = ctx->cfg.imu.i2c_device,
			.i2c_addr = ctx->cfg.imu.i2c_addr,
			.sample_rate_hz = ctx->cfg.imu.sample_rate_hz,
			.gyro_range_dps = ctx->cfg.imu.gyro_range_dps,
			.cal_file = ctx->cfg.imu.cal_file,
			.cal_samples = ctx->cfg.imu.cal_samples,
			.push_fn = maruko_pipeline_imu_push,
			.push_ctx = ctx,
			.use_thread = 0,
		};
		ctx->imu = imu_init(&imu_cfg);
		if (ctx->imu) {
			imu_start(ctx->imu);
		} else {
			fprintf(stderr,
				"WARNING: [maruko] IMU init failed, "
				"continuing without IMU\n");
		}
	}

	if (bind_maruko_pipeline(ctx) != 0)
		return -1;
	/* Initial zoom is built into SCL setup above (zoom rect = precrop,
	 * encoded dim = crop dim).  Live x/y pan goes through
	 * maruko_pipeline_apply_zoom; pct change triggers MUT_RESTART. */

	/* Phase 5: audio capture + RTP/UDP output.  Init AFTER bind_maruko
	 * so ctx->output socket is up; init returns 0 even on failure (warns
	 * and leaves ctx->audio.started=0). */
	(void)maruko_audio_init(&ctx->audio, &ctx->cfg, &ctx->output);
	if (ctx->audio.started) {
		audio_ring_init(&ctx->audio_recorder_ring);
		/* Aligned pointer write — encode thread picks this up on its
		 * next loop iteration. */
		ctx->audio.rec_ring = &ctx->audio_recorder_ring;
	}

	/* TS recorder state — initialised regardless of mode so the chn 1
	 * drain thread can call maruko_ts_recorder_write_stream() safely
	 * (no-op while fd<0).  Audio rate/channels reflect whether
	 * maruko_audio_init succeeded — when active, the PMT advertises
	 * audio and the recorder pops PCM frames off audio_recorder_ring. */
	{
		/* Mirror star6e_runtime: route Opus capture into Opus-in-TS PMT,
		 * PCM into SMPTE 302M.  G.711 is not muxed (rate stays 0). */
		int parsed = audio_codec_parse_name(ctx->cfg.audio.codec);
		uint8_t ts_codec = (parsed == AUDIO_CODEC_TYPE_OPUS)
			? TS_AUDIO_CODEC_OPUS : TS_AUDIO_CODEC_PCM_S302M;
		uint32_t rate = 0, ch = 0;
		if (ctx->audio.started &&
		    (parsed == AUDIO_CODEC_TYPE_RAW ||
		     parsed == AUDIO_CODEC_TYPE_OPUS)) {
			rate = ctx->audio.sample_rate;
			ch   = ctx->audio.channels;
		}
		star6e_ts_recorder_init(&ctx->ts_recorder, rate, (uint8_t)ch,
			ts_codec);
	}
	star6e_recorder_init(&ctx->recorder);
	if (ctx->cfg.record.max_seconds > 0)
		ctx->ts_recorder.max_seconds = ctx->cfg.record.max_seconds;
	if (ctx->cfg.record.max_mb > 0)
		ctx->ts_recorder.max_bytes =
			(uint64_t)ctx->cfg.record.max_mb * 1024 * 1024;

	/* Phase 7: dual VENC chn 1.  Started AFTER bind_maruko_pipeline
	 * succeeds — the Phase 7 SDK probe confirmed CreateChn(dev,1,...)
	 * needs chn 0 fully bound first.  Failure is non-fatal: ctx->dual
	 * stays NULL and the daemon continues with chn 0 only.
	 *
	 * Phase 6: also recognises "dual" (chn 1 → TS file) in addition to
	 * the existing "dual-stream" (chn 1 → UDP). */
	if (ctx->cfg.record.enabled &&
	    (strcmp(ctx->cfg.record.mode, "dual-stream") == 0 ||
	     strcmp(ctx->cfg.record.mode, "dual") == 0) &&
	    !ctx->dual) {
		(void)maruko_pipeline_start_dual(ctx,
			ctx->cfg.record.bitrate,
			ctx->cfg.record.fps,
			ctx->cfg.record.gop_size,
			ctx->cfg.record.mode,
			ctx->cfg.record.server,
			ctx->cfg.record.frame_lost);
	}

	/* Phase 6: open recorder for "mirror" (chn 0 → file) and "dual"
	 * (chn 1 → file) modes.  Must run AFTER start_dual so that, in
	 * dual mode, the drain thread sees the recorder fd >= 0 by the
	 * time it pulls the first chn 1 frame.  Started here so that a
	 * follow-up SIGHUP reload that flips mode also re-opens the file.
	 *
	 * Format dispatch: "ts" → TS muxer + audio interleaving;
	 * "hevc" → raw HEVC stream (no audio).  Both backends share the
	 * same disk-space / rotation / sync_file_range plumbing. */
	if (ctx->cfg.record.enabled && ctx->cfg.record.dir[0] &&
	    (strcmp(ctx->cfg.record.mode, "mirror") == 0 ||
	     strcmp(ctx->cfg.record.mode, "dual") == 0)) {
		if (strcmp(ctx->cfg.record.format, "hevc") == 0) {
			if (star6e_recorder_start(&ctx->recorder,
			    ctx->cfg.record.dir) == 0) {
				printf("> [maruko] recording HEVC to %s (%s)\n",
					ctx->cfg.record.dir,
					ctx->cfg.record.mode);
			}
		} else if (strcmp(ctx->cfg.record.format, "ts") == 0) {
			if (star6e_ts_recorder_start(&ctx->ts_recorder,
			    ctx->cfg.record.dir,
			    ctx->audio.started
				? &ctx->audio_recorder_ring : NULL) == 0) {
				printf("> [maruko] recording TS to %s (%s%s)\n",
					ctx->cfg.record.dir,
					ctx->cfg.record.mode,
					ctx->audio.started ? " + audio" : "");
			}
		} else {
			fprintf(stderr,
				"WARNING: [maruko] record.format '%s' not "
				"supported (ts|hevc) — recording disabled\n",
				ctx->cfg.record.format);
		}
	}

	ctx->output_enabled = 1;
	printf("> [maruko] pipeline configured\n");
	printf("  - Sensor: %ux%u @ %u\n",
		ctx->sensor.mode.crop.width, ctx->sensor.mode.crop.height,
		ctx->sensor.fps);
	printf("  - Image : %ux%u\n",
		ctx->cfg.image_width, ctx->cfg.image_height);
	if (ctx->cfg.image_width != ctx->sensor.mode.crop.width ||
	    ctx->cfg.image_height != ctx->sensor.mode.crop.height) {
		printf("  - VPE scaling: sensor %ux%u -> encode %ux%u\n",
			ctx->sensor.mode.crop.width,
			ctx->sensor.mode.crop.height,
			ctx->cfg.image_width, ctx->cfg.image_height);
	}
	printf("  - 3DNR  : level %d\n", ctx->cfg.vpe_level_3dnr);

	return 0;
}

/* ── Scene-detector stream decoders (i6c_venc_strm) ──────────────────── */

static uint32_t maruko_scene_frame_size(const i6c_venc_strm *s)
{
	uint32_t t = 0;
	unsigned int i;
	if (!s || !s->packet) return 0;
	for (i = 0; i < s->count; i++) t += s->packet[i].length;
	return t;
}

static uint8_t maruko_scene_is_idr(const i6c_venc_strm *s, int codec)
{
	unsigned int i;
	if (!s || !s->packet) return 0;
	for (i = 0; i < s->count; i++) {
		const i6c_venc_pack *p = &s->packet[i];
		unsigned int k, n = p->packNum > 8 ? 8 : p->packNum;
		if (n > 0) {
			for (k = 0; k < n; k++) {
				if (codec == 0 && p->packetInfo[k].packType.h264Nalu == 5)
					return 1;
				if (codec != 0 && p->packetInfo[k].packType.h265Nalu == 19)
					return 1;
			}
		} else {
			if (codec == 0 && p->naluType.h264Nalu == 5) return 1;
			if (codec != 0 && p->naluType.h265Nalu == 19) return 1;
		}
	}
	return 0;
}

static void maruko_scene_request_idr(void *ctx_ptr)
{
	MarukoBackendContext *ctx = ctx_ptr;
	if (idr_rate_limit_allow(ctx->venc_channel))
		maruko_mi_venc_request_idr(ctx->venc_device, ctx->venc_channel, 1);
}

/* Per-iteration state for maruko_pipeline_run / maruko_pipeline_process_stream.
 * Keeps the per-frame work in its own function (mirroring
 * star6e_runtime_process_stream) without ballooning its parameter list. */
typedef struct {
	MarukoRtpState rtp_state;
	H26xParamSets param_sets;
	RtpSidecarSender sidecar;
	StreamMetricsState metrics;
	HevcRtpStats pktzr_interval;
	i6c_venc_pack *cached_packs;
	uint32_t cached_packs_cap;
	unsigned int frame_counter;
	int venc_fd;
	uint64_t last_activity_us;
	uint64_t last_warn_us;
} MarukoStreamRuntime;

#define MARUKO_IDLE_ABORT_US ((uint64_t)20 * 1000000ULL)
#define MARUKO_IDLE_WARN_US  ((uint64_t)1 * 1000000ULL)
#define MARUKO_PKTZR_VERBOSE_ACTIVE(ctx) ((ctx)->cfg.verbose && \
	(ctx)->cfg.rc_codec == PT_H265 && \
	(ctx)->cfg.stream_mode == MARUKO_STREAM_RTP)

static void maruko_pipeline_init_streaming(MarukoBackendContext *ctx,
	MarukoStreamRuntime *rt)
{
	memset(rt, 0, sizeof(*rt));
	if (ctx->cfg.stream_mode == MARUKO_STREAM_RTP) {
		maruko_video_init_rtp_state(&rt->rtp_state, ctx->cfg.rc_codec,
			ctx->sensor.fps);
		rtp_sidecar_sender_init(&rt->sidecar, ctx->cfg.sidecar_port);
	}
	if (ctx->cfg.verbose) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		stream_metrics_start(&rt->metrics, &now);
	}
	/* Block on MI_VENC_GetFd(chn) via poll() instead of spinning on
	 * Query + usleep(500).  The fd signals POLLIN when a frame is
	 * ready, so the thread wakes ~fps-times/s instead of ~2000/s.
	 * If the SDK returns fd < 0 on an unknown BSP variant, fall back
	 * to the original spin loop. */
	rt->venc_fd = maruko_mi_venc_get_fd(ctx->venc_device, ctx->venc_channel);
	rt->last_activity_us = wb_monotonic_us();
	rt->last_warn_us = rt->last_activity_us;
}

static void maruko_pipeline_cleanup_streaming(MarukoBackendContext *ctx,
	MarukoStreamRuntime *rt)
{
	if (rt->venc_fd >= 0) {
		maruko_mi_venc_close_fd(ctx->venc_device, ctx->venc_channel);
		rt->venc_fd = -1;
	}
	free(rt->cached_packs);
	rt->cached_packs = NULL;
	rt->cached_packs_cap = 0;
	rtp_sidecar_sender_close(&rt->sidecar);
}

static int maruko_pipeline_check_idle_abort(MarukoStreamRuntime *rt)
{
	uint64_t now_us = wb_monotonic_us();
	if (now_us - rt->last_warn_us >= MARUKO_IDLE_WARN_US) {
		printf("> [maruko] waiting for encoder data...\n");
		rt->last_warn_us = now_us;
	}
	if (now_us - rt->last_activity_us >= MARUKO_IDLE_ABORT_US) {
		fprintf(stderr,
			"ERROR: [maruko] no encoder data"
			" received; aborting stream loop\n");
		return -1;
	}
	return 0;
}

/* Returns: -1 fatal, 0 retry the outer loop, 1 frame ready (use *stat). */
static int maruko_pipeline_await_frame(MarukoBackendContext *ctx,
	MarukoStreamRuntime *rt, i6c_venc_stat *stat)
{
	if (rt->venc_fd >= 0) {
		/* 1 s timeout caps the g_maruko_running cancellation latency
		 * without wasting cycles on short periodic wakes.  Frames at
		 * ≥60 fps arrive well within this. */
		struct pollfd pfd = { .fd = rt->venc_fd, .events = POLLIN };
		(void)poll(&pfd, 1, 1000);
		/* POLLERR/POLLHUP/POLLNVAL: the SDK closed the fd under us
		 * (BSP quirk, pipeline reinit, VPE unbind).  Fall back to the
		 * Query+usleep path for the rest of the loop's lifetime. */
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			maruko_mi_venc_close_fd(ctx->venc_device,
				ctx->venc_channel);
			rt->venc_fd = -1;
			usleep(1000);
			return 0;
		}
		if (!(pfd.revents & POLLIN))
			return maruko_pipeline_check_idle_abort(rt);
	}

	memset(stat, 0, sizeof(*stat));
	MI_S32 ret = maruko_mi_venc_query(ctx->venc_device,
		ctx->venc_channel, stat);
	if (ret != 0) {
		if (ret == -EAGAIN || ret == EAGAIN) {
			usleep(100);
			return 0;
		}
		fprintf(stderr,
			"ERROR: [maruko] MI_VENC_Query failed %d\n", ret);
		return -1;
	}

	if (stat->curPacks == 0) {
		int rc = maruko_pipeline_check_idle_abort(rt);
		if (rc != 0)
			return rc;
		/* On the fd path this means spurious POLLIN (rare).  On the
		 * fallback spin path, sleep 500us: <5 % of the 11 ms frame
		 * period at 90 fps, keeps frame latency low while reducing
		 * idle syscalls. */
		if (rt->venc_fd < 0)
			usleep(500);
		return 0;
	}

	rt->last_activity_us = wb_monotonic_us();
	rt->last_warn_us = rt->last_activity_us;
	return 1;
}

static void maruko_pipeline_log_verbose_frame(MarukoBackendContext *ctx,
	MarukoStreamRuntime *rt, size_t total_bytes,
	const HevcRtpStats *frame_pktzr, unsigned int pack_count)
{
	StreamMetricsSample sample;
	struct timespec verbose_ts_now;

	stream_metrics_record_frame(&rt->metrics, total_bytes);
	if (MARUKO_PKTZR_VERBOSE_ACTIVE(ctx)) {
		rt->pktzr_interval.total_nals += frame_pktzr->total_nals;
		rt->pktzr_interval.single_packets += frame_pktzr->single_packets;
		rt->pktzr_interval.ap_packets += frame_pktzr->ap_packets;
		rt->pktzr_interval.ap_nals += frame_pktzr->ap_nals;
		rt->pktzr_interval.fu_packets += frame_pktzr->fu_packets;
		rt->pktzr_interval.rtp_packets += frame_pktzr->rtp_packets;
		rt->pktzr_interval.rtp_payload_bytes += frame_pktzr->rtp_payload_bytes;
	}
	clock_gettime(CLOCK_MONOTONIC, &verbose_ts_now);
	if (!stream_metrics_sample(&rt->metrics, &verbose_ts_now, &sample))
		return;

	printf("[verbose] %lds | %u fps | %u kbps"
		" | frame %u | avg %u B/frame | %u packs\n",
		sample.uptime_s, sample.fps, sample.kbps,
		rt->frame_counter, sample.avg_bytes, pack_count);
	if (MARUKO_PKTZR_VERBOSE_ACTIVE(ctx)) {
		unsigned int avg_rtp_payload =
			rt->pktzr_interval.rtp_packets > 0
			? (unsigned int)(rt->pktzr_interval.rtp_payload_bytes /
				rt->pktzr_interval.rtp_packets) : 0;
		printf("[pktzr] nals %u | rtp %u | fill %u B"
			" | single %u | ap %u/%u | fu %u\n",
			rt->pktzr_interval.total_nals,
			rt->pktzr_interval.rtp_packets,
			avg_rtp_payload,
			rt->pktzr_interval.single_packets,
			rt->pktzr_interval.ap_packets,
			rt->pktzr_interval.ap_nals,
			rt->pktzr_interval.fu_packets);
		memset(&rt->pktzr_interval, 0, sizeof(rt->pktzr_interval));
	}
	fflush(stdout);
}

/* Process one ready frame: GetStream → per-frame work → ReleaseStream
 * → optional sidecar trailer + verbose log.  Mirrors
 * star6e_runtime_process_stream().  Returns: -1 fatal, 0 ok, 1 retry
 * outer loop. */
static int maruko_pipeline_process_stream(MarukoBackendContext *ctx,
	MarukoStreamRuntime *rt, const i6c_venc_stat *stat)
{
	i6c_venc_strm stream = {0};

	stream.count = stat->curPacks;
	if (stat->curPacks > rt->cached_packs_cap) {
		free(rt->cached_packs);
		rt->cached_packs = malloc(stat->curPacks * sizeof(i6c_venc_pack));
		if (!rt->cached_packs) {
			rt->cached_packs_cap = 0;
			fprintf(stderr,
				"ERROR: [maruko] packet alloc failed\n");
			return -1;
		}
		rt->cached_packs_cap = stat->curPacks;
	}
	stream.packet = rt->cached_packs;

	/* Drain IMU FIFO BEFORE GetStream so any future telemetry/sidecar
	 * consumer sees fresh samples for the frame currently being
	 * captured.  Cheap when imu.enabled=false (state pointer is NULL).
	 * Mirrors star6e_runtime.c:727-728. */
	if (ctx->imu)
		imu_drain(ctx->imu);

	MI_S32 ret = maruko_mi_venc_get_stream(ctx->venc_device,
		ctx->venc_channel, &stream, 10);
	if (ret != 0) {
		if (ret == -EAGAIN || ret == EAGAIN) {
			usleep(100);
			return 1;
		}
		fprintf(stderr,
			"ERROR: [maruko] MI_VENC_GetStream failed %d\n", ret);
		return -1;
	}

	++rt->frame_counter;

	/* Cold-boot FPS kick: the ISP bin's AE overrides sensor timing on
	 * first init, locking FPS below target (e.g. 74fps instead of
	 * 89fps).  Re-kick MI_SNR_SetFps after ~1 second of frames to
	 * force correct sensor timing.  Same fix as Star6E CUS3A
	 * cold-boot FPS lock. */
	if (rt->frame_counter == (unsigned int)ctx->sensor.fps) {
		MI_SNR_SetFps(ctx->sensor.pad_id, ctx->sensor.fps);
		printf("> [maruko] delayed FPS kick: pad %d fps %u "
			"(cold-boot fix at frame %u)\n",
			ctx->sensor.pad_id, ctx->sensor.fps, rt->frame_counter);
	}

	/* Debug OSD overlay — Star6E parity (star6e_runtime.c:825-849). */
	if (ctx->debug_osd) {
		static unsigned int osd_prev_frame;
		static struct timespec osd_prev_ts;
		static unsigned int osd_fps;
		struct timespec osd_now;

		debug_osd_begin_frame(ctx->debug_osd);
		debug_osd_sample_cpu(ctx->debug_osd);

		clock_gettime(CLOCK_MONOTONIC, &osd_now);
		long osd_ms = (osd_now.tv_sec - osd_prev_ts.tv_sec) * 1000 +
			(osd_now.tv_nsec - osd_prev_ts.tv_nsec) / 1000000;
		if (osd_ms >= 1000) {
			unsigned int df = rt->frame_counter - osd_prev_frame;
			osd_fps = (unsigned int)(df * 1000 / (unsigned long)osd_ms);
			osd_prev_frame = rt->frame_counter;
			osd_prev_ts = osd_now;
		}

		debug_osd_text(ctx->debug_osd, 0, "fps", "%u", osd_fps);
		debug_osd_text(ctx->debug_osd, 1, "cpu", "%d%%",
			debug_osd_get_cpu(ctx->debug_osd));

		{
			int osd_row = 2;
			MarukoIntraRefreshStatus ir;
			maruko_pipeline_intra_refresh_status(&ir);
			if (ir.active) {
				debug_osd_text(ctx->debug_osd, osd_row++, "intra",
					"%s L%u q%u",
					ir.mode_name, ir.effective_lines_per_p,
					ir.effective_qp);
				debug_osd_text(ctx->debug_osd, osd_row++, "gop",
					"%.2fs %s",
					ir.effective_gop_sec,
					ir.gop_auto ? "auto" : "fixed");
			}

			MarukoZoomStatus zoom;
			maruko_pipeline_zoom_status(&zoom);
			if (zoom.active) {
				debug_osd_text(ctx->debug_osd, osd_row++, "zoom",
					"%u.%02ux %ux%u",
					zoom.level_x100 / 100,
					zoom.level_x100 % 100,
					zoom.output_w, zoom.output_h);
				debug_osd_text(ctx->debug_osd, osd_row++, "crop",
					"%ux%u+%u+%u",
					zoom.crop_w, zoom.crop_h,
					zoom.crop_x, zoom.crop_y);
			}
		}

		debug_osd_end_frame(ctx->debug_osd);
	}

	rtp_sidecar_poll(&rt->sidecar);

	uint32_t frame_rtp_ts = rt->rtp_state.timestamp;
	uint16_t seq_before = rt->rtp_state.seq;
	uint64_t ready_us = wb_monotonic_us();
	uint64_t capture_us = (stream.count > 0 && stream.packet)
		? stream.packet[0].timestamp : 0;

	int codec = (ctx->cfg.rc_codec == PT_H265) ? 1 : 0;
	uint32_t frame_size = maruko_scene_frame_size(&stream);
	uint8_t is_idr = maruko_scene_is_idr(&stream, codec);
	scene_update(&ctx->scene, frame_size, is_idr,
		maruko_scene_request_idr, ctx);
	RtpSidecarEncInfo enc_info = {0};
	scene_fill_sidecar(&ctx->scene, &enc_info);

	size_t total_bytes = 0;
	HevcRtpStats frame_pktzr = {0};
	int sidecar_subscribed = rtp_sidecar_is_subscribed(&rt->sidecar);
	if (ctx->output_enabled) {
		/* Observe pressure only when a sidecar probe is subscribed —
		 * see star6e_runtime equivalent.  Always sending: post-encode
		 * skip breaks the H.265 reference chain (HISTORY 0.9.2). */
		if (sidecar_subscribed)
			maruko_output_observe_pressure(&ctx->output);
		total_bytes = maruko_video_send_frame(&stream, &ctx->output,
			&rt->rtp_state, &rt->param_sets, &ctx->cfg,
			MARUKO_PKTZR_VERBOSE_ACTIVE(ctx) ? &frame_pktzr : NULL);
	}

	/* Mirror mode: write chn 0 frames to whichever recorder is active
	 * before the stream is released.  In dual mode the chn 1 drain
	 * thread feeds the recorder, so chn 0 must NOT also write —
	 * concurrent writes would interleave NAL units from two encoders.
	 * At most one of recorder / ts_recorder is active (format dispatch
	 * happens at start), so the inactive call is an fd<0 no-op. */
	if (!ctx->dual) {
		(void)maruko_ts_recorder_write_stream(&ctx->ts_recorder,
			&stream);
		(void)maruko_recorder_write_frame(&ctx->recorder, &stream);
	}

	/* Release the encoder stream immediately after the last consumer
	 * of stream payload (maruko_video_send_frame) so the sidecar emit
	 * and verbose printf below don't hold the VENC output slot.
	 * stream.count is captured locally for the verbose line; sidecar
	 * uses rtp_state only. */
	unsigned int pack_count = stream.count;
	(void)maruko_mi_venc_release_stream(ctx->venc_device,
		ctx->venc_channel, &stream);

	/* HTTP record control: drain the start/stop request flags so they
	 * never accumulate across reinit, then act only when this runtime
	 * actually owns the recorder.  Maruko's chn 1 drain thread (dual
	 * mode) writes the file directly — same race rationale as the
	 * `if (!ctx->dual)` guard on the chn 0 write above.  Format
	 * dispatch mirrors the config-driven start path. */
	if (!ctx->dual) {
		char rec_dir[256];
		int start_pending = venc_api_get_record_start(rec_dir,
			sizeof(rec_dir));
		int stop_pending = venc_api_get_record_stop();
		int is_hevc = (strcmp(ctx->cfg.record.format, "hevc") == 0);
		int is_ts   = (strcmp(ctx->cfg.record.format, "ts") == 0);

		if ((start_pending || stop_pending) && !is_ts && !is_hevc) {
			fprintf(stderr,
				"WARNING: [maruko] HTTP record control "
				"ignored: format='%s' not supported "
				"(ts|hevc)\n", ctx->cfg.record.format);
		} else {
			if (start_pending) {
				/* Stop both — only the format-matching one is
				 * active, but keeping both calls is cheaper
				 * than a branch and matches Star6E's pattern. */
				star6e_recorder_stop(&ctx->recorder);
				star6e_ts_recorder_stop(&ctx->ts_recorder);
				ctx->audio.rec_ring = NULL;
				if (is_hevc) {
					if (star6e_recorder_start(&ctx->recorder,
					    rec_dir) == 0)
						(void)maruko_mi_venc_request_idr(
							ctx->venc_device,
							ctx->venc_channel, 1);
				} else {
					ctx->audio.rec_ring = ctx->audio.started
						? &ctx->audio_recorder_ring
						: NULL;
					if (star6e_ts_recorder_start(
					    &ctx->ts_recorder, rec_dir,
					    ctx->audio.started
						? &ctx->audio_recorder_ring
						: NULL) == 0)
						(void)maruko_mi_venc_request_idr(
							ctx->venc_device,
							ctx->venc_channel, 1);
				}
			}
			if (stop_pending) {
				star6e_recorder_stop(&ctx->recorder);
				star6e_ts_recorder_stop(&ctx->ts_recorder);
				ctx->audio.rec_ring = NULL;
			}
		}
	}

	if (sidecar_subscribed) {
		RtpSidecarTransportInfo tinfo;
		const RtpSidecarTransportInfo *tinfo_ptr = NULL;

		/* Producer already cached fill_pct + lifetime stats inside
		 * maruko_output_observe_pressure() above — read the cache
		 * instead of re-querying.  One SIOCOUTQ ioctl / ring-fill
		 * load per frame, not two. */
		if (ctx->output.ring ||
		    ((ctx->output.transport == VENC_OUTPUT_URI_UNIX ||
		      ctx->output.transport == VENC_OUTPUT_URI_UDP) &&
		     ctx->output.socket_handle >= 0)) {
			memset(&tinfo, 0, sizeof(tinfo));
			tinfo.fill_pct = ctx->output.last_fill_pct;
			tinfo.in_pressure = ctx->output.in_pressure ? 1 : 0;
			tinfo.pressure_drops = ctx->output.pressure_drops;
			tinfo.transport_drops = ctx->output.last_full_drops;
			tinfo.packets_sent = ctx->output.last_writes;
			tinfo_ptr = &tinfo;
		}
		rtp_sidecar_send_frame_transport(&rt->sidecar,
			rt->rtp_state.ssrc, frame_rtp_ts, seq_before,
			(uint16_t)(rt->rtp_state.seq - seq_before),
			capture_us, ready_us, &enc_info, tinfo_ptr);
	}

	if (ctx->cfg.verbose)
		maruko_pipeline_log_verbose_frame(ctx, rt, total_bytes,
			&frame_pktzr, pack_count);

	return 0;
}

int maruko_pipeline_run(MarukoBackendContext *ctx)
{
	MarukoStreamRuntime rt;
	int result = -1;

	if (!ctx || (ctx->output.socket_handle < 0 && !ctx->output.ring))
		return -1;

	if (ctx->cfg.stream_mode == MARUKO_STREAM_RTP &&
	    ctx->cfg.rc_codec != PT_H265) {
		fprintf(stderr,
			"ERROR: [maruko] RTP output mode requires codec=h265;"
			" rc_codec=%d not supported — aborting pipeline\n",
			(int)ctx->cfg.rc_codec);
		return -1;
	}

	if (ctx->cfg.stream_mode == MARUKO_STREAM_RTP)
		printf("> [maruko] RTP packetizer enabled\n");
	printf("> [maruko] entering stream loop\n");

	maruko_pipeline_init_streaming(ctx, &rt);

	while (g_maruko_running) {
		i6c_venc_stat stat;
		int rc;

		if (g_maruko_reinit || venc_api_get_reinit()) {
			g_maruko_reinit = 0;
			printf("> [maruko] reinit requested, breaking stream loop\n");
			result = 1;
			goto cleanup;
		}

		rc = maruko_pipeline_await_frame(ctx, &rt, &stat);
		if (rc < 0)
			goto cleanup;
		if (rc == 0)
			continue;

		rc = maruko_pipeline_process_stream(ctx, &rt, &stat);
		if (rc < 0)
			goto cleanup;
	}
	result = 0;

cleanup:
	maruko_pipeline_cleanup_streaming(ctx, &rt);
	return result;
}

void maruko_pipeline_teardown_graph(MarukoBackendContext *ctx)
{
	if (!ctx)
		return;

	venc_api_clear_active_precrop();
	maruko_pipeline_clear_zoom_status();
	/* Stop dual VENC FIRST — its thread reads from chn 1's fd and
	 * the binding shares the VPE port with chn 0.  Tearing down chn 0
	 * before unbinding chn 1 wedges the SDK on the next reinit. */
	maruko_pipeline_stop_dual(ctx);
	/* Recorders run after stop_dual: in dual mode the drain thread
	 * has already exited and is no longer issuing writes, so close()
	 * cannot race with a write().  In mirror mode no thread holds it.
	 * Stop both — at most one is open, the inactive call is a no-op. */
	star6e_ts_recorder_stop(&ctx->ts_recorder);
	star6e_recorder_stop(&ctx->recorder);
	/* Audio teardown after recorder stop: the recorder pops from
	 * audio_recorder_ring, so the recorder must be quiet before we
	 * can destroy the ring or join the encode thread that pushes
	 * into it.  Audio still pushes into MarukoOutput, so this must
	 * also precede maruko_output_teardown. */
	{
		int audio_was_started = ctx->audio.started;
		ctx->audio.rec_ring = NULL;
		maruko_audio_teardown(&ctx->audio);
		if (audio_was_started) {
			audio_ring_destroy(&ctx->audio_recorder_ring);
			memset(&ctx->audio_recorder_ring, 0,
				sizeof(ctx->audio_recorder_ring));
		}
	}
	/* IMU stop+destroy next — push callback is a stub so order vs
	 * other teardown steps doesn't matter, but keeping the
	 * stop-then-destroy split lets a future telemetry consumer slot
	 * in without rework (Star6E parity). */
	if (ctx->imu) {
		imu_stop(ctx->imu);
		imu_destroy(ctx->imu);
		ctx->imu = NULL;
	}
	if (ctx->debug_osd) {
		debug_osd_destroy(ctx->debug_osd);
		ctx->debug_osd = NULL;
	}
	maruko_output_teardown(&ctx->output);
	if (ctx->bound_vpe_venc) {
		(void)MI_SYS_UnBindChnPort(&ctx->vpe_port, &ctx->venc_port);
		ctx->bound_vpe_venc = 0;
	}
	if (ctx->bound_isp_vpe) {
		(void)MI_SYS_UnBindChnPort(&ctx->isp_port, &ctx->vpe_port);
		ctx->bound_isp_vpe = 0;
	}
	if (ctx->bound_vif_vpe) {
		(void)MI_SYS_UnBindChnPort(&ctx->vif_port, &ctx->isp_port);
		ctx->bound_vif_vpe = 0;
	}
	if (ctx->venc_started) {
		maruko_stop_venc(ctx->venc_device, ctx->venc_channel,
			ctx->venc_dev_created);
		ctx->venc_started = 0;
		ctx->venc_dev_created = 0;
	}
	if (ctx->vpe_started) {
		maruko_stop_vpe_channels();
		ctx->vpe_started = 0;
	}
	if (ctx->vif_started) {
		maruko_stop_vif();
		ctx->vif_started = 0;
	}
	if (ctx->sensor_enabled) {
		(void)MI_SNR_Disable(ctx->sensor.pad_id);
		ctx->sensor_enabled = 0;
	}
}

void maruko_pipeline_teardown(MarukoBackendContext *ctx)
{
	/* Pause first: venc_httpd_stop() only detaches the worker thread, so
	 * a request accepted before the listen socket closed may still be in
	 * dispatch.  pause() drains it and gates any further dispatch with
	 * 503 — by the time it returns, no handler is touching SDK state. */
	venc_httpd_pause();
	venc_httpd_stop();
	maruko_cus3a_stop();
	maruko_pipeline_teardown_graph(ctx);
	if (ctx && ctx->system_initialized) {
		(void)MI_SYS_Exit();
		ctx->system_initialized = 0;
		printf("> [maruko] stage teardown: MI_SYS_Exit\n");
	}
	maruko_mi_deinit();
	/* No resume: the process is exiting (or about to). */
}
