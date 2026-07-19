/*
 * detect_wire.c — DetectBox[] → sidecar DETECT trailer serialiser.
 *
 * See detect_wire.h and protocols/rtp-sidecar.md.  Kept free of MI/SDK deps so
 * it links into the host test runner alongside a synthetic box array.
 */

#include "detect_wire.h"

#include <arpa/inet.h>   /* htons / htonl */
#include <string.h>

#include "rtp_sidecar.h"  /* RtpSidecarDetectHdr / *BoxWire / constants */

/* Upper bound on detections we will consider in one call.  The Star6E reader
 * publishes at most 64; anything past this is dropped (and TRUNCATED set). */
#define DETECT_WIRE_MAX_IN 256

/* Bytes on the wire per BOX record: [tag u8][len u8] + value. */
#define DETECT_WIRE_BOX_REC (2 + (int)sizeof(RtpSidecarDetectBoxWire))

static uint16_t norm_coord(float v, uint16_t dim)
{
	if (dim == 0)
		return 0;
	float f = v / (float)dim;
	if (f <= 0.0f)
		return 0;
	if (f >= 1.0f)
		return 65535;
	return (uint16_t)(f * 65535.0f + 0.5f);
}

static uint8_t score_u8(float s)
{
	if (s <= 0.0f)
		return 0;
	if (s >= 1.0f)
		return 255;
	return (uint8_t)(s * 255.0f + 0.5f);
}

size_t detect_wire_build(uint8_t *out, size_t out_cap,
	const DetectBox *boxes, int n,
	uint16_t model_id, uint32_t detect_seq, uint16_t age_ms,
	uint16_t net_w, uint16_t net_h, size_t budget)
{
	if (!out || out_cap < sizeof(RtpSidecarDetectHdr))
		return 0;

	/* Effective room is the tighter of the caller's byte budget and the
	 * output buffer; must at least hold the header. */
	size_t cap = out_cap < budget ? out_cap : budget;
	if (cap < sizeof(RtpSidecarDetectHdr))
		return 0;

	size_t room_boxes = (cap - sizeof(RtpSidecarDetectHdr)) /
		(size_t)DETECT_WIRE_BOX_REC;
	int max_obj = RTP_SIDECAR_DETECT_MAX;
	if ((size_t)max_obj > room_boxes)
		max_obj = (int)room_boxes;

	if (n < 0 || !boxes)
		n = 0;
	int in = n > DETECT_WIRE_MAX_IN ? DETECT_WIRE_MAX_IN : n;
	int take = in < max_obj ? in : max_obj;
	int truncated = (n > take) ? 1 : 0;

	/* Select the top `take` boxes by score (partial selection sort over an
	 * index array, so the input array is not mutated). */
	int order[DETECT_WIRE_MAX_IN];
	for (int i = 0; i < in; i++)
		order[i] = i;
	for (int i = 0; i < take; i++) {
		int best = i;
		for (int j = i + 1; j < in; j++) {
			if (boxes[order[j]].score > boxes[order[best]].score)
				best = j;
		}
		int tmp = order[i];
		order[i] = order[best];
		order[best] = tmp;
	}

	uint8_t *p = out;
	RtpSidecarDetectHdr hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.model_id     = htons(model_id);
	hdr.schema_ver   = htons(RTP_SIDECAR_DETECT_SCHEMA_V1);
	hdr.object_count = htons((uint16_t)take);
	hdr.flags        = htons(truncated ? RTP_SIDECAR_DETECT_TRUNCATED : 0);
	hdr.detect_seq   = htonl(detect_seq);
	hdr.payload_len  = htons((uint16_t)(take * DETECT_WIRE_BOX_REC));
	hdr.age_ms       = htons(age_ms);
	memcpy(p, &hdr, sizeof(hdr));
	p += sizeof(hdr);

	for (int i = 0; i < take; i++) {
		const DetectBox *b = &boxes[order[i]];
		RtpSidecarDetectBoxWire w;

		*p++ = RTP_SIDECAR_DETECT_TAG_BOX;
		*p++ = (uint8_t)sizeof(RtpSidecarDetectBoxWire);
		w.x1    = htons(norm_coord(b->x1, net_w));
		w.y1    = htons(norm_coord(b->y1, net_h));
		w.x2    = htons(norm_coord(b->x2, net_w));
		w.y2    = htons(norm_coord(b->y2, net_h));
		w.score = score_u8(b->score);
		w.cls   = (uint8_t)(b->cls < 0 ? 0 : (b->cls > 255 ? 255 : b->cls));
		memcpy(p, &w, sizeof(w));
		p += sizeof(w);
	}

	return (size_t)(p - out);
}
