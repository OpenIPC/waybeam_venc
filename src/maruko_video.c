#include "maruko_video.h"

#include "h26x_util.h"
#include "rtp_packetizer.h"
#include "rtp_session.h"

#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

typedef struct {
	int socket_handle;
	const struct sockaddr_in *dst;
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

	/* UDP path */
	if (ctx->socket_handle < 0 || !ctx->dst)
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
	msg.msg_name = (void *)ctx->dst;
	msg.msg_namelen = sizeof(*ctx->dst);
	msg.msg_iov = vec;
	msg.msg_iovlen = iovcnt;
	return sendmsg(ctx->socket_handle, &msg, 0) < 0 ? -1 : 0;
}

static int maruko_send_rtp_packet(int socket_handle,
	const struct sockaddr_in *dst, venc_ring_t *ring,
	const uint8_t *payload, size_t payload_len,
	MarukoRtpState *rtp, int marker)
{
	MarukoRtpWriteContext ctx = {
		.socket_handle = socket_handle,
		.dst = dst,
		.ring = ring,
	};
	RtpPacketizerState state;
	int ret;

	if (!payload || payload_len == 0 || socket_handle < 0 || !dst || !rtp) {
		return -1;
	}

	state.seq = rtp->seq;
	state.timestamp = rtp->timestamp;
	state.ssrc = rtp->ssrc;
	state.payload_type = rtp->payload_type;

	ret = rtp_packetizer_send_packet(&state, maruko_rtp_write, &ctx,
		payload, payload_len, NULL, 0, marker);
	rtp->seq = state.seq;
	return ret;
}

static size_t maruko_send_fu_h264(const uint8_t *data, size_t length,
	int socket_handle, const struct sockaddr_in *dst, venc_ring_t *ring,
	MarukoRtpState *rtp, int is_last, size_t max_payload)
{
	const uint8_t nal_header = data[0];
	const uint8_t nal_nri = (uint8_t)(nal_header & 0x60);
	const uint8_t nal_type = (uint8_t)(nal_header & 0x1F);
	const uint8_t fu_indicator = (uint8_t)(nal_nri | 28);
	const uint8_t *payload = data + 1;
	size_t max_fragment;
	size_t remaining;
	size_t total_bytes = 0;
	int start = 1;
	uint8_t fu_hdr[2];
	MarukoRtpWriteContext ctx = {
		.socket_handle = socket_handle,
		.dst = dst,
		.ring = ring,
	};
	RtpPacketizerState state;

	if (!data || length <= 1 || !rtp || max_payload <= 2) {
		return 0;
	}

	if (max_payload > RTP_BUFFER_MAX) {
		max_payload = RTP_BUFFER_MAX;
	}
	max_fragment = max_payload - 2;
	remaining = length - 1;

	state.seq = rtp->seq;
	state.timestamp = rtp->timestamp;
	state.ssrc = rtp->ssrc;
	state.payload_type = rtp->payload_type;

	while (remaining > 0) {
		size_t chunk = remaining > max_fragment ? max_fragment : remaining;
		int end = (remaining == chunk);
		int marker = (end && is_last) ? 1 : 0;

		fu_hdr[0] = fu_indicator;
		fu_hdr[1] = (uint8_t)((start ? 0x80 : 0x00) |
			(end ? 0x40 : 0x00) | nal_type);

		if (rtp_packetizer_send_packet(&state, maruko_rtp_write, &ctx,
		    fu_hdr, 2, payload, chunk, marker) != 0) {
			rtp->seq = state.seq;
			return total_bytes;
		}

		total_bytes += chunk;
		payload += chunk;
		remaining -= chunk;
		start = 0;
	}

	rtp->seq = state.seq;
	return total_bytes + 1;
}

static size_t maruko_send_nal_rtp_h264(const uint8_t *data, size_t length,
	int socket_handle, const struct sockaddr_in *dst, venc_ring_t *ring,
	MarukoRtpState *rtp, int is_last, size_t max_payload)
{
	if (!data || length == 0 || !rtp) {
		return 0;
	}

	if (length <= max_payload) {
		return maruko_send_rtp_packet(socket_handle, dst, ring,
			data, length, rtp, is_last ? 1 : 0) == 0 ? length : 0;
	}

	return maruko_send_fu_h264(data, length, socket_handle, dst, ring,
		rtp, is_last, max_payload);
}

