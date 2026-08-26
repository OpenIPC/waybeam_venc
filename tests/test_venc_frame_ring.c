/*
 * Unit tests for venc_frame_ring — SPSC shared memory ring buffer for
 * whole encoded video frames.
 *
 * Tests: lifecycle (create/attach/destroy), staged write, bulk write,
 * read, fill/drain, wraparound, validation, concurrent
 * producer/consumer with pthread, hardening (corrupt header, corrupt
 * slot length, oversize append, abort, stats, init_complete).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>

#include "venc_frame_ring.h"
#include "test_helpers.h"

/* ── Lifecycle ──────────────────────────────────────────────────────── */

static int test_fr_create_destroy(void)
{
	int failures = 0;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_cd", 4, 4096);
	CHECK("fr_create_ok", r != NULL);
	CHECK("fr_create_magic", r->hdr->magic == VENC_FRAME_RING_MAGIC);
	CHECK("fr_create_version", r->hdr->version == VENC_FRAME_RING_VERSION);
	CHECK("fr_create_slot_count", r->hdr->slot_count == 4);
	CHECK("fr_create_slot_data_size", r->hdr->slot_data_size == 4096);
	CHECK("fr_create_is_owner", r->is_owner == 1);
	CHECK("fr_create_init_complete", r->hdr->init_complete == 1);
	CHECK("fr_create_epoch_nonzero", r->hdr->epoch != 0);
	venc_frame_ring_destroy(r);

	venc_frame_ring_destroy(NULL);

	return failures;
}

static int test_fr_create_validation(void)
{
	int failures = 0;

	CHECK("fr_null_name", venc_frame_ring_create(NULL, 4, 4096) == NULL);
	CHECK("fr_empty_name", venc_frame_ring_create("", 4, 4096) == NULL);
	CHECK("fr_non_pow2", venc_frame_ring_create("test_fv1", 3, 4096) == NULL);
	CHECK("fr_zero_slots", venc_frame_ring_create("test_fv2", 0, 4096) == NULL);
	/* slot_data_size must be >= VENC_FRAME_META_SIZE (8) */
	CHECK("fr_data_too_small",
	      venc_frame_ring_create("test_fv3", 4, 4) == NULL);
	CHECK("fr_data_min",
	      venc_frame_ring_create("test_fv4", 4, VENC_FRAME_META_SIZE - 1) == NULL);

	venc_frame_ring_t *r = venc_frame_ring_create("test_fv5", 4,
		VENC_FRAME_META_SIZE);
	CHECK("fr_data_exact_meta_ok", r != NULL);
	venc_frame_ring_destroy(r);

	return failures;
}

static int test_fr_attach(void)
{
	int failures = 0;

	venc_frame_ring_t *producer = venc_frame_ring_create("test_fr_att", 8,
		4096);
	CHECK("fr_att_create", producer != NULL);

	venc_frame_ring_t *consumer = venc_frame_ring_attach("test_fr_att");
	CHECK("fr_att_attach", consumer != NULL);
	CHECK("fr_att_magic", consumer->hdr->magic == VENC_FRAME_RING_MAGIC);
	CHECK("fr_att_slot_count", consumer->hdr->slot_count == 8);
	CHECK("fr_att_data_size", consumer->hdr->slot_data_size == 4096);
	CHECK("fr_att_not_owner", consumer->is_owner == 0);

	/* Cross-process write/read via shared memory */
	VencFrameMeta meta;
	memset(&meta, 0, sizeof(meta));
	meta.pts = 12345;
	meta.codec = VENC_FRAME_CODEC_H265;
	meta.flags = VENC_FRAME_FLAG_IDR;

	uint8_t payload[16];
	memset(payload, 0xAB, sizeof(payload));

	int ret = venc_frame_ring_write(producer, &meta, payload, sizeof(payload));
	CHECK("fr_att_cross_write", ret == 0);

	uint8_t buf[4096];
	uint32_t out_len = 0;
	ret = venc_frame_ring_read(consumer, buf, sizeof(buf), &out_len);
	CHECK("fr_att_cross_read", ret == 0);
	CHECK("fr_att_cross_len",
	      out_len == VENC_FRAME_META_SIZE + sizeof(payload));

	VencFrameMeta *rm = (VencFrameMeta *)buf;
	CHECK("fr_att_cross_pts", rm->pts == 12345);
	CHECK("fr_att_cross_codec", rm->codec == VENC_FRAME_CODEC_H265);
	CHECK("fr_att_cross_flags", rm->flags == VENC_FRAME_FLAG_IDR);
	CHECK("fr_att_cross_payload",
	      buf[VENC_FRAME_META_SIZE] == 0xAB &&
	      buf[VENC_FRAME_META_SIZE + 15] == 0xAB);

	venc_frame_ring_destroy(consumer);
	venc_frame_ring_destroy(producer);

	CHECK("fr_att_missing",
	      venc_frame_ring_attach("test_fr_nonexist") == NULL);

	return failures;
}

/* ── Staged write API ──────────────────────────────────────────────── */

