#include "maruko_runtime.h"

const BackendOps *backend_get_selected(void)
{
	return maruko_runtime_backend_ops();
}
