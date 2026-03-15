#include "rtp_adapt.h"
#include "test_helpers.h"

static int test_rtp_adapt_disabled(void)
{
	int failures = 0;
	RtpAdaptState state = {
		.payload_size = 1400,
		.target_pkt_rate = 0,
		.max_frame_size = 1400,
		.sensor_fps = 120,
	};

	CHECK("rtp_adapt_disabled_target",
		rtp_adapt_update(&state, 1, 5000, 0) == 0 &&
		state.payload_size == 1400 &&
		state.ewma_frame_bytes == 0);
	CHECK("rtp_adapt_disabled_mode",
		rtp_adapt_update(&state, 0, 5000, 0) == 0 &&
		state.payload_size == 1400);

	return failures;
}

static int test_rtp_adapt_outlier_skip(void)
{
	int failures = 0;
	RtpAdaptState state = {
		.payload_size = 1200,
		.target_pkt_rate = 30,
		.max_frame_size = 1400,
		.ewma_frame_bytes = 1000U << 8,
		.sensor_fps = 30,
	};

	CHECK("rtp_adapt_outlier_skip",
		rtp_adapt_update(&state, 1, 4000, 0) == 0 &&
		state.payload_size == 1200 &&
		state.ewma_frame_bytes == (1000U << 8));

	return failures;
}

static int test_rtp_adapt_cooldown(void)
{
	int failures = 0;
	RtpAdaptState state = {
		.payload_size = 1400,
		.target_pkt_rate = 1000,
		.max_frame_size = 1400,
		.adapt_cooldown = 2,
		.sensor_fps = 30,
	};

	CHECK("rtp_adapt_cooldown_step1",
		rtp_adapt_update(&state, 1, 100, 0) == 0 &&
		state.payload_size == 1400 &&
		state.adapt_cooldown == 1);
	CHECK("rtp_adapt_cooldown_step2",
		rtp_adapt_update(&state, 1, 100, 0) == 0 &&
		state.payload_size == 1400 &&
		state.adapt_cooldown == 0);
	CHECK("rtp_adapt_cooldown_step3",
		rtp_adapt_update(&state, 1, 100, 0) == 1 &&
		state.payload_size == RTP_MIN_PAYLOAD &&
		state.adapt_cooldown == 15);

	return failures;
}

static int test_rtp_adapt_clamps(void)
{
	int failures = 0;
	RtpAdaptState min_state = {
		.payload_size = 1400,
		.target_pkt_rate = 1000,
		.max_frame_size = 1400,
		.sensor_fps = 30,
	};
	RtpAdaptState max_state = {
		.payload_size = RTP_MIN_PAYLOAD,
		.target_pkt_rate = 10,
		.max_frame_size = 1400,
		.sensor_fps = 120,
	};

	CHECK("rtp_adapt_clamp_min",
		rtp_adapt_update(&min_state, 1, 100, 0) == 1 &&
		min_state.payload_size == RTP_MIN_PAYLOAD);
	CHECK("rtp_adapt_clamp_max",
		rtp_adapt_update(&max_state, 1, 50000, 0) == 1 &&
		max_state.payload_size == 1400);

	return failures;
}

static int test_rtp_adapt_hysteresis(void)
{
	int failures = 0;
	RtpAdaptState state = {
		.payload_size = 1100,
		.target_pkt_rate = 30,
		.max_frame_size = 1400,
		.ewma_frame_bytes = 1200U << 8,
		.sensor_fps = 30,
	};

	CHECK("rtp_adapt_hysteresis_deadband",
		rtp_adapt_update(&state, 1, 1200, 0) == 0 &&
		state.payload_size == 1100 &&
		state.adapt_cooldown == 0);

	return failures;
}

int test_rtp_adapt(void)
{
	int failures = 0;

	CHECK("rtp_adapt_null_state", rtp_adapt_update(NULL, 1, 1000, 0) == -1);
	failures += test_rtp_adapt_disabled();
	failures += test_rtp_adapt_outlier_skip();
	failures += test_rtp_adapt_cooldown();
	failures += test_rtp_adapt_clamps();
	failures += test_rtp_adapt_hysteresis();

	return failures;
}
