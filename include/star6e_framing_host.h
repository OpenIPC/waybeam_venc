#ifndef STAR6E_FRAMING_HOST_H
#define STAR6E_FRAMING_HOST_H

/* Host services that the framing modules (src/star6e_framing_*.c) call back
 * into the core pipeline.  Kept deliberately narrow — a framing module sees
 * only these, never the pipeline's other statics. */

#include <stdint.h>
#include "imu_ring.h"

/* MI_ISP_CUS3A_SetAECropSize confines the AE meter to a sub-rect of the ISP
 * frame in 0..1023 normalized coords. */
typedef struct {
	uint16_t crop_x;  /* 0..1023 */
	uint16_t crop_y;
	uint16_t crop_w;
	uint16_t crop_h;
} Star6eAeCropRect;

/* Push an AE meter rect to the ISP (silent no-op if CUS3A is unavailable). */
void star6e_emit_ae_crop(const Star6eAeCropRect *r);

/* Pipeline-owned IMU sample ring; NULL until imu.enabled init has run.  A
 * framing module's motion estimator may read it frame-aligned. */
ImuRing *star6e_pipeline_imu_ring(void);

#endif /* STAR6E_FRAMING_HOST_H */
