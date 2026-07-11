/* ive_i6c_probe.c — minimal go/no-go for MI_IVE_Create on Maruko/i6c.
 *
 * Recreated 2026-07-07 to test the BSP-matched libmi_ive.so blob-swap
 * hypothesis (see memory maruko_ive_hardware_blocked). The ONLY question this
 * asks: does MI_IVE_Create(0) succeed? That is the exact call that failed under
 * OpenIPC's older blob (d10fcfb, raw /dev/mem RIU-mmap IC check). The matched
 * BSP blob (c6a1e30) does the IC check via /dev/mstar_ive0 instead.
 *
 * dlopen-only, no vendor headers. Preloads libmi_common + libmi_sys
 * (RTLD_GLOBAL) so libmi_ive's MI_SYS/MI_common symbols resolve, then dlopens
 * libmi_ive by explicit path and calls MI_IVE_Create/Destroy.
 *
 * Usage: ive_i6c_probe <dir-with-the-three-libs>
 *   e.g. ive_i6c_probe /tmp/ive
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>

typedef int            MI_S32;
typedef unsigned short MI_U16;
typedef int MI_IVE_HANDLE;              /* handle passed by value */

typedef MI_S32 (*create_fn)(MI_IVE_HANDLE);
typedef MI_S32 (*destroy_fn)(MI_IVE_HANDLE);
/* i6c/Maruko: MI_SYS_Init takes a u16 soc_id. cf. maruko_mi.h:65
 * `int (*fnInit)(uint16_t soc_id)` and star6e.h:156
 * `#define MI_SYS_Init() g_mi_sys.fnInit(0)`. Calling it through a
 * void-signature pointer leaves r0 undefined -> junk soc_id -> ioctl EINVAL. */
typedef MI_S32 (*sys_init_fn)(MI_U16);
typedef MI_S32 (*sys_exit_fn)(MI_U16);

static void *load_global(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    void *h = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
    if (!h) fprintf(stderr, "[preload] dlopen %s FAILED: %s\n", path, dlerror());
    else    fprintf(stderr, "[preload] dlopen %s ok\n", path);
    return h;
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : "/tmp/ive";
    char path[512];

    /* Preload libmi_common/libmi_sys (RTLD_GLOBAL) so libmi_ive can bind its
     * MI_SYS_* / MI_common symbols. Default to the DEVICE's own MI stack in
     * /usr/lib — those are the musl-compatible builds the rest of the system
     * already runs on. (The BSP's uClibc libmi_sys segfaults inside
     * MI_SYS_Init on a musl rootfs.) libmi_ive only needs 5 stable MI_SYS MMA
     * symbols, so mixing the BSP IVE blob with the device MI stack is exactly
     * the intended production swap. Override with MI_SYS_DIR. */
    const char *sysdir = getenv("MI_SYS_DIR");
    if (!sysdir) sysdir = "/usr/lib";
    fprintf(stderr, "[cfg] mi-stack dir=%s   libmi_ive dir=%s\n", sysdir, dir);
    /* Order matters: libmi_sys has undefined CamOsTsem* which live in
     * libcam_os_wrapper.so. Under RTLD_LAZY they resolve on first call — i.e.
     * inside MI_SYS_Init — and segfault if the wrapper was never loaded. */
    load_global(sysdir, "libcam_os_wrapper.so");
    load_global(sysdir, "libmi_common.so");
    void *sys = load_global(sysdir, "libmi_sys.so");

    /* libmi_ive's only external deps are MI_SYS MMA/mmap helpers, which need
     * the MI system brought up first — otherwise the first MMA call inside
     * MI_IVE_Create null-derefs (observed: SIGSEGV). */
    sys_init_fn SysInit = sys ? (sys_init_fn) dlsym(sys, "MI_SYS_Init") : NULL;
    sys_exit_fn SysExit = sys ? (sys_exit_fn) dlsym(sys, "MI_SYS_Exit") : NULL;
    if (!SysInit) {
        fprintf(stderr, "[sys] MI_SYS_Init not found — continuing unguarded\n");
    } else {
        fprintf(stderr, "[sys] MI_SYS_Init=%p — calling...\n", (void*)SysInit);
        MI_S32 sret = SysInit(0);
        fprintf(stderr, "[sys] MI_SYS_Init(0) -> %d (0x%08x)\n", sret, (unsigned)sret);
        if (sret != 0)
            fprintf(stderr, "[sys] WARNING: MI_SYS_Init failed; MI_IVE_Create likely to fault\n");
    }

    snprintf(path, sizeof(path), "%s/libmi_ive.so", dir);
    dlerror();
    void *ive = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
    if (!ive) {
        fprintf(stderr, "IVE_PROBE_JSON {\"stage\":\"dlopen_ive\",\"ok\":false,\"err\":\"%s\"}\n",
                dlerror());
        return 2;
    }
    fprintf(stderr, "[ive] dlopen %s ok\n", path);

    create_fn  Create  = (create_fn)  dlsym(ive, "MI_IVE_Create");
    destroy_fn Destroy = (destroy_fn) dlsym(ive, "MI_IVE_Destroy");
    if (!Create) {
        fprintf(stderr, "IVE_PROBE_JSON {\"stage\":\"dlsym\",\"ok\":false,\"err\":\"MI_IVE_Create missing\"}\n");
        return 3;
    }
    fprintf(stderr, "[ive] MI_IVE_Create=%p MI_IVE_Destroy=%p\n",
            (void*)Create, (void*)Destroy);

    errno = 0;
    MI_S32 ret = Create(0);
    int e = errno;
    fprintf(stderr, "[ive] MI_IVE_Create(0) -> %d (0x%08x) errno=%d (%s)\n",
            ret, (unsigned)ret, e, strerror(e));

    int ok = (ret == 0);
    if (ok && Destroy) {
        MI_S32 dret = Destroy(0);
        fprintf(stderr, "[ive] MI_IVE_Destroy(0) -> %d\n", dret);
    }
    if (SysExit) {
        MI_S32 xret = SysExit(0);
        fprintf(stderr, "[sys] MI_SYS_Exit(0) -> %d\n", xret);
    }

    printf("IVE_PROBE_JSON {\"stage\":\"create\",\"ok\":%s,\"ret\":%d,\"ret_hex\":\"0x%08x\",\"errno\":%d}\n",
           ok ? "true" : "false", ret, (unsigned)ret, e);
    return ok ? 0 : 1;
}
