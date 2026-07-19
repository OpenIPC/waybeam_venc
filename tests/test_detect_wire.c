#include "test_helpers.h"

#include "detect_wire.h"
#include "detect_plugin.h"
#include "rtp_sidecar.h"

#include <arpa/inet.h>
#include <string.h>

/* Minimal consumer-side walker: parse a DETECT trailer, collecting BOX records
 * and skipping any unknown tags by length (the forward-compat contract). */
typedef struct {
	uint16_t model_id, schema_ver, object_count, flags, age_ms;
	uint32_t detect_seq;
	uint16_t payload_len;
	int      boxes_parsed;
	int      unknown_tags;
	RtpSidecarDetectBoxWire box[RTP_SIDECAR_DETECT_MAX];
} ParsedDetect;

static int parse_detect(const uint8_t *buf, size_t len, ParsedDetect *out)
{
	RtpSidecarDetectHdr hdr;

	memset(out, 0, sizeof(*out));
	if (len < sizeof(hdr))
		return -1;
	memcpy(&hdr, buf, sizeof(hdr));
	out->model_id     = ntohs(hdr.model_id);
	out->schema_ver   = ntohs(hdr.schema_ver);
	out->object_count = ntohs(hdr.object_count);
	out->flags        = ntohs(hdr.flags);
	out->detect_seq   = ntohl(hdr.detect_seq);
	out->payload_len  = ntohs(hdr.payload_len);
	out->age_ms       = ntohs(hdr.age_ms);

	const uint8_t *p = buf + sizeof(hdr);
	const uint8_t *end = buf + sizeof(hdr) + out->payload_len;
	if ((size_t)(end - buf) > len)
		return -1;
	while (p + 2 <= end) {
		uint8_t tag = *p++;
		uint8_t tlen = *p++;
		if (p + tlen > end)
			return -1;
		if (tag == RTP_SIDECAR_DETECT_TAG_BOX && tlen == 10) {
			if (out->boxes_parsed < RTP_SIDECAR_DETECT_MAX) {
				RtpSidecarDetectBoxWire w;
				memcpy(&w, p, sizeof(w));
				w.x1 = ntohs(w.x1);
				w.y1 = ntohs(w.y1);
				w.x2 = ntohs(w.x2);
				w.y2 = ntohs(w.y2);
				out->box[out->boxes_parsed++] = w;
			}
		} else {
			out->unknown_tags++;
		}
		p += tlen;
	}
	return 0;
}

