#ifndef VENC_SVCT_H
#define VENC_SVCT_H

#include <stdint.h>

/* SVC-T (refPred) frame classification shared by both backends.
 *
 * The SigmaStar VENC firmware labels every encoded frame with a reference
 * type in the per-frame stream info (h265Info.refType); i6e (Star6E) and
 * i6c (Maruko) agree on the numbering.  ENHANCE_P_NOTFORREF == 4 is
 * device-validated on both SoCs (the TRAIL_N rewrite has shipped on it
 * since 0.10.x).  ENHANCE_P_REFBYENHANCE == 3 follows the vendor enum
 * layout but only ever appears with ref_enhance > 1 (range/fpv presets)
 * — bench-confirm the value on first per-layer deployment of those
 * presets and narrow venc_svct_frame_is_enhance() to NOTFORREF if base
 * frames ever classify as enhance. */
#define VENC_SVCT_REFTYPE_BASE_IDR               0
#define VENC_SVCT_REFTYPE_BASE_P_REFBYBASE       1
#define VENC_SVCT_REFTYPE_BASE_P_REFBYENHANCE    2
#define VENC_SVCT_REFTYPE_ENHANCE_P_REFBYENHANCE 3
#define VENC_SVCT_REFTYPE_ENHANCE_P_NOTFORREF    4

/* True when the frame belongs to the droppable enhancement layer (any
 * ENHANCE_* label).  Base frames never reference enhancement frames, so
 * the whole layer can be carried on a weaker-FEC channel or dropped.
 * Gated on ref_base > 0: with refPred off the encoder emits a flat
 * single-ref stream and every frame matters. */
static inline int venc_svct_frame_is_enhance(uint8_t ref_base,
	unsigned int ref_type)
{
	return ref_base > 0 &&
		(ref_type == VENC_SVCT_REFTYPE_ENHANCE_P_REFBYENHANCE ||
		 ref_type == VENC_SVCT_REFTYPE_ENHANCE_P_NOTFORREF);
}

/* True only for frames no other frame references — the ones safe to
 * relabel TRAIL_N in the bitstream (and to drop individually). */
static inline int venc_svct_frame_is_notforref(uint8_t ref_base,
	unsigned int ref_type)
{
	return ref_base > 0 &&
		ref_type == VENC_SVCT_REFTYPE_ENHANCE_P_NOTFORREF;
}

#endif /* VENC_SVCT_H */
