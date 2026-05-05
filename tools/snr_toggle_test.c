#include "star6e.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum {
  FPS_TIMING_PRE = 0,
  FPS_TIMING_POST = 1,
  FPS_TIMING_BOTH = 2,
} FpsTiming;

typedef enum {
  SETRES_PRE_ONLY = 0,
  SETRES_POST_ONLY = 1,
  SETRES_BOTH = 2,
} SetResTiming;

typedef enum {
  TOUCH_NONE = 0,
  TOUCH_PRE = 1,
  TOUCH_POST = 2,
} TouchTiming;

typedef enum {
  VIF_HOLD_NONE = 0,
  VIF_HOLD_PRE = 1,
  VIF_HOLD_POST = 2,
} VifHoldTiming;

typedef enum {
  ORIEN_NONE = 0,
  ORIEN_PRE = 1,
  ORIEN_POST = 2,
} OrienTiming;

typedef enum {
  FPS_FMT_RAW_ONLY = 0,
  FPS_FMT_RAW_THEN_MILLI = 1,
  FPS_FMT_MILLI_THEN_RAW = 2,
} FpsFormat;

typedef enum {
  PLANE_LINEAR = 0,
  PLANE_HDR = 1,
  PLANE_HDR_THEN_LINEAR = 2,
  PLANE_HDR_POST_LINEAR = 3,
} PlaneModeStrategy;

typedef struct {
  int init_snr_dev;
  FpsTiming fps_timing;
  SetResTiming setres_timing;
  int reenable_cycle;
  TouchTiming touch_vif;
  VifHoldTiming hold_vif;
  OrienTiming orien_timing;
  FpsFormat fps_format;
} ScenarioConfig;

typedef struct {
  MI_SNR_PAD_ID_e pad;
  MI_U32 mode_idx;
  MI_U32 req_fps;
  int init_dev_id;
  int init_data_bytes;
  int init_data_seed;
  int prime_isp;
  int full_matrix;
  int max_cases;
  int delay_ms;
  int verbose;
  int quiet;
  PlaneModeStrategy plane_mode;
  int sticky_state;
  int trace_state;
  int no_reset;
  int skip_fps;
  int cust_pre;
  MI_U32 cust_cmd;
  MI_U32 cust_size;
  MI_U32 cust_value;
  MI_S32 cust_dir;
  int filter_init_snr_dev;
  int filter_setres_timing;
  int filter_fps_timing;
  int filter_hold_vif;
} RunConfig;

typedef struct {
  MI_S32 plane_ret;
  MI_S32 setres_pre_ret;
  MI_S32 fps_pre_ret;
  MI_S32 enable_ret;
  MI_S32 setres_post_ret;
  MI_S32 fps_post_ret;
  MI_S32 reenable_ret;
  MI_S32 touch_vif_pre_ret;
  MI_S32 touch_vif_post_ret;
  MI_S32 hold_vif_pre_ret;
  MI_S32 hold_vif_post_ret;
  MI_S32 orien_pre_ret;
  MI_S32 orien_post_ret;
  MI_S32 final_mode_ret;
  MI_S32 final_fps_ret;
  MI_U8 final_mode_idx;
  MI_U32 final_fps_raw;
  MI_U32 final_fps;
  int fps_any_success;
  int mode_match;
} ScenarioResult;

typedef int (*isp_cus3a_enable_fn_t)(int channel, void* params);

typedef struct {
  int params[13];
} ISPUserspace3AParams;

static const char* fps_timing_name(FpsTiming t) {
  switch (t) {
    case FPS_TIMING_PRE: return "pre";
    case FPS_TIMING_POST: return "post";
    case FPS_TIMING_BOTH: return "both";
    default: return "?";
  }
}

static const char* setres_timing_name(SetResTiming t) {
  switch (t) {
    case SETRES_PRE_ONLY: return "pre";
    case SETRES_POST_ONLY: return "post";
    case SETRES_BOTH: return "both";
    default: return "?";
  }
}

static const char* touch_name(TouchTiming t) {
  switch (t) {
    case TOUCH_NONE: return "none";
    case TOUCH_PRE: return "pre";
    case TOUCH_POST: return "post";
    default: return "?";
  }
}

static const char* hold_vif_name(VifHoldTiming t) {
  switch (t) {
    case VIF_HOLD_NONE: return "none";
    case VIF_HOLD_PRE: return "pre";
    case VIF_HOLD_POST: return "post";
    default: return "?";
  }
}

static const char* orien_name(OrienTiming t) {
  switch (t) {
    case ORIEN_NONE: return "none";
    case ORIEN_PRE: return "pre";
    case ORIEN_POST: return "post";
    default: return "?";
  }
}

static const char* fps_fmt_name(FpsFormat f) {
  switch (f) {
    case FPS_FMT_RAW_ONLY: return "raw";
    case FPS_FMT_RAW_THEN_MILLI: return "raw->milli";
    case FPS_FMT_MILLI_THEN_RAW: return "milli->raw";
    default: return "?";
  }
}

static const char* plane_mode_name(PlaneModeStrategy p) {
  switch (p) {
    case PLANE_LINEAR: return "linear";
    case PLANE_HDR: return "hdr";
    case PLANE_HDR_THEN_LINEAR: return "hdr-linear";
    case PLANE_HDR_POST_LINEAR: return "hdr-post-linear";
    default: return "?";
  }
}

static int parse_fps_timing(const char* s, FpsTiming* out) {
  if (!s || !out) {
    return -1;
  }
  if (strcmp(s, "pre") == 0) {
    *out = FPS_TIMING_PRE;
    return 0;
  }
  if (strcmp(s, "post") == 0) {
    *out = FPS_TIMING_POST;
    return 0;
  }
  if (strcmp(s, "both") == 0) {
    *out = FPS_TIMING_BOTH;
    return 0;
  }
  return -1;
}

