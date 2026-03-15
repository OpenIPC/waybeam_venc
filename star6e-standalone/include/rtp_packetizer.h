#ifndef RTP_PACKETIZER_H
#define RTP_PACKETIZER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint16_t seq;
	uint32_t timestamp;
	uint32_t ssrc;
	uint8_t payload_type;
} RtpPacketizerState;

typedef struct {
	uint32_t packet_count;
	uint32_t payload_bytes;
	int fragmented;
} RtpPacketizerResult;

typedef int (*RtpPacketizerWriteFn)(const uint8_t *header, size_t header_len,
	const uint8_t *payload, size_t payload_len, void *opaque);

/** Send a single RTP packet with the given payload and marker bit. */
int rtp_packetizer_send_packet(RtpPacketizerState *state,
	RtpPacketizerWriteFn writer, void *opaque, const uint8_t *payload,
	size_t payload_len, int marker);

/** Fragment and send a large HEVC NAL unit as FU-A packets. */
size_t rtp_packetizer_send_hevc_fu(RtpPacketizerState *state,
	RtpPacketizerWriteFn writer, void *opaque, const uint8_t *data,
	size_t length, int is_last, size_t max_payload,
	RtpPacketizerResult *result);

/** Send HEVC NAL as single packet or fragment if exceeding max payload. */
size_t rtp_packetizer_send_hevc_nal(RtpPacketizerState *state,
	RtpPacketizerWriteFn writer, void *opaque, const uint8_t *data,
	size_t length, int is_last, size_t max_payload,
	RtpPacketizerResult *result);

#endif /* RTP_PACKETIZER_H */
