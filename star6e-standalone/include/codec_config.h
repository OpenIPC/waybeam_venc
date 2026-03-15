#ifndef CODEC_CONFIG_H
#define CODEC_CONFIG_H

#include "codec_types.h"

/** Resolve codec and rc-mode strings into backend codec/RC enums. */
int codec_config_resolve_codec_rc(const char *codec, const char *mode,
	PAYLOAD_TYPE_E *out_codec, int *out_rc_mode);

#endif /* CODEC_CONFIG_H */
