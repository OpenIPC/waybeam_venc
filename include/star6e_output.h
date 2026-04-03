#ifndef STAR6E_OUTPUT_H
#define STAR6E_OUTPUT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "rtp_packetizer.h"
#include "star6e.h"
#include "venc_config.h"
#include "venc_ring.h"

typedef enum {
	STAR6E_STREAM_MODE_COMPACT = 0,
	STAR6E_STREAM_MODE_RTP = 1,
} Star6eStreamMode;

typedef struct {
	Star6eStreamMode stream_mode;
	VencOutputUri uri;
	int requested_connected_udp;
	int has_server;
	uint16_t max_frame_size;
} Star6eOutputSetup;

typedef struct {
	Star6eStreamMode stream_mode;
	VencOutputUriType transport;
	int socket_handle;
	struct sockaddr_storage dst;
	socklen_t dst_len;
	int connected_udp;
	int requested_connected_udp;
	venc_ring_t *ring;
	uint32_t send_errors;
	uint32_t transport_gen; /* seqlock: odd = write in progress, even = stable */
} Star6eOutput;

typedef struct {
	int socket_handle;
	struct sockaddr_storage dst;
	socklen_t dst_len;
} Star6eAudioSendTarget;

typedef struct {
	const Star6eOutput *video_output;
	int socket_handle;
	struct sockaddr_storage fallback_dst;
	socklen_t fallback_dst_len;
	uint16_t port_override;
	uint16_t max_payload_size;
	Star6eAudioSendTarget cached_target;
	uint32_t cached_gen;
	int cache_valid;
} Star6eAudioOutput;

typedef size_t (*Star6eOutputRtpSendFn)(Star6eOutput *output,
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

/** Send RTP header and payload parts as a single UDP datagram.
 *  payload2 may be NULL/0 for single-part payloads. */
int star6e_output_send_rtp_parts(Star6eOutput *output,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len);

/** Return and reset accumulated send error count. */
uint32_t star6e_output_drain_send_errors(Star6eOutput *output);

/** Send one raw packet in compact stream mode. */
int star6e_output_send_compact_packet(Star6eOutput *output,
	const uint8_t *packet, uint32_t packet_size, uint32_t max_size);

/** Send entire encoder frame in compact stream mode. */
size_t star6e_output_send_compact_frame(Star6eOutput *output,
	const MI_VENC_Stream_t *stream, uint32_t max_size);

/** Send encoder frame via configured output mode (RTP or compact). */
size_t star6e_output_send_frame(Star6eOutput *output,
	const MI_VENC_Stream_t *stream, uint32_t max_size,
	Star6eOutputRtpSendFn rtp_send, void *opaque);

/** Change output destination URI without stopping streaming. */
int star6e_output_apply_server(Star6eOutput *output, const char *uri);

/** Close socket and release output resources. */
void star6e_output_teardown(Star6eOutput *output);

/** Reset audio output state to uninitialized. */
void star6e_audio_output_reset(Star6eAudioOutput *audio_output);

/** Initialize audio output.
 *  port_override=0 shares the active video destination.
 *  port_override!=0 uses dedicated UDP audio, following the video host for
 *  udp:// output and falling back to 127.0.0.1 for unix:// or shm:// video. */
int star6e_audio_output_init(Star6eAudioOutput *audio_output,
	const Star6eOutput *video_output, uint16_t port_override,
	uint16_t max_payload_size);

/** Return the configured UDP audio port, or the shared UDP video port. */
uint16_t star6e_audio_output_port(const Star6eAudioOutput *audio_output);

/** Send audio frame as RTP packets. */
int star6e_audio_output_send_rtp(Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len, RtpPacketizerState *rtp_state,
	uint32_t frame_ticks);

/** Send audio frame in compact mode (raw bytes, no RTP). */
int star6e_audio_output_send_compact(Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len);

/** Send audio frame using the configured output mode. */
int star6e_audio_output_send(Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len, RtpPacketizerState *rtp_state,
	uint32_t frame_ticks);

/** Release audio output resources. */
void star6e_audio_output_teardown(Star6eAudioOutput *audio_output);

#endif
