#ifndef DETECT_DEQUANT_H
#define DETECT_DEQUANT_H

/*
 * IPU output-tensor dequantization.
 *
 * The Infinity6E IPU returns quantized integer tensors (plus a per-tensor
 * scale + zero point) or plain FP32.  YOLOv8 (and any other model) decode
 * starts by converting those to real values:
 *
 *     real = (quant - zero_pnt) * scalar          (SigmaStar affine)
 *
 * Kept as a standalone, SDK-free module so the math is unit-testable on the
 * host and reused by every model decoder.  The exact affine convention is
 * confirmed against real tensors during Phase-1 on-device bring-up.
 */

#include <stddef.h>

#include "star6e_ipu.h"

/** Dequantize one already-widened integer sample. */
float detect_dequant_affine(long long q, float scalar, long long zero_pnt);

/** Dequantize a packed tensor buffer of `count` elements into `dst`.
 *
 *  Reads each element at its native width for `fmt` (u8/int8/int16/int32),
 *  applies the affine, and writes a float.  FP32 input is copied through
 *  (scalar/zero_pnt ignored).  Returns 0 on success, -1 if `fmt` is not a
 *  dequantizable numeric format (e.g. NV12/ARGB — those are input layouts,
 *  not output tensors). */
int detect_dequant_buffer(const void *src, IpuFmt fmt, float scalar,
	long long zero_pnt, float *dst, size_t count);

#endif /* DETECT_DEQUANT_H */
