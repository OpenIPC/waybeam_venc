/*
 * CV610 pipeline ported from the hardware-verified standalone bring-up in
 * snokvist/hisilicon.  Keep platform state confined to this translation unit;
 * the public lifecycle is the small cv610_pipeline_* interface below.
 */
#include "cv610_pipeline.h"
/* Minimal Hi3516CV610 + IMX662 MIPI/VI/ISP graph. The target's MPP module
 * loader and sensor-clock prerequisites are documented in
 * docs/CV610_BACKEND.md. VENC and output ownership stay in cv610_runtime.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>

#include "ot_type.h"
#include "ot_common.h"
#include "ot_common_video.h"
#include "ot_common_vb.h"
#include "ot_common_sys.h"
#include "ot_common_vi.h"
#include "ot_common_isp.h"
#include "ot_common_3a.h"
#include "ot_mipi_rx.h"
#include "ot_sns_ctrl.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_sys_mem.h"
#include "ss_mpi_vb.h"
#include "ss_mpi_vi.h"
#include "ss_mpi_isp.h"
#include "ss_mpi_ae.h"
#include "ss_mpi_awb.h"

#define MIPI_DEV_NODE       "/dev/ot_mipi_rx"
#define SNS_LIB_PATH        "/usr/lib/sensors/libsns_imx662.so"
#define SNS_OBJ_SYMBOL      "g_sns_imx662_obj"
/* Must match IMX662_ID in the sensor driver (imx662_cmos_param.h). */
#define IMX662_SNS_ID       662

#define VI_DEV              0
#define VI_PIPE             0
#define VI_CHN              0

