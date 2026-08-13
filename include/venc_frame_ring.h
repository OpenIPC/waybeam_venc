#ifndef VENC_FRAME_RING_H
#define VENC_FRAME_RING_H

/*
 * SPSC lock-free ring buffer for whole encoded video frames over POSIX
 * shared memory.  Designed for frame-aligned transfer between venc
 * (producer) and waybeam-link (consumer) so the link layer can apply
 * per-frame FEC without re-fragmenting pre-built RTP packets.
 *
 * Each slot carries an 8-byte metadata header (VencFrameMeta) followed
 * by the Annex B frame data (NALs with start codes preserved).
 *
 * Differs from venc_ring (RTP-packet ring):
 *   - uint32_t slot lengths (frames can exceed 64 KB)
 *   - larger default slot_data_size (512 KB vs ~4 KB)
 *   - staged write API (begin/append/commit) to gather scattered NAL
 *     data from the encoder without a separate staging buffer
 *   - separate magic/version to prevent cross-type attach
 *
 * Memory ordering and futex wake follow the same pattern as venc_ring.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

#define VENC_FRAME_RING_MAGIC   0x5646524D  /* "VFRM" */
#define VENC_FRAME_RING_VERSION 1
/* "VHLT" -- marks the producer-health fields on header line 1 as populated.
 * A consumer MUST treat any other value (notably 0, from a pre-0.57
 * producer) as "this producer does not report health" rather than as zero
 * drops, and MUST NOT reject a ring because these bytes are non-zero. */
#define VENC_FRAME_RING_HEALTH_MAGIC 0x56484C54u

/* Metadata prepended to each frame in the slot's data[] */
#define VENC_FRAME_META_SIZE    8
#define VENC_FRAME_CODEC_H265   0x01
#define VENC_FRAME_FLAG_IDR     0x01
#define VENC_FRAME_FLAG_GDR     0x02  /* GDR rolling intra stripe active */
#define VENC_FRAME_FLAG_ENHANCE 0x04  /* SVC-T enhance layer (droppable) */

/* Would discarding a frame with these meta flags break the decoder's
 * reference chain?
 *
 * A ring-full drop lands AFTER encode, so unless the frame was
 * non-referenced the decoder renders garbage until the next IDR — with a
 * long GOP, seconds.  An SVC-T enhance frame is droppable by construction
 * (nothing predicts from it), so discarding one costs exactly one frame and
 * must NOT provoke a recovery IDR: that would spend the largest frame in the
 * stream to repair damage that never happened, into a ring already full.
 *
 * Shared by both backends so the policy has one definition rather than two
 * copies that can drift. */
static inline int venc_frame_drop_breaks_chain(uint8_t flags)
{
	return (flags & VENC_FRAME_FLAG_ENHANCE) ? 0 : 1;
}

typedef struct {
	uint32_t pts;        /* capture timestamp (µs, truncated to 32 bits) */
	uint8_t  codec;      /* VENC_FRAME_CODEC_H265 */
	uint8_t  flags;      /* VENC_FRAME_FLAG_{IDR,GDR,ENHANCE} */
	uint8_t  gdr_pos;    /* 0-based position in GDR cycle (0 when inactive) */
	uint8_t  gdr_len;    /* GDR cycle length in frames (0 when inactive) */
} VencFrameMeta;

/* ── Ring header (3 cache lines, 192 bytes) ──────────────────────────── */

typedef struct __attribute__((aligned(64))) {
	/* Line 0: Immutable config */
	uint32_t magic;
	uint32_t version;
	uint32_t slot_count;      /* must be power of 2 */
	uint32_t slot_data_size;  /* max payload per slot (meta + frame data) */
	uint32_t total_size;      /* total mmap size */
	uint32_t epoch;
	uint32_t init_complete;
	uint8_t  _pad0[36];

	/* Line 1: Producer-owned.
	 *
	 * health_magic/full_drops/throttle_permille were carved out of the
	 * old _pad1[52] in 0.57.0.  sizeof stays 192, `version` stays 1, and
	 * nothing before them moves -- both external consumers address this
	 * header by byte offset (radeon-vrx VFRM_OFF_*, waybeam-link
	 * kFrHdr*), so the change is invisible to them.  See
	 * protocols/frame-shm.md, which is the canonical spec.
	 *
	 * Why they exist: full_drops is otherwise process-local to the
	 * producer (venc_frame_ring_t::stats below), so a consumer is
	 * structurally blind to the drops it is causing.  health_magic is
	 * what stops a new consumer reading an OLD producer's zeroed pad as
	 * "no drops" -- it is set last, after everything else in the header
	 * and before init_complete. */
	uint64_t write_idx        __attribute__((aligned(64)));
	uint32_t futex_seq;
	uint32_t health_magic;      /* VENC_FRAME_RING_HEALTH_MAGIC, or 0 */
	uint64_t full_drops;        /* lifetime full-ring drops */
	uint16_t throttle_permille; /* 1000 = unclamped, 250 = floor */
	uint8_t  _pad1[38];

	/* Line 2: Consumer-owned */
	uint64_t read_idx          __attribute__((aligned(64)));
	uint32_t consumer_waiting;
	uint8_t  _pad2[52];
} venc_frame_ring_hdr_t;