static int parse_setres_timing(const char* s, SetResTiming* out) {
  if (!s || !out) {
    return -1;
  }
  if (strcmp(s, "pre") == 0) {
    *out = SETRES_PRE_ONLY;
    return 0;
  }
  if (strcmp(s, "post") == 0) {
    *out = SETRES_POST_ONLY;
    return 0;
  }
  if (strcmp(s, "both") == 0) {
    *out = SETRES_BOTH;
    return 0;
  }
  return -1;
}

static int parse_hold_vif(const char* s, VifHoldTiming* out) {
  if (!s || !out) {
    return -1;
  }
  if (strcmp(s, "none") == 0) {
    *out = VIF_HOLD_NONE;
    return 0;
  }
  if (strcmp(s, "pre") == 0) {
    *out = VIF_HOLD_PRE;
    return 0;
  }
  if (strcmp(s, "post") == 0) {
    *out = VIF_HOLD_POST;
    return 0;
  }
  return -1;
}

static int parse_plane_mode(const char* s, PlaneModeStrategy* out) {
  if (!s || !out) {
    return -1;
  }
  if (strcmp(s, "linear") == 0) {
    *out = PLANE_LINEAR;
    return 0;
  }
  if (strcmp(s, "hdr") == 0) {
    *out = PLANE_HDR;
    return 0;
  }
  if (strcmp(s, "hdr-linear") == 0) {
    *out = PLANE_HDR_THEN_LINEAR;
    return 0;
  }
  if (strcmp(s, "hdr-post-linear") == 0) {
    *out = PLANE_HDR_POST_LINEAR;
    return 0;
  }
  return -1;
}

static MI_U32 normalize_fps(MI_U32 raw) {
  if (raw >= 1000 && (raw % 1000) == 0) {
    return raw / 1000;
  }
  return raw;
}

static void trace_sensor_state(const RunConfig* run, const char* tag, int include_modes) {
  if (!run->trace_state) {
    return;
  }

  MI_SNR_Res_t cur = {0};
  MI_SNR_PadInfo_t pad = {0};
  MI_U8 cur_idx = 0xFF;
  MI_BOOL plane_enable = false;
  MI_U32 fps_raw = 0;
  MI_S32 cur_ret = MI_SNR_GetCurRes(run->pad, &cur_idx, &cur);
  MI_S32 fps_ret = MI_SNR_GetFps(run->pad, &fps_raw);
  MI_S32 plane_ret = MI_SNR_GetPlaneMode(run->pad, &plane_enable);
  MI_S32 pad_ret = MI_SNR_GetPadInfo(run->pad, &pad);

  if (pad_ret == 0) {
    printf("    snapshot[%s]: cur_ret=%d idx=%u %ux%u fps_ret=%d fps=%u(raw=%u) "
           "plane_ret=%d plane=%d pad_ret=%d intf=%d hdr=%d planes=%u earlyInit=%d\n",
      tag,
      cur_ret, (unsigned)cur_idx, cur.crop.width, cur.crop.height,
      fps_ret, (unsigned)normalize_fps(fps_raw), (unsigned)fps_raw,
      plane_ret, plane_enable ? 1 : 0,
      pad_ret, pad.intf, pad.hdr, pad.planeCnt, pad.earlyInit);
  } else {
    printf("    snapshot[%s]: cur_ret=%d idx=%u %ux%u fps_ret=%d fps=%u(raw=%u) "
           "plane_ret=%d plane=%d pad_ret=%d\n",
      tag,
      cur_ret, (unsigned)cur_idx, cur.crop.width, cur.crop.height,
      fps_ret, (unsigned)normalize_fps(fps_raw), (unsigned)fps_raw,
      plane_ret, plane_enable ? 1 : 0,
      pad_ret);
  }

  if (!include_modes) {
    return;
  }

  MI_U32 res_count = 0;
  MI_S32 count_ret = MI_SNR_QueryResCount(run->pad, &res_count);
  if (count_ret != 0) {
    printf("      modes[%s]: MI_SNR_QueryResCount failed %d\n", tag, count_ret);
    return;
  }

  printf("      modes[%s]: count=%u\n", tag, (unsigned)res_count);
  for (MI_U32 i = 0; i < res_count; ++i) {
    MI_SNR_Res_t res = {0};
    MI_S32 ret = MI_SNR_GetRes(run->pad, i, &res);
    if (ret != 0) {
      printf("        - [%u] ret=%d\n", (unsigned)i, ret);
      continue;
    }
    printf("        - [%u] %ux%u min/max fps %u/%u desc \"%s\"\n",
      (unsigned)i,
      res.crop.width, res.crop.height,
      (unsigned)res.minFps, (unsigned)res.maxFps,
      res.desc);
  }
}

static void delay_if_needed(int delay_ms) {
  if (delay_ms > 0) {
    usleep((useconds_t)delay_ms * 1000);
  }
}

static int prime_isp_userspace_3a(void) {
  void* handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
  if (!handle) {
    printf("WARNING: Unable to load libmi_isp.so for priming (%s)\n", dlerror());
    return -1;
  }

  isp_cus3a_enable_fn_t fn_cus3a_enable =
    (isp_cus3a_enable_fn_t)dlsym(handle, "MI_ISP_CUS3A_Enable");
  if (!fn_cus3a_enable) {
    printf("WARNING: MI_ISP_CUS3A_Enable not found; skip ISP priming\n");
    dlclose(handle);
    return -1;
  }

  ISPUserspace3AParams on = {{0}};
  on.params[0] = 1;
  on.params[1] = 1;
  on.params[2] = 1;
  MI_S32 ret = fn_cus3a_enable(0, &on);
  if (ret == 0) {
    printf("> Primed ISP userspace 3A\n");
  } else {
    printf("WARNING: MI_ISP_CUS3A_Enable priming failed %d\n", ret);
  }

  dlclose(handle);
  return ret;
}

