#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

typedef int MI_S32;
typedef unsigned int MI_U32;
typedef int MI_SNR_PAD_ID_e;

typedef MI_S32 (*cust_fn_t)(MI_SNR_PAD_ID_e, MI_U32, MI_U32, void*, MI_S32);

MI_S32 MI_SNR_CustFunction(MI_SNR_PAD_ID_e pad_id, MI_U32 cmd_id,
  MI_U32 data_size, void* data, MI_S32 dir) {
  static cust_fn_t real_fn = NULL;
  if (!real_fn) {
    real_fn = (cust_fn_t)dlsym(RTLD_NEXT, "MI_SNR_CustFunction");
  }

  uint32_t d0 = 0;
  if (data && data_size >= 4) {
    d0 = *(uint32_t*)data;
  }
  fprintf(stderr,
    "[custfn-intercept pid=%d] pad=%d cmd=0x%08x size=%u data=%p d0=0x%08x dir=%d\n",
    (int)getpid(), pad_id, cmd_id, data_size, data, d0, dir);
  fflush(stderr);

  if (!real_fn) {
    return -1;
  }
  MI_S32 ret = real_fn(pad_id, cmd_id, data_size, data, dir);
  fprintf(stderr, "[custfn-intercept pid=%d] ret=%d\n", (int)getpid(), ret);
  fflush(stderr);
  return ret;
}
