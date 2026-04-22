#include "hevc_rtp.h"

#include "h26x_param_sets.h"
#include "rtp_packetizer.h"
#include "test_helpers.h"

#include <stdint.h>
#include <string.h>

#define CAPTURE_MAX 32

typedef struct {
	size_t header_len;
	size_t payload_len;
	uint8_t payload[RTP_BUFFER_MAX];
	uint8_t header[16];
} CapturedPacket;

typedef struct {
	CapturedPacket packets[CAPTURE_MAX];
	size_t count;
} CaptureCtx;

static int capture_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	CaptureCtx *ctx = opaque;
	CapturedPacket *pkt;
	size_t combined = payload1_len + payload2_len;

	if (!ctx || ctx->count >= CAPTURE_MAX || header_len > sizeof(pkt->header)
	    || combined > RTP_BUFFER_MAX)
		return -1;

	pkt = &ctx->packets[ctx->count];
	memcpy(pkt->header, header, header_len);
	pkt->header_len = header_len;
	memcpy(pkt->payload, payload1, payload1_len);
	if (payload2 && payload2_len > 0)
		memcpy(pkt->payload + payload1_len, payload2, payload2_len);
	pkt->payload_len = combined;
	ctx->count++;
	return 0;
}

static int rtp_marker_set(const uint8_t *header)
{
	return (header[1] & 0x80) ? 1 : 0;
}

static int test_hevc_rtp_aggregates_small_nals(void)
{
	/* Three tiny NALs: VPS, SPS, PPS — all fit in one AP. */
	static const uint8_t vps[] = { 0x40, 0x01, 0xAA };
	static const uint8_t sps[] = { 0x42, 0x01, 0xBB };
	static const uint8_t pps[] = { 0x44, 0x01, 0xCC };
	CaptureCtx capture = {0};
	HevcApBuilder ap;
	HevcRtpStats stats = {0};
	RtpPacketizerState rtp = { .seq = 0x100, .timestamp = 0x1000,
		.ssrc = 0xDEAD, .payload_type = 97 };
	size_t total;
	int failures = 0;

	hevc_rtp_ap_reset(&ap);
	total = hevc_rtp_send_nal(&ap, vps, sizeof(vps), &rtp, capture_write,
		&capture, 0, 1400, &stats);
	total += hevc_rtp_send_nal(&ap, sps, sizeof(sps), &rtp, capture_write,
		&capture, 0, 1400, &stats);
	total += hevc_rtp_send_nal(&ap, pps, sizeof(pps), &rtp, capture_write,
		&capture, 1, 1400, &stats);

	CHECK("hevc_rtp ap sends one packet", capture.count == 1);
	CHECK("hevc_rtp ap total bytes match",
		total == sizeof(vps) + sizeof(sps) + sizeof(pps));
	CHECK("hevc_rtp ap marker on last", rtp_marker_set(capture.packets[0].header));
	CHECK("hevc_rtp ap header type 48",
		capture.count > 0 && (capture.packets[0].payload[0] >> 1 & 0x3F) == 48);
	CHECK("hevc_rtp ap stats", stats.total_nals == 3 && stats.ap_packets == 1
		&& stats.ap_nals == 3 && stats.single_packets == 0
		&& stats.fu_packets == 0 && stats.rtp_packets == 1);
	CHECK("hevc_rtp ap seq advanced by 1", rtp.seq == 0x101);
	return failures;
}

static int test_hevc_rtp_fallback_to_fu(void)
{
	/* Large NAL that exceeds max_payload — must use FU-A. */
	static uint8_t big[3000];
	CaptureCtx capture = {0};
	HevcApBuilder ap;
	HevcRtpStats stats = {0};
	RtpPacketizerState rtp = { .seq = 0x200, .timestamp = 0x2000,
		.ssrc = 0xCAFE, .payload_type = 97 };
	size_t i;
	size_t total;
	int failures = 0;

	/* Build a plausible NAL header: type=1 (TRAIL_N), layer_id=0, tid=1 */
	big[0] = (uint8_t)(1 << 1);
	big[1] = 0x01;
	for (i = 2; i < sizeof(big); ++i)
		big[i] = (uint8_t)(i & 0xFF);

	hevc_rtp_ap_reset(&ap);
	total = hevc_rtp_send_nal(&ap, big, sizeof(big), &rtp, capture_write,
		&capture, 1, 1400, &stats);

	CHECK("hevc_rtp fu total bytes", total == sizeof(big));
	CHECK("hevc_rtp fu multi packet", capture.count >= 2);
	CHECK("hevc_rtp fu stats", stats.fu_packets == capture.count
		&& stats.total_nals == 1 && stats.ap_packets == 0);
	CHECK("hevc_rtp fu last marker",
		capture.count > 0 &&
		rtp_marker_set(capture.packets[capture.count - 1].header));
	CHECK("hevc_rtp fu indicator type 49",
		capture.count > 0 &&
		(capture.packets[0].payload[0] >> 1 & 0x3F) == 49);
	return failures;
}

