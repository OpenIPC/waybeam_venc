#ifndef ENC_RING_BUFFER_H
#define ENC_RING_BUFFER_H

/*
 * Lock-free SPSC ring buffer for encoder frame stats.
 * Single producer (encoder callback thread) writes stats.
 * Single consumer (external reader) reads stats.
 *
 * All memory is preallocated at init — zero allocation in hot path.
 * Memory ordering: acquire/release on index variables.
 */

#include "enc_types.h"

#include <stdint.h>

/* Slot count must be power of 2 */
#define ENC_RING_DEFAULT_SLOTS 512

typedef struct {
	EncoderFrameStats *slots;
	uint32_t           slot_count;      /* must be power of 2 */
	uint32_t           slot_mask;       /* slot_count - 1 */
	volatile uint64_t  write_idx;
	volatile uint64_t  read_idx;
	uint64_t           total_writes;
	uint64_t           total_overwrites;
} EncRingBuffer;

/** Initialize ring buffer with preallocated storage.
 *  slots: caller-owned array of slot_count EncoderFrameStats.
 *  slot_count: must be power of 2.
 *  Returns 0 on success, -1 on invalid args. */
int enc_ring_init(EncRingBuffer *rb, EncoderFrameStats *slots,
	uint32_t slot_count);

/** Write one frame stats entry (producer side, non-blocking).
 *  Overwrites oldest entry if ring is full. */
void enc_ring_write(EncRingBuffer *rb, const EncoderFrameStats *stats);

/** Read oldest unread entry (consumer side, non-blocking).
 *  Returns 0 if an entry was read, -1 if ring is empty. */
int enc_ring_read(EncRingBuffer *rb, EncoderFrameStats *out);

/** Read up to max_count entries into buf (consumer side).
 *  Returns number of entries actually read. */
uint16_t enc_ring_read_batch(EncRingBuffer *rb, EncoderFrameStats *buf,
	uint16_t max_count);

/** Return number of entries available for reading. */
uint32_t enc_ring_available(const EncRingBuffer *rb);

/** Peek at the most recently written entry without consuming.
 *  Returns 0 on success, -1 if ring has no data. */
int enc_ring_peek_latest(const EncRingBuffer *rb, EncoderFrameStats *out);

#endif /* ENC_RING_BUFFER_H */
