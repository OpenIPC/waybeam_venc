#include "rtp_session.h"

#include "test_helpers.h"

static int test_rtp_session_helpers(void)
{
	int failures = 0;

	CHECK("rtp session frame ticks zero",
		rtp_session_frame_ticks(0) == 3000);
	CHECK("rtp session frame ticks 60",
		rtp_session_frame_ticks(60) == 1500);
	CHECK("rtp session payload h265",
		rtp_session_payload_type(PT_H265) == 97);
	CHECK("rtp session payload h264",
		rtp_session_payload_type(PT_H264) == 96);
	return failures;
}

static int test_rtp_session_init(void)
{
	RtpSessionState state;
	int failures = 0;

	rtp_session_init(&state, 123, 120);
	CHECK("rtp session init payload", state.payload_type == 123);
	CHECK("rtp session init frame ticks", state.frame_ticks == 750);
	return failures;
}

int test_rtp_session(void)
{
	int failures = 0;

	failures += test_rtp_session_helpers();
	failures += test_rtp_session_init();
	return failures;
}
