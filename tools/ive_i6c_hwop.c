/* ive_i6c_hwop.c — drive the i6c IVE HARDWARE block directly via
 * /dev/mstar_ive0, bypassing libmi_ive.so entirely.
 *
 * Context: MI_IVE_Shift_Detector is a NEON *software* path (the "MVE" module
 * inside libmi_ive.so is SigmaStar's Simd-based CPU vision lib, not a Motion
 * Vector Engine). But the SoC really does have an IVE hardware block with ~27
 * per-pixel/filter/statistics ops, reached through exactly one ioctl:
 *
 *     IVE_IOC_PROCESS = _IOW('I', 1, ive_ioc_config*) = 0x40044901
 *         drivers/sstar/include/mdrv_ive_io.h:20-21
 *
 * This tool proves the HW path works on Maruko and measures per-op cost, using
 * THRESH (op 0x09) on a known gradient so the result is verifiable. Buffers are
 * MMA-allocated (the driver takes raw MIU physicals: USE_MIU_DIRECT,
 * mdrv_ive.h:99 / hal_ive.c:239-241).
 *
 * Check /proc/interrupts "ive isr" before/after: it MUST increment, unlike the
 * Shift_Detector path.
 *
 * Usage: ive_i6c_hwop [lib-dir]     (default /usr/lib)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <dlfcn.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <stddef.h>

typedef int                MI_S32;
typedef unsigned char      MI_U8;
typedef unsigned short     MI_U16;
typedef unsigned int       MI_U32;
typedef unsigned long long MI_U64;
typedef unsigned long long MI_PHY;
typedef unsigned char      MI_BOOL;

typedef MI_S32 (*sys_init_fn)(MI_U16);
typedef MI_S32 (*sys_exit_fn)(MI_U16);
typedef MI_S32 (*mma_alloc_fn)(MI_U16, MI_U8 *, MI_U32, MI_PHY *);
typedef MI_S32 (*mma_free_fn)(MI_U16, MI_PHY);
typedef MI_S32 (*mmap_fn)(MI_U64, MI_U32, void **, MI_BOOL);
typedef MI_S32 (*munmap_fn)(void *, MI_U32);
typedef MI_S32 (*flushinv_fn)(void *, MI_U32);

static mma_alloc_fn MmaAlloc; static mma_free_fn MmaFree;
static mmap_fn SysMmap;       static munmap_fn SysMunmap;
static flushinv_fn FlushInv;

/* ── kernel ABI (drivers/sstar/include/mdrv_ive_io_st.h) ─────────────────── */
#define IVE_IOC_PROCESS            0x40044901u
#define IVE_IOC_OP_TYPE_THRESH     0x09
#define IVE_IOC_IMAGE_FORMAT_B8C1  0x00
#define IVE_IOC_MODE_THRESH_BINARY 0x00

/* ive_ioc_image: format@0 w@4 h@6 address[3]@8 stride[3]@32  => sizeof 40 */
typedef struct {
	int      format;
	uint16_t width;
	uint16_t height;
	uint64_t address[3];
	uint16_t stride[3];
} ive_img;

/* ive_ioc_config: op_type@0 input@8 output@48 union@88.
 * coeff[] is oversized on purpose: the driver does
 *   copy_from_user(&cfg, arg, sizeof(ive_ioc_config))   (mdrv_ive.c:437)
 * using the KERNEL's sizeof, so our buffer must be >= that. Over-allocating is
 * safe; under-allocating would fault. */
typedef struct {
	int     op_type;
	ive_img input;
	ive_img output;
	unsigned char coeff[256];
} ive_cfg;

#define ITERS 100
static int W=384,H=384;

static void *load_global(const char *dir, const char *n) {
	char p[512]; snprintf(p, sizeof(p), "%s/%s", dir, n);
	return dlopen(p, RTLD_LAZY | RTLD_GLOBAL);
}

static int mma_img(MI_PHY *phy, void **vir, uint32_t size) {
	*phy = 0; *vir = NULL;
	if (MmaAlloc(0, NULL, size, phy) != 0 || !*phy) {
		if (MmaAlloc(0, (MI_U8 *)"mma_heap_name0", size, phy) != 0 || !*phy) return -1;
	}
	if (SysMmap(*phy, size, vir, 0) != 0 || !*vir) { MmaFree(0, *phy); return -1; }
	return 0;
}

static double now_ms(void) {
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static double cpu_ms(void) {
	struct timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static long ive_irq_count(void) {
	FILE *f = fopen("/proc/interrupts", "r");
	if (!f) return -1;
	char line[512]; long v = -1;
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "ive isr")) {
			char *p = strchr(line, ':');
			if (p) v = strtol(p + 1, NULL, 10);
			break;
		}
	}
	fclose(f);
	return v;
}

