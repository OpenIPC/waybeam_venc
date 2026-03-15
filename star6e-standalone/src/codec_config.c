#include "codec_config.h"

#include <stdio.h>
#include <string.h>

int codec_config_resolve_codec_rc(const char *codec, const char *mode,
	PAYLOAD_TYPE_E *out_codec, int *out_rc_mode)
{
	int is_265;
	int is_264;

	if (!codec || !mode || !out_codec || !out_rc_mode) {
		fprintf(stderr, "ERROR: invalid codec/rcMode arguments\n");
		return -1;
	}

	is_265 = (!strcmp(codec, "h265") || !strcmp(codec, "265"));
	is_264 = (!strcmp(codec, "h264") || !strcmp(codec, "264"));

	if (!is_265 && !is_264) {
		fprintf(stderr, "ERROR: unsupported video0.codec '%s'\n", codec);
		return -1;
	}
	*out_codec = is_265 ? PT_H265 : PT_H264;

	if (!strcmp(mode, "cbr")) {
		*out_rc_mode = 3;
	} else if (!strcmp(mode, "vbr")) {
		*out_rc_mode = is_265 ? 4 : 2;
	} else if (!strcmp(mode, "avbr")) {
		*out_rc_mode = is_265 ? 5 : 0;
	} else if (!strcmp(mode, "qvbr")) {
		*out_rc_mode = is_265 ? 6 : 1;
	} else {
		fprintf(stderr, "ERROR: unsupported video0.rcMode '%s'\n", mode);
		return -1;
	}
	return 0;
}
