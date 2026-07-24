#ifndef DETECT_WIRE_H
#define DETECT_WIRE_H

/*
 * detect_wire.h — serialise detector output into a sidecar DETECT trailer.
 *
 * Pure host-side logic (no SDK / MI deps): converts the model-agnostic
 * DetectBox[] the host holds into the opaque DETECT trailer the sidecar
 * carries (RtpSidecarDetectHdr + BOX TLV records, network byte order — see
 * protocols/rtp-sidecar.md).  The sidecar copies the resulting bytes verbatim
 * and never parses tags, keeping detection semantics out of the transport.
 */

#include <stddef.h>
#include <stdint.h>

#include "detect_plugin.h"   /* DetectBox */

/*
 * Build a DETECT trailer at `out` (capacity `out_cap` bytes).
 *
 * boxes/n     : detections in model/network pixel space (corner form,
 *               [0..net_w]×[0..net_h]); score is a 0..1 probability.
 * model_id    : class-table selector written to the header (0 = VisDrone-10).
 * detect_seq  : monotonic inference id (consumer dedup / freshness).
 * age_ms      : snapshot staleness at frame encode time, saturating.
 * net_w/net_h : model input dims used to normalise coords to 0..65535.
 * budget      : max trailer bytes (the datagram space still free); records
 *               beyond the budget or beyond RTP_SIDECAR_DETECT_MAX are dropped
 *               (highest-score kept) and the TRUNCATED flag is set.
 *
 * Returns the number of bytes written (>= sizeof(RtpSidecarDetectHdr) when the
 * header fits — object_count 0 is a valid "ran, empty scene" trailer), or 0 if
 * out_cap/budget cannot hold even the header.
 */
size_t detect_wire_build(uint8_t *out, size_t out_cap,
	const DetectBox *boxes, int n,
	uint16_t model_id, uint32_t detect_seq, uint16_t age_ms,
	uint16_t net_w, uint16_t net_h, size_t budget);

#endif /* DETECT_WIRE_H */
