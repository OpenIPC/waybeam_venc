#ifndef CV610_PIPELINE_H
#define CV610_PIPELINE_H

#include <stdint.h>

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t fps;
	int lanes;
	int data_rate_x2;
	int bayer;
	int raw_bit;
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
