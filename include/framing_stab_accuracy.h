/* framing_stab_accuracy.h — single source of truth for the
 * MI_IVE_Shift_Detector geometry, shared by the Star6E and Maruko stab
 * backends so their detector configs cannot silently drift.
 *
 * Exposed to users as  video0.stab_accuracy = auto | high | medium | low :
 *
 *   level    crop  box  pyramid  search   ~cost   notes
 *   high     384   256  3        96       100%    smoothest; Star6E default
 *   medium   320   192  3        80       ~55%    3-level pyramid, smaller box
 *   low      256   128  2        64       ~33%    cheapest; Maruko default
 *
 * All three keep  margin = (crop - box) / 2 = 64px  — the maximum measurable
 * per-frame shift before the correlation box runs off the tap edge.  The
 * detector is NEON software (~17ms/call at "high" on one A7), so the level is
 * the CPU/quality lever; on the single-core i6c "low" fits ~50fps, "medium"
 * ~10ms.
 *
 * "auto" resolves per-backend to the level that preserves each backend's
 * historical behaviour (high on Star6E, low on single-core Maruko), so an
 * unset field changes nothing.  Both backends read this ONE table — retuning a
 * level here moves both in lock-step. */
#ifndef FRAMING_STAB_ACCURACY_H
#define FRAMING_STAB_ACCURACY_H

#include <string.h>

typedef struct {
	int crop;      /* square detector tap / correlation-window edge (px) */
	int box;       /* correlation box edge (px); margin = (crop - box) / 2 */
	int pyramid;   /* Shift_Detector pyramid levels */
	int search;    /* Shift_Detector search range (px) */
} FramingStabDetector;

/* Resolve a stab_accuracy level name to detector geometry.  "auto", empty, or
 * any unrecognised value resolves to auto_fallback (each backend passes its
 * historical default: "high" for Star6E, "low" for Maruko).  Never fails — an
 * unrecognised auto_fallback itself defaults to "high". */
static inline FramingStabDetector
framing_stab_detector_params(const char *level, const char *auto_fallback)
{
	const FramingStabDetector high   = { 384, 256, 3, 96 };
	const FramingStabDetector medium = { 320, 192, 3, 80 };
	const FramingStabDetector low    = { 256, 128, 2, 64 };
	const char *l = (level && *level) ? level : "auto";

	if (strcmp(l, "high")   == 0) return high;
	if (strcmp(l, "medium") == 0) return medium;
	if (strcmp(l, "low")    == 0) return low;
	/* "auto" or unknown -> the backend's historical default. */
	if (auto_fallback && strcmp(auto_fallback, "low")    == 0) return low;
	if (auto_fallback && strcmp(auto_fallback, "medium") == 0) return medium;
	return high;
}

/* User-facing level names accepted by the API / config loader. */
static inline int framing_stab_accuracy_valid(const char *level)
{
	return level && (
		strcmp(level, "auto")   == 0 ||
		strcmp(level, "high")   == 0 ||
		strcmp(level, "medium") == 0 ||
		strcmp(level, "low")    == 0);
}

#endif /* FRAMING_STAB_ACCURACY_H */
