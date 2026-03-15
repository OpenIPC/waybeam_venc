#include "maruko_output.h"

#include "venc_config.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int maruko_output_init(MarukoOutput *output, uint32_t sink_ip,
	uint16_t sink_port)
{
	if (!output)
		return -1;

	output->socket_handle = -1;
	output->ring = NULL;
	memset(&output->dst, 0, sizeof(output->dst));

	output->socket_handle = socket(AF_INET, SOCK_DGRAM, 0);
	if (output->socket_handle < 0) {
		fprintf(stderr, "ERROR: [maruko] unable to create UDP socket\n");
		return -1;
	}

	output->dst.sin_family = AF_INET;
	output->dst.sin_port = htons(sink_port);
	output->dst.sin_addr.s_addr = sink_ip;
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
	memset(&output->dst, 0, sizeof(output->dst));

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
	char host[128];
	uint16_t port;

	if (!output || !uri)
		return -1;

	/* SHM output doesn't support live server change */
	if (output->ring) {
		fprintf(stderr, "ERROR: [maruko] cannot change server in SHM mode\n");
		return -1;
	}

	if (output->socket_handle < 0)
		return -1;

	if (venc_config_parse_server_uri(uri, host, sizeof(host), &port) != 0)
		return -1;

	output->dst.sin_family = AF_INET;
	output->dst.sin_port = htons(port);
	output->dst.sin_addr.s_addr = inet_addr(host);
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
}
