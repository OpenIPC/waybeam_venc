#include "star6e.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <limits.h>

typedef struct {
  MI_SNR_PAD_ID_e pad;
  int init_dev;
  int query_count;
  int scan_res_pre;
  int get_pad_plane;
  int set_orien;
  int cust_pre;
  int cust_after_query;
  int cust_after_setres;
  int cust_raw;
  int cust_raw2;
  int cust_abi;
  MI_U32 cust_cmd;
  MI_U32 cust_size;
  MI_U32 cust_value;
  MI_S32 cust_dir;
  int touch_vif_pre;
  int hold_vif_pre;
  int hold_vif_post;
  int touch_vpe_pre;
  int hold_vpe_pre;
  int hold_vpe_post;
  int touch_venc_pre;
  int touch_venc_jpeg_pre;
  int touch_vif_post;
  int touch_vpe_post;
  int touch_venc_post;
  int touch_venc_jpeg_post;
  int prime_isp_pre;
  int cus3a_seq_pre;
  int cus3a_off_post;
  int graph_cycle_pre;
  int graph_cycle_ms;
  int graph_cycle_pull_frames;
  int no_state_read;
  int majestic_order;
  MI_U32 getres_idx;
  MI_U32 stage1_mode;
  MI_U32 stage1_fps;
  int skip_stage1_fps;
  MI_U32 stage2_mode;
  MI_U32 stage2_fps;
  int sleep_before_enable_ms;
  int sleep_before_stage2_ms;
  int run_stage2;
  int no_stage1_enable;
  int disable_between;
  int verbose;
} ProbeConfig;

static MI_U32 normalize_fps(MI_U32 raw) {
  if (raw == 0) {
    return 0;
  }
  if (raw >= 1000 && raw % 1000 == 0) {
    return raw / 1000;
  }
  return raw;
}

static int find_open_fd_for_path(const char* needle) {
  DIR* dir = opendir("/proc/self/fd");
  if (!dir) {
    return -1;
  }
  struct dirent* ent = NULL;
  char fd_path[PATH_MAX];
  char target[PATH_MAX];
  while ((ent = readdir(dir)) != NULL) {
    int fd = atoi(ent->d_name);
    if (fd <= 2) {
      continue;
    }
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%s", ent->d_name);
    ssize_t n = readlink(fd_path, target, sizeof(target) - 1);
    if (n <= 0) {
      continue;
    }
    target[n] = '\0';
    if (strstr(target, needle) != NULL) {
      closedir(dir);
      return fd;
    }
  }
  closedir(dir);
  return -1;
}

static void print_state(MI_SNR_PAD_ID_e pad, const char* label) {
  MI_SNR_Res_t cur = {0};
  MI_U8 cur_idx = 0;
  MI_U32 raw_fps = 0;
  MI_BOOL plane = 0;
  MI_S32 mode_ret = MI_SNR_GetCurRes(pad, &cur_idx, &cur);
  MI_S32 fps_ret = MI_SNR_GetFps(pad, &raw_fps);
  MI_S32 plane_ret = MI_SNR_GetPlaneMode(pad, &plane);

  printf("[%s] mode_ret=%d idx=%u %ux%u fps_ret=%d fps=%u(raw=%u) plane_ret=%d plane=%d\n",
    label, mode_ret, (unsigned)cur_idx, cur.crop.width, cur.crop.height,
    fps_ret, (unsigned)normalize_fps(raw_fps), (unsigned)raw_fps, plane_ret, (int)plane);
}

static void print_modes(MI_SNR_PAD_ID_e pad) {
  MI_U32 count = 0;
  MI_S32 ret = MI_SNR_QueryResCount(pad, &count);
  printf("modes: query_ret=%d count=%u\n", ret, (unsigned)count);
  if (ret != 0) {
    return;
  }
  for (MI_U32 i = 0; i < count; ++i) {
    MI_SNR_Res_t res = {0};
    ret = MI_SNR_GetRes(pad, i, &res);
    if (ret != 0) {
      printf("  [%u] getres failed %d\n", (unsigned)i, ret);
      continue;
    }
    printf("  [%u] %ux%u min/max fps %u/%u desc \"%s\"\n",
      (unsigned)i, res.crop.width, res.crop.height,
      (unsigned)res.minFps, (unsigned)res.maxFps, res.desc);
  }
}

static MI_S32 set_fps_with_retry(MI_SNR_PAD_ID_e pad, MI_U32 fps, const char* label) {
  MI_S32 ret = MI_SNR_SetFps(pad, fps);
  printf("%s MI_SNR_SetFps(%u) -> %d\n", label, (unsigned)fps, ret);
  if (ret == 0) {
    return 0;
  }
  MI_U64 milli = (MI_U64)fps * 1000ULL;
  if (milli > 0xFFFFFFFFULL) {
    return ret;
  }
  MI_S32 retry = MI_SNR_SetFps(pad, (MI_U32)milli);
  printf("%s MI_SNR_SetFps(%u) -> %d\n", label, (unsigned)milli, retry);
  return retry;
}

static void stop_vif_best_effort(void) {
  (void)MI_VIF_DisableChnPort(0, 0);
  (void)MI_VIF_DisableDev(0);
}

static MI_S32 start_vif_from_sensor(MI_SNR_PAD_ID_e pad) {
  MI_SNR_PadInfo_t pad_info = {0};
  MI_SNR_PlaneInfo_t plane = {0};
  MI_VIF_DevAttr_t dev = {0};
  MI_VIF_PortAttr_t port = {0};

  MI_S32 ret = MI_SNR_GetPadInfo(pad, &pad_info);
  if (ret != 0) {
    return ret;
  }

  ret = MI_SNR_GetPlaneInfo(pad, 0, &plane);
  if (ret != 0) {
    return ret;
  }

  dev.intf = pad_info.intf;
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
  if (ret != 0) {
    stop_vif_best_effort();
    return ret;
  }

  return 0;
}

static MI_S32 touch_vif_once(MI_SNR_PAD_ID_e pad) {
  MI_S32 ret = start_vif_from_sensor(pad);
  stop_vif_best_effort();
  return ret;
}

static void stop_vpe_best_effort(void) {
  (void)MI_VPE_DisablePort(0, 0);
  (void)MI_VPE_StopChannel(0);
  (void)MI_VPE_DestroyChannel(0);
}

