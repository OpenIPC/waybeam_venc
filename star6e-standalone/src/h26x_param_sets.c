#include "h26x_param_sets.h"

#include <string.h>

static void h26x_param_sets_store(uint8_t *dst, size_t *dst_len,
	size_t dst_cap, const uint8_t *src, size_t src_len)
{
	if (!dst || !dst_len || !src || src_len == 0 || src_len >= dst_cap)
		return;

	memcpy(dst, src, src_len);
	*dst_len = src_len;
}

void h26x_param_sets_update(H26xParamSets *sets, PAYLOAD_TYPE_E codec,
	uint8_t nal_type, const uint8_t *nal, size_t nal_len)
{
	if (!sets || !nal || nal_len == 0)
		return;

	if (codec == PT_H265) {
		if (nal_type == 32)
			h26x_param_sets_store(sets->vps, &sets->vps_len,
				sizeof(sets->vps), nal, nal_len);
		else if (nal_type == 33)
			h26x_param_sets_store(sets->sps, &sets->sps_len,
				sizeof(sets->sps), nal, nal_len);
		else if (nal_type == 34)
			h26x_param_sets_store(sets->pps, &sets->pps_len,
				sizeof(sets->pps), nal, nal_len);
		return;
	}

	if (codec == PT_H264) {
		if (nal_type == 7)
			h26x_param_sets_store(sets->sps, &sets->sps_len,
				sizeof(sets->sps), nal, nal_len);
		else if (nal_type == 8)
			h26x_param_sets_store(sets->pps, &sets->pps_len,
				sizeof(sets->pps), nal, nal_len);
	}
}

size_t h26x_param_sets_get_prepend(const H26xParamSets *sets,
	PAYLOAD_TYPE_E codec, uint8_t nal_type, H26xParamSetRef *refs,
	size_t max_refs)
{
	size_t count = 0;

	if (!sets || !refs || max_refs == 0)
		return 0;

	if (codec == PT_H265 && (nal_type == 19 || nal_type == 20)) {
		if (sets->vps_len && count < max_refs) {
			refs[count].data = sets->vps;
			refs[count].len = sets->vps_len;
			count++;
		}
		if (sets->sps_len && count < max_refs) {
			refs[count].data = sets->sps;
			refs[count].len = sets->sps_len;
			count++;
		}
		if (sets->pps_len && count < max_refs) {
			refs[count].data = sets->pps;
			refs[count].len = sets->pps_len;
			count++;
		}
		return count;
	}

	if (codec == PT_H264 && nal_type == 5) {
		if (sets->sps_len && count < max_refs) {
			refs[count].data = sets->sps;
			refs[count].len = sets->sps_len;
			count++;
		}
		if (sets->pps_len && count < max_refs) {
			refs[count].data = sets->pps;
			refs[count].len = sets->pps_len;
			count++;
		}
	}

	return count;
}
