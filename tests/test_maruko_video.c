#include "maruko_video.h"
#include "test_helpers.h"
#include "venc_rec_writer.h"

#include <stdlib.h>
#include <string.h>

/* ── maruko_video_stream_flatten: the async recorder's copy ───────────── */

static int test_maruko_flatten_matches_the_star6e_contract(void)
{
	i6c_venc_pack packs[2];
	i6c_venc_strm stream;
	uint8_t d0[16], d1[8];
	uint8_t *out;
	size_t len = 0;
	int is_idr = -1;
	int failures = 0;
	int i;

	for (i = 0; i < 16; i++) d0[i] = (uint8_t)(0x10 + i);
	for (i = 0; i < 8; i++)  d1[i] = (uint8_t)(0xA0 + i);

	memset(packs, 0, sizeof(packs));
	memset(&stream, 0, sizeof(stream));
	packs[0].data = d0; packs[0].length = 16; packs[0].packNum = 2;
	packs[0].packetInfo[0].offset = 0; packs[0].packetInfo[0].length = 4;
	packs[0].packetInfo[1].offset = 8; packs[0].packetInfo[1].length = 4;
	packs[1].data = d1; packs[1].length = 8; packs[1].offset = 2;
	packs[1].packNum = 0;
	stream.packet = packs; stream.count = 2;

	out = maruko_video_stream_flatten(&stream, &len, &is_idr);
	CHECK("mk flatten returned a buffer", out != NULL);
	CHECK("mk flatten length", len == 4 + 4 + 6);
	if (out && len == 14) {
		CHECK("mk flatten span 1", memcmp(out, d0, 4) == 0);
		CHECK("mk flatten span 2", memcmp(out + 4, d0 + 8, 4) == 0);
		CHECK("mk flatten fallback offset",
			memcmp(out + 8, d1 + 2, 6) == 0);
	} else {
		CHECK("mk flatten payload reachable", 0);
	}
	CHECK("mk flatten no IRAP", is_idr == 0);
	free(out);

	packs[0].packetInfo[0].packType.h265Nalu = 20;
	out = maruko_video_stream_flatten(&stream, &len, &is_idr);
	CHECK("mk flatten idr_n_lp detected", out && is_idr == 1);
	free(out);

	/* A partly-valid stream must be refused whole, not truncated. */
	packs[1].packNum = (unsigned int)(sizeof(packs[1].packetInfo) /
		sizeof(packs[1].packetInfo[0])) + 1;
	CHECK("mk flatten refuses a partly-valid stream",
		maruko_video_stream_flatten(&stream, &len, &is_idr) == NULL);

	CHECK("mk flatten NULL stream",
		maruko_video_stream_flatten(NULL, &len, &is_idr) == NULL);
	return failures;
}

/* Same byte cap as the star6e helper, and it has to be tested separately —
 * the two are independent implementations over different SDK types, and a
 * guard present in one says nothing about the other. */
static int test_maruko_flatten_refuses_an_access_unit_over_the_queue_cap(void)
{
	i6c_venc_pack pack;
	i6c_venc_strm stream;
	size_t oversized = VENC_REC_WRITER_MAX_BYTES + 1;
	uint8_t *big = malloc(oversized);
	size_t len = 0;
	int is_idr = -1;
	int failures = 0;

	CHECK("mk cap test allocated its stimulus", big != NULL);
	if (!big)
		return failures;
	memset(big, 0x5A, oversized);

	memset(&pack, 0, sizeof(pack));
	memset(&stream, 0, sizeof(stream));
	pack.data = big;
	pack.length = (unsigned int)oversized;
	pack.packNum = 0;
	stream.packet = &pack;
	stream.count = 1;

	CHECK("mk flatten refuses an AU over the queue cap",
		maruko_video_stream_flatten(&stream, &len, &is_idr) == NULL);
	CHECK("mk refused AU reports no length", len == 0);

	pack.length = (unsigned int)(VENC_REC_WRITER_MAX_BYTES - 1);
	{
		uint8_t *out = maruko_video_stream_flatten(&stream, &len,
			&is_idr);

		CHECK("mk flatten accepts an AU just under the cap",
			out != NULL);
		CHECK("mk accepted AU reports its length",
			len == VENC_REC_WRITER_MAX_BYTES - 1);
		free(out);
	}
	free(big);
	return failures;
}