static int test_fr_staged_write(void)
{
	int failures = 0;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_staged", 4,
		1024);
	CHECK("fr_staged_create", r != NULL);

	VencFrameMeta meta;
	memset(&meta, 0, sizeof(meta));
	meta.pts = 42;
	meta.codec = VENC_FRAME_CODEC_H265;

	int ret = venc_frame_ring_begin_write(r, &meta);
	CHECK("fr_staged_begin", ret == 0);
	CHECK("fr_staged_active", r->write_active == 1);

	/* Append two NAL chunks */
	uint8_t nal1[32];
	memset(nal1, 0x11, sizeof(nal1));
	ret = venc_frame_ring_append(r, nal1, sizeof(nal1));
	CHECK("fr_staged_append1", ret == 0);

	uint8_t nal2[64];
	memset(nal2, 0x22, sizeof(nal2));
	ret = venc_frame_ring_append(r, nal2, sizeof(nal2));
	CHECK("fr_staged_append2", ret == 0);

	venc_frame_ring_commit_write(r);
	CHECK("fr_staged_committed", r->write_active == 0);

	/* Read it back */
	uint8_t buf[1024];
	uint32_t out_len = 0;
	ret = venc_frame_ring_read(r, buf, sizeof(buf), &out_len);
	CHECK("fr_staged_read", ret == 0);
	CHECK("fr_staged_read_len",
	      out_len == VENC_FRAME_META_SIZE + 32 + 64);

	VencFrameMeta *rm = (VencFrameMeta *)buf;
	CHECK("fr_staged_pts", rm->pts == 42);
	CHECK("fr_staged_nal1", buf[VENC_FRAME_META_SIZE] == 0x11);
	CHECK("fr_staged_nal2", buf[VENC_FRAME_META_SIZE + 32] == 0x22);

	/* Ring should be empty */
	ret = venc_frame_ring_read(r, buf, sizeof(buf), &out_len);
	CHECK("fr_staged_empty", ret == -1);

	venc_frame_ring_destroy(r);
	return failures;
}

/* ── Abort write ───────────────────────────────────────────────────── */

static int test_fr_abort_write(void)
{
	int failures = 0;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_abort", 4,
		1024);
	CHECK("fr_abort_create", r != NULL);

	VencFrameMeta meta;
	memset(&meta, 0, sizeof(meta));
	meta.pts = 99;
	meta.codec = VENC_FRAME_CODEC_H265;

	int ret = venc_frame_ring_begin_write(r, &meta);
	CHECK("fr_abort_begin", ret == 0);

	uint8_t data[16];
	memset(data, 0xCC, sizeof(data));
	ret = venc_frame_ring_append(r, data, sizeof(data));
	CHECK("fr_abort_append", ret == 0);

	venc_frame_ring_abort_write(r);
	CHECK("fr_abort_inactive", r->write_active == 0);

	/* write_idx should NOT have advanced */
	CHECK("fr_abort_no_advance",
	      __atomic_load_n(&r->hdr->write_idx, __ATOMIC_RELAXED) == 0);

	/* Ring should be empty */
	uint8_t buf[1024];
	uint32_t out_len;
	ret = venc_frame_ring_read(r, buf, sizeof(buf), &out_len);
	CHECK("fr_abort_empty", ret == -1);

	venc_frame_ring_destroy(r);
	return failures;
}

/* ── Oversize append ───────────────────────────────────────────────── */

static int test_fr_oversize_append(void)
{
	int failures = 0;

	/* slot_data_size = 64 (meta 8 + up to 56 bytes of NAL data) */
	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_ovs", 4, 64);
	CHECK("fr_ovs_create", r != NULL);

	VencFrameMeta meta;
	memset(&meta, 0, sizeof(meta));
	meta.codec = VENC_FRAME_CODEC_H265;

	int ret = venc_frame_ring_begin_write(r, &meta);
	CHECK("fr_ovs_begin", ret == 0);

	/* Append that exceeds remaining capacity */
	uint8_t big[128];
	memset(big, 0xFF, sizeof(big));
	ret = venc_frame_ring_append(r, big, sizeof(big));
	CHECK("fr_ovs_append_fail", ret == -1);
	CHECK("fr_ovs_oversize_stat", r->stats.oversize_drops == 1);

	venc_frame_ring_abort_write(r);

	venc_frame_ring_destroy(r);
	return failures;
}

/* ── Bulk write ────────────────────────────────────────────────────── */

static int test_fr_bulk_write(void)
{
	int failures = 0;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_bulk", 4,
		1024);
	CHECK("fr_bulk_create", r != NULL);

	VencFrameMeta meta;
	memset(&meta, 0, sizeof(meta));
	meta.pts = 777;
	meta.codec = VENC_FRAME_CODEC_H265;
	meta.flags = VENC_FRAME_FLAG_IDR;

	uint8_t payload[100];
	memset(payload, 0x55, sizeof(payload));

	int ret = venc_frame_ring_write(r, &meta, payload, sizeof(payload));
	CHECK("fr_bulk_write", ret == 0);

	uint8_t buf[1024];
	uint32_t out_len = 0;
	ret = venc_frame_ring_read(r, buf, sizeof(buf), &out_len);
	CHECK("fr_bulk_read", ret == 0);
	CHECK("fr_bulk_len", out_len == VENC_FRAME_META_SIZE + 100);

	VencFrameMeta *rm = (VencFrameMeta *)buf;
	CHECK("fr_bulk_pts", rm->pts == 777);
	CHECK("fr_bulk_flags", rm->flags == VENC_FRAME_FLAG_IDR);
	CHECK("fr_bulk_payload", buf[VENC_FRAME_META_SIZE] == 0x55);

	/* Empty write (meta only, no payload) */
	meta.pts = 888;
	meta.flags = 0;
	ret = venc_frame_ring_write(r, &meta, NULL, 0);
	CHECK("fr_bulk_empty_write", ret == 0);

	ret = venc_frame_ring_read(r, buf, sizeof(buf), &out_len);
	CHECK("fr_bulk_empty_read", ret == 0);
	CHECK("fr_bulk_empty_len", out_len == VENC_FRAME_META_SIZE);
	rm = (VencFrameMeta *)buf;
	CHECK("fr_bulk_empty_pts", rm->pts == 888);

	venc_frame_ring_destroy(r);
	return failures;
}

