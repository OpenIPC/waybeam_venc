#include "star6e_output.h"

#include "test_helpers.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
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

static int create_unix_receiver(const char *name)
{
	struct sockaddr_un addr;
	struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
	size_t name_len;
	socklen_t addr_len;
	int socket_handle;

	if (!name || !name[0])
		return -1;

	name_len = strlen(name);
	if (name_len > sizeof(addr.sun_path) - 2)
		return -1;

	socket_handle = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (socket_handle < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path + 1, name, name_len);
	addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
	if (bind(socket_handle, (struct sockaddr *)&addr, addr_len) != 0) {
		close(socket_handle);
		return -1;
	}

	(void)setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		sizeof(timeout));
	return socket_handle;
}

static uint16_t output_udp_port(const Star6eOutput *output)
{
	const struct sockaddr_in *dst;

	if (!output || output->dst_len != sizeof(*dst))
		return 0;

	dst = (const struct sockaddr_in *)&output->dst;
	return ntohs(dst->sin_port);
}

static uint32_t output_udp_addr(const Star6eOutput *output)
{
	const struct sockaddr_in *dst;

	if (!output || output->dst_len != sizeof(*dst))
		return 0;

	dst = (const struct sockaddr_in *)&output->dst;
	return dst->sin_addr.s_addr;
}

static int g_test_star6e_rtp_send_called;
static int g_test_star6e_rtp_send_valid;

static size_t test_star6e_output_rtp_send_stub(const Star6eOutput *output,
	const MI_VENC_Stream_t *stream, void *opaque)
{
	size_t *result = opaque;

	g_test_star6e_rtp_send_called++;
	g_test_star6e_rtp_send_valid = (output != NULL && stream != NULL);
	return result ? *result : 0;
}

static int test_star6e_output_reset_state(void)
{
	Star6eOutput output;
	int failures = 0;

	memset(&output, 0xA5, sizeof(output));
	star6e_output_reset(&output);
	CHECK("star6e output reset socket", output.socket_handle == -1);
	CHECK("star6e output reset transport",
		output.transport == VENC_OUTPUT_URI_UDP);
	CHECK("star6e output reset not rtp", !star6e_output_is_rtp(&output));
	CHECK("star6e output reset not shm", !star6e_output_is_shm(&output));
	return failures;
}

static int test_star6e_output_udp_init(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;
	int ret;

	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600",
		"rtp", 1400, 0);
	CHECK("star6e output udp prepare", ret == 0);
	CHECK("star6e output udp setup is rtp", star6e_output_setup_is_rtp(&setup));
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output udp init", ret == 0);
	CHECK("star6e output udp transport",
		output.transport == VENC_OUTPUT_URI_UDP);
	CHECK("star6e output udp is rtp", star6e_output_is_rtp(&output));
	CHECK("star6e output udp is not shm", !star6e_output_is_shm(&output));
	CHECK("star6e output udp socket", output.socket_handle >= 0);
	CHECK("star6e output udp ring null", output.ring == NULL);
	CHECK("star6e output udp port", output_udp_port(&output) == 5600);
	CHECK("star6e output udp addr",
		output_udp_addr(&output) == inet_addr("127.0.0.1"));
	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_output_udp_invalid_host_rejected(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;
	int ret;

	ret = star6e_output_prepare(&setup, "udp://localhost:5600",
		"rtp", 1400, 0);
	CHECK("star6e output invalid host prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output invalid host init rejected", ret == -1);
	return failures;
}