#ifdef __cplusplus
static_assert(sizeof(venc_frame_ring_hdr_t) == 192,
              "venc_frame_ring_hdr_t must be exactly 192 bytes");
static_assert(sizeof(VencFrameMeta) == VENC_FRAME_META_SIZE,
              "VencFrameMeta must be exactly 8 bytes");
#else
_Static_assert(sizeof(venc_frame_ring_hdr_t) == 192,
               "venc_frame_ring_hdr_t must be exactly 192 bytes");
_Static_assert(sizeof(VencFrameMeta) == VENC_FRAME_META_SIZE,
               "VencFrameMeta must be exactly 8 bytes");
/* Consumers address these by byte offset, not through this struct, so a
 * reorder here would silently move them out from under radeon-vrx and
 * waybeam-link with nothing failing to compile.  Pin them. */
_Static_assert(offsetof(venc_frame_ring_hdr_t, write_idx) == 64, "off 64");
_Static_assert(offsetof(venc_frame_ring_hdr_t, futex_seq) == 72, "off 72");
_Static_assert(offsetof(venc_frame_ring_hdr_t, health_magic) == 76, "off 76");
_Static_assert(offsetof(venc_frame_ring_hdr_t, full_drops) == 80, "off 80");
_Static_assert(offsetof(venc_frame_ring_hdr_t, throttle_permille) == 88,
               "off 88");
_Static_assert(offsetof(venc_frame_ring_hdr_t, read_idx) == 128, "off 128");
_Static_assert(offsetof(venc_frame_ring_hdr_t, consumer_waiting) == 136,
               "off 136");
#endif

/* Per-slot layout: 4-byte length prefix + data */
typedef struct {
	uint32_t length;
	uint8_t  data[];  /* [slot_data_size] */
} venc_frame_ring_slot_t;

typedef struct {
	uint64_t writes;
	uint64_t reads;
	uint64_t full_drops;
	uint64_t oversize_drops;
	uint64_t bad_slot_drops;
} venc_frame_ring_stats_t;

typedef struct {
	venc_frame_ring_hdr_t *hdr;
	uint8_t               *slots_base;
	uint32_t               slot_stride;
	uint32_t               slot_data_size;
	uint32_t               map_size;
	int                    is_owner;
	char                   name[256];
	venc_frame_ring_stats_t stats;
	/* Staged write state (producer only) */
	uint32_t               write_offset;
	int                    write_active;
} venc_frame_ring_t;

/* ── Create / attach / destroy ───────────────────────────────────────── */

venc_frame_ring_t *venc_frame_ring_create(const char *shm_name,
	uint32_t slot_count, uint32_t slot_data_size);

venc_frame_ring_t *venc_frame_ring_attach(const char *shm_name);

void venc_frame_ring_destroy(venc_frame_ring_t *r);

/* ── Producer-side observability ─────────────────────────────────────── */

typedef struct {
	uint32_t slot_count;
	uint32_t used_slots;
	uint8_t  fill_pct;
	uint64_t writes;
	uint64_t reads;
	uint64_t full_drops;
	uint64_t oversize_drops;
	uint64_t bad_slot_drops;
} venc_frame_ring_fill_t;

