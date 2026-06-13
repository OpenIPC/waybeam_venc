#include "hevc_rtp.h"

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

size_t hevc_rtp_send_nal(const uint8_t *data, size_t len,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	int is_last, size_t max_payload, HevcRtpStats *stats)
{
	RtpPacketizerResult result;
	size_t total_bytes;

	if (!data || len == 0 || !rtp || !write_cb)
		return 0;

	if (stats)
		stats->total_nals++;

	total_bytes = rtp_packetizer_send_hevc_nal(rtp, write_cb, opaque, data,
		len, is_last, max_payload, stats ? &result : NULL);
	if (stats)
		apply_stats(stats, &result);
	return total_bytes;
}

size_t hevc_rtp_prepend_param_sets(const H26xParamSets *params, uint8_t nal_type,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	size_t max_payload, HevcRtpStats *stats)
{
	H26xParamSetRef refs[3];
	size_t count;
	size_t total_bytes = 0;
	size_t i;

	if (!params || !rtp)
		return 0;

	count = h26x_param_sets_get_prepend(params, PT_H265, nal_type, refs,
		sizeof(refs) / sizeof(refs[0]));
	for (i = 0; i < count; ++i) {
		total_bytes += hevc_rtp_send_nal(refs[i].data, refs[i].len,
			rtp, write_cb, opaque, 0, max_payload, stats);
	}

	return total_bytes;
}
