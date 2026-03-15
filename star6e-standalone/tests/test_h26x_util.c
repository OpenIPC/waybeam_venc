#include <stddef.h>
#include <stdint.h>

#include "h26x_util.h"
#include "test_helpers.h"

int test_h26x_util(void)
{
	int failures = 0;

	const uint8_t three_byte_start_code[] = {0x00, 0x00, 0x01, 0x65, 0xAA};
	const uint8_t four_byte_start_code[] = {0x00, 0x00, 0x00, 0x01, 0x42, 0x01};
	const uint8_t no_start_code[] = {0x26, 0x01};

	const uint8_t *ptr = three_byte_start_code;
	size_t len = sizeof(three_byte_start_code);
	h26x_util_strip_start_code(&ptr, &len);
	CHECK("h26x_strip_three_ptr", ptr == &three_byte_start_code[3]);
	CHECK("h26x_strip_three_len", len == 2);

	ptr = four_byte_start_code;
	len = sizeof(four_byte_start_code);
	h26x_util_strip_start_code(&ptr, &len);
	CHECK("h26x_strip_four_ptr", ptr == &four_byte_start_code[4]);
	CHECK("h26x_strip_four_len", len == 2);

	ptr = no_start_code;
	len = sizeof(no_start_code);
	h26x_util_strip_start_code(&ptr, &len);
	CHECK("h26x_strip_none_ptr", ptr == no_start_code);
	CHECK("h26x_strip_none_len", len == sizeof(no_start_code));

	CHECK("h26x_h264_type", h26x_util_h264_nalu_type((const uint8_t[]){0x65}, 1) == 5);
	CHECK("h26x_h264_null", h26x_util_h264_nalu_type(NULL, 0) == 0);
	CHECK("h26x_hevc_type", h26x_util_hevc_nalu_type((const uint8_t[]){0x26, 0x01}, 2) == 19);
	CHECK("h26x_hevc_null", h26x_util_hevc_nalu_type(NULL, 0) == 0);
	CHECK("h26x_hevc_layer_id",
		h26x_util_hevc_get_layer_id((const uint8_t[]){0x03, 0x29}, 2) == 37);
	CHECK("h26x_hevc_layer_id_short", h26x_util_hevc_get_layer_id(NULL, 0) == 0);
	CHECK("h26x_hevc_tid_plus1",
		h26x_util_hevc_get_tid_plus1((const uint8_t[]){0x03, 0x29}, 2) == 1);
	CHECK("h26x_hevc_tid_plus1_short",
		h26x_util_hevc_get_tid_plus1(NULL, 0) == 1);

	return failures;
}
