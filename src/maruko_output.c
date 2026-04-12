#include "maruko_output.h"

#include "output_socket.h"

#include <stdio.h>
#include <string.h>
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

	return output_socket_configure(&output->socket_handle, &output->dst,
		&output->dst_len, &output->transport, uri,
		output->requested_connected_udp, &output->connected_udp);
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

	slot_data = (uint32_t)max_payload + 12;
	output->ring = venc_ring_create(shm_name, 512, slot_data);
	if (!output->ring) {
		fprintf(stderr, "ERROR: [maruko] venc_ring_create(%s) failed\n",
			shm_name);
		return -1;
	}

	printf("> [maruko] SHM output: %s (slot_data=%u)\n", shm_name,
		slot_data);
	return 0;
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

	return output_socket_configure(&output->socket_handle, &output->dst,
		&output->dst_len, &output->transport, &parsed,
		output->requested_connected_udp, &output->connected_udp);
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
}