static inline int venc_frame_ring_get_fill(const venc_frame_ring_t *r,
	venc_frame_ring_fill_t *out)
{
	if (!r || !r->hdr || !out)
		return -1;
	uint64_t w = __atomic_load_n(&r->hdr->write_idx, __ATOMIC_ACQUIRE);
	uint64_t rd = __atomic_load_n(&r->hdr->read_idx, __ATOMIC_RELAXED);
	uint32_t used = (w >= rd) ? (uint32_t)(w - rd) : 0;
	uint32_t slot_count = r->hdr->slot_count;
	if (used > slot_count)
		used = slot_count;
	out->slot_count = slot_count;
	out->used_slots = used;
	out->fill_pct = slot_count
		? (uint8_t)((uint64_t)used * 100u / slot_count)
		: 0u;
	out->writes = r->stats.writes;
	out->reads = r->stats.reads;
	out->full_drops = r->stats.full_drops;
	out->oversize_drops = r->stats.oversize_drops;
	out->bad_slot_drops = r->stats.bad_slot_drops;
	return 0;
}

/* Publish the producer's self-imposed bitrate clamp (permille, 1000 =
 * unclamped).  Producer-only; a no-op on an attached (consumer) ring. */
static inline void venc_frame_ring_set_throttle(venc_frame_ring_t *r,
	uint16_t permille)
{
	if (!r || !r->hdr || !r->is_owner)
		return;
	__atomic_store_n(&r->hdr->throttle_permille, permille,
		__ATOMIC_RELAXED);
}

/* ── Inline helpers ──────────────────────────────────────────────────── */

static inline venc_frame_ring_slot_t *venc_frame_ring_slot_at(
	const venc_frame_ring_t *r, uint32_t idx)
{
	return (venc_frame_ring_slot_t *)(r->slots_base +
		(uint64_t)idx * r->slot_stride);
}

/* ── Staged write API (producer) ─────────────────────────────────────
 *
 * Usage:
 *   venc_frame_ring_begin_write(r, &meta)   // reserves a slot
 *   venc_frame_ring_append(r, nal1, len1)   // append NAL data
 *   venc_frame_ring_append(r, nal2, len2)   // append more
 *   venc_frame_ring_commit_write(r)         // publish to consumer
 *
 * If any append fails (oversize), call abort_write() instead of commit.
 */

static inline int venc_frame_ring_begin_write(venc_frame_ring_t *r,
	const VencFrameMeta *meta)
{
	uint64_t w, rd;
	uint32_t idx;
	venc_frame_ring_slot_t *slot;

	if (!r || !r->hdr || !meta || r->write_active)
		return -1;

	w = __atomic_load_n(&r->hdr->write_idx, __ATOMIC_RELAXED);
	rd = __atomic_load_n(&r->hdr->read_idx, __ATOMIC_ACQUIRE);

	if (w - rd >= r->hdr->slot_count) {
		r->stats.full_drops++;
		/* Mirror into the shared header so the consumer can see the
		 * drops it is causing.  Relaxed: our own cache line, and the
		 * value is evidence for a rate controller, not an exact
		 * accounting -- a reader may lag by a frame. */
		if (r->is_owner)
			__atomic_store_n(&r->hdr->full_drops,
				r->stats.full_drops, __ATOMIC_RELAXED);
		return -1;
	}

	if (VENC_FRAME_META_SIZE > r->slot_data_size) {
		r->stats.oversize_drops++;
		return -1;
	}

	idx = (uint32_t)(w & (r->hdr->slot_count - 1));
	slot = venc_frame_ring_slot_at(r, idx);
	memcpy(slot->data, meta, VENC_FRAME_META_SIZE);

	r->write_offset = VENC_FRAME_META_SIZE;
	r->write_active = 1;
	return 0;
}

static inline int venc_frame_ring_append(venc_frame_ring_t *r,
	const void *data, uint32_t len)
{
	uint64_t w;
	uint32_t idx;
	venc_frame_ring_slot_t *slot;

	if (!r || !r->write_active || !data || len == 0)
		return -1;

	if ((uint64_t)r->write_offset + len > r->slot_data_size) {
		r->stats.oversize_drops++;
		return -1;
	}

	w = __atomic_load_n(&r->hdr->write_idx, __ATOMIC_RELAXED);
	idx = (uint32_t)(w & (r->hdr->slot_count - 1));
	slot = venc_frame_ring_slot_at(r, idx);
	memcpy(slot->data + r->write_offset, data, len);
	r->write_offset += len;
	return 0;
}