/* ── Fill and drain ────────────────────────────────────────────────── */

static int test_fr_fill_drain(void)
{
	int failures = 0;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_fd", 4, 256);
	CHECK("fr_fd_create", r != NULL);

	VencFrameMeta meta;
	memset(&meta, 0, sizeof(meta));
	meta.codec = VENC_FRAME_CODEC_H265;

	uint8_t data[32];

	/* Fill all 4 slots */
	for (int i = 0; i < 4; i++) {
		meta.pts = (uint32_t)i;
		memset(data, (uint8_t)i, sizeof(data));
		int ret = venc_frame_ring_write(r, &meta, data, sizeof(data));
		CHECK("fr_fd_fill", ret == 0);
	}

	/* 5th write should fail (ring full) */
	meta.pts = 99;
	int ret = venc_frame_ring_write(r, &meta, data, sizeof(data));
	CHECK("fr_fd_full", ret == -1);

	/* Drain all 4 and verify ordering */
	uint8_t buf[256];
	uint32_t out_len;
	for (int i = 0; i < 4; i++) {
		ret = venc_frame_ring_read(r, buf, sizeof(buf), &out_len);
		CHECK("fr_fd_drain_ok", ret == 0);
		CHECK("fr_fd_drain_len",
		      out_len == VENC_FRAME_META_SIZE + 32);
		VencFrameMeta *rm = (VencFrameMeta *)buf;
		CHECK("fr_fd_drain_pts", rm->pts == (uint32_t)i);
		CHECK("fr_fd_drain_data",
		      buf[VENC_FRAME_META_SIZE] == (uint8_t)i);
	}

	/* Should be empty */
	ret = venc_frame_ring_read(r, buf, sizeof(buf), &out_len);
	CHECK("fr_fd_empty", ret == -1);

	venc_frame_ring_destroy(r);
	return failures;
}

/* ── Wraparound ────────────────────────────────────────────────────── */

static int test_fr_wraparound(void)
{
	int failures = 0;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_wrap", 4,
		256);
	CHECK("fr_wrap_create", r != NULL);

	VencFrameMeta meta;
	memset(&meta, 0, sizeof(meta));
	meta.codec = VENC_FRAME_CODEC_H265;

	uint8_t data[8];
	uint8_t buf[256];
	uint32_t out_len;

	/* Write and read 10 times (wraps 4-slot ring) */
	for (int i = 0; i < 10; i++) {
		meta.pts = (uint32_t)i;
		memset(data, (uint8_t)i, sizeof(data));
		int ret = venc_frame_ring_write(r, &meta, data, sizeof(data));
		CHECK("fr_wrap_write", ret == 0);

		ret = venc_frame_ring_read(r, buf, sizeof(buf), &out_len);
		CHECK("fr_wrap_read", ret == 0);

		VencFrameMeta *rm = (VencFrameMeta *)buf;
		CHECK("fr_wrap_pts", rm->pts == (uint32_t)i);
		CHECK("fr_wrap_data", buf[VENC_FRAME_META_SIZE] == (uint8_t)i);
	}

	CHECK("fr_wrap_write_idx", r->hdr->write_idx == 10);
	CHECK("fr_wrap_read_idx", r->hdr->read_idx == 10);

	venc_frame_ring_destroy(r);
	return failures;
}

/* ── Concurrent producer/consumer ──────────────────────────────────── */

#define FR_CONCURRENT_COUNT 5000

typedef struct {
	venc_frame_ring_t *ring;
	int errors;
} FrThreadArg;

static void *fr_producer_thread(void *arg)
{
	FrThreadArg *ta = (FrThreadArg *)arg;
	VencFrameMeta meta;
	uint8_t data[4];

	memset(&meta, 0, sizeof(meta));
	meta.codec = VENC_FRAME_CODEC_H265;

	for (int i = 0; i < FR_CONCURRENT_COUNT; i++) {
		meta.pts = (uint32_t)i;
		data[0] = (uint8_t)(i & 0xFF);
		data[1] = (uint8_t)((i >> 8) & 0xFF);
		data[2] = 0;
		data[3] = 0;
		while (venc_frame_ring_write(ta->ring, &meta, data, 4) != 0)
			usleep(1);
	}
	return NULL;
}

static void *fr_consumer_thread(void *arg)
{
	FrThreadArg *ta = (FrThreadArg *)arg;
	uint8_t buf[256];
	uint32_t out_len;
	int expected = 0;

	for (int i = 0; i < FR_CONCURRENT_COUNT; i++) {
		while (venc_frame_ring_read(ta->ring, buf, sizeof(buf),
		       &out_len) != 0)
			usleep(1);
		VencFrameMeta *rm = (VencFrameMeta *)buf;
		if (rm->pts != (uint32_t)expected)
			ta->errors++;
		int seq = buf[VENC_FRAME_META_SIZE] |
			  (buf[VENC_FRAME_META_SIZE + 1] << 8);
		if (seq != expected)
			ta->errors++;
		expected++;
	}
	return NULL;
}

