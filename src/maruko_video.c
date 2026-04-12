#include "maruko_video.h"

#include "h26x_util.h"
#include "hevc_rtp.h"
#include "rtp_packetizer.h"
#include "rtp_session.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

typedef struct {
	int socket_handle;
	const struct sockaddr_storage *dst;
	socklen_t dst_len;
	int connected_udp;
	venc_ring_t *ring;
} MarukoRtpWriteContext;

static int maruko_rtp_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	const MarukoRtpWriteContext *ctx = opaque;
	struct iovec vec[3];
	struct msghdr msg;
	int iovcnt;

	if (!ctx || !header || !payload1 || header_len == 0 || payload1_len == 0)
		return -1;

	/* SHM path: write RTP packet to ring buffer (flatten payload parts) */
	if (ctx->ring) {
		size_t total_payload = payload1_len + payload2_len;
		if (header_len > UINT16_MAX || total_payload > UINT16_MAX)
			return -1;
		if (payload2 && payload2_len > 0) {
			uint8_t flat[RTP_BUFFER_MAX];
			if (total_payload > sizeof(flat))
				return -1;
			memcpy(flat, payload1, payload1_len);
			memcpy(flat + payload1_len, payload2, payload2_len);
			return venc_ring_write(ctx->ring, header,
				(uint16_t)header_len, flat,
				(uint16_t)total_payload);
		}
		return venc_ring_write(ctx->ring, header, (uint16_t)header_len,
			payload1, (uint16_t)payload1_len);
	}

	/* Socket path */
	if (ctx->socket_handle < 0)
		return -1;
	if (!ctx->connected_udp && (!ctx->dst || ctx->dst_len == 0))
		return -1;

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
	if (ctx->connected_udp) {
		msg.msg_name = NULL;
		msg.msg_namelen = 0;
	} else {
		msg.msg_name = (void *)ctx->dst;
		msg.msg_namelen = ctx->dst_len;
	}
	msg.msg_iov = vec;
	msg.msg_iovlen = iovcnt;
	return sendmsg(ctx->socket_handle, &msg, 0) < 0 ? -1 : 0;
}

/* H.265-only frame sender: iterates the stream, delegates AP aggregation
 * and FU-A fragmentation to the shared hevc_rtp module. */
static size_t maruko_send_frame_hevc(const i6c_venc_strm *stream,
	MarukoRtpWriteContext *ctx, MarukoRtpState *rtp, H26xParamSets *params,
	size_t max_payload, HevcRtpStats *stats)
{
	RtpPacketizerState state;
	HevcApBuilder ap;
	size_t total_bytes = 0;
	unsigned int i;

	state.seq = rtp->seq;
	state.timestamp = rtp->timestamp;
	state.ssrc = rtp->ssrc;
	state.payload_type = rtp->payload_type;

	if (max_payload > RTP_BUFFER_MAX)
		max_payload = RTP_BUFFER_MAX;

	hevc_rtp_ap_reset(&ap);

	for (i = 0; i < stream->count; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];
		const unsigned int info_cap =
			(unsigned int)(sizeof(pack->packetInfo) /
			sizeof(pack->packetInfo[0]));
		unsigned int nal_count;
		unsigned int k;

		if (!pack->data)
			continue;

		nal_count = pack->packNum > 0 ? (unsigned int)pack->packNum : 1;
		if (pack->packNum > 0 && nal_count > info_cap)
			nal_count = info_cap;

		for (k = 0; k < nal_count; ++k) {
			const uint8_t *data = NULL;
			const uint8_t *nal_ptr;
			size_t length = 0;
			size_t nal_len;
			uint8_t nal_type;
			int last_nal;

			if (pack->packNum > 0) {
				MI_U32 offset = pack->packetInfo[k].offset;
				MI_U32 len = pack->packetInfo[k].length;

				if (len == 0 || offset >= pack->length ||
				    len > (pack->length - offset))
					continue;
				data = pack->data + offset;
				length = len;
			} else {
				if (pack->length <= pack->offset)
					continue;
				data = pack->data + pack->offset;
				length = pack->length - pack->offset;
			}

			nal_ptr = data;
			nal_len = length;
			h26x_util_strip_start_code(&nal_ptr, &nal_len);
			if (!nal_ptr || nal_len == 0)
				continue;

			nal_type = h26x_util_hevc_nalu_type(nal_ptr, nal_len);
			if (pack->packNum > 0)
				nal_type = (uint8_t)pack->packetInfo[k].packType.h265Nalu;

			if (params)
				h26x_param_sets_update(params, PT_H265, nal_type,
					nal_ptr, nal_len);

			last_nal = (i == stream->count - 1) &&
				((pack->packNum > 0 && k == nal_count - 1) ||
				 (pack->packNum == 0));

			if (params) {
				total_bytes += hevc_rtp_prepend_param_sets(params,
					nal_type, &ap, &state, maruko_rtp_write,
					ctx, max_payload, stats);
			}

			total_bytes += hevc_rtp_send_nal(&ap, nal_ptr, nal_len,
				&state, maruko_rtp_write, ctx, last_nal,
				max_payload, stats);
		}
	}

	total_bytes += hevc_rtp_ap_flush(&ap, &state, maruko_rtp_write, ctx, 1,
		stats);
	rtp->seq = state.seq;
	return total_bytes;
}