static MI_S32 set_fps_with_format(MI_SNR_PAD_ID_e pad, MI_U32 fps, FpsFormat fmt,
  const char* phase, int verbose, int quiet)
{
  MI_S32 ret = -1;
  MI_U32 raw = fps;
  MI_U32 milli = fps * 1000;

  if (fmt == FPS_FMT_RAW_ONLY) {
    ret = MI_SNR_SetFps(pad, raw);
    if (!quiet && (verbose || ret != 0)) {
      printf("    MI_SNR_SetFps(%s, %u) -> %d\n", phase, raw, ret);
    }
    return ret;
  }

  if (fmt == FPS_FMT_RAW_THEN_MILLI) {
    ret = MI_SNR_SetFps(pad, raw);
    if (!quiet && (verbose || ret != 0)) {
      printf("    MI_SNR_SetFps(%s, %u) -> %d\n", phase, raw, ret);
    }
    if (ret == 0) {
      return 0;
    }
    ret = MI_SNR_SetFps(pad, milli);
    if (!quiet && (verbose || ret != 0)) {
      printf("    MI_SNR_SetFps(%s, %u) -> %d\n", phase, milli, ret);
    }
    return ret;
  }

  ret = MI_SNR_SetFps(pad, milli);
  if (!quiet && (verbose || ret != 0)) {
    printf("    MI_SNR_SetFps(%s, %u) -> %d\n", phase, milli, ret);
  }
  if (ret == 0) {
    return 0;
  }

  ret = MI_SNR_SetFps(pad, raw);
  if (!quiet && (verbose || ret != 0)) {
    printf("    MI_SNR_SetFps(%s, %u) -> %d\n", phase, raw, ret);
  }
  return ret;
}

static MI_S32 apply_plane_mode_strategy(MI_SNR_PAD_ID_e pad, PlaneModeStrategy strategy,
  const char* phase, int verbose, int quiet)
{
  MI_S32 ret = 0;
  if (strategy == PLANE_HDR || strategy == PLANE_HDR_THEN_LINEAR
    || strategy == PLANE_HDR_POST_LINEAR)
  {
    ret = MI_SNR_SetPlaneMode(pad, (MI_SNR_PlaneMode_e)1);
    if (!quiet && (verbose || ret != 0)) {
      printf("    MI_SNR_SetPlaneMode(%s, hdr) -> %d\n", phase, ret);
    }
    if (ret != 0 && strategy == PLANE_HDR) {
      return ret;
    }
  }

  if (strategy == PLANE_LINEAR || strategy == PLANE_HDR_THEN_LINEAR) {
    MI_S32 linear_ret = MI_SNR_SetPlaneMode(pad, E_MI_SNR_PLANE_MODE_LINEAR);
    if (!quiet && (verbose || linear_ret != 0)) {
      printf("    MI_SNR_SetPlaneMode(%s, linear) -> %d\n", phase, linear_ret);
    }
    ret = linear_ret;
  }

  return ret;
}

static MI_S32 run_cust_probe(const RunConfig* run, const char* phase) {
  if (!run->cust_pre) {
    return 0;
  }

  MI_U32 value = run->cust_value;
  void* payload = NULL;
  if (run->cust_size > 0) {
    payload = &value;
  }

  MI_S32 ret = MI_SNR_CustFunction(run->pad, run->cust_cmd, run->cust_size,
    payload, run->cust_dir);
  if (run->verbose || ret != 0) {
    printf("    MI_SNR_CustFunction(%s, cmd=0x%08x size=%u value=0x%08x dir=%d) -> %d\n",
      phase, (unsigned)run->cust_cmd, (unsigned)run->cust_size,
      (unsigned)run->cust_value, (int)run->cust_dir, (int)ret);
  }
  return ret;
}

static void stop_vif_best_effort(void) {
  (void)MI_VIF_DisableChnPort(0, 0);
  (void)MI_VIF_DisableDev(0);
}

static MI_S32 start_vif_from_sensor(MI_SNR_PAD_ID_e pad, int verbose) {
  MI_SNR_PadInfo_t pad_info = {0};
  MI_SNR_PlaneInfo_t plane = {0};
  MI_VIF_DevAttr_t dev = {0};
  MI_VIF_PortAttr_t port = {0};

  MI_S32 ret = MI_SNR_GetPadInfo(pad, &pad_info);
  if (ret != 0) {
    if (verbose) {
      printf("    MI_SNR_GetPadInfo -> %d\n", ret);
    }
    return ret;
  }

  ret = MI_SNR_GetPlaneInfo(pad, 0, &plane);
  if (ret != 0) {
    if (verbose) {
      printf("    MI_SNR_GetPlaneInfo -> %d\n", ret);
    }
    return ret;
  }

  dev.intf = pad_info.intf;
  /* Match Maruko/Majestic setup to avoid unsupported workmode on cold boot. */
  dev.work = I6_VIF_WORK_1MULTIPLEX;
  dev.hdr = I6_HDR_OFF;

  if (pad_info.intf == I6_INTF_MIPI) {
    dev.edge = I6_EDGE_DOUBLE;
    dev.input = pad_info.intfAttr.mipi.input;
  } else if (pad_info.intf == I6_INTF_BT656) {
    dev.edge = pad_info.intfAttr.bt656.edge;
    dev.sync = pad_info.intfAttr.bt656.sync;
    dev.bitswap = pad_info.intfAttr.bt656.bitswap;
  }

  ret = MI_VIF_SetDevAttr(0, &dev);
  if (ret != 0) {
    stop_vif_best_effort();
    return ret;
  }

  ret = MI_VIF_EnableDev(0);
  if (ret != 0) {
    stop_vif_best_effort();
    return ret;
  }

  port.capt = plane.capt;
  port.dest.width = plane.capt.width;
  port.dest.height = plane.capt.height;
  port.field = 0;
  port.interlaceOn = 0;
  if (plane.bayer > I6_BAYER_END) {
    port.pixFmt = plane.pixFmt;
  } else {
    port.pixFmt = (i6_common_pixfmt)
      (I6_PIXFMT_RGB_BAYER + plane.precision * I6_BAYER_END + plane.bayer);
  }
  port.frate = I6_VIF_FRATE_FULL;
  port.frameLineCnt = 0;

  ret = MI_VIF_SetChnPortAttr(0, 0, &port);
  if (ret != 0) {
    stop_vif_best_effort();
    return ret;
  }

  ret = MI_VIF_EnableChnPort(0, 0);
  return ret;
}

