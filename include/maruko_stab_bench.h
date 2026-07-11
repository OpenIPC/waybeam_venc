#ifndef MARUKO_STAB_BENCH_H
#define MARUKO_STAB_BENCH_H

/* Phase-1 SCL bench for the Maruko stab port (see
 * specs/2026-07-08-maruko-stab/plan.md).  This is a DEVELOPMENT-ONLY probe,
 * compiled in only when the build sets STAB_BENCH=1 (-DHAVE_STAB_BENCH=1); a
 * production image carries none of this code.  When enabled it does nothing
 * unless the env var WAYBEAM_STAB_BENCH is set at launch, so a bench build is
 * still safe to run normally.
 *
 * WAYBEAM_STAB_BENCH selects which sub-bench(es) run (comma/space separated,
 * or one of the presets):
 *   1a    — enable free SCL port 2 @384x384 compress=0, drain, measure cadence
 *   1b    — hammer MI_SCL_SetOutputPortParam on port 0 at N Hz (poke gate)
 *   1c    — SCL crop.x/y granularity probe on port 2 (odd vs even)
 *   1d    — port-2 tap + IVE Shift_Detector + port-0 emit; sustained-fps proof
 *   tap   — 1a + 1c   (safe: never touches port 0 / the encode path)
 *   all   — 1a, 1c, 1b, 1d in that order
 *   1     — alias for "tap" (the safe default)
 */

/* MarukoBackendContext is a tagless typedef (see maruko_pipeline.h) which
 * cannot be forward-declared, so includers must have maruko_pipeline.h in
 * scope first.  The only includer is src/maruko_pipeline.c, which does. */

#if defined(HAVE_STAB_BENCH)
void maruko_stab_bench_maybe_start(MarukoBackendContext *ctx);
void maruko_stab_bench_stop(void);
#else
static inline void maruko_stab_bench_maybe_start(MarukoBackendContext *ctx)
{
	(void)ctx;
}
static inline void maruko_stab_bench_stop(void) {}
#endif

#endif /* MARUKO_STAB_BENCH_H */
