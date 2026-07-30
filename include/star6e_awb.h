#ifndef STAR6E_AWB_H
#define STAR6E_AWB_H

#include <stdint.h>

/*
 * Star6E userspace AWB loop.
 *
 * The i6e ISP-internal AWB does not converge under this pipeline: it latches
 * whatever it estimated in the first second and never moves again (on IMX335 a
 * standing yellow cast).  The SDK's own userspace-3A stack does converge, but
 * it runs per frame and pins the CPU at 120fps — measured 100% busy, box
 * unusable.  See HISTORY 0.56.0.
 *
 * So AWB is handed to userspace (Cus3AEnable_t.bAWB = 1) and driven from here
 * instead, at a rate WE choose:
 *
 *     MI_ISP_AWB_GetAwbHwAvgStats  — 128x90 block R/G/B averages, already
 *                                    computed by hardware, free to read
 *     grey-world + block rejection — a few hundred microseconds
 *     MI_ISP_CUS3A_SetAwbParam     — apply
 *
 * Because nothing here is per-frame, the cost is independent of frame rate:
 * 120fps costs exactly what 60fps costs.  That is the whole point — 15 Hz
 * (the default) was measured negligible at 100fps.
 *
 * Threading: one thread, started/stopped with the pipeline.  All SDK calls are
 * made from that thread only.
 */

/** Start the AWB loop at `hz` (0 disables — AWB then holds whatever the ISP
 *  last applied).  Safe to call when already running (no-op).  Returns 0 on
 *  success, -1 if the SDK entry points are unavailable. */
int star6e_awb_start(uint32_t hz);

/** Stop the loop and join the thread.  Leaves the last applied gains in place.
 *  Safe to call when not running. */
void star6e_awb_stop(void);

/** Live rate change; 0 pauses the loop without tearing the thread down. */
void star6e_awb_set_rate(uint32_t hz);

/** Pause/resume without stopping.  Used by `isp.awbMode=ct_manual`, which
 *  hands AWB back to the ISP-internal algorithm so the existing colour-
 *  temperature path applies unchanged. */
void star6e_awb_set_paused(int paused);

/** Debug-OSD/diagnostic snapshot.  Returns 1 when the loop is running (and
 *  fills the outputs), 0 when it is not.  Any pointer may be NULL. */
int star6e_awb_status(uint32_t *r_gain, uint32_t *g_gain, uint32_t *b_gain,
	uint32_t *ticks, int *paused);

#endif /* STAR6E_AWB_H */
