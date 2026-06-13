#include "h26x_util.h"

void h26x_util_strip_start_code(const uint8_t **data, size_t *length)
{
	const uint8_t *ptr;
	size_t len;

	if (!data || !*data || !length) {
		return;
	}

	ptr = *data;
	len = *length;

	while (len >= 4 && ptr[0] == 0x00 && ptr[1] == 0x00 &&
		ptr[2] == 0x00 && ptr[3] == 0x01) {
		ptr += 4;
		len -= 4;
	}
	while (len >= 3 && ptr[0] == 0x00 && ptr[1] == 0x00 &&
		ptr[2] == 0x01) {
		ptr += 3;
		len -= 3;
	}

	*data = ptr;
	*length = len;
}

uint8_t h26x_util_h264_nalu_type(const uint8_t *data, size_t len)
{
	if (!data || len == 0) {
		return 0;
	}
	return (uint8_t)(data[0] & 0x1F);
}

uint8_t h26x_util_hevc_nalu_type(const uint8_t *data, size_t len)
{
	if (!data || len == 0) {
		return 0;
	}
	return (uint8_t)((data[0] >> 1) & 0x3F);
}