static int test_fr_concurrent(void)
{
	int failures = 0;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_conc", 64,
		256);
	CHECK("fr_conc_create", r != NULL);

	FrThreadArg prod_arg = {.ring = r, .errors = 0};
	FrThreadArg cons_arg = {.ring = r, .errors = 0};

	pthread_t prod, cons;
	pthread_create(&cons, NULL, fr_consumer_thread, &cons_arg);
	pthread_create(&prod, NULL, fr_producer_thread, &prod_arg);

	pthread_join(prod, NULL);
	pthread_join(cons, NULL);

	CHECK("fr_conc_no_errors", cons_arg.errors == 0);
	CHECK("fr_conc_all_consumed",
	      r->hdr->read_idx == FR_CONCURRENT_COUNT);

	venc_frame_ring_destroy(r);
	return failures;
}

/* ── Stride alignment ──────────────────────────────────────────────── */

static int test_fr_stride_alignment(void)
{
	int failures = 0;

	/* slot_data_size=100 → raw = 4+100=104, aligned to 104 (already 8-aligned) */
	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_al1", 4, 100);
	CHECK("fr_al1_create", r != NULL);
	CHECK("fr_al1_stride", r->slot_stride == 104);
	venc_frame_ring_destroy(r);

	/* slot_data_size=13 → raw = 4+13=17, aligned to 24 */
	r = venc_frame_ring_create("test_fr_al2", 4, 13);
	CHECK("fr_al2_create", r != NULL);
	CHECK("fr_al2_stride", r->slot_stride == 24);
	venc_frame_ring_destroy(r);

	/* slot_data_size=4096 → raw = 4+4096=4100, aligned to 4104 */
	r = venc_frame_ring_create("test_fr_al3", 4, 4096);
	CHECK("fr_al3_create", r != NULL);
	CHECK("fr_al3_stride", r->slot_stride == 4104);
	venc_frame_ring_destroy(r);

	return failures;
}

/* ── Corrupt header ────────────────────────────────────────────────── */

static int test_fr_corrupt_header(void)
{
	int failures = 0;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_ch", 4, 4096);
	CHECK("fr_ch_create", r != NULL);

	uint32_t saved_magic = r->hdr->magic;
	r->hdr->magic = 0xDEADBEEF;
	CHECK("fr_corrupt_magic",
	      venc_frame_ring_attach("test_fr_ch") == NULL);
	r->hdr->magic = saved_magic;

	uint32_t saved_ver = r->hdr->version;
	r->hdr->version = 99;
	CHECK("fr_corrupt_version",
	      venc_frame_ring_attach("test_fr_ch") == NULL);
	r->hdr->version = saved_ver;

	uint32_t saved_total = r->hdr->total_size;
	r->hdr->total_size = saved_total + 4096;
	CHECK("fr_corrupt_total",
	      venc_frame_ring_attach("test_fr_ch") == NULL);
	r->hdr->total_size = saved_total;

	uint32_t saved_sc = r->hdr->slot_count;
	r->hdr->slot_count = 5;
	CHECK("fr_corrupt_slotcount",
	      venc_frame_ring_attach("test_fr_ch") == NULL);
	r->hdr->slot_count = saved_sc;

	/* Normal attach still works after restoring */
	venc_frame_ring_t *c = venc_frame_ring_attach("test_fr_ch");
	CHECK("fr_restore_attach", c != NULL);
	if (c) venc_frame_ring_destroy(c);

	venc_frame_ring_destroy(r);
	return failures;
}

/* ── Corrupt slot length ───────────────────────────────────────────── */

static int test_fr_corrupt_slot_length(void)
{
	int failures = 0;

	venc_frame_ring_t *producer = venc_frame_ring_create(
		"test_fr_csl", 4, 1024);
	CHECK("fr_csl_create", producer != NULL);

	VencFrameMeta meta;
	memset(&meta, 0, sizeof(meta));
	meta.codec = VENC_FRAME_CODEC_H265;

	uint8_t data[16];
	memset(data, 0xAA, sizeof(data));
	int ret = venc_frame_ring_write(producer, &meta, data, sizeof(data));
	CHECK("fr_csl_write", ret == 0);

	/* Corrupt the slot length beyond slot_data_size */
	venc_frame_ring_slot_t *slot = venc_frame_ring_slot_at(producer, 0);
	slot->length = 999999;

	venc_frame_ring_t *consumer = venc_frame_ring_attach("test_fr_csl");
	CHECK("fr_csl_attach", consumer != NULL);

	uint8_t buf[1024];
	uint32_t out_len = 0;
	ret = venc_frame_ring_read(consumer, buf, sizeof(buf), &out_len);
	CHECK("fr_csl_read_fail", ret == -1);
	CHECK("fr_csl_bad_drops", consumer->stats.bad_slot_drops == 1);

	CHECK("fr_csl_idx_advanced",
	      __atomic_load_n(&consumer->hdr->read_idx,
	                      __ATOMIC_RELAXED) == 1);

	if (consumer) venc_frame_ring_destroy(consumer);
	venc_frame_ring_destroy(producer);
	return failures;
}

/* ── Overflow on create ────────────────────────────────────────────── */

static int test_fr_overflow_create(void)
{
	int failures = 0;

	/* Total would overflow uint32_t */
	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_ov",
		(uint32_t)1 << 16, (uint32_t)1 << 20);
	CHECK("fr_overflow_create_null", r == NULL);

	return failures;
}

/* ── init_complete ─────────────────────────────────────────────────── */

