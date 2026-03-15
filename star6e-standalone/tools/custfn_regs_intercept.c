#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

typedef int MI_S32;
typedef MI_S32 (*custfn_8_t)(uint32_t, uint32_t, uint32_t, uint32_t,
                              uint32_t, uint32_t, uint32_t, uint32_t);

MI_S32 MI_SNR_CustFunction(void) {
  static custfn_8_t real_fn = NULL;
  static int count = 0;
  if (!real_fn) {
    real_fn = (custfn_8_t)dlsym(RTLD_NEXT, "MI_SNR_CustFunction");
  }

  register uint32_t r0 asm("r0");
  register uint32_t r1 asm("r1");
  register uint32_t r2 asm("r2");
  register uint32_t r3 asm("r3");
  uint32_t s0 = 0;
  uint32_t s1 = 0;
  uint32_t s2 = 0;
  uint32_t s3 = 0;
  asm volatile(
    "ldr %0, [sp, #0]\n\t"
    "ldr %1, [sp, #4]\n\t"
    "ldr %2, [sp, #8]\n\t"
    "ldr %3, [sp, #12]\n\t"
    : "=r"(s0), "=r"(s1), "=r"(s2), "=r"(s3));

  if (count < 16) {
    fprintf(stderr,
      "[custfn-regs pid=%d n=%d] r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x "
      "s0=0x%08x s1=0x%08x s2=0x%08x s3=0x%08x\n",
      (int)getpid(), count, r0, r1, r2, r3, s0, s1, s2, s3);
    fflush(stderr);
  }
  ++count;

  if (!real_fn) {
    return -1;
  }
  return real_fn(r0, r1, r2, r3, s0, s1, s2, s3);
}
