#ifndef VENC_RING_H
#define VENC_RING_H

/*
 * SPSC (Single-Producer Single-Consumer) lock-free ring buffer over
 * POSIX shared memory.  Designed for zero-syscall steady-state RTP
 * packet transfer between venc (producer) and wfb_tx (consumer).
 *
 * Memory ordering: acquire/release on index variables.
 * Consumer sleep: futex on futex_seq (dedicated 32-bit word).
 */

#include <stdint.h>
#include <string.h>

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

#define VENC_RING_MAGIC   0x56454E43  /* "VENC" */
#define VENC_RING_VERSION 2

/* ── Ring header (3 cache lines, 192 bytes) ──────────────────────────── */

typedef struct __attribute__((aligned(64))) {
	/* Line 0: Immutable config (set once at creation) */
	uint32_t magic;
	uint32_t version;
	uint32_t slot_count;      /* must be power of 2 */
	uint32_t slot_data_size;  /* max payload per slot */
	uint32_t total_size;      /* total mmap size */
	uint32_t epoch;           /* unique per producer instance (pid) */
	uint32_t init_complete;   /* set to 1 last, after all fields written */
	uint8_t  _pad0[36];

	/* Line 1: Producer-owned */
	uint64_t write_idx        __attribute__((aligned(64)));
	uint32_t futex_seq;       /* 32-bit monotonic counter for futex wake/wait */
	uint8_t  _pad1[52];

	/* Line 2: Consumer-owned */
	uint64_t read_idx          __attribute__((aligned(64)));
	uint32_t consumer_waiting; /* futex flag: 1 = sleeping */
	uint8_t  _pad2[52];
} venc_ring_hdr_t;

#ifdef __cplusplus
static_assert(sizeof(venc_ring_hdr_t) == 192,
              "venc_ring_hdr_t must be exactly 192 bytes (3 cache lines)");
static_assert(__alignof__(venc_ring_hdr_t) == 64,
              "venc_ring_hdr_t must be 64-byte aligned");
#else
_Static_assert(sizeof(venc_ring_hdr_t) == 192,
               "venc_ring_hdr_t must be exactly 192 bytes (3 cache lines)");
_Static_assert(__alignof__(venc_ring_hdr_t) == 64,
               "venc_ring_hdr_t must be 64-byte aligned");
#endif

/* Per-slot layout: 2-byte length prefix + data */
typedef struct {
	uint16_t length;
	uint8_t  data[];  /* flexible array [slot_data_size] */
} venc_ring_slot_t;

/* Observability counters (local handle, not in SHM) */
typedef struct {
	uint64_t writes;
	uint64_t reads;
	uint64_t full_drops;
	uint64_t oversize_drops;
	uint64_t bad_slot_drops;
} venc_ring_stats_t;

/* Opaque handle */
typedef struct {
	venc_ring_hdr_t *hdr;
	uint8_t         *slots_base;  /* start of slot array */
	uint32_t         slot_stride; /* sizeof(uint16_t) + slot_data_size, aligned */
	uint32_t         slot_data_size;
	uint32_t         map_size;    /* total mmap size (for munmap) */
	int              is_owner;    /* 1 = created (will unlink), 0 = attached */
	char             name[256];
	venc_ring_stats_t stats;
} venc_ring_t;

/* ── Create / attach / destroy ───────────────────────────────────────── */

/* Producer: create shared memory ring.  slot_count must be power of 2.
 * shm_name: POSIX shm name (e.g. "venc_wfb" → /dev/shm/venc_wfb).
 * Returns NULL on failure. */
venc_ring_t *venc_ring_create(const char *shm_name, uint32_t slot_count,
                               uint32_t slot_data_size);

/* Consumer: attach to existing ring (validates magic + version + init_complete).
 * Returns NULL on failure. */
venc_ring_t *venc_ring_attach(const char *shm_name);

/* Destroy handle.  If is_owner, also shm_unlink. */
void venc_ring_destroy(venc_ring_t *r);

/* ── Inline write (producer) ─────────────────────────────────────────── */

static inline venc_ring_slot_t *venc_ring_slot_at(const venc_ring_t *r,
                                                    uint32_t idx)
{
	return (venc_ring_slot_t *)(r->slots_base + (uint64_t)idx * r->slot_stride);
}

/* 3-segment write: header + payload1 + optional payload2.  p2 may be NULL
 * / p2_len == 0 to write only 2 segments.  Lets callers avoid pre-
 * flattening fragmented RTP payloads (H.265 FU) into a stack buffer
 * before the ring write. */