static int test_fr_init_complete(void)
{
	int failures = 0;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_ic", 4, 4096);
	CHECK("fr_ic_create", r != NULL);

	__atomic_store_n(&r->hdr->init_complete, 0, __ATOMIC_RELEASE);
	CHECK("fr_ic_cleared",
	      venc_frame_ring_attach("test_fr_ic") == NULL);

	__atomic_store_n(&r->hdr->init_complete, 1, __ATOMIC_RELEASE);
	venc_frame_ring_t *c = venc_frame_ring_attach("test_fr_ic");
	CHECK("fr_ic_restored", c != NULL);
	if (c) venc_frame_ring_destroy(c);

	venc_frame_ring_destroy(r);
	return failures;
}

/* ── Stats counters ────────────────────────────────────────────────── */

static int test_fr_stats(void)
{
	int failures = 0;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_st", 2, 64);
	CHECK("fr_st_create", r != NULL);

	VencFrameMeta meta;
	memset(&meta, 0, sizeof(meta));
	meta.codec = VENC_FRAME_CODEC_H265;

	/* Oversize bulk write → oversize_drops */
	uint8_t big[128];
	memset(big, 0xFF, sizeof(big));
	venc_frame_ring_write(r, &meta, big, sizeof(big));
	CHECK("fr_st_oversize", r->stats.oversize_drops == 1);

	/* Normal writes (fill 2 slots) */
	uint8_t data[8];
	memset(data, 0x42, sizeof(data));
	venc_frame_ring_write(r, &meta, data, sizeof(data));
	venc_frame_ring_write(r, &meta, data, sizeof(data));
	CHECK("fr_st_writes", r->stats.writes == 2);

	/* Full drop */
	venc_frame_ring_write(r, &meta, data, sizeof(data));
	CHECK("fr_st_full_drop", r->stats.full_drops == 1);

	/* Read 2 frames */
	uint8_t buf[256];
	uint32_t out_len;
	venc_frame_ring_read(r, buf, sizeof(buf), &out_len);
	venc_frame_ring_read(r, buf, sizeof(buf), &out_len);
	CHECK("fr_st_reads", r->stats.reads == 2);

	venc_frame_ring_destroy(r);
	return failures;
}

/* ── Destroy clears init_complete ──────────────────────────────────── */

static int test_fr_destroy_clears_init(void)
{
	int failures = 0;

	venc_frame_ring_t *producer = venc_frame_ring_create(
		"test_fr_dic", 4, 4096);
	CHECK("fr_dic_create", producer != NULL);

	venc_frame_ring_t *consumer = venc_frame_ring_attach("test_fr_dic");
	CHECK("fr_dic_attach", consumer != NULL);
	CHECK("fr_dic_consumer_sees_1",
	      __atomic_load_n(&consumer->hdr->init_complete,
	                      __ATOMIC_ACQUIRE) == 1);

	volatile uint32_t *consumer_ic = &consumer->hdr->init_complete;

	venc_frame_ring_destroy(producer);

	uint32_t ic = __atomic_load_n(consumer_ic, __ATOMIC_ACQUIRE);
	CHECK("fr_dic_consumer_sees_0", ic == 0);

	CHECK("fr_dic_reattach_fails",
	      venc_frame_ring_attach("test_fr_dic") == NULL);

	venc_frame_ring_destroy(consumer);
	return failures;
}

/* ── Fill observation ──────────────────────────────────────────────── */

static int test_fr_get_fill(void)
{
	int failures = 0;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_fill", 4,
		256);
	CHECK("fr_fill_create", r != NULL);

	venc_frame_ring_fill_t fill;
	int ret = venc_frame_ring_get_fill(r, &fill);
	CHECK("fr_fill_empty", ret == 0);
	CHECK("fr_fill_empty_used", fill.used_slots == 0);
	CHECK("fr_fill_empty_pct", fill.fill_pct == 0);

	VencFrameMeta meta;
	memset(&meta, 0, sizeof(meta));
	meta.codec = VENC_FRAME_CODEC_H265;
	uint8_t data[8];
	memset(data, 0, sizeof(data));

	venc_frame_ring_write(r, &meta, data, sizeof(data));
	venc_frame_ring_write(r, &meta, data, sizeof(data));

	ret = venc_frame_ring_get_fill(r, &fill);
	CHECK("fr_fill_half", ret == 0);
	CHECK("fr_fill_half_used", fill.used_slots == 2);
	CHECK("fr_fill_half_pct", fill.fill_pct == 50);
	CHECK("fr_fill_half_writes", fill.writes == 2);

	venc_frame_ring_destroy(r);
	return failures;
}

/* ── Read rejects oversize frame ───────────────────────────────────── */

static int test_fr_read_oversize(void)
{
	int failures = 0;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_ro", 4, 256);
	CHECK("fr_ro_create", r != NULL);

	VencFrameMeta meta;
	memset(&meta, 0, sizeof(meta));
	meta.codec = VENC_FRAME_CODEC_H265;
	uint8_t data[64];
	memset(data, 0xBB, sizeof(data));

	int ret = venc_frame_ring_write(r, &meta, data, sizeof(data));
	CHECK("fr_ro_write", ret == 0);

	/* Read with a buffer too small for the frame */
	uint8_t small_buf[16];
	uint32_t out_len = 0;
	ret = venc_frame_ring_read(r, small_buf, sizeof(small_buf), &out_len);
	CHECK("fr_ro_read_fail", ret == -1);
	CHECK("fr_ro_oversize_stat", r->stats.oversize_drops == 1);

	/* Slot was consumed (read_idx advanced) */
	CHECK("fr_ro_idx_advanced",
	      __atomic_load_n(&r->hdr->read_idx, __ATOMIC_RELAXED) == 1);

	/* Ring should be empty now */
	uint8_t buf[256];
	ret = venc_frame_ring_read(r, buf, sizeof(buf), &out_len);
	CHECK("fr_ro_empty", ret == -1);

	venc_frame_ring_destroy(r);
	return failures;
}

