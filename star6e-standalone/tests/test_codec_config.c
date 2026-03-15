#include "codec_config.h"
#include "test_helpers.h"

int test_codec_config(void)
{
	int failures = 0;
	PAYLOAD_TYPE_E codec = PT_H264;
	int rc_mode = -1;

	CHECK("codec_h265_cbr",
		codec_config_resolve_codec_rc("h265", "cbr", &codec, &rc_mode) == 0 &&
		codec == PT_H265 && rc_mode == 3);
	CHECK("codec_265_qvbr",
		codec_config_resolve_codec_rc("265", "qvbr", &codec, &rc_mode) == 0 &&
		codec == PT_H265 && rc_mode == 6);
	CHECK("codec_h264_vbr",
		codec_config_resolve_codec_rc("h264", "vbr", &codec, &rc_mode) == 0 &&
		codec == PT_H264 && rc_mode == 2);
	CHECK("codec_264_avbr",
		codec_config_resolve_codec_rc("264", "avbr", &codec, &rc_mode) == 0 &&
		codec == PT_H264 && rc_mode == 0);
	CHECK("codec_invalid_codec",
		codec_config_resolve_codec_rc("jpeg", "cbr", &codec, &rc_mode) != 0);
	CHECK("codec_invalid_mode",
		codec_config_resolve_codec_rc("h264", "bogus", &codec, &rc_mode) != 0);
	CHECK("codec_null_codec",
		codec_config_resolve_codec_rc(NULL, "cbr", &codec, &rc_mode) != 0);
	CHECK("codec_null_mode",
		codec_config_resolve_codec_rc("h264", NULL, &codec, &rc_mode) != 0);
	CHECK("codec_null_out_codec",
		codec_config_resolve_codec_rc("h264", "cbr", NULL, &rc_mode) != 0);
	CHECK("codec_null_out_rc_mode",
		codec_config_resolve_codec_rc("h264", "cbr", &codec, NULL) != 0);

	return failures;
}