#define CV610_CHECK(expr) do { \
		td_s32 check_ret = (expr); \
		if (check_ret != TD_SUCCESS) { \
			fprintf(stderr, "FAIL %s = 0x%x\n", #expr, check_ret); \
			return -1; \
		} \
		printf("  ok  %s\n", #expr); \
	} while (0)

/* Same, but tolerate "already initialized" (BUSY). */
static td_s32 cv610_error_id(td_s32 ret)
{
	return ret & 0x1fff;
}

#define CV610_CHECK_OR_BUSY(expr) do { \
		td_s32 check_ret = (expr); \
		if (check_ret != TD_SUCCESS && \
			cv610_error_id(check_ret) != OT_ERR_BUSY) { \
			fprintf(stderr, "FAIL %s = 0x%x\n", #expr, check_ret); \
			return -1; \
		} \
		printf("  ok  %s%s\n", #expr, \
			(check_ret == TD_SUCCESS) ? "" : "  (already up)"); \
	} while (0)

typedef struct {
	unsigned int width;
	unsigned int height;
	float        fps;
	int          lanes;        /* 1..4 */
	int          data_rate_x2; /* MIPI_DATA_RATE_X1 or X2 */
	int          bayer;        /* ot_isp_bayer_format 0..3 */
	int          raw_bit;      /* 10 or 12 */
	int          vi_online;    /* VI -> ISP online/realtime mode */
} Cv610PipelineRuntimeConfig;

static volatile sig_atomic_t g_stop;
static pthread_t    g_isp_thread;
static int          g_isp_thread_ok;

static void *isp_thread_fn(void *arg)
{
	(void)arg;
	/* ss_mpi_isp_run() does not return until ss_mpi_isp_exit(). */
	td_s32 ret = ss_mpi_isp_run(VI_PIPE);
	printf("[isp] ss_mpi_isp_run returned 0x%x\n", ret);
	return NULL;
}

/* ------------------------------------------------------------------ MIPI -- */

static int mipi_setup(const Cv610PipelineRuntimeConfig *c)
{
	combo_dev_attr_t attr;
	combo_dev_t      dev = 0;
	sns_clk_source_t clk = 0;
	sns_rst_source_t rst = 0;
	lane_divide_mode_t hs = LANE_DIVIDE_MODE_0;
	int fd, i;

	memset(&attr, 0, sizeof(attr));
	attr.devno          = dev;
	attr.input_mode     = INPUT_MODE_MIPI;
	attr.data_rate      = c->data_rate_x2 ? MIPI_DATA_RATE_X2 : MIPI_DATA_RATE_X1;
	attr.img_rect.x     = 0;
	attr.img_rect.y     = 0;
	attr.img_rect.width  = c->width;
	attr.img_rect.height = c->height;
	attr.mipi_attr.input_data_type =
		(c->raw_bit == 10) ? DATA_TYPE_RAW_10BIT : DATA_TYPE_RAW_12BIT;
	attr.mipi_attr.wdr_mode = OT_MIPI_WDR_MODE_NONE;
	for (i = 0; i < MIPI_LANE_NUM; i++) {
		attr.mipi_attr.lane_id[i] = (i < c->lanes) ? (short)i : (short)-1;
	}

	fd = open(MIPI_DEV_NODE, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", MIPI_DEV_NODE, strerror(errno));
		return -1;
	}

#define mipi_ioctl(req, arg) do { \
		if (ioctl(fd, (req), (arg)) < 0) { \
			fprintf(stderr, "FAIL mipi ioctl %s: %s\n", #req, strerror(errno)); \
			close(fd); \
			return -1; \
		} \
	} while (0)

	mipi_ioctl(OT_MIPI_SET_HS_MODE, &hs);
	mipi_ioctl(OT_MIPI_DISABLE_MIPI_CLOCK, &dev);
	mipi_ioctl(OT_MIPI_RESET_MIPI, &dev);
	mipi_ioctl(OT_MIPI_DISABLE_SENSOR_CLOCK, &clk);
	mipi_ioctl(OT_MIPI_RESET_SENSOR, &rst);

	mipi_ioctl(OT_MIPI_SET_DEV_ATTR, &attr);

	mipi_ioctl(OT_MIPI_ENABLE_MIPI_CLOCK, &dev);
	mipi_ioctl(OT_MIPI_UNRESET_MIPI, &dev);
	mipi_ioctl(OT_MIPI_ENABLE_SENSOR_CLOCK, &clk);
	mipi_ioctl(OT_MIPI_UNRESET_SENSOR, &rst);
#undef mipi_ioctl

	close(fd);
	printf("  ok  mipi: %ux%u raw%d, %d lane(s), data_rate x%d\n",
		   c->width, c->height, c->raw_bit, c->lanes, c->data_rate_x2 ? 2 : 1);
	return 0;
}

/* --------------------------------------------------------------- VB / SYS -- */

static int sys_setup(const Cv610PipelineRuntimeConfig *c)
{
	ot_vb_cfg vb;
	ot_vi_vpss_mode vi_vpss_mode;
	td_u64 raw_blk, yuv_blk;
	td_s32 sys_ret, vb_ret;
	unsigned int i;

	/* RAW12 is stored 16bpp in the pipe buffers; be generous, this is a probe. */
	raw_blk = (td_u64)c->width * c->height * 2 + 0x4000;
	yuv_blk = (td_u64)c->width * c->height * 3 / 2 + 0x4000;

	memset(&vb, 0, sizeof(vb));
	vb.max_pool_cnt = 2;
	vb.common_pool[0].blk_size = raw_blk;
	vb.common_pool[0].blk_cnt  = 4;
	vb.common_pool[1].blk_size = yuv_blk;
	vb.common_pool[1].blk_cnt  = 4;

	/* VB/SYS state lives in the kernel and survives the process that created
	 * it, and only the creating process may tear it down — so after a crashed
	 * run vb_exit returns NOT_PERM and the config below cannot be replaced.
	 * BUSY is therefore acceptable for the normal offline restart path. For
	 * online mode it is not: selecting VI-online must happen after a complete,
	 * successful SYS init and before any VI pipe exists. */
	/* C does not define function-argument evaluation order. Keep the vendor
	 * API's SYS-then-VB shutdown order explicit and make each result legible. */
	sys_ret = ss_mpi_sys_exit();
	vb_ret = ss_mpi_vb_exit();
	printf("  pre-clean: sys_exit=0x%x vb_exit=0x%x\n", sys_ret, vb_ret);

	memset(&vi_vpss_mode, 0, sizeof(vi_vpss_mode));
	for (i = 0; i < OT_VI_MAX_PIPE_NUM; i++) {
		vi_vpss_mode.mode[i] = OT_VI_OFFLINE_VPSS_OFFLINE;
	}
	if (c->vi_online) {
		vi_vpss_mode.mode[VI_PIPE] = OT_VI_ONLINE_VPSS_OFFLINE;
	}

	CV610_CHECK_OR_BUSY(ss_mpi_vb_set_cfg(&vb));
	CV610_CHECK_OR_BUSY(ss_mpi_vb_init());
	if (c->vi_online) {
		/* Online mode can only be selected after a complete SYS init. Do not
		 * hide a half-initialized module set behind the usual BUSY tolerance. */
		CV610_CHECK(ss_mpi_sys_init());
		CV610_CHECK(ss_mpi_sys_set_vi_vpss_mode(&vi_vpss_mode));
		CV610_CHECK(ss_mpi_sys_set_vi_aiisp_mode(VI_PIPE, OT_VI_AIISP_MODE_DEFAULT));
		memset(&vi_vpss_mode, 0, sizeof(vi_vpss_mode));
		CV610_CHECK(ss_mpi_sys_get_vi_vpss_mode(&vi_vpss_mode));
		if (vi_vpss_mode.mode[VI_PIPE] != OT_VI_ONLINE_VPSS_OFFLINE) {
			fprintf(stderr, "FAIL VI pipe %d mode readback = %d\n", VI_PIPE,
					vi_vpss_mode.mode[VI_PIPE]);
			return -1;
		}
	} else {
		CV610_CHECK_OR_BUSY(ss_mpi_sys_init());
	}
	printf("  ok  VI/ISP mode: %s\n", c->vi_online ? "online" : "offline");
	return 0;
}

/* --------------------------------------------------------------- VI setup -- */

static int vi_setup(const Cv610PipelineRuntimeConfig *c)
{
	ot_vi_dev_attr  dev_attr;
	ot_vi_pipe_attr pipe_attr;

	/* Same kernel-side-state problem as VB: a previous run that died leaves
	 * the VI device enabled, and set_dev_attr then fails NOT_DISABLE. */
	ss_mpi_vi_disable_chn(VI_PIPE, VI_CHN);
	ss_mpi_vi_stop_pipe(VI_PIPE);
	ss_mpi_vi_destroy_pipe(VI_PIPE);
	ss_mpi_vi_unbind(VI_DEV, VI_PIPE);
	ss_mpi_vi_disable_dev(VI_DEV);

	memset(&dev_attr, 0, sizeof(dev_attr));
	dev_attr.intf_mode          = OT_VI_INTF_MODE_MIPI;
	dev_attr.work_mode          = OT_VI_WORK_MODE_MULTIPLEX_1;
	dev_attr.component_mask[0]  = 0xFFC00000;
	dev_attr.component_mask[1]  = 0x0;
	dev_attr.scan_mode          = OT_VI_SCAN_PROGRESSIVE;
	dev_attr.ad_chn_id[0]       = -1;
	dev_attr.ad_chn_id[1]       = -1;
	dev_attr.ad_chn_id[2]       = -1;
	dev_attr.ad_chn_id[3]       = -1;
	dev_attr.data_seq           = OT_VI_DATA_SEQ_YUYV;
	dev_attr.data_type          = OT_VI_DATA_TYPE_RAW;
	dev_attr.data_reverse       = TD_FALSE;
	dev_attr.in_size.width      = c->width;
	dev_attr.in_size.height     = c->height;
	dev_attr.data_rate          = c->data_rate_x2 ? OT_DATA_RATE_X2 : OT_DATA_RATE_X1;

	CV610_CHECK(ss_mpi_vi_set_dev_attr(VI_DEV, &dev_attr));
	CV610_CHECK(ss_mpi_vi_enable_dev(VI_DEV));
	CV610_CHECK(ss_mpi_vi_bind(VI_DEV, VI_PIPE));

	memset(&pipe_attr, 0, sizeof(pipe_attr));
	pipe_attr.pipe_bypass_mode = OT_VI_PIPE_BYPASS_NONE;
	pipe_attr.isp_bypass       = TD_FALSE;
	pipe_attr.size.width       = c->width;
	pipe_attr.size.height      = c->height;
	pipe_attr.pixel_format     = (c->raw_bit == 10) ?
								 OT_PIXEL_FORMAT_RGB_BAYER_10BPP :
								 OT_PIXEL_FORMAT_RGB_BAYER_12BPP;
	pipe_attr.compress_mode    = OT_COMPRESS_MODE_NONE;
	pipe_attr.frame_rate_ctrl.src_frame_rate = OT_VI_INVALID_FRAME_RATE;
	pipe_attr.frame_rate_ctrl.dst_frame_rate = OT_VI_INVALID_FRAME_RATE;

	CV610_CHECK(ss_mpi_vi_create_pipe(VI_PIPE, &pipe_attr));
	/* The ISP's mem_init queries pipe size from VI, so the pipe must exist. */
	CV610_CHECK(ss_mpi_vi_start_pipe(VI_PIPE));
	return 0;
}

static int vi_start_chn(const Cv610PipelineRuntimeConfig *c)
{
	ot_vi_chn_attr chn_attr;

	memset(&chn_attr, 0, sizeof(chn_attr));
	chn_attr.size.width    = c->width;
	chn_attr.size.height   = c->height;
	/* CV610 VI accepts only YVU (NV21) semiplanar: set_chn_attr rejects
	 * YUV_SEMIPLANAR_420 with 0xa0108007, and open_vi.ko's
	 * vi_check_yuv_frame_pixel_format passes only formats 37/38/52
	 * (verified on hardware 2026-07-30). NV12 exists only via VPSS. */
	chn_attr.pixel_format  = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
	chn_attr.dynamic_range = OT_DYNAMIC_RANGE_SDR8;
	chn_attr.video_format  = OT_VIDEO_FORMAT_LINEAR;
	chn_attr.compress_mode = OT_COMPRESS_MODE_NONE;
	chn_attr.mirror_en     = TD_FALSE;
	chn_attr.flip_en       = TD_FALSE;
	chn_attr.depth         = 0;
	chn_attr.frame_rate_ctrl.src_frame_rate = OT_VI_INVALID_FRAME_RATE;
	chn_attr.frame_rate_ctrl.dst_frame_rate = OT_VI_INVALID_FRAME_RATE;

	CV610_CHECK(ss_mpi_vi_set_chn_attr(VI_PIPE, VI_CHN, &chn_attr));
	CV610_CHECK(ss_mpi_vi_enable_chn(VI_PIPE, VI_CHN));

	/* isp_mem_init asks the VI kernel export vi_get_pipe_hdr_attr() for this
	 * pipe's dynamic range.  On CV610 that export reads physical channel 0's
	 * attributes, so the channel must be configured before isp_mem_init. */
	return 0;
}

/* ------------------------------------------------------------ sensor/ISP -- */

static void            *g_sns_handle;
static ot_isp_sns_obj  *g_sns_obj;

static ot_isp_3a_alg_lib g_ae_lib  = { .id = VI_PIPE, .lib_name = "ot_ae_lib"  };
static ot_isp_3a_alg_lib g_awb_lib = { .id = VI_PIPE, .lib_name = "ot_awb_lib" };

static int sensor_setup(int i2c_bus)
{
	ot_isp_sns_commbus bus;

	g_sns_handle = dlopen(SNS_LIB_PATH, RTLD_NOW | RTLD_GLOBAL);
	if (g_sns_handle == NULL) {
		fprintf(stderr, "dlopen %s: %s\n", SNS_LIB_PATH, dlerror());
		return -1;
	}
	g_sns_obj = (ot_isp_sns_obj *)dlsym(g_sns_handle, SNS_OBJ_SYMBOL);
	if (g_sns_obj == NULL) {
		fprintf(stderr, "dlsym %s: %s\n", SNS_OBJ_SYMBOL, dlerror());
		return -1;
	}
	printf("  ok  dlopen %s -> %s @ %p\n", SNS_LIB_PATH, SNS_OBJ_SYMBOL, (void *)g_sns_obj);

	if (g_sns_obj->pfn_set_bus_info != NULL) {
		bus.i2c_dev = (td_s8)i2c_bus;
		CV610_CHECK(g_sns_obj->pfn_set_bus_info(VI_PIPE, bus));
	}

	CV610_CHECK(ss_mpi_ae_register(VI_PIPE, &g_ae_lib));
	CV610_CHECK(ss_mpi_awb_register(VI_PIPE, &g_awb_lib));
	CV610_CHECK(g_sns_obj->pfn_register_callback(VI_PIPE, &g_ae_lib, &g_awb_lib));
	return 0;
}

static int isp_setup(const Cv610PipelineRuntimeConfig *c)
{
	ot_isp_pub_attr pub;
	ot_isp_bind_attr bind;

	memset(&pub, 0, sizeof(pub));
	pub.wnd_rect.x      = 0;
	pub.wnd_rect.y      = 0;
	pub.wnd_rect.width  = c->width;
	pub.wnd_rect.height = c->height;
	pub.sns_size.width  = c->width;
	pub.sns_size.height = c->height;
	pub.frame_rate      = c->fps;
	pub.bayer_format    = (ot_isp_bayer_format)c->bayer;
	pub.wdr_mode        = OT_WDR_MODE_NONE;
	pub.sns_mode        = 0;

	/* Tell the ISP which registered 3A libs and which sensor this pipe uses;
	 * without it isp_mem_init cannot resolve the sensor and fails NOT_CFG. */
	memset(&bind, 0, sizeof(bind));
	bind.sns_id  = IMX662_SNS_ID;
	bind.ae_lib  = g_ae_lib;
	bind.awb_lib = g_awb_lib;
	CV610_CHECK(ss_mpi_isp_set_bind_attr(VI_PIPE, &bind));

	CV610_CHECK(ss_mpi_isp_mem_init(VI_PIPE));
	CV610_CHECK(ss_mpi_isp_set_pub_attr(VI_PIPE, &pub));
	CV610_CHECK(ss_mpi_isp_init(VI_PIPE));
	return 0;
}

static int configure_output_color(void)
{
	ot_isp_csc_attr csc;
	td_s32 ret;

	memset(&csc, 0, sizeof(csc));
	ret = ss_mpi_isp_get_csc_attr(VI_PIPE, &csc);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "FAIL ss_mpi_isp_get_csc_attr = 0x%x\n", ret);
		return -1;
	}
	/* Preserve the standalone streamer's device-verified output.  The VENC
	 * VUI advertises full-range BT.709, so keep the ISP CSC in agreement. */
	csc.enable = TD_TRUE;
	csc.satu = 60;
	csc.contr = 53;
	ret = ss_mpi_isp_set_csc_attr(VI_PIPE, &csc);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "FAIL ss_mpi_isp_set_csc_attr = 0x%x\n", ret);
		return -1;
	}
	printf("  output CSC: gamut=%d saturation=%u contrast=%u full-range=%d\n",
		   csc.color_gamut, csc.satu, csc.contr, !csc.limited_range_en);
	return 0;
}

