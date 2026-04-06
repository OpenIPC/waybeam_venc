#include "maruko_pipeline.h"

#include "isp_runtime.h"
#include "maruko_bindings.h"
#include "maruko_config.h"
#include "maruko_output.h"
#include "maruko_video.h"
#include "pipeline_common.h"
#include "rtp_sidecar.h"
#include "sensor_select.h"
#include "stream_metrics.h"
#include "venc_api.h"
#include "venc_httpd.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_us(void)
{
	struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
	clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
	return (uint64_t)ts.tv_sec * 1000000ULL +
	       (uint64_t)(ts.tv_nsec / 1000);
}

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
static int g_maruko_isp_initialized = 0;
static i6c_isp_impl g_maruko_isp;
static i6c_scl_impl g_maruko_scl;
static int g_maruko_isp_loaded = 0;
static int g_maruko_scl_loaded = 0;
static int g_maruko_isp_dev_created = 0;
static int g_maruko_scl_dev_created = 0;
static int g_maruko_isp_chn_created = 0;
static int g_maruko_scl_chn_created = 0;

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

/* Enable CUS3A framework once using the progressive sequence that
 * majestic/SDK uses during ISP init: 100 -> 110 -> 111.
 * The ISP pipeline requires CUS3A active for frame processing —
 * without it the ISP FIFO stalls at >=60fps. */
static void maruko_enable_cus3a(void)
{
	void *h = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!h)
		return;
	typedef int (*fn_t)(MI_U32 dev_id, MI_U32 channel, void *params);
	fn_t fn = (fn_t)dlsym(h, "MI_ISP_CUS3A_Enable");
	if (fn) {
		MI_BOOL p100[3] = {1, 0, 0};
		MI_BOOL p110[3] = {1, 1, 0};
		MI_BOOL p111[3] = {1, 1, 1};
		fn(0, 0, p100);
		fn(0, 0, p110);
		MI_S32 ret = fn(0, 0, p111);
		if (ret != 0) {
			fprintf(stderr,
				"WARNING: [maruko] MI_ISP_CUS3A_Enable(1,1,1) failed %d\n",
				ret);
		}
	}
	dlclose(h);
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
	MI_BOOL p111[3] = {1, 1, 1};

	(void)ctx;
	fn_cus3a = (cus3a_fn_t)lib->cus3a_enable;
	if (!fn_cus3a)
		return;

	fn_cus3a(0, 0, p100);
	fn_cus3a(0, 0, p110);
	fn_cus3a(0, 0, p111);
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
	return isp_runtime_load_bin_file(isp_bin_path, &hooks);
}

static void *maruko_load_symbol(void *handle, const char *lib_name,
	const char *sym_name)
{
	void *sym = dlsym(handle, sym_name);
	if (!sym) {
		const char *err = dlerror();
		fprintf(stderr, "ERROR: [maruko] dlsym(%s:%s) failed: %s\n",
			lib_name, sym_name, err ? err : "unknown");
	}
	return sym;
}

static void i6c_isp_unload(i6c_isp_impl *isp)
{
	if (!isp)
		return;
	if (isp->handle)
		dlclose(isp->handle);
	memset(isp, 0, sizeof(*isp));
}

