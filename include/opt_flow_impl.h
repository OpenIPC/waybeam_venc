#ifndef OPT_FLOW_IMPL_H
#define OPT_FLOW_IMPL_H

#include <stdint.h>

void *optflow_lk_create_impl(uint32_t capture_width, uint32_t capture_height,
	uint32_t osd_space_width, uint32_t osd_space_height,
	int verbose, uint32_t fps, int show_osd,
	const void *vpe_port, const void *osd_port);

void optflow_lk_on_stream_impl(void *state, uint32_t pack_count);

void optflow_lk_destroy_impl(void *state);

void *optflow_sad_create_impl(uint32_t capture_width, uint32_t capture_height,
	uint32_t osd_space_width, uint32_t osd_space_height,
	int verbose, uint32_t fps, int show_osd,
	const void *vpe_port, const void *osd_port);

void optflow_sad_on_stream_impl(void *state, uint32_t pack_count);

void optflow_sad_destroy_impl(void *state);

#endif /* OPT_FLOW_IMPL_H */