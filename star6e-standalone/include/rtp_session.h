#ifndef RTP_SESSION_H
#define RTP_SESSION_H

#include "codec_types.h"

#include <stdint.h>

typedef struct {
	uint16_t seq;
	uint32_t timestamp;
	uint32_t ssrc;
	uint32_t frame_ticks;
	uint8_t payload_type;
} RtpSessionState;

/** Initialize RTP session with payload type and timestamp rate. */
void rtp_session_init(RtpSessionState *state, uint8_t payload_type,
	uint32_t sensor_fps);

/** Calculate RTP timestamp increment per frame at given FPS. */
uint32_t rtp_session_frame_ticks(uint32_t sensor_fps);

/** Map H.26x codec enum to RTP payload type code. */
uint8_t rtp_session_payload_type(PAYLOAD_TYPE_E codec);

#endif /* RTP_SESSION_H */