static inline void venc_frame_ring_commit_write(venc_frame_ring_t *r)
{
	uint64_t w;
	uint32_t idx;
	venc_frame_ring_slot_t *slot;

	if (!r || !r->write_active)
		return;

	w = __atomic_load_n(&r->hdr->write_idx, __ATOMIC_RELAXED);
	idx = (uint32_t)(w & (r->hdr->slot_count - 1));
	slot = venc_frame_ring_slot_at(r, idx);
	slot->length = r->write_offset;

	__atomic_store_n(&r->hdr->write_idx, w + 1, __ATOMIC_RELEASE);
	r->stats.writes++;
	r->write_active = 0;
	r->write_offset = 0;

#ifdef __linux__
	__atomic_fetch_add(&r->hdr->futex_seq, 1, __ATOMIC_RELEASE);
	if (__atomic_load_n(&r->hdr->consumer_waiting, __ATOMIC_RELAXED))
		syscall(SYS_futex, &r->hdr->futex_seq,
		        FUTEX_WAKE, 1, NULL, NULL, 0);
#endif
}

static inline void venc_frame_ring_abort_write(venc_frame_ring_t *r)
{
	if (!r)
		return;
	r->write_active = 0;
	r->write_offset = 0;
}

/* ── Bulk write (producer convenience) ───────────────────────────────── */

static inline int venc_frame_ring_write(venc_frame_ring_t *r,
	const VencFrameMeta *meta, const void *data, uint32_t len)
{
	if (venc_frame_ring_begin_write(r, meta) != 0)
		return -1;
	if (len > 0 && venc_frame_ring_append(r, data, len) != 0) {
		venc_frame_ring_abort_write(r);
		return -1;
	}
	venc_frame_ring_commit_write(r);
	return 0;
}

/* ── Read (consumer) ─────────────────────────────────────────────────── */

static inline int venc_frame_ring_read(venc_frame_ring_t *r,
	void *buf, uint32_t buf_size, uint32_t *out_len)
{
	uint64_t rd, w;
	uint32_t idx, len;
	venc_frame_ring_slot_t *slot;

	if (!r || !r->hdr)
		return -1;

	rd = __atomic_load_n(&r->hdr->read_idx, __ATOMIC_RELAXED);
	w = __atomic_load_n(&r->hdr->write_idx, __ATOMIC_ACQUIRE);

	if (rd >= w)
		return -1;

	idx = (uint32_t)(rd & (r->hdr->slot_count - 1));
	slot = venc_frame_ring_slot_at(r, idx);
	len = slot->length;

	if (len > r->slot_data_size) {
		r->stats.bad_slot_drops++;
		__atomic_store_n(&r->hdr->read_idx, rd + 1, __ATOMIC_RELEASE);
		return -1;
	}

	if (len > buf_size) {
		r->stats.oversize_drops++;
		__atomic_store_n(&r->hdr->read_idx, rd + 1, __ATOMIC_RELEASE);
		return -1;
	}

	memcpy(buf, slot->data, len);
	if (out_len) *out_len = len;

	__atomic_store_n(&r->hdr->read_idx, rd + 1, __ATOMIC_RELEASE);
	r->stats.reads++;
	return 0;
}

static inline int venc_frame_ring_read_wait(venc_frame_ring_t *r,
	void *buf, uint32_t buf_size, uint32_t *out_len, int timeout_ms)
{
	if (!r || !r->hdr)
		return -1;

	for (;;) {
		if (venc_frame_ring_read(r, buf, buf_size, out_len) == 0)
			return 0;

#ifdef __linux__
		uint32_t seq = __atomic_load_n(&r->hdr->futex_seq,
			__ATOMIC_ACQUIRE);
		__atomic_store_n(&r->hdr->consumer_waiting, 1,
			__ATOMIC_RELEASE);

		if (venc_frame_ring_read(r, buf, buf_size, out_len) == 0) {
			__atomic_store_n(&r->hdr->consumer_waiting, 0,
				__ATOMIC_RELEASE);
			return 0;
		}

		struct timespec ts;
		struct timespec *tsp = NULL;
		if (timeout_ms > 0) {
			ts.tv_sec = timeout_ms / 1000;
			ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
			tsp = &ts;
		}

		syscall(SYS_futex, &r->hdr->futex_seq,
		        FUTEX_WAIT, seq, tsp, NULL, 0);

		__atomic_store_n(&r->hdr->consumer_waiting, 0,
			__ATOMIC_RELEASE);
#else
		usleep(1000);
#endif
		if (venc_frame_ring_read(r, buf, buf_size, out_len) == 0)
			return 0;

		if (timeout_ms > 0)
			return -1;
	}
}

#endif /* VENC_FRAME_RING_H */