static int test_hevc_rtp_prepend_param_sets_on_idr(void)
{
	static const uint8_t vps[] = { 0x40, 0x01, 0xAA };
	static const uint8_t sps[] = { 0x42, 0x01, 0xBB };
	static const uint8_t pps[] = { 0x44, 0x01, 0xCC };
	static const uint8_t idr[] = { 0x26, 0x01, 0xDD, 0xEE };
	H26xParamSets params = {0};
	CaptureCtx capture = {0};
	HevcApBuilder ap;
	HevcRtpStats stats = {0};
	RtpPacketizerState rtp = { .seq = 0x300, .timestamp = 0x3000,
		.ssrc = 0xFEED, .payload_type = 97 };
	int failures = 0;
	size_t prepend_total;
	size_t idr_total;

	h26x_param_sets_update(&params, PT_H265, 32, vps, sizeof(vps));
	h26x_param_sets_update(&params, PT_H265, 33, sps, sizeof(sps));
	h26x_param_sets_update(&params, PT_H265, 34, pps, sizeof(pps));

	hevc_rtp_ap_reset(&ap);
	/* Prepend queues into AP; bytes are not counted until flush. */
	prepend_total = hevc_rtp_prepend_param_sets(&params, 19, &ap, &rtp,
		capture_write, &capture, 1400, &stats);
	CHECK("hevc_rtp prepend queued 3 nals", stats.total_nals == 3);
	CHECK("hevc_rtp prepend no packet before flush",
		capture.count == 0 && prepend_total == 0);

	/* Sending the IDR with is_last=1 flushes the whole AP — one packet. */
	idr_total = hevc_rtp_send_nal(&ap, idr, sizeof(idr), &rtp, capture_write,
		&capture, 1, 1400, &stats);
	CHECK("hevc_rtp prepend+idr flush total",
		idr_total == sizeof(vps) + sizeof(sps) + sizeof(pps) + sizeof(idr));
	CHECK("hevc_rtp prepend+idr single AP packet", capture.count == 1);
	CHECK("hevc_rtp prepend+idr stats",
		stats.total_nals == 4 && stats.ap_packets == 1 &&
		stats.ap_nals == 4 && stats.rtp_packets == 1);

	/* Non-IDR nal type should not prepend anything. */
	memset(&stats, 0, sizeof(stats));
	memset(&capture, 0, sizeof(capture));
	hevc_rtp_ap_reset(&ap);
	prepend_total = hevc_rtp_prepend_param_sets(&params, 1, &ap, &rtp,
		capture_write, &capture, 1400, &stats);
	CHECK("hevc_rtp prepend skipped for non-idr",
		prepend_total == 0 && stats.total_nals == 0);
	return failures;
}

/* A small NAL followed by a large NAL in the same frame: the AP must be
 * flushed (as a single-RTP since it had only one queued NAL) before the
 * large NAL goes out as FU-A.  The FU-A tail must carry the marker bit. */
