#ifndef CV610_AUDIO_H
#define CV610_AUDIO_H

#include "audio_ring.h"
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

/** Tee encoded Opus access units into `ring` for the TS recorder, or stop
 *  teeing when `ring` is NULL.  Safe to call from another thread while the
 *  capture thread runs; a NULL state is a no-op.  The caller owns the ring
 *  and must not destroy it before clearing the tee. */
void cv610_audio_set_record_ring(Cv610AudioState *state, AudioRing *ring);

/** Non-zero while the capture thread is alive; clears when it exits. */
int cv610_audio_is_running(Cv610AudioState *state);

/** Re-derive the audio destination from `video_output` and repoint the socket,
 *  for a live `outgoing.server` change.  Safe to call from the httpd thread
 *  while the capture thread runs: the transport fields are seqlock-protected
 *  and the capture thread snapshots them once per frame.
 *
 *  Returns 0 when there was nothing to do (no state, audio disabled, or
 *  `audio_port < 0`) as well as on success; -1 only when a destination was
 *  wanted and could not be programmed. */
int cv610_audio_apply_server(Cv610AudioState *state, const VencConfig *config,
	const VencOutputUri *video_output);

#endif /* CV610_AUDIO_H */