static MI_S32 touch_vif_once(MI_SNR_PAD_ID_e pad, int verbose) {
  MI_S32 ret = start_vif_from_sensor(pad, verbose);
  stop_vif_best_effort();
  return ret;
}

static void reset_sensor_state(MI_SNR_PAD_ID_e pad) {
  stop_vif_best_effort();
  (void)MI_SNR_Disable(pad);
  (void)MI_SNR_DeInitDev();
}

static void run_one_scenario(const RunConfig* run, const ScenarioConfig* sc, ScenarioResult* out) {
  memset(out, 0, sizeof(*out));
  out->plane_ret = -99999;
  out->setres_pre_ret = -99999;
  out->fps_pre_ret = -99999;
  out->enable_ret = -99999;
  out->setres_post_ret = -99999;
  out->fps_post_ret = -99999;
  out->reenable_ret = -99999;
  out->touch_vif_pre_ret = -99999;
  out->touch_vif_post_ret = -99999;
  out->hold_vif_pre_ret = -99999;
  out->hold_vif_post_ret = -99999;
  out->orien_pre_ret = -99999;
  out->orien_post_ret = -99999;
  out->final_mode_ret = -99999;
  out->final_fps_ret = -99999;

  int vif_hold_active = 0;

  if (!run->sticky_state && !run->no_reset) {
    reset_sensor_state(run->pad);
  }
  trace_sensor_state(run, "start", 1);
  if (run->prime_isp) {
    (void)prime_isp_userspace_3a();
  }
  (void)run_cust_probe(run, "pre");
  delay_if_needed(run->delay_ms);

  if (sc->init_snr_dev) {
    MI_SNR_InitParam_t init = {0};
    MI_U8 init_data_buf[256] = {0};
    init.u32DevId = (MI_U32)run->init_dev_id;
    if (run->init_data_bytes > 0) {
      for (int i = 0; i < run->init_data_bytes; ++i) {
        init_data_buf[i] = (MI_U8)((run->init_data_seed + i) & 0xFF);
      }
      init.u8Data = init_data_buf;
    }
    out->reenable_ret = MI_SNR_InitDev(&init);
    if (run->verbose) {
      printf("    MI_SNR_InitDev(dev=%u data=%d seed=%d) -> %d\n",
        (unsigned)init.u32DevId, run->init_data_bytes, run->init_data_seed, out->reenable_ret);
    }
  }

  out->plane_ret = apply_plane_mode_strategy(run->pad, run->plane_mode,
    "pre", run->verbose, run->quiet);
  if (run->verbose && run->quiet) {
    printf("    plane-mode strategy(pre) -> %d\n", out->plane_ret);
  }
  trace_sensor_state(run, "after-plane-pre", 1);
  delay_if_needed(run->delay_ms);

  if (sc->hold_vif == VIF_HOLD_PRE) {
    out->hold_vif_pre_ret = start_vif_from_sensor(run->pad, run->verbose);
    if (run->verbose) {
      printf("    hold_vif(pre) -> %d\n", out->hold_vif_pre_ret);
    }
    if (out->hold_vif_pre_ret == 0) {
      vif_hold_active = 1;
    }
    delay_if_needed(run->delay_ms);
  }

  if (sc->setres_timing == SETRES_PRE_ONLY || sc->setres_timing == SETRES_BOTH) {
    out->setres_pre_ret = MI_SNR_SetRes(run->pad, run->mode_idx);
    if (run->verbose) {
      printf("    MI_SNR_SetRes(pre, mode %u) -> %d\n", run->mode_idx, out->setres_pre_ret);
    }
    trace_sensor_state(run, "after-setres-pre", 1);
    delay_if_needed(run->delay_ms);
  }

  if (sc->orien_timing == ORIEN_PRE) {
    out->orien_pre_ret = MI_SNR_SetOrien(run->pad, 0, 0);
    if (run->verbose) {
      printf("    MI_SNR_SetOrien(pre) -> %d\n", out->orien_pre_ret);
    }
    delay_if_needed(run->delay_ms);
  }

  if (sc->touch_vif == TOUCH_PRE) {
    out->touch_vif_pre_ret = touch_vif_once(run->pad, run->verbose);
    if (run->verbose) {
      printf("    touch_vif(pre) -> %d\n", out->touch_vif_pre_ret);
    }
    delay_if_needed(run->delay_ms);
  }

  if (!run->skip_fps && (sc->fps_timing == FPS_TIMING_PRE || sc->fps_timing == FPS_TIMING_BOTH)) {
    out->fps_pre_ret = set_fps_with_format(run->pad, run->req_fps, sc->fps_format,
      "pre", run->verbose, run->quiet);
    if (out->fps_pre_ret == 0) {
      out->fps_any_success = 1;
    }
    delay_if_needed(run->delay_ms);
  }

  out->enable_ret = MI_SNR_Enable(run->pad);
  if (run->verbose) {
    printf("    MI_SNR_Enable -> %d\n", out->enable_ret);
  }
  trace_sensor_state(run, "after-enable", 0);
  delay_if_needed(run->delay_ms);

  if (run->plane_mode == PLANE_HDR_POST_LINEAR) {
    MI_S32 post_plane_ret = MI_SNR_SetPlaneMode(run->pad, E_MI_SNR_PLANE_MODE_LINEAR);
    if (!run->quiet && (run->verbose || post_plane_ret != 0)) {
      printf("    MI_SNR_SetPlaneMode(post-enable, linear) -> %d\n", post_plane_ret);
    }
    delay_if_needed(run->delay_ms);
  }

  if (sc->hold_vif == VIF_HOLD_POST) {
    out->hold_vif_post_ret = start_vif_from_sensor(run->pad, run->verbose);
    if (run->verbose) {
      printf("    hold_vif(post) -> %d\n", out->hold_vif_post_ret);
    }
    if (out->hold_vif_post_ret == 0) {
      vif_hold_active = 1;
    }
    delay_if_needed(run->delay_ms);
  }

  if (sc->setres_timing == SETRES_POST_ONLY || sc->setres_timing == SETRES_BOTH) {
    out->setres_post_ret = MI_SNR_SetRes(run->pad, run->mode_idx);
    if (run->verbose) {
      printf("    MI_SNR_SetRes(post, mode %u) -> %d\n", run->mode_idx, out->setres_post_ret);
    }
    trace_sensor_state(run, "after-setres-post", 0);
    delay_if_needed(run->delay_ms);
  }

  if (sc->orien_timing == ORIEN_POST) {
    out->orien_post_ret = MI_SNR_SetOrien(run->pad, 0, 0);
    if (run->verbose) {
      printf("    MI_SNR_SetOrien(post) -> %d\n", out->orien_post_ret);
    }
    delay_if_needed(run->delay_ms);
  }

  if (sc->touch_vif == TOUCH_POST) {
    out->touch_vif_post_ret = touch_vif_once(run->pad, run->verbose);
    if (run->verbose) {
      printf("    touch_vif(post) -> %d\n", out->touch_vif_post_ret);
    }
    delay_if_needed(run->delay_ms);
  }

  if (!run->skip_fps && (sc->fps_timing == FPS_TIMING_POST || sc->fps_timing == FPS_TIMING_BOTH)) {
    out->fps_post_ret = set_fps_with_format(run->pad, run->req_fps, sc->fps_format,
      "post", run->verbose, run->quiet);
    if (out->fps_post_ret == 0) {
      out->fps_any_success = 1;
    }
    trace_sensor_state(run, "after-fps-post", 0);
    delay_if_needed(run->delay_ms);
  }

  if (sc->reenable_cycle) {
    MI_S32 ret = MI_SNR_Disable(run->pad);
    if (run->verbose) {
      printf("    MI_SNR_Disable(recycle) -> %d\n", ret);
    }
    ret = apply_plane_mode_strategy(run->pad, run->plane_mode,
      "recycle", run->verbose, run->quiet);
    if (run->verbose && run->quiet) {
      printf("    plane-mode strategy(recycle) -> %d\n", ret);
    }
    ret = MI_SNR_SetRes(run->pad, run->mode_idx);
    if (run->verbose) {
      printf("    MI_SNR_SetRes(recycle, mode %u) -> %d\n", run->mode_idx, ret);
    }
    out->reenable_ret = MI_SNR_Enable(run->pad);
    if (run->verbose) {
      printf("    MI_SNR_Enable(recycle) -> %d\n", out->reenable_ret);
    }
    if (run->plane_mode == PLANE_HDR_POST_LINEAR) {
      MI_S32 post_plane_ret = MI_SNR_SetPlaneMode(run->pad, E_MI_SNR_PLANE_MODE_LINEAR);
      if (!run->quiet && (run->verbose || post_plane_ret != 0)) {
        printf("    MI_SNR_SetPlaneMode(recycle-post-enable, linear) -> %d\n", post_plane_ret);
      }
    }
    if (!run->skip_fps && sc->fps_timing != FPS_TIMING_PRE) {
      MI_S32 fps_ret = set_fps_with_format(run->pad, run->req_fps, sc->fps_format,
        "recycle-post", run->verbose, run->quiet);
      if (fps_ret == 0) {
        out->fps_any_success = 1;
      }
      out->fps_post_ret = fps_ret;
    }
  }

  MI_SNR_Res_t cur = {0};
  out->final_mode_ret = MI_SNR_GetCurRes(run->pad, &out->final_mode_idx, &cur);
  out->final_fps_ret = MI_SNR_GetFps(run->pad, &out->final_fps_raw);
  out->final_fps = normalize_fps(out->final_fps_raw);
  out->mode_match = (out->final_mode_ret == 0 && out->final_mode_idx == run->mode_idx);
  trace_sensor_state(run, "final", 0);

  if (vif_hold_active) {
    stop_vif_best_effort();
  }

  if (!run->sticky_state && !run->no_reset) {
    reset_sensor_state(run->pad);
  }
}

