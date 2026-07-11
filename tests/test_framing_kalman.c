/* test_framing_kalman.c — prove the extracted framing_kalman.c is bit-identical
 * to the original inline Kalman that lived in star6e_framing_stab.c.
 *
 * ref_step() below is the ORIGINAL math verbatim (facc integrate, pause
 * glide-home, Kalman predict/update, round + clamp).  We drive both it and the
 * extracted framing_kalman_step() with the same pseudo-random measurement
 * stream — shakes, big pans, pause toggles, varied tau/clamp/q/r — and assert
 * the applied acc AND the full internal state match exactly at every step.
 * Both run identical float ops in identical order, so bit-exact `==` is the
 * correct comparison; any divergence means the extraction changed behaviour. */
#include "test_helpers.h"
#include "framing_kalman.h"

#include <math.h>
#include <stdint.h>

/* g_test_pass_count / g_test_fail_count are defined in test_runner.c. */

/* Reference state = original 6 globals + the two facc locals, as one struct. */
typedef struct {
	double facc_x, facc_y;
	double x_est, y_est;
	double x_p, y_p;
	double q, r;
} Ref;

static void ref_reset(Ref *k, double q_cfg, double r_cfg)
{
	k->facc_x = 0.0; k->facc_y = 0.0;
	k->x_est = 0.0;  k->y_est = 0.0;
	k->x_p = 1.0;    k->y_p = 1.0;
	k->q = (q_cfg >= 0.0001 && q_cfg <= 5.0) ? q_cfg : 0.03;
	k->r = (r_cfg >= 0.01   && r_cfg <= 200.0) ? r_cfg : 2.0;
}

/* Verbatim copy of the pre-refactor inline block (star6e_framing_stab.c). */
static void ref_step(Ref *k, double meas_dx, double meas_dy, int paused,
	uint32_t tau, int max_x, int max_y, int *acc_x, int *acc_y)
{
	k->facc_x += meas_dx;
	k->facc_y += meas_dy;
	if (paused) {
		double scale;
		if (tau < 2) tau = 2;
		scale = (double)(tau - 1) / (double)tau;
		k->facc_x -= meas_dx;
		k->facc_y -= meas_dy;
		k->facc_x *= scale;
		k->facc_y *= scale;
		k->x_est *= scale;
		k->y_est *= scale;
		if (fabs(k->facc_x) < 0.5) k->facc_x = 0.0;
		if (fabs(k->facc_y) < 0.5) k->facc_y = 0.0;
	} else {
		double pp_x = k->x_p + k->q;
		double k_x  = pp_x / (pp_x + k->r);
		double pp_y = k->y_p + k->q;
		double k_y  = pp_y / (pp_y + k->r);
		k->x_est += k_x * (k->facc_x - k->x_est);
		k->x_p = (1.0 - k_x) * pp_x;
		k->y_est += k_y * (k->facc_y - k->y_est);
		k->y_p = (1.0 - k_y) * pp_y;
	}
	int ax = (int)lround(k->facc_x - k->x_est);
	int ay = (int)lround(k->facc_y - k->y_est);
	if (ax < -max_x) ax = -max_x;
	if (ax >  max_x) ax =  max_x;
	if (ay < -max_y) ay = -max_y;
	if (ay >  max_y) ay =  max_y;
	*acc_x = ax;
	*acc_y = ay;
}

/* Deterministic LCG so the sequence is reproducible across runs/platforms. */
static uint32_t g_lcg = 0x12345678u;
static double frand(double lo, double hi)
{
	g_lcg = g_lcg * 1664525u + 1013904223u;
	double u = (g_lcg >> 8) / 16777216.0; /* [0,1) */
	return lo + u * (hi - lo);
}

static int states_equal(const Ref *r, const FramingKalman *k)
{
	return r->facc_x == k->facc_x && r->facc_y == k->facc_y &&
	       r->x_est == k->x_est && r->y_est == k->y_est &&
	       r->x_p == k->x_p && r->y_p == k->y_p &&
	       r->q == k->q && r->r == k->r;
}

