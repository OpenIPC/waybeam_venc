#include "maruko_output.h"

#include "output_socket.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int maruko_output_init(MarukoOutput *output, const VencOutputUri *uri,
	int requested_connected_udp)
{
	if (!output)
		return -1;
	if (!uri || uri->type == VENC_OUTPUT_URI_SHM)
		return -1;

	output->socket_handle = -1;
	output->ring = NULL;
	output->dst_len = 0;
	output->transport = VENC_OUTPUT_URI_UDP;
	memset(&output->dst, 0, sizeof(output->dst));
	output->requested_connected_udp = requested_connected_udp ? 1 : 0;
	output->connected_udp = 0;
	output->send_errors = 0;
	memset(&output->batch, 0, sizeof(output->batch));
	output->batch.socket_handle = -1;

	if (output_socket_configure(&output->socket_handle, &output->dst,
	    &output->dst_len, &output->transport, uri,
	    output->requested_connected_udp, &output->connected_udp) != 0)
		return -1;
	__atomic_fetch_add(&output->transport_gen, 2, __ATOMIC_RELEASE);
	return 0;
}

int maruko_output_init_shm(MarukoOutput *output, const char *shm_name,
	uint16_t max_payload)
{
	uint32_t slot_data;

	if (!output || !shm_name || !shm_name[0])
		return -1;

	output->socket_handle = -1;
	output->ring = NULL;
	output->dst_len = 0;
	output->transport = VENC_OUTPUT_URI_UDP;
	memset(&output->dst, 0, sizeof(output->dst));
	output->requested_connected_udp = 0;
	output->connected_udp = 0;
	output->send_errors = 0;
	memset(&output->batch, 0, sizeof(output->batch));
	output->batch.socket_handle = -1;

	slot_data = (uint32_t)max_payload + 12;
	output->ring = venc_ring_create(shm_name, 512, slot_data);
	if (!output->ring) {
		fprintf(stderr, "ERROR: [maruko] venc_ring_create(%s) failed\n",
			shm_name);
		return -1;
	}

	printf("> [maruko] SHM output: %s (slot_data=%u)\n", shm_name,
		slot_data);
	__atomic_fetch_add(&output->transport_gen, 2, __ATOMIC_RELEASE);
	return 0;
}

uint16_t maruko_output_max_payload_cap(const MarukoOutput *output)
{
	uint32_t cap;

	if (!output)
		return 0;
	if (!output->ring)
		return UINT16_MAX;
	cap = output->ring->slot_data_size;
	if (cap <= 12)
		return 0;
	cap -= 12; /* RTP header */
	return cap > UINT16_MAX ? UINT16_MAX : (uint16_t)cap;
}

int maruko_output_apply_server(MarukoOutput *output, const char *uri)
{
	VencOutputUri parsed;

	if (!output || !uri)
		return -1;

	/* SHM output doesn't support live server change */
	if (output->ring) {
		fprintf(stderr, "ERROR: [maruko] cannot change server in SHM mode\n");
		return -1;
	}

	if (venc_config_parse_output_uri(uri, &parsed) != 0)
		return -1;
	if (parsed.type == VENC_OUTPUT_URI_SHM) {
		fprintf(stderr, "ERROR: [maruko] cannot change server to shm:// live\n");
		return -1;
	}

	__atomic_fetch_add(&output->transport_gen, 1, __ATOMIC_RELEASE); /* odd = writing */
	if (output_socket_configure(&output->socket_handle, &output->dst,
	    &output->dst_len, &output->transport, &parsed,
	    output->requested_connected_udp, &output->connected_udp) != 0) {
		__atomic_fetch_add(&output->transport_gen, 1, __ATOMIC_RELEASE); /* restore even */
		return -1;
	}
	__atomic_fetch_add(&output->transport_gen, 1, __ATOMIC_RELEASE); /* even = stable */
	return 0;
}

uint32_t maruko_output_drain_send_errors(MarukoOutput *output)
{
	uint32_t n;
	if (!output)
		return 0;
	n = output->send_errors;
	output->send_errors = 0;
	return n;
}

/* Flush the accumulated batch via sendmmsg().
 *
 * On partial success (sendmmsg returns 0 < n < count) or EINTR, retry
 * from the first unsent message. Only a persistent error (non-EINTR
 * failure on the next unsent message) ends the loop; the remaining
 * unsent packets are counted into output->send_errors so the caller can
 * observe silent drops via maruko_output_drain_send_errors().
 *
 * Returns number of messages successfully sent. Always resets
 * batch->count to 0. */
