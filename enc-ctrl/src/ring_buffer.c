#include "ring_buffer.h"

#include <string.h>

static int is_power_of_2(uint32_t v)
{
	return v > 0 && (v & (v - 1)) == 0;
}

int enc_ring_init(EncRingBuffer *rb, EncoderFrameStats *slots,
	uint32_t slot_count)
{
	if (!rb || !slots || !is_power_of_2(slot_count))
		return -1;

	memset(rb, 0, sizeof(*rb));
	rb->slots = slots;
	rb->slot_count = slot_count;
	rb->slot_mask = slot_count - 1;
	return 0;
}

void enc_ring_write(EncRingBuffer *rb, const EncoderFrameStats *stats)
{
	uint64_t w;
	uint64_t rd;
	uint32_t idx;

	if (!rb || !stats)
		return;

	w = __atomic_load_n(&rb->write_idx, __ATOMIC_RELAXED);
	rd = __atomic_load_n(&rb->read_idx, __ATOMIC_ACQUIRE);

	/* If ring is full, advance read pointer (overwrite oldest) */
	if (w - rd >= rb->slot_count) {
		__atomic_store_n(&rb->read_idx, rd + 1, __ATOMIC_RELEASE);
		rb->total_overwrites++;
	}

	idx = (uint32_t)(w & rb->slot_mask);
	rb->slots[idx] = *stats;

	__atomic_store_n(&rb->write_idx, w + 1, __ATOMIC_RELEASE);
	rb->total_writes++;
}

int enc_ring_read(EncRingBuffer *rb, EncoderFrameStats *out)
{
	uint64_t rd;
	uint64_t w;
	uint32_t idx;

	if (!rb || !out)
		return -1;

	rd = __atomic_load_n(&rb->read_idx, __ATOMIC_RELAXED);
	w = __atomic_load_n(&rb->write_idx, __ATOMIC_ACQUIRE);

	if (rd >= w)
		return -1;

	idx = (uint32_t)(rd & rb->slot_mask);
	*out = rb->slots[idx];

	__atomic_store_n(&rb->read_idx, rd + 1, __ATOMIC_RELEASE);
	return 0;
}

uint16_t enc_ring_read_batch(EncRingBuffer *rb, EncoderFrameStats *buf,
	uint16_t max_count)
{
	uint16_t count = 0;

	if (!rb || !buf)
		return 0;

	while (count < max_count) {
		if (enc_ring_read(rb, &buf[count]) != 0)
			break;
		count++;
	}

	return count;
}

uint32_t enc_ring_available(const EncRingBuffer *rb)
{
	uint64_t w;
	uint64_t rd;

	if (!rb)
		return 0;

	w = __atomic_load_n(&rb->write_idx, __ATOMIC_ACQUIRE);
	rd = __atomic_load_n(&rb->read_idx, __ATOMIC_RELAXED);

	if (w <= rd)
		return 0;

	return (uint32_t)(w - rd);
}

int enc_ring_peek_latest(const EncRingBuffer *rb, EncoderFrameStats *out)
{
	uint64_t w;
	uint32_t idx;

	if (!rb || !out)
		return -1;

	w = __atomic_load_n(&rb->write_idx, __ATOMIC_ACQUIRE);
	if (w == 0)
		return -1;

	idx = (uint32_t)((w - 1) & rb->slot_mask);
	*out = rb->slots[idx];
	return 0;
}

uint16_t enc_ring_snapshot(const EncRingBuffer *rb, EncoderFrameStats *buf,
	uint16_t max_count)
{
	uint64_t w;
	uint64_t rd;
	uint32_t avail;
	uint32_t count;
	uint32_t i;
	uint64_t start;

	if (!rb || !buf || max_count == 0)
		return 0;

	w = __atomic_load_n(&rb->write_idx, __ATOMIC_ACQUIRE);
	rd = __atomic_load_n(&rb->read_idx, __ATOMIC_RELAXED);

	if (w <= rd)
		return 0;

	avail = (uint32_t)(w - rd);
	count = avail < (uint32_t)max_count ? avail : (uint32_t)max_count;

	/* Copy the most recent 'count' entries, oldest first */
	start = w - count;
	for (i = 0; i < count; i++) {
		uint32_t idx = (uint32_t)((start + i) & rb->slot_mask);
		buf[i] = rb->slots[idx];
	}

	return (uint16_t)count;
}
