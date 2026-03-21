#include "star6e_hevc_rtp.h"

#include "h26x_util.h"

#include <string.h>

typedef struct {
	uint8_t payload[RTP_BUFFER_MAX];
	size_t payload_len;
	size_t nal_bytes;
	uint16_t nal_count;
	uint8_t forbidden_zero;
	uint8_t layer_id;
	uint8_t tid_plus1;
} HevcApBuilder;

static void star6e_hevc_rtp_apply_stats(Star6eHevcRtpStats *stats,
	const RtpPacketizerResult *result)
{
	if (!stats || !result || result->packet_count == 0)
		return;

	if (result->fragmented)
		stats->fu_packets += result->packet_count;
	else
		stats->single_packets += result->packet_count;
	stats->rtp_packets += result->packet_count;
	stats->rtp_payload_bytes += result->payload_bytes;
}

static int star6e_hevc_rtp_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	return star6e_output_send_rtp_parts(opaque, header, header_len,
		payload1, payload1_len, payload2, payload2_len);
}

static void hevc_ap_reset(HevcApBuilder *ap)
{
	if (!ap)
		return;
	ap->payload_len = 0;
	ap->nal_bytes = 0;
	ap->nal_count = 0;
	ap->forbidden_zero = 0;
	ap->layer_id = 0;
	ap->tid_plus1 = 1;
}

static int hevc_ap_can_add(const HevcApBuilder *ap, size_t nal_len,
	size_t max_payload)
{
	size_t payload_len;

	if (!ap || nal_len == 0 || nal_len > UINT16_MAX)
		return 0;

	payload_len = ap->payload_len;
	if (ap->nal_count == 0)
		payload_len = 2;
	return (payload_len + 2 + nal_len) <= max_payload;
}

static void hevc_ap_add(HevcApBuilder *ap, const uint8_t *nal, size_t nal_len)
{
	size_t offset;
	uint8_t layer_id;
	uint8_t tid_plus1;

	if (!ap || !nal || nal_len == 0)
		return;

	layer_id = h26x_util_hevc_get_layer_id(nal, nal_len);
	tid_plus1 = h26x_util_hevc_get_tid_plus1(nal, nal_len);

	if (ap->nal_count == 0) {
		ap->payload_len = 2;
		ap->forbidden_zero = (uint8_t)(nal[0] & 0x80);
		ap->layer_id = layer_id;
		ap->tid_plus1 = tid_plus1;
	} else {
		ap->forbidden_zero |= (uint8_t)(nal[0] & 0x80);
		if (layer_id < ap->layer_id)
			ap->layer_id = layer_id;
		if (tid_plus1 < ap->tid_plus1)
			ap->tid_plus1 = tid_plus1;
	}

	offset = ap->payload_len;
	ap->payload[offset] = (uint8_t)((nal_len >> 8) & 0xFF);
	ap->payload[offset + 1] = (uint8_t)(nal_len & 0xFF);
	memcpy(ap->payload + offset + 2, nal, nal_len);
	ap->payload_len += 2 + nal_len;
	ap->nal_bytes += nal_len;
	ap->nal_count++;
}

static int send_rtp_packet(const Star6eOutput *output, const uint8_t *payload,
	size_t payload_len, RtpPacketizerState *rtp, int marker)
{
	if (!output || !payload || payload_len == 0 || !rtp)
		return 0;

	return rtp_packetizer_send_packet(rtp, star6e_hevc_rtp_write,
		(void *)output, payload, payload_len, NULL, 0, marker);
}

static size_t hevc_ap_flush(HevcApBuilder *ap, const Star6eOutput *output,
	RtpPacketizerState *rtp, int marker, Star6eHevcRtpStats *stats)
{
	size_t total_bytes = 0;

	if (!ap || ap->nal_count == 0 || !rtp)
		return 0;

	if (ap->nal_count == 1) {
		size_t nal_len = ((size_t)ap->payload[2] << 8) | ap->payload[3];
		const uint8_t *nal = ap->payload + 4;

		if (send_rtp_packet(output, nal, nal_len, rtp, marker) == 0) {
			total_bytes = nal_len;
			if (stats) {
				stats->single_packets++;
				stats->rtp_packets++;
				stats->rtp_payload_bytes += (uint32_t)nal_len;
			}
		}
		hevc_ap_reset(ap);
		return total_bytes;
	}

	ap->payload[0] = (uint8_t)(ap->forbidden_zero | (48 << 1) |
		((ap->layer_id >> 5) & 0x01));
	ap->payload[1] = (uint8_t)(((ap->layer_id & 0x1F) << 3) |
		(ap->tid_plus1 & 0x07));

	if (send_rtp_packet(output, ap->payload, ap->payload_len, rtp, marker) == 0) {
		total_bytes = ap->nal_bytes;
		if (stats) {
			stats->ap_packets++;
			stats->ap_nals += ap->nal_count;
			stats->rtp_packets++;
			stats->rtp_payload_bytes += (uint32_t)ap->payload_len;
		}
	}

	hevc_ap_reset(ap);
	return total_bytes;
}

