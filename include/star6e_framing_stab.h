#ifndef STAR6E_FRAMING_STAB_H
#define STAR6E_FRAMING_STAB_H

#include "star6e_framing.h"

/* HW-crop image stabilization preset ("stab").  Compiled in when STAB=1. */
extern const FramingModule star6e_framing_stab;

/* Floating-image stabilization preset ("stab-fill"): constant-scale image on a
 * black border (CPU compose).  Shares the module's detector/geometry/IVE with
 * "stab"; compiled in when STAB=1. */
extern const FramingModule star6e_framing_stab_fill;

#endif /* STAR6E_FRAMING_STAB_H */
