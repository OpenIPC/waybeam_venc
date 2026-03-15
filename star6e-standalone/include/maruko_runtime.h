#ifndef MARUKO_RUNTIME_H
#define MARUKO_RUNTIME_H

#include "backend.h"

/** Return the Maruko backend operations table. */
const BackendOps *maruko_runtime_backend_ops(void);

#endif /* MARUKO_RUNTIME_H */
