/*
 * Unit tests for imu_ring.h — IMU sample ring buffer.
 * Host-compiled (no hardware dependencies).
 *
 * Build: gcc -O2 -I../include -pthread tests/test_imu_ring.c -o tests/test_imu_ring
 * Run:   ./tests/test_imu_ring
 */

#include "imu_ring.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
	tests_run++; \
	printf("  %-40s ", #name); \
	fflush(stdout); \
} while (0)

#define PASS() do { \
	tests_passed++; \
	printf("PASS\n"); \
} while (0)

#define FAIL(msg) do { \
	printf("FAIL: %s\n", msg); \
} while (0)

static struct timespec make_ts(long sec, long nsec)
{
	struct timespec ts = { .tv_sec = sec, .tv_nsec = nsec };
	return ts;
}

static ImuRingSample make_sample(long sec, long nsec, float gx)
{
	ImuRingSample s = {0};
	s.ts = make_ts(sec, nsec);
	s.gyro_x = gx;
	return s;
}

/* Test: init/destroy doesn't crash */
static void test_init_destroy(void)
{
	TEST(init_destroy);
	ImuRing r;
	imu_ring_init(&r);
	assert(r.head == 0);
	assert(r.count == 0);
	imu_ring_destroy(&r);
	PASS();
}

/* Test: push and count */
static void test_push_count(void)
{
	TEST(push_count);
	ImuRing r;
	imu_ring_init(&r);

	ImuRingSample s = make_sample(1, 0, 1.0f);
	imu_ring_push(&r, &s);
	assert(r.count == 1);
	assert(r.head == 1);

	s = make_sample(2, 0, 2.0f);
	imu_ring_push(&r, &s);
	assert(r.count == 2);
	assert(r.head == 2);

	imu_ring_destroy(&r);
	PASS();
}

/* Test: FIFO ordering — oldest to newest */
static void test_ordering(void)
{
	TEST(ordering);
	ImuRing r;
	imu_ring_init(&r);

	for (int i = 0; i < 10; i++) {
		ImuRingSample s = make_sample(i, 0, (float)i);
		imu_ring_push(&r, &s);
	}

	ImuRingSample out[10];
	uint32_t n = imu_ring_read_range(&r, make_ts(0, 0), make_ts(9, 0),
		out, 10);

	assert(n == 10);
	for (uint32_t i = 0; i < n; i++) {
		assert(out[i].gyro_x == (float)i);
	}

	imu_ring_destroy(&r);
	PASS();
}

/* Test: capacity overflow wraps correctly */
static void test_overflow_wrap(void)
{
	TEST(overflow_wrap);
	ImuRing r;
	imu_ring_init(&r);

	/* Fill beyond capacity */
	for (int i = 0; i < IMU_RING_CAPACITY + 100; i++) {
		ImuRingSample s = make_sample(i, 0, (float)i);
		imu_ring_push(&r, &s);
	}

	assert(r.count == IMU_RING_CAPACITY);

	/* Oldest should be 100, newest should be CAPACITY+99 */
	ImuRingSample out[IMU_RING_CAPACITY];
	uint32_t n = imu_ring_read_range(&r, make_ts(0, 0),
		make_ts(IMU_RING_CAPACITY + 200, 0), out, IMU_RING_CAPACITY);

	assert(n == IMU_RING_CAPACITY);
	assert(out[0].gyro_x == 100.0f);
	assert(out[n - 1].gyro_x == (float)(IMU_RING_CAPACITY + 99));

	imu_ring_destroy(&r);
	PASS();
}

/* Test: read_range with time window filtering */
static void test_read_range_filter(void)
{
	TEST(read_range_filter);
	ImuRing r;
	imu_ring_init(&r);

	for (int i = 0; i < 20; i++) {
		ImuRingSample s = make_sample(i, 0, (float)i);
		imu_ring_push(&r, &s);
	}

	/* Read only samples between t=5 and t=14 */
	ImuRingSample out[20];
	uint32_t n = imu_ring_read_range(&r, make_ts(5, 0), make_ts(14, 0),
		out, 20);

	assert(n == 10);
	assert(out[0].gyro_x == 5.0f);
	assert(out[9].gyro_x == 14.0f);

	imu_ring_destroy(&r);
	PASS();
}

/* Test: read_range with empty ring */
static void test_read_range_empty(void)
{
	TEST(read_range_empty);
	ImuRing r;
	imu_ring_init(&r);

	ImuRingSample out[10];
	uint32_t n = imu_ring_read_range(&r, make_ts(0, 0), make_ts(10, 0),
		out, 10);

	assert(n == 0);

	imu_ring_destroy(&r);
	PASS();
}

