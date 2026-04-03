#include "star6e_video.h"

#include "test_helpers.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int create_udp_receiver(uint16_t *port)
{
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
	int socket_handle;

	socket_handle = socket(AF_INET, SOCK_DGRAM, 0);
	if (socket_handle < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(0);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(socket_handle, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(socket_handle);
		return -1;
	}
	if (getsockname(socket_handle, (struct sockaddr *)&addr, &addr_len) != 0) {
		close(socket_handle);
		return -1;
	}

	(void)setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		sizeof(timeout));
	*port = ntohs(addr.sin_port);
	return socket_handle;
}

static int reserve_udp_port(uint16_t *port)
{
	int socket_handle = create_udp_receiver(port);

	if (socket_handle < 0)
		return -1;
	close(socket_handle);
	return 0;
}

static int test_star6e_video_init_rtp_state(void)
{
	VencConfig cfg;
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eVideoState state = {0};
	int failures = 0;
	int ret;

	venc_config_defaults(&cfg);

	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600", "rtp", 1400, 0);
	CHECK("star6e video rtp prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e video rtp output init", ret == 0);

	star6e_video_init(&state, &cfg, 60, &output);
	CHECK("star6e video rtp max frame", state.max_frame_size == 1400);
	CHECK("star6e video rtp payload", state.rtp_payload_size == 1400);
	CHECK("star6e video rtp sensor fps", state.sensor_framerate == 60);
	CHECK("star6e video rtp frame ticks", state.rtp_frame_ticks == 1500);
	CHECK("star6e video rtp payload type", state.rtp_state.payload_type == 97);
	CHECK("star6e video rtp frame counter", state.frame_counter == 0);

	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_video_init_compact_state(void)
{
	VencConfig cfg;
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eVideoState state = {0};
	int failures = 0;
	int ret;

	venc_config_defaults(&cfg);
	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600", "compact", 1400, 0);
	CHECK("star6e video compact prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e video compact output init", ret == 0);

	star6e_video_init(&state, &cfg, 120, &output);
	CHECK("star6e video compact sensor fps", state.sensor_framerate == 120);
	CHECK("star6e video compact frame ticks", state.rtp_frame_ticks == 0);
	CHECK("star6e video compact payload type", state.rtp_state.payload_type == 0);
	CHECK("star6e video compact params zero", state.param_sets.vps_len == 0);

	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_video_send_frame_rtp(void)
{
	static const uint8_t nal[] = { 0x02, 0x01, 0xAA, 0xBB };
	VencConfig cfg;
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eVideoState state = {0};
	MI_VENC_Pack_t pack = {0};
	MI_VENC_Stream_t stream = {0};
	uint8_t buf[64];
	char uri[64];
	uint16_t seq_before;
	uint32_t ts_before;
	size_t total_bytes;
	ssize_t received;
	uint16_t port;
	int recv_socket;
	int failures = 0;
	int ret;

	venc_config_defaults(&cfg);

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e video send receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "rtp", 1400, 0);
	CHECK("star6e video send prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e video send output init", ret == 0);
	star6e_video_init(&state, &cfg, 30, &output);
	seq_before = state.rtp_state.seq;
	ts_before = state.rtp_state.timestamp;

	pack.data = (uint8_t *)nal;
	pack.length = sizeof(nal);
	pack.packNum = 1;
	pack.packetInfo[0].offset = 0;
	pack.packetInfo[0].length = sizeof(nal);
	pack.packetInfo[0].packType.h265Nalu = 1;
	stream.count = 1;
	stream.packet = &pack;

	total_bytes = star6e_video_send_frame(&state, &output, &stream, 1, 0, NULL);
	CHECK("star6e video send bytes", total_bytes == sizeof(nal));
	CHECK("star6e video send frame counter", state.frame_counter == 1);
	CHECK("star6e video send seq advanced",
		state.rtp_state.seq == (uint16_t)(seq_before + 1));
	CHECK("star6e video send timestamp advanced",
		state.rtp_state.timestamp == ts_before + state.rtp_frame_ticks);

	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e video send recv size", received == (ssize_t)(12 + sizeof(nal)));
	CHECK("star6e video send recv payload type",
		received >= 2 && (buf[1] & 0x7F) == 97);

	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_video_send_frame_disabled(void)
{
	static const uint8_t nal[] = { 0x02, 0x01, 0xAA, 0xBB };
	VencConfig cfg;
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eVideoState state = {0};
	MI_VENC_Pack_t pack = {0};
	MI_VENC_Stream_t stream = {0};
	uint32_t seq_before;
	uint32_t ts_before;
	size_t total_bytes;
	int failures = 0;
	int ret;

	venc_config_defaults(&cfg);

	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600", "rtp", 1400, 0);
	CHECK("star6e video disabled prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e video disabled output init", ret == 0);
	star6e_video_init(&state, &cfg, 30, &output);
	seq_before = state.rtp_state.seq;
	ts_before = state.rtp_state.timestamp;

	pack.data = (uint8_t *)nal;
	pack.length = sizeof(nal);
	pack.packNum = 1;
	pack.packetInfo[0].offset = 0;
	pack.packetInfo[0].length = sizeof(nal);
	pack.packetInfo[0].packType.h265Nalu = 1;
	stream.count = 1;
	stream.packet = &pack;

	total_bytes = star6e_video_send_frame(&state, &output, &stream, 0, 0, NULL);
	CHECK("star6e video disabled bytes", total_bytes == 0);
	CHECK("star6e video disabled frame counter", state.frame_counter == 1);
	CHECK("star6e video disabled seq stable", state.rtp_state.seq == seq_before);
	CHECK("star6e video disabled ts stable", state.rtp_state.timestamp == ts_before);

	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_video_sidecar_ext(void)
{
	static const uint8_t nal[] = { 0x02, 0x01, 0xAA, 0xBB };
	VencConfig cfg;
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eVideoState state = {0};
	MI_VENC_Pack_t pack = {0};
	MI_VENC_Stream_t stream = {0};
	RtpSidecarSubscribe sub = {0};
	RtpSidecarEncInfo enc_info = {0};
	RtpSidecarFrameExt wire = {0};
	struct sockaddr_in sidecar_addr;
	ssize_t received;
	uint16_t probe_port;
	uint16_t sidecar_port;
	int probe_socket;
	int failures = 0;
	int ret;

	venc_config_defaults(&cfg);
	ret = reserve_udp_port(&sidecar_port);
	CHECK("star6e sidecar reserve port", ret == 0);
	probe_socket = create_udp_receiver(&probe_port);
	CHECK("star6e sidecar probe socket", probe_socket >= 0);
	cfg.outgoing.sidecar_port = sidecar_port;

	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600", "rtp", 1400, 0);
	CHECK("star6e sidecar prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e sidecar output init", ret == 0);
	star6e_video_init(&state, &cfg, 30, &output);

	memset(&sidecar_addr, 0, sizeof(sidecar_addr));
	sidecar_addr.sin_family = AF_INET;
	sidecar_addr.sin_port = htons(sidecar_port);
	sidecar_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sub.magic = htonl(RTP_SIDECAR_MAGIC);
	sub.version = RTP_SIDECAR_VERSION;
	sub.msg_type = RTP_SIDECAR_MSG_SUBSCRIBE;
	ret = (int)sendto(probe_socket, &sub, sizeof(sub), 0,
		(struct sockaddr *)&sidecar_addr, sizeof(sidecar_addr));
	CHECK("star6e sidecar subscribe send", ret == (int)sizeof(sub));

	pack.data = (uint8_t *)nal;
	pack.length = sizeof(nal);
	pack.timestamp = 123456;
	pack.packNum = 1;
	pack.packetInfo[0].offset = 0;
	pack.packetInfo[0].length = sizeof(nal);
	pack.packetInfo[0].packType.h265Nalu = 1;
	stream.count = 1;
	stream.packet = &pack;

	enc_info.frame_size_bytes = 4321;
	enc_info.frame_type = RTP_SIDECAR_FRAME_IDR;
	enc_info.qp = 27;
	enc_info.complexity = 91;
	enc_info.scene_change = 1;
	enc_info.gop_state = 3;
	enc_info.idr_inserted = 1;
	enc_info.frames_since_idr = 15;

	(void)star6e_video_send_frame(&state, &output, &stream, 1, 0, &enc_info);

	received = recv(probe_socket, &wire, sizeof(wire), 0);
	CHECK("star6e sidecar recv size",
		received == (ssize_t)sizeof(RtpSidecarFrameExt));
	CHECK("star6e sidecar recv enc flag",
		received >= (ssize_t)sizeof(RtpSidecarFrame) &&
		(wire.frame.flags & RTP_SIDECAR_FLAG_ENC_INFO) != 0);
	CHECK("star6e sidecar recv keyframe flag",
		received >= (ssize_t)sizeof(RtpSidecarFrame) &&
		(wire.frame.flags & RTP_SIDECAR_FLAG_KEYFRAME) != 0);
	CHECK("star6e sidecar recv seq_count",
		received >= (ssize_t)sizeof(RtpSidecarFrame) &&
		ntohs(wire.frame.seq_count) == 1);
	CHECK("star6e sidecar recv frame size",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		ntohl(wire.enc.frame_size_bytes) == enc_info.frame_size_bytes);
	CHECK("star6e sidecar recv frame type",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		wire.enc.frame_type == enc_info.frame_type);
	CHECK("star6e sidecar recv qp",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		wire.enc.qp == enc_info.qp);
	CHECK("star6e sidecar recv complexity",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		wire.enc.complexity == enc_info.complexity);
	CHECK("star6e sidecar recv scene change",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		wire.enc.scene_change == enc_info.scene_change);
	CHECK("star6e sidecar recv gop state",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		wire.enc.gop_state == enc_info.gop_state);
	CHECK("star6e sidecar recv idr inserted",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		wire.enc.idr_inserted == enc_info.idr_inserted);
	CHECK("star6e sidecar recv fsi",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		ntohs(wire.enc.frames_since_idr) == enc_info.frames_since_idr);

	star6e_output_teardown(&output);
	close(probe_socket);
	return failures;
}

int test_star6e_video(void)
{
	int failures = 0;

	failures += test_star6e_video_init_rtp_state();
	failures += test_star6e_video_init_compact_state();
	failures += test_star6e_video_send_frame_rtp();
	failures += test_star6e_video_send_frame_disabled();
	failures += test_star6e_video_sidecar_ext();
	return failures;
}