static size_t send_nal_rtp_hevc(const uint8_t *data, size_t length,
	const Star6eOutput *output, RtpPacketizerState *rtp, int is_last,
	size_t max_payload, Star6eHevcRtpStats *stats)
{
	RtpPacketizerResult result;
	size_t total_bytes;

	if (!data || length == 0 || !rtp)
		return 0;

	total_bytes = rtp_packetizer_send_hevc_nal(rtp, star6e_hevc_rtp_write,
		(void *)output, data, length, is_last, max_payload,
		stats ? &result : NULL);
	if (stats)
		star6e_hevc_rtp_apply_stats(stats, &result);
	return total_bytes;
}

static size_t send_or_queue_nal_rtp_hevc(const uint8_t *data, size_t length,
	const Star6eOutput *output, RtpPacketizerState *rtp, HevcApBuilder *ap,
	int is_last, size_t max_payload, Star6eHevcRtpStats *stats)
{
	size_t total_bytes = 0;

	if (!data || length == 0 || !rtp || !ap)
		return 0;

	if (stats)
		stats->total_nals++;

	if (length > max_payload || length > UINT16_MAX) {
		total_bytes += hevc_ap_flush(ap, output, rtp, 0, stats);
		total_bytes += send_nal_rtp_hevc(data, length, output, rtp,
			is_last, max_payload, stats);
		return total_bytes;
	}

	if (!hevc_ap_can_add(ap, length, max_payload)) {
		total_bytes += hevc_ap_flush(ap, output, rtp, 0, stats);
		if (!hevc_ap_can_add(ap, length, max_payload)) {
			total_bytes += send_nal_rtp_hevc(data, length, output, rtp,
				is_last, max_payload, stats);
			return total_bytes;
		}
	}

	hevc_ap_add(ap, data, length);
	if (is_last)
		total_bytes += hevc_ap_flush(ap, output, rtp, 1, stats);
	return total_bytes;
}

static size_t send_prepend_param_sets_hevc(const H26xParamSets *params,
	uint8_t nal_type, const Star6eOutput *output, RtpPacketizerState *rtp,
	HevcApBuilder *ap, size_t max_payload, Star6eHevcRtpStats *stats)
{
	H26xParamSetRef refs[3];
	size_t count;
	size_t total_bytes = 0;

	if (!params || !rtp || !ap)
		return 0;

	count = h26x_param_sets_get_prepend(params, PT_H265, nal_type, refs,
		sizeof(refs) / sizeof(refs[0]));
	for (size_t i = 0; i < count; ++i) {
		total_bytes += send_or_queue_nal_rtp_hevc(refs[i].data, refs[i].len,
			output, rtp, ap, 0, max_payload, stats);
	}

	return total_bytes;
}

size_t star6e_hevc_rtp_send_frame(const MI_VENC_Stream_t *stream,
	const Star6eOutput *output, RtpPacketizerState *rtp,
	uint32_t frame_ticks, H26xParamSets *params, size_t max_payload,
	Star6eHevcRtpStats *stats)
{
	size_t total_bytes = 0;
	HevcApBuilder ap;

	if (!stream || !output || !rtp)
		return 0;

	if (max_payload > RTP_BUFFER_MAX)
		max_payload = RTP_BUFFER_MAX;

	hevc_ap_reset(&ap);

	for (unsigned int i = 0; i < stream->count; ++i) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];
		const unsigned int info_cap = (unsigned int)(sizeof(pack->packetInfo) /
			sizeof(pack->packetInfo[0]));
		unsigned int nal_count = (pack->packNum > 0) ?
			(unsigned int)pack->packNum : 1;

		if (pack->packNum > 0 && nal_count > info_cap)
			nal_count = info_cap;
		if (!pack->data)
			continue;

		for (unsigned int k = 0; k < nal_count; ++k) {
			const uint8_t *data = NULL;
			size_t length = 0;
			const uint8_t *nal_ptr;
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
				if (pack->length <= pack->offset)
					continue;
				data = pack->data + pack->offset;
				length = pack->length - pack->offset;
			}

			if (!data || length == 0)
				continue;

			nal_ptr = data;
			nal_len = length;
			h26x_util_strip_start_code(&nal_ptr, &nal_len);
			if (nal_len == 0)
				continue;

			nal_type = h26x_util_hevc_nalu_type(nal_ptr, nal_len);
			if (pack->packNum > 0)
				nal_type = (uint8_t)pack->packetInfo[k].packType.h265Nalu;

			if (params)
				h26x_param_sets_update(params, PT_H265, nal_type, nal_ptr, nal_len);

			last_nal = (i == stream->count - 1) &&
				((pack->packNum > 0 && k == nal_count - 1) ||
				 (pack->packNum == 0));

			if (params) {
				total_bytes += send_prepend_param_sets_hevc(params, nal_type,
					output, rtp, &ap, max_payload, stats);
			}

			total_bytes += send_or_queue_nal_rtp_hevc(nal_ptr, nal_len, output,
				rtp, &ap, last_nal, max_payload, stats);
		}
	}

	total_bytes += hevc_ap_flush(&ap, output, rtp, 1, stats);
	rtp->timestamp += frame_ticks;
	return total_bytes;
}
