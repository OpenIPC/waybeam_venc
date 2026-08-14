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

/* VPSS scales, it does not invent detail: upscaling past the captured size
 * costs encoder bandwidth for no information.  Refuse it rather than let a
 * config quietly waste the link. */
#define CV610_OUT_ALIGN 8u
#define CV610_OUT_MIN   128u

const Cv610SensorMode *cv610_mode_for_fps(uint32_t fps)
{
	size_t i;

	for (i = 0; i < CV610_MODE_COUNT; ++i) {
		if (g_cv610_modes[i].fps == fps)
			return &g_cv610_modes[i];
	}
	return NULL;
}

const char *cv610_mode_check_output(const Cv610SensorMode *mode,
	uint32_t width, uint32_t height)
{
	if (mode == NULL)
		return "no sensor mode selected";
	/* Only a fully unset geometry means "the mode's own size".  A half-set
	 * one (1920x0) is a typo, not a request, and must not be completed into
	 * something the caller did not ask for. */
	if (width == 0 && height == 0)
		return NULL;
	if (width == 0 || height == 0)
		return "CV610 video0.size needs both a width and a height";
	if (width % CV610_OUT_ALIGN != 0 || height % CV610_OUT_ALIGN != 0)
		return "CV610 video0.size must be a multiple of 8";
	if (width < CV610_OUT_MIN || height < CV610_OUT_MIN)
		return "CV610 video0.size must be at least 128x128";
	if (width > mode->width || height > mode->height)
		return "CV610 video0.size cannot exceed the sensor mode's capture size";
	return NULL;
}

void cv610_mode_resolve_output(const Cv610SensorMode *mode,
	uint32_t req_width, uint32_t req_height,
	uint32_t *out_width, uint32_t *out_height)
{
	uint32_t w = 0;
	uint32_t h = 0;

	if (mode != NULL) {
		int given = (req_width != 0 && req_height != 0);

		w = given ? req_width : mode->width;
		h = given ? req_height : mode->height;
	}
	if (out_width)
		*out_width = w;
	if (out_height)
		*out_height = h;
}