int test_maruko_video(void)
{
	MarukoOutput output;
	i6c_venc_pack pack;
	i6c_venc_strm stream;
	uint8_t data[16] = { 0 };
	int failures = 0;

	failures += test_maruko_flatten_matches_the_star6e_contract();
	failures += test_maruko_flatten_refuses_an_access_unit_over_the_queue_cap();
	unsigned int info_cap;

	memset(&output, 0, sizeof(output));
	memset(&pack, 0, sizeof(pack));
	memset(&stream, 0, sizeof(stream));
	info_cap = (unsigned int)(sizeof(pack.packetInfo) /
		sizeof(pack.packetInfo[0]));

	CHECK("maruko packetInfo null stream rejected",
		!maruko_video_stream_packet_info_complete(NULL));
	CHECK("maruko packetInfo empty stream rejected",
		!maruko_video_stream_packet_info_complete(&stream));

	stream.count = 1;
	stream.packet = &pack;
	CHECK("maruko packetInfo null data rejected",
		!maruko_video_stream_packet_info_complete(&stream));

	pack.data = data;
	pack.length = sizeof(data);
	pack.offset = 0;
	CHECK("maruko packetInfo fallback accepted",
		maruko_video_stream_packet_info_complete(&stream));
	pack.offset = pack.length;
	CHECK("maruko packetInfo empty fallback rejected",
		!maruko_video_stream_packet_info_complete(&stream));

	pack.offset = 0;
	pack.packNum = 1;
	pack.packetInfo[0].offset = 0;
	pack.packetInfo[0].length = 0;
	CHECK("maruko packetInfo zero descriptor rejected",
		!maruko_video_stream_packet_info_complete(&stream));
	pack.packetInfo[0].offset = pack.length;
	pack.packetInfo[0].length = 1;
	CHECK("maruko packetInfo offset rejected",
		!maruko_video_stream_packet_info_complete(&stream));
	pack.packetInfo[0].offset = 8;
	pack.packetInfo[0].length = 9;
	CHECK("maruko packetInfo overrun rejected",
		!maruko_video_stream_packet_info_complete(&stream));
	pack.packetInfo[0].offset = 4;
	pack.packetInfo[0].length = 8;
	CHECK("maruko packetInfo descriptor accepted",
		maruko_video_stream_packet_info_complete(&stream));
	pack.packNum = info_cap + 1;
	CHECK("maruko packetInfo table overflow rejected",
		!maruko_video_stream_packet_info_complete(&stream));

	/* An invalid AU is rejected every time and actuates nothing -- the
	 * decision to ask for a keyframe belongs to the consumer, which is
	 * the only party that can see whether the decoder is broken. */
	output.bad_au_drops = 0;
	CHECK("maruko invalid AU rejected",
		maruko_video_reject_incomplete_access_unit(&stream, &output) == 1);
	CHECK("maruko invalid AU counted", output.bad_au_drops == 1);
	CHECK("maruko invalid AU rejected again",
		maruko_video_reject_incomplete_access_unit(&stream, &output) == 1);
	/* The one-shot WARN must not become a one-shot COUNTER -- the whole
	 * point is that a repeating fault stays visible after the first line. */
	CHECK("maruko invalid AU counts every time", output.bad_au_drops == 2);
	output.svct_active = 1;
	stream.h265Info.refType = MARUKO_REFTYPE_ENHANCE_P_NOTFORREF;
	CHECK("maruko droppable invalid AU still rejected",
		maruko_video_reject_incomplete_access_unit(&stream, &output) == 1);

	return failures;
}
