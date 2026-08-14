#include "cv610_modes.h"

/* All four entries are hardware-verified on the IMX662 bring-up board.
 *
 * The clock column is the one that bites.  The sensor keeps whatever line
 * timing it was programmed with regardless of the MCLK it is actually fed,
 * so a mismatch is silent: frames simply arrive at fps * actual / expected
 * while /fps/live and /api/v1/modes both still report the nominal rate.
 * Measured 43.6 fps for the 60 fps mode when it ran on the 100 fps mode's
 * 27 MHz clock.  Only 100 fps takes the sensor's 24 MHz INCK profile
 * overclocked to 27 MHz (INCK_SEL 0x04); the rest run 37.125 MHz. */
static const Cv610SensorMode g_cv610_modes[] = {
	{ 1920, 1080,  30, 12, 37125000u, "1080p30 RAW12" },
	{ 1920, 1080,  60, 12, 37125000u, "1080p60 RAW12" },
	{ 1920, 1080,  90, 10, 37125000u, "1080p90 RAW10" },
	{ 1920, 1080, 100, 10, 27000000u, "1080p100 RAW10" },
};

#define CV610_MODE_COUNT \
	(sizeof(g_cv610_modes) / sizeof(g_cv610_modes[0]))

const Cv610SensorMode *cv610_mode_table(size_t *count)
{
	if (count)
		*count = CV610_MODE_COUNT;
	return g_cv610_modes;
}

const Cv610SensorMode *cv610_mode_lookup(uint32_t width, uint32_t height,
	uint32_t fps)
{
	size_t i;

	/* Only a fully unset geometry falls back to the default.  A half-set
	 * one (1920x0) is a typo, not a request, and must not silently widen
	 * into a mode the caller did not ask for. */
	if (width == 0 && height == 0) {
		width = g_cv610_modes[0].width;
		height = g_cv610_modes[0].height;
	}
	for (i = 0; i < CV610_MODE_COUNT; ++i) {
		if (g_cv610_modes[i].width == width &&
			g_cv610_modes[i].height == height &&
			g_cv610_modes[i].fps == fps)
			return &g_cv610_modes[i];
	}
	return NULL;
}
