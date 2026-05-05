#ifndef MARUKO_RECORDER_H
#define MARUKO_RECORDER_H

/* Maruko adapter for the raw-HEVC file recorder.
 * Mirrors star6e_recorder_write_frame() but consumes i6c_venc_strm
 * (Maruko SDK) instead of MI_VENC_Stream_t (Star6E SDK).  Reuses
 * Star6eRecorderState for file lifecycle / disk-space / sync logic. */

#include "sigmastar_types.h"
#include "star6e_recorder.h"

int maruko_recorder_write_frame(Star6eRecorderState *state,
	const i6c_venc_strm *stream);

#endif /* MARUKO_RECORDER_H */
