#ifndef MARUKO_STABFILL_PROBE_H
#define MARUKO_STABFILL_PROBE_H

#include <stdint.h>

/* Phase 5a go/no-go bench for Maruko (i6c) stab-fill.
 *
 * VERDICT HISTORY: the original 5a run concluded "the i6c VENC does not encode
 * manual pushes".  That device result was an ABI ARTIFACT — the probe's
 * BufConf used eBufType=0 (BUFDATA_RAW on i6c, not FRAME) and a Star6E-shaped
 * union offset, so every push was silently degenerate.  With the corrected
 * i6c MI_SYS_BufConf_t (now fixed in this probe too) the direct manual push
 * DOES encode at the full sensor rate — it is the shipped stab-fill feed path
 * (see src/maruko_framing_stab.c fill mode).  Also note: only VENC channels
 * 0..2 exist (MI_VENC_MAX_CHN_NUM_PER_DC=3), and a sibling frame-base channel
 * cannot coexist with a RING-fed chn 0 on the single H26x device (SYS/BUSY),
 * so this sibling-channel probe understates what the real single-channel
 * frame-base graph can do.
 *
 * Env-gated: only runs when MARUKO_STABFILL_PROBE is set.  Called once after
 * channel-0 is up; tears its probe channel down cleanly (never SIGKILL).
 * chn0_attr is the live i6c_venc_chn used for channel 0, reused as the probe
 * channel's encode config (opaque void* to keep this header SDK-free). */
void maruko_stabfill_probe_run(int venc_dev, const void *chn0_attr,
	uint32_t enc_w, uint32_t enc_h);

#endif /* MARUKO_STABFILL_PROBE_H */
