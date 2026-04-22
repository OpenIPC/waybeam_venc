#include "star6e_hevc_rtp.h"

#include "h26x_util.h"
#include "hevc_rtp.h"

#include <string.h>

static int star6e_hevc_rtp_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	return star6e_output_send_rtp_parts(opaque, header, header_len,
		payload1, payload1_len, payload2, payload2_len);
}

size_t star6e_hevc_rtp_send_frame(const MI_VENC_Stream_t *stream,
	Star6eOutput *output, RtpPacketizerState *rtp,
	uint32_t frame_ticks, H26xParamSets *params, size_t max_payload,
	Star6eHevcRtpStats *stats)
{
	size_t total_bytes = 0;
	HevcApBuilder ap;

	if (!stream || !output || !rtp)
		return 0;

	if (max_payload > RTP_BUFFER_MAX)
		max_payload = RTP_BUFFER_MAX;

	hevc_rtp_ap_reset(&ap);

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
				total_bytes += hevc_rtp_prepend_param_sets(params,
					nal_type, &ap, rtp,
					star6e_hevc_rtp_write, output, max_payload,
					stats);
			}

			total_bytes += hevc_rtp_send_nal(&ap, nal_ptr, nal_len, rtp,
				star6e_hevc_rtp_write, output, last_nal, max_payload,
				stats);
		}
	}

	total_bytes += hevc_rtp_ap_flush(&ap, rtp, star6e_hevc_rtp_write, output,
		1, stats);
	rtp->timestamp += frame_ticks;
	return total_bytes;
}