static size_t maruko_send_frame_rtp(const i6c_venc_strm *stream,
	int socket_handle, const struct sockaddr_storage *dst,
	socklen_t dst_len, int connected_udp, venc_ring_t *ring,
	MarukoRtpState *rtp, H26xParamSets *params, PAYLOAD_TYPE_E codec,
	size_t max_payload, HevcRtpStats *stats)
{
	MarukoRtpWriteContext ctx = {
		.socket_handle = socket_handle,
		.dst = dst,
		.dst_len = dst_len,
		.connected_udp = connected_udp,
		.ring = ring,
	};
	size_t total_bytes;

	if (!stream || !dst || !rtp)
		return 0;
	if (codec != PT_H265) {
		static int warned;
		if (!warned) {
			fprintf(stderr,
				"WARNING: Maruko RTP output supports H.265 only; "
				"requested codec %d ignored\n", (int)codec);
			warned = 1;
		}
		return 0;
	}

	total_bytes = maruko_send_frame_hevc(stream, &ctx, rtp, params,
		max_payload, stats);
	rtp->timestamp += rtp->frame_ticks;
	return total_bytes;
}

static size_t maruko_send_udp_chunks(const uint8_t *data, size_t length,
	int socket_handle, const struct sockaddr_storage *dst,
	socklen_t dst_len, int connected_udp, uint32_t max_size)
{
	size_t total_sent = 0;
	size_t chunk_cap;

	if (!data || length == 0 || socket_handle < 0)
		return 0;
	if (!connected_udp && (!dst || dst_len == 0))
		return 0;

	chunk_cap = max_size ? max_size : 1400;
	while (total_sent < length) {
		size_t remaining = length - total_sent;
		size_t chunk = remaining > chunk_cap ? chunk_cap : remaining;
		ssize_t rc;

		if (connected_udp) {
			/* Connected UDP socket: destination was set at
			 * connect() time. Passing a sockaddr here would
			 * return EISCONN on some kernels. */
			rc = send(socket_handle, data + total_sent, chunk, 0);
		} else {
			rc = sendto(socket_handle, data + total_sent, chunk, 0,
				(const struct sockaddr *)dst, dst_len);
		}

		if (rc < 0)
			break;
		total_sent += chunk;
	}
	return total_sent;
}

static size_t maruko_send_frame_compact(const i6c_venc_strm *stream,
	int socket_handle, const struct sockaddr_storage *dst,
	socklen_t dst_len, int connected_udp, uint32_t max_size)
{
	size_t total_bytes = 0;
	unsigned int i;

	if (!stream)
		return 0;
	if (!connected_udp && !dst)
		return 0;

	for (i = 0; i < stream->count; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];

		if (!pack->data)
			continue;

		if (pack->packNum > 0) {
			const unsigned int info_cap =
				(unsigned int)(sizeof(pack->packetInfo) /
				sizeof(pack->packetInfo[0]));
			unsigned int nal_count = (unsigned int)pack->packNum;
			unsigned int k;

			if (nal_count > info_cap)
				nal_count = info_cap;

			for (k = 0; k < nal_count; ++k) {
				MI_U32 length = pack->packetInfo[k].length;
				MI_U32 offset = pack->packetInfo[k].offset;

				if (length == 0 || offset >= pack->length ||
				    length > (pack->length - offset))
					continue;
				total_bytes += maruko_send_udp_chunks(
					pack->data + offset, length,
					socket_handle, dst, dst_len,
					connected_udp, max_size);
			}
		} else if (pack->length > pack->offset) {
			MI_U32 length = pack->length - pack->offset;

			total_bytes += maruko_send_udp_chunks(pack->data +
				pack->offset, length, socket_handle, dst,
				dst_len, connected_udp, max_size);
		}
	}

	return total_bytes;
}

void maruko_video_init_rtp_state(MarukoRtpState *rtp,
	PAYLOAD_TYPE_E codec, uint32_t sensor_fps)
{
	if (!rtp)
		return;

	rtp_session_init(rtp, rtp_session_payload_type(codec), sensor_fps);
}

size_t maruko_video_send_frame(const i6c_venc_strm *stream,
	const MarukoOutput *output, MarukoRtpState *rtp,
	H26xParamSets *params, MarukoBackendConfig *cfg, HevcRtpStats *stats)
{
	size_t total_bytes;

	if (!stream || !output || !cfg)
		return 0;
	if (output->socket_handle < 0 && !output->ring)
		return 0;

	if (cfg->stream_mode == MARUKO_STREAM_RTP) {
		total_bytes = maruko_send_frame_rtp(stream, output->socket_handle,
			&output->dst, output->dst_len, output->connected_udp,
			output->ring, rtp, params,
			cfg->rc_codec, cfg->rtp_payload_size, stats);
	} else if (!output->ring) {
		total_bytes = maruko_send_frame_compact(stream,
			output->socket_handle, &output->dst, output->dst_len,
			output->connected_udp, cfg->max_frame_size);
	} else {
		/* Compact mode not supported over SHM */
		return 0;
	}

	return total_bytes;
}