static int test_hevc_rtp_mixed_small_then_large(void)
{
	static const uint8_t small[] = { 0x02, 0x01, 0x33, 0x44 };
	static uint8_t big[3000];
	CaptureCtx capture = {0};
	HevcApBuilder ap;
	HevcRtpStats stats = {0};
	RtpPacketizerState rtp = { .seq = 0x400, .timestamp = 0x4000,
		.ssrc = 0xBEEF, .payload_type = 97 };
	size_t i;
	size_t total;
	int failures = 0;

	big[0] = (uint8_t)(1 << 1);
	big[1] = 0x01;
	for (i = 2; i < sizeof(big); ++i)
		big[i] = (uint8_t)(i & 0xFF);

	hevc_rtp_ap_reset(&ap);
	total = hevc_rtp_send_nal(&ap, small, sizeof(small), &rtp, capture_write,
		&capture, 0, 1400, &stats);
	total += hevc_rtp_send_nal(&ap, big, sizeof(big), &rtp, capture_write,
		&capture, 1, 1400, &stats);

	CHECK("hevc_rtp mixed total bytes", total == sizeof(small) + sizeof(big));
	CHECK("hevc_rtp mixed multiple packets", capture.count >= 3);
	CHECK("hevc_rtp mixed first is single (not AP)",
		capture.count > 0 &&
		(capture.packets[0].payload[0] >> 1 & 0x3F) != 48);
	CHECK("hevc_rtp mixed last marker",
		capture.count > 0 &&
		rtp_marker_set(capture.packets[capture.count - 1].header));
	CHECK("hevc_rtp mixed stats split",
		stats.total_nals == 2 && stats.single_packets == 1 &&
		stats.ap_packets == 0 && stats.fu_packets >= 2);
	return failures;
}

/* Passing stats=NULL must be safe at every entry point. */
static int test_hevc_rtp_null_stats(void)
{
	static const uint8_t vps[] = { 0x40, 0x01, 0xAA };
	static const uint8_t sps[] = { 0x42, 0x01, 0xBB };
	CaptureCtx capture = {0};
	HevcApBuilder ap;
	RtpPacketizerState rtp = { .seq = 0x500, .timestamp = 0x5000,
		.ssrc = 0xABCD, .payload_type = 97 };
	H26xParamSets params = {0};
	size_t total;
	int failures = 0;

	h26x_param_sets_update(&params, PT_H265, 32, vps, sizeof(vps));
	h26x_param_sets_update(&params, PT_H265, 33, sps, sizeof(sps));

	hevc_rtp_ap_reset(&ap);
	total = hevc_rtp_send_nal(&ap, vps, sizeof(vps), &rtp, capture_write,
		&capture, 0, 1400, NULL);
	total += hevc_rtp_send_nal(&ap, sps, sizeof(sps), &rtp, capture_write,
		&capture, 1, 1400, NULL);
	CHECK("hevc_rtp null-stats send ok", total == sizeof(vps) + sizeof(sps));

	hevc_rtp_ap_reset(&ap);
	total = hevc_rtp_prepend_param_sets(&params, 19, &ap, &rtp,
		capture_write, &capture, 1400, NULL);
	total += hevc_rtp_ap_flush(&ap, &rtp, capture_write, &capture, 1, NULL);
	CHECK("hevc_rtp null-stats prepend+flush ok",
		total == sizeof(vps) + sizeof(sps));
	return failures;
}

/* hevc_rtp_send_nal with is_last=1 on a lone small NAL must emit a single
 * RTP packet with the marker bit set. */
static int test_hevc_rtp_single_marker_on_last(void)
{
	static const uint8_t nal[] = { 0x02, 0x01, 0x77, 0x88, 0x99 };
	CaptureCtx capture = {0};
	HevcApBuilder ap;
	HevcRtpStats stats = {0};
	RtpPacketizerState rtp = { .seq = 0x600, .timestamp = 0x6000,
		.ssrc = 0x1234, .payload_type = 97 };
	size_t total;
	int failures = 0;

	hevc_rtp_ap_reset(&ap);
	total = hevc_rtp_send_nal(&ap, nal, sizeof(nal), &rtp, capture_write,
		&capture, 1, 1400, &stats);
	CHECK("hevc_rtp single total", total == sizeof(nal));
	CHECK("hevc_rtp single one packet", capture.count == 1);
	CHECK("hevc_rtp single marker set",
		capture.count > 0 && rtp_marker_set(capture.packets[0].header));
	CHECK("hevc_rtp single stats",
		stats.total_nals == 1 && stats.single_packets == 1 &&
		stats.ap_packets == 0 && stats.fu_packets == 0);
	return failures;
}

int test_hevc_rtp(void)
{
	int failures = 0;

	failures += test_hevc_rtp_aggregates_small_nals();
	failures += test_hevc_rtp_fallback_to_fu();
	failures += test_hevc_rtp_prepend_param_sets_on_idr();
	failures += test_hevc_rtp_mixed_small_then_large();
	failures += test_hevc_rtp_null_stats();
	failures += test_hevc_rtp_single_marker_on_last();
	return failures;
}
