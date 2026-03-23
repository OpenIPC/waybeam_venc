#ifndef OPTFLOW_H
#define OPTFLOW_H

#include <stdint.h>

typedef struct OptFlowState OptFlowState;

/** Create Star6E optical-flow tracker state. Returns NULL on allocation failure. */
OptFlowState *optflow_create(uint32_t capture_width, uint32_t capture_height,
	int verbose, uint32_t fps, const void *vpe_port);

/** Notify the tracker that one encoded frame was observed in the stream loop. */
void optflow_on_stream(OptFlowState *state, uint32_t pack_count);

/** Destroy tracker state and emit a short shutdown summary. */
void optflow_destroy(OptFlowState *state);

#endif /* OPTFLOW_H */