/* ── NULL guards on read/read_wait ────────────────────────────────── */

static int test_fr_read_null_guard(void)
{
	int failures = 0;

	CHECK("fr_rng_read_null", venc_frame_ring_read(NULL, NULL, 0, NULL) == -1);
	CHECK("fr_rng_wait_null",
	      venc_frame_ring_read_wait(NULL, NULL, 0, NULL, 100) == -1);

	venc_frame_ring_t dummy;
	memset(&dummy, 0, sizeof(dummy));
	dummy.hdr = NULL;
	CHECK("fr_rng_read_null_hdr",
	      venc_frame_ring_read(&dummy, NULL, 0, NULL) == -1);
	CHECK("fr_rng_wait_null_hdr",
	      venc_frame_ring_read_wait(&dummy, NULL, 0, NULL, 100) == -1);

	return failures;
}

/* ── read_wait wakeup path ─────────────────────────────────────────── */

static void *fr_delayed_producer_thread(void *arg)
{
	venc_frame_ring_t *r = (venc_frame_ring_t *)arg;
	VencFrameMeta meta;
	uint8_t data[4] = {0, 0, 0, 1};

	usleep(5000);
	memset(&meta, 0, sizeof(meta));
	meta.pts = 1234;
	meta.codec = VENC_FRAME_CODEC_H265;
	meta.flags = VENC_FRAME_FLAG_IDR;
	(void)venc_frame_ring_write(r, &meta, data, sizeof(data));
	return NULL;
}

static int test_fr_read_wait_wakeup(void)
{
	int failures = 0;
	uint8_t buf[256];
	uint32_t out_len = 0;
	pthread_t prod;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_wait", 4,
		256);
	CHECK("fr_wait_create", r != NULL);
	if (!r)
		return failures;

	pthread_create(&prod, NULL, fr_delayed_producer_thread, r);
	int ret = venc_frame_ring_read_wait(r, buf, sizeof(buf), &out_len,
		1000);
	pthread_join(prod, NULL);

	CHECK("fr_wait_read", ret == 0);
	CHECK("fr_wait_len", out_len == VENC_FRAME_META_SIZE + 4);
	VencFrameMeta *rm = (VencFrameMeta *)buf;
	CHECK("fr_wait_pts", rm->pts == 1234);
	CHECK("fr_wait_consumer_clear",
	      __atomic_load_n(&r->hdr->consumer_waiting,
		      __ATOMIC_RELAXED) == 0);

	venc_frame_ring_destroy(r);
	return failures;
}

/* ── slot_data_size upper bound ───────────────────────────────────── */

static int test_fr_slot_data_size_limit(void)
{
	int failures = 0;

	CHECK("fr_sdsl_max",
	      venc_frame_ring_create("test_fr_sdsl", 4, UINT32_MAX) == NULL);
	CHECK("fr_sdsl_near_max",
	      venc_frame_ring_create("test_fr_sdsl2", 4, UINT32_MAX - 4) == NULL);

	return failures;
}

/* ── Double begin_write rejected ───────────────────────────────────── */

static int test_fr_double_begin(void)
{
	int failures = 0;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_dbl", 4,
		256);
	CHECK("fr_dbl_create", r != NULL);

	VencFrameMeta meta;
	memset(&meta, 0, sizeof(meta));
	meta.codec = VENC_FRAME_CODEC_H265;

	int ret = venc_frame_ring_begin_write(r, &meta);
	CHECK("fr_dbl_begin1", ret == 0);

	ret = venc_frame_ring_begin_write(r, &meta);
	CHECK("fr_dbl_begin2_rejected", ret == -1);

	venc_frame_ring_abort_write(r);
	venc_frame_ring_destroy(r);
	return failures;
}


/* ── Producer health fields (0.57.0) ───────────────────────────────── */

/* These three live at fixed byte offsets that radeon-vrx and waybeam-link
 * address directly, not through this struct.  A reorder would move them out
 * from under both consumers with nothing failing to compile, so assert the
 * offsets from the test as well as from the header's static asserts, and
 * read them back through a raw byte pointer the way a real consumer does. */
