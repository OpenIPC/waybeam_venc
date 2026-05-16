#include "codec_config.h"

#include <stdio.h>
#include <string.h>

int codec_config_resolve_codec_rc(const char *mode,
	PAYLOAD_TYPE_E *out_codec, int *out_rc_mode)
{
	if (!mode || !out_codec || !out_rc_mode) {
		fprintf(stderr, "ERROR: invalid rcMode arguments\n");
		return -1;
	}

	*out_codec = PT_H265;

	if (!strcmp(mode, "cbr")) {
		*out_rc_mode = 3;
	} else if (!strcmp(mode, "vbr")) {
		*out_rc_mode = 4;
	} else if (!strcmp(mode, "avbr")) {
		*out_rc_mode = 5;
	} else if (!strcmp(mode, "qvbr")) {
		*out_rc_mode = 6;
	} else {
		fprintf(stderr, "ERROR: unsupported video0.rcMode '%s'\n", mode);
		return -1;
	}
	return 0;
}
