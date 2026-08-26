#include "venc_frame_ring.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Slot stride: length prefix (4) + data, aligned to 8 bytes */
static uint32_t calc_slot_stride(uint32_t slot_data_size)
{
	uint32_t raw = (uint32_t)sizeof(uint32_t) + slot_data_size;
	return (raw + 7u) & ~7u;
}

static uint64_t calc_total_size(uint32_t slot_count, uint32_t slot_data_size)
{
	uint32_t stride = calc_slot_stride(slot_data_size);
	uint64_t slots_area = (uint64_t)slot_count * stride;
	uint64_t total = (uint64_t)sizeof(venc_frame_ring_hdr_t) + slots_area;
	return total;
}

static int is_power_of_2(uint32_t v)
{
	return v > 0 && (v & (v - 1)) == 0;
}

venc_frame_ring_t *venc_frame_ring_create(const char *shm_name,
	uint32_t slot_count, uint32_t slot_data_size)
{
	uint64_t total;
	char name[256];
	int fd;
	void *map;
	venc_frame_ring_hdr_t *hdr;
	venc_frame_ring_t *r;
	struct timespec ts;

	if (!shm_name || !shm_name[0] || !is_power_of_2(slot_count) ||
	    slot_data_size < VENC_FRAME_META_SIZE ||
	    slot_data_size > (UINT32_MAX - 8))
		return NULL;

	total = calc_total_size(slot_count, slot_data_size);
	if (total > UINT32_MAX)
		return NULL;

	if (shm_name[0] == '/')
		snprintf(name, sizeof(name), "%s", shm_name);
	else
		snprintf(name, sizeof(name), "/%s", shm_name);

	/* Stale-ring guard: same pattern as venc_ring.c */
	(void)shm_unlink(name);
	fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
	if (fd < 0) {
		fprintf(stderr, "[venc_frame_ring] shm_open(%s) failed: %s\n",
		        name, strerror(errno));
		return NULL;
	}

	if (ftruncate(fd, (off_t)total) != 0) {
		fprintf(stderr, "[venc_frame_ring] ftruncate(%llu) failed: %s\n",
		        (unsigned long long)total, strerror(errno));
		close(fd);
		shm_unlink(name);
		return NULL;
	}

	map = mmap(NULL, (size_t)total, PROT_READ | PROT_WRITE,
	           MAP_SHARED, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		fprintf(stderr, "[venc_frame_ring] mmap(%llu) failed: %s\n",
		        (unsigned long long)total, strerror(errno));
		shm_unlink(name);
		return NULL;
	}

	memset(map, 0, (size_t)total);

	hdr = (venc_frame_ring_hdr_t *)map;
	hdr->magic = VENC_FRAME_RING_MAGIC;
	hdr->version = VENC_FRAME_RING_VERSION;
	hdr->slot_count = slot_count;
	hdr->slot_data_size = slot_data_size;
	hdr->total_size = (uint32_t)total;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	hdr->epoch = (uint32_t)((uint64_t)ts.tv_sec * 1000 +
		ts.tv_nsec / 1000000);

	/* Producer health (protocols/frame-shm.md).  Counters first, marker
	 * last, and the whole group before init_complete -- a consumer must
	 * never see VHLT set over uninitialised counters. */
	hdr->full_drops = 0;
	hdr->low_water_permille = 0;
	__atomic_store_n(&hdr->health_magic, VENC_FRAME_RING_HEALTH_MAGIC,
		__ATOMIC_RELEASE);

	__atomic_store_n(&hdr->init_complete, 1, __ATOMIC_RELEASE);

	r = (venc_frame_ring_t *)calloc(1, sizeof(*r));
	if (!r) {
		munmap(map, (size_t)total);
		shm_unlink(name);
		return NULL;
	}

	r->hdr = hdr;
	r->slots_base = (uint8_t *)map + sizeof(venc_frame_ring_hdr_t);
	r->slot_stride = calc_slot_stride(slot_data_size);
	r->slot_data_size = slot_data_size;
	r->map_size = (uint32_t)total;
	r->is_owner = 1;
	snprintf(r->name, sizeof(r->name), "%s", name);

	printf("[venc_frame_ring] Created %s: %u slots x %u KB = %u KB "
	       "(epoch=%u)\n",
	       name, slot_count, slot_data_size / 1024,
	       (uint32_t)(total / 1024), hdr->epoch);
	return r;
}