static int i6c_isp_load(i6c_isp_impl *isp)
{
	if (!isp)
		return -1;
	memset(isp, 0, sizeof(*isp));
	isp->handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!isp->handle) {
		const char *err = dlerror();
		fprintf(stderr,
			"ERROR: [maruko] dlopen(libmi_isp.so) failed: %s\n",
			err ? err : "unknown");
		return -1;
	}

	isp->fnCreateDevice = (int (*)(int, unsigned int *))
		maruko_load_symbol(isp->handle, "libmi_isp.so",
			"MI_ISP_CreateDevice");
	isp->fnDestroyDevice = (int (*)(int))
		maruko_load_symbol(isp->handle, "libmi_isp.so",
			"MI_ISP_DestoryDevice");
	isp->fnCreateChannel = (int (*)(int, int, i6c_isp_chn *))
		maruko_load_symbol(isp->handle, "libmi_isp.so",
			"MI_ISP_CreateChannel");
	isp->fnDestroyChannel = (int (*)(int, int))
		maruko_load_symbol(isp->handle, "libmi_isp.so",
			"MI_ISP_DestroyChannel");
	isp->fnSetChannelParam = (int (*)(int, int, i6c_isp_para *))
		maruko_load_symbol(isp->handle, "libmi_isp.so",
			"MI_ISP_SetChnParam");
	isp->fnStartChannel = (int (*)(int, int))
		maruko_load_symbol(isp->handle, "libmi_isp.so",
			"MI_ISP_StartChannel");
	isp->fnStopChannel = (int (*)(int, int))
		maruko_load_symbol(isp->handle, "libmi_isp.so",
			"MI_ISP_StopChannel");
	isp->fnDisablePort = (int (*)(int, int, int))
		maruko_load_symbol(isp->handle, "libmi_isp.so",
			"MI_ISP_DisableOutputPort");
	isp->fnEnablePort = (int (*)(int, int, int))
		maruko_load_symbol(isp->handle, "libmi_isp.so",
			"MI_ISP_EnableOutputPort");
	isp->fnSetPortConfig = (int (*)(int, int, int, i6c_isp_port *))
		maruko_load_symbol(isp->handle, "libmi_isp.so",
			"MI_ISP_SetOutputPortParam");

	if (!isp->fnCreateDevice || !isp->fnDestroyDevice ||
	    !isp->fnCreateChannel || !isp->fnDestroyChannel ||
	    !isp->fnSetChannelParam || !isp->fnStartChannel ||
	    !isp->fnStopChannel || !isp->fnDisablePort ||
	    !isp->fnEnablePort || !isp->fnSetPortConfig) {
		i6c_isp_unload(isp);
		return -1;
	}

	return 0;
}

static void i6c_scl_unload(i6c_scl_impl *scl)
{
	if (!scl)
		return;
	if (scl->handle)
		dlclose(scl->handle);
	memset(scl, 0, sizeof(*scl));
}

