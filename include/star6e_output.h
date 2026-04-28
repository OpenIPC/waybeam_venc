#ifndef STAR6E_OUTPUT_H
#define STAR6E_OUTPUT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "rtp_packetizer.h"
#include "star6e.h"
#include "venc_config.h"
#include "venc_ring.h"

/* Per-frame sendmmsg() batch capacity. Typical frame at 25 Mbps/120 fps is
 * 19 RTP packets; IDR can exceed that but rarely >50. If it does, the send
 * path mid-flushes (one extra sendmmsg call) rather than dropping. */
#define STAR6E_OUTPUT_BATCH_MAX 64
/* Size of owned per-slot scratch that holds a copy of the RTP header
 * concatenated with payload1. payload1 is either a 3-byte FU header
 * (tiny) or an AP packet (up to one MTU). Worst case at the
 * VENC_OUTPUT_PAYLOAD_CEILING_BYTES (4000) live-validated max is
 * 12 (RTP) + 4000 = 4012 bytes; rounded up to 4096 for slack and
 * alignment. Sized for jumbo-frame links such as the Realtek 3993 MTU. */
#define STAR6E_OUTPUT_BATCH_SLOT_SCRATCH 4096

typedef enum {
	STAR6E_STREAM_MODE_COMPACT = 0,
	STAR6E_STREAM_MODE_RTP = 1,
} Star6eStreamMode;

typedef struct {
	Star6eStreamMode stream_mode;
	VencOutputUri uri;
	int requested_connected_udp;
	int has_server;
} Star6eOutputSetup;

/* Per-frame sendmmsg batch. We own `scratch[slot]` containing
 * [RTP header || payload1] concatenated — both the header (built on the
 * caller's stack by rtp_packetizer_send_packet) and payload1 (an FU-A
 * header or an AP builder buffer, both reused across packets) must be
 * copied because the caller reuses that memory before we flush.
 *
 * payload2, when present, is a slice of the encoder NAL buffer — that
 * memory is stable until MI_VENC_ReleaseStream which is called after
 * end_frame(), so we keep it as a zero-copy iovec pointer.
 *
 * iovec layout: 2 iovs per slot. iov[2*slot] -> scratch[slot], length =
 * header_len+payload1_len. iov[2*slot+1] -> external payload2 pointer
 * (msg_iovlen becomes 1 if payload2 is absent).
 *
 * Transport snapshot (socket_handle/dst/dst_len/connected_udp) is
 * captured at begin_frame() under the transport_gen seqlock so that a
 * concurrent apply_server() on the HTTP thread cannot retarget queued
 * packets mid-frame. Enqueue and flush dereference only batch fields,
 * never the live Star6eOutput transport state. */
typedef struct {
	uint8_t scratch[STAR6E_OUTPUT_BATCH_MAX][STAR6E_OUTPUT_BATCH_SLOT_SCRATCH];
	struct iovec iov[STAR6E_OUTPUT_BATCH_MAX * 2];
	struct mmsghdr msgs[STAR6E_OUTPUT_BATCH_MAX];
	size_t count;
	int active;
	/* Transport snapshot taken at begin_frame() under transport_gen */
	int socket_handle;
	struct sockaddr_storage dst;
	socklen_t dst_len;
	int connected_udp;
} Star6eOutputBatch;

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
	int send_buf_capacity; /* cached SO_SNDBUF (kernel-reported), 0 = unknown */
	Star6eOutputBatch batch;
	/* Transport-pressure observation state (telemetry only — never gates
	 * frame transmission).  Meaningful for any transport with a queue:
	 * shm:// (ring fill), unix:// / udp:// (SIOCOUTQ / SO_SNDBUF).
	 * `in_pressure` is the hysteresis flag (enters at fill_pct >=
	 * cfg->outgoing.high_water_pct, exits at fill_pct < lo).
	 * `pressure_drops` increments whenever the flag is asserted —
	 * "frames observed in pressure" (the wire field name `pressure_drops`
	 * is preserved for ABI stability; pre-v0.9.2 it meant "frames
	 * skipped" but post-encode skip was rolled back, see HISTORY).
	 * Both surfaced via /api/v1/transport/status and the sidecar
	 * trailer.  uint32_t (not uint64_t) so the HTTP-thread reader sees
	 * an atomic single-load on ARMv7; wraps in 24855 days at 120 fps
	 * continuous pressure — fine. */
	int in_pressure;
	uint32_t pressure_drops;
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
	const char *stream_mode_name, int connected_udp);

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

/** Observe transport pressure for telemetry. Updates `output->in_pressure`
 *  with hysteresis on cfg->outgoing watermarks and increments
 *  `output->pressure_drops` while in pressure.  Never directs the caller
 *  to skip — the caller MUST always emit the frame.  See
 *  `venc_observe_pressure` in venc_ring.h for the rationale (skip-on-
 *  pressure broke H.265 reference chains). */
void star6e_output_observe_pressure(Star6eOutput *output,
	const VencConfig *cfg);

/** Begin accumulating RTP packets for a frame. When the transport is UDP
 *  and SHM is not in use, subsequent star6e_output_send_rtp_parts() calls
 *  queue into a sendmmsg() batch instead of per-packet sendmsg(). The
 *  batch is flushed by star6e_output_end_frame(). Safe to call when the
 *  transport is SHM — it becomes a no-op in that case. */
void star6e_output_begin_frame(Star6eOutput *output);

/** Flush any batched RTP packets for the current frame via sendmmsg().
 *  Returns the number of messages successfully sent, or 0 if batching is
 *  inactive (no packets queued or SHM transport). */
int star6e_output_end_frame(Star6eOutput *output);

/** Send RTP header and payload parts as a single UDP datagram.
 *  payload2 may be NULL/0 for single-part payloads.
 *  When a batch is active (between begin_frame/end_frame) and the transport
 *  is UDP, the packet is queued into the batch. Otherwise it is sent
 *  immediately. */
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
