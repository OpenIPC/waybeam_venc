#include "maruko_video.h"
#include "test_helpers.h"

#include <string.h>

static int g_recovery_calls;

static int recovery_stub(void *ctx)
{
	(void)ctx;
	g_recovery_calls++;
	return 1;
}

int test_maruko_video(void)
{
	MarukoOutput output;
	i6c_venc_pack pack;
	i6c_venc_strm stream;
	uint8_t data[16] = { 0 };
	int failures = 0;
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

	output.request_idr = recovery_stub;
	g_recovery_calls = 0;
	CHECK("maruko invalid AU rejected",
		maruko_video_reject_incomplete_access_unit(&stream, &output) == 1);
	CHECK("maruko invalid AU requests recovery", g_recovery_calls == 1);
	CHECK("maruko invalid AU recovery paced",
		maruko_video_reject_incomplete_access_unit(&stream, &output) == 1 &&
		g_recovery_calls == 1);

	output.drop_idr_last_us = 0;
	output.svct_active = 1;
	stream.h265Info.refType = MARUKO_REFTYPE_ENHANCE_P_NOTFORREF;
	CHECK("maruko droppable invalid AU skips recovery",
		maruko_video_reject_incomplete_access_unit(&stream, &output) == 1 &&
		g_recovery_calls == 1);

	return failures;
}
