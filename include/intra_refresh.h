#ifndef INTRA_REFRESH_H
#define INTRA_REFRESH_H

#include <stdint.h>

/* GDR-style rolling intra refresh, mode-driven.
 *
 *   off       feature disabled
 *   fast      ~150 ms self-heal window  (FPV racing, low-latency, clean link)
 *   balanced  ~500 ms self-heal window  (general FPV, default when on)
 *   robust    ~1000 ms self-heal window (lossy long-range)
 *
 * Per-field overrides (intra_refresh_lines, intra_refresh_qp, gop_size) are
 * applied on top of the mode defaults; non-zero wins.
 *
 * Per-mode stripe QP defaults (codec-dependent). Lower QP = better stripe
 * quality + more bitrate cost. Robust runs the lowest QP because lossy
 * links want the cleanest possible recovery anchor; fast runs the highest
 * because clean links can absorb minor stripe banding without artifacts. */

typedef enum {
	INTRA_MODE_OFF = 0,
	INTRA_MODE_FAST,
	INTRA_MODE_BALANCED,
	INTRA_MODE_ROBUST,
} IntraRefreshMode;

typedef struct {
	IntraRefreshMode mode;
	uint32_t target_ms;       /* mode constant; 0 if off */
	uint32_t total_rows;      /* ceil(extent / 32); 0 if off. Units along the
	                           * REFRESH AXIS, not necessarily rows — see
	                           * intra_refresh_compute(). */
	uint32_t lines;           /* effective stripe lines per P-frame; 0 if off */
	uint32_t gop_frames;      /* auto GOP in frames; 0 if off or overridden */
	double   gop_sec;         /* auto GOP in seconds; 0.0 if off or overridden */
	uint32_t req_iqp;         /* I-MB QP (codec default if override==0); 0 if off */
	int      lines_clamped;   /* override exceeded total_rows and was clamped */
	int      gop_overridden;  /* explicit gop_size > 0 suppressed auto-GOP */
} IntraRefreshDerived;

IntraRefreshMode intra_refresh_parse_mode(const char *s);
const char      *intra_refresh_mode_name(IntraRefreshMode m);

/* Compute derived parameters for H.265 (CTU=32).
 * `override_lines==0` means "use mode auto", `override_qp==0` means
 * "use codec default", `explicit_gop_sec>0` suppresses auto-GOP.
 *
 * `extent` is the picture dimension the refresh sweep travels ALONG, in pixels
 * — NOT always the height. A row-refresh backend (Star6E, Maruko) passes the
 * height; CV610 refreshes by column and passes the WIDTH. Everything derived
 * from it (`total_rows`, `lines`) is therefore in 32-px units of that axis.
 * Passing the wrong dimension does not fail loudly: it silently makes the sweep
 * cover the wrong distance, so the refresh runs at the aspect ratio's factor
 * away from the mode's target_ms. That was a real bug (CV610 swept 34 units on
 * a 60-unit-wide picture, 1.7x slower than "fast" asks for).
 *
 * Always populates `*out`; for OFF mode all fields are zero. */
void intra_refresh_compute(
	IntraRefreshMode mode,
	uint32_t extent, uint32_t fps,
	uint32_t override_lines,
	uint32_t override_qp,
	double   explicit_gop_sec,
	IntraRefreshDerived *out);

#endif /* INTRA_REFRESH_H */
