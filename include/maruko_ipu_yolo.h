#ifndef MARUKO_IPU_YOLO_H
#define MARUKO_IPU_YOLO_H

#include "detect_plugin.h"
#include "maruko_pipeline.h"
#include "venc_config.h"

#include <stdint.h>

#define MARUKO_DETECT_SNAP_MAX 64

typedef struct {
	DetectBox boxes[MARUKO_DETECT_SNAP_MAX];
	int count;
	uint32_t seq;
	uint64_t produced_us;
	uint16_t net_w;
	uint16_t net_h;
	uint16_t model_id;
} MarukoDetectSnapshot;

int maruko_ipu_yolo_start(MarukoBackendContext *ctx,
	const VencConfig *vcfg);
int maruko_ipu_yolo_reload(MarukoBackendContext *ctx,
	const VencConfig *vcfg);
void maruko_ipu_yolo_stop(MarukoBackendContext *ctx);
int maruko_ipu_yolo_snapshot(MarukoBackendContext *ctx,
	MarukoDetectSnapshot *out);

#endif