static int i6c_scl_load(i6c_scl_impl *scl)
{
	if (!scl)
		return -1;
	memset(scl, 0, sizeof(*scl));
	scl->handle = dlopen("libmi_scl.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!scl->handle) {
		const char *err = dlerror();
		fprintf(stderr,
			"ERROR: [maruko] dlopen(libmi_scl.so) failed: %s\n",
			err ? err : "unknown");
		return -1;
	}

	scl->fnCreateDevice = (int (*)(int, unsigned int *))
		maruko_load_symbol(scl->handle, "libmi_scl.so",
			"MI_SCL_CreateDevice");
	scl->fnDestroyDevice = (int (*)(int))
		maruko_load_symbol(scl->handle, "libmi_scl.so",
			"MI_SCL_DestroyDevice");
	scl->fnAdjustChannelRotation = (int (*)(int, int, int *))
		maruko_load_symbol(scl->handle, "libmi_scl.so",
			"MI_SCL_SetChnParam");
	scl->fnCreateChannel = (int (*)(int, int, unsigned int *))
		maruko_load_symbol(scl->handle, "libmi_scl.so",
			"MI_SCL_CreateChannel");
	scl->fnDestroyChannel = (int (*)(int, int))
		maruko_load_symbol(scl->handle, "libmi_scl.so",
			"MI_SCL_DestroyChannel");
	scl->fnStartChannel = (int (*)(int, int))
		maruko_load_symbol(scl->handle, "libmi_scl.so",
			"MI_SCL_StartChannel");
	scl->fnStopChannel = (int (*)(int, int))
		maruko_load_symbol(scl->handle, "libmi_scl.so",
			"MI_SCL_StopChannel");
	scl->fnDisablePort = (int (*)(int, int, int))
		maruko_load_symbol(scl->handle, "libmi_scl.so",
			"MI_SCL_DisableOutputPort");
	scl->fnEnablePort = (int (*)(int, int, int))
		maruko_load_symbol(scl->handle, "libmi_scl.so",
			"MI_SCL_EnableOutputPort");
	scl->fnSetPortConfig = (int (*)(int, int, int, i6c_scl_port *))
		maruko_load_symbol(scl->handle, "libmi_scl.so",
			"MI_SCL_SetOutputPortParam");

	if (!scl->fnCreateDevice || !scl->fnDestroyDevice ||
	    !scl->fnAdjustChannelRotation || !scl->fnCreateChannel ||
	    !scl->fnDestroyChannel || !scl->fnStartChannel ||
	    !scl->fnStopChannel || !scl->fnDisablePort ||
	    !scl->fnEnablePort || !scl->fnSetPortConfig) {
		i6c_scl_unload(scl);
		return -1;
	}

	return 0;
}

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

	if (!g_maruko_isp_loaded) {
		if (i6c_isp_load(&g_maruko_isp) != 0) {
			fprintf(stderr,
				"ERROR: [maruko] failed to load i6c ISP symbols\n");
			return -1;
		}
		g_maruko_isp_loaded = 1;
	}

	if (!g_maruko_isp_dev_created) {
		unsigned int sensor_mask = (1u << (unsigned int)sensor->pad_id);
		ret = g_maruko_isp.fnCreateDevice(0, &sensor_mask);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] MI_ISP_CreateDevice failed %d\n", ret);
			goto fail;
		}
		g_maruko_isp_dev_created = 1;
	}
	dev = 1;

	if (!g_maruko_isp_chn_created) {
		i6c_isp_chn isp_chn = {0};
		isp_chn.sensorId = (unsigned int)sensor->pad_id;
		ret = g_maruko_isp.fnCreateChannel(0, 0, &isp_chn);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] MI_ISP_CreateChannel failed %d\n",
				ret);
			goto fail;
		}
		g_maruko_isp_chn_created = 1;
	}
	chn = 1;

	i6c_isp_para isp_para = {0};
	isp_para.hdr = I6_HDR_OFF;
	isp_para.level3DNR = vpe_level_3dnr;
	ret = g_maruko_isp.fnSetChannelParam(0, 0, &isp_para);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_ISP_SetChnParam failed %d\n", ret);
		goto fail;
	}

	ret = g_maruko_isp.fnStartChannel(0, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_ISP_StartChannel failed %d\n",
			ret);
		goto fail;
	}
	started = 1;

	i6c_isp_port isp_port = {0};
	isp_port.crop = sensor->plane.capt;
	isp_port.pixFmt = I6_PIXFMT_YUV420SP;
	isp_port.compress = I6_COMPR_NONE;
	printf("> [maruko] ISP port: crop(%u,%u %ux%u) fmt=%d compress=%d\n",
		isp_port.crop.x, isp_port.crop.y,
		isp_port.crop.width, isp_port.crop.height,
		isp_port.pixFmt, isp_port.compress);
	ret = g_maruko_isp.fnSetPortConfig(0, 0, 0, &isp_port);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_ISP_SetOutputPortParam failed %d\n",
			ret);
		goto fail;
	}

	ret = g_maruko_isp.fnEnablePort(0, 0, 0);
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
		(void)g_maruko_isp.fnDisablePort(0, 0, 0);
	if (started)
		(void)g_maruko_isp.fnStopChannel(0, 0);
	if (chn)
		(void)g_maruko_isp.fnDestroyChannel(0, 0);
	if (dev)
		(void)g_maruko_isp.fnDestroyDevice(0);
	return ret ? ret : -1;
}

