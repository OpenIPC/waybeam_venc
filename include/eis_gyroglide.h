#ifndef EIS_GYROGLIDE_H
#define EIS_GYROGLIDE_H

/*
 * GyroGlide-Lite EIS backend — frame-synchronous batched gyro integration
 * with spring-damped crop-window control.
 *
 * Key differences from legacy:
 *   - Timestamp-based per-sample integration (not uniform dt/n)
 *   - Direct position state with exponential recenter
 *   - Deadband on per-frame integrated delta
 *   - Edge-aware accelerated recentering
 *   - Optional slew limiting
 *   - Runtime gyro bias adaptation
 */

#include "eis.h"

/*
 * eis_gyroglide_create — Allocate and initialize the GyroGlide-Lite backend.
 * Called by eis_create() when cfg->mode is "gyroglide".
 * Returns NULL on failure.
 */
EisState *eis_gyroglide_create(const EisConfig *cfg);

#endif /* EIS_GYROGLIDE_H */