static int test_fr_producer_health(void)
{
	int failures = 0;
	VencFrameMeta meta = {0};
	const unsigned char *raw;
	uint32_t hm;
	uint64_t fd_drops;
	uint16_t thr;
	int i;

	venc_frame_ring_t *r = venc_frame_ring_create("test_fr_health", 2, 64);
	CHECK("fr_health_create", r != NULL);
	if (!r)
		return failures;

	raw = (const unsigned char *)r->hdr;
	memcpy(&hm, raw + 76, sizeof(hm));
	memcpy(&fd_drops, raw + 80, sizeof(fd_drops));
	memcpy(&thr, raw + 88, sizeof(thr));
	CHECK("fr_health_magic_at_76", hm == VENC_FRAME_RING_HEALTH_MAGIC);
	CHECK("fr_health_drops_at_80_zero", fd_drops == 0);
	CHECK("fr_health_low_water_at_88_seed", thr == 0);

	/* Marker must be published before init_complete, so a consumer that
	 * has attached at all always sees a coherent group. */
	CHECK("fr_health_init_complete", r->hdr->init_complete == 1);

	/* Fill the ring, then overflow it: the header counter must track the
	 * process-local one the consumer cannot see. */
	meta.codec = VENC_FRAME_CODEC_H265;
	for (i = 0; i < 2; ++i)
		CHECK("fr_health_write_ok",
			venc_frame_ring_write(r, &meta, "xy", 2) == 0);
	CHECK("fr_health_overflow_rejected",
		venc_frame_ring_write(r, &meta, "xy", 2) == -1);

	memcpy(&fd_drops, raw + 80, sizeof(fd_drops));
	CHECK("fr_health_drops_published", fd_drops == 1);
	CHECK("fr_health_drops_match_local",
		fd_drops == r->stats.full_drops);

	CHECK("fr_health_overflow_rejected2",
		venc_frame_ring_write(r, &meta, "xy", 2) == -1);
	memcpy(&fd_drops, raw + 80, sizeof(fd_drops));
	CHECK("fr_health_drops_accumulate", fd_drops == 2);

	/* Low-water publication at the pinned offset 88, in slots. */
	venc_frame_ring_set_low_water(r, 6);
	memcpy(&thr, raw + 88, sizeof(thr));
	CHECK("fr_health_low_water_published", thr == 6);
	venc_frame_ring_set_low_water(r, 1);
	memcpy(&thr, raw + 88, sizeof(thr));
	CHECK("fr_health_low_water_healthy", thr == 1);

	/* Nothing before the new fields moved, and the header is still the
	 * size every consumer maps. */
	CHECK("fr_health_hdr_still_192",
		sizeof(venc_frame_ring_hdr_t) == 192);
	/* v2: offset 88 changed meaning (throttle clamp -> ring low-water) and
	 * the polarity inverted with it, so consumers MUST see a version they
	 * do not recognise rather than silently misread the field. */
	CHECK("fr_health_version_is_2",
		r->hdr->version == VENC_FRAME_RING_VERSION &&
		r->hdr->version == 2);

	venc_frame_ring_destroy(r);
	return failures;
}

/* An attached (consumer) ring must never write the producer's fields. */
/* venc measures ring pressure and publishes it; it never acts on it.  The
 * low-water tracker is that measurement: the lowest occupancy reached in each
 * 200 ms window, in SLOTS.
 *
 * Two things the encoding has to get right, both learned from the clamp this
 * replaced.  Low-water, not high-water: a healthy consumer reading one frame
 * per event-loop iteration routinely spikes the ring 2-3 slots inside a window
 * and drains again, so high-water calls that congestion at random.  And raw
 * slots, not permille: the caller samples just AFTER writing, so a perfectly
 * drained ring still reads 1 slot -- the healthy band is <= 1, not 0 -- and at
 * the ring's 16-slot default one slot is 62.5 permille, which truncates to 62
 * and converts back to 0.  A fraction cannot carry the one reading that
 * matters. */
