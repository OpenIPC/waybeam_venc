#ifndef MARUKO_OUTPUT_H
#define MARUKO_OUTPUT_H

#include "venc_config.h"
#include "venc_ring.h"

#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>

/* Per-frame sendmmsg() batch capacity. Typical frame at 25 Mbps/120 fps is
 * 19 RTP packets; IDR can exceed that but rarely >50. If it does, the send
 * path mid-flushes (one extra sendmmsg call) rather than dropping. */
#define MARUKO_OUTPUT_BATCH_MAX 64
/* Size of owned per-slot scratch that holds a copy of the RTP header
 * concatenated with payload1. payload1 is either a 3-byte FU-A header
 * (tiny) or an AP packet (up to one MTU). Worst case at the
 * VENC_OUTPUT_PAYLOAD_CEILING_BYTES (4000) live-validated max is
 * 12 (RTP) + 4000 = 4012 bytes; rounded up to 4096 for slack and
 * alignment. Sized for jumbo-frame links such as the Realtek 3993 MTU. */
#define MARUKO_OUTPUT_BATCH_SLOT_SCRATCH 4096

/* Per-frame sendmmsg batch. We own `scratch[slot]` containing
 * [RTP header || payload1] concatenated — both the header (built on the
 * caller's stack by rtp_packetizer_send_packet) and payload1 (an FU-A
 * header or an AP builder buffer, both reused across packets) must be
 * copied because the caller reuses that memory before we flush.
 *
 * payload2, when present, is a slice of the encoder NAL buffer — that
 * memory is stable until the encoder stream is released after end_frame(),
 * so we keep it as a zero-copy iovec pointer.
 *
 * Transport snapshot (socket_handle/dst/dst_len/connected_udp) is
 * captured at begin_frame() under the transport_gen seqlock so that a
 * concurrent apply_server() on the HTTP thread cannot retarget queued
 * packets mid-frame. Enqueue and flush dereference only batch fields,
 * never the live MarukoOutput transport state. */
typedef struct {
	uint8_t scratch[MARUKO_OUTPUT_BATCH_MAX][MARUKO_OUTPUT_BATCH_SLOT_SCRATCH];
	struct iovec iov[MARUKO_OUTPUT_BATCH_MAX * 2];
	struct mmsghdr msgs[MARUKO_OUTPUT_BATCH_MAX];
	size_t count;
	int active;
	/* Transport snapshot taken at begin_frame() under transport_gen */
	int socket_handle;
	struct sockaddr_storage dst;
	socklen_t dst_len;
	int connected_udp;
} MarukoOutputBatch;

/** Maruko output module — manages socket/SHM lifecycle and destination. */
typedef struct {
	int socket_handle;
	struct sockaddr_storage dst;
	socklen_t dst_len;
	VencOutputUriType transport;
	venc_ring_t *ring;
	int requested_connected_udp; /* user preference, persisted for apply_server */
	int connected_udp;           /* actual kernel state — set by configure() */
	uint32_t send_errors;
	uint32_t transport_gen; /* seqlock: odd = write in progress, even = stable */
	MarukoOutputBatch batch;
} MarukoOutput;

/** Initialize UDP or Unix socket output from a parsed URI. */
int maruko_output_init(MarukoOutput *output, const VencOutputUri *uri,
	int requested_connected_udp);

/** Initialize SHM output: create shared memory ring buffer. */
int maruko_output_init_shm(MarukoOutput *output, const char *shm_name);

/** Change output destination URI without stopping streaming (udp:// or unix://). */
int maruko_output_apply_server(MarukoOutput *output, const char *uri);

/** Begin accumulating RTP packets for a frame. When the transport is UDP
 *  and SHM is not in use, subsequent enqueues queue into a sendmmsg()
 *  batch. The batch is flushed by maruko_output_end_frame(). Safe to call
 *  when the transport is SHM — it becomes a no-op in that case. */
void maruko_output_begin_frame(MarukoOutput *output);

/** Flush any batched RTP packets for the current frame via sendmmsg().
 *  Returns number of messages successfully sent, or 0 if batching is
 *  inactive (no packets queued or SHM transport). */
int maruko_output_end_frame(MarukoOutput *output);

/** Queue one RTP packet into the active batch. Returns 0 on success,
 *  -1 if the packet cannot fit the scratch slot (caller falls back to
 *  immediate send). Intended for use by maruko_video.c's RTP write
 *  callback. */
int maruko_output_batch_enqueue(MarukoOutput *output,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len);

/** Return and reset accumulated send error count. */
uint32_t maruko_output_drain_send_errors(MarukoOutput *output);

/** Close socket/ring and release output resources. */
void maruko_output_teardown(MarukoOutput *output);

#endif /* MARUKO_OUTPUT_H */
