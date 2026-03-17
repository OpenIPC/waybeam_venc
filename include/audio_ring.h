#ifndef AUDIO_RING_H
#define AUDIO_RING_H

/*
 * SPSC mutex-based ring buffer for PCM audio frames.
 * Bridges audio capture thread → main recording loop.
 * Follows imu_ring.h pattern.
 */

#include <stdint.h>
#include <string.h>
#include <pthread.h>

/* Max PCM frame size: 320 samples × 2 bytes × 2 channels = 1280 */
#define AUDIO_RING_PCM_MAX   1280
#define AUDIO_RING_CAPACITY  64

typedef struct {
	uint8_t  pcm[AUDIO_RING_PCM_MAX];
	uint16_t length;           /* actual PCM bytes this frame */
	uint64_t timestamp_us;     /* CLOCK_MONOTONIC microseconds */
} AudioRingEntry;

typedef struct {
	AudioRingEntry buf[AUDIO_RING_CAPACITY];
	uint32_t head;      /* next write index */
	uint32_t tail;      /* next read index */
	uint32_t count;     /* entries currently in ring */
	uint32_t dropped;   /* overflow counter */
	pthread_mutex_t lock;
	pthread_cond_t  cond;  /* signalled by push; used by pop_wait */
} AudioRing;

static inline void audio_ring_init(AudioRing *r)
{
	memset(r, 0, sizeof(*r));
	pthread_mutex_init(&r->lock, NULL);
	pthread_cond_init(&r->cond, NULL);
}

static inline void audio_ring_destroy(AudioRing *r)
{
	pthread_cond_destroy(&r->cond);
	pthread_mutex_destroy(&r->lock);
}

/** Push a PCM frame into the ring.  Drops oldest on overflow.
 *  Signals any thread blocked in audio_ring_pop_wait(). */
static inline void audio_ring_push(AudioRing *r,
	const uint8_t *pcm, uint16_t length, uint64_t timestamp_us)
{
	pthread_mutex_lock(&r->lock);

	AudioRingEntry *e = &r->buf[r->head];
	uint16_t copy_len = length;
	if (copy_len > AUDIO_RING_PCM_MAX)
		copy_len = AUDIO_RING_PCM_MAX;
	memcpy(e->pcm, pcm, copy_len);
	e->length = copy_len;
	e->timestamp_us = timestamp_us;

	r->head = (r->head + 1) % AUDIO_RING_CAPACITY;
	if (r->count < AUDIO_RING_CAPACITY) {
		r->count++;
	} else {
		/* Overflow: advance tail, drop oldest */
		r->tail = (r->tail + 1) % AUDIO_RING_CAPACITY;
		r->dropped++;
	}

	pthread_cond_signal(&r->cond);
	pthread_mutex_unlock(&r->lock);
}

/** Wake any thread blocked in audio_ring_pop_wait() without pushing data.
 *  Use to unblock the consumer on shutdown. */
static inline void audio_ring_wake(AudioRing *r)
{
	pthread_mutex_lock(&r->lock);
	pthread_cond_signal(&r->cond);
	pthread_mutex_unlock(&r->lock);
}

/** Pop a PCM frame, blocking up to timeout_ms for one to arrive.
 *  Returns 1 on success, 0 on timeout. */
static inline int audio_ring_pop_wait(AudioRing *r, AudioRingEntry *out,
	int timeout_ms)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (long)timeout_ms * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&r->lock);
	while (r->count == 0) {
		if (pthread_cond_timedwait(&r->cond, &r->lock, &ts) != 0) {
			pthread_mutex_unlock(&r->lock);
			return 0;
		}
	}
	*out = r->buf[r->tail];
	r->tail = (r->tail + 1) % AUDIO_RING_CAPACITY;
	r->count--;
	pthread_mutex_unlock(&r->lock);
	return 1;
}

/** Pop a PCM frame from the ring.  Returns 1 on success, 0 if empty. */
static inline int audio_ring_pop(AudioRing *r, AudioRingEntry *out)
{
	pthread_mutex_lock(&r->lock);

	if (r->count == 0) {
		pthread_mutex_unlock(&r->lock);
		return 0;
	}

	*out = r->buf[r->tail];
	r->tail = (r->tail + 1) % AUDIO_RING_CAPACITY;
	r->count--;

	pthread_mutex_unlock(&r->lock);
	return 1;
}

/** Return current entry count (snapshot). */
static inline uint32_t audio_ring_count(AudioRing *r)
{
	uint32_t n;

	pthread_mutex_lock(&r->lock);
	n = r->count;
	pthread_mutex_unlock(&r->lock);
	return n;
}

#endif /* AUDIO_RING_H */
