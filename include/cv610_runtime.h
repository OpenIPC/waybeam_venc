#ifndef CV610_RUNTIME_H
#define CV610_RUNTIME_H

#include "backend.h"

#include <stdint.h>

typedef struct {
	char mode_name[16];
	int active;
	int mi_supported; /* Kept for status-JSON parity; means MPP API present. */
	int apply_ok;
	uint32_t target_ms;
	uint32_t total_rows;
	uint32_t requested_lines;
	uint32_t effective_lines_per_p;
	int lines_clamped;
	uint32_t requested_qp;
	uint32_t effective_qp;
	double explicit_gop_sec;
	double effective_gop_sec;
	int gop_auto;
} Cv610IntraRefreshStatus;

typedef struct {
	int active;
	int mi_supported; /* Kept for status-JSON parity; means MPP API present. */
	int apply_ok;
	uint32_t base;
	uint32_t enhance;
	int pred;
} Cv610RefPredStatus;

const BackendOps *cv610_runtime_backend_ops(void);
void cv610_runtime_intra_refresh_status(Cv610IntraRefreshStatus *out);
void cv610_runtime_ref_pred_status(Cv610RefPredStatus *out);

#endif /* CV610_RUNTIME_H */