static MI_S32 start_vpe_from_sensor(MI_SNR_PAD_ID_e pad) {
  MI_SNR_PlaneInfo_t plane = {0};
  MI_S32 ret = MI_SNR_GetPlaneInfo(pad, 0, &plane);
  if (ret != 0) {
    return ret;
  }

  MI_VPE_ChannelAttr_t ch = {0};
  ch.capt.width = plane.capt.width;
  ch.capt.height = plane.capt.height;
  ch.hdr = I6_HDR_OFF;
  ch.sensor = (i6_vpe_sens)((int)pad + 1);
  ch.mode = I6_VPE_MODE_REALTIME;
  if (plane.bayer > I6_BAYER_END) {
    ch.pixFmt = plane.pixFmt;
  } else {
    ch.pixFmt = (i6_common_pixfmt)
      (I6_PIXFMT_RGB_BAYER + plane.precision * I6_BAYER_END + plane.bayer);
  }

  ret = MI_VPE_CreateChannel(0, &ch);
  if (ret != 0) {
    stop_vpe_best_effort();
    return ret;
  }

  MI_VPE_ChannelParam_t param = {0};
  param.hdr = I6_HDR_OFF;
  param.level3DNR = 1;
  param.mirror = 0;
  param.flip = 0;
  param.lensAdjOn = 0;
  ret = MI_VPE_SetChannelParam(0, &param);
  if (ret != 0) {
    stop_vpe_best_effort();
    return ret;
  }

  ret = MI_VPE_StartChannel(0);
  if (ret != 0) {
    stop_vpe_best_effort();
    return ret;
  }

  MI_VPE_PortAttr_t port = {0};
  port.output.width = plane.capt.width;
  port.output.height = plane.capt.height;
  port.pixFmt = I6_PIXFMT_YUV420SP;
  port.compress = I6_COMPR_NONE;

  ret = MI_VPE_SetPortMode(0, 0, &port);
  if (ret != 0) {
    stop_vpe_best_effort();
    return ret;
  }

  ret = MI_VPE_EnablePort(0, 0);
  if (ret != 0) {
    stop_vpe_best_effort();
    return ret;
  }
  return 0;
}

static MI_S32 touch_vpe_once(MI_SNR_PAD_ID_e pad) {
  MI_S32 ret = start_vpe_from_sensor(pad);
  stop_vpe_best_effort();
  return ret;
}