venc_frame_ring_t *venc_frame_ring_attach(const char *shm_name)
{
	char name[256];
	int fd;
	venc_frame_ring_hdr_t tmp;
	uint64_t expected_total;
	struct stat st;
	void *map;
	venc_frame_ring_hdr_t *hdr;
	venc_frame_ring_t *r;

	if (!shm_name || !shm_name[0])
		return NULL;

	if (shm_name[0] == '/')
		snprintf(name, sizeof(name), "%s", shm_name);
	else
		snprintf(name, sizeof(name), "/%s", shm_name);

	fd = shm_open(name, O_RDWR, 0);
	if (fd < 0)
		return NULL;

	memset(&tmp, 0, sizeof(tmp));
	if (read(fd, &tmp, sizeof(tmp)) != (ssize_t)sizeof(tmp)) {
		close(fd);
		return NULL;
	}

	if (tmp.magic != VENC_FRAME_RING_MAGIC ||
	    tmp.version != VENC_FRAME_RING_VERSION ||
	    !is_power_of_2(tmp.slot_count) ||
	    tmp.slot_data_size < VENC_FRAME_META_SIZE ||
	    tmp.init_complete != 1) {
		close(fd);
		return NULL;
	}

	expected_total = calc_total_size(tmp.slot_count, tmp.slot_data_size);
	if (expected_total > UINT32_MAX ||
	    (uint32_t)expected_total != tmp.total_size) {
		close(fd);
		return NULL;
	}

	if (fstat(fd, &st) != 0 ||
	    (uint64_t)st.st_size != expected_total) {
		close(fd);
		return NULL;
	}

	map = mmap(NULL, (size_t)expected_total, PROT_READ | PROT_WRITE,
	           MAP_SHARED, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return NULL;

	hdr = (venc_frame_ring_hdr_t *)map;
	if (hdr->magic != VENC_FRAME_RING_MAGIC ||
	    hdr->version != VENC_FRAME_RING_VERSION ||
	    hdr->slot_count != tmp.slot_count ||
	    hdr->slot_data_size != tmp.slot_data_size ||
	    hdr->total_size != (uint32_t)expected_total ||
	    __atomic_load_n(&hdr->init_complete, __ATOMIC_ACQUIRE) != 1) {
		munmap(map, (size_t)expected_total);
		return NULL;
	}

	r = (venc_frame_ring_t *)calloc(1, sizeof(*r));
	if (!r) {
		munmap(map, (size_t)expected_total);
		return NULL;
	}

	r->hdr = hdr;
	r->slots_base = (uint8_t *)map + sizeof(venc_frame_ring_hdr_t);
	r->slot_stride = calc_slot_stride(hdr->slot_data_size);
	r->slot_data_size = hdr->slot_data_size;
	r->map_size = (uint32_t)expected_total;
	r->is_owner = 0;
	snprintf(r->name, sizeof(r->name), "%s", name);

	return r;
}

void venc_frame_ring_destroy(venc_frame_ring_t *r)
{
	if (!r) return;

	if (r->hdr) {
		if (r->is_owner)
			__atomic_store_n(&r->hdr->init_complete, 0,
				__ATOMIC_RELEASE);
		munmap(r->hdr, r->map_size);
		r->hdr = NULL;
	}

	if (r->is_owner && r->name[0]) {
		shm_unlink(r->name);
		printf("[venc_frame_ring] Destroyed %s\n", r->name);
	}

	free(r);
}
