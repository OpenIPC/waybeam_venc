#ifndef EIS_RING_H
#define EIS_RING_H

/*
 * Shared timestamped gyro-only motion sample ring buffer.
 * Header-only, thread-safe (mutex-protected push/read).
 *
 * Used by both EIS backends (legacy and gyroglide) for
 * frame-synchronous batch extraction of gyro data.
 *
 * Follows the same pattern as imu_ring.h.
 */

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

typedef struct {
	struct timespec ts;
	float gyro_x;
	float gyro_y;
	float gyro_z;
} EisMotionSample;

#define EIS_RING_CAPACITY 2048

typedef struct {
	EisMotionSample buf[EIS_RING_CAPACITY];
	uint32_t head;     /* next write index */
	uint32_t count;    /* valid samples in buffer */
	pthread_mutex_t lock;
} EisMotionRing;

static inline void eis_ring_init(EisMotionRing *r)
{
	memset(r, 0, sizeof(*r));
	pthread_mutex_init(&r->lock, NULL);
}

static inline void eis_ring_destroy(EisMotionRing *r)
{
	pthread_mutex_destroy(&r->lock);
}

static inline void eis_ring_push(EisMotionRing *r, const EisMotionSample *s)
{
	pthread_mutex_lock(&r->lock);
	r->buf[r->head] = *s;
	r->head = (r->head + 1) % EIS_RING_CAPACITY;
	if (r->count < EIS_RING_CAPACITY)
		r->count++;
	pthread_mutex_unlock(&r->lock);
}

/* Return 1 if a >= b (struct timespec comparison) */
static inline int eis_ring_ts_ge(struct timespec a, struct timespec b)
{
	if (a.tv_sec != b.tv_sec)
		return a.tv_sec > b.tv_sec;
	return a.tv_nsec >= b.tv_nsec;
}

/*
 * eis_ring_read_range — Copy samples between t0 and t1 (inclusive).
 * Returns number of samples copied, up to max_out.
 */
static inline uint32_t eis_ring_read_range(EisMotionRing *r,
	struct timespec t0, struct timespec t1,
	EisMotionSample *out, uint32_t max_out)
{
	pthread_mutex_lock(&r->lock);
	uint32_t n = 0;
	if (r->count == 0) {
		pthread_mutex_unlock(&r->lock);
		return 0;
	}

	uint32_t start;
	if (r->count < EIS_RING_CAPACITY)
		start = 0;
	else
		start = r->head; /* oldest sample */

	for (uint32_t i = 0; i < r->count && n < max_out; i++) {
		uint32_t idx = (start + i) % EIS_RING_CAPACITY;
		const EisMotionSample *s = &r->buf[idx];
		if (eis_ring_ts_ge(s->ts, t0) && eis_ring_ts_ge(t1, s->ts))
			out[n++] = *s;
	}
	pthread_mutex_unlock(&r->lock);
	return n;
}

/* Return current sample count (thread-safe). */
static inline uint32_t eis_ring_count(EisMotionRing *r)
{
	pthread_mutex_lock(&r->lock);
	uint32_t c = r->count;
	pthread_mutex_unlock(&r->lock);
	return c;
}

#endif /* EIS_RING_H */