/* Run ITERS THRESH ops at w x h. Returns wall ms/op; sets *cpu and *bad. */
static double run_size(int fd, int w, int h, double *cpu_out, int *bad_out, long *irq_delta)
{
	MI_PHY sphy = 0, dphy = 0; void *svir = NULL, *dvir = NULL;
	uint32_t sz = (uint32_t)w * h;
	*bad_out = -1; *cpu_out = 0; *irq_delta = 0;
	if (mma_img(&sphy, &svir, sz) != 0) return -1;
	if (mma_img(&dphy, &dvir, sz) != 0) { SysMunmap(svir, sz); MmaFree(0, sphy); return -1; }

	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			((unsigned char *)svir)[y * w + x] = (unsigned char)(x & 0xff);
	memset(dvir, 0xAA, sz);
	FlushInv(svir, sz);

	ive_cfg cfg; memset(&cfg, 0, sizeof(cfg));
	cfg.op_type = IVE_IOC_OP_TYPE_THRESH;
	cfg.input.format = IVE_IOC_IMAGE_FORMAT_B8C1;
	cfg.input.width = (uint16_t)w; cfg.input.height = (uint16_t)h;
	cfg.input.address[0] = sphy;   cfg.input.stride[0] = (uint16_t)w;
	cfg.output = cfg.input;
	cfg.output.address[0] = dphy;  cfg.output.stride[0] = (uint16_t)w;
	*(int *)     (cfg.coeff + 0) = IVE_IOC_MODE_THRESH_BINARY;
	*(uint16_t *)(cfg.coeff + 4) = 128;
	*(uint16_t *)(cfg.coeff + 6) = 128;
	cfg.coeff[8] = 0; cfg.coeff[9] = 0; cfg.coeff[10] = 255;

	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	long irqA = ive_irq_count();
	double t0 = now_ms(), c0 = cpu_ms();
	for (int i = 0; i < ITERS; i++) {
		if (ioctl(fd, IVE_IOC_PROCESS, &cfg) != 0) { *bad_out = -2; break; }
		poll(&pfd, 1, 500);
	}
	double t1 = now_ms(), c1 = cpu_ms();
	*irq_delta = ive_irq_count() - irqA;

	FlushInv(dvir, sz);
	int bad = 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			unsigned char in = (unsigned char)(x & 0xff);
			unsigned char exp = (in > 128) ? 255 : 0;
			if (((unsigned char *)dvir)[y * w + x] != exp) bad++;
		}
	if (*bad_out != -2) *bad_out = bad;
	*cpu_out = (c1 - c0) / ITERS;

	SysMunmap(svir, sz); MmaFree(0, sphy);
	SysMunmap(dvir, sz); MmaFree(0, dphy);
	return (t1 - t0) / ITERS;
}

int main(int argc, char **argv)
{
	const char *dir = (argc > 1) ? argv[1] : "/usr/lib";
	load_global(dir, "libcam_os_wrapper.so");
	load_global(dir, "libmi_common.so");
	void *sys = load_global(dir, "libmi_sys.so");
	if (!sys) { fprintf(stderr, "no libmi_sys\n"); return 2; }
	sys_init_fn SysInit = (sys_init_fn) dlsym(sys, "MI_SYS_Init");
	sys_exit_fn SysExit = (sys_exit_fn) dlsym(sys, "MI_SYS_Exit");
	MmaAlloc = (mma_alloc_fn) dlsym(sys, "MI_SYS_MMA_Alloc");
	MmaFree  = (mma_free_fn)  dlsym(sys, "MI_SYS_MMA_Free");
	SysMmap  = (mmap_fn)      dlsym(sys, "MI_SYS_Mmap");
	SysMunmap= (munmap_fn)    dlsym(sys, "MI_SYS_Munmap");
	FlushInv = (flushinv_fn)  dlsym(sys, "MI_SYS_FlushInvCache");
	if (!SysInit || !MmaAlloc) { fprintf(stderr, "missing MI_SYS syms\n"); return 3; }
	if (SysInit(0) != 0) { fprintf(stderr, "MI_SYS_Init failed\n"); return 4; }

	int fd = open("/dev/mstar_ive0", O_RDWR);
	if (fd < 0) { perror("open /dev/mstar_ive0"); return 6; }

	static const int sizes[][2] = {
		{64,64},{128,128},{256,256},{384,384},{640,480},{1280,720},{1920,1080}
	};
	int n = (int)(sizeof(sizes)/sizeof(sizes[0]));
	printf("\n  %-12s %10s %10s %8s %10s %8s\n",
	       "size", "ms/op", "cpu ms/op", "core%%", "Mpix/s", "irq");
	printf("  ---------------------------------------------------------------\n");
	for (int i = 0; i < n; i++) {
		double cpu; int bad; long irq;
		double per = run_size(fd, sizes[i][0], sizes[i][1], &cpu, &bad, &irq);
		if (per < 0) { printf("  %dx%-8d  ALLOC/RUN FAILED\n", sizes[i][0], sizes[i][1]); continue; }
		char sz[16]; snprintf(sz, sizeof(sz), "%dx%d", sizes[i][0], sizes[i][1]);
		printf("  %-12s %10.4f %10.4f %7.0f%% %10.1f %8ld%s\n",
		       sz, per, cpu, per > 0 ? cpu/per*100 : 0,
		       (sizes[i][0]*(double)sizes[i][1])/(per/1000.0)/1e6, irq,
		       bad == 0 ? "" : (bad == -2 ? "  IOCTL-ERR" : "  MISMATCH"));
	}
	printf("\n  (ITERS=%d per size; irq should equal ITERS if HW-backed)\n\n", ITERS);

	close(fd);
	if (SysExit) SysExit(0);
	return 0;
}
