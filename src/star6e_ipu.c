#include "star6e_ipu.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dlfcn.h>

Star6eIpuImpl g_mi_ipu;

static void *ipu_symbol(void *handle, const char *sym)
{
	void *addr = dlsym(handle, sym);
	if (!addr)
		fprintf(stderr, "[ipu] missing symbol %s: %s\n", sym, dlerror());
	return addr;
}

int star6e_ipu_load(void)
{
	memset(&g_mi_ipu, 0, sizeof(g_mi_ipu));

	/* libmi_ipu.so calls CamOs* from libcam_os_wrapper.so but is not
	 * linked against it (vendor apps link both).  Pull the wrapper in
	 * first with GLOBAL visibility; ignore failure — the host process
	 * (e.g. waybeam) may already have it loaded via the MI libs. */
	g_mi_ipu.cam_os_handle = dlopen("libcam_os_wrapper.so",
		RTLD_LAZY | RTLD_GLOBAL);
	g_mi_ipu.cam_fs_handle = dlopen("libcam_fs_wrapper.so",
		RTLD_LAZY | RTLD_GLOBAL);
	g_mi_ipu.mi_sys_handle = dlopen("libmi_sys.so",
		RTLD_LAZY | RTLD_GLOBAL);
	if (g_mi_ipu.mi_sys_handle) {
		g_mi_ipu.fnSysInit = (int (*)(unsigned short))
			dlsym(g_mi_ipu.mi_sys_handle, "MI_SYS_Init");
		g_mi_ipu.fnSysConfigPool = (int (*)(void *))
			dlsym(g_mi_ipu.mi_sys_handle,
			      "MI_SYS_ConfigPrivateMMAPool");
	}

	g_mi_ipu.handle = dlopen("libmi_ipu.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!g_mi_ipu.handle) {
		fprintf(stderr, "[ipu] dlopen libmi_ipu.so failed: %s\n", dlerror());
		return -1;
	}

	dlerror(); /* clear stale error state before the symbol sweep */

	g_mi_ipu.fnCreateDevice = (int (*)(IpuDevAttr *, IpuReadFn, char *,
		unsigned int))ipu_symbol(g_mi_ipu.handle, "MI_IPU_CreateDevice");
	g_mi_ipu.fnDestroyDevice = (int (*)(void))
		ipu_symbol(g_mi_ipu.handle, "MI_IPU_DestroyDevice");
	g_mi_ipu.fnCreateChannel = (int (*)(unsigned int *, IpuChnAttr *,
		IpuReadFn, char *))ipu_symbol(g_mi_ipu.handle, "MI_IPU_CreateCHN");
	g_mi_ipu.fnDestroyChannel = (int (*)(unsigned int))
		ipu_symbol(g_mi_ipu.handle, "MI_IPU_DestroyCHN");
	g_mi_ipu.fnGetIODesc = (int (*)(unsigned int, IpuTensorIODesc *))
		ipu_symbol(g_mi_ipu.handle, "MI_IPU_GetInOutTensorDesc");
	g_mi_ipu.fnGetInputTensors = (int (*)(unsigned int, IpuTensorVector *))
		ipu_symbol(g_mi_ipu.handle, "MI_IPU_GetInputTensors");
	g_mi_ipu.fnGetOutputTensors = (int (*)(unsigned int, IpuTensorVector *))
		ipu_symbol(g_mi_ipu.handle, "MI_IPU_GetOutputTensors");
	g_mi_ipu.fnPutInputTensors = (int (*)(unsigned int, IpuTensorVector *))
		ipu_symbol(g_mi_ipu.handle, "MI_IPU_PutInputTensors");
	g_mi_ipu.fnPutOutputTensors = (int (*)(unsigned int, IpuTensorVector *))
		ipu_symbol(g_mi_ipu.handle, "MI_IPU_PutOutputTensors");
	g_mi_ipu.fnInvoke = (int (*)(unsigned int, IpuTensorVector *,
		IpuTensorVector *))ipu_symbol(g_mi_ipu.handle, "MI_IPU_Invoke");
	g_mi_ipu.fnGetOfflineInfo = (int (*)(IpuReadFn, char *,
		IpuOfflineInfo *))ipu_symbol(g_mi_ipu.handle,
		"MI_IPU_GetOfflineModeStaticInfo");

	if (!g_mi_ipu.fnCreateDevice || !g_mi_ipu.fnDestroyDevice ||
	    !g_mi_ipu.fnCreateChannel || !g_mi_ipu.fnDestroyChannel ||
	    !g_mi_ipu.fnGetIODesc || !g_mi_ipu.fnGetInputTensors ||
	    !g_mi_ipu.fnGetOutputTensors || !g_mi_ipu.fnPutInputTensors ||
	    !g_mi_ipu.fnPutOutputTensors || !g_mi_ipu.fnInvoke ||
	    !g_mi_ipu.fnGetOfflineInfo) {
		star6e_ipu_unload();
		return -1;
	}

	return 0;
}

