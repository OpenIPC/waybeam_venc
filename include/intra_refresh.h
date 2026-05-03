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
	uint32_t total_rows;      /* ceil(height / lcu_h); 0 if off */
	uint32_t lines;           /* effective stripe lines per P-frame; 0 if off */
	uint32_t gop_frames;      /* auto GOP in frames; 0 if off or overridden */
	double   gop_sec;         /* auto GOP in seconds; 0.0 if off or overridden */
	uint32_t req_iqp;         /* I-MB QP (codec default if override==0); 0 if off */
	int      lines_clamped;   /* override exceeded total_rows and was clamped */
	int      gop_overridden;  /* explicit gop_size > 0 suppressed auto-GOP */
} IntraRefreshDerived;

IntraRefreshMode intra_refresh_parse_mode(const char *s);
const char      *intra_refresh_mode_name(IntraRefreshMode m);

/* Compute derived parameters. `is_h265` selects LCU size (32 for H.265,
 * 16 for H.264) and codec-default QP (48 for H.265, 45 for H.264).
 * `override_lines==0` means "use mode auto", `override_qp==0` means
 * "use codec default", `explicit_gop_sec>0` suppresses auto-GOP.
 *
 * Always populates `*out`; for OFF mode all fields are zero. */
void intra_refresh_compute(
	IntraRefreshMode mode,
	uint32_t height, uint32_t fps, int is_h265,
	uint32_t override_lines,
	uint32_t override_qp,
	double   explicit_gop_sec,
	IntraRefreshDerived *out);

#endif /* INTRA_REFRESH_H */
