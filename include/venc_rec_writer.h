#ifndef VENC_REC_WRITER_H
#define VENC_REC_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Asynchronous recorder writer — keeps a blocking disk from stalling the
 * live video path.
 *
 * Every backend calls the recorder from its encode drain loop, between the
 * transport send and the SDK's ReleaseStream.  The recorder write is a
 * blocking write(2), so when storage blocks, ReleaseStream is late, the
 * encoder's output queue backs up, and the LIVE transport stalls with it.
 *
 * Device-measured on CV610 with a marginal SD card, sampling the transport
 * and the recorder together once a second:
 *
 *     t   pkt/s   recfps
 *     6   1437     103
 *     7    321      22   <- both collapse
 *     8    314      26   <- together, in the same proportion
 *     9   1495      89   <- both recover together
 *
 * The proportionality is what identifies it as one thread: a network fault
 * would have left the recorder at 103 fps.  Idle, the same stream is flat
 * within +/-3%.  A coarser run caught a full second at zero packets followed
 * by catch-up bursts well above the steady rate, so frames were delayed and
 * then flushed — latency jitter rather than loss, but a one-second stall on a
 * live FPV link is not survivable.
 *
 * This module moves the write to its own thread behind a bounded queue, so a
 * slow disk costs a dropped RECORDING frame and never a stalled live link.
 * That is the same principle as the 0.69.0 egress work: the live path is not
 * hostage to a secondary consumer.
 *
 * Star6E's dual mode already drains its second channel on a dedicated thread
 * and is therefore immune; this gives mirror mode the same property.
 *
 * Ownership: push() takes the access unit buffer and frees it, whether it is
 * queued or dropped.  Callers hand over a malloc'd buffer and forget it.
 *
 * Threading: exactly one producer (the drain loop) and one consumer (the
 * writer thread).  The sink therefore runs on the writer thread, so the
 * recorder state it mutates must not be touched by the producer while the
 * writer is running.
 *
 * LIFETIME — one writer per RECORDING, not one per process.  Start it after
 * the recorder file is open; stop it before closing that file.  Both stops
 * end in an unconditional pthread_join, so on return the sink is guaranteed
 * never to run again and the close needs no further serialisation.  A
 * long-lived writer would need a separate barrier before every close, a lock
 * between the sink and the close for the case that barrier cannot cover, and
 * a counter reset at every start; tying the lifetime to the recording
 * removes all three.  It also makes the handle itself the answer to "is a
 * recording running", which is what the backends' producer gates test.
 */

/* Called on the writer thread, once per queued access unit, in order. */
typedef void (*VencRecSinkFn)(void *ctx, const uint8_t *au, size_t len,
	uint64_t pts_90khz, int is_idr);

/* Queue cap.  A bound in BYTES rather than frames: frame sizes vary by an
 * order of magnitude between a P-slice and an IRAP, so a frame count would
 * cap memory at whatever the worst case happened to be.  4 MB is about 3.5 s
 * at 9 Mbps and is a ceiling, not a reservation — buffers are per-frame and
 * the queue sits empty unless the disk is actually stalling. */
#define VENC_REC_WRITER_MAX_BYTES (4u * 1024u * 1024u)

/* Writer-thread stack.  The sink runs the TS muxer, whose
 * star6e_ts_recorder_write_video() holds a 551 KB automatic array
 * (ts_buf[3000 * 188]).  musl — every SigmaStar and CV610 target — gives a
 * new pthread 128 KB by default, so the default would overflow on the first
 * recorded frame.  glibc's 8 MB default is why a host test cannot catch it.
 * Lazily committed, so this costs nothing while idle. */
#define VENC_REC_WRITER_STACK_BYTES (1024u * 1024u)

typedef struct VencRecWriter VencRecWriter;

/* Create the queue and start the writer thread.  Returns 0 on success and
 * stores the handle in *out; on failure *out is NULL and the caller should
 * fall back to writing synchronously. */
int venc_rec_writer_start(VencRecWriter **out, VencRecSinkFn sink, void *ctx);

/* Hand one access unit to the writer.  NEVER blocks.
 *
 * Takes ownership of `au` unconditionally: it is freed by the writer thread
 * after the sink runs, or immediately if the queue is full.  Returns 1 if
 * queued, 0 if dropped because the queue was at its cap.
 *
 * A NULL writer also frees the buffer and returns 0, so a caller that failed
 * to start one does not have to special-case the teardown path. */
int venc_rec_writer_push(VencRecWriter *w, uint8_t *au, size_t len,
	uint64_t pts_90khz, int is_idr);

/* Drain everything already queued, join the thread, and free the writer.
 * Safe on NULL.  After this returns, the sink is guaranteed not to run
 * again, so the recorder it writes to can be closed.
 *
 * UNBOUNDED: it waits for the whole queue to reach the disk.  Only safe to
 * call from a thread that is allowed to block for seconds — i.e. teardown,
 * not the encode loop.  Use venc_rec_writer_stop_bounded() there. */
void venc_rec_writer_stop(VencRecWriter *w);

/* As above, but give up after `grace_ms` and abandon whatever is still
 * queued.  *dropped (may be NULL) is SET to the writer's running total,
 * abandoned frames included — it is assigned, not accumulated into.
 *
 * Bounded by grace_ms PLUS at most one sink call: the queue is abandoned
 * before the join, so the join waits only for the access unit already in the
 * writer's hands.  The join still happens, so this is as hard a barrier as
 * the unbounded stop — it just keeps less of the recording.
 *
 * This is the stop for the encode loop.  Waiting for a stalled disk to
 * accept 4 MB would put the stall straight back on the live video path at
 * every record/stop — the exact failure this module removes, just relocated
 * to a rarer event.  Losing the tail of a recording is the correct trade. */
void venc_rec_writer_stop_bounded(VencRecWriter *w, unsigned grace_ms,
	uint64_t *dropped);

/* Observability.  `dropped` is the count of access units the queue refused
 * because it was full — silent drops would make a damaged recording look
 * like a clean one.  Any pointer may be NULL; a NULL writer zeroes them. */
void venc_rec_writer_stats(const VencRecWriter *w, uint64_t *queued,
	uint64_t *dropped, uint32_t *depth, uint32_t *peak_depth);

#ifdef __cplusplus
}
#endif

#endif /* VENC_REC_WRITER_H */
