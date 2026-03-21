#include "star6e_output.h"

#include "venc_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define STAR6E_RTP_HEADER_SIZE 12

static uint16_t star6e_read_be16(const uint8_t *data)
{
	return (uint16_t)((uint16_t)data[0] << 8 | (uint16_t)data[1]);
}

static uint32_t star6e_read_be32(const uint8_t *data)
{
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
		((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static void star6e_write_be16(uint8_t *data, uint16_t value)
{
	data[0] = (uint8_t)(value >> 8);
	data[1] = (uint8_t)(value & 0xff);
}

static void star6e_write_be32(uint8_t *data, uint32_t value)
{
	data[0] = (uint8_t)(value >> 24);
	data[1] = (uint8_t)((value >> 16) & 0xff);
	data[2] = (uint8_t)((value >> 8) & 0xff);
	data[3] = (uint8_t)(value & 0xff);
}

static int star6e_output_send_udp_parts(int socket_handle,
	const struct sockaddr_in *dst, const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len)
{
	struct iovec vec[3];
	struct msghdr msg;
	ssize_t sent;
	int iovcnt;

	if (socket_handle < 0 || !dst || !header || !payload1 ||
	    header_len == 0 || payload1_len == 0) {
		return -1;
	}

	vec[0].iov_base = (void *)header;
	vec[0].iov_len = header_len;
	vec[1].iov_base = (void *)payload1;
	vec[1].iov_len = payload1_len;
	iovcnt = 2;
	if (payload2 && payload2_len > 0) {
		vec[2].iov_base = (void *)payload2;
		vec[2].iov_len = payload2_len;
		iovcnt = 3;
	}

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = (void *)dst;
	msg.msg_namelen = sizeof(*dst);
	msg.msg_iov = vec;
	msg.msg_iovlen = iovcnt;
	sent = sendmsg(socket_handle, &msg, 0);
	return sent < 0 ? -1 : 0;
}

static int star6e_audio_output_resolve_destination(
	const Star6eAudioOutput *audio_output, struct sockaddr_in *dst)
{
	if (!audio_output || !audio_output->video_output || !dst)
		return -1;

	*dst = audio_output->video_output->dst;
	if (audio_output->port_override != 0)
		dst->sin_port = htons(audio_output->port_override);
	return 0;
}

static int star6e_audio_output_write_rtp(const uint8_t *header,
	size_t header_len, const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	const Star6eAudioOutput *audio_output = opaque;
	struct sockaddr_in dst;

	if (!audio_output ||
	    star6e_audio_output_resolve_destination(audio_output, &dst) != 0) {
		return -1;
	}

	return star6e_output_send_udp_parts(audio_output->socket_handle, &dst,
		header, header_len, payload1, payload1_len,
		payload2, payload2_len);
}

static void star6e_output_setup_reset(Star6eOutputSetup *setup)
{
	if (!setup)
		return;

	memset(setup, 0, sizeof(*setup));
	setup->transport = STAR6E_OUTPUT_TRANSPORT_UDP;
}

void star6e_output_reset(Star6eOutput *output)
{
	if (!output)
		return;

	memset(output, 0, sizeof(*output));
	output->socket_handle = -1;
	output->transport = STAR6E_OUTPUT_TRANSPORT_UDP;
}

static Star6eStreamMode star6e_output_stream_mode_from_name(
	const char *stream_mode_name)
{
	if (stream_mode_name && strcmp(stream_mode_name, "compact") == 0)
		return STAR6E_STREAM_MODE_COMPACT;

	return STAR6E_STREAM_MODE_RTP;
}

int star6e_output_prepare(Star6eOutputSetup *setup, const char *server_uri,
	const char *stream_mode_name, uint16_t max_frame_size, int connected_udp)
{
	if (!setup)
		return -1;

	star6e_output_setup_reset(setup);
	setup->stream_mode = star6e_output_stream_mode_from_name(stream_mode_name);
	setup->connected_udp = connected_udp ? 1 : 0;
	setup->max_frame_size = max_frame_size;

	if (!server_uri || !server_uri[0])
		return 0;

	setup->has_server = 1;
	if (strncmp(server_uri, "shm://", 6) == 0) {
		if (setup->stream_mode != STAR6E_STREAM_MODE_RTP) {
			fprintf(stderr, "ERROR: shm:// output requires RTP stream mode.\n");
			return -1;
		}
		snprintf(setup->shm_name, sizeof(setup->shm_name), "%s",
			server_uri + 6);
		if (!setup->shm_name[0]) {
			fprintf(stderr, "ERROR: shm:// URI missing name\n");
			return -1;
		}
		setup->transport = STAR6E_OUTPUT_TRANSPORT_SHM;
		return 0;
	}

	if (venc_config_parse_server_uri(server_uri, setup->host,
	    sizeof(setup->host), &setup->port) != 0) {
		return -1;
	}

	return 0;
}

int star6e_output_setup_is_rtp(const Star6eOutputSetup *setup)
{
	return setup && setup->stream_mode == STAR6E_STREAM_MODE_RTP;
}

int star6e_output_init(Star6eOutput *output, const Star6eOutputSetup *setup)
{
	uint32_t slot_data;

	if (!output)
		return -1;

	star6e_output_reset(output);
	if (!setup)
		return -1;

	output->stream_mode = setup->stream_mode;
	output->connected_udp = setup->connected_udp;
	if (!setup->has_server)
		return 0;

	if (setup->transport == STAR6E_OUTPUT_TRANSPORT_SHM) {
		slot_data = (uint32_t)setup->max_frame_size + 12;
		output->ring = venc_ring_create(setup->shm_name, 512, slot_data);
		if (!output->ring) {
			fprintf(stderr, "ERROR: venc_ring_create(%s) failed\n",
				setup->shm_name);
			return -1;
		}

		output->transport = STAR6E_OUTPUT_TRANSPORT_SHM;
		memset(&output->dst, 0, sizeof(output->dst));
		output->dst.sin_family = AF_INET;
		output->dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		output->connected_udp = 0;
		return 0;
	}

	output->socket_handle = socket(AF_INET, SOCK_DGRAM, 0);
	if (output->socket_handle < 0) {
		fprintf(stderr, "ERROR: Unable to create UDP socket\n");
		return -1;
	}

	memset(&output->dst, 0, sizeof(output->dst));
	output->dst.sin_family = AF_INET;
	output->dst.sin_port = htons(setup->port);
	output->dst.sin_addr.s_addr = inet_addr(setup->host);
	if (output->connected_udp && output->dst.sin_addr.s_addr != 0) {
		if (connect(output->socket_handle, (struct sockaddr *)&output->dst,
		    sizeof(output->dst)) != 0) {
			fprintf(stderr, "WARNING: UDP connect() failed (%d), using unconnected\n",
				errno);
			output->connected_udp = 0;
		}
	}

	return 0;
}

int star6e_output_is_rtp(const Star6eOutput *output)
{
	return output && output->stream_mode == STAR6E_STREAM_MODE_RTP;
}

int star6e_output_is_shm(const Star6eOutput *output)
{
	return output && output->transport == STAR6E_OUTPUT_TRANSPORT_SHM;
}

uint32_t star6e_output_drain_send_errors(Star6eOutput *output)
{
	uint32_t n;
	if (!output)
		return 0;
	n = output->send_errors;
	output->send_errors = 0;
	return n;
}

int star6e_output_send_rtp_parts(const Star6eOutput *output,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len)
{
	if (!output || !header || !payload1 || header_len == 0 || payload1_len == 0)
		return -1;

	if (output->ring) {
		size_t total_payload = payload1_len + payload2_len;
		if (header_len > UINT16_MAX || total_payload > UINT16_MAX)
			return -1;
		if (payload2 && payload2_len > 0) {
			uint8_t flat[RTP_BUFFER_MAX];
			if (total_payload > sizeof(flat))
				return -1;
			memcpy(flat, payload1, payload1_len);
			memcpy(flat + payload1_len, payload2, payload2_len);
			return venc_ring_write(output->ring, header,
				(uint16_t)header_len, flat,
				(uint16_t)total_payload);
		}
		return venc_ring_write(output->ring, header, (uint16_t)header_len,
			payload1, (uint16_t)payload1_len);
	}

	if (star6e_output_send_udp_parts(output->socket_handle, &output->dst,
	    header, header_len, payload1, payload1_len,
	    payload2, payload2_len) != 0) {
		((Star6eOutput *)output)->send_errors++;
		return -1;
	}
	return 0;
}

int star6e_output_send_compact_packet(const Star6eOutput *output,
	const uint8_t *packet, uint32_t packet_size, uint32_t max_size)
{
	uint32_t payload_offset = STAR6E_RTP_HEADER_SIZE;
	uint32_t payload_size;
	const uint8_t *payload;
	uint32_t offset = 0;
	uint8_t marker;
	uint16_t sequence;
	uint32_t timestamp;
	uint32_t ssrc_id;
	uint32_t max_fragment;

	if (!output || output->socket_handle < 0 ||
	    output->transport != STAR6E_OUTPUT_TRANSPORT_UDP ||
	    !packet || packet_size == 0) {
		return -1;
	}

	if (packet_size <= max_size) {
		(void)sendto(output->socket_handle, packet, packet_size, 0,
			(const struct sockaddr *)&output->dst, sizeof(output->dst));
		return 0;
	}

	if (max_size <= payload_offset || packet_size <= payload_offset)
		return 0;

	payload_size = packet_size - payload_offset;
	payload = packet + payload_offset;
	marker = packet[1] & 0x80;
	sequence = star6e_read_be16(packet + 2);
	timestamp = star6e_read_be32(packet + 4);
	ssrc_id = star6e_read_be32(packet + 8);
	max_fragment = max_size - payload_offset;

	while (offset < payload_size) {
		uint8_t fragment_header[STAR6E_RTP_HEADER_SIZE];
		struct iovec vec[2];
		struct msghdr msg;
		uint32_t fragment_size = payload_size - offset;

		if (fragment_size > max_fragment)
			fragment_size = max_fragment;

		memcpy(fragment_header, packet, STAR6E_RTP_HEADER_SIZE);
		fragment_header[1] = (uint8_t)((packet[1] & 0x7f) |
			((offset + fragment_size >= payload_size) ? marker : 0));
		star6e_write_be16(fragment_header + 2, sequence++);
		star6e_write_be32(fragment_header + 4, timestamp);
		star6e_write_be32(fragment_header + 8, ssrc_id);

		vec[0].iov_base = fragment_header;
		vec[0].iov_len = sizeof(fragment_header);
		vec[1].iov_base = (void *)(payload + offset);
		vec[1].iov_len = fragment_size;

		memset(&msg, 0, sizeof(msg));
		msg.msg_name = (void *)&output->dst;
		msg.msg_namelen = sizeof(output->dst);
		msg.msg_iov = vec;
		msg.msg_iovlen = 2;

		(void)sendmsg(output->socket_handle, &msg, 0);
		offset += fragment_size;
	}

	return 0;
}

size_t star6e_output_send_compact_frame(const Star6eOutput *output,
	const MI_VENC_Stream_t *stream, uint32_t max_size)
{
	size_t total_bytes = 0;
	unsigned int i;

	if (!output || !stream)
		return 0;

	for (i = 0; i < stream->count; ++i) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];

		if (!pack->data)
			continue;

		if (pack->packNum > 0) {
			const unsigned int info_cap = (unsigned int)(sizeof(pack->packetInfo) /
				sizeof(pack->packetInfo[0]));
			unsigned int nal_count = (unsigned int)pack->packNum;
			unsigned int k;

			if (nal_count > info_cap)
				nal_count = info_cap;

			for (k = 0; k < nal_count; ++k) {
				MI_U32 length = pack->packetInfo[k].length;
				MI_U32 offset = pack->packetInfo[k].offset;

				if (length == 0 || offset >= pack->length ||
				    length > (pack->length - offset)) {
					continue;
				}

				total_bytes += length;
				(void)star6e_output_send_compact_packet(output,
					pack->data + offset, length, max_size);
			}
			continue;
		}

		if (pack->length > pack->offset) {
			MI_U32 length = pack->length - pack->offset;

			total_bytes += length;
			(void)star6e_output_send_compact_packet(output,
				pack->data + pack->offset, length, max_size);
		}
	}

	return total_bytes;
}

size_t star6e_output_send_frame(const Star6eOutput *output,
	const MI_VENC_Stream_t *stream, uint32_t max_size,
	Star6eOutputRtpSendFn rtp_send, void *opaque)
{
	if (!output || !stream)
		return 0;

	if (star6e_output_is_rtp(output)) {
		if (!rtp_send)
			return 0;
		return rtp_send(output, stream, opaque);
	}

	return star6e_output_send_compact_frame(output, stream, max_size);
}

int star6e_output_apply_server(Star6eOutput *output, const char *uri)
{
	char host[128];
	uint16_t port;

	if (!output || output->transport != STAR6E_OUTPUT_TRANSPORT_UDP || !uri)
		return -1;

	if (venc_config_parse_server_uri(uri, host, sizeof(host), &port) != 0)
		return -1;

	/* Create socket on first use (startup with outgoing.enabled=false
	 * skips socket creation; the API may set a server later). */
	if (output->socket_handle < 0) {
		output->socket_handle = socket(AF_INET, SOCK_DGRAM, 0);
		if (output->socket_handle < 0) {
			fprintf(stderr, "ERROR: Unable to create UDP socket\n");
			return -1;
		}
	}

	output->dst.sin_family = AF_INET;
	output->dst.sin_port = htons(port);
	output->dst.sin_addr.s_addr = inet_addr(host);
	if (output->connected_udp) {
		if (connect(output->socket_handle, (struct sockaddr *)&output->dst,
		    sizeof(output->dst)) != 0) {
			fprintf(stderr, "WARNING: UDP connect to %s:%u failed (%d)\n",
				host, port, errno);
		}
	}

	return 0;
}

void star6e_output_teardown(Star6eOutput *output)
{
	if (!output)
		return;

	if (output->ring) {
		venc_ring_destroy(output->ring);
		output->ring = NULL;
	}
	if (output->socket_handle >= 0) {
		close(output->socket_handle);
		output->socket_handle = -1;
	}

	memset(&output->dst, 0, sizeof(output->dst));
	output->connected_udp = 0;
	output->transport = STAR6E_OUTPUT_TRANSPORT_UDP;
}

void star6e_audio_output_reset(Star6eAudioOutput *audio_output)
{
	if (!audio_output)
		return;

	memset(audio_output, 0, sizeof(*audio_output));
	audio_output->socket_handle = -1;
}

int star6e_audio_output_init(Star6eAudioOutput *audio_output,
	const Star6eOutput *video_output, uint16_t port_override,
	uint16_t max_payload_size)
{
	if (!audio_output)
		return -1;

	star6e_audio_output_reset(audio_output);
	if (!video_output)
		return -1;

	audio_output->video_output = video_output;
	audio_output->port_override = port_override;
	audio_output->max_payload_size = max_payload_size;
	if (port_override == 0) {
		audio_output->socket_handle = video_output->socket_handle;
		return 0;
	}

	audio_output->socket_handle = socket(AF_INET, SOCK_DGRAM, 0);
	if (audio_output->socket_handle < 0) {
		fprintf(stderr, "[audio] ERROR: cannot create audio UDP socket\n");
		return -1;
	}

	return 0;
}

uint16_t star6e_audio_output_port(const Star6eAudioOutput *audio_output)
{
	struct sockaddr_in dst;

	if (!audio_output ||
	    star6e_audio_output_resolve_destination(audio_output, &dst) != 0) {
		return 0;
	}

	return ntohs(dst.sin_port);
}

int star6e_audio_output_send_rtp(const Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len, RtpPacketizerState *rtp_state,
	uint32_t frame_ticks)
{
	if (!audio_output || !data || len == 0 || !rtp_state)
		return -1;

	if (rtp_packetizer_send_packet(rtp_state, star6e_audio_output_write_rtp,
	    (void *)audio_output, data, len, NULL, 0, 0) != 0) {
		return -1;
	}

	rtp_state->timestamp += frame_ticks;
	return 0;
}

int star6e_audio_output_send_compact(const Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len)
{
	struct sockaddr_in dst;
	uint16_t max_data;
	size_t offset = 0;

	if (!audio_output || !data || len == 0 || audio_output->socket_handle < 0 ||
	    star6e_audio_output_resolve_destination(audio_output, &dst) != 0) {
		return -1;
	}

	max_data = audio_output->max_payload_size > 4 ?
		(uint16_t)(audio_output->max_payload_size - 4) : 0;
	if (max_data == 0)
		return 0;

	while (offset < len) {
		uint8_t hdr[4];
		struct iovec vec[2];
		struct msghdr msg;
		size_t chunk = len - offset;

		if (chunk > max_data)
			chunk = max_data;

		hdr[0] = 0xAA;
		hdr[1] = 0x01;
		hdr[2] = (uint8_t)((chunk >> 8) & 0xFF);
		hdr[3] = (uint8_t)(chunk & 0xFF);

		vec[0].iov_base = hdr;
		vec[0].iov_len = sizeof(hdr);
		vec[1].iov_base = (void *)(data + offset);
		vec[1].iov_len = chunk;

		memset(&msg, 0, sizeof(msg));
		msg.msg_name = (void *)&dst;
		msg.msg_namelen = sizeof(dst);
		msg.msg_iov = vec;
		msg.msg_iovlen = 2;
		(void)sendmsg(audio_output->socket_handle, &msg, 0);
		offset += chunk;
	}

	return 0;
}

int star6e_audio_output_send(const Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len, RtpPacketizerState *rtp_state,
	uint32_t frame_ticks)
{
	if (audio_output && audio_output->video_output &&
	    star6e_output_is_rtp(audio_output->video_output)) {
		return star6e_audio_output_send_rtp(audio_output, data, len,
			rtp_state, frame_ticks);
	}

	return star6e_audio_output_send_compact(audio_output, data, len);
}

void star6e_audio_output_teardown(Star6eAudioOutput *audio_output)
{
	if (!audio_output)
		return;

	if (audio_output->socket_handle >= 0 &&
	    audio_output->port_override != 0) {
		close(audio_output->socket_handle);
	}
	star6e_audio_output_reset(audio_output);
}
