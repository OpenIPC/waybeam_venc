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

	{
		const uint8_t access_unit[] = {
			0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0xaa,
			0x00, 0x00, 0x01, 0x26, 0x01, 0xbb, 0x00, 0x00
		};
		size_t cursor = 0;
		const uint8_t *nal = NULL;
		size_t nal_len = 0;

		CHECK("h26x_annexb_first", h26x_util_annexb_next(access_unit,
			sizeof(access_unit), &cursor, &nal, &nal_len) == 1);
		CHECK("h26x_annexb_first_ptr", nal == &access_unit[4]);
		CHECK("h26x_annexb_first_len", nal_len == 3);
		CHECK("h26x_annexb_second", h26x_util_annexb_next(access_unit,
			sizeof(access_unit), &cursor, &nal, &nal_len) == 1);
		CHECK("h26x_annexb_second_ptr", nal == &access_unit[10]);
		CHECK("h26x_annexb_second_len", nal_len == 3);
		CHECK("h26x_annexb_end", h26x_util_annexb_next(access_unit,
			sizeof(access_unit), &cursor, &nal, &nal_len) == 0);
		CHECK("h26x_annexb_bad_cursor", h26x_util_annexb_next(access_unit,
			sizeof(access_unit), &(size_t){sizeof(access_unit) + 1},
			&nal, &nal_len) == 0);
	}

	return failures;
}
