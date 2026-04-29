/*
 * Unit tests for venc_ring.c — SPSC shared memory ring buffer.
 *
 * Tests: lifecycle (create/attach/destroy), single write/read,
 * fill/drain, wraparound, slot stride alignment, validation,
 * concurrent producer/consumer with pthread, hardening (corrupt
 * header, corrupt slot length, overflow, init_complete, stats).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>

#include "venc_ring.h"
#include "test_helpers.h"

/* ── Lifecycle ──────────────────────────────────────────────────────── */

static int test_create_destroy(void)
{
	int failures = 0;

	venc_ring_t *r = venc_ring_create("test_cd", 4, 1400);
	CHECK("create_ok", r != NULL);
	CHECK("create_magic", r->hdr->magic == VENC_RING_MAGIC);
	CHECK("create_version", r->hdr->version == VENC_RING_VERSION);
	CHECK("create_slot_count", r->hdr->slot_count == 4);
	CHECK("create_slot_data_size", r->hdr->slot_data_size == 1400);
	CHECK("create_is_owner", r->is_owner == 1);
	CHECK("create_init_complete", r->hdr->init_complete == 1);
	CHECK("create_epoch_nonzero", r->hdr->epoch != 0);
	venc_ring_destroy(r);

	/* Destroy NULL is safe */
	venc_ring_destroy(NULL);

	return failures;
}

static int test_create_validation(void)
{
	int failures = 0;

	/* NULL name */
	CHECK("null_name", venc_ring_create(NULL, 4, 1400) == NULL);
	/* Empty name */
	CHECK("empty_name", venc_ring_create("", 4, 1400) == NULL);
	/* Non-power-of-2 slot count */
	CHECK("non_pow2", venc_ring_create("test_v1", 3, 1400) == NULL);
	CHECK("zero_slots", venc_ring_create("test_v2", 0, 1400) == NULL);
	/* Zero data size */
	CHECK("zero_data", venc_ring_create("test_v3", 4, 0) == NULL);
	/* Data size too large */
	CHECK("data_too_large", venc_ring_create("test_v4", 4, 65536) == NULL);
	/* Boundary: max valid data size */
	venc_ring_t *r = venc_ring_create("test_v5", 4, 65535);
	CHECK("max_data_ok", r != NULL);
	venc_ring_destroy(r);

	return failures;
}

static int test_attach(void)
{
	int failures = 0;

	venc_ring_t *producer = venc_ring_create("test_att", 8, 2048);
	CHECK("att_create", producer != NULL);

	venc_ring_t *consumer = venc_ring_attach("test_att");
	CHECK("att_attach", consumer != NULL);
	CHECK("att_magic", consumer->hdr->magic == VENC_RING_MAGIC);
	CHECK("att_slot_count", consumer->hdr->slot_count == 8);
	CHECK("att_data_size", consumer->hdr->slot_data_size == 2048);
	CHECK("att_not_owner", consumer->is_owner == 0);

	/* Both see same shared memory (different mmaps, same content) */
	CHECK("att_shared_write_idx",
	      &producer->hdr->write_idx != &consumer->hdr->write_idx);

	/* Write via producer, read via consumer to prove shared memory */
	uint8_t data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
	int ret = venc_ring_write(producer, data, 4, NULL, 0);
	CHECK("att_cross_write", ret == 0);
	uint8_t buf[32];
	uint16_t out_len;
	ret = venc_ring_read(consumer, buf, sizeof(buf), &out_len);
	CHECK("att_cross_read", ret == 0);
	CHECK("att_cross_data", memcmp(buf, data, 4) == 0);

	venc_ring_destroy(consumer);
	venc_ring_destroy(producer);

	/* Attach to non-existent ring */
	CHECK("att_missing", venc_ring_attach("test_nonexist") == NULL);

	return failures;
}

/* ── Single write/read ──────────────────────────────────────────────── */

