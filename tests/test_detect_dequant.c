#include "test_helpers.h"

#include "detect_dequant.h"

#include <math.h>
#include <string.h>

static int close_to(float a, float b)
{
	return fabsf(a - b) < 1e-4f;
}

int test_detect_dequant(void)
{
	int failures = 0;

	/* Affine: real = (q - zero) * scalar */
	CHECK("affine_basic",
		close_to(detect_dequant_affine(10, 0.5f, 2), 4.0f));
	CHECK("affine_zero_point_negative",
		close_to(detect_dequant_affine(0, 2.0f, -3), 6.0f));
	CHECK("affine_identity_scale",
		close_to(detect_dequant_affine(7, 1.0f, 0), 7.0f));

	/* U8 buffer */
	{
		unsigned char src[4] = { 0, 128, 255, 64 };
		float dst[4];
		int rc = detect_dequant_buffer(src, IPU_FMT_U8, 0.25f, 128,
			dst, 4);
		CHECK("u8_rc", rc == 0);
		CHECK("u8_v0", close_to(dst[0], (0 - 128) * 0.25f));
		CHECK("u8_v1", close_to(dst[1], 0.0f));
		CHECK("u8_v2", close_to(dst[2], (255 - 128) * 0.25f));
	}

	/* INT8 buffer — signedness matters */
	{
		unsigned char src[3];
		float dst[3];
		int rc;
		signed char s0 = -128, s1 = 127, s2 = -1;
		memcpy(&src[0], &s0, 1);
		memcpy(&src[1], &s1, 1);
		memcpy(&src[2], &s2, 1);
		rc = detect_dequant_buffer(src, IPU_FMT_INT8, 1.0f, 0, dst, 3);
		CHECK("int8_rc", rc == 0);
		CHECK("int8_v0", close_to(dst[0], -128.0f));
		CHECK("int8_v1", close_to(dst[1], 127.0f));
		CHECK("int8_v2", close_to(dst[2], -1.0f));
	}

	/* INT16 buffer */
	{
		short vals[2] = { 1000, -1000 };
		unsigned char src[sizeof(vals)];
		float dst[2];
		int rc;
		memcpy(src, vals, sizeof(vals));
		rc = detect_dequant_buffer(src, IPU_FMT_INT16, 0.01f, 0, dst, 2);
		CHECK("int16_rc", rc == 0);
		CHECK("int16_v0", close_to(dst[0], 10.0f));
		CHECK("int16_v1", close_to(dst[1], -10.0f));
	}

	/* FP32 buffer — passthrough, scalar/zero ignored */
	{
		float vals[2] = { 3.5f, -2.25f };
		unsigned char src[sizeof(vals)];
		float dst[2];
		int rc;
		memcpy(src, vals, sizeof(vals));
		rc = detect_dequant_buffer(src, IPU_FMT_FP32, 99.0f, 99, dst, 2);
		CHECK("fp32_rc", rc == 0);
		CHECK("fp32_v0", close_to(dst[0], 3.5f));
		CHECK("fp32_v1", close_to(dst[1], -2.25f));
	}

	/* Non-dequantizable formats are rejected */
	{
		unsigned char src[4] = { 0, 0, 0, 0 };
		float dst[4];
		CHECK("nv12_rejected",
			detect_dequant_buffer(src, IPU_FMT_NV12, 1.0f, 0, dst, 4)
				== -1);
		CHECK("argb_rejected",
			detect_dequant_buffer(src, IPU_FMT_ARGB8888, 1.0f, 0,
				dst, 1) == -1);
	}

	return failures;
}
