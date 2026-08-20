#include "maruko_video.h"

#include "h26x_util.h"
#include "hevc_rtp.h"
#include "timing.h"
#include "rtp_packetizer.h"
#include "rtp_session.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

/* MARUKO_REFTYPE_ENHANCE_P_NOTFORREF (=5) is defined in maruko_video.h. */

typedef struct {
	MarukoOutput *output;
	venc_ring_t *ring;
} MarukoRtpWriteContext;

static int maruko_rtp_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	const MarukoRtpWriteContext *ctx = opaque;
	MarukoOutput *output;

	if (!ctx || !header || !payload1 || header_len == 0 || payload1_len == 0)
		return -1;

	/* SHM path: write RTP packet to ring buffer (zero pre-flatten copy). */
	if (ctx->ring) {
		size_t total_payload = payload1_len + payload2_len;
		if (header_len > UINT16_MAX || total_payload > UINT16_MAX)
			return -1;
		return venc_ring_write3(ctx->ring,
			header, (uint16_t)header_len,
			payload1, (uint16_t)payload1_len,
			payload2, (uint16_t)payload2_len);
	}

	output = ctx->output;
	if (!output)
		return -1;

	/* Batched path: enqueue into sendmmsg() batch when active. */
	if (output->batch.active) {
		if (maruko_output_batch_enqueue(output, header, header_len,
		    payload1, payload1_len, payload2, payload2_len) == 0)
			return 0;
		/* Scratch slot too small for this packet — flush anything
		 * already queued (to preserve ordering), then fall through to
		 * immediate send. */
		(void)maruko_output_end_frame(output);
		/* Reopen the batch for subsequent packets in this frame. */
		maruko_output_begin_frame(output);
	}

	/* Fallback: either batch inactive or packet too big for scratch —
	 * send immediately via sendmsg(). */
	if (output_socket_send_parts(output->socket_handle, &output->dst,
	    output->dst_len, output->connected_udp,
	    header, header_len, payload1, payload1_len,
	    payload2, payload2_len) != 0) {
		maruko_output_account_send_failure(output, output->socket_handle, 1);
		return -1;
	}
	__atomic_fetch_add(&output->socket_writes, 1, __ATOMIC_RELAXED);
	return 0;
}

/* H.265-only frame sender: iterates the stream, delegates AP aggregation
 * and FU-A fragmentation to the shared hevc_rtp module. */
static size_t maruko_send_frame_hevc(const i6c_venc_strm *stream,
	MarukoRtpWriteContext *ctx, MarukoRtpState *rtp, H26xParamSets *params,
	size_t max_payload, HevcRtpStats *stats)
{
	RtpPacketizerState state;
	size_t total_bytes = 0;
	unsigned int i;

	state.seq = rtp->seq;
	state.timestamp = rtp->timestamp;
	state.ssrc = rtp->ssrc;
	state.payload_type = rtp->payload_type;

	if (max_payload > RTP_BUFFER_MAX)
		max_payload = RTP_BUFFER_MAX;

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
					nal_type, &state, maruko_rtp_write,
					ctx, max_payload, stats);
			}

			total_bytes += hevc_rtp_send_nal(nal_ptr, nal_len,
				&state, maruko_rtp_write, ctx, last_nal,
				max_payload, stats);
		}
	}

	rtp->seq = state.seq;
	return total_bytes;
}

