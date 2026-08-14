#ifndef CV610_MODES_H
#define CV610_MODES_H

#include <stddef.h>
#include <stdint.h>

/* One sensor mode of the CV610 bring-up graph.
 *
 * Mirrors g_imx662_mode_tbl in sensors/cv610/imx662/imx662_cmos.c, which the
 * sensor plugin keeps to itself — nothing in the MPP API reports a mode's
 * bit depth or its expected input clock, and both are needed before the
 * sensor is even probed.  Config validation, pipeline setup, and
 * /api/v1/modes all read this table so the four facts cannot drift apart. */
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

/* Resolve a requested geometry to a mode.  A zero width AND height mean
 * "whatever the first entry is", matching video0.size=auto.  Returns NULL
 * when no mode matches, which is the only rejection rule the CV610 backend
 * applies to video0.size and video0.fps. */
const Cv610SensorMode *cv610_mode_lookup(uint32_t width, uint32_t height,
	uint32_t fps);

#endif /* CV610_MODES_H */
