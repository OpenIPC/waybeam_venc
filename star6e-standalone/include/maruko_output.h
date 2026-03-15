#ifndef MARUKO_OUTPUT_H
#define MARUKO_OUTPUT_H

#include "venc_ring.h"

#include <netinet/in.h>
#include <stdint.h>

/** Maruko output module — manages socket/SHM lifecycle and destination. */
typedef struct {
	int socket_handle;
	struct sockaddr_in dst;
	venc_ring_t *ring;
} MarukoOutput;

/** Initialize UDP output: create socket and set destination. */
int maruko_output_init(MarukoOutput *output, uint32_t sink_ip,
	uint16_t sink_port);

/** Initialize SHM output: create shared memory ring buffer. */
int maruko_output_init_shm(MarukoOutput *output, const char *shm_name,
	uint16_t max_payload);

/** Change output destination URI without stopping streaming (UDP only). */
int maruko_output_apply_server(MarukoOutput *output, const char *uri);

/** Close socket/ring and release output resources. */
void maruko_output_teardown(MarukoOutput *output);

#endif /* MARUKO_OUTPUT_H */