static void print_help(const char* prog) {
  printf("Usage: %s [options]\n", prog);
  printf("  --sensor-index N   Sensor pad index (0..3), default 0\n");
  printf("  --sensor-mode N    Sensor mode index, default 3\n");
  printf("  -f N               Requested FPS, default 120\n");
  printf("  --init-dev-id N    MI_SNR_InitDev dev id, default 0\n");
  printf("  --init-data-bytes N Bytes for MI_SNR_InitDev data ptr (0..256), default 0\n");
  printf("  --init-data-seed N Init payload seed byte (0..255), default 0\n");
  printf("  --max-cases N      Max scenarios to run, default 48\n");
  printf("  --full-matrix      Expand matrix dimensions (use with --max-cases)\n");
  printf("  --prime-isp        Prime userspace 3A before each scenario\n");
  printf("  --delay-ms N       Delay between calls in each scenario, default 0\n");
  printf("  --sticky-state     Keep sensor state between scenarios (no per-case reset)\n");
  printf("  --quiet            Suppress per-case MISS logs (prints only potential hits)\n");
  printf("  --trace-state      Print sensor state snapshots around key calls\n");
  printf("  --no-reset         Do not call Disable/DeInit before/after each case\n");
  printf("  --skip-fps         Skip MI_SNR_SetFps calls in scenarios\n");
  printf("  --plane-mode M     Plane mode strategy: linear|hdr|hdr-linear|hdr-post-linear\n");
  printf("  --cust-pre         Call MI_SNR_CustFunction before each scenario\n");
  printf("  --cust-cmd X       CustFunction command id (hex/dec), default 0x80046900\n");
  printf("  --cust-size N      CustFunction payload size, default 4\n");
  printf("  --cust-value X     CustFunction payload value (u32), default 0\n");
  printf("  --cust-dir N       CustFunction dir integer, default 0\n");
  printf("  --init-snr-dev N   Filter init path: 0 or 1\n");
  printf("  --setres-timing T  Filter setres timing: pre|post|both\n");
  printf("  --fps-timing T     Filter fps timing: pre|post|both\n");
  printf("  --hold-vif T       Filter held VIF timing: none|pre|post\n");
  printf("  --verbose          Print per-call returns\n");
  printf("  --help             Show help\n");
}