/* Test: read_range with no matching samples */
static void test_read_range_no_match(void)
{
	TEST(read_range_no_match);
	ImuRing r;
	imu_ring_init(&r);

	for (int i = 0; i < 5; i++) {
		ImuRingSample s = make_sample(i, 0, (float)i);
		imu_ring_push(&r, &s);
	}

	/* Query a time window that doesn't overlap */
	ImuRingSample out[10];
	uint32_t n = imu_ring_read_range(&r, make_ts(100, 0), make_ts(200, 0),
		out, 10);

	assert(n == 0);

	imu_ring_destroy(&r);
	PASS();
}

/* Test: max_out limits output */
static void test_max_out_limit(void)
{
	TEST(max_out_limit);
	ImuRing r;
	imu_ring_init(&r);

	for (int i = 0; i < 100; i++) {
		ImuRingSample s = make_sample(i, 0, (float)i);
		imu_ring_push(&r, &s);
	}

	ImuRingSample out[5];
	uint32_t n = imu_ring_read_range(&r, make_ts(0, 0), make_ts(99, 0),
		out, 5);

	assert(n == 5);
	/* Should be the first 5 matching (oldest) */
	assert(out[0].gyro_x == 0.0f);
	assert(out[4].gyro_x == 4.0f);

	imu_ring_destroy(&r);
	PASS();
}

/* Test: nanosecond-level timestamp comparison */
static void test_ts_nanoseconds(void)
{
	TEST(ts_nanoseconds);
	ImuRing r;
	imu_ring_init(&r);

	/* Push samples at sub-second intervals */
	for (int i = 0; i < 10; i++) {
		ImuRingSample s = make_sample(1, i * 100000000L, (float)i);
		imu_ring_push(&r, &s);
	}

	/* Read samples between 0.3s and 0.7s within second 1 */
	ImuRingSample out[10];
	uint32_t n = imu_ring_read_range(&r,
		make_ts(1, 300000000L), make_ts(1, 700000000L),
		out, 10);

	assert(n == 5); /* samples at 0.3, 0.4, 0.5, 0.6, 0.7 */
	assert(out[0].gyro_x == 3.0f);
	assert(out[4].gyro_x == 7.0f);

	imu_ring_destroy(&r);
	PASS();
}

/* Test: 6-axis data preservation */
static void test_six_axis_data(void)
{
	TEST(six_axis_data);
	ImuRing r;
	imu_ring_init(&r);

	ImuRingSample s = {0};
	s.ts = make_ts(1, 0);
	s.gyro_x = 1.1f;
	s.gyro_y = 2.2f;
	s.gyro_z = 3.3f;
	s.accel_x = 4.4f;
	s.accel_y = 5.5f;
	s.accel_z = 9.81f;
	imu_ring_push(&r, &s);

	ImuRingSample out[1];
	uint32_t n = imu_ring_read_range(&r, make_ts(0, 0), make_ts(2, 0),
		out, 1);

	assert(n == 1);
	assert(out[0].gyro_x == 1.1f);
	assert(out[0].gyro_y == 2.2f);
	assert(out[0].gyro_z == 3.3f);
	assert(out[0].accel_x == 4.4f);
	assert(out[0].accel_y == 5.5f);
	assert(out[0].accel_z == 9.81f);

	imu_ring_destroy(&r);
	PASS();
}

/* Test: concurrent push/read (basic thread safety) */
static void *writer_thread(void *arg)
{
	ImuRing *r = (ImuRing *)arg;
	for (int i = 0; i < 10000; i++) {
		ImuRingSample s = make_sample(i / 1000, (i % 1000) * 1000000L, (float)i);
		imu_ring_push(r, &s);
	}
	return NULL;
}

static void test_concurrent_push_read(void)
{
	TEST(concurrent_push_read);
	ImuRing r;
	imu_ring_init(&r);

	pthread_t writer;
	pthread_create(&writer, NULL, writer_thread, &r);

	/* Concurrent reads while writer is pushing */
	int read_count = 0;
	for (int i = 0; i < 100; i++) {
		ImuRingSample out[64];
		uint32_t n = imu_ring_read_range(&r, make_ts(0, 0), make_ts(100, 0),
			out, 64);
		read_count += n;
		/* Just verify no crash, no assertion on exact count */
	}

	pthread_join(writer, NULL);

	/* After writer done, verify ring is consistent */
	assert(r.count <= IMU_RING_CAPACITY);
	assert(r.count > 0);

	imu_ring_destroy(&r);
	PASS();
}

int main(void)
{
	printf("imu_ring unit tests:\n");

	test_init_destroy();
	test_push_count();
	test_ordering();
	test_overflow_wrap();
	test_read_range_filter();
	test_read_range_empty();
	test_read_range_no_match();
	test_max_out_limit();
	test_ts_nanoseconds();
	test_six_axis_data();
	test_concurrent_push_read();

	printf("\n%d/%d tests passed\n", tests_passed, tests_run);
	return (tests_passed == tests_run) ? 0 : 1;
}