static int configure_maruko_scl(const SensorSelectResult *sensor,
	uint32_t out_width, uint32_t out_height)
{
	MI_S32 ret = 0;
	int dev = 0, chn = 0, started = 0, port = 0;

	if (!g_maruko_scl_loaded) {
		if (i6c_scl_load(&g_maruko_scl) != 0) {
			fprintf(stderr,
				"ERROR: [maruko] failed to load i6c SCL symbols\n");
			return -1;
		}
		g_maruko_scl_loaded = 1;
	}

	if (!g_maruko_scl_dev_created) {
		unsigned int scl_bind = 0x3;
		ret = g_maruko_scl.fnCreateDevice(0, &scl_bind);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] MI_SCL_CreateDevice failed %d\n",
				ret);
			goto fail;
		}
		g_maruko_scl_dev_created = 1;
	}
	dev = 1;

	if (!g_maruko_scl_chn_created) {
		unsigned int scl_reserved = 0;
		ret = g_maruko_scl.fnCreateChannel(0, 0, &scl_reserved);
		if (ret != 0) {
			fprintf(stderr,
				"ERROR: [maruko] MI_SCL_CreateChannel failed %d\n",
				ret);
			goto fail;
		}
		g_maruko_scl_chn_created = 1;
	}
	chn = 1;

	int rotation = 0;
	ret = g_maruko_scl.fnAdjustChannelRotation(0, 0, &rotation);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SCL_SetChnParam failed %d\n", ret);
		goto fail;
	}

	ret = g_maruko_scl.fnStartChannel(0, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SCL_StartChannel failed %d\n",
			ret);
		goto fail;
	}
	started = 1;

	PipelinePrecropRect precrop = pipeline_common_compute_precrop(
		sensor->plane.capt.width, sensor->plane.capt.height,
		out_width, out_height);

	i6c_scl_port scl_port = {0};
	scl_port.crop = sensor->plane.capt;
	scl_port.crop.x += precrop.x;
	scl_port.crop.y += precrop.y;
	scl_port.crop.width = precrop.w;
	scl_port.crop.height = precrop.h;
	scl_port.output.width = (unsigned short)out_width;
	scl_port.output.height = (unsigned short)out_height;
	scl_port.pixFmt = I6_PIXFMT_YUV420SP;
	/* Maruko VENC ring input expects IFC compress mode (enum value 6). */
	scl_port.compress = (i6_common_compr)6;
	printf("> [maruko] SCL port: crop(%u,%u %ux%u) out(%ux%u) "
		"fmt=%d compress=%d\n",
		scl_port.crop.x, scl_port.crop.y,
		scl_port.crop.width, scl_port.crop.height,
		scl_port.output.width, scl_port.output.height,
		scl_port.pixFmt, scl_port.compress);
	ret = g_maruko_scl.fnSetPortConfig(0, 0, 0, &scl_port);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SCL_SetOutputPortParam failed %d\n",
			ret);
		goto fail;
	}

	ret = g_maruko_scl.fnEnablePort(0, 0, 0);
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SCL_EnableOutputPort failed %d\n",
			ret);
		goto fail;
	}
	port = 1;

	return 0;

fail:
	if (port)
		(void)g_maruko_scl.fnDisablePort(0, 0, 0);
	if (started)
		(void)g_maruko_scl.fnStopChannel(0, 0);
	if (chn)
		(void)g_maruko_scl.fnDestroyChannel(0, 0);
	if (dev)
		(void)g_maruko_scl.fnDestroyDevice(0);
	return ret ? ret : -1;
}

static int maruko_start_vpe(const SensorSelectResult *sensor,
	uint32_t out_width, uint32_t out_height, int vpe_level_3dnr)
{
	int isp_started = 0;

	if (configure_maruko_isp(sensor, vpe_level_3dnr) != 0)
		return -1;
	isp_started = 1;

	if (configure_maruko_scl(sensor, out_width, out_height) != 0)
		goto fail_scl;

	return 0;

fail_scl:
	if (isp_started) {
		(void)g_maruko_isp.fnDisablePort(0, 0, 0);
		(void)g_maruko_isp.fnStopChannel(0, 0);
		(void)g_maruko_isp.fnDestroyChannel(0, 0);
		(void)g_maruko_isp.fnDestroyDevice(0);
		i6c_isp_unload(&g_maruko_isp);
		g_maruko_isp_loaded = 0;
	}
	return -1;
}

/* Stop VPE channels only — keep devices and dlopen handles alive.
 * Used during reinit to avoid kernel mutex destruction. */
/* Stop VPE channels only — skip ISP DestroyChannel which crashes
 * with "Mutex not initialized" when CUS3A state persists in kernel. */
static void maruko_stop_vpe_channels(void)
{
	if (g_maruko_scl_loaded) {
		(void)g_maruko_scl.fnDisablePort(0, 0, 0);
		(void)g_maruko_scl.fnStopChannel(0, 0);
		(void)g_maruko_scl.fnDestroyChannel(0, 0);
		g_maruko_scl_chn_created = 0;
	}
	if (g_maruko_isp_loaded) {
		(void)g_maruko_isp.fnDisablePort(0, 0, 0);
		(void)g_maruko_isp.fnStopChannel(0, 0);
		/* Skip DestroyChannel — kernel ISP retains CUS3A mutex
		 * state that crashes on destroy+recreate cycle. */
	}
}

