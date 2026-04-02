#include "ring_buffer.h"
#include "test_helpers.h"

#include <string.h>

static int test_ring_basic(void)
{
	EncoderFrameStats slots[8];
	EncRingBuffer rb;
	EncoderFrameStats stats, out;
	int failures = 0;

	CHECK("ring init valid", enc_ring_init(&rb, slots, 8) == 0);
	CHECK("ring init null rb", enc_ring_init(NULL, slots, 8) == -1);
	CHECK("ring init null slots", enc_ring_init(&rb, NULL, 8) == -1);
	CHECK("ring init non-power-2", enc_ring_init(&rb, slots, 7) == -1);

	enc_ring_init(&rb, slots, 8);

	CHECK("ring empty read", enc_ring_read(&rb, &out) == -1);
	CHECK("ring available empty", enc_ring_available(&rb) == 0);

	memset(&stats, 0, sizeof(stats));
	stats.frame_seq = 42;
	stats.frame_size_bytes = 1000;
	enc_ring_write(&rb, &stats);

	CHECK("ring available 1", enc_ring_available(&rb) == 1);
	CHECK("ring read ok", enc_ring_read(&rb, &out) == 0);
	CHECK("ring read seq", out.frame_seq == 42);
	CHECK("ring read size", out.frame_size_bytes == 1000);
	CHECK("ring available after read", enc_ring_available(&rb) == 0);

	return failures;
}

static int test_ring_wrap(void)
{
	EncoderFrameStats slots[4];
	EncRingBuffer rb;
	EncoderFrameStats stats, out;
	int failures = 0;
	uint32_t i;

	enc_ring_init(&rb, slots, 4);

	/* Write 6 entries into 4-slot ring — should overwrite oldest 2 */
	for (i = 0; i < 6; i++) {
		memset(&stats, 0, sizeof(stats));
		stats.frame_seq = i + 1;
		enc_ring_write(&rb, &stats);
	}

	CHECK("ring wrap available", enc_ring_available(&rb) == 4);

	/* Should read 3, 4, 5, 6 (oldest 2 overwritten) */
	CHECK("ring wrap read 0", enc_ring_read(&rb, &out) == 0);
	CHECK("ring wrap seq 3", out.frame_seq == 3);

	CHECK("ring wrap read 1", enc_ring_read(&rb, &out) == 0);
	CHECK("ring wrap seq 4", out.frame_seq == 4);

	CHECK("ring wrap read 2", enc_ring_read(&rb, &out) == 0);
	CHECK("ring wrap seq 5", out.frame_seq == 5);

	CHECK("ring wrap read 3", enc_ring_read(&rb, &out) == 0);
	CHECK("ring wrap seq 6", out.frame_seq == 6);

	CHECK("ring wrap empty", enc_ring_read(&rb, &out) == -1);

	return failures;
}

static int test_ring_peek_latest(void)
{
	EncoderFrameStats slots[8];
	EncRingBuffer rb;
	EncoderFrameStats stats, out;
	int failures = 0;
	uint32_t i;

	enc_ring_init(&rb, slots, 8);

	CHECK("ring peek empty", enc_ring_peek_latest(&rb, &out) == -1);

	for (i = 0; i < 5; i++) {
		memset(&stats, 0, sizeof(stats));
		stats.frame_seq = (i + 1) * 10;
		enc_ring_write(&rb, &stats);
	}

	CHECK("ring peek ok", enc_ring_peek_latest(&rb, &out) == 0);
	CHECK("ring peek latest seq", out.frame_seq == 50);

	/* Peek should not consume */
	CHECK("ring available after peek", enc_ring_available(&rb) == 5);

	return failures;
}

static int test_ring_batch_read(void)
{
	EncoderFrameStats slots[8];
	EncRingBuffer rb;
	EncoderFrameStats stats;
	EncoderFrameStats batch[16];
	int failures = 0;
	uint16_t count;
	uint32_t i;

	enc_ring_init(&rb, slots, 8);

	for (i = 0; i < 5; i++) {
		memset(&stats, 0, sizeof(stats));
		stats.frame_seq = i;
		enc_ring_write(&rb, &stats);
	}

	count = enc_ring_read_batch(&rb, batch, 16);
	CHECK("batch count", count == 5);
	CHECK("batch seq 0", batch[0].frame_seq == 0);
	CHECK("batch seq 4", batch[4].frame_seq == 4);
	CHECK("batch empty after", enc_ring_available(&rb) == 0);

	return failures;
}

static int test_ring_snapshot(void)
{
	EncoderFrameStats slots[8];
	EncRingBuffer rb;
	EncoderFrameStats stats;
	EncoderFrameStats snap[16];
	int failures = 0;
	uint16_t count;
	uint32_t i;

	enc_ring_init(&rb, slots, 8);

	for (i = 0; i < 5; i++) {
		memset(&stats, 0, sizeof(stats));
		stats.frame_seq = i + 10;
		enc_ring_write(&rb, &stats);
	}

	count = enc_ring_snapshot(&rb, snap, 16);
	CHECK("snapshot count", count == 5);
	CHECK("snapshot oldest", snap[0].frame_seq == 10);
	CHECK("snapshot newest", snap[4].frame_seq == 14);

	/* Non-destructive: second call returns same data */
	count = enc_ring_snapshot(&rb, snap, 16);
	CHECK("snapshot repeat count", count == 5);
	CHECK("snapshot repeat oldest", snap[0].frame_seq == 10);

	/* Available unchanged (not consumed) */
	CHECK("snapshot no consume", enc_ring_available(&rb) == 5);

	/* Snapshot with wrap: write 10 entries into 8-slot ring */
	enc_ring_init(&rb, slots, 8);
	for (i = 0; i < 10; i++) {
		memset(&stats, 0, sizeof(stats));
		stats.frame_seq = i;
		enc_ring_write(&rb, &stats);
	}

	count = enc_ring_snapshot(&rb, snap, 4);
	CHECK("snapshot wrap count", count == 4);
	/* Most recent 4: seq 6, 7, 8, 9 */
	CHECK("snapshot wrap oldest", snap[0].frame_seq == 6);
	CHECK("snapshot wrap newest", snap[3].frame_seq == 9);

	return failures;
}

int test_ring_buffer(void)
{
	int failures = 0;

	failures += test_ring_basic();
	failures += test_ring_wrap();
	failures += test_ring_peek_latest();
	failures += test_ring_batch_read();
	failures += test_ring_snapshot();

	return failures;
}
