#include "maruko_ts_recorder.h"
#include "ts_mux.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

int maruko_ts_recorder_write_stream(Star6eTsRecorderState *state,
	const i6c_venc_strm *stream)
{
	uint8_t nal_buf[512 * 1024];  /* matches Star6E adapter — ~50 Mbps IDR */
	size_t nal_len = 0;
	int is_idr = 0;
	struct timespec now;
	uint64_t pts;

	if (!state || state->fd < 0 || !stream || !stream->packet)
		return 0;

	for (unsigned int i = 0; i < stream->count; ++i) {
		const i6c_venc_pack *pack = &stream->packet[i];
		if (!pack->data)
			continue;

		if (pack->packNum > 0) {
			const unsigned int info_cap = (unsigned int)(
				sizeof(pack->packetInfo) /
				sizeof(pack->packetInfo[0]));
			unsigned int nal_count = (unsigned int)pack->packNum;
			if (nal_count > info_cap)
				nal_count = info_cap;

			for (unsigned int k = 0; k < nal_count; ++k) {
				unsigned int off = pack->packetInfo[k].offset;
				unsigned int len = pack->packetInfo[k].length;

				if (len == 0 || off >= pack->length ||
				    len > (pack->length - off))
					continue;

				unsigned int nalu = (unsigned int)
					pack->packetInfo[k].packType.h265Nalu;
				if (nalu == 19 || nalu == 20)
					is_idr = 1;

				if (nal_len + len > sizeof(nal_buf)) {
					fprintf(stderr,
						"[maruko_ts] frame too large "
						"(%zu + %u > %zu), truncated\n",
						nal_len, len, sizeof(nal_buf));
					break;
				}
				memcpy(nal_buf + nal_len,
					pack->data + off, len);
				nal_len += len;
			}
		} else {
			if (pack->length <= pack->offset)
				continue;
			unsigned int len = pack->length - pack->offset;
			if (nal_len + len > sizeof(nal_buf)) {
				fprintf(stderr,
					"[maruko_ts] frame too large "
					"(%zu + %u > %zu), truncated\n",
					nal_len, len, sizeof(nal_buf));
				break;
			}
			memcpy(nal_buf + nal_len,
				pack->data + pack->offset, len);
			nal_len += len;
		}
	}

	if (nal_len == 0)
		return 0;

	clock_gettime(CLOCK_MONOTONIC, &now);
	pts = ts_mux_timespec_to_pts((uint32_t)now.tv_sec,
		(uint32_t)now.tv_nsec);

	return star6e_ts_recorder_write_video(state, nal_buf, nal_len, pts,
		is_idr);
}
