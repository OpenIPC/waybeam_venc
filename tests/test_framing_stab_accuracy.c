/* test_framing_stab_accuracy.c — the shared Shift_Detector accuracy table.
 *
 * Guards the single source of truth both stab backends resolve through
 * (framing_stab_accuracy.h): the level geometry, the margin invariant, the
 * per-backend "auto" resolution, and the lenient unknown->fallback behaviour.
 * If Star6E or Maruko ever want to change a level, this test pins the contract
 * so the two backends cannot silently diverge again. */
#include "test_helpers.h"
#include "framing_stab_accuracy.h"

static int eq(FramingStabDetector d, int crop, int box, int pyr, int search)
{
	return d.crop == crop && d.box == box &&
	       d.pyramid == pyr && d.search == search;
}

int test_framing_stab_accuracy(void)
{
	int failures = 0;

	/* Explicit levels resolve to the fixed table regardless of fallback. */
	CHECK("high  = 384/256/3/96",
		eq(framing_stab_detector_params("high", "low"), 384, 256, 3, 96));
	CHECK("medium= 320/192/3/80",
		eq(framing_stab_detector_params("medium", "high"), 320, 192, 3, 80));
	CHECK("low   = 256/128/2/64",
		eq(framing_stab_detector_params("low", "high"), 256, 128, 2, 64));

	/* margin = (crop-box)/2 must be 64 at every level (the max measurable
	 * shift before the correlation box runs off the tap edge). */
	CHECK("margin high  = 64",
		(384 - 256) / 2 == 64 &&
		(framing_stab_detector_params("high", "high").crop -
		 framing_stab_detector_params("high", "high").box) / 2 == 64);
	CHECK("margin medium= 64",
		(framing_stab_detector_params("medium", "high").crop -
		 framing_stab_detector_params("medium", "high").box) / 2 == 64);
	CHECK("margin low   = 64",
		(framing_stab_detector_params("low", "low").crop -
		 framing_stab_detector_params("low", "low").box) / 2 == 64);

	/* "auto" resolves to the backend fallback: Star6E->high, Maruko->low. */
	CHECK("auto+high fallback -> high",
		eq(framing_stab_detector_params("auto", "high"), 384, 256, 3, 96));
	CHECK("auto+low fallback -> low",
		eq(framing_stab_detector_params("auto", "low"), 256, 128, 2, 64));
	CHECK("auto+medium fallback -> medium",
		eq(framing_stab_detector_params("auto", "medium"), 320, 192, 3, 80));

	/* Empty / NULL level behaves like "auto". */
	CHECK("empty -> fallback high",
		eq(framing_stab_detector_params("", "high"), 384, 256, 3, 96));
	CHECK("NULL -> fallback low",
		eq(framing_stab_detector_params(NULL, "low"), 256, 128, 2, 64));

	/* Unknown explicit value is lenient -> the backend fallback (never fails). */
	CHECK("garbage level -> fallback low",
		eq(framing_stab_detector_params("ludicrous", "low"), 256, 128, 2, 64));
	CHECK("garbage level + garbage fallback -> high (safe default)",
		eq(framing_stab_detector_params("ludicrous", "bogus"), 384, 256, 3, 96));

	/* Validity predicate: only the four names, and not garbage/NULL. */
	CHECK("valid: auto",   framing_stab_accuracy_valid("auto"));
	CHECK("valid: high",   framing_stab_accuracy_valid("high"));
	CHECK("valid: medium", framing_stab_accuracy_valid("medium"));
	CHECK("valid: low",    framing_stab_accuracy_valid("low"));
	CHECK("invalid: HIGH (case)", !framing_stab_accuracy_valid("HIGH"));
	CHECK("invalid: empty",       !framing_stab_accuracy_valid(""));
	CHECK("invalid: NULL",        !framing_stab_accuracy_valid(NULL));
	CHECK("invalid: garbage",     !framing_stab_accuracy_valid("ludicrous"));

	return failures;
}
