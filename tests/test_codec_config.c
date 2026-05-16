#include "codec_config.h"
#include "test_helpers.h"

/* Video codec is hardcoded H.265 — the helper only validates / maps
 * the rc-mode string now. */
int test_codec_config(void)
{
	int failures = 0;
	PAYLOAD_TYPE_E codec = PT_H264;
	int rc_mode = -1;

	CHECK("rc_cbr",
		codec_config_resolve_codec_rc("cbr", &codec, &rc_mode) == 0 &&
		codec == PT_H265 && rc_mode == 3);
	CHECK("rc_vbr",
		codec_config_resolve_codec_rc("vbr", &codec, &rc_mode) == 0 &&
		codec == PT_H265 && rc_mode == 4);
	CHECK("rc_avbr",
		codec_config_resolve_codec_rc("avbr", &codec, &rc_mode) == 0 &&
		codec == PT_H265 && rc_mode == 5);
	CHECK("rc_qvbr",
		codec_config_resolve_codec_rc("qvbr", &codec, &rc_mode) == 0 &&
		codec == PT_H265 && rc_mode == 6);
	CHECK("rc_invalid_mode",
		codec_config_resolve_codec_rc("bogus", &codec, &rc_mode) != 0);
	CHECK("rc_null_mode",
		codec_config_resolve_codec_rc(NULL, &codec, &rc_mode) != 0);
	CHECK("rc_null_out_codec",
		codec_config_resolve_codec_rc("cbr", NULL, &rc_mode) != 0);
	CHECK("rc_null_out_rc_mode",
		codec_config_resolve_codec_rc("cbr", &codec, NULL) != 0);

	return failures;
}
