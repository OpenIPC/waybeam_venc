/* test_attitude_est.c — complementary-filter attitude estimator checks.
 *
 * Drives attitude_est with synthetic accel/gyro sequences at the default
 * 200 Hz ODR and asserts roll/pitch converge to known ground truth within
 * tolerance (spec: multi-telemetry HUD group 6, review finding #4).
 */
#include "test_helpers.h"
#include "attitude_est.h"

#include <math.h>
#include <stdlib.h>

#define G 9.80665f
#define DT_US 5000ULL /* 200 Hz */

/* Feed n static samples of a gravity vector tilted by (roll, pitch). */
static void feed_static(AttitudeEst *e, float roll, float pitch,
	uint64_t *ts, int n)
{
	/* Gravity in sensor frame for x-fwd/y-right/z-down convention:
	 * ax = -sin(pitch)*g, ay = sin(roll)*cos(pitch)*g,
	 * az = cos(roll)*cos(pitch)*g. */
	float ax = -sinf(pitch) * G;
	float ay = sinf(roll) * cosf(pitch) * G;
	float az = cosf(roll) * cosf(pitch) * G;
	for (int i = 0; i < n; i++) {
		*ts += DT_US;
		attitude_est_update(e, 0.0f, 0.0f, 0.0f, ax, ay, az, *ts);
	}
}