void star6e_ipu_unload(void)
{
	if (g_mi_ipu.handle)
		dlclose(g_mi_ipu.handle);
	if (g_mi_ipu.cam_os_handle)
		dlclose(g_mi_ipu.cam_os_handle);
	if (g_mi_ipu.cam_fs_handle)
		dlclose(g_mi_ipu.cam_fs_handle);
	if (g_mi_ipu.mi_sys_handle)
		dlclose(g_mi_ipu.mi_sys_handle);
	memset(&g_mi_ipu, 0, sizeof(g_mi_ipu));
}

int star6e_ipu_scrub(void)
{
	IpuDevAttr dev;
	struct timespec t0, t1;
	int loaded_here = 0;
	long ms;

	/* No mi_ipu.ko loaded → nothing to scrub (and nothing that could
	 * have been poisoned).  Gate on the device node so non-NPU boxes
	 * stay silent instead of logging a dlopen failure every boot. */
	if (access("/dev/mi_ipu", F_OK) != 0)
		return 0;

	clock_gettime(CLOCK_MONOTONIC, &t0);

	if (!g_mi_ipu.handle) {
		if (star6e_ipu_load() != 0)
			return -1;
		loaded_here = 1;
	}

	/* Idempotent when the host already ran MI_SYS_Init. */
	if (g_mi_ipu.fnSysInit)
		g_mi_ipu.fnSysInit(0);

	/* The var-buf size only bounds this throwaway device's dynamic
	 * allocations — any non-zero value is accepted; no network is
	 * loaded through it. */
	memset(&dev, 0, sizeof(dev));
	dev.max_dyn_buf_size = 0x400000;
	dev.yuv420_walign = 16;
	dev.yuv420_halign = 2;
	dev.rgb_walign = 16;

	if (g_mi_ipu.fnCreateDevice(&dev, NULL, NULL, 0) != 0) {
		fprintf(stderr, "[ipu-scrub] CreateDevice failed — skipped\n");
		if (loaded_here)
			star6e_ipu_unload();
		return -1;
	}
	if (g_mi_ipu.fnDestroyDevice() != 0)
		fprintf(stderr, "[ipu-scrub] DestroyDevice failed — throwaway "
			"device leaks until exit; a later detector start may "
			"fail its own CreateDevice\n");

	if (loaded_here)
		star6e_ipu_unload();

	clock_gettime(CLOCK_MONOTONIC, &t1);
	ms = (t1.tv_sec - t0.tv_sec) * 1000 +
		(t1.tv_nsec - t0.tv_nsec) / 1000000;
	fprintf(stderr, "[ipu-scrub] NPU driver state reconciled (%ld ms)\n", ms);
	return 0;
}

const char *star6e_ipu_fmt_name(IpuFmt fmt)
{
	switch (fmt) {
	case IPU_FMT_U8:       return "u8";
	case IPU_FMT_NV12:     return "nv12";
	case IPU_FMT_INT16:    return "int16";
	case IPU_FMT_INT32:    return "int32";
	case IPU_FMT_INT8:     return "int8";
	case IPU_FMT_FP32:     return "fp32";
	case IPU_FMT_ARGB8888: return "argb8888";
	case IPU_FMT_ABGR8888: return "abgr8888";
	case IPU_FMT_UNKNOWN:  return "unknown";
	}
	return "invalid";
}
