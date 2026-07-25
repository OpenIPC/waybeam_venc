#ifndef STAR6E_FRAMING_STAB_H
#define STAR6E_FRAMING_STAB_H

#include "star6e_framing.h"

/* HW-crop image stabilization preset ("stab").  Compiled in when STAB=1. */
extern const FramingModule star6e_framing_stab;

/* Floating-image stabilization preset ("stab-fill"): constant-scale image on a
 * black border (CPU compose).  Shares the module's detector/geometry/IVE with
 * "stab"; compiled in when STAB=1. */
extern const FramingModule star6e_framing_stab_fill;

/* Debug-OSD snapshot from the stab/stab-fill module: last detector measurement
 * + Kalman correction (stab pixels), pause state, and whether the fill preset
 * is active.  Returns 1 when a stab thread is running, else 0 (hide the row).
 * Defined in star6e_framing_stab.c — only call under HAVE_FRAMING_STAB. */
int star6e_framing_stab_osd_status(int *acc_x, int *acc_y,
	int *meas_x, int *meas_y, int *paused, int *fill);

#endif /* STAR6E_FRAMING_STAB_H */
