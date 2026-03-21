#include "maruko_config.h"
#include "test_helpers.h"
#include "venc_config.h"

#include <arpa/inet.h>
#include <string.h>

int test_maruko_config(void)
{
	int failures = 0;
	MarukoBackendConfig cfg;
	VencConfig vcfg;

	memset(&cfg, 0xA5, sizeof(cfg));
	maruko_config_defaults(&cfg);
	CHECK("maruko defaults width", cfg.sensor_width == 1920);
	CHECK("maruko defaults height", cfg.sensor_height == 1080);
	CHECK("maruko defaults image width", cfg.image_width == 1920);
	CHECK("maruko defaults image height", cfg.image_height == 1080);
	CHECK("maruko defaults fps", cfg.sensor_fps == 30);
	CHECK("maruko defaults bitrate", cfg.venc_max_rate == 8192);
	CHECK("maruko defaults gop", cfg.venc_gop_size == 30);
	CHECK("maruko defaults payload", cfg.max_frame_size == 1400);
	CHECK("maruko defaults codec", cfg.rc_codec == PT_H265);
	CHECK("maruko defaults rc mode", cfg.rc_mode == 3);
	CHECK("maruko defaults stream mode", cfg.stream_mode == MARUKO_STREAM_RTP);
	CHECK("maruko defaults forced pad", cfg.forced_sensor_pad == (MI_SNR_PAD_ID_e)-1);
	CHECK("maruko defaults forced mode", cfg.forced_sensor_mode == -1);
	CHECK("maruko defaults 3dnr", cfg.vpe_level_3dnr == 1);
	CHECK("maruko defaults verbose", cfg.verbose == 0);

	venc_config_defaults(&vcfg);
	vcfg.video0.width = 2688;
	vcfg.video0.height = 1520;
	vcfg.video0.fps = 90;
	vcfg.video0.bitrate = 6000;
	vcfg.video0.gop_size = 1.5;
	strcpy(vcfg.video0.codec, "h264");
	strcpy(vcfg.video0.rc_mode, "vbr");
	strcpy(vcfg.outgoing.server, "udp://192.168.2.20:5602");
	strcpy(vcfg.outgoing.stream_mode, "compact");
	vcfg.outgoing.max_payload_size = 900;
	vcfg.sensor.index = 2;
	vcfg.sensor.mode = 3;
	strcpy(vcfg.isp.sensor_bin, "/etc/sensors/imx415.bin");
	vcfg.fpv.noise_level = 4;
	vcfg.system.verbose = true;

	CHECK("maruko config from venc ok", maruko_config_from_venc(&vcfg, &cfg) == 0);
	CHECK("maruko config width", cfg.sensor_width == 2688);
	CHECK("maruko config height", cfg.sensor_height == 1520);
	CHECK("maruko config fps", cfg.sensor_fps == 90);
	CHECK("maruko config bitrate", cfg.venc_max_rate == 6000);
	CHECK("maruko config gop", cfg.venc_gop_size == 135);
	CHECK("maruko config gop seconds", cfg.venc_gop_seconds == 1.5);
	CHECK("maruko config payload", cfg.rtp_payload_size == 900);
	CHECK("maruko config sink ip",
		cfg.udp_sink_ip == inet_addr("192.168.2.20"));
	CHECK("maruko config sink port", cfg.udp_sink_port == 5602);
	CHECK("maruko config codec", cfg.rc_codec == PT_H264);
	CHECK("maruko config rc mode", cfg.rc_mode == 2);
	CHECK("maruko config stream mode", cfg.stream_mode == MARUKO_STREAM_COMPACT);
	CHECK("maruko config forced pad", cfg.forced_sensor_pad == (MI_SNR_PAD_ID_e)2);
	CHECK("maruko config forced mode", cfg.forced_sensor_mode == 3);
	CHECK("maruko config isp bin", strcmp(cfg.isp_bin_path, "/etc/sensors/imx415.bin") == 0);
	CHECK("maruko config 3dnr", cfg.vpe_level_3dnr == 4);
	CHECK("maruko config verbose", cfg.verbose == 1);

	strcpy(vcfg.outgoing.server, "bad://server");
	CHECK("maruko config bad uri fails", maruko_config_from_venc(&vcfg, &cfg) != 0);

	strcpy(vcfg.outgoing.server, "udp://192.168.2.20:5602");
	strcpy(vcfg.video0.codec, "jpeg");
	CHECK("maruko config bad codec fails", maruko_config_from_venc(&vcfg, &cfg) != 0);

	CHECK("maruko config null vcfg fails", maruko_config_from_venc(NULL, &cfg) != 0);
	CHECK("maruko config null cfg fails", maruko_config_from_venc(&vcfg, NULL) != 0);

	return failures;
}