static int maruko_batch_flush(MarukoOutput *output)
{
	MarukoOutputBatch *b = &output->batch;
	size_t sent_total = 0;
	int fd;

	if (b->count == 0)
		return 0;

	/* Use the batch-snapshotted socket — output->socket_handle can be
	 * mutated by a concurrent apply_server() on the HTTP thread between
	 * begin_frame and here. */
	fd = b->socket_handle;
	if (fd < 0) {
		output->send_errors += (uint32_t)b->count;
		b->count = 0;
		return 0;
	}

	while (sent_total < b->count) {
		int n = sendmmsg(fd, b->msgs + sent_total,
			(unsigned int)(b->count - sent_total), 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			output->send_errors +=
				(uint32_t)(b->count - sent_total);
			break;
		}
		if (n == 0) {
			output->send_errors +=
				(uint32_t)(b->count - sent_total);
			break;
		}
		sent_total += (size_t)n;
	}

	b->count = 0;
	return (int)sent_total;
}

void maruko_output_begin_frame(MarukoOutput *output)
{
	MarukoOutputBatch *b;
	uint32_t gen_before, gen_after;

	if (!output)
		return;
	b = &output->batch;
	b->count = 0;
	b->active = 0;

	/* SHM output is not batched — skip the snapshot entirely. */
	if (output->ring)
		return;

	/* Seqlock read of transport state: retry while apply_server() holds
	 * an odd generation. Matches the writer pattern in
	 * maruko_output_apply_server. */
	for (;;) {
		gen_before = __atomic_load_n(&output->transport_gen,
			__ATOMIC_ACQUIRE);
		if (gen_before & 1u) {
			continue;
		}
		b->socket_handle = output->socket_handle;
		b->dst = output->dst;
		b->dst_len = output->dst_len;
		b->connected_udp = output->connected_udp;
		gen_after = __atomic_load_n(&output->transport_gen,
			__ATOMIC_ACQUIRE);
		if (gen_before == gen_after)
			break;
	}

	b->active = (b->socket_handle >= 0) ? 1 : 0;
}

int maruko_output_end_frame(MarukoOutput *output)
{
	int sent;

	if (!output || !output->batch.active)
		return 0;
	sent = maruko_batch_flush(output);
	output->batch.active = 0;
	return sent;
}

int maruko_output_batch_enqueue(MarukoOutput *output,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len)
{
	MarukoOutputBatch *b;
	size_t slot;
	size_t scratch_len;
	struct iovec *iov;
	struct msghdr *hdr;

	if (!output || !output->batch.active)
		return -1;
	if (!header || !payload1 || header_len == 0 || payload1_len == 0)
		return -1;

	b = &output->batch;
	scratch_len = header_len + payload1_len;
	if (scratch_len > MARUKO_OUTPUT_BATCH_SLOT_SCRATCH)
		return -1;

	if (b->count >= MARUKO_OUTPUT_BATCH_MAX)
		maruko_batch_flush(output);

	slot = b->count;
	iov = &b->iov[slot * 2];
	hdr = &b->msgs[slot].msg_hdr;

	/* Copy header + payload1 into owned scratch so the caller can reuse
	 * both stack buffers for the next packet before we flush. */
	memcpy(b->scratch[slot], header, header_len);
	memcpy(b->scratch[slot] + header_len, payload1, payload1_len);
	iov[0].iov_base = b->scratch[slot];
	iov[0].iov_len = scratch_len;

	if (payload2 && payload2_len > 0) {
		iov[1].iov_base = (void *)payload2;
		iov[1].iov_len = payload2_len;
	}

	memset(hdr, 0, sizeof(*hdr));
	if (b->connected_udp) {
		hdr->msg_name = NULL;
		hdr->msg_namelen = 0;
	} else {
		hdr->msg_name = (void *)&b->dst;
		hdr->msg_namelen = b->dst_len;
	}
	hdr->msg_iov = iov;
	hdr->msg_iovlen = (payload2 && payload2_len > 0) ? 2 : 1;
	b->msgs[slot].msg_len = 0;

	b->count++;
	return 0;
}

void maruko_output_teardown(MarukoOutput *output)
{
	if (!output)
		return;

	if (output->ring) {
		venc_ring_destroy(output->ring);
		output->ring = NULL;
	}
	if (output->socket_handle >= 0) {
		close(output->socket_handle);
		output->socket_handle = -1;
	}
	memset(&output->dst, 0, sizeof(output->dst));
	output->dst_len = 0;
	output->transport = VENC_OUTPUT_URI_UDP;
	output->batch.active = 0;
	output->batch.count = 0;
	output->batch.socket_handle = -1;
}
