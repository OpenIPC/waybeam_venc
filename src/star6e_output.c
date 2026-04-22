#include "star6e_output.h"

#include "output_socket.h"
#include "venc_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
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

static int resolve_shared_audio_target(const Star6eAudioOutput *audio_output,
	Star6eAudioSendTarget *target)
{
	if (!audio_output || !audio_output->video_output || !target)
		return -1;

	if (audio_output->video_output->socket_handle < 0 ||
	    audio_output->video_output->ring ||
	    audio_output->video_output->dst_len == 0) {
		return -1;
	}

	target->socket_handle = audio_output->video_output->socket_handle;
	memcpy(&target->dst, &audio_output->video_output->dst, sizeof(target->dst));
	target->dst_len = audio_output->video_output->dst_len;
	return 0;
}

static int resolve_dedicated_audio_target(const Star6eAudioOutput *audio_output,
	Star6eAudioSendTarget *target)
{
	struct sockaddr_in *udp_dst;

	if (!audio_output || !audio_output->video_output || !target ||
	    audio_output->socket_handle < 0) {
		return -1;
	}

	target->socket_handle = audio_output->socket_handle;
	if (audio_output->video_output->transport == VENC_OUTPUT_URI_UDP &&
	    audio_output->video_output->dst_len == sizeof(struct sockaddr_in)) {
		memcpy(&target->dst, &audio_output->video_output->dst,
			sizeof(target->dst));
		target->dst_len = audio_output->video_output->dst_len;
		udp_dst = (struct sockaddr_in *)&target->dst;
		udp_dst->sin_port = htons(audio_output->port_override);
		return 0;
	}

	memcpy(&target->dst, &audio_output->fallback_dst, sizeof(target->dst));
	target->dst_len = audio_output->fallback_dst_len;
	return 0;
}

static int resolve_audio_target(const Star6eAudioOutput *audio_output,
	Star6eAudioSendTarget *target)
{
	if (!audio_output || !target)
		return -1;
	if (audio_output->port_override == 0)
		return resolve_shared_audio_target(audio_output, target);
	return resolve_dedicated_audio_target(audio_output, target);
}

static int resolve_cached_audio_target(Star6eAudioOutput *ao,
	Star6eAudioSendTarget *target)
{
	uint32_t gen;

	if (!ao || !target || !ao->video_output)
		return -1;

	gen = __atomic_load_n(&ao->video_output->transport_gen, __ATOMIC_ACQUIRE);
	if (ao->cache_valid && ao->cached_gen == gen && !(gen & 1)) {
		*target = ao->cached_target;
		return 0;
	}

	/* Cache miss or writer in progress — resolve from live state */
	if (resolve_audio_target(ao, target) != 0)
		return -1;

	/* Only cache if the generation is stable (even = no write in progress)
	 * and unchanged since we started reading */
	gen = __atomic_load_n(&ao->video_output->transport_gen, __ATOMIC_ACQUIRE);
	if (!(gen & 1)) {
		ao->cached_target = *target;
		ao->cached_gen = gen;
		ao->cache_valid = 1;
	}
	return 0;
}

static int send_audio_rtp(const uint8_t *header,
	size_t header_len, const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	const Star6eAudioSendTarget *target = opaque;

	if (!target)
		return -1;

	return output_socket_send_parts(target->socket_handle, &target->dst,
		target->dst_len, 0 /* audio socket never connect()ed */,
		header, header_len, payload1, payload1_len, payload2, payload2_len);
}

static void star6e_output_setup_reset(Star6eOutputSetup *setup)
{
	if (!setup)
		return;

	memset(setup, 0, sizeof(*setup));
	setup->uri.type = VENC_OUTPUT_URI_UDP;
}

