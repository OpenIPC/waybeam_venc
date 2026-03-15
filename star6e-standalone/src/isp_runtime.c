#include "isp_runtime.h"

#include "file_util.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void isp_runtime_lib_reset(IspRuntimeLib *lib)
{
	if (!lib)
		return;

	memset(lib, 0, sizeof(*lib));
}

int isp_runtime_open(IspRuntimeLib *lib, const char *log_prefix)
{
	const char *prefix = log_prefix ? log_prefix : "";

	if (!lib)
		return -1;

	isp_runtime_lib_reset(lib);
	lib->handle = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!lib->handle) {
		fprintf(stderr, "ERROR: %sunable to load libmi_isp.so (%s)\n", prefix,
			dlerror());
		return -1;
	}

	lib->disable_userspace3a = dlsym(lib->handle,
		"MI_ISP_DisableUserspace3A");
	lib->load_bin_api = dlsym(lib->handle, "MI_ISP_API_CmdLoadBinFile");
	lib->load_bin_api_alt = dlsym(lib->handle, "MI_ISP_ApiCmdLoadBinFile");
	lib->cus3a_enable = dlsym(lib->handle, "MI_ISP_CUS3A_Enable");
	if (!lib->load_bin_api && !lib->load_bin_api_alt) {
		fprintf(stderr, "ERROR: %sISP bin loader symbol not found in libmi_isp.so\n",
			prefix);
		isp_runtime_close(lib);
		return -1;
	}

	return 0;
}

void isp_runtime_close(IspRuntimeLib *lib)
{
	if (!lib)
		return;

	if (lib->handle)
		dlclose(lib->handle);
	isp_runtime_lib_reset(lib);
}

static void isp_runtime_quiet_begin(const IspRuntimeLoadHooks *hooks)
{
	if (hooks && hooks->quiet_begin)
		hooks->quiet_begin(hooks->ctx);
}

static void isp_runtime_quiet_end(const IspRuntimeLoadHooks *hooks)
{
	if (hooks && hooks->quiet_end)
		hooks->quiet_end(hooks->ctx);
}

int isp_runtime_load_bin_file(const char *isp_bin_path,
	const IspRuntimeLoadHooks *hooks)
{
	const char *prefix = hooks && hooks->log_prefix ? hooks->log_prefix : "";
	IspRuntimeLib lib;
	int ret;

	if (!isp_bin_path || !*isp_bin_path)
		return 0;

	if (file_util_validate_regular_file(isp_bin_path, "ISP bin", prefix) != 0)
		return -1;

	if (isp_runtime_open(&lib, prefix) != 0)
		return -1;

	printf("> %sLoading ISP file: %s\n", prefix, isp_bin_path);
	isp_runtime_quiet_begin(hooks);
	if (hooks && hooks->disable_userspace3a && lib.disable_userspace3a) {
		ret = hooks->disable_userspace3a(&lib, hooks->ctx);
		if (ret != 0) {
			isp_runtime_quiet_end(hooks);
			fprintf(stderr, "WARNING: %sMI_ISP_DisableUserspace3A failed %d\n",
				prefix, ret);
			isp_runtime_quiet_begin(hooks);
		}
	}

	if (hooks && hooks->wait_ready) {
		ret = hooks->wait_ready(&lib, hooks->ctx);
		if (ret != 0)
			fprintf(stderr, "WARNING: %sISP readiness wait failed, proceeding anyway\n",
				prefix);
	} else {
		usleep(20 * 1000);
	}
	ret = hooks && hooks->load_bin
		? hooks->load_bin(&lib, isp_bin_path,
			hooks->load_key, hooks->ctx)
		: -1;
	if (hooks && hooks->post_load && lib.cus3a_enable)
		hooks->post_load(&lib, hooks->ctx);
	isp_runtime_quiet_end(hooks);

	isp_runtime_close(&lib);
	if (ret != 0) {
		fprintf(stderr, "ERROR: %sMI_ISP_*CmdLoadBinFile failed %d for %s\n",
			prefix, ret, isp_bin_path);
		return ret;
	}

	printf("> %sISP file loaded successfully: %s\n", prefix, isp_bin_path);
	return 0;
}
