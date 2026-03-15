#include "rtp_session.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static void rtp_session_seed_random(void)
{
	static int seeded = 0;

	if (seeded)
		return;

	seeded = 1;
	srand((unsigned int)time(NULL));
}

void rtp_session_init(RtpSessionState *state, uint8_t payload_type,
	uint32_t sensor_fps)
{
	if (!state)
		return;

	rtp_session_seed_random();
	memset(state, 0, sizeof(*state));
	state->seq = (uint16_t)(rand() & 0xFFFF);
	state->timestamp = (uint32_t)rand();
	state->ssrc = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
	state->frame_ticks = rtp_session_frame_ticks(sensor_fps);
	state->payload_type = payload_type;
}

uint32_t rtp_session_frame_ticks(uint32_t sensor_fps)
{
	if (sensor_fps == 0)
		return 3000;

	return (uint32_t)((90000 + sensor_fps / 2) / sensor_fps);
}

uint8_t rtp_session_payload_type(PAYLOAD_TYPE_E codec)
{
	return codec == PT_H265 ? 97 : 96;
}