int test_detect_wire(void)
{
	int failures = 0;
	uint8_t buf[RTP_SIDECAR_DGRAM_MAX];

	/* 1. Basic build + parse round-trip (640×352 net). */
	{
		DetectBox in[2];
		memset(in, 0, sizeof(in));
		in[0] = (DetectBox){ 0.0f, 0.0f, 640.0f, 352.0f, 1.0f, 3 };
		in[1] = (DetectBox){ 320.0f, 176.0f, 640.0f, 352.0f, 0.5f, 8 };

		size_t n = detect_wire_build(buf, sizeof(buf), in, 2,
			RTP_SIDECAR_DETECT_MODEL_VISDRONE, 42, 17,
			640, 352, sizeof(buf));
		CHECK("basic_len", n == sizeof(RtpSidecarDetectHdr) + 2 * 12);

		ParsedDetect d;
		CHECK("basic_parse", parse_detect(buf, n, &d) == 0);
		CHECK("basic_model", d.model_id == RTP_SIDECAR_DETECT_MODEL_VISDRONE);
		CHECK("basic_schema", d.schema_ver == RTP_SIDECAR_DETECT_SCHEMA_V1);
		CHECK("basic_count", d.object_count == 2);
		CHECK("basic_seq", d.detect_seq == 42);
		CHECK("basic_age", d.age_ms == 17);
		CHECK("basic_not_trunc", (d.flags & RTP_SIDECAR_DETECT_TRUNCATED) == 0);
		CHECK("basic_payload_len", d.payload_len == 2 * 12);
		CHECK("basic_boxes", d.boxes_parsed == 2);
		CHECK("basic_no_unknown", d.unknown_tags == 0);

		/* Box 0: full-frame corner box → normalized 0..65535. */
		CHECK("box0_x1", d.box[0].x1 == 0);
		CHECK("box0_y1", d.box[0].y1 == 0);
		CHECK("box0_x2", d.box[0].x2 == 65535);
		CHECK("box0_y2", d.box[0].y2 == 65535);
		CHECK("box0_score", d.box[0].score == 255);
		CHECK("box0_cls", d.box[0].cls == 3);

		/* Box 1: half-frame origin → ~32768; score 0.5 → 128. */
		CHECK("box1_x1", d.box[1].x1 == 32768);
		CHECK("box1_y1", d.box[1].y1 == 32768);
		CHECK("box1_score", d.box[1].score == 128);
		CHECK("box1_cls", d.box[1].cls == 8);
	}

	/* 2. Empty snapshot (ran, no detections) → header only, valid. */
	{
		size_t n = detect_wire_build(buf, sizeof(buf), NULL, 0,
			0, 7, 0, 640, 352, sizeof(buf));
		CHECK("empty_len", n == sizeof(RtpSidecarDetectHdr));
		ParsedDetect d;
		CHECK("empty_parse", parse_detect(buf, n, &d) == 0);
		CHECK("empty_count", d.object_count == 0);
		CHECK("empty_seq", d.detect_seq == 7);
		CHECK("empty_not_trunc", (d.flags & RTP_SIDECAR_DETECT_TRUNCATED) == 0);
	}

	/* 3. Object-cap truncation + top-by-score selection. */
	{
		DetectBox in[30];
		memset(in, 0, sizeof(in));
		for (int i = 0; i < 30; i++) {
			in[i].x1 = 0; in[i].y1 = 0; in[i].x2 = 64; in[i].y2 = 64;
			in[i].cls = i;
			in[i].score = (float)(i + 1) / 40.0f; /* ascending */
		}
		size_t n = detect_wire_build(buf, sizeof(buf), in, 30,
			0, 1, 0, 640, 352, sizeof(buf));
		ParsedDetect d;
		CHECK("cap_parse", parse_detect(buf, n, &d) == 0);
		CHECK("cap_count", d.object_count == RTP_SIDECAR_DETECT_MAX);
		CHECK("cap_trunc", (d.flags & RTP_SIDECAR_DETECT_TRUNCATED) != 0);
		/* Highest score first → cls 29 leads the record list. */
		CHECK("cap_top_first", d.box[0].cls == 29);
		CHECK("cap_top_score", d.box[0].score >= d.box[1].score);
	}

	/* 4. Byte-budget truncation. */
	{
		DetectBox in[10];
		memset(in, 0, sizeof(in));
		for (int i = 0; i < 10; i++) {
			in[i].x2 = 64; in[i].y2 = 64; in[i].cls = i;
			in[i].score = (float)(i + 1) / 20.0f;
		}
		/* Room for header + 3 records only. */
		size_t budget = sizeof(RtpSidecarDetectHdr) + 3 * 12;
		size_t n = detect_wire_build(buf, sizeof(buf), in, 10,
			0, 1, 0, 640, 352, budget);
		ParsedDetect d;
		CHECK("budget_parse", parse_detect(buf, n, &d) == 0);
		CHECK("budget_count", d.object_count == 3);
		CHECK("budget_trunc", (d.flags & RTP_SIDECAR_DETECT_TRUNCATED) != 0);
		CHECK("budget_len", n == budget);
	}

	/* 5. Out buffer too small for even the header → 0. */
	{
		uint8_t tiny[8];
		DetectBox in = { 0, 0, 10, 10, 0.9f, 1 };
		size_t n = detect_wire_build(tiny, sizeof(tiny), &in, 1,
			0, 1, 0, 640, 352, sizeof(tiny));
		CHECK("tiny_zero", n == 0);
	}

	/* 6. Unknown-tag skip: hand-craft a trailer with an unknown tag between
	 *    two BOX records and confirm the walker skips it. */
	{
		uint8_t craft[sizeof(RtpSidecarDetectHdr) + 12 + 4 + 12];
		uint8_t *p = craft;
		RtpSidecarDetectHdr hdr;
		memset(&hdr, 0, sizeof(hdr));
		hdr.object_count = htons(2);
		hdr.schema_ver = htons(RTP_SIDECAR_DETECT_SCHEMA_V1);
		hdr.payload_len = htons(12 + 4 + 12); /* box, unknown(2), box */
		memcpy(p, &hdr, sizeof(hdr));
		p += sizeof(hdr);
		/* BOX */
		*p++ = RTP_SIDECAR_DETECT_TAG_BOX; *p++ = 10;
		memset(p, 0, 10); p += 10;
		/* unknown tag 0x90, len 2 */
		*p++ = 0x90; *p++ = 2; *p++ = 0xAB; *p++ = 0xCD;
		/* BOX */
		*p++ = RTP_SIDECAR_DETECT_TAG_BOX; *p++ = 10;
		memset(p, 0, 10); p += 10;

		ParsedDetect d;
		CHECK("skip_parse", parse_detect(craft, sizeof(craft), &d) == 0);
		CHECK("skip_boxes", d.boxes_parsed == 2);
		CHECK("skip_unknown", d.unknown_tags == 1);
	}

	return failures;
}
