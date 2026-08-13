#include "h26x_util.h"

static size_t annexb_find_start_code(const uint8_t *data, size_t len,
	size_t from, size_t *code_len)
{
	size_t i;

	for (i = from; i + 3 <= len; ++i) {
		if (data[i] != 0 || data[i + 1] != 0)
			continue;
		if (data[i + 2] == 1) {
			*code_len = 3;
			return i;
		}
		if (i + 4 <= len && data[i + 2] == 0 && data[i + 3] == 1) {
			*code_len = 4;
			return i;
		}
	}
	return len;
}

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

int h26x_util_annexb_next(const uint8_t *data, size_t len, size_t *cursor,
	const uint8_t **nal, size_t *nal_len)
{
	size_t pos;
	size_t code_len;

	if (!data || !cursor || !nal || !nal_len || *cursor > len)
		return 0;
	pos = *cursor;
	while ((pos = annexb_find_start_code(data, len, pos, &code_len)) < len) {
		size_t start = pos + code_len;
		size_t next_code_len;
		size_t next = annexb_find_start_code(data, len, start,
			&next_code_len);
		size_t payload_len = next - start;

		(void)next_code_len;
		*cursor = next;
		while (payload_len > 0 && data[start + payload_len - 1] == 0)
			payload_len--;
		if (payload_len > 0) {
			*nal = data + start;
			*nal_len = payload_len;
			return 1;
		}
		pos = next;
	}
	*cursor = len;
	return 0;
}
