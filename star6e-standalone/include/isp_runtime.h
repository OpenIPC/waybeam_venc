#ifndef ISP_RUNTIME_H
#define ISP_RUNTIME_H

#include <stddef.h>

typedef struct {
	void *handle;
	void *disable_userspace3a;
	void *load_bin_api;
	void *load_bin_api_alt;
	void *cus3a_enable;
} IspRuntimeLib;

typedef struct {
	const char *log_prefix;
	unsigned int load_key;
	void *ctx;
	void (*quiet_begin)(void *ctx);
	void (*quiet_end)(void *ctx);
	int (*disable_userspace3a)(const IspRuntimeLib *lib, void *ctx);
	int (*wait_ready)(const IspRuntimeLib *lib, void *ctx);
	int (*load_bin)(const IspRuntimeLib *lib, const char *path,
		unsigned int load_key, void *ctx);
	void (*post_load)(const IspRuntimeLib *lib, void *ctx);
} IspRuntimeLoadHooks;

/** Reset ISP runtime library handle to unloaded state. */
void isp_runtime_lib_reset(IspRuntimeLib *lib);

/** Load and resolve ISP runtime library symbols. */
int isp_runtime_open(IspRuntimeLib *lib, const char *log_prefix);

/** Unload ISP runtime library. */
void isp_runtime_close(IspRuntimeLib *lib);

/** Load ISP binary tuning file via platform-specific hooks. */
int isp_runtime_load_bin_file(const char *isp_bin_path,
	const IspRuntimeLoadHooks *hooks);

#endif /* ISP_RUNTIME_H */