/* One long mixed run at a given (q,r); returns 1 on full equivalence. */
static int run_equiv(double q, double r, int seed_bump)
{
	Ref ref;
	FramingKalman kal;
	g_lcg = 0x12345678u + (uint32_t)seed_bump * 2654435761u;
	ref_reset(&ref, q, r);
	framing_kalman_reset(&kal, q, r);
	if (!states_equal(&ref, &kal))
		return 0;

	int max_x = 40, max_y = 30;
	for (int i = 0; i < 20000; i++) {
		/* Mostly small shake; occasional large pan to exercise clamping. */
		double mx = (i % 400 == 0) ? frand(-120, 120) : frand(-3, 3);
		double my = (i % 550 == 0) ? frand(-90, 90) : frand(-3, 3);
		/* Toggle pause in bursts to hit both branches + the transition. */
		int paused = ((i / 300) % 3) == 2;
		/* Vary the glide tau (incl. the <2 clamp) and the border budget. */
		uint32_t tau = (uint32_t)((i % 7 == 0) ? 1 : (10 + (i % 40)));
		if (i % 1000 == 0) { max_x = 20 + (i % 30); max_y = 15 + (i % 25); }

		int rax, ray, kax, kay;
		ref_step(&ref, mx, my, paused, tau, max_x, max_y, &rax, &ray);
		framing_kalman_step(&kal, mx, my, paused, tau, max_x, max_y, &kax, &kay);
		if (rax != kax || ray != kay || !states_equal(&ref, &kal))
			return 0;
	}
	return 1;
}

int test_framing_kalman(void)
{
	int failures = 0;

	/* Equivalence across several (q,r) — defaults, extremes, mid. */
	CHECK("equiv q=0.03 r=2.0 (defaults)", run_equiv(0.03, 2.0, 1));
	CHECK("equiv q=0.30 r=10.0",           run_equiv(0.30, 10.0, 2));
	CHECK("equiv q=0.001 r=0.5",           run_equiv(0.001, 0.5, 3));
	CHECK("equiv q=1.0 r=50.0",            run_equiv(1.0, 50.0, 4));
	CHECK("equiv q=5.0 r=200.0 (max)",     run_equiv(5.0, 200.0, 5));

	/* reset() must clamp out-of-range q/r to the defaults, in-range passes. */
	FramingKalman k;
	framing_kalman_reset(&k, 0.03, 2.0);
	CHECK("reset in-range keeps q", k.q == 0.03);
	CHECK("reset in-range keeps r", k.r == 2.0);
	CHECK("reset zeroes facc/est",  k.facc_x == 0.0 && k.facc_y == 0.0 &&
	                                k.x_est == 0.0 && k.y_est == 0.0);
	CHECK("reset seeds P=1",        k.x_p == 1.0 && k.y_p == 1.0);
	framing_kalman_reset(&k, 0.0, 1000.0);   /* both out of range */
	CHECK("reset q underflow -> default", k.q == FRAMING_KALMAN_Q_DEFAULT);
	CHECK("reset r overflow -> default",  k.r == FRAMING_KALMAN_R_DEFAULT);
	framing_kalman_reset(&k, 99.0, 0.0001);  /* both out of range other way */
	CHECK("reset q overflow -> default",  k.q == FRAMING_KALMAN_Q_DEFAULT);
	CHECK("reset r underflow -> default", k.r == FRAMING_KALMAN_R_DEFAULT);

	/* A paused-from-start run must glide facc toward 0 and stay at acc=0. */
	framing_kalman_reset(&k, 0.03, 2.0);
	int ax = 99, ay = 99;
	for (int i = 0; i < 200; i++)
		framing_kalman_step(&k, 0.0, 0.0, 1, 30, 40, 30, &ax, &ay);
	CHECK("paused idle stays centred", ax == 0 && ay == 0 &&
	                                   k.facc_x == 0.0 && k.facc_y == 0.0);

	return failures;
}
