#ifndef MARUKO_CONFIG_H
#define MARUKO_CONFIG_H

#include "codec_types.h"
#include "sensor_select.h"
#include "star6e.h"
#include "venc_config.h"

#include <stdint.h>

typedef enum {
	MARUKO_STREAM_COMPACT = 0,
	MARUKO_STREAM_RTP = 1,
} MarukoStreamMode;

typedef struct {
	uint32_t sensor_width;
	uint32_t sensor_height;
	uint32_t image_width;
	uint32_t image_height;
	uint32_t sensor_fps;
	uint32_t venc_max_rate;
	uint32_t venc_gop_size;
	double venc_gop_seconds;
	uint16_t max_frame_size;
	uint16_t rtp_payload_size;
	VencOutputUri output_uri;
	uint16_t sidecar_port;
	PAYLOAD_TYPE_E rc_codec;
	int rc_mode;
	MarukoStreamMode stream_mode;
	MI_SNR_PAD_ID_e forced_sensor_pad;
	int forced_sensor_mode;
	/* Configured isp.sensorBin path; resolved (with sensor-name fallback)
	 * by the pipeline once sensor_select has run.  Empty string means
	 * "use default fallback". */
	char isp_bin_path[256];
	int vpe_level_3dnr;
	SensorUnlockConfig sensor_unlock;
	int oc_level;
	uint16_t scene_threshold;
	uint8_t scene_holdoff;
	uint8_t frame_lost;
	int verbose;
	int connected_udp;
	int keep_aspect;
} MarukoBackendConfig;

/** Fill config with compiled-in defaults for Maruko backend. */
void maruko_config_defaults(MarukoBackendConfig *cfg);

/** Convert generic VencConfig into Maruko-specific backend config. */
int maruko_config_from_venc(const VencConfig *vcfg, MarukoBackendConfig *cfg);

#endif /* MARUKO_CONFIG_H */
