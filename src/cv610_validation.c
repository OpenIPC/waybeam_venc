#include "venc_api.h"

#include <math.h>
#include <string.h>

/* Validate the deliberately narrow CV610 feature set for both startup config
 * loading and staged HTTP mutations. */
const char *cv610_validate_config(const VencConfig *cfg)
{
	VencOutputUri uri;

	if (!cfg)
		return "missing config";
	if ((cfg->video0.width != 0 || cfg->video0.height != 0) &&
		(cfg->video0.width != 1920 || cfg->video0.height != 1080))
		return "CV610 currently requires video0 1920x1080";
	if (cfg->video0.fps != 30 && cfg->video0.fps != 60 &&
		cfg->video0.fps != 90 && cfg->video0.fps != 100)
		return "CV610 video0.fps must be 30, 60, 90, or 100";
	if (cfg->video0.bitrate < VENC_BITRATE_MIN_KBPS ||
		cfg->video0.bitrate > VENC_BITRATE_MAX_KBPS)
		return "video0.bitrate is outside the supported range";
	if (cfg->video0.qp_delta < -10 || cfg->video0.qp_delta > 30)
		return "CV610 video0.qp_delta must be between -10 and 30";
	if (!isfinite(cfg->video0.gop_size) || cfg->video0.gop_size < 0.0 ||
		cfg->video0.gop_size * cfg->video0.fps > 65536.0)
		return "CV610 video0.gop_size exceeds the encoder's 65536-frame limit";
	if (strcmp(cfg->video0.rc_mode, "cbr") != 0)
		return "CV610 phase one supports video0.rc_mode=cbr only";
	if (strcmp(cfg->video0.resilience, "off") != 0)
		return "CV610 phase one supports video0.resilience=off only";
	if (strcmp(cfg->video0.framing, "off") != 0)
		return "CV610 phase one supports video0.framing=off only";
	if (cfg->audio.enabled &&
		(cfg->audio.sample_rate != 48000 || cfg->audio.channels != 1 ||
		 strcmp(cfg->audio.codec, "opus") != 0))
		return "CV610 audio requires 48000 Hz, mono, Opus";
	if (cfg->audio.enabled && !cfg->outgoing.enabled)
		return "CV610 audio requires outgoing output to be enabled";
	if (cfg->audio.enabled &&
		(cfg->outgoing.audio_port < 0 || cfg->outgoing.audio_port > 65535))
		return "CV610 audio requires outgoing.audio_port in range 0..65535";
	if (cfg->outgoing.max_payload_size < VENC_OUTPUT_PAYLOAD_MIN_BYTES ||
		cfg->outgoing.max_payload_size > VENC_OUTPUT_PAYLOAD_CEILING_BYTES)
		return "outgoing.max_payload_size is outside the supported range";
	if (!cfg->outgoing.enabled)
		return NULL;
	if (strcmp(cfg->outgoing.stream_mode, "rtp") != 0)
		return "CV610 phase one supports outgoing.stream_mode=rtp only";
	if (venc_config_parse_output_uri(cfg->outgoing.server, &uri) != 0)
		return "invalid outgoing.server URI";
	if (uri.type == VENC_OUTPUT_URI_SHM)
		return "CV610 phase one supports udp://, unix://, or frame-shm:// output";
	/* unix:// and frame-shm:// are local video transports. Their positive
	 * audio_port intentionally selects the Waybeam Link loopback UDP side
	 * channel; port 0 can inherit a destination only from UDP video. */
	if (cfg->audio.enabled && cfg->outgoing.audio_port == 0 &&
		uri.type != VENC_OUTPUT_URI_UDP)
		return "CV610 audio port 0 requires a UDP video destination";
	return NULL;
}

#ifndef HAVE_CV610_HTTP_API
const char *venc_api_validate_loaded_config(const VencConfig *cfg)
{
	return cv610_validate_config(cfg);
}
#endif
