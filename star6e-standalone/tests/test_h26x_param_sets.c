#include "h26x_param_sets.h"
#include "test_helpers.h"

#include <string.h>

static int test_h26x_param_sets_update_hevc(void)
{
	int failures = 0;
	H26xParamSets sets = {0};
	const uint8_t vps[] = {0x40, 0x01, 0xAA};
	const uint8_t sps[] = {0x42, 0x01, 0xBB, 0xCC};
	const uint8_t pps[] = {0x44, 0x01, 0xDD};

	h26x_param_sets_update(&sets, PT_H265, 32, vps, sizeof(vps));
	h26x_param_sets_update(&sets, PT_H265, 33, sps, sizeof(sps));
	h26x_param_sets_update(&sets, PT_H265, 34, pps, sizeof(pps));

	CHECK("h26x_param_sets_hevc_vps",
		sets.vps_len == sizeof(vps) &&
		memcmp(sets.vps, vps, sizeof(vps)) == 0);
	CHECK("h26x_param_sets_hevc_sps",
		sets.sps_len == sizeof(sps) &&
		memcmp(sets.sps, sps, sizeof(sps)) == 0);
	CHECK("h26x_param_sets_hevc_pps",
		sets.pps_len == sizeof(pps) &&
		memcmp(sets.pps, pps, sizeof(pps)) == 0);

	return failures;
}

static int test_h26x_param_sets_update_h264(void)
{
	int failures = 0;
	H26xParamSets sets = {0};
	const uint8_t sps[] = {0x67, 0x64, 0x00};
	const uint8_t pps[] = {0x68, 0xEE};

	h26x_param_sets_update(&sets, PT_H264, 7, sps, sizeof(sps));
	h26x_param_sets_update(&sets, PT_H264, 8, pps, sizeof(pps));

	CHECK("h26x_param_sets_h264_sps",
		sets.sps_len == sizeof(sps) &&
		memcmp(sets.sps, sps, sizeof(sps)) == 0);
	CHECK("h26x_param_sets_h264_pps",
		sets.pps_len == sizeof(pps) &&
		memcmp(sets.pps, pps, sizeof(pps)) == 0);
	CHECK("h26x_param_sets_h264_vps_untouched", sets.vps_len == 0);

	return failures;
}

static int test_h26x_param_sets_oversize_ignored(void)
{
	int failures = 0;
	H26xParamSets sets = {0};
	uint8_t oversize[512];

	memset(oversize, 0xAB, sizeof(oversize));
	h26x_param_sets_update(&sets, PT_H265, 32, oversize, sizeof(oversize));
	h26x_param_sets_update(&sets, PT_H264, 7, oversize, sizeof(oversize));

	CHECK("h26x_param_sets_oversize_hevc_vps", sets.vps_len == 0);
	CHECK("h26x_param_sets_oversize_h264_sps", sets.sps_len == 0);

	return failures;
}

static int test_h26x_param_sets_prepend_hevc(void)
{
	int failures = 0;
	H26xParamSets sets = {0};
	H26xParamSetRef refs[3];
	const uint8_t vps[] = {0x40, 0x01, 0xAA};
	const uint8_t sps[] = {0x42, 0x01, 0xBB};
	const uint8_t pps[] = {0x44, 0x01, 0xCC};
	size_t count;

	h26x_param_sets_update(&sets, PT_H265, 32, vps, sizeof(vps));
	h26x_param_sets_update(&sets, PT_H265, 33, sps, sizeof(sps));
	h26x_param_sets_update(&sets, PT_H265, 34, pps, sizeof(pps));
	count = h26x_param_sets_get_prepend(&sets, PT_H265, 19, refs,
		sizeof(refs) / sizeof(refs[0]));

	CHECK("h26x_param_sets_prepend_hevc_count", count == 3);
	CHECK("h26x_param_sets_prepend_hevc_order",
		refs[0].len == sizeof(vps) &&
		refs[1].len == sizeof(sps) &&
		refs[2].len == sizeof(pps) &&
		memcmp(refs[0].data, vps, sizeof(vps)) == 0 &&
		memcmp(refs[1].data, sps, sizeof(sps)) == 0 &&
		memcmp(refs[2].data, pps, sizeof(pps)) == 0);

	return failures;
}

static int test_h26x_param_sets_prepend_h264(void)
{
	int failures = 0;
	H26xParamSets sets = {0};
	H26xParamSetRef refs[2];
	const uint8_t sps[] = {0x67, 0x64, 0x00};
	const uint8_t pps[] = {0x68, 0xEE};
	size_t count;

	h26x_param_sets_update(&sets, PT_H264, 7, sps, sizeof(sps));
	h26x_param_sets_update(&sets, PT_H264, 8, pps, sizeof(pps));
	count = h26x_param_sets_get_prepend(&sets, PT_H264, 5, refs,
		sizeof(refs) / sizeof(refs[0]));

	CHECK("h26x_param_sets_prepend_h264_count", count == 2);
	CHECK("h26x_param_sets_prepend_h264_order",
		refs[0].len == sizeof(sps) &&
		refs[1].len == sizeof(pps) &&
		memcmp(refs[0].data, sps, sizeof(sps)) == 0 &&
		memcmp(refs[1].data, pps, sizeof(pps)) == 0);

	return failures;
}

static int test_h26x_param_sets_prepend_non_idr(void)
{
	int failures = 0;
	H26xParamSets sets = {0};
	H26xParamSetRef refs[3];
	const uint8_t sps[] = {0x67, 0x64, 0x00};

	h26x_param_sets_update(&sets, PT_H264, 7, sps, sizeof(sps));

	CHECK("h26x_param_sets_prepend_non_idr",
		h26x_param_sets_get_prepend(&sets, PT_H264, 1, refs,
			sizeof(refs) / sizeof(refs[0])) == 0);
	CHECK("h26x_param_sets_prepend_null_safe",
		h26x_param_sets_get_prepend(NULL, PT_H265, 19, refs,
			sizeof(refs) / sizeof(refs[0])) == 0);

	return failures;
}

int test_h26x_param_sets(void)
{
	int failures = 0;
	H26xParamSets sets = {0};
	const uint8_t nal[] = {0x01};

	h26x_param_sets_update(NULL, PT_H265, 32, nal, sizeof(nal));
	h26x_param_sets_update(&sets, PT_H265, 32, NULL, sizeof(nal));
	CHECK("h26x_param_sets_null_safe", sets.vps_len == 0);

	failures += test_h26x_param_sets_update_hevc();
	failures += test_h26x_param_sets_update_h264();
	failures += test_h26x_param_sets_oversize_ignored();
	failures += test_h26x_param_sets_prepend_hevc();
	failures += test_h26x_param_sets_prepend_h264();
	failures += test_h26x_param_sets_prepend_non_idr();

	return failures;
}
