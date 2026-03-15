#ifndef STAR6E_OUTPUT_H
#define STAR6E_OUTPUT_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#include "rtp_packetizer.h"
#include "star6e.h"
#include "venc_ring.h"

typedef enum {
	STAR6E_STREAM_MODE_COMPACT = 0,
	STAR6E_STREAM_MODE_RTP = 1,
} Star6eStreamMode;

typedef enum {
	STAR6E_OUTPUT_TRANSPORT_UDP = 0,
	STAR6E_OUTPUT_TRANSPORT_SHM = 1,
} Star6eOutputTransport;

typedef struct {
	Star6eStreamMode stream_mode;
	Star6eOutputTransport transport;
	int connected_udp;
	int has_server;
	uint16_t max_frame_size;
	char host[128];
	uint16_t port;
	char shm_name[128];
} Star6eOutputSetup;

typedef struct {
	Star6eStreamMode stream_mode;
	Star6eOutputTransport transport;
	int socket_handle;
	struct sockaddr_in dst;
	int connected_udp;
	venc_ring_t *ring;
} Star6eOutput;

typedef struct {
	const Star6eOutput *video_output;
	int socket_handle;
	uint16_t port_override;
	uint16_t max_payload_size;
} Star6eAudioOutput;

typedef size_t (*Star6eOutputRtpSendFn)(const Star6eOutput *output,
	const MI_VENC_Stream_t *stream, void *opaque);

/** Validate and prepare output config from URI and stream mode name. */
int star6e_output_prepare(Star6eOutputSetup *setup, const char *server_uri,
	const char *stream_mode_name, uint16_t max_frame_size, int connected_udp);

/** Check if output setup is configured for RTP streaming. */
int star6e_output_setup_is_rtp(const Star6eOutputSetup *setup);

/** Reset output state to uninitialized. */
void star6e_output_reset(Star6eOutput *output);

/** Create socket and connect to destination per setup config. */
int star6e_output_init(Star6eOutput *output, const Star6eOutputSetup *setup);

/** Check if active output uses RTP mode. */
int star6e_output_is_rtp(const Star6eOutput *output);

/** Check if active output uses shared memory mode. */
int star6e_output_is_shm(const Star6eOutput *output);

/** Send RTP header and payload as a single UDP datagram. */
int star6e_output_send_rtp_parts(const Star6eOutput *output,
	const uint8_t *header, size_t header_len, const uint8_t *payload,
	size_t payload_len);

/** Send one raw packet in compact stream mode. */
int star6e_output_send_compact_packet(const Star6eOutput *output,
	const uint8_t *packet, uint32_t packet_size, uint32_t max_size);

/** Send entire encoder frame in compact stream mode. */
size_t star6e_output_send_compact_frame(const Star6eOutput *output,
	const MI_VENC_Stream_t *stream, uint32_t max_size);

/** Send encoder frame via configured output mode (RTP or compact). */
size_t star6e_output_send_frame(const Star6eOutput *output,
	const MI_VENC_Stream_t *stream, uint32_t max_size,
	Star6eOutputRtpSendFn rtp_send, void *opaque);

/** Change output destination URI without stopping streaming. */
int star6e_output_apply_server(Star6eOutput *output, const char *uri);

/** Close socket and release output resources. */
void star6e_output_teardown(Star6eOutput *output);

/** Reset audio output state to uninitialized. */
void star6e_audio_output_reset(Star6eAudioOutput *audio_output);

/** Initialize audio output sharing the video socket on a different port. */
int star6e_audio_output_init(Star6eAudioOutput *audio_output,
	const Star6eOutput *video_output, uint16_t port_override,
	uint16_t max_payload_size);

/** Return the UDP port used for audio RTP transmission. */
uint16_t star6e_audio_output_port(const Star6eAudioOutput *audio_output);

/** Send audio frame as RTP packets. */
int star6e_audio_output_send_rtp(const Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len, RtpPacketizerState *rtp_state,
	uint32_t frame_ticks);

/** Send audio frame in compact mode (raw bytes, no RTP). */
int star6e_audio_output_send_compact(const Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len);

/** Send audio frame using the configured output mode. */
int star6e_audio_output_send(const Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len, RtpPacketizerState *rtp_state,
	uint32_t frame_ticks);

/** Release audio output resources. */
void star6e_audio_output_teardown(Star6eAudioOutput *audio_output);

#endif