static size_t maruko_send_nal_rtp_hevc(const uint8_t *data, size_t length,
	int socket_handle, const struct sockaddr_in *dst, venc_ring_t *ring,
	MarukoRtpState *rtp, int is_last, size_t max_payload)
{
	MarukoRtpWriteContext ctx = {
		.socket_handle = socket_handle,
		.dst = dst,
		.ring = ring,
	};
	RtpPacketizerState state;
	size_t total_bytes;

	if (!data || length == 0 || !rtp) {
		return 0;
	}

	state.seq = rtp->seq;
	state.timestamp = rtp->timestamp;
	state.ssrc = rtp->ssrc;
	state.payload_type = rtp->payload_type;

	total_bytes = rtp_packetizer_send_hevc_nal(&state, maruko_rtp_write,
		&ctx, data, length, is_last, max_payload, NULL);
	rtp->seq = state.seq;
	return total_bytes;
}

static size_t maruko_send_prepend_param_sets(const H26xParamSets *params,
	PAYLOAD_TYPE_E codec, uint8_t nal_type, int socket_handle,
	const struct sockaddr_in *dst, venc_ring_t *ring,
	MarukoRtpState *rtp, size_t max_payload)
{
	H26xParamSetRef refs[3];
	size_t count;
	size_t total_bytes = 0;
	size_t i;

	if (!params || !rtp) {
		return 0;
	}

	count = h26x_param_sets_get_prepend(params, codec, nal_type, refs,
		sizeof(refs) / sizeof(refs[0]));
	for (i = 0; i < count; ++i) {
		if (codec == PT_H265) {
			total_bytes += maruko_send_nal_rtp_hevc(refs[i].data,
				refs[i].len, socket_handle, dst, ring,
				rtp, 0, max_payload);
		} else {
			total_bytes += maruko_send_nal_rtp_h264(refs[i].data,
				refs[i].len, socket_handle, dst, ring,
				rtp, 0, max_payload);
		}
	}

	return total_bytes;
}

static size_t maruko_send_frame_rtp(const i6c_venc_strm *stream,
	int socket_handle, const struct sockaddr_in *dst, venc_ring_t *ring,
	MarukoRtpState *rtp, H26xParamSets *params, PAYLOAD_TYPE_E codec,
	size_t max_payload)
{
	size_t total_bytes = 0;
	unsigned int i;

	if (!stream || !dst || !rtp) {
		return 0;
	}
	if (codec != PT_H264 && codec != PT_H265) {
		return 0;
	}

	for (i = 0; i < stream->count; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];
		const unsigned int info_cap = (unsigned int)(sizeof(pack->packetInfo) /
			sizeof(pack->packetInfo[0]));
		unsigned int nal_count;
		unsigned int k;

		if (!pack->data) {
			continue;
		}

		nal_count = pack->packNum > 0 ? (unsigned int)pack->packNum : 1;
		if (pack->packNum > 0 && nal_count > info_cap) {
			nal_count = info_cap;
		}

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
				    len > (pack->length - offset)) {
					continue;
				}
				data = pack->data + offset;
				length = len;
			} else {
				if (pack->length <= pack->offset) {
					continue;
				}
				data = pack->data + pack->offset;
				length = pack->length - pack->offset;
			}

			nal_ptr = data;
			nal_len = length;
			h26x_util_strip_start_code(&nal_ptr, &nal_len);
			if (!nal_ptr || nal_len == 0) {
				continue;
			}

			if (codec == PT_H265) {
				nal_type = h26x_util_hevc_nalu_type(nal_ptr, nal_len);
				if (pack->packNum > 0) {
					nal_type =
						(uint8_t)pack->packetInfo[k].packType.h265Nalu;
				}
			} else {
				nal_type = h26x_util_h264_nalu_type(nal_ptr, nal_len);
				if (pack->packNum > 0) {
					nal_type =
						(uint8_t)pack->packetInfo[k].packType.h264Nalu;
				}
			}

			if (params) {
				h26x_param_sets_update(params, codec, nal_type,
					nal_ptr, nal_len);
			}

			last_nal = (i == stream->count - 1) &&
				((pack->packNum > 0 && k == nal_count - 1) ||
				(pack->packNum == 0));

			if (params) {
				total_bytes += maruko_send_prepend_param_sets(params,
					codec, nal_type, socket_handle, dst, ring,
					rtp, max_payload);
			}

			if (codec == PT_H265) {
				total_bytes += maruko_send_nal_rtp_hevc(nal_ptr,
					nal_len, socket_handle, dst, ring, rtp,
					last_nal, max_payload);
			} else {
				total_bytes += maruko_send_nal_rtp_h264(nal_ptr,
					nal_len, socket_handle, dst, ring, rtp,
					last_nal, max_payload);
			}
		}
	}

	rtp->timestamp += rtp->frame_ticks;
	return total_bytes;
}