static int enable_sensor_ccm(void)
{
	ot_isp_color_matrix_attr attr;
	td_s32 ret;

	memset(&attr, 0, sizeof(attr));
	ret = ss_mpi_isp_get_ccm_attr(VI_PIPE, &attr);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "FAIL ss_mpi_isp_get_ccm_attr = 0x%x\n", ret);
		return -1;
	}
	if (attr.auto_attr.ccm_tab_num < 3) {
		fprintf(stderr, "FAIL sensor supplied only %u CCM anchors\n",
				attr.auto_attr.ccm_tab_num);
		return -1;
	}
	attr.op_type = OT_OP_MODE_AUTO;
	attr.auto_attr.iso_act_en = TD_FALSE;
	attr.auto_attr.temp_act_en = TD_FALSE;
	ret = ss_mpi_isp_set_ccm_attr(VI_PIPE, &attr);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "FAIL ss_mpi_isp_set_ccm_attr = 0x%x\n", ret);
		return -1;
	}
	printf("  sensor CCM enabled: %u anchors, ISO/temperature bypass off\n",
		   attr.auto_attr.ccm_tab_num);
	return 0;
}

/* --------------------------------------------------------------- teardown -- */

/* Safe after partial initialization; BackendOps calls this on init failure. */
static void mpp_cleanup(void)
{
	td_s32 sys_ret, vb_ret;

	printf("== teardown ==\n");
	/* Match the vendor sample's shutdown dependency order: stop ISP and its
	 * 3A/sensor users before dismantling the VI pipe that feeds it. */
	ss_mpi_isp_exit(VI_PIPE);
	if (g_isp_thread_ok) {
		pthread_join(g_isp_thread, NULL);
		g_isp_thread_ok = 0;
	}
	if (g_sns_obj != NULL && g_sns_obj->pfn_un_register_callback != NULL) {
		g_sns_obj->pfn_un_register_callback(VI_PIPE, &g_ae_lib, &g_awb_lib);
	}
	ss_mpi_awb_unregister(VI_PIPE, &g_awb_lib);
	ss_mpi_ae_unregister(VI_PIPE, &g_ae_lib);
	ss_mpi_vi_disable_chn(VI_PIPE, VI_CHN);
	ss_mpi_vi_stop_pipe(VI_PIPE);
	ss_mpi_vi_destroy_pipe(VI_PIPE);
	ss_mpi_vi_unbind(VI_DEV, VI_PIPE);
	ss_mpi_vi_disable_dev(VI_DEV);
	if (g_sns_handle != NULL) {
		dlclose(g_sns_handle);
		g_sns_handle = NULL;
		g_sns_obj = NULL;
	}
	sys_ret = ss_mpi_sys_exit();
	vb_ret = ss_mpi_vb_exit();
	printf("  sys_exit=0x%x vb_exit=0x%x\n", sys_ret, vb_ret);
}

