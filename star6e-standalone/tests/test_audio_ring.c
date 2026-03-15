#include "audio_ring.h"

#include "test_helpers.h"

#include <stdio.h>
#include <string.h>

static int test_audio_ring_init(void)
{
	AudioRing r;
	int failures = 0;

	audio_ring_init(&r);
	CHECK("ring init count", r.count == 0);
	CHECK("ring init head", r.head == 0);
	CHECK("ring init tail", r.tail == 0);
	CHECK("ring init dropped", r.dropped == 0);
	audio_ring_destroy(&r);
	return failures;
}

static int test_audio_ring_push_pop(void)
{
	AudioRing r;
	AudioRingEntry out;
	uint8_t pcm[16];
	int failures = 0;
	int ret;

	audio_ring_init(&r);

	memset(pcm, 0xAB, sizeof(pcm));
	audio_ring_push(&r, pcm, sizeof(pcm), 12345);
	CHECK("push count", audio_ring_count(&r) == 1);

	ret = audio_ring_pop(&r, &out);
	CHECK("pop returns 1", ret == 1);
	CHECK("pop length", out.length == sizeof(pcm));
	CHECK("pop timestamp", out.timestamp_us == 12345);
	CHECK("pop data", out.pcm[0] == 0xAB && out.pcm[15] == 0xAB);
	CHECK("count after pop", audio_ring_count(&r) == 0);

	audio_ring_destroy(&r);
	return failures;
}

static int test_audio_ring_empty_pop(void)
{
	AudioRing r;
	AudioRingEntry out;
	int failures = 0;

	audio_ring_init(&r);
	CHECK("pop empty returns 0", audio_ring_pop(&r, &out) == 0);
	audio_ring_destroy(&r);
	return failures;
}

static int test_audio_ring_fifo_order(void)
{
	AudioRing r;
	AudioRingEntry out;
	uint8_t pcm[4];
	int failures = 0;

	audio_ring_init(&r);

	for (uint32_t i = 0; i < 5; i++) {
		pcm[0] = (uint8_t)i;
		audio_ring_push(&r, pcm, 1, (uint64_t)i * 100);
	}
	CHECK("fifo count", audio_ring_count(&r) == 5);

	for (uint32_t i = 0; i < 5; i++) {
		int ret = audio_ring_pop(&r, &out);
		CHECK("fifo pop ok", ret == 1);
		CHECK("fifo order", out.pcm[0] == (uint8_t)i);
		CHECK("fifo timestamp", out.timestamp_us == (uint64_t)i * 100);
	}
	CHECK("fifo empty after", audio_ring_count(&r) == 0);

	audio_ring_destroy(&r);
	return failures;
}

static int test_audio_ring_overflow(void)
{
	AudioRing r;
	AudioRingEntry out;
	uint8_t pcm[4];
	int failures = 0;

	audio_ring_init(&r);

	/* Fill beyond capacity */
	for (uint32_t i = 0; i < AUDIO_RING_CAPACITY + 10; i++) {
		pcm[0] = (uint8_t)(i & 0xFF);
		audio_ring_push(&r, pcm, 1, (uint64_t)i);
	}

	CHECK("overflow count capped", audio_ring_count(&r) == AUDIO_RING_CAPACITY);
	CHECK("overflow dropped", r.dropped == 10);

	/* First pop should be entry #10 (oldest 10 were dropped) */
	audio_ring_pop(&r, &out);
	CHECK("overflow oldest is #10", out.pcm[0] == 10);

	audio_ring_destroy(&r);
	return failures;
}

static int test_audio_ring_large_pcm_clamped(void)
{
	AudioRing r;
	AudioRingEntry out;
	uint8_t pcm[AUDIO_RING_PCM_MAX + 100];
	int failures = 0;

	audio_ring_init(&r);
	memset(pcm, 0xCC, sizeof(pcm));
	audio_ring_push(&r, pcm, (uint16_t)sizeof(pcm), 999);
	audio_ring_pop(&r, &out);
	CHECK("large pcm clamped", out.length == AUDIO_RING_PCM_MAX);

	audio_ring_destroy(&r);
	return failures;
}

int test_audio_ring(void)
{
	int failures = 0;

	failures += test_audio_ring_init();
	failures += test_audio_ring_push_pop();
	failures += test_audio_ring_empty_pop();
	failures += test_audio_ring_fifo_order();
	failures += test_audio_ring_overflow();
	failures += test_audio_ring_large_pcm_clamped();

	return failures;
}
