#ifndef IMU_RING_H
#define IMU_RING_H

/*
 * Generic timestamped 6-axis IMU sample ring buffer.
 * Thread-safe (mutex-protected push/read).
 *
 * Originally extracted from the (now-removed, 0.8.0) EIS module
 * for reuse by any frame-synced consumer — telemetry export,
 * sidecar gcsv logging, future stabilization framework, etc.
 */

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

typedef struct {
	struct timespec ts;
	float gyro_x, gyro_y, gyro_z;    /* rad/s */
	float accel_x, accel_y, accel_z;  /* m/s^2 */
} ImuRingSample;

#define IMU_RING_CAPACITY 2048

typedef struct {
	ImuRingSample buf[IMU_RING_CAPACITY];
	uint32_t head;     /* next write index */
	uint32_t count;    /* valid samples in buffer */
	pthread_mutex_t lock;
} ImuRing;

static inline void imu_ring_init(ImuRing *r)
{
	memset(r, 0, sizeof(*r));
	pthread_mutex_init(&r->lock, NULL);
}

static inline void imu_ring_destroy(ImuRing *r)
{
	pthread_mutex_destroy(&r->lock);
}

static inline void imu_ring_push(ImuRing *r, const ImuRingSample *s)
{
	pthread_mutex_lock(&r->lock);
	r->buf[r->head] = *s;
	r->head = (r->head + 1) % IMU_RING_CAPACITY;
	if (r->count < IMU_RING_CAPACITY)
		r->count++;
	pthread_mutex_unlock(&r->lock);
}

/* Return 1 if a >= b (struct timespec comparison) */
static inline int imu_ring_ts_ge(struct timespec a, struct timespec b)
{
	if (a.tv_sec != b.tv_sec)
		return a.tv_sec > b.tv_sec;
	return a.tv_nsec >= b.tv_nsec;
}

/*
 * imu_ring_read_range — Copy samples between t0 and t1 (inclusive) into out[].
 * Returns number of samples copied, up to max_out.
 */
static inline uint32_t imu_ring_read_range(ImuRing *r, struct timespec t0,
	struct timespec t1, ImuRingSample *out, uint32_t max_out)
{
	pthread_mutex_lock(&r->lock);
	uint32_t n = 0;
	if (r->count == 0) {
		pthread_mutex_unlock(&r->lock);
		return 0;
	}

	/* Walk from oldest to newest, collect samples in [t0, t1] */
	uint32_t start;
	if (r->count < IMU_RING_CAPACITY)
		start = 0;
	else
		start = r->head; /* oldest sample */

	for (uint32_t i = 0; i < r->count && n < max_out; i++) {
		uint32_t idx = (start + i) % IMU_RING_CAPACITY;
		const ImuRingSample *s = &r->buf[idx];
		/* s->ts >= t0 && s->ts <= t1 */
		if (imu_ring_ts_ge(s->ts, t0) && imu_ring_ts_ge(t1, s->ts)) {
			out[n++] = *s;
		}
	}
	pthread_mutex_unlock(&r->lock);
	return n;
}

#endif
