#include <stdio.h>
#include "venc_ring.h"

int main(int argc, char **argv)
{
	const char *name = (argc > 1) ? argv[1] : "venc_wfb";
	venc_ring_t *r = venc_ring_attach(name);
	if (!r) {
		fprintf(stderr, "Failed to attach to '%s'\n", name);
		return 1;
	}
	uint64_t w = __atomic_load_n(&r->hdr->write_idx, __ATOMIC_ACQUIRE);
	uint64_t rd = __atomic_load_n(&r->hdr->read_idx, __ATOMIC_ACQUIRE);
	uint32_t seq = __atomic_load_n(&r->hdr->futex_seq, __ATOMIC_ACQUIRE);
	printf("slots=%u data_size=%u total=%u\n",
	       r->hdr->slot_count, r->hdr->slot_data_size, r->hdr->total_size);
	printf("epoch=%u init_complete=%u version=%u\n",
	       r->hdr->epoch, r->hdr->init_complete, r->hdr->version);
	printf("write_idx=%lu read_idx=%lu lag=%lu futex_seq=%u\n",
	       (unsigned long)w, (unsigned long)rd, (unsigned long)(w - rd), seq);
	printf("ring %s\n", (w - rd >= r->hdr->slot_count) ? "FULL" : "ok");
	venc_ring_destroy(r);
	return 0;
}