static int test_star6e_output_udp_apply_server(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;
	int ret;

	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600",
		"rtp", 1400, 1);
	CHECK("star6e output udp apply prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output udp apply init", ret == 0);
	ret = star6e_output_apply_server(&output, "udp://127.0.0.1:5601");
	CHECK("star6e output udp apply ok", ret == 0);
	CHECK("star6e output udp apply port", output_udp_port(&output) == 5601);
	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_output_udp_send_rtp(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	uint8_t header[12] = { 0x80, 97, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3 };
	uint8_t payload[4] = { 1, 2, 3, 4 };
	uint8_t buf[16];
	char uri[64];
	uint16_t port;
	ssize_t received;
	int recv_socket;
	int failures = 0;
	int ret;

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e output udp rtp receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "rtp", 1400, 0);
	CHECK("star6e output udp rtp prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output udp rtp init", ret == 0);
	ret = star6e_output_send_rtp_parts(&output, header, sizeof(header), payload,
		sizeof(payload), NULL, 0);
	CHECK("star6e output udp rtp send", ret == 0);
	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e output udp rtp recv size",
		received == (ssize_t)(sizeof(header) + sizeof(payload)));
	CHECK("star6e output udp rtp recv header",
		received >= (ssize_t)sizeof(header) &&
		memcmp(buf, header, sizeof(header)) == 0);
	CHECK("star6e output udp rtp recv payload",
		received >= (ssize_t)(sizeof(header) + sizeof(payload)) &&
		memcmp(buf + sizeof(header), payload, sizeof(payload)) == 0);
	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_output_shm_send_rtp(void)
{
	char uri[64];
	char name[48];
	Star6eOutputSetup setup;
	Star6eOutput output;
	venc_ring_t *attached = NULL;
	uint8_t header[12] = { 0x80, 97, 0, 9, 0, 0, 0, 8, 0, 0, 0, 7 };
	uint8_t payload[5] = { 5, 4, 3, 2, 1 };
	uint8_t slot_data[32];
	uint16_t slot_len = 0;
	int failures = 0;
	int ret;

	snprintf(name, sizeof(name), "test_star6e_output_send_%ld", (long)getpid());
	snprintf(uri, sizeof(uri), "shm://%s", name);
	ret = star6e_output_prepare(&setup, uri, "rtp", 1400, 0);
	CHECK("star6e output shm send prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output shm send init", ret == 0);
	ret = star6e_output_send_rtp_parts(&output, header, sizeof(header), payload,
		sizeof(payload), NULL, 0);
	CHECK("star6e output shm send", ret == 0);
	attached = venc_ring_attach(name);
	CHECK("star6e output shm attach", attached != NULL);
	ret = attached ? venc_ring_read(attached, slot_data, sizeof(slot_data),
		&slot_len) : -1;
	CHECK("star6e output shm read", ret == 0);
	CHECK("star6e output shm read slot len",
		slot_len == (uint16_t)(sizeof(header) + sizeof(payload)));
	CHECK("star6e output shm read header",
		memcmp(slot_data, header, sizeof(header)) == 0);
	CHECK("star6e output shm read payload",
		memcmp(slot_data + sizeof(header), payload, sizeof(payload)) == 0);
	if (attached)
		venc_ring_destroy(attached);
	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_output_shm_init(void)
{
	char uri[64];
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;
	int ret;

	snprintf(uri, sizeof(uri), "shm://test_star6e_output_%ld", (long)getpid());
	ret = star6e_output_prepare(&setup, uri, "rtp", 1400, 0);
	CHECK("star6e output shm prepare", ret == 0);
	CHECK("star6e output shm setup is rtp", star6e_output_setup_is_rtp(&setup));
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output shm init", ret == 0);
	CHECK("star6e output shm transport",
		output.transport == VENC_OUTPUT_URI_SHM);
	CHECK("star6e output shm is rtp", star6e_output_is_rtp(&output));
	CHECK("star6e output shm is shm", star6e_output_is_shm(&output));
	CHECK("star6e output shm socket", output.socket_handle == -1);
	CHECK("star6e output shm ring", output.ring != NULL);
	CHECK("star6e output shm no socket destination", output.dst_len == 0);
	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_output_udp_send_compact(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	uint8_t packet[20] = {
		0x80, 0xe1, 0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 2,
		9, 8, 7, 6, 5, 4, 3, 2
	};
	uint8_t buf[32];
	char uri[64];
	uint16_t port;
	ssize_t received;
	int recv_socket;
	int failures = 0;
	int ret;

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e output compact receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "compact", 1400, 0);
	CHECK("star6e output compact prepare", ret == 0);
	CHECK("star6e output compact setup not rtp",
		!star6e_output_setup_is_rtp(&setup));
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output compact init", ret == 0);
	CHECK("star6e output compact is not rtp",
		!star6e_output_is_rtp(&output));
	ret = star6e_output_send_compact_packet(&output, packet, sizeof(packet), 64);
	CHECK("star6e output compact send", ret == 0);
	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e output compact recv size", received == (ssize_t)sizeof(packet));
	CHECK("star6e output compact recv data",
		received == (ssize_t)sizeof(packet) &&
		memcmp(buf, packet, sizeof(packet)) == 0);
	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_output_unix_send_rtp(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	uint8_t header[12] = { 0x80, 97, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3 };
	uint8_t payload[4] = { 1, 2, 3, 4 };
	uint8_t buf[16];
	char name[64];
	char uri[96];
	ssize_t received;
	int recv_socket;
	int failures = 0;
	int ret;

	snprintf(name, sizeof(name), "test_star6e_unix_rtp_%ld", (long)getpid());
	recv_socket = create_unix_receiver(name);
	CHECK("star6e unix rtp receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "unix://%s", name);
	ret = star6e_output_prepare(&setup, uri, "rtp", 1400, 1);
	CHECK("star6e unix rtp prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e unix rtp init", ret == 0);
	CHECK("star6e unix rtp transport", output.transport == VENC_OUTPUT_URI_UNIX);
	CHECK("star6e unix rtp connected udp ignored", output.connected_udp == 0);
	ret = star6e_output_send_rtp_parts(&output, header, sizeof(header), payload,
		sizeof(payload), NULL, 0);
	CHECK("star6e unix rtp send", ret == 0);
	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e unix rtp recv size",
		received == (ssize_t)(sizeof(header) + sizeof(payload)));
	CHECK("star6e unix rtp recv data",
		received == (ssize_t)(sizeof(header) + sizeof(payload)) &&
		memcmp(buf, header, sizeof(header)) == 0 &&
		memcmp(buf + sizeof(header), payload, sizeof(payload)) == 0);
	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_output_unix_send_compact(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	uint8_t packet[20] = {
		0x80, 0xe1, 0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 2,
		9, 8, 7, 6, 5, 4, 3, 2
	};
	uint8_t buf[32];
	char name[64];
	char uri[96];
	ssize_t received;
	int recv_socket;
	int failures = 0;
	int ret;

	snprintf(name, sizeof(name), "test_star6e_unix_compact_%ld", (long)getpid());
	recv_socket = create_unix_receiver(name);
	CHECK("star6e unix compact receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "unix://%s", name);
	ret = star6e_output_prepare(&setup, uri, "compact", 1400, 0);
	CHECK("star6e unix compact prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e unix compact init", ret == 0);
	CHECK("star6e unix compact transport", output.transport == VENC_OUTPUT_URI_UNIX);
	ret = star6e_output_send_compact_packet(&output, packet, sizeof(packet), 64);
	CHECK("star6e unix compact send", ret == 0);
	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e unix compact recv size", received == (ssize_t)sizeof(packet));
	CHECK("star6e unix compact recv data",
		received == (ssize_t)sizeof(packet) &&
		memcmp(buf, packet, sizeof(packet)) == 0);
	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_output_send_frame_rtp_dispatch(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	MI_VENC_Stream_t stream = {0};
	size_t expected = 1234;
	size_t actual;
	int failures = 0;
	int ret;

	g_test_star6e_rtp_send_called = 0;
	g_test_star6e_rtp_send_valid = 0;
	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600", "rtp", 1400, 0);
	CHECK("star6e output send frame rtp prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output send frame rtp init", ret == 0);
	actual = star6e_output_send_frame(&output, &stream, 1400,
		test_star6e_output_rtp_send_stub, &expected);
	CHECK("star6e output send frame rtp callback called",
		g_test_star6e_rtp_send_called == 1);
	CHECK("star6e output send frame rtp callback args",
		g_test_star6e_rtp_send_valid == 1);
	CHECK("star6e output send frame rtp return", actual == expected);
	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_output_udp_send_compact_frame(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	MI_VENC_Pack_t packs[2];
	MI_VENC_Stream_t stream = {0};
	uint8_t data_a[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	uint8_t data_b[12] = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21 };
	uint8_t buf[32];
	char uri[64];
	uint16_t port;
	ssize_t received;
	size_t total_bytes;
	int recv_socket;
	int failures = 0;
	int ret;

	memset(packs, 0, sizeof(packs));
	packs[0].data = data_a;
	packs[0].length = sizeof(data_a);
	packs[0].packNum = 2;
	packs[0].packetInfo[0].offset = 1;
	packs[0].packetInfo[0].length = 3;
	packs[0].packetInfo[1].offset = 6;
	packs[0].packetInfo[1].length = 4;

	packs[1].data = data_b;
	packs[1].length = sizeof(data_b);
	packs[1].offset = 5;

	stream.count = 2;
	stream.packet = packs;

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e output compact frame receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "compact", 1400, 0);
	CHECK("star6e output compact frame prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output compact frame init", ret == 0);
	total_bytes = star6e_output_send_compact_frame(&output, &stream, 64);
	CHECK("star6e output compact frame bytes", total_bytes == 14);

	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e output compact frame recv 0 size", received == 3);
	CHECK("star6e output compact frame recv 0 data",
		received == 3 && memcmp(buf, data_a + 1, 3) == 0);

	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e output compact frame recv 1 size", received == 4);
	CHECK("star6e output compact frame recv 1 data",
		received == 4 && memcmp(buf, data_a + 6, 4) == 0);

	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e output compact frame recv 2 size", received == 7);
	CHECK("star6e output compact frame recv 2 data",
		received == 7 && memcmp(buf, data_b + 5, 7) == 0);

	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_output_send_frame_compact_dispatch(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	MI_VENC_Pack_t pack = {0};
	MI_VENC_Stream_t stream = {0};
	uint8_t data[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
	uint8_t buf[32];
	char uri[64];
	uint16_t port;
	ssize_t received;
	size_t total_bytes;
	int recv_socket;
	int failures = 0;
	int ret;

	pack.data = data;
	pack.length = sizeof(data);
	pack.offset = 4;
	stream.count = 1;
	stream.packet = &pack;

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e output send frame compact receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "compact", 1400, 0);
	CHECK("star6e output send frame compact prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output send frame compact init", ret == 0);
	g_test_star6e_rtp_send_called = 0;
	g_test_star6e_rtp_send_valid = 0;
	total_bytes = star6e_output_send_frame(&output, &stream, 64,
		test_star6e_output_rtp_send_stub, NULL);
	CHECK("star6e output send frame compact callback skipped",
		g_test_star6e_rtp_send_called == 0);
	CHECK("star6e output send frame compact bytes", total_bytes == 8);

	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e output send frame compact recv size", received == 8);
	CHECK("star6e output send frame compact recv data",
		received == 8 && memcmp(buf, data + 4, 8) == 0);

	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_output_shm_compact_rejected(void)
{
	char uri[64];
	Star6eOutputSetup setup;
	int failures = 0;
	int ret;

	snprintf(uri, sizeof(uri), "shm://test_star6e_output_bad_%ld", (long)getpid());
	ret = star6e_output_prepare(&setup, uri, "compact", 1400, 0);
	CHECK("star6e output shm compact rejected", ret == -1);
	return failures;
}

static int test_star6e_output_shm_apply_server_rejected(void)
{
	char uri[64];
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;
	int ret;

	snprintf(uri, sizeof(uri), "shm://test_star6e_output_apply_%ld", (long)getpid());
	ret = star6e_output_prepare(&setup, uri, "rtp", 1400, 0);
	CHECK("star6e output shm apply prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output shm apply init", ret == 0);
	ret = star6e_output_apply_server(&output, "udp://127.0.0.1:5602");
	CHECK("star6e output shm apply rejected", ret == -1);
	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_output_empty_server_prepare(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;
	int ret;

	ret = star6e_output_prepare(&setup, "", "rtp", 1400, 0);
	CHECK("star6e output empty prepare", ret == 0);
	CHECK("star6e output empty prepare has no server", setup.has_server == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output empty init", ret == 0);
	CHECK("star6e output empty socket", output.socket_handle == -1);
	CHECK("star6e output empty ring", output.ring == NULL);
	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_output_prepare_defaults_to_rtp(void)
{
	Star6eOutputSetup setup;
	int failures = 0;
	int ret;

	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600",
		"unexpected", 1400, 0);
	CHECK("star6e output default mode prepare", ret == 0);
	CHECK("star6e output default mode is rtp",
		star6e_output_setup_is_rtp(&setup));
	return failures;
}

static int test_star6e_audio_output_reset_state(void)
{
	Star6eAudioOutput audio_output;
	int failures = 0;

	memset(&audio_output, 0xA5, sizeof(audio_output));
	star6e_audio_output_reset(&audio_output);
	CHECK("star6e audio output reset socket", audio_output.socket_handle == -1);
	CHECK("star6e audio output reset port", star6e_audio_output_port(&audio_output) == 0);
	return failures;
}

static int test_star6e_audio_output_send_rtp(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	RtpPacketizerState rtp_state;
	uint8_t payload[5] = { 1, 2, 3, 4, 5 };
	uint8_t buf[32];
	char uri[64];
	uint16_t port;
	ssize_t received;
	int recv_socket;
	int failures = 0;
	int ret;

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e audio rtp receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "rtp", 1400, 0);
	CHECK("star6e audio rtp prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e audio rtp output init", ret == 0);
	ret = star6e_audio_output_init(&audio_output, &output, 0, 1400);
	CHECK("star6e audio rtp init", ret == 0);
	CHECK("star6e audio rtp port", star6e_audio_output_port(&audio_output) == port);

	memset(&rtp_state, 0, sizeof(rtp_state));
	rtp_state.seq = 0x1234;
	rtp_state.timestamp = 0x01020304;
	rtp_state.ssrc = 0x05060708;
	rtp_state.payload_type = 110;

	ret = star6e_audio_output_send_rtp(&audio_output, payload, sizeof(payload),
		&rtp_state, 320);
	CHECK("star6e audio rtp send", ret == 0);

	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e audio rtp recv size",
		received == (ssize_t)(12 + sizeof(payload)));
	CHECK("star6e audio rtp payload type",
		received >= 12 && buf[1] == 110);
	CHECK("star6e audio rtp seq",
		received >= 4 && buf[2] == 0x12 && buf[3] == 0x34);
	CHECK("star6e audio rtp timestamp",
		received >= 8 && buf[4] == 0x01 && buf[5] == 0x02 &&
		buf[6] == 0x03 && buf[7] == 0x04);
	CHECK("star6e audio rtp ssrc",
		received >= 12 && buf[8] == 0x05 && buf[9] == 0x06 &&
		buf[10] == 0x07 && buf[11] == 0x08);
	CHECK("star6e audio rtp payload",
		received >= (ssize_t)(12 + sizeof(payload)) &&
		memcmp(buf + 12, payload, sizeof(payload)) == 0);
	CHECK("star6e audio rtp seq advance", rtp_state.seq == 0x1235);
	CHECK("star6e audio rtp timestamp advance",
		rtp_state.timestamp == 0x01020304u + 320u);

	star6e_audio_output_teardown(&audio_output);
	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_audio_output_shm_dedicated_local_udp(void)
{
	char uri[64];
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	uint8_t payload[2] = { 3, 1 };
	uint8_t buf[16];
	uint16_t audio_port;
	ssize_t received;
	int recv_socket_audio;
	int failures = 0;
	int ret;

	snprintf(uri, sizeof(uri), "shm://test_star6e_audio_shm_%ld", (long)getpid());
	ret = star6e_output_prepare(&setup, uri, "rtp", 1400, 0);
	CHECK("star6e audio shm prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e audio shm output init", ret == 0);
	recv_socket_audio = create_udp_receiver(&audio_port);
	CHECK("star6e audio shm receiver", recv_socket_audio >= 0);
	ret = star6e_audio_output_init(&audio_output, &output, audio_port, 16);
	CHECK("star6e audio shm dedicated init", ret == 0);
	CHECK("star6e audio shm dedicated port",
		star6e_audio_output_port(&audio_output) == audio_port);
	ret = star6e_audio_output_send_compact(&audio_output, payload, sizeof(payload));
	CHECK("star6e audio shm dedicated send", ret == 0);
	received = recv(recv_socket_audio, buf, sizeof(buf), 0);
	CHECK("star6e audio shm dedicated recv size", received == 6);
	CHECK("star6e audio shm dedicated recv data",
		received == 6 && buf[0] == 0xAA && buf[1] == 0x01 &&
		buf[2] == 0x00 && buf[3] == 0x02 &&
		memcmp(buf + 4, payload, sizeof(payload)) == 0);
	star6e_audio_output_teardown(&audio_output);
	star6e_output_teardown(&output);
	close(recv_socket_audio);
	return failures;
}

static int test_star6e_audio_output_shared_apply_server(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	uint8_t payload[3] = { 9, 8, 7 };
	uint8_t buf[16];
	char uri_a[64];
	char uri_b[64];
	uint16_t port_a;
	uint16_t port_b;
	ssize_t received;
	int recv_socket_a;
	int recv_socket_b;
	int failures = 0;
	int ret;

	recv_socket_a = create_udp_receiver(&port_a);
	CHECK("star6e audio shared receiver a", recv_socket_a >= 0);
	recv_socket_b = create_udp_receiver(&port_b);
	CHECK("star6e audio shared receiver b", recv_socket_b >= 0);
	snprintf(uri_a, sizeof(uri_a), "udp://127.0.0.1:%u", port_a);
	snprintf(uri_b, sizeof(uri_b), "udp://127.0.0.1:%u", port_b);

	ret = star6e_output_prepare(&setup, uri_a, "compact", 1400, 0);
	CHECK("star6e audio shared prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e audio shared output init", ret == 0);
	ret = star6e_audio_output_init(&audio_output, &output, 0, 16);
	CHECK("star6e audio shared init", ret == 0);

	ret = star6e_audio_output_send(&audio_output, payload, sizeof(payload), NULL, 0);
	CHECK("star6e audio shared send a", ret == 0);
	received = recv(recv_socket_a, buf, sizeof(buf), 0);
	CHECK("star6e audio shared recv a size", received == 7);
	CHECK("star6e audio shared recv a data",
		received == 7 && buf[0] == 0xAA && buf[1] == 0x01 &&
		buf[2] == 0x00 && buf[3] == 0x03 &&
		memcmp(buf + 4, payload, sizeof(payload)) == 0);

	ret = star6e_output_apply_server(&output, uri_b);
	CHECK("star6e audio shared apply server", ret == 0);
	CHECK("star6e audio shared port update",
		star6e_audio_output_port(&audio_output) == port_b);
	ret = star6e_audio_output_send(&audio_output, payload, sizeof(payload), NULL, 0);
	CHECK("star6e audio shared send b", ret == 0);
	received = recv(recv_socket_b, buf, sizeof(buf), 0);
	CHECK("star6e audio shared recv b size", received == 7);
	CHECK("star6e audio shared recv b data",
		received == 7 && buf[0] == 0xAA && buf[1] == 0x01 &&
		buf[2] == 0x00 && buf[3] == 0x03 &&
		memcmp(buf + 4, payload, sizeof(payload)) == 0);

	star6e_audio_output_teardown(&audio_output);
	star6e_output_teardown(&output);
	close(recv_socket_b);
	close(recv_socket_a);
	return failures;
}

static int test_star6e_audio_output_shared_switch_udp_to_unix(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	uint8_t payload[3] = { 7, 6, 5 };
	uint8_t buf[16];
	char udp_uri[64];
	char unix_name[64];
	char unix_uri[96];
	uint16_t udp_port;
	ssize_t received;
	int udp_socket;
	int unix_socket;
	int failures = 0;
	int ret;

	udp_socket = create_udp_receiver(&udp_port);
	CHECK("star6e audio udp->unix receiver udp", udp_socket >= 0);
	snprintf(unix_name, sizeof(unix_name), "test_star6e_audio_udp_unix_%ld",
		(long)getpid());
	unix_socket = create_unix_receiver(unix_name);
	CHECK("star6e audio udp->unix receiver unix", unix_socket >= 0);
	snprintf(udp_uri, sizeof(udp_uri), "udp://127.0.0.1:%u", udp_port);
	snprintf(unix_uri, sizeof(unix_uri), "unix://%s", unix_name);

	ret = star6e_output_prepare(&setup, udp_uri, "compact", 1400, 0);
	CHECK("star6e audio udp->unix prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e audio udp->unix output init", ret == 0);
	ret = star6e_audio_output_init(&audio_output, &output, 0, 16);
	CHECK("star6e audio udp->unix audio init", ret == 0);

	ret = star6e_audio_output_send(&audio_output, payload, sizeof(payload), NULL, 0);
	CHECK("star6e audio udp->unix send udp", ret == 0);
	received = recv(udp_socket, buf, sizeof(buf), 0);
	CHECK("star6e audio udp->unix recv udp size", received == 7);

	ret = star6e_output_apply_server(&output, unix_uri);
	CHECK("star6e audio udp->unix apply unix", ret == 0);
	CHECK("star6e audio udp->unix shared port reset",
		star6e_audio_output_port(&audio_output) == 0);
	ret = star6e_audio_output_send(&audio_output, payload, sizeof(payload), NULL, 0);
	CHECK("star6e audio udp->unix send unix", ret == 0);
	received = recv(unix_socket, buf, sizeof(buf), 0);
	CHECK("star6e audio udp->unix recv unix size", received == 7);
	CHECK("star6e audio udp->unix recv unix data",
		received == 7 && buf[0] == 0xAA && buf[1] == 0x01 &&
		buf[2] == 0x00 && buf[3] == 0x03 &&
		memcmp(buf + 4, payload, sizeof(payload)) == 0);

	star6e_audio_output_teardown(&audio_output);
	star6e_output_teardown(&output);
	close(unix_socket);
	close(udp_socket);
	return failures;
}

static int test_star6e_audio_output_dedicated_port(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	uint8_t payload[2] = { 4, 2 };
	uint8_t buf[16];
	char video_uri_a[64];
	char video_uri_b[64];
	uint16_t video_port_a;
	uint16_t video_port_b;
	uint16_t audio_port;
	ssize_t received;
	int recv_socket_video_a;
	int recv_socket_video_b;
	int recv_socket_audio;
	int failures = 0;
	int ret;

	recv_socket_video_a = create_udp_receiver(&video_port_a);
	CHECK("star6e audio dedicated receiver video a", recv_socket_video_a >= 0);
	recv_socket_video_b = create_udp_receiver(&video_port_b);
	CHECK("star6e audio dedicated receiver video b", recv_socket_video_b >= 0);
	recv_socket_audio = create_udp_receiver(&audio_port);
	CHECK("star6e audio dedicated receiver audio", recv_socket_audio >= 0);

	snprintf(video_uri_a, sizeof(video_uri_a), "udp://127.0.0.1:%u", video_port_a);
	snprintf(video_uri_b, sizeof(video_uri_b), "udp://127.0.0.1:%u", video_port_b);

	ret = star6e_output_prepare(&setup, video_uri_a, "compact", 1400, 0);
	CHECK("star6e audio dedicated prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e audio dedicated output init", ret == 0);
	ret = star6e_audio_output_init(&audio_output, &output, audio_port, 16);
	CHECK("star6e audio dedicated init", ret == 0);
	CHECK("star6e audio dedicated port",
		star6e_audio_output_port(&audio_output) == audio_port);

	ret = star6e_output_apply_server(&output, video_uri_b);
	CHECK("star6e audio dedicated apply server", ret == 0);
	CHECK("star6e audio dedicated port stable",
		star6e_audio_output_port(&audio_output) == audio_port);
	ret = star6e_audio_output_send_compact(&audio_output, payload, sizeof(payload));
	CHECK("star6e audio dedicated send", ret == 0);

	received = recv(recv_socket_audio, buf, sizeof(buf), 0);
	CHECK("star6e audio dedicated recv size", received == 6);
	CHECK("star6e audio dedicated recv data",
		received == 6 && buf[0] == 0xAA && buf[1] == 0x01 &&
		buf[2] == 0x00 && buf[3] == 0x02 &&
		memcmp(buf + 4, payload, sizeof(payload)) == 0);

	star6e_audio_output_teardown(&audio_output);
	star6e_output_teardown(&output);
	close(recv_socket_audio);
	close(recv_socket_video_b);
	close(recv_socket_video_a);
	return failures;
}

static int test_star6e_audio_output_unix_dedicated_local_udp(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	uint8_t payload[2] = { 4, 2 };
	uint8_t buf[16];
	char unix_name_a[64];
	char unix_name_b[64];
	char unix_uri_a[96];
	char unix_uri_b[96];
	uint16_t audio_port;
	ssize_t received;
	int recv_socket_audio;
	int recv_socket_unix_a;
	int recv_socket_unix_b;
	int failures = 0;
	int ret;

	snprintf(unix_name_a, sizeof(unix_name_a), "test_star6e_audio_unix_a_%ld",
		(long)getpid());
	snprintf(unix_name_b, sizeof(unix_name_b), "test_star6e_audio_unix_b_%ld",
		(long)getpid());
	recv_socket_unix_a = create_unix_receiver(unix_name_a);
	CHECK("star6e audio unix dedicated receiver a", recv_socket_unix_a >= 0);
	recv_socket_unix_b = create_unix_receiver(unix_name_b);
	CHECK("star6e audio unix dedicated receiver b", recv_socket_unix_b >= 0);
	recv_socket_audio = create_udp_receiver(&audio_port);
	CHECK("star6e audio unix dedicated receiver audio", recv_socket_audio >= 0);
	snprintf(unix_uri_a, sizeof(unix_uri_a), "unix://%s", unix_name_a);
	snprintf(unix_uri_b, sizeof(unix_uri_b), "unix://%s", unix_name_b);

	ret = star6e_output_prepare(&setup, unix_uri_a, "compact", 1400, 0);
	CHECK("star6e audio unix dedicated prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e audio unix dedicated output init", ret == 0);
	ret = star6e_audio_output_init(&audio_output, &output, audio_port, 16);
	CHECK("star6e audio unix dedicated init", ret == 0);
	CHECK("star6e audio unix dedicated port",
		star6e_audio_output_port(&audio_output) == audio_port);

	ret = star6e_audio_output_send_compact(&audio_output, payload, sizeof(payload));
	CHECK("star6e audio unix dedicated send a", ret == 0);
	received = recv(recv_socket_audio, buf, sizeof(buf), 0);
	CHECK("star6e audio unix dedicated recv a size", received == 6);
	CHECK("star6e audio unix dedicated recv a data",
		received == 6 && buf[0] == 0xAA && buf[1] == 0x01 &&
		buf[2] == 0x00 && buf[3] == 0x02 &&
		memcmp(buf + 4, payload, sizeof(payload)) == 0);

	ret = star6e_output_apply_server(&output, unix_uri_b);
	CHECK("star6e audio unix dedicated apply b", ret == 0);
	ret = star6e_audio_output_send_compact(&audio_output, payload, sizeof(payload));
	CHECK("star6e audio unix dedicated send b", ret == 0);
	received = recv(recv_socket_audio, buf, sizeof(buf), 0);
	CHECK("star6e audio unix dedicated recv b size", received == 6);

	star6e_audio_output_teardown(&audio_output);
	star6e_output_teardown(&output);
	close(recv_socket_audio);
	close(recv_socket_unix_b);
	close(recv_socket_unix_a);
	return failures;
}

static int test_star6e_audio_output_shared_teardown_keeps_video_socket(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	uint8_t header[12] = { 0x80, 97, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3 };
	uint8_t payload[4] = { 1, 2, 3, 4 };
	uint8_t buf[16];
	char uri[64];
	uint16_t port;
	ssize_t received;
	int recv_socket;
	int failures = 0;
	int ret;

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e audio shared teardown receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "rtp", 1400, 0);
	CHECK("star6e audio shared teardown prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e audio shared teardown output init", ret == 0);
	ret = star6e_audio_output_init(&audio_output, &output, 0, 1400);
	CHECK("star6e audio shared teardown init", ret == 0);

	star6e_audio_output_teardown(&audio_output);
	ret = star6e_output_send_rtp_parts(&output, header, sizeof(header), payload,
		sizeof(payload), NULL, 0);
	CHECK("star6e audio shared teardown video send", ret == 0);
	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e audio shared teardown recv size",
		received == (ssize_t)(sizeof(header) + sizeof(payload)));

	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

int test_star6e_output(void)
{
	int failures = 0;

	failures += test_star6e_output_reset_state();
	failures += test_star6e_output_udp_init();
	failures += test_star6e_output_udp_invalid_host_rejected();
	failures += test_star6e_output_udp_apply_server();
	failures += test_star6e_output_udp_send_rtp();
	failures += test_star6e_output_udp_send_compact();
	failures += test_star6e_output_unix_send_rtp();
	failures += test_star6e_output_unix_send_compact();
	failures += test_star6e_output_send_frame_rtp_dispatch();
	failures += test_star6e_output_udp_send_compact_frame();
	failures += test_star6e_output_send_frame_compact_dispatch();
	failures += test_star6e_output_shm_init();
	failures += test_star6e_output_shm_send_rtp();
	failures += test_star6e_output_shm_compact_rejected();
	failures += test_star6e_output_shm_apply_server_rejected();
	failures += test_star6e_output_empty_server_prepare();
	failures += test_star6e_output_prepare_defaults_to_rtp();
	failures += test_star6e_audio_output_reset_state();
	failures += test_star6e_audio_output_send_rtp();
	failures += test_star6e_audio_output_shm_dedicated_local_udp();
	failures += test_star6e_audio_output_shared_apply_server();
	failures += test_star6e_audio_output_shared_switch_udp_to_unix();
	failures += test_star6e_audio_output_dedicated_port();
	failures += test_star6e_audio_output_unix_dedicated_local_udp();
	failures += test_star6e_audio_output_shared_teardown_keeps_video_socket();
	return failures;
}