/* Full VPE stop — destroy devices and unload libs.
 * Used during final shutdown only. */
static void maruko_stop_vpe(void)
{
	maruko_stop_vpe_channels();
	g_maruko_isp_chn_created = 0;
	g_maruko_scl_chn_created = 0;
	if (g_maruko_scl_dev_created) {
		(void)g_maruko_scl.fnDestroyDevice(0);
		g_maruko_scl_dev_created = 0;
	}
	if (g_maruko_scl_loaded) {
		i6c_scl_unload(&g_maruko_scl);
		g_maruko_scl_loaded = 0;
	}
	if (g_maruko_isp_dev_created) {
		(void)g_maruko_isp.fnDestroyDevice(0);
		g_maruko_isp_dev_created = 0;
	}
	if (g_maruko_isp_loaded) {
		i6c_isp_unload(&g_maruko_isp);
		g_maruko_isp_loaded = 0;
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
	if (cfg->rc_codec == PT_H265) {
		attr->rate.mode = MARUKO_VENC_RC_H265_CBR;
		attr->rate.h265Cbr = (i6c_venc_rate_h26xcbr){
			.gop = gop, .statTime = 1, .fpsNum = framerate,
			.fpsDen = 1, .bitrate = bit_rate_bits, .avgLvl = 1,
		};
	} else {
		attr->rate.mode = MARUKO_VENC_RC_H264_CBR;
		attr->rate.h264Cbr = (i6c_venc_rate_h26xcbr){
			.gop = gop, .statTime = 1, .fpsNum = framerate,
			.fpsDen = 1, .bitrate = bit_rate_bits, .avgLvl = 1,
		};
	}
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

	/* Sample-venc behavior: configure VENC private ring pool
	 * before StartRecvPic. */
	MI_U16 venc_ring = (MI_U16)height;
	if (venc_ring == 0)
		venc_ring = 1;
	MI_S32 pool_ret = maruko_config_dev_ring_pool(I6C_SYS_MOD_VENC,
		(MI_U32)venc_dev, (MI_U16)width, (MI_U16)height, venc_ring);
	printf("> [maruko] VENC ring pool: %ux%u ring=%u ret=%d\n",
		width, height, venc_ring, pool_ret);

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

	return 0;
}

static void maruko_stop_venc(MI_VENC_DEV venc_dev, MI_VENC_CHN chn,
	int destroy_dev)
{
	(void)maruko_mi_venc_stop_recv(venc_dev, chn);
	(void)maruko_mi_venc_destroy_chn(venc_dev, chn);
	if (destroy_dev)
		(void)maruko_mi_venc_destroy_dev(venc_dev);
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
	/* ISP clock: 384 MHz (required for >= 1080p) */
	maruko_sysfs_write("/sys/devices/virtual/mstar/isp0/isp_clk",
		"384000000");

	/* SCL clock: index 1 = 533 MHz (max available).
	 * This sysfs node takes an index, not a frequency. */
	maruko_sysfs_write("/sys/devices/virtual/mstar/mscl/clk", "1");

	if (verbose) {
		printf("> [maruko] ISP clock -> 384 MHz\n");
		printf("> [maruko] SCL clock -> 533 MHz (idx 1)\n");
	}

	if (oc_level >= 1) {
		/* VENC secondary + AXI clocks are at 320 MHz by default;
		 * primary is already 384 MHz. No sysfs write interface
		 * discovered yet — revisit when VENC becomes bottleneck. */
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
	MI_S32 ret = MI_SYS_Init();
	if (ret != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_SYS_Init failed %d\n", ret);
		return ret;
	}
	ctx->system_initialized = 1;
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
	 * hangs (e.g. imx415 mode 1). Clamp to crop. */
	if (ctx->sensor.mode.crop.width > 0 &&
	    ctx->sensor.plane.capt.width > ctx->sensor.mode.crop.width) {
		fprintf(stderr, "WARNING: [maruko] sensor overscan detected: "
			"capture %ux%u > crop %ux%u — clamping to crop\n",
			ctx->sensor.plane.capt.width, ctx->sensor.plane.capt.height,
			ctx->sensor.mode.crop.width, ctx->sensor.mode.crop.height);
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

	/* Sample-venc behavior: configure SCL private ring pool
	 * before SCL device creation. */
	MI_U16 scl_ring = (MI_U16)(eff_h / 4);
	if (scl_ring == 0)
		scl_ring = 1;
	MI_S32 pool_ret = maruko_config_dev_ring_pool(I6C_SYS_MOD_SCL, 0,
		(MI_U16)eff_w, (MI_U16)eff_h, scl_ring);
	printf("> [maruko] SCL ring pool: %ux%u ring=%u ret=%d\n",
		eff_w, eff_h, scl_ring, pool_ret);
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
#if !defined(PLATFORM_MARUKO)
	if (MI_VENC_GetChnDevid(ctx->venc_channel, &venc_device) != 0) {
		fprintf(stderr,
			"ERROR: [maruko] MI_VENC_GetChnDevid failed\n");
		return -1;
	}
#endif

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

	/* ISP bin load and CUS3A enable must only run once per process
	 * lifetime.  On reinit, kernel ISP driver retains CUS3A state;
	 * re-running the 100→110→111 sequence causes a mutex deadlock. */
	if (!g_maruko_isp_initialized) {
		if (ctx->cfg.isp_bin_path && *ctx->cfg.isp_bin_path) {
			ret = maruko_load_isp_bin(ctx->cfg.isp_bin_path);
			if (ret != 0)
				return -1;
		}
		maruko_enable_cus3a();
		g_maruko_isp_initialized = 1;
	}
	/* Exposure cap is safe to reapply — FPS may have changed. */
	pipeline_common_cap_exposure_for_fps(ctx->sensor.fps,
		ctx->cfg.exposure_cap_us);

	if (ctx->cfg.output_uri.type == VENC_OUTPUT_URI_SHM) {
		if (ctx->cfg.stream_mode != MARUKO_STREAM_RTP) {
			fprintf(stderr, "ERROR: [maruko] shm:// requires RTP mode\n");
			return -1;
		}
		if (maruko_output_init_shm(&ctx->output, ctx->cfg.output_uri.endpoint,
		    ctx->cfg.rtp_payload_size) != 0)
			return -1;
	} else {
		if (maruko_output_init(&ctx->output, &ctx->cfg.output_uri) != 0)
			return -1;
	}

	return 0;
}

int maruko_pipeline_configure_graph(MarukoBackendContext *ctx)
{
	maruko_set_hw_clocks(ctx->cfg.oc_level, ctx->cfg.verbose);

	if (setup_maruko_graph_dimensions(ctx) != 0)
		return -1;

	uint32_t out_w = ctx->cfg.image_width;
	uint32_t out_h = ctx->cfg.image_height;

	if (maruko_start_vif(&ctx->sensor) != 0)
		return -1;
	ctx->vif_started = 1;

	if (maruko_start_vpe(&ctx->sensor, out_w, out_h,
	    ctx->cfg.vpe_level_3dnr) != 0)
		return -1;
	ctx->vpe_started = 1;

	if (bind_maruko_pipeline(ctx) != 0)
		return -1;

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

int maruko_pipeline_run(MarukoBackendContext *ctx)
{
	if (!ctx || (ctx->output.socket_handle < 0 && !ctx->output.ring))
		return -1;

	if (ctx->cfg.stream_mode == MARUKO_STREAM_RTP)
		printf("> [maruko] RTP packetizer enabled\n");
	printf("> [maruko] entering stream loop\n");

	MarukoRtpState rtp_state = {0};
	H26xParamSets param_sets = {0};
	RtpSidecarSender sidecar = {0};
	if (ctx->cfg.stream_mode == MARUKO_STREAM_RTP) {
		maruko_video_init_rtp_state(&rtp_state, ctx->cfg.rc_codec,
			ctx->sensor.fps);
		rtp_sidecar_sender_init(&sidecar, ctx->cfg.sidecar_port);
	}

	unsigned int frame_counter = 0;
	unsigned int idle_counter = 0;
	StreamMetricsState metrics;
	if (ctx->cfg.verbose) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		stream_metrics_start(&metrics, &now);
	}
	while (g_maruko_running) {
		if (g_maruko_reinit || venc_api_get_reinit()) {
			g_maruko_reinit = 0;
			printf("> [maruko] reinit requested, breaking stream loop\n");
			rtp_sidecar_sender_close(&sidecar);
			return 1;
		}

		i6c_venc_stat stat = {0};
		MI_S32 ret = maruko_mi_venc_query(ctx->venc_device,
			ctx->venc_channel, &stat);
		if (ret != 0) {
			if (ret == -EAGAIN || ret == EAGAIN) {
				idle_wait(&sidecar, 2);
				continue;
			}
			fprintf(stderr,
				"ERROR: [maruko] MI_VENC_Query failed %d\n",
				ret);
			return -1;
		}

		if (stat.curPacks == 0) {
			idle_counter++;
			if ((idle_counter % 500) == 0)
				printf("> [maruko] waiting for encoder data...\n");
			if (idle_counter > 3000) {
				fprintf(stderr,
					"ERROR: [maruko] no encoder data"
					" received; aborting stream loop\n");
				return -1;
			}
			idle_wait(&sidecar, 1);
			continue;
		}
		idle_counter = 0;

		i6c_venc_strm stream = {0};
		stream.count = stat.curPacks;
		stream.packet = calloc(stat.curPacks, sizeof(i6c_venc_pack));
		if (!stream.packet) {
			fprintf(stderr,
				"ERROR: [maruko] packet alloc failed\n");
			return -1;
		}

		ret = maruko_mi_venc_get_stream(ctx->venc_device,
			ctx->venc_channel, &stream, 40);
		if (ret != 0) {
			free(stream.packet);
			stream.packet = NULL;
			if (ret == -EAGAIN || ret == EAGAIN) {
				idle_wait(&sidecar, 2);
				continue;
			}
			fprintf(stderr,
				"ERROR: [maruko] MI_VENC_GetStream failed %d\n",
				ret);
			return -1;
		}

		++frame_counter;

		rtp_sidecar_poll(&sidecar);

		uint32_t frame_rtp_ts = rtp_state.timestamp;
		uint16_t seq_before = rtp_state.seq;
		uint64_t ready_us = monotonic_us();
		uint64_t capture_us = (stream.count > 0 && stream.packet)
			? stream.packet[0].timestamp : 0;

		size_t total_bytes = 0;
		if (ctx->output_enabled) {
			total_bytes = maruko_video_send_frame(&stream,
				&ctx->output, &rtp_state, &param_sets,
				&ctx->cfg);
		}

			rtp_sidecar_send_frame(&sidecar, rtp_state.ssrc, frame_rtp_ts,
				seq_before,
				(uint16_t)(rtp_state.seq - seq_before),
				capture_us, ready_us, NULL);

		if (ctx->cfg.verbose) {
			StreamMetricsSample sample;
			struct timespec verbose_ts_now;

			stream_metrics_record_frame(&metrics, total_bytes);
			clock_gettime(CLOCK_MONOTONIC, &verbose_ts_now);
			if (stream_metrics_sample(&metrics, &verbose_ts_now,
			    &sample)) {
				printf("[verbose] %lds | %u fps | %u kbps"
					" | frame %u | avg %u B/frame"
					" | %u packs\n",
					sample.uptime_s, sample.fps,
					sample.kbps, frame_counter,
					sample.avg_bytes, stream.count);
				fflush(stdout);
			}
		}

		(void)maruko_mi_venc_release_stream(ctx->venc_device,
			ctx->venc_channel, &stream);
		free(stream.packet);
		stream.packet = NULL;
	}

	rtp_sidecar_sender_close(&sidecar);
	return 0;
}

void maruko_pipeline_teardown_graph(MarukoBackendContext *ctx)
{
	if (!ctx)
		return;

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
	venc_httpd_stop();
	maruko_pipeline_teardown_graph(ctx);
	if (ctx && ctx->system_initialized) {
		(void)MI_SYS_Exit();
		ctx->system_initialized = 0;
		printf("> [maruko] stage teardown: MI_SYS_Exit\n");
	}
}