static size_t maruko_send_frame_rtp(const i6c_venc_strm *stream,
	MarukoOutput *output, MarukoRtpState *rtp, H26xParamSets *params,
	PAYLOAD_TYPE_E codec, size_t max_payload, HevcRtpStats *stats)
{
	MarukoRtpWriteContext ctx = {
		.output = output,
		.ring = output ? output->ring : NULL,
	};
	size_t total_bytes;

	if (!stream || !output || !rtp)
		return 0;
	/* HEVC is the only supported codec since 0.10.12 — defensive
	 * non-PT_H265 branch removed. */
	(void)codec;

	/* Open sendmmsg batch for UDP; no-op for SHM. */
	maruko_output_begin_frame(output);
	total_bytes = maruko_send_frame_hevc(stream, &ctx, rtp, params,
		max_payload, stats);
	(void)maruko_output_end_frame(output);
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

static size_t maruko_send_frame_ring(const i6c_venc_strm *stream,
	MarukoOutput *output)
{
	VencFrameMeta meta;
	venc_frame_ring_t *frame_ring;
	size_t total_bytes = 0;
	unsigned int i;
	int is_idr = 0;

	if (!stream || !output || !output->frame_ring)
		return 0;

	frame_ring = output->frame_ring;

	/* IDR detection from packType.h265Nalu (types 19, 20) */
	for (i = 0; i < stream->count && !is_idr; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];
		if (pack->packNum > 0) {
			const unsigned int info_cap = (unsigned int)(
				sizeof(pack->packetInfo) /
				sizeof(pack->packetInfo[0]));
			unsigned int nal_count = (unsigned int)pack->packNum;
			unsigned int k;
			if (nal_count > info_cap)
				nal_count = info_cap;
			for (k = 0; k < nal_count; ++k) {
				uint8_t nt = (uint8_t)pack->packetInfo[k]
					.packType.h265Nalu;
				if (nt == 19 || nt == 20) {
					is_idr = 1;
					break;
				}
			}
		}
	}

	memset(&meta, 0, sizeof(meta));
	meta.pts = (stream->count > 0 && stream->packet)
		? (uint32_t)stream->packet[0].timestamp : 0;
	meta.codec = VENC_FRAME_CODEC_H265;
	meta.flags = is_idr ? VENC_FRAME_FLAG_IDR : 0;
	if (!is_idr && output->gdr_active) {
		meta.flags |= VENC_FRAME_FLAG_GDR;
		meta.gdr_pos = output->gdr_counter;
		meta.gdr_len = output->gdr_cycle_len;
		output->gdr_counter++;
		if (output->gdr_counter >= output->gdr_cycle_len)
			output->gdr_counter = 0;
	} else if (is_idr) {
		output->gdr_counter = 0;
	}
	if (output->svct_active &&
	    stream->h265Info.refType == MARUKO_REFTYPE_ENHANCE_P_NOTFORREF)
		meta.flags |= VENC_FRAME_FLAG_ENHANCE;

	if (venc_frame_ring_begin_write(frame_ring, &meta) != 0) {
		if (venc_frame_drop_breaks_chain(meta.flags) &&
		    output->request_idr &&
		    venc_frame_drop_idr_due(&output->drop_idr_last_us,
					    wb_monotonic_us()) &&
		    output->request_idr(output->idr_ctx) == 0) {
			/* Swallowed by the shared 100 ms limiter — roll the
			 * holdoff back so the next drop retries. */
			output->drop_idr_last_us = 0;
		}
		return 0;
	}

	for (i = 0; i < stream->count; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];

		if (!pack->data)
			continue;

		if (pack->packNum > 0) {
			const unsigned int info_cap = (unsigned int)(
				sizeof(pack->packetInfo) /
				sizeof(pack->packetInfo[0]));
			unsigned int nal_count = (unsigned int)pack->packNum;
			unsigned int k;

			if (nal_count > info_cap) {
				/* Never ship a frame missing NALs — abort
				 * and heal like a ring-full drop (mirrors
				 * the star6e walker). */
				if (!output->trunc_warned) {
					output->trunc_warned = 1;
					fprintf(stderr,
						"WARN: pack has %u NALs, "
						"packetInfo caps at %u — "
						"frame dropped\n",
						nal_count, info_cap);
				}
				venc_frame_ring_abort_write(frame_ring);
				if (venc_frame_drop_breaks_chain(meta.flags) &&
				    output->request_idr &&
				    venc_frame_drop_idr_due(
					&output->drop_idr_last_us,
					wb_monotonic_us()) &&
				    output->request_idr(output->idr_ctx) == 0)
					output->drop_idr_last_us = 0;
				return 0;
			}

			for (k = 0; k < nal_count; ++k) {
				unsigned int length = pack->packetInfo[k].length;
				unsigned int offset = pack->packetInfo[k].offset;

				if (length == 0 || offset >= pack->length ||
				    length > (pack->length - offset))
					continue;

				if (venc_frame_ring_append(frame_ring,
				    pack->data + offset, length) != 0) {
					venc_frame_ring_abort_write(frame_ring);
					return 0;
				}
				total_bytes += length;
			}
			continue;
		}

		if (pack->length > pack->offset) {
			unsigned int length = pack->length - pack->offset;

			if (venc_frame_ring_append(frame_ring,
			    pack->data + pack->offset, length) != 0) {
				venc_frame_ring_abort_write(frame_ring);
				return 0;
			}
			total_bytes += length;
		}
	}

	venc_frame_ring_commit_write(frame_ring);
	return total_bytes;
}

size_t maruko_video_send_frame(const i6c_venc_strm *stream,
	MarukoOutput *output, MarukoRtpState *rtp,
	H26xParamSets *params, MarukoBackendConfig *cfg, HevcRtpStats *stats)
{
	size_t total_bytes;

	if (!stream || !output || !cfg)
		return 0;

	if (output->frame_ring)
		return maruko_send_frame_ring(stream, output);

	if (output->socket_handle < 0 && !output->ring)
		return 0;

	if (cfg->stream_mode == MARUKO_STREAM_RTP) {
		total_bytes = maruko_send_frame_rtp(stream, output, rtp, params,
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
