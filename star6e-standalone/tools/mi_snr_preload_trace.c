#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  uint32_t dev_id;
  uint8_t* data;
} snr_init_param_probe_t;

static void trace_log(const char* fmt, ...) {
  char line[512];
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  int off = snprintf(line, sizeof(line), "[snr-trace %ld.%03ld pid=%d] ",
    (long)ts.tv_sec, (long)(ts.tv_nsec / 1000000L), (int)getpid());
  if (off < 0 || off >= (int)sizeof(line)) {
    return;
  }

  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line + off, sizeof(line) - (size_t)off, fmt, ap);
  va_end(ap);
  if (n < 0) {
    return;
  }

  size_t len = strnlen(line, sizeof(line));
  if (len + 1 < sizeof(line)) {
    line[len++] = '\n';
  }
  (void)write(STDERR_FILENO, line, len);
}

static void* load_next(const char* symbol) {
  void* fn = dlsym(RTLD_NEXT, symbol);
  if (!fn) {
    trace_log("%s resolve failed: %s", symbol, dlerror());
  }
  return fn;
}

int MI_SNR_InitDev(void* init_param) {
  static int (*real_fn)(void* init_param);
  if (!real_fn) {
    real_fn = (int (*)(void*))load_next("MI_SNR_InitDev");
  }
  if (!real_fn) {
    return -1;
  }

  if (init_param) {
    const snr_init_param_probe_t* p = (const snr_init_param_probe_t*)init_param;
    trace_log("MI_SNR_InitDev(dev=%u data=%p)", p->dev_id, (void*)p->data);
  } else {
    trace_log("MI_SNR_InitDev(NULL)");
  }

  int ret = real_fn(init_param);
  trace_log("MI_SNR_InitDev -> %d", ret);
  return ret;
}

int MI_SNR_DeInitDev(void) {
  static int (*real_fn)(void);
  if (!real_fn) {
    real_fn = (int (*)(void))load_next("MI_SNR_DeInitDev");
  }
  if (!real_fn) {
    return -1;
  }
  trace_log("MI_SNR_DeInitDev()");
  int ret = real_fn();
  trace_log("MI_SNR_DeInitDev -> %d", ret);
  return ret;
}

int MI_SNR_SetPlaneMode(int pad, int mode) {
  static int (*real_fn)(int, int);
  if (!real_fn) {
    real_fn = (int (*)(int, int))load_next("MI_SNR_SetPlaneMode");
  }
  if (!real_fn) {
    return -1;
  }
  trace_log("MI_SNR_SetPlaneMode(pad=%d mode=%d)", pad, mode);
  int ret = real_fn(pad, mode);
  trace_log("MI_SNR_SetPlaneMode -> %d", ret);
  return ret;
}

int MI_SNR_GetPlaneMode(int pad, bool* enable) {
  static int (*real_fn)(int, bool*);
  if (!real_fn) {
    real_fn = (int (*)(int, bool*))load_next("MI_SNR_GetPlaneMode");
  }
  if (!real_fn) {
    return -1;
  }
  int ret = real_fn(pad, enable);
  trace_log("MI_SNR_GetPlaneMode(pad=%d) -> %d enable=%d",
    pad, ret, enable ? (*enable ? 1 : 0) : -1);
  return ret;
}

int MI_SNR_SetRes(int pad, unsigned int idx) {
  static int (*real_fn)(int, unsigned int);
  if (!real_fn) {
    real_fn = (int (*)(int, unsigned int))load_next("MI_SNR_SetRes");
  }
  if (!real_fn) {
    return -1;
  }
  trace_log("MI_SNR_SetRes(pad=%d idx=%u)", pad, idx);
  int ret = real_fn(pad, idx);
  trace_log("MI_SNR_SetRes -> %d", ret);
  return ret;
}

int MI_SNR_GetCurRes(int pad, unsigned char* cur_idx, void* cur_res) {
  static int (*real_fn)(int, unsigned char*, void*);
  if (!real_fn) {
    real_fn = (int (*)(int, unsigned char*, void*))load_next("MI_SNR_GetCurRes");
  }
  if (!real_fn) {
    return -1;
  }
  int ret = real_fn(pad, cur_idx, cur_res);
  trace_log("MI_SNR_GetCurRes(pad=%d) -> %d idx=%d",
    pad, ret, cur_idx ? (int)(*cur_idx) : -1);
  return ret;
}

int MI_SNR_SetFps(int pad, unsigned int fps) {
  static int (*real_fn)(int, unsigned int);
  if (!real_fn) {
    real_fn = (int (*)(int, unsigned int))load_next("MI_SNR_SetFps");
  }
  if (!real_fn) {
    return -1;
  }
  trace_log("MI_SNR_SetFps(pad=%d fps=%u)", pad, fps);
  int ret = real_fn(pad, fps);
  trace_log("MI_SNR_SetFps -> %d", ret);
  return ret;
}

int MI_SNR_GetFps(int pad, unsigned int* fps) {
  static int (*real_fn)(int, unsigned int*);
  if (!real_fn) {
    real_fn = (int (*)(int, unsigned int*))load_next("MI_SNR_GetFps");
  }
  if (!real_fn) {
    return -1;
  }
  int ret = real_fn(pad, fps);
  trace_log("MI_SNR_GetFps(pad=%d) -> %d fps=%u",
    pad, ret, fps ? *fps : 0);
  return ret;
}

int MI_SNR_SetOrien(int pad, bool mirror, bool flip) {
  static int (*real_fn)(int, bool, bool);
  if (!real_fn) {
    real_fn = (int (*)(int, bool, bool))load_next("MI_SNR_SetOrien");
  }
  if (!real_fn) {
    return -1;
  }
  trace_log("MI_SNR_SetOrien(pad=%d mirror=%d flip=%d)",
    pad, mirror ? 1 : 0, flip ? 1 : 0);
  int ret = real_fn(pad, mirror, flip);
  trace_log("MI_SNR_SetOrien -> %d", ret);
  return ret;
}

int MI_SNR_Enable(int pad) {
  static int (*real_fn)(int);
  if (!real_fn) {
    real_fn = (int (*)(int))load_next("MI_SNR_Enable");
  }
  if (!real_fn) {
    return -1;
  }
  trace_log("MI_SNR_Enable(pad=%d)", pad);
  int ret = real_fn(pad);
  trace_log("MI_SNR_Enable -> %d", ret);
  return ret;
}

int MI_SNR_Disable(int pad) {
  static int (*real_fn)(int);
  if (!real_fn) {
    real_fn = (int (*)(int))load_next("MI_SNR_Disable");
  }
  if (!real_fn) {
    return -1;
  }
  trace_log("MI_SNR_Disable(pad=%d)", pad);
  int ret = real_fn(pad);
  trace_log("MI_SNR_Disable -> %d", ret);
  return ret;
}

__attribute__((constructor))
static void snr_trace_ctor(void) {
  trace_log("MI_SNR preload trace active");
}
