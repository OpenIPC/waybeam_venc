#include "venc_ring.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Slot stride: length prefix (2) + data, aligned to 8 bytes */
static uint32_t calc_slot_stride(uint32_t slot_data_size)
{
	uint32_t raw = (uint32_t)sizeof(uint16_t) + slot_data_size;
	return (raw + 7u) & ~7u;
}

/* Overflow-checked total size.  Returns 0 on overflow. */
static uint32_t calc_total_size(uint32_t slot_count, uint32_t slot_data_size)
{
	uint32_t stride = calc_slot_stride(slot_data_size);
	uint64_t slots_area = (uint64_t)slot_count * stride;
	uint64_t total = (uint64_t)sizeof(venc_ring_hdr_t) + slots_area;
	if (total > UINT32_MAX)
		return 0;
	return (uint32_t)total;
}

static int is_power_of_2(uint32_t v)
{
	return v > 0 && (v & (v - 1)) == 0;
}

venc_ring_t *venc_ring_create(const char *shm_name, uint32_t slot_count,
                               uint32_t slot_data_size)
{
	if (!shm_name || !shm_name[0] || !is_power_of_2(slot_count) ||
	    slot_data_size == 0 || slot_data_size > 65535)
		return NULL;

	uint32_t total = calc_total_size(slot_count, slot_data_size);
	if (total == 0)
		return NULL;  /* overflow */

	/* Prepend '/' if not present (POSIX shm_open requirement) */
	char name[256];
	if (shm_name[0] == '/')
		snprintf(name, sizeof(name), "%s", shm_name);
	else
		snprintf(name, sizeof(name), "/%s", shm_name);

	/* Stale-ring guard: a previous producer killed by SIGKILL (or
	 * crashing before venc_ring_destroy()) leaves /dev/shm/<name>
	 * with init_complete=1 and an old wfb_tx still mmap'd to it.
	 * O_TRUNC reuses the same inode and races wfb_tx through
	 * magic=0/init_complete=0 during memset() while it still has
	 * stale slot_stride / epoch cached locally — silent corrupt
	 * reads or hangs. Instead, unlink the name (existing mappings
	 * remain valid on the orphaned inode until the consumer
	 * detaches), then O_EXCL-create a fresh inode the old consumer
	 * never saw. */
	(void)shm_unlink(name);
	int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
	if (fd < 0) {
		fprintf(stderr, "[venc_ring] shm_open(%s) failed: %s\n",
		        name, strerror(errno));
		return NULL;
	}

	if (ftruncate(fd, (off_t)total) != 0) {
		fprintf(stderr, "[venc_ring] ftruncate(%u) failed: %s\n",
		        total, strerror(errno));
		close(fd);
		shm_unlink(name);
		return NULL;
	}

	void *map = mmap(NULL, total, PROT_READ | PROT_WRITE,
	                 MAP_SHARED, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		fprintf(stderr, "[venc_ring] mmap(%u) failed: %s\n",
		        total, strerror(errno));
		shm_unlink(name);
		return NULL;
	}

	memset(map, 0, total);

	venc_ring_hdr_t *hdr = (venc_ring_hdr_t *)map;
	hdr->magic = VENC_RING_MAGIC;
	hdr->version = VENC_RING_VERSION;
	hdr->slot_count = slot_count;
	hdr->slot_data_size = slot_data_size;
	hdr->total_size = total;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	hdr->epoch = (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

	/* init_complete set last — consumer must see all fields before this */
	__atomic_store_n(&hdr->init_complete, 1, __ATOMIC_RELEASE);

	venc_ring_t *r = (venc_ring_t *)calloc(1, sizeof(*r));
	if (!r) {
		munmap(map, total);
		shm_unlink(name);
		return NULL;
	}

	r->hdr = hdr;
	r->slots_base = (uint8_t *)map + sizeof(venc_ring_hdr_t);
	r->slot_stride = calc_slot_stride(slot_data_size);
	r->slot_data_size = slot_data_size;
	r->map_size = total;
	r->is_owner = 1;
	snprintf(r->name, sizeof(r->name), "%s", name);

	printf("[venc_ring] Created %s: %u slots × %u bytes = %u KB (epoch=%u)\n",
	       name, slot_count, slot_data_size, total / 1024, hdr->epoch);
	return r;
}

venc_ring_t *venc_ring_attach(const char *shm_name)
{
	if (!shm_name || !shm_name[0])
		return NULL;

	char name[256];
	if (shm_name[0] == '/')
		snprintf(name, sizeof(name), "%s", shm_name);
	else
		snprintf(name, sizeof(name), "/%s", shm_name);

	int fd = shm_open(name, O_RDWR, 0);
	if (fd < 0)
		return NULL;

	/* Read header to validate before full mmap */
	venc_ring_hdr_t tmp = {0};
	if (read(fd, &tmp, sizeof(tmp)) != (ssize_t)sizeof(tmp)) {
		close(fd);
		return NULL;
	}

	/* Validate magic, version, init_complete */
	if (tmp.magic != VENC_RING_MAGIC || tmp.version != VENC_RING_VERSION ||
	    !is_power_of_2(tmp.slot_count) || tmp.slot_data_size == 0 ||
	    tmp.slot_data_size > 65535 || tmp.init_complete != 1) {
		close(fd);
		return NULL;
	}

	/* Recompute expected total and verify consistency */
	uint32_t expected_total = calc_total_size(tmp.slot_count, tmp.slot_data_size);
	if (expected_total == 0 || expected_total != tmp.total_size) {
		close(fd);
		return NULL;
	}

	/* fstat() to verify actual SHM size matches */
	struct stat st;
	if (fstat(fd, &st) != 0 || (uint64_t)st.st_size != (uint64_t)expected_total) {
		close(fd);
		return NULL;
	}

	void *map = mmap(NULL, expected_total, PROT_READ | PROT_WRITE,
	                 MAP_SHARED, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return NULL;

	/* Re-validate header from mapped memory */
	venc_ring_hdr_t *hdr = (venc_ring_hdr_t *)map;
	if (hdr->magic != VENC_RING_MAGIC || hdr->version != VENC_RING_VERSION ||
	    hdr->slot_count != tmp.slot_count ||
	    hdr->slot_data_size != tmp.slot_data_size ||
	    hdr->total_size != expected_total ||
	    __atomic_load_n(&hdr->init_complete, __ATOMIC_ACQUIRE) != 1) {
		munmap(map, expected_total);
		return NULL;
	}

	venc_ring_t *r = (venc_ring_t *)calloc(1, sizeof(*r));
	if (!r) {
		munmap(map, expected_total);
		return NULL;
	}

	r->hdr = hdr;
	r->slots_base = (uint8_t *)map + sizeof(venc_ring_hdr_t);
	r->slot_stride = calc_slot_stride(hdr->slot_data_size);
	r->slot_data_size = hdr->slot_data_size;
	r->map_size = expected_total;
	r->is_owner = 0;
	snprintf(r->name, sizeof(r->name), "%s", name);

	return r;
}

void venc_ring_destroy(venc_ring_t *r)
{
	if (!r) return;

	if (r->hdr) {
		if (r->is_owner)
			__atomic_store_n(&r->hdr->init_complete, 0, __ATOMIC_RELEASE);
		munmap(r->hdr, r->map_size);
		r->hdr = NULL;
	}

	if (r->is_owner && r->name[0]) {
		shm_unlink(r->name);
		printf("[venc_ring] Destroyed %s\n", r->name);
	}

	free(r);
}
