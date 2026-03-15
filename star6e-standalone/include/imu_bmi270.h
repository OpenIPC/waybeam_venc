#ifndef IMU_BMI270_H
#define IMU_BMI270_H

#include <stdint.h>
#include <pthread.h>
#include <time.h>

/*
 * BMI270 IMU driver — standalone module for frame-synced 6-axis data.
 *
 * Two reader modes, selected automatically:
 *   - FIFO (frame-synced): BMI270 FIFO accumulates samples between video
 *     frames.  Caller drains via imu_drain() at each frame — no thread.
 *   - Polling (threaded): background thread reads one sample per ioctl
 *     at ODR.  Fallback when FIFO init fails.
 *
 * Usage (FIFO / frame-synced):
 *   1. Call imu_init() with config
 *   2. Call imu_start() — enables FIFO, no thread launched
 *   3. Call imu_drain() once per video frame
 *   4. Call imu_stop() + imu_destroy() at shutdown
 *
 * Usage (polling / fallback):
 *   1-2. Same as above — imu_start() launches reader thread
 *   3.   imu_drain() is a no-op (thread pushes samples continuously)
 *   4.   Same
 */

typedef struct {
	struct timespec ts;  /* CLOCK_MONOTONIC timestamp (interpolated in FIFO mode) */
	float gyro_x;    /* rad/s */
	float gyro_y;    /* rad/s */
	float gyro_z;    /* rad/s */
	float accel_x;   /* m/s^2 */
	float accel_y;   /* m/s^2 */
	float accel_z;   /* m/s^2 */
} ImuSample;

/* Callback: called from IMU thread for each sample.  Must be thread-safe. */
typedef void (*imu_push_fn)(void *ctx, const ImuSample *sample);

typedef struct {
	const char *i2c_device;  /* e.g. "/dev/i2c-1" */
	uint8_t i2c_addr;        /* e.g. 0x68 */
	int sample_rate_hz;      /* target rate (200 default) */
	int gyro_range_dps;      /* ±deg/s: 125, 250, 500, 1000, 2000 (default 1000) */
	const char *cal_file;    /* calibration file path (default: /etc/imu.cal) */
	int cal_samples;         /* auto-bias samples at startup (default: 400 = 2s@200Hz) */
	imu_push_fn push_fn;     /* sample callback */
	void *push_ctx;          /* opaque context for callback */
	int use_thread;          /* 1 = force reader thread (for standalone test without frame loop) */
} ImuConfig;

typedef struct ImuState ImuState;

/*
 * imu_init — Open I2C, upload firmware, configure sensor.
 * Returns NULL on failure (prints error to stderr).
 */
ImuState *imu_init(const ImuConfig *cfg);

/*
 * imu_start — Enable FIFO or launch polling thread.
 * In FIFO mode, no thread is started — call imu_drain() per frame.
 * Returns 0 on success.
 */
int imu_start(ImuState *st);

/*
 * imu_drain — Read all accumulated FIFO samples and push via callback.
 * Call once per video frame.  Returns sample count.
 * In polling mode (no FIFO), this is a no-op returning 0.
 */
int imu_drain(ImuState *st);

/*
 * imu_stop — Signal thread to stop and join.  Safe to call if not started.
 */
void imu_stop(ImuState *st);

/*
 * imu_destroy — Close I2C fd and free state.  Call imu_stop() first.
 * Safe to call with NULL.
 */
void imu_destroy(ImuState *st);

/*
 * imu_get_stats — Query sample counters for diagnostics.
 */
typedef struct {
	uint64_t samples_read;
	uint64_t read_errors;
	float last_gyro_x, last_gyro_y, last_gyro_z;
} ImuStats;

void imu_get_stats(ImuState *st, ImuStats *out);

#endif
