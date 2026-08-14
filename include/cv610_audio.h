#ifndef CV610_AUDIO_H
#define CV610_AUDIO_H

#include "venc_config.h"

#include <stdint.h>

typedef struct Cv610AudioState Cv610AudioState;

/* Start the hardware-proven CV610 inner-codec -> AI -> AENC/Opus path.
 * The encoded Opus access units are wrapped with the shared RTP packetizer
 * and sent beside the configured video transport. */
Cv610AudioState *cv610_audio_start(const VencConfig *config,
	const VencOutputUri *video_output);

void cv610_audio_stop(Cv610AudioState *state);

void cv610_audio_get_stats(Cv610AudioState *state, uint64_t *frames,
	uint64_t *bytes, uint64_t *packets, uint64_t *drops);

/** Non-zero while the capture thread is alive; clears when it exits. */
int cv610_audio_is_running(Cv610AudioState *state);

#endif /* CV610_AUDIO_H */
