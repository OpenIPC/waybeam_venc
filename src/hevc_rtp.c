#include "hevc_rtp.h"

#include "h26x_util.h"

#include <string.h>

static void apply_stats(HevcRtpStats *stats, const RtpPacketizerResult *result)
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

void hevc_rtp_ap_reset(HevcApBuilder *ap)
{
	if (!ap)
		return;
	ap->_internal.payload_len = 0;
	ap->_internal.nal_bytes = 0;
	ap->_internal.nal_count = 0;
	ap->_internal.forbidden_zero = 0;
	ap->_internal.layer_id = 0;
	ap->_internal.tid_plus1 = 1;
}

static int ap_can_add(const HevcApBuilder *ap, size_t nal_len,
	size_t max_payload)
{
	size_t payload_len;

	if (!ap || nal_len == 0 || nal_len > UINT16_MAX)
		return 0;

	payload_len = ap->_internal.payload_len;
	if (ap->_internal.nal_count == 0)
		payload_len = 2;
	return (payload_len + 2 + nal_len) <= max_payload;
}

static void ap_add(HevcApBuilder *ap, const uint8_t *nal, size_t nal_len)
{
	size_t offset;
	uint8_t layer_id;
	uint8_t tid_plus1;

	if (!ap || !nal || nal_len == 0)
		return;

	layer_id = h26x_util_hevc_get_layer_id(nal, nal_len);
	tid_plus1 = h26x_util_hevc_get_tid_plus1(nal, nal_len);

	if (ap->_internal.nal_count == 0) {
		ap->_internal.payload_len = 2;
		ap->_internal.forbidden_zero = (uint8_t)(nal[0] & 0x80);
		ap->_internal.layer_id = layer_id;
		ap->_internal.tid_plus1 = tid_plus1;
	} else {
		ap->_internal.forbidden_zero |= (uint8_t)(nal[0] & 0x80);
		if (layer_id < ap->_internal.layer_id)
			ap->_internal.layer_id = layer_id;
		if (tid_plus1 < ap->_internal.tid_plus1)
			ap->_internal.tid_plus1 = tid_plus1;
	}

	offset = ap->_internal.payload_len;
	ap->_internal.payload[offset] = (uint8_t)((nal_len >> 8) & 0xFF);
	ap->_internal.payload[offset + 1] = (uint8_t)(nal_len & 0xFF);
	memcpy(ap->_internal.payload + offset + 2, nal, nal_len);
	ap->_internal.payload_len += 2 + nal_len;
	ap->_internal.nal_bytes += nal_len;
	ap->_internal.nal_count++;
}

static int send_single_rtp_packet(RtpPacketizerState *rtp,
	RtpPacketizerWriteFn write_cb, void *opaque, const uint8_t *payload,
	size_t payload_len, int marker)
{
	if (!payload || payload_len == 0 || !rtp)
		return -1;

	return rtp_packetizer_send_packet(rtp, write_cb, opaque, payload,
		payload_len, NULL, 0, marker);
}

size_t hevc_rtp_ap_flush(HevcApBuilder *ap, RtpPacketizerState *rtp,
	RtpPacketizerWriteFn write_cb, void *opaque, int marker,
	HevcRtpStats *stats)
{
	size_t total_bytes = 0;

	if (!ap || ap->_internal.nal_count == 0 || !rtp)
		return 0;

	if (ap->_internal.nal_count == 1) {
		size_t nal_len = ((size_t)ap->_internal.payload[2] << 8) |
			ap->_internal.payload[3];
		const uint8_t *nal = ap->_internal.payload + 4;

		if (send_single_rtp_packet(rtp, write_cb, opaque, nal, nal_len,
		    marker) == 0) {
			total_bytes = nal_len;
			if (stats) {
				stats->single_packets++;
				stats->rtp_packets++;
				stats->rtp_payload_bytes += (uint32_t)nal_len;
			}
		}
		hevc_rtp_ap_reset(ap);
		return total_bytes;
	}

	ap->_internal.payload[0] = (uint8_t)(ap->_internal.forbidden_zero |
		(48 << 1) | ((ap->_internal.layer_id >> 5) & 0x01));
	ap->_internal.payload[1] = (uint8_t)(((ap->_internal.layer_id & 0x1F) << 3) |
		(ap->_internal.tid_plus1 & 0x07));

	if (send_single_rtp_packet(rtp, write_cb, opaque, ap->_internal.payload,
	    ap->_internal.payload_len, marker) == 0) {
		total_bytes = ap->_internal.nal_bytes;
		if (stats) {
			stats->ap_packets++;
			stats->ap_nals += ap->_internal.nal_count;
			stats->rtp_packets++;
			stats->rtp_payload_bytes += (uint32_t)ap->_internal.payload_len;
		}
	}

	hevc_rtp_ap_reset(ap);
	return total_bytes;
}

static size_t send_nal_direct(const uint8_t *data, size_t length,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	int is_last, size_t max_payload, HevcRtpStats *stats)
{
	RtpPacketizerResult result;
	size_t total_bytes;

	if (!data || length == 0 || !rtp)
		return 0;

	total_bytes = rtp_packetizer_send_hevc_nal(rtp, write_cb, opaque, data,
		length, is_last, max_payload, stats ? &result : NULL);
	if (stats)
		apply_stats(stats, &result);
	return total_bytes;
}

size_t hevc_rtp_send_nal(HevcApBuilder *ap, const uint8_t *data, size_t len,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	int is_last, size_t max_payload, HevcRtpStats *stats)
{
	size_t total_bytes = 0;

	if (!data || len == 0 || !rtp || !ap)
		return 0;

	if (stats)
		stats->total_nals++;

	if (len > max_payload || len > UINT16_MAX) {
		total_bytes += hevc_rtp_ap_flush(ap, rtp, write_cb, opaque, 0, stats);
		total_bytes += send_nal_direct(data, len, rtp, write_cb, opaque,
			is_last, max_payload, stats);
		return total_bytes;
	}

	if (!ap_can_add(ap, len, max_payload)) {
		total_bytes += hevc_rtp_ap_flush(ap, rtp, write_cb, opaque, 0, stats);
		if (!ap_can_add(ap, len, max_payload)) {
			total_bytes += send_nal_direct(data, len, rtp, write_cb,
				opaque, is_last, max_payload, stats);
			return total_bytes;
		}
	}

	ap_add(ap, data, len);
	if (is_last)
		total_bytes += hevc_rtp_ap_flush(ap, rtp, write_cb, opaque, 1, stats);
	return total_bytes;
}

size_t hevc_rtp_prepend_param_sets(const H26xParamSets *params, uint8_t nal_type,
	HevcApBuilder *ap, RtpPacketizerState *rtp,
	RtpPacketizerWriteFn write_cb, void *opaque, size_t max_payload,
	HevcRtpStats *stats)
{
	H26xParamSetRef refs[3];
	size_t count;
	size_t total_bytes = 0;
	size_t i;

	if (!params || !rtp || !ap)
		return 0;

	count = h26x_param_sets_get_prepend(params, PT_H265, nal_type, refs,
		sizeof(refs) / sizeof(refs[0]));
	for (i = 0; i < count; ++i) {
		total_bytes += hevc_rtp_send_nal(ap, refs[i].data, refs[i].len,
			rtp, write_cb, opaque, 0, max_payload, stats);
	}

	return total_bytes;
}
