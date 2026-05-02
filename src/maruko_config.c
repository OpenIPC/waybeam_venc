#include "maruko_config.h"

#include "codec_config.h"
#include "pipeline_common.h"
#include "rtp_packetizer.h"

#include <stdio.h>
#include <string.h>

void maruko_config_defaults(MarukoBackendConfig *cfg)
{
	if (!cfg) {
		return;
	}

	memset(cfg, 0, sizeof(*cfg));
	cfg->sensor_width = 1920;
	cfg->sensor_height = 1080;
	cfg->image_width = 1920;
	cfg->image_height = 1080;
	cfg->sensor_fps = 30;
	cfg->venc_max_rate = 8192;
	cfg->venc_gop_size = 30;
	cfg->venc_gop_seconds = 1.0;
	cfg->max_frame_size = RTP_DEFAULT_PAYLOAD;
	cfg->output_uri.type = VENC_OUTPUT_URI_UDP;
	snprintf(cfg->output_uri.host, sizeof(cfg->output_uri.host), "%s",
		"127.0.0.1");
	cfg->output_uri.port = 5000;
	cfg->rc_codec = PT_H265;
	cfg->rc_mode = 3;
	cfg->stream_mode = MARUKO_STREAM_RTP;
	cfg->forced_sensor_pad = (MI_SNR_PAD_ID_e)-1;
	cfg->forced_sensor_mode = -1;
	cfg->isp_bin_path[0] = '\0';
	cfg->vpe_level_3dnr = 1;
	cfg->verbose = 0;
	cfg->connected_udp = 1;  /* match VencConfig default */
	cfg->keep_aspect = 1;    /* match VencConfig default (true) */
	cfg->show_osd = 0;
	cfg->ae_fps = 15;
	cfg->isp_gain_max = 0;
	snprintf(cfg->ae_mode, sizeof(cfg->ae_mode), "%s", "native");
	memset(&cfg->imu, 0, sizeof(cfg->imu));
}

int maruko_config_from_venc(const VencConfig *vcfg, MarukoBackendConfig *cfg)
{
	if (!vcfg || !cfg) {
		return -1;
	}

	maruko_config_defaults(cfg);

	cfg->sensor_width = vcfg->video0.width;
	cfg->sensor_height = vcfg->video0.height;
	cfg->image_width = vcfg->video0.width;
	cfg->image_height = vcfg->video0.height;
	cfg->sensor_fps = vcfg->video0.fps;
	cfg->venc_max_rate = vcfg->video0.bitrate;
	cfg->venc_gop_seconds = vcfg->video0.gop_size;
	cfg->venc_gop_size = pipeline_common_gop_frames(cfg->venc_gop_seconds,
		cfg->sensor_fps);

	cfg->max_frame_size = vcfg->outgoing.max_payload_size;
	cfg->rtp_payload_size = vcfg->outgoing.max_payload_size;
	cfg->sidecar_port = vcfg->outgoing.sidecar_port;

	if (vcfg->outgoing.server[0] &&
	    venc_config_parse_output_uri(vcfg->outgoing.server,
	    &cfg->output_uri) != 0)
		return -1;

	cfg->stream_mode =
		(strcmp(vcfg->outgoing.stream_mode, "compact") == 0) ?
		MARUKO_STREAM_COMPACT : MARUKO_STREAM_RTP;

	if (codec_config_resolve_codec_rc(vcfg->video0.codec, vcfg->video0.rc_mode,
	    &cfg->rc_codec, &cfg->rc_mode) != 0) {
		return -1;
	}

	cfg->forced_sensor_pad = (vcfg->sensor.index >= 0) ?
		(MI_SNR_PAD_ID_e)vcfg->sensor.index : (MI_SNR_PAD_ID_e)-1;
	cfg->forced_sensor_mode = vcfg->sensor.mode;
	/* Configured path is copied verbatim; pipeline resolves it after
	 * sensor_select runs (may pick the per-sensor fallback). */
	snprintf(cfg->isp_bin_path, sizeof(cfg->isp_bin_path), "%s",
		vcfg->isp.sensor_bin);
	cfg->vpe_level_3dnr = vcfg->fpv.noise_level;
	cfg->sensor_unlock = (SensorUnlockConfig){
		.enabled = vcfg->sensor.unlock_enabled ? 1 : 0,
		.cmd_id = vcfg->sensor.unlock_cmd,
		.reg = vcfg->sensor.unlock_reg,
		.value = vcfg->sensor.unlock_value,
		.dir = (MI_SNR_CustDir_e)vcfg->sensor.unlock_dir,
	};
	cfg->oc_level = vcfg->system.overclock_level;
	cfg->scene_threshold = vcfg->video0.scene_threshold;
	cfg->scene_holdoff = vcfg->video0.scene_holdoff;
	cfg->frame_lost = vcfg->video0.frame_lost ? 1 : 0;
	cfg->verbose = vcfg->system.verbose ? 1 : 0;
	cfg->connected_udp = vcfg->outgoing.connected_udp ? 1 : 0;
	cfg->keep_aspect = vcfg->isp.keep_aspect ? 1 : 0;
	cfg->show_osd = vcfg->debug.show_osd ? 1 : 0;
	cfg->ae_fps = vcfg->isp.ae_fps;
	cfg->isp_gain_max = vcfg->isp.gain_max;
	snprintf(cfg->ae_mode, sizeof(cfg->ae_mode), "%s",
		vcfg->isp.ae_mode[0] ? vcfg->isp.ae_mode : "native");
	cfg->imu = vcfg->imu;

	return 0;
}