int cv610_pipeline_start(const Cv610PipelineConfig *config)
{
	Cv610PipelineRuntimeConfig c;

	if (config == NULL) {
		return -1;
	}
	memset(&c, 0, sizeof(c));
	c.width = config->width;
	c.height = config->height;
	c.fps = (float)config->fps;
	c.lanes = config->lanes;
	c.data_rate_x2 = config->data_rate_x2;
	c.bayer = config->bayer;
	c.raw_bit = config->raw_bit;
	c.vi_online = config->vi_online;
	g_stop = 0;

	printf("== cv610 sys/vb ==\n");
	if (sys_setup(&c) != 0) {
		return -1;
	}
	printf("== cv610 mipi ==\n");
	if (mipi_setup(&c) != 0) {
		return -1;
	}
	printf("== cv610 vi ==\n");
	if (vi_setup(&c) != 0 || vi_start_chn(&c) != 0) {
		return -1;
	}
	printf("== cv610 sensor/isp ==\n");
	if (sensor_setup(config->i2c_bus) != 0 || isp_setup(&c) != 0 ||
		configure_output_color() != 0) {
		return -1;
	}
	if (pthread_create(&g_isp_thread, NULL, isp_thread_fn, NULL) != 0) {
		fprintf(stderr, "FAIL pthread_create ISP\n");
		return -1;
	}
	g_isp_thread_ok = 1;
	if (enable_sensor_ccm() != 0) {
		return -1;
	}
	return 0;
}

void cv610_pipeline_stop(void)
{
	g_stop = 1;
	mpp_cleanup();
}

void cv610_pipeline_request_stop(void)
{
	g_stop = 1;
}

int cv610_pipeline_stop_requested(void)
{
	return g_stop != 0;
}