static inline int venc_ring_write3(venc_ring_t *r,
    const void *hdr, uint16_t hdr_len,
    const void *p1, uint16_t p1_len,
    const void *p2, uint16_t p2_len)
{
	uint32_t total = (uint32_t)hdr_len + (uint32_t)p1_len + (uint32_t)p2_len;
	if (total > r->slot_data_size) {
		r->stats.oversize_drops++;
		return -1;
	}

	uint64_t w = __atomic_load_n(&r->hdr->write_idx, __ATOMIC_RELAXED);
	uint64_t rd = __atomic_load_n(&r->hdr->read_idx, __ATOMIC_ACQUIRE);

	if (w - rd >= r->hdr->slot_count) {
		r->stats.full_drops++;
		return -1;  /* ring full — drop packet */
	}

	uint32_t idx = (uint32_t)(w & (r->hdr->slot_count - 1));
	venc_ring_slot_t *slot = venc_ring_slot_at(r, idx);
	slot->length = (uint16_t)total;
	if (hdr_len)
		memcpy(slot->data, hdr, hdr_len);
	if (p1_len)
		memcpy(slot->data + hdr_len, p1, p1_len);
	if (p2_len)
		memcpy(slot->data + hdr_len + p1_len, p2, p2_len);

	__atomic_store_n(&r->hdr->write_idx, w + 1, __ATOMIC_RELEASE);
	r->stats.writes++;

#ifdef __linux__
	__atomic_fetch_add(&r->hdr->futex_seq, 1, __ATOMIC_RELEASE);
	if (__atomic_load_n(&r->hdr->consumer_waiting, __ATOMIC_RELAXED))
		syscall(SYS_futex, &r->hdr->futex_seq,
		        FUTEX_WAKE, 1, NULL, NULL, 0);
#endif

	return 0;
}

static inline int venc_ring_write(venc_ring_t *r,
    const void *hdr, uint16_t hdr_len,
    const void *payload, uint16_t payload_len)
{
	return venc_ring_write3(r, hdr, hdr_len, payload, payload_len, NULL, 0);
}

/* ── Inline read (consumer) ──────────────────────────────────────────── */

static inline int venc_ring_read(venc_ring_t *r,
    void *buf, uint16_t buf_size, uint16_t *out_len)
{
	uint64_t rd = __atomic_load_n(&r->hdr->read_idx, __ATOMIC_RELAXED);
	uint64_t w = __atomic_load_n(&r->hdr->write_idx, __ATOMIC_ACQUIRE);

	if (rd >= w)
		return -1;  /* ring empty */

	uint32_t idx = (uint32_t)(rd & (r->hdr->slot_count - 1));
	venc_ring_slot_t *slot = venc_ring_slot_at(r, idx);

	uint16_t len = slot->length;

	/* Validate slot length against slot_data_size */
	if (len > r->slot_data_size) {
		r->stats.bad_slot_drops++;
		__atomic_store_n(&r->hdr->read_idx, rd + 1, __ATOMIC_RELEASE);
		return -1;  /* corrupt slot — skip */
	}

	if (len > buf_size)
		len = buf_size;  /* truncate if consumer buffer too small */

	memcpy(buf, slot->data, len);
	if (out_len) *out_len = len;

	__atomic_store_n(&r->hdr->read_idx, rd + 1, __ATOMIC_RELEASE);
	r->stats.reads++;
	return 0;
}

/* Blocking read with futex wait (consumer). timeout_ms <= 0 = infinite. */
static inline int venc_ring_read_wait(venc_ring_t *r,
    void *buf, uint16_t buf_size, uint16_t *out_len, int timeout_ms)
{
	for (;;) {
		if (venc_ring_read(r, buf, buf_size, out_len) == 0)
			return 0;

#ifdef __linux__
		uint32_t seq = __atomic_load_n(&r->hdr->futex_seq, __ATOMIC_ACQUIRE);
		__atomic_store_n(&r->hdr->consumer_waiting, 1, __ATOMIC_RELEASE);

		struct timespec ts;
		struct timespec *tsp = NULL;
		if (timeout_ms > 0) {
			ts.tv_sec = timeout_ms / 1000;
			ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
			tsp = &ts;
		}

		syscall(SYS_futex, &r->hdr->futex_seq,
		        FUTEX_WAIT, seq, tsp, NULL, 0);

		__atomic_store_n(&r->hdr->consumer_waiting, 0, __ATOMIC_RELEASE);
#else
		usleep(1000);
#endif
		/* Re-check after wake */
		if (venc_ring_read(r, buf, buf_size, out_len) == 0)
			return 0;

		if (timeout_ms > 0)
			return -1;  /* timeout */
	}
}

#endif /* VENC_RING_H */