static void fill_h26x_attr(i6_venc_attr_h26x* attr, MI_U32 width, MI_U32 height) {
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

static void fill_mjpg_attr(i6_venc_attr_mjpg* attr, MI_U32 width, MI_U32 height) {
  attr->maxWidth = width;
  attr->maxHeight = height;
  attr->bufSize = width * height * 3 / 2;
  attr->byFrame = 1;
  attr->width = width;
  attr->height = height;
  attr->dcfThumbs = 0;
  attr->markPerRow = 0;
}

static MI_S32 touch_venc_once(void) {
  MI_VENC_CHN chn = 0;
  MI_VENC_ChnAttr_t attr = {0};
  attr.attrib.codec = I6_VENC_CODEC_H265;
  fill_h26x_attr(&attr.attrib.h265, 1440, 1080);
  attr.rate.mode = I6_VENC_RATEMODE_H265CBR;
  attr.rate.h265Cbr = (i6_venc_rate_h26xcbr){
    .gop = 9,
    .statTime = 1,
    .fpsNum = 90,
    .fpsDen = 1,
    .bitrate = 12096 * 1024,
    .avgLvl = 1,
  };

  MI_S32 ret = MI_VENC_CreateChn(chn, &attr);
  if (ret != 0) {
    return ret;
  }
  ret = MI_VENC_StartRecvPic(chn);
  if (ret != 0) {
    (void)MI_VENC_DestroyChn(chn);
    return ret;
  }
  (void)MI_VENC_StopRecvPic(chn);
  (void)MI_VENC_DestroyChn(chn);
  return 0;
}

static MI_S32 start_venc_h265_hold(MI_VENC_CHN chn, MI_U32 fps) {
  MI_VENC_ChnAttr_t attr = {0};
  attr.attrib.codec = I6_VENC_CODEC_H265;
  fill_h26x_attr(&attr.attrib.h265, 1440, 1080);
  attr.rate.mode = I6_VENC_RATEMODE_H265CBR;
  attr.rate.h265Cbr = (i6_venc_rate_h26xcbr){
    .gop = 9,
    .statTime = 1,
    .fpsNum = fps,
    .fpsDen = 1,
    .bitrate = 12096 * 1024,
    .avgLvl = 1,
  };

  MI_S32 ret = MI_VENC_CreateChn(chn, &attr);
  if (ret != 0) {
    return ret;
  }
  ret = MI_VENC_StartRecvPic(chn);
  if (ret != 0) {
    (void)MI_VENC_DestroyChn(chn);
    return ret;
  }
  return 0;
}

static void stop_venc_best_effort(MI_VENC_CHN chn) {
  (void)MI_VENC_StopRecvPic(chn);
  (void)MI_VENC_DestroyChn(chn);
}

static void graph_pull_venc_frames(MI_VENC_CHN chn, int pulls) {
  for (int i = 0; i < pulls; ++i) {
    MI_VENC_Stat_t stat = {0};
    if (MI_VENC_Query(chn, &stat) != 0) {
      continue;
    }
    MI_U32 pack_count = stat.curPacks;
    if (pack_count == 0) {
      pack_count = 1;
    }
    if (pack_count > 32) {
      pack_count = 32;
    }

    MI_VENC_Pack_t packs[32];
    memset(packs, 0, sizeof(packs));
    MI_VENC_Stream_t stream = {0};
    stream.packet = packs;
    stream.count = pack_count;

    if (MI_VENC_GetStream(chn, &stream, 500) == 0) {
      (void)MI_VENC_ReleaseStream(chn, &stream);
    }
  }
}

static MI_S32 run_graph_cycle_once(MI_SNR_PAD_ID_e pad, int hold_ms, int pull_frames) {
  MI_S32 ret = start_vif_from_sensor(pad);
  if (ret != 0) {
    stop_vif_best_effort();
    return ret;
  }

  ret = start_vpe_from_sensor(pad);
  if (ret != 0) {
    stop_vpe_best_effort();
    stop_vif_best_effort();
    return ret;
  }

  MI_VENC_CHN chn = 0;
  ret = start_venc_h265_hold(chn, 90);
  if (ret != 0) {
    stop_venc_best_effort(chn);
    stop_vpe_best_effort();
    stop_vif_best_effort();
    return ret;
  }

  MI_U32 venc_dev = 0;
  ret = MI_VENC_GetChnDevid(chn, &venc_dev);
  if (ret != 0) {
    stop_venc_best_effort(chn);
    stop_vpe_best_effort();
    stop_vif_best_effort();
    return ret;
  }

  MI_SYS_ChnPort_t vif_port = {
    .module = I6_SYS_MOD_VIF,
    .device = 0,
    .channel = 0,
    .port = 0,
  };
  MI_SYS_ChnPort_t vpe_port = {
    .module = I6_SYS_MOD_VPE,
    .device = 0,
    .channel = 0,
    .port = 0,
  };
  MI_SYS_ChnPort_t venc_port = {
    .module = I6_SYS_MOD_VENC,
    .device = venc_dev,
    .channel = chn,
    .port = 0,
  };

  int bound_vif_vpe = 0;
  int bound_vpe_venc = 0;
  ret = MI_SYS_BindChnPort2(&vif_port, &vpe_port, 30, 30,
    I6_SYS_LINK_REALTIME | I6_SYS_LINK_LOWLATENCY, 0);
  if (ret == 0) {
    bound_vif_vpe = 1;
  }

  if (ret == 0) {
    ret = MI_SYS_BindChnPort2(&vpe_port, &venc_port, 30, 30, I6_SYS_LINK_FRAMEBASE, 0);
    if (ret == 0) {
      bound_vpe_venc = 1;
    }
  }

  if (ret == 0 && pull_frames > 0) {
    graph_pull_venc_frames(chn, pull_frames);
  }

  if (hold_ms > 0) {
    usleep((useconds_t)hold_ms * 1000U);
  }

  if (bound_vpe_venc) {
    (void)MI_SYS_UnBindChnPort(&vpe_port, &venc_port);
  }
  if (bound_vif_vpe) {
    (void)MI_SYS_UnBindChnPort(&vif_port, &vpe_port);
  }

  stop_venc_best_effort(chn);
  stop_vpe_best_effort();
  stop_vif_best_effort();
  return ret;
}

static MI_S32 touch_venc_jpeg_once(void) {
  MI_VENC_CHN chn = 2;
  MI_VENC_ChnAttr_t attr = {0};
  attr.attrib.codec = I6_VENC_CODEC_MJPG;
  fill_mjpg_attr(&attr.attrib.mjpg, 1920, 1080);
  attr.rate.mode = I6_VENC_RATEMODE_MJPGCBR;
  attr.rate.mjpgCbr = (i6_venc_rate_mjpgcbr){
    .bitrate = 4900 * 1024,
    .fpsNum = 5,
    .fpsDen = 1,
  };

  MI_S32 ret = MI_VENC_CreateChn(chn, &attr);
  if (ret != 0) {
    return ret;
  }
  ret = MI_VENC_StartRecvPic(chn);
  if (ret != 0) {
    (void)MI_VENC_DestroyChn(chn);
    return ret;
  }
  (void)MI_VENC_StopRecvPic(chn);
  (void)MI_VENC_DestroyChn(chn);
  return 0;
}

typedef int (*isp_cus3a_enable_fn_t)(int channel, void* params);

typedef struct {
  int params[13];
} ISPUserspace3AParams;

static MI_S32 prime_isp_userspace_3a(void) {
  void* handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
  if (!handle) {
    return -1;
  }
  isp_cus3a_enable_fn_t fn_cus3a_enable =
    (isp_cus3a_enable_fn_t)dlsym(handle, "MI_ISP_CUS3A_Enable");
  if (!fn_cus3a_enable) {
    dlclose(handle);
    return -2;
  }
  ISPUserspace3AParams on = {{0}};
  on.params[0] = 1;
  on.params[1] = 1;
  on.params[2] = 1;
  MI_S32 ret = fn_cus3a_enable(0, &on);
  dlclose(handle);
  return ret;
}

static MI_S32 set_isp_userspace_3a(int ae, int awb, int af) {
  void* handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
  if (!handle) {
    return -1;
  }
  isp_cus3a_enable_fn_t fn_cus3a_enable =
    (isp_cus3a_enable_fn_t)dlsym(handle, "MI_ISP_CUS3A_Enable");
  if (!fn_cus3a_enable) {
    dlclose(handle);
    return -2;
  }
  ISPUserspace3AParams p = {{0}};
  p.params[0] = ae ? 1 : 0;
  p.params[1] = awb ? 1 : 0;
  p.params[2] = af ? 1 : 0;
  MI_S32 ret = fn_cus3a_enable(0, &p);
  dlclose(handle);
  return ret;
}

static MI_S32 run_cust_probe(const ProbeConfig* cfg) {
  MI_U32 value = cfg->cust_value;
  void* payload = (cfg->cust_size > 0) ? (void*)&value : NULL;
  if (cfg->cust_raw2) {
    typedef struct {
      MI_U32 pad;
      MI_U32 cmd;
      MI_U32 size;
      void* data;
      MI_S32 dir;
    } SNRCustInner;
    typedef struct {
      MI_U32 payload_len;
      MI_U32 cmd;
      void* payload;
      MI_S32 payload_hi;
      MI_U32 payload_size;
    } SNRCustOuter;
    SNRCustInner in = {0};
    SNRCustOuter out = {0};
    int fd = find_open_fd_for_path("/dev/mi_sensor");
    int owned_fd = 0;
    if (fd < 0) {
      fd = open("/dev/mi_sensor", O_RDWR);
      owned_fd = 1;
    }
    if (fd < 0) {
      return -1;
    }
    in.pad = (MI_U32)cfg->pad;
    in.cmd = cfg->cust_cmd;
    in.size = cfg->cust_size;
    in.data = payload;
    in.dir = cfg->cust_dir;
    out.payload_len = (MI_U32)sizeof(in);
    out.cmd = cfg->cust_cmd;
    out.payload = &in;
    out.payload_hi = (((intptr_t)out.payload) < 0) ? -1 : 0;
    out.payload_size = cfg->cust_size;
    MI_S32 ret = ioctl(fd, 0xc014690f, &out);
    if (owned_fd) {
      close(fd);
    }
    return ret;
  }
  if (cfg->cust_raw) {
    typedef struct {
      MI_U32 payload_len;
      MI_U32 cmd;
      void* data;
      MI_S32 reserved;
      MI_U32 data_size;
    } SNRCustIoctl;
    SNRCustIoctl req = {0};
    MI_U32 value = cfg->cust_value;
    int fd = find_open_fd_for_path("/dev/mi_sensor");
    int owned_fd = 0;
    if (fd < 0) {
      fd = open("/dev/mi_sensor", O_RDWR);
      owned_fd = 1;
    }
    if (fd < 0) {
      return -1;
    }
    req.payload_len = (MI_U32)sizeof(req);
    req.cmd = cfg->cust_cmd;
    req.data = (cfg->cust_size > 0) ? (void*)&value : NULL;
    req.reserved = -1;
    req.data_size = cfg->cust_size;
    MI_S32 ret = ioctl(fd, 0xc014690f, &req);
    if (owned_fd) {
      close(fd);
    }
    return ret;
  }
  if (cfg->cust_abi > 0) {
    void* sym = dlsym(RTLD_DEFAULT, "MI_SNR_CustFunction");
    if (!sym) {
      return -2;
    }
    typedef MI_S32 (*FnAny)(void);
    FnAny fn = (FnAny)sym;
    switch (cfg->cust_abi) {
      case 1: {
        typedef MI_S32 (*Fn)(MI_SNR_PAD_ID_e, MI_U32, void*, MI_U32, MI_S32);
        return ((Fn)fn)(cfg->pad, cfg->cust_cmd, payload, cfg->cust_size, cfg->cust_dir);
      }
      case 2: {
        typedef MI_S32 (*Fn)(MI_SNR_PAD_ID_e, MI_U32, void*, MI_S32, MI_U32);
        return ((Fn)fn)(cfg->pad, cfg->cust_cmd, payload, cfg->cust_dir, cfg->cust_size);
      }
      case 3: {
        typedef MI_S32 (*Fn)(MI_SNR_PAD_ID_e, MI_S32, MI_U32, MI_U32, void*);
        return ((Fn)fn)(cfg->pad, cfg->cust_dir, cfg->cust_cmd, cfg->cust_size, payload);
      }
      case 4: {
        typedef MI_S32 (*Fn)(MI_SNR_PAD_ID_e, MI_S32, MI_U32, void*, MI_U32);
        return ((Fn)fn)(cfg->pad, cfg->cust_dir, cfg->cust_cmd, payload, cfg->cust_size);
      }
      case 5:
        return ((MI_S32 (*)(MI_SNR_PAD_ID_e, MI_U32, void*, MI_S32, MI_U32, MI_S32))fn)(
          cfg->pad, cfg->cust_cmd, payload, -1, cfg->cust_size, cfg->cust_dir);
      case 6:
        return ((MI_S32 (*)(MI_SNR_PAD_ID_e, MI_U32, MI_U32, void*, MI_U32, MI_S32))fn)(
          cfg->pad, cfg->cust_cmd, 0, payload, cfg->cust_size, cfg->cust_dir);
      case 7:
        return ((MI_S32 (*)(MI_U32, void*, MI_U32, MI_S32))fn)(
          cfg->cust_cmd, payload, cfg->cust_size, cfg->cust_dir);
      case 8:
        return ((MI_S32 (*)(MI_U32, void*, MI_S32, MI_U32))fn)(
          cfg->cust_cmd, payload, cfg->cust_dir, cfg->cust_size);
      case 9:
        return ((MI_S32 (*)(MI_SNR_PAD_ID_e, MI_U32, void*, MI_U32, MI_S32, MI_U32))fn)(
          cfg->pad, cfg->cust_cmd, payload, cfg->cust_size, cfg->cust_dir, 0);
      case 10:
        return ((MI_S32 (*)(MI_SNR_PAD_ID_e, MI_U32, void*, MI_S32, MI_U32, MI_U32))fn)(
          cfg->pad, cfg->cust_cmd, payload, -1, cfg->cust_size, 0);
      case 11:
        return ((MI_S32 (*)(MI_SNR_PAD_ID_e, MI_U32, MI_U32, void*, MI_S32, MI_U32))fn)(
          cfg->pad, cfg->cust_cmd, 0, payload, -1, cfg->cust_size);
      case 12:
        return ((MI_S32 (*)(MI_SNR_PAD_ID_e, MI_U32, MI_U32, void*, MI_U32, MI_S32))fn)(
          cfg->pad, cfg->cust_cmd, 0, payload, cfg->cust_size, cfg->cust_dir);
      case 13:
        return ((MI_S32 (*)(MI_U32, MI_SNR_PAD_ID_e, void*, MI_U32, MI_S32))fn)(
          cfg->cust_cmd, cfg->pad, payload, cfg->cust_size, cfg->cust_dir);
      case 14:
        return ((MI_S32 (*)(MI_U32, MI_SNR_PAD_ID_e, void*, MI_S32, MI_U32))fn)(
          cfg->cust_cmd, cfg->pad, payload, cfg->cust_dir, cfg->cust_size);
      case 15:
        return ((MI_S32 (*)(MI_U32, MI_SNR_PAD_ID_e, void*, MI_S32, MI_U32, MI_S32))fn)(
          cfg->cust_cmd, cfg->pad, payload, -1, cfg->cust_size, cfg->cust_dir);
      case 16:
        return ((MI_S32 (*)(MI_U32, MI_SNR_PAD_ID_e, MI_U32, void*, MI_U32, MI_S32))fn)(
          cfg->cust_cmd, cfg->pad, 0, payload, cfg->cust_size, cfg->cust_dir);
      case 17:
        return ((MI_S32 (*)(MI_SNR_PAD_ID_e, MI_U32, void*, MI_U32, MI_S32, MI_U32, MI_U32))fn)(
          cfg->pad, cfg->cust_cmd, payload, cfg->cust_size, cfg->cust_dir, 0, 0);
      case 18:
        return ((MI_S32 (*)(MI_SNR_PAD_ID_e, MI_U32, void*, MI_S32, MI_U32, MI_S32, MI_U32))fn)(
          cfg->pad, cfg->cust_cmd, payload, -1, cfg->cust_size, cfg->cust_dir, 0);
      case 19:
        return ((MI_S32 (*)(MI_U32, void*, MI_S32, MI_U32, MI_S32))fn)(
          cfg->cust_cmd, payload, -1, cfg->cust_size, cfg->cust_dir);
      case 20:
        return ((MI_S32 (*)(MI_U32, void*, MI_S32, MI_U32, MI_S32, MI_U32))fn)(
          cfg->cust_cmd, payload, -1, cfg->cust_size, cfg->cust_dir, 0);
      case 21:
        return ((MI_S32 (*)(MI_U32, MI_SNR_PAD_ID_e, void*, MI_S32, MI_U32, MI_S32))fn)(
          cfg->cust_cmd, cfg->pad, payload, -1, cfg->cust_size, cfg->cust_dir);
      case 22:
        return ((MI_S32 (*)(MI_U32, MI_SNR_PAD_ID_e, void*, MI_S32, MI_U32, MI_S32, MI_U32))fn)(
          cfg->cust_cmd, cfg->pad, payload, -1, cfg->cust_size, cfg->cust_dir, 0);
      case 23:
        return ((MI_S32 (*)(MI_SNR_PAD_ID_e, MI_U32, void*, MI_S32, MI_U32, MI_S32, MI_U32, MI_U32))fn)(
          cfg->pad, cfg->cust_cmd, payload, -1, cfg->cust_size, cfg->cust_dir, 0, 0);
      case 24:
        return ((MI_S32 (*)(MI_U32, void*, MI_S32, MI_U32, MI_S32, MI_U32, MI_U32))fn)(
          cfg->cust_cmd, payload, -1, cfg->cust_size, cfg->cust_dir, 0, 0);
      default:
        return -3;
    }
  }
  return MI_SNR_CustFunction(cfg->pad, cfg->cust_cmd, cfg->cust_size, payload, cfg->cust_dir);
}

static void print_help(const char* prog) {
  printf("Usage: %s [options]\n", prog);
  printf("  --sensor-index N      Sensor pad index (0..3), default 0\n");
  printf("  --init-snr-dev        Call MI_SNR_InitDev(dev=0) before sequence\n");
  printf("  --query-count         Call MI_SNR_QueryResCount before stage-1\n");
  printf("  --scan-res-pre        Call MI_SNR_GetRes for all indices before stage-1\n");
  printf("  --get-pad-plane       Call MI_SNR_GetPadInfo/GetPlaneInfo around stage-1\n");
  printf("  --set-orien           Call MI_SNR_SetOrien(0,0) in stage-1\n");
  printf("  --cust-pre            Call MI_SNR_CustFunction before stage-1\n");
  printf("  --cust-after-query    Move cust call to after Query/GetRes in stage-1\n");
  printf("  --cust-after-setres   Move cust call to after SetRes in stage-1\n");
  printf("  --cust-raw            Use raw /dev/mi_sensor ioctl for cust command\n");
  printf("  --cust-raw2           Use raw /dev/mi_sensor ioctl with nested payload\n");
  printf("  --cust-abi N          Alternate MI_SNR_CustFunction ABI permutation (1..24)\n");
  printf("  --cust-cmd X          CustFunction command id, default 0x80046900\n");
  printf("  --cust-size N         CustFunction payload size, default 4\n");
  printf("  --cust-value X        CustFunction payload value, default 0\n");
  printf("  --cust-dir N          CustFunction dir, default 0\n");
  printf("  --touch-vif-pre       Start/stop VIF once before stage-1\n");
  printf("  --hold-vif-pre        Start VIF before stage-1 and hold until cleanup\n");
  printf("  --hold-vif-post       Start VIF after stage-1 Enable and hold until cleanup\n");
  printf("  --touch-vpe-pre       Create/start/stop one VPE channel before stage-1\n");
  printf("  --hold-vpe-pre        Start one VPE channel before stage-1 and hold until cleanup\n");
  printf("  --hold-vpe-post       Start one VPE channel after stage-1 Enable and hold until cleanup\n");
  printf("  --touch-venc-pre      Create/start/stop one H265 VENC channel before stage-1\n");
  printf("  --touch-venc-jpeg-pre Create/start/stop one MJPEG VENC channel before stage-1\n");
  printf("  --touch-vif-post      Start/stop VIF once after stage-1 Enable\n");
  printf("  --touch-vpe-post      Create/start/stop one VPE channel after stage-1 Enable\n");
  printf("  --touch-venc-post     Create/start/stop one H265 VENC channel after stage-1 Enable\n");
  printf("  --touch-venc-jpeg-post Create/start/stop one MJPEG VENC channel after stage-1 Enable\n");
  printf("  --prime-isp-pre       Call MI_ISP_CUS3A_Enable(1,1,1) before stage-1\n");
  printf("  --cus3a-seq-pre       Run CUS3A sequence (100->110->111) before stage-1\n");
  printf("  --cus3a-off-post      Run CUS3A disable (000) after post-stage1 prelude\n");
  printf("  --graph-cycle-pre     Run short VIF->VPE->VENC bind/unbind warmup before stage-1\n");
  printf("  --graph-cycle-ms N    Hold graph warmup for N ms, default 200\n");
  printf("  --graph-cycle-pull-frames N Pull N frames from VENC during graph warmup, default 0\n");
  printf("  --no-state-read       Skip MI_SNR_GetCurRes/GetFps/GetPlaneMode snapshots\n");
  printf("  --majestic-order      Use stage-1 order closer to Majestic sensor init\n");
  printf("  --getres-index N      MI_SNR_GetRes index in stage-1, default 3\n");
  printf("  --stage1-mode N       Stage-1 SetRes index, default 1\n");
  printf("  --stage1-fps N        Stage-1 SetFps value, default 30\n");
  printf("  --skip-stage1-fps     Skip stage-1 MI_SNR_SetFps call\n");
  printf("  --sleep-before-enable-ms N Sleep before stage-1 Enable, default 0\n");
  printf("  --sleep-before-stage2-ms N Sleep before stage-2 SetRes/Fps, default 0\n");
  printf("  --no-stage1-enable    Skip stage-1 MI_SNR_Enable\n");
  printf("  --disable-between     MI_SNR_Disable between stage-1 and stage-2\n");
  printf("  --stage2-mode N       Stage-2 SetRes index, default 3\n");
  printf("  --stage2-fps N        Stage-2 SetFps value, default 120\n");
  printf("  --skip-stage2         Run only stage-1\n");
  printf("  --verbose             Print mode table at startup\n");
  printf("  --help                Show this help\n");
}

int main(int argc, const char* argv[]) {
  ProbeConfig cfg = {
    .pad = E_MI_SNR_PAD_ID_0,
    .init_dev = 0,
    .query_count = 0,
    .scan_res_pre = 0,
    .get_pad_plane = 0,
    .set_orien = 0,
    .cust_pre = 0,
    .cust_after_query = 0,
    .cust_after_setres = 0,
    .cust_raw = 0,
    .cust_raw2 = 0,
    .cust_abi = 0,
    .cust_cmd = 0x80046900u,
    .cust_size = 4,
    .cust_value = 0,
    .cust_dir = 0,
    .touch_vif_pre = 0,
    .hold_vif_pre = 0,
    .hold_vif_post = 0,
    .touch_vpe_pre = 0,
    .hold_vpe_pre = 0,
    .hold_vpe_post = 0,
    .touch_venc_pre = 0,
    .touch_venc_jpeg_pre = 0,
    .touch_vif_post = 0,
    .touch_vpe_post = 0,
    .touch_venc_post = 0,
    .touch_venc_jpeg_post = 0,
    .prime_isp_pre = 0,
    .cus3a_seq_pre = 0,
    .cus3a_off_post = 0,
    .graph_cycle_pre = 0,
    .graph_cycle_ms = 200,
    .graph_cycle_pull_frames = 0,
    .no_state_read = 0,
    .majestic_order = 0,
    .getres_idx = 3,
    .stage1_mode = 1,
    .stage1_fps = 30,
    .skip_stage1_fps = 0,
    .stage2_mode = 3,
    .stage2_fps = 120,
    .sleep_before_enable_ms = 0,
    .sleep_before_stage2_ms = 0,
    .run_stage2 = 1,
    .no_stage1_enable = 0,
    .disable_between = 0,
    .verbose = 0,
  };

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--help") == 0) {
      print_help(argv[0]);
      return 0;
    } else if (strcmp(argv[i], "--sensor-index") == 0 && i + 1 < argc) {
      int idx = atoi(argv[++i]);
      if (idx < 0 || idx > 3) {
        printf("ERROR: --sensor-index must be 0..3\n");
        return 1;
      }
      cfg.pad = (MI_SNR_PAD_ID_e)idx;
    } else if (strcmp(argv[i], "--init-snr-dev") == 0) {
      cfg.init_dev = 1;
    } else if (strcmp(argv[i], "--query-count") == 0) {
      cfg.query_count = 1;
    } else if (strcmp(argv[i], "--scan-res-pre") == 0) {
      cfg.scan_res_pre = 1;
    } else if (strcmp(argv[i], "--get-pad-plane") == 0) {
      cfg.get_pad_plane = 1;
    } else if (strcmp(argv[i], "--set-orien") == 0) {
      cfg.set_orien = 1;
    } else if (strcmp(argv[i], "--cust-pre") == 0) {
      cfg.cust_pre = 1;
    } else if (strcmp(argv[i], "--cust-after-query") == 0) {
      cfg.cust_after_query = 1;
    } else if (strcmp(argv[i], "--cust-after-setres") == 0) {
      cfg.cust_after_setres = 1;
    } else if (strcmp(argv[i], "--cust-raw") == 0) {
      cfg.cust_raw = 1;
    } else if (strcmp(argv[i], "--cust-raw2") == 0) {
      cfg.cust_raw2 = 1;
    } else if (strcmp(argv[i], "--cust-abi") == 0 && i + 1 < argc) {
      cfg.cust_abi = atoi(argv[++i]);
      if (cfg.cust_abi < 0 || cfg.cust_abi > 24) {
        printf("ERROR: --cust-abi must be 0..24\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--cust-cmd") == 0 && i + 1 < argc) {
      cfg.cust_cmd = (MI_U32)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--cust-size") == 0 && i + 1 < argc) {
      cfg.cust_size = (MI_U32)strtoul(argv[++i], NULL, 0);
      if (cfg.cust_size > 4) {
        printf("ERROR: --cust-size must be <= 4\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--cust-value") == 0 && i + 1 < argc) {
      cfg.cust_value = (MI_U32)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--cust-dir") == 0 && i + 1 < argc) {
      cfg.cust_dir = (MI_S32)strtol(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--touch-vif-pre") == 0) {
      cfg.touch_vif_pre = 1;
    } else if (strcmp(argv[i], "--hold-vif-pre") == 0) {
      cfg.hold_vif_pre = 1;
    } else if (strcmp(argv[i], "--hold-vif-post") == 0) {
      cfg.hold_vif_post = 1;
    } else if (strcmp(argv[i], "--touch-vpe-pre") == 0) {
      cfg.touch_vpe_pre = 1;
    } else if (strcmp(argv[i], "--hold-vpe-pre") == 0) {
      cfg.hold_vpe_pre = 1;
    } else if (strcmp(argv[i], "--hold-vpe-post") == 0) {
      cfg.hold_vpe_post = 1;
    } else if (strcmp(argv[i], "--touch-venc-pre") == 0) {
      cfg.touch_venc_pre = 1;
    } else if (strcmp(argv[i], "--touch-venc-jpeg-pre") == 0) {
      cfg.touch_venc_jpeg_pre = 1;
    } else if (strcmp(argv[i], "--touch-vif-post") == 0) {
      cfg.touch_vif_post = 1;
    } else if (strcmp(argv[i], "--touch-vpe-post") == 0) {
      cfg.touch_vpe_post = 1;
    } else if (strcmp(argv[i], "--touch-venc-post") == 0) {
      cfg.touch_venc_post = 1;
    } else if (strcmp(argv[i], "--touch-venc-jpeg-post") == 0) {
      cfg.touch_venc_jpeg_post = 1;
    } else if (strcmp(argv[i], "--prime-isp-pre") == 0) {
      cfg.prime_isp_pre = 1;
    } else if (strcmp(argv[i], "--cus3a-seq-pre") == 0) {
      cfg.cus3a_seq_pre = 1;
    } else if (strcmp(argv[i], "--cus3a-off-post") == 0) {
      cfg.cus3a_off_post = 1;
    } else if (strcmp(argv[i], "--graph-cycle-pre") == 0) {
      cfg.graph_cycle_pre = 1;
    } else if (strcmp(argv[i], "--graph-cycle-ms") == 0 && i + 1 < argc) {
      cfg.graph_cycle_ms = atoi(argv[++i]);
      if (cfg.graph_cycle_ms < 0 || cfg.graph_cycle_ms > 10000) {
        printf("ERROR: --graph-cycle-ms must be 0..10000\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--graph-cycle-pull-frames") == 0 && i + 1 < argc) {
      cfg.graph_cycle_pull_frames = atoi(argv[++i]);
      if (cfg.graph_cycle_pull_frames < 0 || cfg.graph_cycle_pull_frames > 100) {
        printf("ERROR: --graph-cycle-pull-frames must be 0..100\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--no-state-read") == 0) {
      cfg.no_state_read = 1;
    } else if (strcmp(argv[i], "--majestic-order") == 0) {
      cfg.majestic_order = 1;
    } else if (strcmp(argv[i], "--getres-index") == 0 && i + 1 < argc) {
      cfg.getres_idx = (MI_U32)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--stage1-mode") == 0 && i + 1 < argc) {
      cfg.stage1_mode = (MI_U32)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--stage1-fps") == 0 && i + 1 < argc) {
      cfg.stage1_fps = (MI_U32)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--skip-stage1-fps") == 0) {
      cfg.skip_stage1_fps = 1;
    } else if (strcmp(argv[i], "--sleep-before-enable-ms") == 0 && i + 1 < argc) {
      cfg.sleep_before_enable_ms = atoi(argv[++i]);
      if (cfg.sleep_before_enable_ms < 0 || cfg.sleep_before_enable_ms > 5000) {
        printf("ERROR: --sleep-before-enable-ms must be 0..5000\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--sleep-before-stage2-ms") == 0 && i + 1 < argc) {
      cfg.sleep_before_stage2_ms = atoi(argv[++i]);
      if (cfg.sleep_before_stage2_ms < 0 || cfg.sleep_before_stage2_ms > 10000) {
        printf("ERROR: --sleep-before-stage2-ms must be 0..10000\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--no-stage1-enable") == 0) {
      cfg.no_stage1_enable = 1;
    } else if (strcmp(argv[i], "--disable-between") == 0) {
      cfg.disable_between = 1;
    } else if (strcmp(argv[i], "--stage2-mode") == 0 && i + 1 < argc) {
      cfg.stage2_mode = (MI_U32)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--stage2-fps") == 0 && i + 1 < argc) {
      cfg.stage2_fps = (MI_U32)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--skip-stage2") == 0) {
      cfg.run_stage2 = 0;
    } else if (strcmp(argv[i], "--verbose") == 0) {
      cfg.verbose = 1;
    } else {
      printf("ERROR: Unknown argument: %s\n", argv[i]);
      print_help(argv[0]);
      return 1;
    }
  }

  MI_S32 ret = MI_SYS_Init();
  if (ret != 0) {
    printf("ERROR: MI_SYS_Init failed %d\n", ret);
    return 1;
  }

  if (cfg.init_dev) {
    MI_SNR_InitParam_t init = {0};
    ret = MI_SNR_InitDev(&init);
    printf("MI_SNR_InitDev(dev=%u) -> %d\n", (unsigned)init.u32DevId, ret);
  }

  printf("snr_sequence_probe: pad=%d getres=%u stage1(mode=%u,fps=%u) stage2=%s(mode=%u,fps=%u)\n",
    cfg.pad, (unsigned)cfg.getres_idx, (unsigned)cfg.stage1_mode, (unsigned)cfg.stage1_fps,
    cfg.run_stage2 ? "on" : "off", (unsigned)cfg.stage2_mode, (unsigned)cfg.stage2_fps);
  printf("options: query=%d scan-res-pre=%d padplane=%d orien=%d cust=%d(cmd=0x%08x,size=%u,val=0x%08x,dir=%d) "
         "cust-after-query=%d cust-after-setres=%d cust-raw=%d cust-raw2=%d cust-abi=%d "
         "touch-vif=%d hold-vif=%d hold-vif-post=%d touch-vpe=%d hold-vpe=%d hold-vpe-post=%d "
         "touch-venc=%d touch-venc-jpeg=%d touch-vif-post=%d touch-vpe-post=%d "
         "touch-venc-post=%d touch-venc-jpeg-post=%d prime-isp=%d "
         "cus3a-seq-pre=%d cus3a-off-post=%d "
         "graph-cycle-pre=%d graph-cycle-ms=%d graph-cycle-pull-frames=%d "
         "sleep-before-enable-ms=%d sleep-before-stage2-ms=%d no-state-read=%d "
         "majestic-order=%d skip-stage1-fps=%d\n",
    cfg.query_count, cfg.scan_res_pre, cfg.get_pad_plane, cfg.set_orien, cfg.cust_pre,
    (unsigned)cfg.cust_cmd, (unsigned)cfg.cust_size, (unsigned)cfg.cust_value, (int)cfg.cust_dir,
    cfg.cust_after_query, cfg.cust_after_setres, cfg.cust_raw, cfg.cust_raw2, cfg.cust_abi,
    cfg.touch_vif_pre, cfg.hold_vif_pre, cfg.hold_vif_post,
    cfg.touch_vpe_pre, cfg.hold_vpe_pre, cfg.hold_vpe_post,
    cfg.touch_venc_pre, cfg.touch_venc_jpeg_pre, cfg.touch_vif_post, cfg.touch_vpe_post,
    cfg.touch_venc_post, cfg.touch_venc_jpeg_post, cfg.prime_isp_pre,
    cfg.cus3a_seq_pre, cfg.cus3a_off_post,
    cfg.graph_cycle_pre, cfg.graph_cycle_ms, cfg.graph_cycle_pull_frames,
    cfg.sleep_before_enable_ms, cfg.sleep_before_stage2_ms, cfg.no_state_read,
    cfg.majestic_order, cfg.skip_stage1_fps);

  if (!cfg.no_state_read) {
    print_state(cfg.pad, "start");
  }
  if (cfg.verbose) {
    print_modes(cfg.pad);
  }

  if (cfg.majestic_order && cfg.cust_pre && !cfg.cust_after_query && !cfg.cust_after_setres) {
    ret = run_cust_probe(&cfg);
    printf("[stage1] MI_SNR_CustFunction(cmd=0x%08x,size=%u,val=0x%08x,dir=%d) -> %d\n",
      (unsigned)cfg.cust_cmd, (unsigned)cfg.cust_size, (unsigned)cfg.cust_value,
      (int)cfg.cust_dir, ret);
  }

  if (cfg.majestic_order && cfg.query_count) {
    MI_U32 count = 0;
    ret = MI_SNR_QueryResCount(cfg.pad, &count);
    printf("[stage1] MI_SNR_QueryResCount -> %d (count=%u)\n", ret, (unsigned)count);
  }

  if (cfg.majestic_order && cfg.scan_res_pre) {
    MI_U32 count = 0;
    MI_S32 qret = MI_SNR_QueryResCount(cfg.pad, &count);
    printf("[stage1] scan-res-pre query -> %d (count=%u)\n", qret, (unsigned)count);
    if (qret == 0) {
      for (MI_U32 idx = 0; idx < count; ++idx) {
        MI_SNR_Res_t res = {0};
        MI_S32 gret = MI_SNR_GetRes(cfg.pad, idx, &res);
        if (gret == 0) {
          printf("[stage1] scan-res-pre getres[%u] -> %d (%ux%u maxfps=%u)\n",
            (unsigned)idx, gret, res.crop.width, res.crop.height, (unsigned)res.maxFps);
        } else {
          printf("[stage1] scan-res-pre getres[%u] -> %d\n", (unsigned)idx, gret);
        }
      }
    }
  }

  MI_SNR_Res_t probe_res = {0};
  if (cfg.majestic_order) {
    ret = MI_SNR_GetRes(cfg.pad, cfg.getres_idx, &probe_res);
    printf("[stage1] MI_SNR_GetRes(idx=%u) -> %d", (unsigned)cfg.getres_idx, ret);
    if (ret == 0) {
      printf(" (%ux%u maxfps=%u)\n",
        probe_res.crop.width, probe_res.crop.height, (unsigned)probe_res.maxFps);
    } else {
      printf("\n");
    }
  }

  ret = MI_SNR_SetPlaneMode(cfg.pad, E_MI_SNR_PLANE_MODE_LINEAR);
  printf("[stage1] MI_SNR_SetPlaneMode(linear) -> %d\n", ret);

  if (!cfg.majestic_order && cfg.cust_pre && !cfg.cust_after_query && !cfg.cust_after_setres) {
    ret = run_cust_probe(&cfg);
    printf("[stage1] MI_SNR_CustFunction(cmd=0x%08x,size=%u,val=0x%08x,dir=%d) -> %d\n",
      (unsigned)cfg.cust_cmd, (unsigned)cfg.cust_size, (unsigned)cfg.cust_value,
      (int)cfg.cust_dir, ret);
  }

  if (!cfg.majestic_order && cfg.query_count) {
    MI_U32 count = 0;
    ret = MI_SNR_QueryResCount(cfg.pad, &count);
    printf("[stage1] MI_SNR_QueryResCount -> %d (count=%u)\n", ret, (unsigned)count);
  }

  if (!cfg.majestic_order && cfg.get_pad_plane) {
    MI_SNR_PadInfo_t pad = {0};
    MI_SNR_PlaneInfo_t plane = {0};
    MI_S32 pad_ret = MI_SNR_GetPadInfo(cfg.pad, &pad);
    MI_S32 plane_ret = MI_SNR_GetPlaneInfo(cfg.pad, 0, &plane);
    printf("[stage1] MI_SNR_GetPadInfo -> %d, MI_SNR_GetPlaneInfo -> %d\n", pad_ret, plane_ret);
  }

  int vif_held = 0;
  int vpe_held = 0;
  if (cfg.prime_isp_pre) {
    ret = prime_isp_userspace_3a();
    printf("[stage1] prime_isp_userspace_3a -> %d\n", ret);
  }
  if (cfg.cus3a_seq_pre) {
    MI_S32 r100 = set_isp_userspace_3a(1, 0, 0);
    MI_S32 r110 = set_isp_userspace_3a(1, 1, 0);
    MI_S32 r111 = set_isp_userspace_3a(1, 1, 1);
    printf("[stage1] cus3a_seq_pre 100=%d 110=%d 111=%d\n", r100, r110, r111);
  }
  if (cfg.graph_cycle_pre) {
    ret = run_graph_cycle_once(cfg.pad, cfg.graph_cycle_ms, cfg.graph_cycle_pull_frames);
    printf("[stage1] run_graph_cycle_once(%d ms, pull=%d) -> %d\n",
      cfg.graph_cycle_ms, cfg.graph_cycle_pull_frames, ret);
  }
  if (cfg.touch_vif_pre) {
    ret = touch_vif_once(cfg.pad);
    printf("[stage1] touch_vif_once -> %d\n", ret);
  }
  if (cfg.hold_vif_pre) {
    ret = start_vif_from_sensor(cfg.pad);
    vif_held = (ret == 0);
    printf("[stage1] start_vif_from_sensor(hold) -> %d\n", ret);
  }
  if (cfg.touch_vpe_pre) {
    ret = touch_vpe_once(cfg.pad);
    printf("[stage1] touch_vpe_once -> %d\n", ret);
  }
  if (cfg.hold_vpe_pre) {
    ret = start_vpe_from_sensor(cfg.pad);
    vpe_held = (ret == 0);
    printf("[stage1] start_vpe_from_sensor(hold) -> %d\n", ret);
  }
  if (cfg.touch_venc_pre) {
    ret = touch_venc_once();
    printf("[stage1] touch_venc_once -> %d\n", ret);
  }
  if (cfg.touch_venc_jpeg_pre) {
    ret = touch_venc_jpeg_once();
    printf("[stage1] touch_venc_jpeg_once -> %d\n", ret);
  }

  if (!cfg.majestic_order) {
    ret = MI_SNR_GetRes(cfg.pad, cfg.getres_idx, &probe_res);
    printf("[stage1] MI_SNR_GetRes(idx=%u) -> %d", (unsigned)cfg.getres_idx, ret);
    if (ret == 0) {
      printf(" (%ux%u maxfps=%u)\n",
        probe_res.crop.width, probe_res.crop.height, (unsigned)probe_res.maxFps);
    } else {
      printf("\n");
    }
  }

  if (cfg.cust_pre && cfg.cust_after_query && !cfg.cust_after_setres) {
    ret = run_cust_probe(&cfg);
    printf("[stage1] MI_SNR_CustFunction(after-query, cmd=0x%08x,size=%u,val=0x%08x,dir=%d) -> %d\n",
      (unsigned)cfg.cust_cmd, (unsigned)cfg.cust_size, (unsigned)cfg.cust_value,
      (int)cfg.cust_dir, ret);
  }

  ret = MI_SNR_SetRes(cfg.pad, cfg.stage1_mode);
  printf("[stage1] MI_SNR_SetRes(%u) -> %d\n", (unsigned)cfg.stage1_mode, ret);

  if (cfg.cust_pre && cfg.cust_after_setres) {
    ret = run_cust_probe(&cfg);
    printf("[stage1] MI_SNR_CustFunction(after-setres, cmd=0x%08x,size=%u,val=0x%08x,dir=%d) -> %d\n",
      (unsigned)cfg.cust_cmd, (unsigned)cfg.cust_size, (unsigned)cfg.cust_value,
      (int)cfg.cust_dir, ret);
  }

  if (cfg.set_orien) {
    ret = MI_SNR_SetOrien(cfg.pad, 0, 0);
    printf("[stage1] MI_SNR_SetOrien(0,0) -> %d\n", ret);
  }

  if (!cfg.skip_stage1_fps) {
    (void)set_fps_with_retry(cfg.pad, cfg.stage1_fps, "[stage1]");
  }

  if (!cfg.no_stage1_enable) {
    if (cfg.sleep_before_enable_ms > 0) {
      usleep((useconds_t)cfg.sleep_before_enable_ms * 1000U);
      printf("[stage1] sleep-before-enable %d ms\n", cfg.sleep_before_enable_ms);
    }
    ret = MI_SNR_Enable(cfg.pad);
    printf("[stage1] MI_SNR_Enable -> %d\n", ret);
  }

  if (cfg.majestic_order && cfg.get_pad_plane) {
    MI_SNR_PadInfo_t pad = {0};
    MI_SNR_PlaneInfo_t plane = {0};
    MI_S32 pad_ret = MI_SNR_GetPadInfo(cfg.pad, &pad);
    MI_S32 plane_ret = MI_SNR_GetPlaneInfo(cfg.pad, 0, &plane);
    printf("[stage1] MI_SNR_GetPadInfo(post-enable) -> %d, MI_SNR_GetPlaneInfo(post-enable) -> %d\n",
      pad_ret, plane_ret);
  }

  if (!cfg.no_state_read) {
    print_state(cfg.pad, "after-stage1");
  }

  if (cfg.touch_vif_post) {
    ret = touch_vif_once(cfg.pad);
    printf("[post-stage1] touch_vif_once -> %d\n", ret);
  }
  if (cfg.hold_vif_post && !vif_held) {
    ret = start_vif_from_sensor(cfg.pad);
    vif_held = (ret == 0);
    printf("[post-stage1] start_vif_from_sensor(hold) -> %d\n", ret);
  }
  if (cfg.touch_vpe_post) {
    ret = touch_vpe_once(cfg.pad);
    printf("[post-stage1] touch_vpe_once -> %d\n", ret);
  }
  if (cfg.hold_vpe_post && !vpe_held) {
    ret = start_vpe_from_sensor(cfg.pad);
    vpe_held = (ret == 0);
    printf("[post-stage1] start_vpe_from_sensor(hold) -> %d\n", ret);
  }
  if (cfg.touch_venc_post) {
    ret = touch_venc_once();
    printf("[post-stage1] touch_venc_once -> %d\n", ret);
  }
  if (cfg.touch_venc_jpeg_post) {
    ret = touch_venc_jpeg_once();
    printf("[post-stage1] touch_venc_jpeg_once -> %d\n", ret);
  }
  if (cfg.cus3a_off_post) {
    ret = set_isp_userspace_3a(0, 0, 0);
    printf("[post-stage1] set_isp_userspace_3a(0,0,0) -> %d\n", ret);
  }

  if (cfg.disable_between) {
    ret = MI_SNR_Disable(cfg.pad);
    printf("[between] MI_SNR_Disable -> %d\n", ret);
    if (!cfg.no_state_read) {
      print_state(cfg.pad, "after-disable-between");
    }
  }

  if (cfg.run_stage2) {
    if (cfg.sleep_before_stage2_ms > 0) {
      usleep((useconds_t)cfg.sleep_before_stage2_ms * 1000U);
      printf("[between] sleep-before-stage2 %d ms\n", cfg.sleep_before_stage2_ms);
    }

    ret = MI_SNR_SetRes(cfg.pad, cfg.stage2_mode);
    printf("[stage2] MI_SNR_SetRes(%u) -> %d\n", (unsigned)cfg.stage2_mode, ret);

    (void)set_fps_with_retry(cfg.pad, cfg.stage2_fps, "[stage2]");

    if (cfg.disable_between) {
      ret = MI_SNR_Enable(cfg.pad);
      printf("[stage2] MI_SNR_Enable -> %d\n", ret);
    }

    if (!cfg.no_state_read) {
      print_state(cfg.pad, "after-stage2");
    }
  }

  if (vpe_held) {
    stop_vpe_best_effort();
  }
  if (vif_held) {
    stop_vif_best_effort();
  }
  (void)MI_SNR_Disable(cfg.pad);
  (void)MI_SNR_DeInitDev();
  (void)MI_SYS_Exit();
  return 0;
}
