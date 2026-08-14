#ifndef CV610_MODES_H
#define CV610_MODES_H

#include <stddef.h>
#include <stdint.h>

/* One sensor mode of the CV610 graph.
 *
 * Mirrors g_imx662_mode_tbl in sensors/cv610/imx662/imx662_cmos.c, which the
 * sensor plugin keeps to itself — nothing in the MPP API reports a mode's
 * bit depth or its expected input clock, and both are needed before the
 * sensor is even probed.  Config validation, pipeline setup, and
 * /api/v1/modes all read this table so the four facts cannot drift apart.
 *
 * width/height here are what the SENSOR captures.  The encoded size is
 * video0.size and is reached by scaling in VPSS, so the two are independent
 * — see cv610_mode_check_output(). */
typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t fps;
	uint32_t raw_bit;          /* MIPI RAW bit depth carried by this mode */
	uint32_t sensor_clock_hz;  /* MCLK the mode's line timing assumes */
	const char *desc;          /* /api/v1/modes label */
} Cv610SensorMode;

/* The mode table, in advertised (index) order.  Never NULL. */
const Cv610SensorMode *cv610_mode_table(size_t *count);

/* The sensor mode for a requested frame rate, or NULL if there is none.
 * Frame rate alone selects the mode: every entry captures the same geometry,
 * and any smaller encoded size is produced by scaling rather than by a
 * different capture. */
const Cv610SensorMode *cv610_mode_for_fps(uint32_t fps);

/* Whether an encoded geometry can be produced from this mode.  Returns NULL
 * when it can, or a static reason string.  A zero width AND height mean
 * "the mode's own size" and always pass. */
const char *cv610_mode_check_output(const Cv610SensorMode *mode,
	uint32_t width, uint32_t height);

/* The encoded geometry to use, resolving 0x0 to the mode's own size. */
void cv610_mode_resolve_output(const Cv610SensorMode *mode,
	uint32_t req_width, uint32_t req_height,
	uint32_t *out_width, uint32_t *out_height);

#endif /* CV610_MODES_H */