void star6e_output_reset(Star6eOutput *output)
{
	if (!output)
		return;

	memset(output, 0, sizeof(*output));
	output->socket_handle = -1;
	output->transport = VENC_OUTPUT_URI_UDP;
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
	setup->requested_connected_udp = connected_udp ? 1 : 0;
	setup->max_frame_size = max_frame_size;

	if (!server_uri || !server_uri[0])
		return 0;

	setup->has_server = 1;
	if (venc_config_parse_output_uri(server_uri, &setup->uri) != 0)
		return -1;
	if (setup->uri.type == VENC_OUTPUT_URI_SHM) {
		if (setup->stream_mode != STAR6E_STREAM_MODE_RTP) {
			fprintf(stderr, "ERROR: shm:// output requires RTP stream mode.\n");
			return -1;
		}
		return 0;
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
	output->requested_connected_udp = setup->requested_connected_udp;
	if (!setup->has_server)
		return 0;

	if (setup->uri.type == VENC_OUTPUT_URI_SHM) {
		slot_data = (uint32_t)setup->max_frame_size + 12;
		output->ring = venc_ring_create(setup->uri.endpoint, 512, slot_data);
		if (!output->ring) {
			fprintf(stderr, "ERROR: venc_ring_create(%s) failed\n",
				setup->uri.endpoint);
			return -1;
		}

		output->transport = VENC_OUTPUT_URI_SHM;
		memset(&output->dst, 0, sizeof(output->dst));
		output->dst_len = 0;
		output->connected_udp = 0;
		__atomic_fetch_add(&output->transport_gen, 2, __ATOMIC_RELEASE);
		return 0;
	}

	if (output_socket_configure(&output->socket_handle, &output->dst,
	    &output->dst_len, &output->transport, &setup->uri,
	    output->requested_connected_udp, &output->connected_udp) != 0)
		return -1;
	__atomic_fetch_add(&output->transport_gen, 2, __ATOMIC_RELEASE);
	return 0;
}

int star6e_output_is_rtp(const Star6eOutput *output)
{
	return output && output->stream_mode == STAR6E_STREAM_MODE_RTP;
}

int star6e_output_is_shm(const Star6eOutput *output)
{
	return output && output->transport == VENC_OUTPUT_URI_SHM;
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

/* Flush the accumulated batch via sendmmsg().
 *
 * On partial success (sendmmsg returns 0 < n < count) or EINTR, retry
 * from the first unsent message. Only a persistent error (non-EINTR
 * failure on the next unsent message) ends the loop; the remaining
 * unsent packets are counted into output->send_errors so the caller can
 * observe silent drops via star6e_output_drain_send_errors().
 *
 * Returns number of messages successfully sent. Always resets
 * batch->count to 0. */
static int star6e_batch_flush(Star6eOutput *output)
{
	Star6eOutputBatch *b = &output->batch;
	size_t sent_total = 0;
	int fd;

	if (b->count == 0)
		return 0;

	/* Use the batch-snapshotted socket — output->socket_handle can be
	 * mutated by a concurrent apply_server() on the HTTP thread between
	 * begin_frame and here. */
	fd = b->socket_handle;
	if (fd < 0) {
		output->send_errors += (uint32_t)b->count;
		b->count = 0;
		return 0;
	}

	while (sent_total < b->count) {
		int n = sendmmsg(fd, b->msgs + sent_total,
			(unsigned int)(b->count - sent_total), 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			/* Permanent error on the next unsent message:
			 * account remaining as drops and bail. */
			output->send_errors +=
				(uint32_t)(b->count - sent_total);
			break;
		}
		if (n == 0) {
			/* Defensive: sendmmsg returning 0 should not happen,
			 * but treat as permanent to avoid a spin. */
			output->send_errors +=
				(uint32_t)(b->count - sent_total);
			break;
		}
		sent_total += (size_t)n;
	}

	b->count = 0;
	return (int)sent_total;
}

void star6e_output_begin_frame(Star6eOutput *output)
{
	Star6eOutputBatch *b;
	uint32_t gen_before, gen_after;

	if (!output)
		return;
	b = &output->batch;
	b->count = 0;
	b->active = 0;

	/* SHM output is not batched — skip the snapshot entirely. */
	if (output->ring)
		return;

	/* Seqlock read of transport state: retry while apply_server() holds
	 * an odd generation. Matches the writer pattern in
	 * star6e_output_apply_server. */
	for (;;) {
		gen_before = __atomic_load_n(&output->transport_gen,
			__ATOMIC_ACQUIRE);
		if (gen_before & 1u) {
			/* Writer in progress — spin briefly. */
			continue;
		}
		b->socket_handle = output->socket_handle;
		b->dst = output->dst;
		b->dst_len = output->dst_len;
		b->connected_udp = output->connected_udp;
		gen_after = __atomic_load_n(&output->transport_gen,
			__ATOMIC_ACQUIRE);
		if (gen_before == gen_after)
			break;
	}

	b->active = (b->socket_handle >= 0) ? 1 : 0;
}

int star6e_output_end_frame(Star6eOutput *output)
{
	int sent;

	if (!output || !output->batch.active)
		return 0;
	sent = star6e_batch_flush(output);
	output->batch.active = 0;
	return sent;
}

/* Queue one RTP packet into the active batch. Returns 0 on success,
 * -1 if the packet cannot fit the scratch slot (caller falls back to
 * immediate send).
 *
 * Both the header and payload1 are copied into scratch because both live
 * on the caller's stack and are reused between packets (rtp header is
 * built in rtp_packetizer_send_packet; payload1 is either a 3-byte FU-A
 * header or the HevcApBuilder's payload buffer, reset after each AP
 * packet). payload2, when present, is a slice of the VENC stream buffer
 * which remains valid until MI_VENC_ReleaseStream is called in the
 * pipeline after end_frame(), so we keep it as a zero-copy iovec. */
static int star6e_batch_enqueue(Star6eOutput *output,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len)
{
	Star6eOutputBatch *b = &output->batch;
	size_t slot;
	size_t scratch_len = header_len + payload1_len;
	struct iovec *iov;
	struct msghdr *hdr;

	if (scratch_len > STAR6E_OUTPUT_BATCH_SLOT_SCRATCH)
		return -1;

	if (b->count >= STAR6E_OUTPUT_BATCH_MAX)
		star6e_batch_flush(output);

	slot = b->count;
	iov = &b->iov[slot * 2];
	hdr = &b->msgs[slot].msg_hdr;

	/* Copy header + payload1 into owned scratch so the caller can reuse
	 * both stack buffers for the next packet before we flush. */
	memcpy(b->scratch[slot], header, header_len);
	memcpy(b->scratch[slot] + header_len, payload1, payload1_len);
	iov[0].iov_base = b->scratch[slot];
	iov[0].iov_len = scratch_len;

	if (payload2 && payload2_len > 0) {
		iov[1].iov_base = (void *)payload2;
		iov[1].iov_len = payload2_len;
	}

	memset(hdr, 0, sizeof(*hdr));
	/* Use the transport snapshot taken at begin_frame(), not the live
	 * output fields — those may be mutated by apply_server() on the
	 * HTTP thread. */
	if (b->connected_udp) {
		hdr->msg_name = NULL;
		hdr->msg_namelen = 0;
	} else {
		hdr->msg_name = (void *)&b->dst;
		hdr->msg_namelen = b->dst_len;
	}
	hdr->msg_iov = iov;
	hdr->msg_iovlen = (payload2 && payload2_len > 0) ? 2 : 1;
	b->msgs[slot].msg_len = 0;

	b->count++;
	return 0;
}

int star6e_output_send_rtp_parts(Star6eOutput *output,
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
		return venc_ring_write3(output->ring,
			header, (uint16_t)header_len,
			payload1, (uint16_t)payload1_len,
			payload2, (uint16_t)payload2_len);
	}

	if (output->batch.active) {
		if (star6e_batch_enqueue(output, header, header_len,
		    payload1, payload1_len, payload2, payload2_len) == 0)
			return 0;
		/* Scratch slot too small for this packet — flush anything
		 * we already have (to preserve ordering), then fall through
		 * to immediate send for this one packet. */
		star6e_batch_flush(output);
	}

	/* Fallback: either batching inactive (probe/audio/compact paths) or
	 * a packet too big for scratch — send immediately. */
	if (output_socket_send_parts(output->socket_handle, &output->dst,
	    output->dst_len, output->connected_udp,
	    header, header_len, payload1, payload1_len,
	    payload2, payload2_len) != 0) {
		output->send_errors++;
		return -1;
	}
	return 0;
}

int star6e_output_send_compact_packet(Star6eOutput *output,
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
	    output->transport == VENC_OUTPUT_URI_SHM ||
	    !packet || packet_size == 0) {
		return -1;
	}

	if (packet_size <= max_size) {
		ssize_t sent = sendto(output->socket_handle, packet, packet_size, 0,
			(const struct sockaddr *)&output->dst, output->dst_len);

		if (sent < 0) {
			output->send_errors++;
			return -1;
		}
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
		msg.msg_namelen = output->dst_len;
		msg.msg_iov = vec;
		msg.msg_iovlen = 2;
		if (sendmsg(output->socket_handle, &msg, 0) < 0) {
			output->send_errors++;
			return -1;
		}
		offset += fragment_size;
	}

	return 0;
}