static const char* next_arg_value_or_empty(int argc, const char* argv[],
  int* arg_id)
{
  if (*arg_id + 1 < argc) {
    *arg_id += 1;
    return argv[*arg_id];
  }
  return "";
}

int main(int argc, const char* argv[]) {
  const char* prog_name = "snr_toggle_test";

  if (argc > 0 && argv[0] && *argv[0]) {
    prog_name = argv[0];
  }

  if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "/?")
      || !strcmp(argv[1], "/h"))) {
    print_help(prog_name);
    return 1;
  }

  RunConfig run = {
    .pad = E_MI_SNR_PAD_ID_0,
    .mode_idx = 3,
    .req_fps = 120,
    .init_dev_id = 0,
    .init_data_bytes = 0,
    .init_data_seed = 0,
    .prime_isp = 0,
    .full_matrix = 0,
    .max_cases = 48,
    .delay_ms = 0,
    .verbose = 0,
    .quiet = 0,
    .plane_mode = PLANE_LINEAR,
    .sticky_state = 0,
    .trace_state = 0,
    .no_reset = 0,
    .skip_fps = 0,
    .cust_pre = 0,
    .cust_cmd = 0x80046900u,
    .cust_size = 4,
    .cust_value = 0,
    .cust_dir = 0,
    .filter_init_snr_dev = -1,
    .filter_setres_timing = -1,
    .filter_fps_timing = -1,
    .filter_hold_vif = -1,
  };

  for (int arg_id = 1; arg_id < argc; ++arg_id) {
    const char* arg = argv[arg_id];
    const char* arg_value = NULL;

    if (!strcmp(arg, "--sensor-index")) {
      int idx = atoi(next_arg_value_or_empty(argc, argv, &arg_id));
      if (idx < 0 || idx > 3) {
        printf("ERROR: --sensor-index must be 0..3\n");
        return 1;
      }
      run.pad = (MI_SNR_PAD_ID_e)idx;
      continue;
    }

    if (!strcmp(arg, "--sensor-mode")) {
      int idx = atoi(next_arg_value_or_empty(argc, argv, &arg_id));
      if (idx < 0) {
        printf("ERROR: --sensor-mode must be >= 0\n");
        return 1;
      }
      run.mode_idx = (MI_U32)idx;
      continue;
    }

    if (!strcmp(arg, "-f")) {
      int fps = atoi(next_arg_value_or_empty(argc, argv, &arg_id));
      if (fps <= 0) {
        printf("ERROR: -f must be > 0\n");
        return 1;
      }
      run.req_fps = (MI_U32)fps;
      continue;
    }

    if (!strcmp(arg, "--init-dev-id")) {
      int dev_id = atoi(next_arg_value_or_empty(argc, argv, &arg_id));
      if (dev_id < 0 || dev_id > 65535) {
        printf("ERROR: --init-dev-id must be 0..65535\n");
        return 1;
      }
      run.init_dev_id = dev_id;
      continue;
    }

    if (!strcmp(arg, "--init-data-bytes")) {
      int n = atoi(next_arg_value_or_empty(argc, argv, &arg_id));
      if (n < 0 || n > 256) {
        printf("ERROR: --init-data-bytes must be 0..256\n");
        return 1;
      }
      run.init_data_bytes = n;
      continue;
    }

    if (!strcmp(arg, "--init-data-seed")) {
      int seed = atoi(next_arg_value_or_empty(argc, argv, &arg_id));
      if (seed < 0 || seed > 255) {
        printf("ERROR: --init-data-seed must be 0..255\n");
        return 1;
      }
      run.init_data_seed = seed;
      continue;
    }

    if (!strcmp(arg, "--max-cases")) {
      int n = atoi(next_arg_value_or_empty(argc, argv, &arg_id));
      if (n <= 0) {
        printf("ERROR: --max-cases must be > 0\n");
        return 1;
      }
      run.max_cases = n;
      continue;
    }

    if (!strcmp(arg, "--full-matrix")) {
      run.full_matrix = 1;
      continue;
    }

    if (!strcmp(arg, "--prime-isp")) {
      run.prime_isp = 1;
      continue;
    }

    if (!strcmp(arg, "--delay-ms")) {
      int ms = atoi(next_arg_value_or_empty(argc, argv, &arg_id));
      if (ms < 0 || ms > 5000) {
        printf("ERROR: --delay-ms must be 0..5000\n");
        return 1;
      }
      run.delay_ms = ms;
      continue;
    }

    if (!strcmp(arg, "--verbose")) {
      run.verbose = 1;
      continue;
    }

    if (!strcmp(arg, "--quiet")) {
      run.quiet = 1;
      continue;
    }

    if (!strcmp(arg, "--sticky-state")) {
      run.sticky_state = 1;
      continue;
    }

    if (!strcmp(arg, "--trace-state")) {
      run.trace_state = 1;
      continue;
    }

    if (!strcmp(arg, "--no-reset")) {
      run.no_reset = 1;
      continue;
    }

    if (!strcmp(arg, "--skip-fps")) {
      run.skip_fps = 1;
      continue;
    }

    if (!strcmp(arg, "--cust-pre")) {
      run.cust_pre = 1;
      continue;
    }

    if (!strcmp(arg, "--cust-cmd")) {
      arg_value = next_arg_value_or_empty(argc, argv, &arg_id);
      run.cust_cmd = (MI_U32)strtoul(arg_value, NULL, 0);
      continue;
    }

    if (!strcmp(arg, "--cust-size")) {
      int n = atoi(next_arg_value_or_empty(argc, argv, &arg_id));
      if (n < 0 || n > 4) {
        printf("ERROR: --cust-size must be 0..4\n");
        return 1;
      }
      run.cust_size = (MI_U32)n;
      continue;
    }

    if (!strcmp(arg, "--cust-value")) {
      arg_value = next_arg_value_or_empty(argc, argv, &arg_id);
      run.cust_value = (MI_U32)strtoul(arg_value, NULL, 0);
      continue;
    }

    if (!strcmp(arg, "--cust-dir")) {
      arg_value = next_arg_value_or_empty(argc, argv, &arg_id);
      run.cust_dir = (MI_S32)strtol(arg_value, NULL, 0);
      continue;
    }

    if (!strcmp(arg, "--setres-timing")) {
      SetResTiming t = SETRES_PRE_ONLY;
      arg_value = next_arg_value_or_empty(argc, argv, &arg_id);
      if (parse_setres_timing(arg_value, &t) != 0) {
        printf("ERROR: --setres-timing must be pre|post|both\n");
        return 1;
      }
      run.filter_setres_timing = (int)t;
      continue;
    }

    if (!strcmp(arg, "--init-snr-dev")) {
      int v = atoi(next_arg_value_or_empty(argc, argv, &arg_id));
      if (v != 0 && v != 1) {
        printf("ERROR: --init-snr-dev must be 0 or 1\n");
        return 1;
      }
      run.filter_init_snr_dev = v;
      continue;
    }

    if (!strcmp(arg, "--plane-mode")) {
      PlaneModeStrategy p = PLANE_LINEAR;
      arg_value = next_arg_value_or_empty(argc, argv, &arg_id);
      if (parse_plane_mode(arg_value, &p) != 0) {
        printf("ERROR: --plane-mode must be linear|hdr|hdr-linear|hdr-post-linear\n");
        return 1;
      }
      run.plane_mode = p;
      continue;
    }

    if (!strcmp(arg, "--fps-timing")) {
      FpsTiming t = FPS_TIMING_PRE;
      arg_value = next_arg_value_or_empty(argc, argv, &arg_id);
      if (parse_fps_timing(arg_value, &t) != 0) {
        printf("ERROR: --fps-timing must be pre|post|both\n");
        return 1;
      }
      run.filter_fps_timing = (int)t;
      continue;
    }

    if (!strcmp(arg, "--hold-vif")) {
      VifHoldTiming t = VIF_HOLD_NONE;
      arg_value = next_arg_value_or_empty(argc, argv, &arg_id);
      if (parse_hold_vif(arg_value, &t) != 0) {
        printf("ERROR: --hold-vif must be none|pre|post\n");
        return 1;
      }
      run.filter_hold_vif = (int)t;
      continue;
    }

    if (!strcmp(arg, "--help")) {
      print_help(prog_name);
      return 0;
    }

    printf("ERROR: Unknown argument\n");
    return 1;
  }

  MI_U32 res_count = 0;
  MI_SNR_Res_t selected_mode = {0};
  MI_S32 ret = MI_SYS_Init();
  if (ret != 0) {
    printf("ERROR: MI_SYS_Init failed %d\n", ret);
    return ret;
  }

  ret = MI_SNR_QueryResCount(run.pad, &res_count);
  if (ret != 0 || run.mode_idx >= res_count) {
    printf("ERROR: invalid mode index %u for pad %d (count %u, ret %d)\n",
      run.mode_idx, run.pad, res_count, ret);
    MI_SYS_Exit();
    return 1;
  }

  ret = MI_SNR_GetRes(run.pad, run.mode_idx, &selected_mode);
  if (ret != 0) {
    printf("ERROR: MI_SNR_GetRes(pad %d, mode %u) failed %d\n", run.pad, run.mode_idx, ret);
    MI_SYS_Exit();
    return 1;
  }

  printf("snr_toggle_test: pad %d mode %u (%ux%u min/max fps %u/%u) req-fps %u\n",
    run.pad, run.mode_idx, selected_mode.crop.width, selected_mode.crop.height,
    selected_mode.minFps, selected_mode.maxFps, run.req_fps);
  printf("matrix: %s, max-cases: %d, prime-isp: %s, delay-ms: %d, sticky-state: %s, plane: %s\n",
    run.full_matrix ? "full" : "fast", run.max_cases,
    run.prime_isp ? "on" : "off", run.delay_ms,
    run.sticky_state ? "on" : "off",
    plane_mode_name(run.plane_mode));
  printf("reset: %s, fps-calls: %s\n",
    run.no_reset ? "skip" : "default",
    run.skip_fps ? "skip" : "enabled");
  printf("cust-probe: %s cmd=0x%08x size=%u value=0x%08x dir=%d\n",
    run.cust_pre ? "on" : "off",
    (unsigned)run.cust_cmd, (unsigned)run.cust_size,
    (unsigned)run.cust_value, (int)run.cust_dir);
  printf("init-param: dev=%d data-bytes=%d seed=%d\n",
    run.init_dev_id, run.init_data_bytes, run.init_data_seed);
  if (run.filter_init_snr_dev >= 0
    || run.filter_setres_timing >= 0
    || run.filter_fps_timing >= 0
    || run.filter_hold_vif >= 0)
  {
    printf("filters: init=%s setres=%s fps=%s hold=%s\n",
      run.filter_init_snr_dev >= 0 ? (run.filter_init_snr_dev ? "1" : "0") : "*",
      run.filter_setres_timing >= 0
        ? setres_timing_name((SetResTiming)run.filter_setres_timing) : "*",
      run.filter_fps_timing >= 0
        ? fps_timing_name((FpsTiming)run.filter_fps_timing) : "*",
      run.filter_hold_vif >= 0
        ? hold_vif_name((VifHoldTiming)run.filter_hold_vif) : "*");
  }

  if (run.sticky_state && !run.no_reset) {
    reset_sensor_state(run.pad);
  }

  int init_opts[2] = {0, 1};
  FpsTiming fps_opts[3] = {FPS_TIMING_PRE, FPS_TIMING_POST, FPS_TIMING_BOTH};
  SetResTiming setres_opts_fast[2] = {SETRES_PRE_ONLY, SETRES_BOTH};
  SetResTiming setres_opts_full[3] = {SETRES_PRE_ONLY, SETRES_POST_ONLY, SETRES_BOTH};
  int reenable_opts[2] = {0, 1};
  TouchTiming touch_opts_fast[2] = {TOUCH_NONE, TOUCH_POST};
  TouchTiming touch_opts_full[3] = {TOUCH_NONE, TOUCH_PRE, TOUCH_POST};
  VifHoldTiming hold_opts_fast[2] = {VIF_HOLD_NONE, VIF_HOLD_PRE};
  VifHoldTiming hold_opts_full[3] = {VIF_HOLD_NONE, VIF_HOLD_PRE, VIF_HOLD_POST};
  OrienTiming orien_opts_fast[2] = {ORIEN_NONE, ORIEN_PRE};
  OrienTiming orien_opts_full[3] = {ORIEN_NONE, ORIEN_PRE, ORIEN_POST};
  FpsFormat fmt_opts_fast[1] = {FPS_FMT_RAW_THEN_MILLI};
  FpsFormat fmt_opts_full[3] = {
    FPS_FMT_RAW_ONLY, FPS_FMT_RAW_THEN_MILLI, FPS_FMT_MILLI_THEN_RAW};

  int pass_mode_count = 0;
  int pass_fps_count = 0;
  int pass_both_count = 0;
  int case_idx = 0;

  for (size_t i0 = 0; i0 < 2; ++i0) {
    for (size_t i1 = 0; i1 < 3; ++i1) {
      size_t setres_len = run.full_matrix ? 3 : 2;
      for (size_t i2 = 0; i2 < setres_len; ++i2) {
        for (size_t i3 = 0; i3 < (run.full_matrix ? 2 : 1); ++i3) {
          size_t hold_len = run.full_matrix ? 3 : 2;
          for (size_t i4 = 0; i4 < hold_len; ++i4) {
            size_t touch_len = run.full_matrix ? 3 : 2;
            for (size_t i5 = 0; i5 < touch_len; ++i5) {
              size_t orien_len = run.full_matrix ? 3 : 2;
              for (size_t i6 = 0; i6 < orien_len; ++i6) {
                size_t fmt_len = run.full_matrix ? 3 : 1;
                for (size_t i7 = 0; i7 < fmt_len; ++i7) {
                if (case_idx >= run.max_cases) {
                  goto matrix_done;
                }

                ScenarioConfig sc = {0};
                ScenarioResult out = {0};

                sc.init_snr_dev = init_opts[i0];
                sc.fps_timing = fps_opts[i1];
                sc.setres_timing = run.full_matrix ? setres_opts_full[i2] : setres_opts_fast[i2];
                sc.reenable_cycle = run.full_matrix ? reenable_opts[i3] : 0;
                sc.hold_vif = run.full_matrix ? hold_opts_full[i4] : hold_opts_fast[i4];
                sc.touch_vif = run.full_matrix ? touch_opts_full[i5] : touch_opts_fast[i5];
                sc.orien_timing = run.full_matrix ? orien_opts_full[i6] : orien_opts_fast[i6];
                sc.fps_format = run.full_matrix ? fmt_opts_full[i7] : fmt_opts_fast[i7];

                if (run.filter_setres_timing >= 0
                  && sc.setres_timing != (SetResTiming)run.filter_setres_timing)
                {
                  continue;
                }
                if (run.filter_init_snr_dev >= 0
                  && sc.init_snr_dev != run.filter_init_snr_dev)
                {
                  continue;
                }
                if (run.filter_fps_timing >= 0
                  && sc.fps_timing != (FpsTiming)run.filter_fps_timing)
                {
                  continue;
                }
                if (run.filter_hold_vif >= 0
                  && sc.hold_vif != (VifHoldTiming)run.filter_hold_vif)
                {
                  continue;
                }

                if (!run.quiet) {
                  printf("[%03d] init=%d fps=%-4s setres=%-4s reenable=%d hold=%-4s vif=%-4s orien=%-4s fmt=%-10s\n",
                    case_idx + 1, sc.init_snr_dev, fps_timing_name(sc.fps_timing),
                    setres_timing_name(sc.setres_timing), sc.reenable_cycle,
                    hold_vif_name(sc.hold_vif),
                    touch_name(sc.touch_vif), orien_name(sc.orien_timing),
                    fps_fmt_name(sc.fps_format));
                }

                run_one_scenario(&run, &sc, &out);

                int fps_ok = out.fps_any_success;
                if (out.mode_match) {
                  pass_mode_count++;
                }
                if (fps_ok) {
                  pass_fps_count++;
                }
                if (out.mode_match && fps_ok) {
                  pass_both_count++;
                }

                if (!run.quiet || out.mode_match || fps_ok) {
                  printf("[%03d] -> mode=%s (cur=%u req=%u) fps_set=%s getfps=%u(raw=%u) cur_mode_ret=%d\n",
                    case_idx + 1,
                    out.mode_match ? "OK" : "MISS",
                    (unsigned)out.final_mode_idx, (unsigned)run.mode_idx,
                    fps_ok ? "OK" : "FAIL",
                    (unsigned)out.final_fps, (unsigned)out.final_fps_raw,
                    out.final_mode_ret);
                }

                if (run.verbose) {
                  printf("         rets: plane=%d res_pre=%d en=%d res_post=%d fps_pre=%d fps_post=%d re_en=%d hold_pre=%d hold_post=%d vif_pre=%d vif_post=%d\n",
                    out.plane_ret, out.setres_pre_ret, out.enable_ret, out.setres_post_ret,
                    out.fps_pre_ret, out.fps_post_ret, out.reenable_ret,
                    out.hold_vif_pre_ret, out.hold_vif_post_ret,
                    out.touch_vif_pre_ret, out.touch_vif_post_ret);
                }

                case_idx++;
                }
              }
            }
          }
        }
      }
    }
  }

matrix_done:
  printf("summary: ran %d case(s), mode_ok=%d, fps_ok=%d, both_ok=%d\n",
    case_idx, pass_mode_count, pass_fps_count, pass_both_count);

  if (run.sticky_state && !run.no_reset) {
    reset_sensor_state(run.pad);
  }

  MI_SYS_Exit();
  return 0;
}
