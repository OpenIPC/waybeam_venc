#ifndef RTP_ADAPT_H
#define RTP_ADAPT_H

#include <stddef.h>
#include <stdint.h>

#define RTP_DEFAULT_PAYLOAD 1400
#define RTP_BUFFER_MAX 8192
#define RTP_MIN_PAYLOAD 1000

typedef struct {
	uint16_t payload_size;
	uint16_t target_pkt_rate;
	uint16_t max_frame_size;
	uint16_t adapt_cooldown;
	uint32_t ewma_frame_bytes;
	uint32_t sensor_fps;
} RtpAdaptState;

/** Recalculate RTP payload size based on frame size and target packet rate. */
int rtp_adapt_update(RtpAdaptState *state, int enabled, size_t frame_bytes,
	int verbose);

#endif /* RTP_ADAPT_H */