static size_t maruko_send_udp_chunks(const uint8_t *data, size_t length,
	int socket_handle, const struct sockaddr_in *dst, uint32_t max_size)
{
	size_t total_sent = 0;
	size_t chunk_cap;

	if (!data || length == 0 || socket_handle < 0 || !dst) {
		return 0;
	}

	chunk_cap = max_size ? max_size : 1400;
	while (total_sent < length) {
		size_t remaining = length - total_sent;
		size_t chunk = remaining > chunk_cap ? chunk_cap : remaining;
		ssize_t rc = sendto(socket_handle, data + total_sent, chunk, 0,
			(const struct sockaddr *)dst, sizeof(*dst));

		if (rc < 0) {
			break;
		}
		total_sent += chunk;
	}
	return total_sent;
}

static size_t maruko_send_frame_compact(const i6c_venc_strm *stream,
	int socket_handle, const struct sockaddr_in *dst, uint32_t max_size)
{
	size_t total_bytes = 0;
	unsigned int i;

	if (!stream || !dst) {
		return 0;
	}

	for (i = 0; i < stream->count; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];

		if (!pack->data) {
			continue;
		}

		if (pack->packNum > 0) {
			const unsigned int info_cap =
				(unsigned int)(sizeof(pack->packetInfo) /
				sizeof(pack->packetInfo[0]));
			unsigned int nal_count = (unsigned int)pack->packNum;
			unsigned int k;

			if (nal_count > info_cap) {
				nal_count = info_cap;
			}

			for (k = 0; k < nal_count; ++k) {
				MI_U32 length = pack->packetInfo[k].length;
				MI_U32 offset = pack->packetInfo[k].offset;

				if (length == 0 || offset >= pack->length ||
				    length > (pack->length - offset)) {
					continue;
				}
				total_bytes += maruko_send_udp_chunks(
					pack->data + offset, length, socket_handle,
					dst, max_size);
			}
		} else if (pack->length > pack->offset) {
			MI_U32 length = pack->length - pack->offset;

			total_bytes += maruko_send_udp_chunks(pack->data +
				pack->offset, length, socket_handle, dst,
				max_size);
		}
	}

	return total_bytes;
}

void maruko_video_init_rtp_state(MarukoRtpState *rtp,
	PAYLOAD_TYPE_E codec, uint32_t sensor_fps)
{
	if (!rtp) {
		return;
	}

	rtp_session_init(rtp, rtp_session_payload_type(codec), sensor_fps);
}

size_t maruko_video_send_frame(const i6c_venc_strm *stream,
	const MarukoOutput *output, MarukoRtpState *rtp,
	H26xParamSets *params, MarukoBackendConfig *cfg)
{
	size_t total_bytes;

	if (!stream || !output || !cfg)
		return 0;
	if (output->socket_handle < 0 && !output->ring)
		return 0;

	if (cfg->stream_mode == MARUKO_STREAM_RTP) {
		total_bytes = maruko_send_frame_rtp(stream, output->socket_handle,
			&output->dst, output->ring, rtp, params,
			cfg->rc_codec, cfg->rtp_payload_size);
	} else if (!output->ring) {
		total_bytes = maruko_send_frame_compact(stream,
			output->socket_handle, &output->dst,
			cfg->max_frame_size);
	} else {
		/* Compact mode not supported over SHM */
		return 0;
	}

	return total_bytes;
}
