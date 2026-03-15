#include "star6e_runtime.h"

const BackendOps *backend_get_selected(void)
{
	return star6e_runtime_backend_ops();
}