static int test_write_read(void)
{
	int failures = 0;

	venc_ring_t *r = venc_ring_create("test_wr", 4, 64);
	CHECK("wr_create", r != NULL);

	/* Write a two-part message (hdr + payload) like RTP */
	uint8_t hdr[12] = {0x80, 0x61, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
	uint8_t payload[20];
	memset(payload, 0xAB, sizeof(payload));

	int ret = venc_ring_write(r, hdr, 12, payload, 20);
	CHECK("wr_write_ok", ret == 0);

	/* Read it back */
	uint8_t buf[64];
	uint16_t out_len = 0;
	ret = venc_ring_read(r, buf, sizeof(buf), &out_len);
	CHECK("wr_read_ok", ret == 0);
	CHECK("wr_read_len", out_len == 32);
	CHECK("wr_read_hdr", memcmp(buf, hdr, 12) == 0);
	CHECK("wr_read_payload", buf[12] == 0xAB && buf[31] == 0xAB);

	/* Ring should be empty now */
	ret = venc_ring_read(r, buf, sizeof(buf), &out_len);
	CHECK("wr_empty", ret == -1);

	venc_ring_destroy(r);
	return failures;
}

/* ── Overflow: write too large ──────────────────────────────────────── */

static int test_write_overflow(void)
{
	int failures = 0;

	venc_ring_t *r = venc_ring_create("test_of", 4, 32);
	CHECK("of_create", r != NULL);

	uint8_t big[64];
	memset(big, 0xFF, sizeof(big));

	/* Total exceeds slot_data_size → should fail */
	int ret = venc_ring_write(r, big, 20, big, 20);
	CHECK("of_too_large", ret == -1);

	/* Exactly at limit should succeed */
	ret = venc_ring_write(r, big, 16, big, 16);
	CHECK("of_exact_fit", ret == 0);

	venc_ring_destroy(r);
	return failures;
}

/* ── Fill and drain ─────────────────────────────────────────────────── */

static int test_fill_drain(void)
{
	int failures = 0;

	venc_ring_t *r = venc_ring_create("test_fd", 4, 32);
	CHECK("fd_create", r != NULL);

	uint8_t data[8];

	/* Fill all 4 slots */
	for (int i = 0; i < 4; i++) {
		memset(data, (uint8_t)i, sizeof(data));
		int ret = venc_ring_write(r, data, sizeof(data), NULL, 0);
		CHECK("fd_fill", ret == 0);
	}

	/* 5th write should fail (ring full) */
	int ret = venc_ring_write(r, data, sizeof(data), NULL, 0);
	CHECK("fd_full", ret == -1);

	/* Drain all 4 and verify ordering */
	uint8_t buf[32];
	uint16_t out_len;
	for (int i = 0; i < 4; i++) {
		ret = venc_ring_read(r, buf, sizeof(buf), &out_len);
		CHECK("fd_drain_ok", ret == 0);
		CHECK("fd_drain_len", out_len == 8);
		CHECK("fd_drain_data", buf[0] == (uint8_t)i);
	}

	/* Should be empty */
	ret = venc_ring_read(r, buf, sizeof(buf), &out_len);
	CHECK("fd_empty", ret == -1);

	venc_ring_destroy(r);
	return failures;
}

/* ── Wraparound ─────────────────────────────────────────────────────── */

static int test_wraparound(void)
{
	int failures = 0;

	venc_ring_t *r = venc_ring_create("test_wrap", 4, 32);
	CHECK("wrap_create", r != NULL);

	uint8_t data[8];
	uint8_t buf[32];
	uint16_t out_len;

	/* Write and read 10 times (wraps around 4-slot ring multiple times) */
	for (int i = 0; i < 10; i++) {
		memset(data, (uint8_t)i, sizeof(data));
		int ret = venc_ring_write(r, data, sizeof(data), NULL, 0);
		CHECK("wrap_write", ret == 0);

		ret = venc_ring_read(r, buf, sizeof(buf), &out_len);
		CHECK("wrap_read", ret == 0);
		CHECK("wrap_data", buf[0] == (uint8_t)i);
	}

	/* Verify indices have advanced beyond slot_count */
	CHECK("wrap_write_idx", r->hdr->write_idx == 10);
	CHECK("wrap_read_idx", r->hdr->read_idx == 10);

	venc_ring_destroy(r);
	return failures;
}

/* ── Concurrent producer/consumer ───────────────────────────────────── */

#define CONCURRENT_COUNT 10000

typedef struct {
	venc_ring_t *ring;
	int errors;
} ThreadArg;

static void *producer_thread(void *arg)
{
	ThreadArg *ta = (ThreadArg *)arg;
	uint8_t hdr[4] = {0};
	for (int i = 0; i < CONCURRENT_COUNT; i++) {
		/* Encode sequence number in header */
		hdr[0] = (uint8_t)(i & 0xFF);
		hdr[1] = (uint8_t)((i >> 8) & 0xFF);
		while (venc_ring_write(ta->ring, hdr, 4, NULL, 0) != 0) {
			/* Ring full — spin briefly */
			usleep(1);
		}
	}
	return NULL;
}

static void *consumer_thread(void *arg)
{
	ThreadArg *ta = (ThreadArg *)arg;
	uint8_t buf[64];
	uint16_t out_len;
	int expected = 0;
	for (int i = 0; i < CONCURRENT_COUNT; i++) {
		while (venc_ring_read(ta->ring, buf, sizeof(buf), &out_len) != 0) {
			usleep(1);
		}
		int seq = buf[0] | (buf[1] << 8);
		if (seq != expected)
			ta->errors++;
		expected++;
	}
	return NULL;
}

static int test_concurrent(void)
{
	int failures = 0;

	venc_ring_t *r = venc_ring_create("test_conc", 64, 32);
	CHECK("conc_create", r != NULL);

	ThreadArg prod_arg = {.ring = r, .errors = 0};
	ThreadArg cons_arg = {.ring = r, .errors = 0};

	pthread_t prod, cons;
	pthread_create(&cons, NULL, consumer_thread, &cons_arg);
	pthread_create(&prod, NULL, producer_thread, &prod_arg);

	pthread_join(prod, NULL);
	pthread_join(cons, NULL);

	CHECK("conc_no_errors", cons_arg.errors == 0);
	CHECK("conc_all_consumed", r->hdr->read_idx == CONCURRENT_COUNT);

	venc_ring_destroy(r);
	return failures;
}

/* ── Slot stride alignment ──────────────────────────────────────────── */

static int test_stride_alignment(void)
{
	int failures = 0;

	/* slot_data_size=13 → raw = 2+13=15, aligned to 16 */
	venc_ring_t *r = venc_ring_create("test_align", 4, 13);
	CHECK("align_create", r != NULL);
	CHECK("align_stride_16", r->slot_stride == 16);
	venc_ring_destroy(r);

	/* slot_data_size=14 → raw = 2+14=16, aligned to 16 */
	r = venc_ring_create("test_align2", 4, 14);
	CHECK("align2_create", r != NULL);
	CHECK("align2_stride_16", r->slot_stride == 16);
	venc_ring_destroy(r);

	/* slot_data_size=1400 → raw = 2+1400=1402, aligned to 1408 */
	r = venc_ring_create("test_align3", 4, 1400);
	CHECK("align3_create", r != NULL);
	CHECK("align3_stride_1408", r->slot_stride == 1408);
	venc_ring_destroy(r);

	return failures;
}

/* ── Hardening: corrupt header ──────────────────────────────────────── */

static int test_corrupt_header(void)
{
	int failures = 0;

	venc_ring_t *r = venc_ring_create("test_corrupt_hdr", 4, 1400);
	CHECK("corrupt_hdr_create", r != NULL);

	/* Corrupt magic in SHM, then attempt attach — must fail */
	uint32_t saved_magic = r->hdr->magic;
	r->hdr->magic = 0xDEADBEEF;
	CHECK("corrupt_magic_attach", venc_ring_attach("test_corrupt_hdr") == NULL);
	r->hdr->magic = saved_magic;

	/* Corrupt version */
	uint32_t saved_ver = r->hdr->version;
	r->hdr->version = 99;
	CHECK("corrupt_version_attach", venc_ring_attach("test_corrupt_hdr") == NULL);
	r->hdr->version = saved_ver;

	/* Corrupt total_size (mismatch with computed) */
	uint32_t saved_total = r->hdr->total_size;
	r->hdr->total_size = saved_total + 4096;
	CHECK("corrupt_total_attach", venc_ring_attach("test_corrupt_hdr") == NULL);
	r->hdr->total_size = saved_total;

	/* Corrupt slot_count (non-power-of-2) */
	uint32_t saved_sc = r->hdr->slot_count;
	r->hdr->slot_count = 5;
	CHECK("corrupt_slotcount_attach", venc_ring_attach("test_corrupt_hdr") == NULL);
	r->hdr->slot_count = saved_sc;

	/* Verify normal attach still works after restoring */
	venc_ring_t *c = venc_ring_attach("test_corrupt_hdr");
	CHECK("restore_attach", c != NULL);
	if (c) venc_ring_destroy(c);

	venc_ring_destroy(r);
	return failures;
}

/* ── Hardening: corrupt slot length ─────────────────────────────────── */

static int test_corrupt_slot_length(void)
{
	int failures = 0;

	venc_ring_t *producer = venc_ring_create("test_corrupt_slot", 4, 64);
	CHECK("corrupt_slot_create", producer != NULL);

	/* Write a valid packet */
	uint8_t data[16];
	memset(data, 0xAA, sizeof(data));
	int ret = venc_ring_write(producer, data, 16, NULL, 0);
	CHECK("corrupt_slot_write", ret == 0);

	/* Manually corrupt the slot length to exceed slot_data_size */
	venc_ring_slot_t *slot = venc_ring_slot_at(producer, 0);
	slot->length = 9999;  /* way beyond 64-byte slot_data_size */

	/* Attach consumer and attempt read — must fail (skip corrupt slot) */
	venc_ring_t *consumer = venc_ring_attach("test_corrupt_slot");
	CHECK("corrupt_slot_attach", consumer != NULL);

	uint8_t buf[128];
	uint16_t out_len = 0;
	ret = venc_ring_read(consumer, buf, sizeof(buf), &out_len);
	CHECK("corrupt_slot_read_fail", ret == -1);
	CHECK("corrupt_slot_bad_drops", consumer->stats.bad_slot_drops == 1);

	/* read_idx should have advanced past the corrupt slot */
	CHECK("corrupt_slot_idx_advanced",
	      __atomic_load_n(&consumer->hdr->read_idx, __ATOMIC_RELAXED) == 1);

	if (consumer) venc_ring_destroy(consumer);
	venc_ring_destroy(producer);
	return failures;
}

/* ── Hardening: overflow size on create ─────────────────────────────── */

static int test_overflow_create(void)
{
	int failures = 0;

	/* Huge slot_count × slot_data_size that would overflow uint32_t */
	venc_ring_t *r = venc_ring_create("test_overflow",
	                                    (uint32_t)1 << 20,  /* 1M slots */
	                                    65535);              /* max data */
	CHECK("overflow_create_null", r == NULL);

	return failures;
}

/* ── Hardening: init_complete ───────────────────────────────────────── */

static int test_init_complete(void)
{
	int failures = 0;

	venc_ring_t *r = venc_ring_create("test_initc", 4, 1400);
	CHECK("initc_create", r != NULL);

	/* Clear init_complete — attach must fail */
	__atomic_store_n(&r->hdr->init_complete, 0, __ATOMIC_RELEASE);
	CHECK("initc_cleared_attach", venc_ring_attach("test_initc") == NULL);

	/* Restore — attach must succeed */
	__atomic_store_n(&r->hdr->init_complete, 1, __ATOMIC_RELEASE);
	venc_ring_t *c = venc_ring_attach("test_initc");
	CHECK("initc_restored_attach", c != NULL);
	if (c) venc_ring_destroy(c);

	venc_ring_destroy(r);
	return failures;
}

/* ── Hardening: stats counters ──────────────────────────────────────── */

static int test_stats_counters(void)
{
	int failures = 0;

	venc_ring_t *r = venc_ring_create("test_stats", 2, 32);
	CHECK("stats_create", r != NULL);

	uint8_t data[8];
	memset(data, 0x42, sizeof(data));

	/* Oversize write → oversize_drops */
	uint8_t big[64];
	memset(big, 0xFF, sizeof(big));
	venc_ring_write(r, big, 32, big, 16);  /* 48 > 32 */
	CHECK("stats_oversize", r->stats.oversize_drops == 1);

	/* Normal writes (fill 2 slots) */
	venc_ring_write(r, data, 8, NULL, 0);
	venc_ring_write(r, data, 8, NULL, 0);
	CHECK("stats_writes", r->stats.writes == 2);

	/* Full drop */
	venc_ring_write(r, data, 8, NULL, 0);
	CHECK("stats_full_drop", r->stats.full_drops == 1);

	/* Read 2 packets */
	uint8_t buf[64];
	uint16_t out_len;
	venc_ring_read(r, buf, sizeof(buf), &out_len);
	venc_ring_read(r, buf, sizeof(buf), &out_len);
	CHECK("stats_reads", r->stats.reads == 2);

	venc_ring_destroy(r);
	return failures;
}

/* ── Hardening: uint16_t overflow in write ──────────────────────────── */

static int test_write_u16_overflow(void)
{
	int failures = 0;

	venc_ring_t *r = venc_ring_create("test_u16ov", 4, 65535);
	CHECK("u16ov_create", r != NULL);

	/* hdr_len + payload_len = 65535 + 1 = 65536, would overflow uint16_t
	 * but total is computed as uint32_t, so it correctly exceeds slot_data_size
	 * and is rejected */
	uint8_t dummy[1] = {0};
	int ret = venc_ring_write(r, NULL, 0, dummy, 1);
	CHECK("u16ov_small_ok", ret == 0);

	/* This should succeed: exactly at limit */
	uint8_t *big = (uint8_t *)calloc(1, 65535);
	CHECK("u16ov_alloc", big != NULL);
	if (big) {
		ret = venc_ring_write(r, big, 65535, NULL, 0);
		CHECK("u16ov_max_ok", ret == 0);

		/* This should fail: 65535 + 1 = 65536 > 65535 */
		ret = venc_ring_write(r, big, 65535, dummy, 1);
		CHECK("u16ov_exceed_fail", ret == -1);
		free(big);
	}

	venc_ring_destroy(r);
	return failures;
}

/* ── Hardening: destroy clears init_complete (visible to consumer) ──── */

static int test_destroy_clears_init(void)
{
	int failures = 0;

	venc_ring_t *producer = venc_ring_create("test_destroy_ic", 4, 1400);
	CHECK("dic_create", producer != NULL);
	CHECK("dic_init_complete_1", producer->hdr->init_complete == 1);

	/* Consumer attaches — gets its own mmap of the same SHM pages */
	venc_ring_t *consumer = venc_ring_attach("test_destroy_ic");
	CHECK("dic_attach", consumer != NULL);
	CHECK("dic_consumer_sees_1",
	      __atomic_load_n(&consumer->hdr->init_complete, __ATOMIC_ACQUIRE) == 1);

	/* Save consumer's view of init_complete address before producer destroy */
	volatile uint32_t *consumer_ic = &consumer->hdr->init_complete;

	/* Producer destroys — must clear init_complete before munmap */
	venc_ring_destroy(producer);

	/* Consumer's mmap still points to the same underlying pages.
	 * init_complete should now be 0 (set by producer before munmap). */
	uint32_t ic = __atomic_load_n(consumer_ic, __ATOMIC_ACQUIRE);
	CHECK("dic_consumer_sees_0", ic == 0);

	/* Consumer can no longer re-attach (SHM was unlinked by producer) */
	CHECK("dic_reattach_fails", venc_ring_attach("test_destroy_ic") == NULL);

	venc_ring_destroy(consumer);
	return failures;
}

/* ── Hardening: producer restart with attached consumer ─────────────── */

static int test_producer_restart_orphans_old_inode(void)
{
	int failures = 0;

	/* Step 1: producer A creates the ring, consumer C attaches. */
	venc_ring_t *prod_a = venc_ring_create("test_relink", 4, 64);
	CHECK("rl_create_a", prod_a != NULL);
	uint32_t epoch_a = prod_a->hdr->epoch;

	venc_ring_t *cons_c = venc_ring_attach("test_relink");
	CHECK("rl_attach_c", cons_c != NULL);
	CHECK("rl_c_sees_epoch_a", cons_c->hdr->epoch == epoch_a);

	/* Step 2: simulate SIGKILL of producer A. Drop the local mapping
	 * without calling venc_ring_destroy() — /dev/shm/test_relink
	 * persists with init_complete=1, and consumer C still mmaps it. */
	munmap(prod_a->hdr, prod_a->map_size);
	free(prod_a);

	/* Step 3: producer B starts with the same name. Must succeed
	 * (shm_unlink + O_EXCL handles the stale file) and pick a fresh
	 * epoch. Sleep 2 ms so CLOCK_MONOTONIC ms tick advances —
	 * otherwise the new epoch can collide with epoch_a in the test. */
	struct timespec ts = {0, 2 * 1000 * 1000};
	nanosleep(&ts, NULL);

	venc_ring_t *prod_b = venc_ring_create("test_relink", 4, 64);
	CHECK("rl_create_b", prod_b != NULL);
	CHECK("rl_b_new_epoch", prod_b->hdr->epoch != epoch_a);

	/* Step 4: consumer C is mapped to the orphaned inode and still
	 * sees A's epoch. (This is the property the wfb_tx-side guard
	 * uses to detect producer restart and force re-attach.) */
	CHECK("rl_c_still_sees_epoch_a", cons_c->hdr->epoch == epoch_a);

	/* Step 5: a fresh consumer attaching by name now resolves to
	 * producer B's inode, sees B's epoch. */
	venc_ring_t *cons_c2 = venc_ring_attach("test_relink");
	CHECK("rl_attach_c2", cons_c2 != NULL);
	CHECK("rl_c2_sees_epoch_b", cons_c2->hdr->epoch == prod_b->hdr->epoch);

	venc_ring_destroy(cons_c2);
	venc_ring_destroy(cons_c);
	venc_ring_destroy(prod_b);

	return failures;
}

/* ── Entry point ────────────────────────────────────────────────────── */

int test_venc_ring(void)
{
	int failures = 0;

	failures += test_create_destroy();
	failures += test_create_validation();
	failures += test_attach();
	failures += test_write_read();
	failures += test_write_overflow();
	failures += test_fill_drain();
	failures += test_wraparound();
	failures += test_concurrent();
	failures += test_stride_alignment();
	failures += test_corrupt_header();
	failures += test_corrupt_slot_length();
	failures += test_overflow_create();
	failures += test_init_complete();
	failures += test_stats_counters();
	failures += test_write_u16_overflow();
	failures += test_destroy_clears_init();
	failures += test_producer_restart_orphans_old_inode();

	return failures;
}
