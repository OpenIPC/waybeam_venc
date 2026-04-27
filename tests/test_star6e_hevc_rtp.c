#include "star6e_hevc_rtp.h"

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

static int test_star6e_hevc_rtp_sends_idr_with_prepended_ap(void)
{
	static const uint8_t vps[] = { 0x40, 0x01, 0xAA };
	static const uint8_t sps[] = { 0x42, 0x01, 0xBB };
	static const uint8_t pps[] = { 0x44, 0x01, 0xCC };
	static const uint8_t idr[] = { 0x26, 0x01, 0xDD, 0xEE };
	Star6eOutputSetup setup;
	Star6eOutput output;
	H26xParamSets params = {0};
	RtpPacketizerState rtp = {
		.seq = 0x1234,
		.timestamp = 0x01020304,
		.ssrc = 0x05060708,
		.payload_type = 97,
	};
	Star6eHevcRtpStats stats = {0};
	MI_VENC_Pack_t pack = {0};
	MI_VENC_Stream_t stream = {0};
	uint8_t buf[64];
	char uri[64];
	size_t sent_bytes;
	ssize_t received;
	uint16_t port;
	int recv_socket;
	int failures = 0;
	int ret;

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e hevc rtp receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "rtp", 0);
	CHECK("star6e hevc rtp prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e hevc rtp init", ret == 0);

	h26x_param_sets_update(&params, PT_H265, 32, vps, sizeof(vps));
	h26x_param_sets_update(&params, PT_H265, 33, sps, sizeof(sps));
	h26x_param_sets_update(&params, PT_H265, 34, pps, sizeof(pps));

	pack.data = (uint8_t *)idr;
	pack.length = sizeof(idr);
	pack.packNum = 1;
	pack.packetInfo[0].offset = 0;
	pack.packetInfo[0].length = sizeof(idr);
	pack.packetInfo[0].packType.h265Nalu = 19;
	stream.count = 1;
	stream.packet = &pack;

	sent_bytes = star6e_hevc_rtp_send_frame(&stream, &output, &rtp, 3000,
		&params, 1400, &stats);
	CHECK("star6e hevc rtp sent bytes",
		sent_bytes == sizeof(vps) + sizeof(sps) + sizeof(pps) + sizeof(idr));
	CHECK("star6e hevc rtp stats",
		stats.total_nals == 4 &&
		stats.single_packets == 0 &&
		stats.ap_packets == 1 &&
		stats.ap_nals == 4 &&
		stats.fu_packets == 0 &&
		stats.rtp_packets == 1 &&
		stats.rtp_payload_bytes == (2 + 2 + sizeof(vps) + 2 + sizeof(sps) +
			2 + sizeof(pps) + 2 + sizeof(idr)));
	CHECK("star6e hevc rtp state advanced",
		rtp.seq == 0x1235 && rtp.timestamp == 0x01020EBc);

	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e hevc rtp recv size",
		received == (ssize_t)(12 + stats.rtp_payload_bytes));
	CHECK("star6e hevc rtp marker and payload type",
		received >= 12 &&
		buf[0] == 0x80 &&
		buf[1] == (uint8_t)(0x80 | 97));
	CHECK("star6e hevc rtp ap header",
		received >= 14 &&
		buf[12] == 0x60 &&
		buf[13] == 0x01);

	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

int test_star6e_hevc_rtp(void)
{
	int failures = 0;

	failures += test_star6e_hevc_rtp_sends_idr_with_prepended_ap();
	return failures;
}
