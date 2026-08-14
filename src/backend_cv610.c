#include "backend.h"
#include "cv610_runtime.h"

const BackendOps *backend_get_selected(void)
{
	return cv610_runtime_backend_ops();
}