size_t star6e_output_send_compact_frame(Star6eOutput *output,
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

size_t star6e_output_send_frame(Star6eOutput *output,
	const MI_VENC_Stream_t *stream, uint32_t max_size,
	Star6eOutputRtpSendFn rtp_send, void *opaque)
{
	size_t total;

	if (!output || !stream)
		return 0;

	if (star6e_output_is_rtp(output)) {
		if (!rtp_send)
			return 0;
		star6e_output_begin_frame(output);
		total = rtp_send(output, stream, opaque);
		star6e_output_end_frame(output);
		return total;
	}

	return star6e_output_send_compact_frame(output, stream, max_size);
}

int star6e_output_apply_server(Star6eOutput *output, const char *uri)
{
	VencOutputUri parsed;

	if (!output || !uri)
		return -1;
	if (output->ring) {
		fprintf(stderr, "ERROR: live switch away from shm:// is not supported "
			"(requires restart)\n");
		return -1;
	}
	if (venc_config_parse_output_uri(uri, &parsed) != 0)
		return -1;
	if (parsed.type == VENC_OUTPUT_URI_SHM) {
		fprintf(stderr, "ERROR: live switch to shm:// is not supported\n");
		return -1;
	}

	__atomic_fetch_add(&output->transport_gen, 1, __ATOMIC_RELEASE); /* odd = writing */
	if (output_socket_configure(&output->socket_handle, &output->dst,
	    &output->dst_len, &output->transport, &parsed,
	    output->requested_connected_udp, &output->connected_udp) != 0) {
		__atomic_fetch_add(&output->transport_gen, 1, __ATOMIC_RELEASE); /* restore even */
		return -1;
	}
	__atomic_fetch_add(&output->transport_gen, 1, __ATOMIC_RELEASE); /* even = stable */
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
	output->dst_len = 0;
	output->connected_udp = 0;
	output->requested_connected_udp = 0;
	output->transport = VENC_OUTPUT_URI_UDP;
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
		return 0;
	}

	audio_output->socket_handle = socket(AF_INET, SOCK_DGRAM, 0);
	if (audio_output->socket_handle < 0) {
		fprintf(stderr, "[audio] ERROR: cannot create audio UDP socket\n");
		return -1;
	}

	if (output_socket_fill_udp_destination("127.0.0.1", port_override,
	    &audio_output->fallback_dst, &audio_output->fallback_dst_len) != 0) {
		close(audio_output->socket_handle);
		audio_output->socket_handle = -1;
		return -1;
	}

	return 0;
}

