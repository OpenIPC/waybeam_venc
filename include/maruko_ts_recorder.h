#ifndef MARUKO_TS_RECORDER_H
#define MARUKO_TS_RECORDER_H

#include "sigmastar_types.h"
#include "star6e_ts_recorder.h"

/** Adapter: extract NAL data from a Maruko VENC stream and feed it to the
 *  shared TS recorder via star6e_ts_recorder_write_video().  Mirrors
 *  star6e_ts_recorder_write_stream() but consumes i6c_venc_strm (Maruko)
 *  instead of i6_venc_strm (Star6E).
 *
 *  No-op if the recorder is not active (state->fd < 0), which makes it
 *  safe to call unconditionally from the encode/drain loop.  Returns
 *  bytes written, 0 if inactive, or -1 on write error. */
int maruko_ts_recorder_write_stream(Star6eTsRecorderState *state,
	const i6c_venc_strm *stream);

#endif /* MARUKO_TS_RECORDER_H */
