#ifndef HEVC_RTP_H
#define HEVC_RTP_H

#include "h26x_param_sets.h"
#include "rtp_packetizer.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t total_nals;
	uint32_t single_packets;
	uint32_t ap_packets;
	uint32_t ap_nals;
	uint32_t fu_packets;
	uint32_t rtp_packets;
	uint32_t rtp_payload_bytes;
} HevcRtpStats;

/* Aggregation-packet builder.  Layout is exposed only so callers can
 * stack-allocate; fields under _internal are implementation detail and
 * must be accessed only through hevc_rtp_* helpers. */
typedef struct {
	struct {
		uint8_t payload[RTP_BUFFER_MAX];
		size_t payload_len;
		size_t nal_bytes;
		uint16_t nal_count;
		uint8_t forbidden_zero;
		uint8_t layer_id;
		uint8_t tid_plus1;
	} _internal;
} HevcApBuilder;

void hevc_rtp_ap_reset(HevcApBuilder *ap);

/** Send one HEVC NAL: aggregates into AP when small, else single-RTP or FU-A.
 *  If is_last is non-zero, flushes AP with marker bit after queueing. */
size_t hevc_rtp_send_nal(HevcApBuilder *ap, const uint8_t *data, size_t len,
	RtpPacketizerState *rtp, RtpPacketizerWriteFn write_cb, void *opaque,
	int is_last, size_t max_payload, HevcRtpStats *stats);

/** Flush any pending AP (call at end of frame if no NAL was sent as last). */
size_t hevc_rtp_ap_flush(HevcApBuilder *ap, RtpPacketizerState *rtp,
	RtpPacketizerWriteFn write_cb, void *opaque, int marker,
	HevcRtpStats *stats);

/** Queue VPS/SPS/PPS prepend NALs for an IDR NAL type.  No-op for non-IDR. */
size_t hevc_rtp_prepend_param_sets(const H26xParamSets *params, uint8_t nal_type,
	HevcApBuilder *ap, RtpPacketizerState *rtp,
	RtpPacketizerWriteFn write_cb, void *opaque, size_t max_payload,
	HevcRtpStats *stats);

#endif /* HEVC_RTP_H */
