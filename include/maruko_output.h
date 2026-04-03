#ifndef MARUKO_OUTPUT_H
#define MARUKO_OUTPUT_H

#include "venc_config.h"
#include "venc_ring.h"

#include <stdint.h>
#include <sys/socket.h>

/** Maruko output module — manages socket/SHM lifecycle and destination. */
typedef struct {
	int socket_handle;
	struct sockaddr_storage dst;
	socklen_t dst_len;
	VencOutputUriType transport;
	venc_ring_t *ring;
} MarukoOutput;

/** Initialize UDP or Unix socket output from a parsed URI. */
int maruko_output_init(MarukoOutput *output, const VencOutputUri *uri);

/** Initialize SHM output: create shared memory ring buffer. */
int maruko_output_init_shm(MarukoOutput *output, const char *shm_name,
	uint16_t max_payload);

/** Change output destination URI without stopping streaming (udp:// or unix://). */
int maruko_output_apply_server(MarukoOutput *output, const char *uri);

/** Close socket/ring and release output resources. */
void maruko_output_teardown(MarukoOutput *output);

#endif /* MARUKO_OUTPUT_H */
