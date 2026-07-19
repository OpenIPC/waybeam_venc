#include "detect_dequant.h"

#include <string.h>

float detect_dequant_affine(long long q, float scalar, long long zero_pnt)
{
	return (float)(q - zero_pnt) * scalar;
}

int detect_dequant_buffer(const void *src, IpuFmt fmt, float scalar,
	long long zero_pnt, float *dst, size_t count)
{
	const unsigned char *bytes = (const unsigned char *)src;
	size_t i;

	switch (fmt) {
	case IPU_FMT_U8:
		for (i = 0; i < count; i++)
			dst[i] = detect_dequant_affine(bytes[i], scalar, zero_pnt);
		return 0;
	case IPU_FMT_INT8:
		for (i = 0; i < count; i++)
			dst[i] = detect_dequant_affine((signed char)bytes[i],
				scalar, zero_pnt);
		return 0;
	case IPU_FMT_INT16:
		for (i = 0; i < count; i++) {
			short v;
			memcpy(&v, bytes + i * sizeof(v), sizeof(v));
			dst[i] = detect_dequant_affine(v, scalar, zero_pnt);
		}
		return 0;
	case IPU_FMT_INT32:
		for (i = 0; i < count; i++) {
			int v;
			memcpy(&v, bytes + i * sizeof(v), sizeof(v));
			dst[i] = detect_dequant_affine(v, scalar, zero_pnt);
		}
		return 0;
	case IPU_FMT_FP32:
		for (i = 0; i < count; i++) {
			float v;
			memcpy(&v, bytes + i * sizeof(v), sizeof(v));
			dst[i] = v;
		}
		return 0;
	case IPU_FMT_NV12:
	case IPU_FMT_UNKNOWN:
	case IPU_FMT_ARGB8888:
	case IPU_FMT_ABGR8888:
		break;
	}
	return -1;
}