int test_attitude_est(void)
{
	int failures = 0;

	/* 1: level start snaps from the first accel sample */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, 0.0f, 0.0f, &ts, 1);
		CHECK("att_init_level_roll",
		      abs(attitude_est_roll_cdeg(&e)) <= 5);
		CHECK("att_init_level_pitch",
		      abs(attitude_est_pitch_cdeg(&e)) <= 5);
		CHECK("att_not_settled_yet", !attitude_est_settled(&e));
	}

	/* 2: static 30° roll converges within 1° and settles */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		float roll = 30.0f * (float)M_PI / 180.0f;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, roll, 0.0f, &ts, 400); /* 2 s */
		CHECK("att_roll30_converged",
		      abs(attitude_est_roll_cdeg(&e) - 300) <= 10);
		CHECK("att_roll30_pitch_zero",
		      abs(attitude_est_pitch_cdeg(&e)) <= 10);
		CHECK("att_roll30_settled", attitude_est_settled(&e));
	}

	/* 3: static -15° pitch converges within 1° */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		float pitch = -15.0f * (float)M_PI / 180.0f;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, 0.0f, pitch, &ts, 400);
		CHECK("att_pitchm15_converged",
		      abs(attitude_est_pitch_cdeg(&e) - (-150)) <= 10);
	}

	/* 4: gyro carries a fast roll rotation (accel held at level gravity —
	 * a manoeuvre the accel actively disagrees with). 90°/s for 0.5 s
	 * with tc=2 s must land near 45°: the complementary leak over 0.5 s
	 * is ~1-e^(-0.25) ≈ 22% toward 0, so expect ~35-45°. */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, 0.0f, 0.0f, &ts, 200); /* settle level */
		float rate = 90.0f * (float)M_PI / 180.0f;
		for (int i = 0; i < 100; i++) { /* 0.5 s */
			ts += DT_US;
			attitude_est_update(&e, rate, 0.0f, 0.0f,
			                    0.0f, 0.0f, G, ts);
		}
		int16_t r = attitude_est_roll_cdeg(&e);
		CHECK("att_gyro_roll_tracks", r >= 330 && r <= 460);
	}

	/* 5: free-fall / manoeuvre accel (|a| far from 1 g) is rejected —
	 * attitude holds on gyro alone instead of chasing garbage. */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, 0.0f, 0.0f, &ts, 200);
		for (int i = 0; i < 200; i++) {
			ts += DT_US;
			attitude_est_update(&e, 0.0f, 0.0f, 0.0f,
			                    0.5f, 0.5f, 1.0f, ts); /* ~0.13 g */
		}
		CHECK("att_lowg_rejected",
		      abs(attitude_est_roll_cdeg(&e)) <= 10 &&
		      abs(attitude_est_pitch_cdeg(&e)) <= 10);
	}

	/* 6: yaw integrates gyro z and is relative */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, 0.0f, 0.0f, &ts, 10);
		float rate = 45.0f * (float)M_PI / 180.0f;
		for (int i = 0; i < 200; i++) { /* 1 s */
			ts += DT_US;
			attitude_est_update(&e, 0.0f, 0.0f, rate,
			                    0.0f, 0.0f, G, ts);
		}
		int16_t y = attitude_est_yaw_cdeg(&e);
		CHECK("att_yaw_integrates", y >= 430 && y <= 470);
	}

	/* 7: out-of-order timestamps are ignored, big gaps clamped */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, 0.0f, 0.0f, &ts, 200);
		int16_t before = attitude_est_roll_cdeg(&e);
		/* out-of-order sample with wild gyro must be a no-op */
		attitude_est_update(&e, 10.0f, 10.0f, 10.0f,
		                    0.0f, 0.0f, G, ts - 100000);
		CHECK("att_ooo_ignored",
		      attitude_est_roll_cdeg(&e) == before);
		/* 10 s gap with 1 rad/s roll rate: dt clamps to 0.1 s →
		 * ~5.7° not ~570° */
		attitude_est_update(&e, 1.0f, 0.0f, 0.0f,
		                    0.0f, 0.0f, G, ts + 10000000);
		CHECK("att_gap_clamped",
		      abs(attitude_est_roll_cdeg(&e)) < 100);
	}

	/* 8: axis map — identity passes vectors through */
	{
		AttitudeAxisMap m;
		float out[3];
		CHECK("axmap_identity_init",
		      attitude_axis_map_init(&m, "+x", "+z", 0, 0) == 0);
		attitude_axis_map_apply(&m, 1.0f, 2.0f, 3.0f, out);
		CHECK("axmap_identity_apply",
		      out[0] == 1.0f && out[1] == 2.0f && out[2] == 3.0f);
	}

	/* 9: axis map — signed permutation (fwd=-y, down=+x →
	 * right = down×fwd = -z) */
	{
		AttitudeAxisMap m;
		float out[3];
		CHECK("axmap_perm_init",
		      attitude_axis_map_init(&m, "-y", "+x", 0, 0) == 0);
		attitude_axis_map_apply(&m, 1.0f, 2.0f, 3.0f, out);
		CHECK("axmap_perm_apply",
		      out[0] == -2.0f && out[1] == -3.0f && out[2] == 1.0f);
	}

	/* 10: axis map — a sensor mounted x-up (gravity along -x) reads
	 * level once remapped with axisDown=-x */
	{
		AttitudeAxisMap m;
		float out[3];
		CHECK("axmap_xup_init",
		      attitude_axis_map_init(&m, "+z", "-x", 0, 0) == 0);
		attitude_axis_map_apply(&m, -G, 0.0f, 0.0f, out);
		CHECK("axmap_xup_level",
		      out[0] == 0.0f && out[1] == 0.0f && out[2] == G);

		AttitudeEst e;
		uint64_t ts = 1000000;
		attitude_est_init(&e, 0.0f);
		for (int i = 0; i < 300; i++) {
			float a[3];
			ts += DT_US;
			attitude_axis_map_apply(&m, -G, 0.0f, 0.0f, a);
			attitude_est_update(&e, 0.0f, 0.0f, 0.0f,
			                    a[0], a[1], a[2], ts);
		}
		CHECK("axmap_xup_est_level",
		      abs(attitude_est_roll_cdeg(&e)) < 10 &&
		      abs(attitude_est_pitch_cdeg(&e)) < 10);
	}

	/* 11: axis map — invalid or parallel axes reject, keep identity */
	{
		AttitudeAxisMap m;
		float out[3];
		CHECK("axmap_bad_parse",
		      attitude_axis_map_init(&m, "x", "+z", 0, 0) == -1);
		CHECK("axmap_bad_axis",
		      attitude_axis_map_init(&m, "+w", "+z", 0, 0) == -1);
		CHECK("axmap_null", attitude_axis_map_init(&m, NULL, "+z", 0, 0) == -1);
		CHECK("axmap_parallel",
		      attitude_axis_map_init(&m, "+x", "-x", 0, 0) == -1);
		attitude_axis_map_apply(&m, 4.0f, 5.0f, 6.0f, out);
		CHECK("axmap_reject_keeps_identity",
		      out[0] == 4.0f && out[1] == 5.0f && out[2] == 6.0f);
	}

	/* 12: boresight trim — a board tilted 20° nose-up inside an x-up
	 * housing (the .201 case) reads level with axisDown=-x, axisFwd=+z,
	 * trimPitchDeg=20 (the pitch it showed while held level). */
	{
		AttitudeAxisMap m;
		float out[3];
		float c20 = cosf(20.0f * (float)M_PI / 180.0f);
		float s20 = sinf(20.0f * (float)M_PI / 180.0f);
		CHECK("axmap_trim_init",
		      attitude_axis_map_init(&m, "+z", "-x", 0, 20.0f) == 0);
		attitude_axis_map_apply(&m, -G * c20, 0.0f, -G * s20, out);
		CHECK("axmap_trim_pitch_level",
		      fabsf(out[0]) < 1e-3f && fabsf(out[1]) < 1e-3f &&
		      fabsf(out[2] - G) < 1e-3f);
	}

	/* 12b: trim solver — inverse of test 12: the .201 level-pose mean
	 * accel must solve to (0, +20) for the x-up housing */
	{
		float c20 = cosf(20.0f * (float)M_PI / 180.0f);
		float s20 = sinf(20.0f * (float)M_PI / 180.0f);
		float tr = 99.0f, tp = 99.0f;
		CHECK("axsolve_ok",
		      attitude_axis_map_solve_trims("+z", "-x",
		          -G * c20, 0.0f, -G * s20, &tr, &tp) == 0);
		CHECK("axsolve_pitch", fabsf(tp - 20.0f) < 0.1f);
		CHECK("axsolve_roll", fabsf(tr) < 0.1f);
		/* level identity mount solves to zero trims */
		CHECK("axsolve_identity_ok",
		      attitude_axis_map_solve_trims("+x", "+z",
		          0.0f, 0.0f, G, &tr, &tp) == 0);
		CHECK("axsolve_identity_zero",
		      fabsf(tr) < 0.01f && fabsf(tp) < 0.01f);
		/* implausible gravity (free-fall / moving) rejects */
		CHECK("axsolve_lowg_reject",
		      attitude_axis_map_solve_trims("+x", "+z",
		          0.0f, 0.0f, 1.0f, &tr, &tp) == -1);
		/* invalid axes reject */
		CHECK("axsolve_bad_axes_reject",
		      attitude_axis_map_solve_trims("+x", "-x",
		          0.0f, 0.0f, G, &tr, &tp) == -1);
	}

	/* 13: roll trim on an identity mount */
	{
		AttitudeAxisMap m;
		float out[3];
		float c10 = cosf(10.0f * (float)M_PI / 180.0f);
		float s10 = sinf(10.0f * (float)M_PI / 180.0f);
		CHECK("axmap_trim_roll_init",
		      attitude_axis_map_init(&m, "+x", "+z", 10.0f, 0) == 0);
		attitude_axis_map_apply(&m, 0.0f, G * s10, G * c10, out);
		CHECK("axmap_trim_roll_level",
		      fabsf(out[0]) < 1e-3f && fabsf(out[1]) < 1e-3f &&
		      fabsf(out[2] - G) < 1e-3f);
	}

	/* 14: corrupt-sample hardening — a NaN/Inf IMU sample must be dropped,
	 * not permanently poison the filter (and Inf must not spin wrap_pi).
	 * S1 review finding: an unguarded NaN left the estimator emitting a
	 * stuck "valid" level attitude for the rest of the process. */
	{
		AttitudeEst e;
		uint64_t ts = 1000000;
		attitude_est_init(&e, 0.0f);
		feed_static(&e, 0.0f, 0.0f, &ts, 1);   /* healthy, level */
		CHECK("att_healthy_after_init", attitude_est_healthy(&e));
		for (int i = 0; i < 50; i++) {         /* NaN burst — dropped */
			ts += DT_US;
			attitude_est_update(&e, NAN, NAN, NAN, NAN, NAN, NAN, ts);
		}
		ts += DT_US;                           /* Inf — must return */
		attitude_est_update(&e, INFINITY, 0.0f, 0.0f, 0.0f, 0.0f, G, ts);
		CHECK("att_healthy_after_corrupt", attitude_est_healthy(&e));
		CHECK("att_not_poisoned_roll",
		      abs(attitude_est_roll_cdeg(&e)) <= 5);
		CHECK("att_not_poisoned_pitch",
		      abs(attitude_est_pitch_cdeg(&e)) <= 5);
		/* good samples still drive the estimator after the corrupt burst:
		 * converge from the level init to 30° roll (>=5 tc @2 s). */
		feed_static(&e, 30.0f * (float)M_PI / 180.0f, 0.0f, &ts, 2000);
		CHECK("att_recovers_after_corrupt",
		      abs(attitude_est_roll_cdeg(&e) - 300) <= 20);
	}

	return failures;
}
