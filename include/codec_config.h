#ifndef CODEC_CONFIG_H
#define CODEC_CONFIG_H

#include "codec_types.h"

/** Resolve the rc-mode string into the backend RC enum.
 *  Video codec is always H.265 (out_codec = PT_H265). */
int codec_config_resolve_codec_rc(const char *mode,
	PAYLOAD_TYPE_E *out_codec, int *out_rc_mode);

#endif /* CODEC_CONFIG_H */