static int test_fr_low_water(void)
{
	int failures = 0;
	VencRingLowWater t;

	/* A window that has not elapsed publishes nothing, so the header keeps
	 * the previous window's verdict rather than flapping mid-window. */
	venc_ring_low_water_reset(&t, 1000000);
	venc_ring_low_water_observe(&t, 8, 8);
	CHECK("lw_window_open_no_publish",
		venc_ring_low_water_tick(&t, 1000000 +
			VENC_RING_LOW_WATER_WINDOW_US - 1) == 0);
	CHECK("lw_window_open_keeps_seed", venc_ring_low_water_slots(&t) == 0);

	/* A ring that never dropped below full across the whole window is real
	 * standing backlog: 8/8 slots. */
	CHECK("lw_full_window_publishes",
		venc_ring_low_water_tick(&t, 1000000 +
			VENC_RING_LOW_WATER_WINDOW_US) == 1);
	CHECK("lw_full_window_is_capacity",
		venc_ring_low_water_slots(&t) == 8);

	/* The healthy steady state: the producer samples just after writing,
	 * so a consumer that is keeping up leaves exactly one frame queued.
	 * This MUST be distinguishable from a fully drained ring -- it is the
	 * reading a permille encoding destroys at 16 slots. */
	venc_ring_low_water_observe(&t, 3, 8);
	venc_ring_low_water_observe(&t, 1, 8);
	venc_ring_low_water_observe(&t, 2, 8);
	CHECK("lw_one_slot_is_1",
		venc_ring_low_water_tick(&t, 1000000 +
			2 * VENC_RING_LOW_WATER_WINDOW_US) == 1 &&
		venc_ring_low_water_slots(&t) == 1);

	/* ...and a genuinely empty ring is 0, distinct from the above. */
	venc_ring_low_water_observe(&t, 4, 8);
	venc_ring_low_water_observe(&t, 0, 8);
	CHECK("lw_drained_is_0",
		venc_ring_low_water_tick(&t, 1000000 +
			3 * VENC_RING_LOW_WATER_WINDOW_US) == 1 &&
		venc_ring_low_water_slots(&t) == 0);

	/* One touch of bottom anywhere in the window clears it, however deep
	 * the spikes around it were -- that is the whole point of low-water. */
	venc_ring_low_water_observe(&t, 7, 8);
	venc_ring_low_water_observe(&t, 1, 8);
	venc_ring_low_water_observe(&t, 8, 8);
	CHECK("lw_spikes_around_bottom_ignored",
		venc_ring_low_water_tick(&t, 1000000 +
			4 * VENC_RING_LOW_WATER_WINDOW_US) == 1 &&
		venc_ring_low_water_slots(&t) == 1);

	/* Each window starts clean: last window's minimum must not leak into
	 * the next one, or a single good window would mask a standing backlog
	 * forever. */
	venc_ring_low_water_observe(&t, 8, 8);
	CHECK("lw_window_resets_between",
		venc_ring_low_water_tick(&t, 1000000 +
			5 * VENC_RING_LOW_WATER_WINDOW_US) == 1 &&
		venc_ring_low_water_slots(&t) == 8);

	/* A 16-slot ring reports 1 slot as 1 -- the regression this encoding
	 * exists to prevent (62.5 permille truncates to 62, which converts
	 * back to 0 and reads as a drained ring). */
	venc_ring_low_water_reset(&t, 0);
	venc_ring_low_water_observe(&t, 1, 16);
	CHECK("lw_one_slot_of_16_survives",
		venc_ring_low_water_tick(&t, VENC_RING_LOW_WATER_WINDOW_US) == 1 &&
		venc_ring_low_water_slots(&t) == 1);

	/* Degenerate geometry must not publish out-of-range occupancy. */
	venc_ring_low_water_reset(&t, 0);
	venc_ring_low_water_observe(&t, 0, 0);
	CHECK("lw_zero_slot_count_safe",
		venc_ring_low_water_tick(&t, VENC_RING_LOW_WATER_WINDOW_US) == 1 &&
		venc_ring_low_water_slots(&t) == 0);

	/* Occupancy above capacity (a torn read across a resize) clamps at the
	 * point of publication -- the value crosses a process boundary. */
	venc_ring_low_water_reset(&t, 0);
	venc_ring_low_water_observe(&t, 99, 8);
	CHECK("lw_over_capacity_clamps",
		venc_ring_low_water_tick(&t, VENC_RING_LOW_WATER_WINDOW_US) == 1 &&
		venc_ring_low_water_slots(&t) == 8);

	/* NULL is inert in every direction. */
	venc_ring_low_water_reset(NULL, 0);
	venc_ring_low_water_observe(NULL, 1, 8);
	CHECK("lw_null_tick_safe", venc_ring_low_water_tick(NULL, 0) == 0);
	CHECK("lw_null_read_safe", venc_ring_low_water_slots(NULL) == 0);

	/* End-to-end: a full ring must actually reject, which is the event the
	 * measurement hangs off.  8 slots, 9th write fails. */
	{
		venc_frame_ring_t *r = venc_frame_ring_create("test_fr_dropc",
			8, 4096);
		VencFrameMeta meta;
		int i;
		int rejected = 0;

		CHECK("fr_chain_ring_created", r != NULL);
		if (r) {
			memset(&meta, 0, sizeof(meta));
			meta.codec = VENC_FRAME_CODEC_H265;
			for (i = 0; i < 8; ++i) {
				if (venc_frame_ring_begin_write(r, &meta) != 0)
					break;
				venc_frame_ring_commit_write(r);
			}
			CHECK("fr_chain_filled_8", i == 8);
			rejected = venc_frame_ring_begin_write(r, &meta) != 0;
			CHECK("fr_chain_9th_rejected", rejected);
			CHECK("fr_chain_full_drop_counted",
				r->stats.full_drops == 1);
			venc_frame_ring_destroy(r);
		}
	}

	return failures;
}

static int test_fr_health_consumer_readonly(void)
{
	int failures = 0;
	venc_frame_ring_t *w, *rd;

	w = venc_frame_ring_create("test_fr_health_ro", 2, 64);
	CHECK("fr_health_ro_create", w != NULL);
	if (!w)
		return failures;
	rd = venc_frame_ring_attach("test_fr_health_ro");
	CHECK("fr_health_ro_attach", rd != NULL);
	if (!rd) {
		venc_frame_ring_destroy(w);
		return failures;
	}

	venc_frame_ring_set_low_water(w, 5);
	venc_frame_ring_set_low_water(rd, 2);   /* must be ignored */
	CHECK("fr_health_ro_consumer_ignored",
		w->hdr->low_water_slots == 5);
	CHECK("fr_health_ro_consumer_reads",
		rd->hdr->low_water_slots == 5);
	CHECK("fr_health_ro_consumer_sees_magic",
		rd->hdr->health_magic == VENC_FRAME_RING_HEALTH_MAGIC);

	venc_frame_ring_destroy(rd);
	venc_frame_ring_destroy(w);
	return failures;
}

/* ── Entry point ───────────────────────────────────────────────────── */

int test_venc_frame_ring(void)
{
	int failures = 0;

	failures += test_fr_create_destroy();
	failures += test_fr_create_validation();
	failures += test_fr_attach();
	failures += test_fr_staged_write();
	failures += test_fr_abort_write();
	failures += test_fr_oversize_append();
	failures += test_fr_bulk_write();
	failures += test_fr_fill_drain();
	failures += test_fr_wraparound();
	failures += test_fr_concurrent();
	failures += test_fr_stride_alignment();
	failures += test_fr_corrupt_header();
	failures += test_fr_corrupt_slot_length();
	failures += test_fr_overflow_create();
	failures += test_fr_init_complete();
	failures += test_fr_stats();
	failures += test_fr_destroy_clears_init();
	failures += test_fr_get_fill();
	failures += test_fr_read_oversize();
	failures += test_fr_read_null_guard();
	failures += test_fr_read_wait_wakeup();
	failures += test_fr_slot_data_size_limit();
	failures += test_fr_double_begin();
	failures += test_fr_producer_health();
	failures += test_fr_health_consumer_readonly();
	failures += test_fr_low_water();

	return failures;
}