uint16_t star6e_audio_output_port(const Star6eAudioOutput *audio_output)
{
	const struct sockaddr_in *dst;

	if (!audio_output)
		return 0;
	if (audio_output->port_override != 0)
		return audio_output->port_override;
	if (!audio_output->video_output ||
	    audio_output->video_output->transport != VENC_OUTPUT_URI_UDP ||
	    audio_output->video_output->dst_len != sizeof(*dst)) {
		return 0;
	}

	dst = (const struct sockaddr_in *)&audio_output->video_output->dst;
	return ntohs(dst->sin_port);
}

int star6e_audio_output_send_rtp(Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len, RtpPacketizerState *rtp_state,
	uint32_t frame_ticks)
{
	Star6eAudioSendTarget target;

	if (!audio_output || !data || len == 0 || !rtp_state)
		return -1;
	if (resolve_cached_audio_target(audio_output, &target) != 0)
		return -1;

	if (rtp_packetizer_send_packet(rtp_state, send_audio_rtp, &target,
	    data, len, NULL, 0, 0) != 0) {
		return -1;
	}

	rtp_state->timestamp += frame_ticks;
	return 0;
}

int star6e_audio_output_send_compact(Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len)
{
	Star6eAudioSendTarget target;
	uint16_t max_data;
	size_t offset = 0;

	if (!audio_output || !data || len == 0 ||
	    resolve_cached_audio_target(audio_output, &target) != 0) {
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
		msg.msg_name = (void *)&target.dst;
		msg.msg_namelen = target.dst_len;
		msg.msg_iov = vec;
		msg.msg_iovlen = 2;
		if (sendmsg(target.socket_handle, &msg, 0) < 0)
			return -1;
		offset += chunk;
	}

	return 0;
}

int star6e_audio_output_send(Star6eAudioOutput *audio_output,
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
