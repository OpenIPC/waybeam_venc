#ifndef CV610_PIPELINE_H
#define CV610_PIPELINE_H

#include <stdint.h>

typedef struct {
	/* Sensor capture geometry (what VI and the ISP see). */
	uint32_t width;
	uint32_t height;
	/* Encoded geometry (what VPSS scales to and VENC encodes).  Equal to
	 * the capture size unless video0.size asks for something smaller. */
	uint32_t out_width;
	uint32_t out_height;
	/* isp.keepAspect: centre-crop the capture to the encoded aspect ratio
	 * before scaling, so a 4:3 video0.size out of a 16:9 sensor is framed
	 * rather than squashed.  Same rule Star6E and Maruko apply. */
	int keep_aspect;
	uint32_t fps;
	int lanes;
	int data_rate_x2;
	int bayer;
	int raw_bit;
	/* MCLK this mode's sensor line timing assumes.  Zero leaves whatever
	 * the loader set, which suits exactly one mode. */
	uint32_t sensor_clock_hz;
	int vi_online;
	int i2c_bus;
} Cv610PipelineConfig;

/* Start the hardware-verified IMX662 MIPI -> VI -> ISP graph. */
int cv610_pipeline_start(const Cv610PipelineConfig *cfg);

/* Stop the ISP thread and release sensor, VI, SYS, and VB resources. */
void cv610_pipeline_stop(void);

/* Signal-safe stop flag used by the backend's SIGINT/SIGTERM handler. */
void cv610_pipeline_request_stop(void);
int cv610_pipeline_stop_requested(void);

#endif /* CV610_PIPELINE_H */
