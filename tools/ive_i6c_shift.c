/* ive_i6c_shift.c — exercise MI_IVE_Shift_Detector on Maruko/i6c.
 *
 * Follow-up to ive_i6c_probe.c, which proved MI_IVE_Create/Destroy work once
 * the BSP-matched libmi_ive.so (c6a1e30) is installed. This tool answers the
 * actual feature question for video.framing stab parity:
 *
 *   1. does MI_IVE_Shift_Detector recover a KNOWN synthetic translation?
 *   2. how long does it take per call on i6c's hardware MVE block?
 *      (Star6E's i6e runs the same API as a userspace-CPU fallback: ~19ms)
 *
 * Key i6c-vs-i6e difference: i6c IVE is a real HW block, so the source/dest
 * images must live in CONTIGUOUS PHYSICAL memory. Star6E gets away with
 * posix_memalign + MI_SYS_Va2Pa because its detector is CPU-side. Here we
 * allocate via MI_SYS_MMA_Alloc + MI_SYS_Mmap (which is precisely why
 * libmi_ive.so imports MMA_Alloc/Free, Mmap/Munmap and FlushInvCache).
 *
 * Struct layouts / ctrl geometry copied verbatim from the working Star6E
 * implementation (src/star6e_framing_stab.c).
 *
 * Usage: ive_i6c_shift [lib-dir]        (default /usr/lib)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <dlfcn.h>
#include <stdint.h>

typedef int                MI_S32;
typedef unsigned char      MI_U8;
typedef unsigned short     MI_U16;
typedef unsigned int       MI_U32;
typedef unsigned long long MI_U64;
typedef unsigned long long MI_PHY;
typedef unsigned char      MI_BOOL;

/* ── IVE types (mirror star6e_framing_stab.c) ───────────────────────────── */
#define E_IVE_IMAGE_TYPE_U8C1 0x0
#define E_IVE_IMAGE_TYPE_S8C1 0x1
#define E_IVE_SHIFT_DETECT_MODE_SINGLE 0x00

typedef struct {
	int     eType;
	MI_PHY  aphyPhyAddr[3];
	MI_U8  *apu8VirAddr[3];
	MI_U16  azu16Stride[3];
	MI_U16  u16Width;
	MI_U16  u16Height;
	MI_U16  u16Reserved;
} IveImage_t;

typedef struct {
	int    enMode;
	MI_U8  pyramid_level;
	MI_U8  search_range;
	MI_U16 u16Left;
	MI_U16 u16Top;
	MI_U16 u16Width;
	MI_U16 u16Height;
} IveShiftCtrl_t;

/* Geometry — the FULL values Star6E ships (384 crop / 256 box / 3 levels). */
#define CROP_W 384
#define CROP_H 384
#define BOX    256
#define PYRAMID 3
#define SEARCH_RANGE 96
#define ITERS  100

/* ── vendor fn pointers ─────────────────────────────────────────────────── */
typedef MI_S32 (*sys_init_fn)(MI_U16);
typedef MI_S32 (*sys_exit_fn)(MI_U16);
typedef MI_S32 (*mma_alloc_fn)(MI_U16, MI_U8 *, MI_U32, MI_PHY *);
typedef MI_S32 (*mma_free_fn)(MI_U16, MI_PHY);
typedef MI_S32 (*mmap_fn)(MI_U64, MI_U32, void **, MI_BOOL);
typedef MI_S32 (*munmap_fn)(void *, MI_U32);
typedef MI_S32 (*flushinv_fn)(void *, MI_U32);
typedef MI_S32 (*ive_create_fn)(int);
typedef MI_S32 (*ive_destroy_fn)(int);
typedef MI_S32 (*ive_shift_fn)(int, IveImage_t *, IveImage_t *,
	IveImage_t *, IveImage_t *, IveShiftCtrl_t *, MI_BOOL);

static mma_alloc_fn MmaAlloc;
static mma_free_fn  MmaFree;
static mmap_fn      SysMmap;
static munmap_fn    SysMunmap;
static flushinv_fn  FlushInv;

static void *load_global(const char *dir, const char *name) {
	char p[512];
	snprintf(p, sizeof(p), "%s/%s", dir, name);
	void *h = dlopen(p, RTLD_LAZY | RTLD_GLOBAL);
	if (!h) fprintf(stderr, "[preload] %s FAILED: %s\n", p, dlerror());
	return h;
}

/* MMA-backed image. width/height in px; stride 64-aligned. */
static int alloc_img(IveImage_t *img, MI_U16 w, MI_U16 h, int type)
{
	MI_U32 stride = (w + 63u) & ~63u;
	MI_U32 size   = stride * h;
	MI_PHY phy = 0;
	void *vir = NULL;

	memset(img, 0, sizeof(*img));
	if (MmaAlloc(0, NULL, size, &phy) != 0 || !phy) {
		/* some SDK builds want an explicit heap name */
		if (MmaAlloc(0, (MI_U8 *)"mma_heap_name0", size, &phy) != 0 || !phy) {
			fprintf(stderr, "[mma] alloc %u bytes FAILED\n", size);
			return -1;
		}
	}
	if (SysMmap(phy, size, &vir, 0 /* uncached */) != 0 || !vir) {
		fprintf(stderr, "[mma] mmap phy=0x%llx FAILED\n", (unsigned long long)phy);
		MmaFree(0, phy);
		return -1;
	}
	memset(vir, 0, size);
	img->eType = type;
	img->u16Width = w;
	img->u16Height = h;
	img->azu16Stride[0] = (MI_U16)stride;
	img->aphyPhyAddr[0] = phy;
	img->apu8VirAddr[0] = (MI_U8 *)vir;
	return 0;
}

static void free_img(IveImage_t *img)
{
	MI_U32 size = (MI_U32)img->azu16Stride[0] * img->u16Height;
	if (img->apu8VirAddr[0]) SysMunmap(img->apu8VirAddr[0], size);
	if (img->aphyPhyAddr[0]) MmaFree(0, img->aphyPhyAddr[0]);
	memset(img, 0, sizeof(*img));
}

/* Deterministic textured base image with strong local structure so block
 * matching has a unique correlation peak. Hash noise, then 3x3 box blur to
 * give it spatial correlation (pure white noise aliases under pyramiding). */
#define BASE 512
#define MARGIN 48
static unsigned char base_raw[BASE * BASE];
static unsigned char base_img[BASE * BASE];

static void build_base(void)
{
	for (int y = 0; y < BASE; y++)
		for (int x = 0; x < BASE; x++) {
			unsigned h = (unsigned)(x * 374761393u + y * 668265263u);
			h = (h ^ (h >> 13)) * 1274126177u;
			base_raw[y * BASE + x] = (unsigned char)(h >> 24);
		}
	for (int y = 0; y < BASE; y++)
		for (int x = 0; x < BASE; x++) {
			int s = 0, n = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					int yy = y + dy, xx = x + dx;
					if (yy < 0 || yy >= BASE || xx < 0 || xx >= BASE) continue;
					s += base_raw[yy * BASE + xx];
					n++;
				}
			base_img[y * BASE + x] = (unsigned char)(s / n);
		}
}

/* Copy a CROP_W x CROP_H window of base_img at (ox,oy) into img. */
static void fill_window(IveImage_t *img, int ox, int oy)
{
	for (int y = 0; y < CROP_H; y++)
		memcpy(img->apu8VirAddr[0] + (size_t)y * img->azu16Stride[0],
		       base_img + (size_t)(oy + y) * BASE + ox, CROP_W);
	FlushInv(img->apu8VirAddr[0], (MI_U32)img->azu16Stride[0] * img->u16Height);
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Process CPU time. If cpu ~= wall the detector is burning the core (software);
 * if cpu << wall it is blocked waiting on the HW MVE block (true offload). */
static double cpu_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	const char *dir = (argc > 1) ? argv[1] : "/usr/lib";
	char p[512];

	load_global(dir, "libcam_os_wrapper.so");
	load_global(dir, "libmi_common.so");
	void *sys = load_global(dir, "libmi_sys.so");
	if (!sys) return 2;

	sys_init_fn SysInit = (sys_init_fn) dlsym(sys, "MI_SYS_Init");
	sys_exit_fn SysExit = (sys_exit_fn) dlsym(sys, "MI_SYS_Exit");
	MmaAlloc  = (mma_alloc_fn) dlsym(sys, "MI_SYS_MMA_Alloc");
	MmaFree   = (mma_free_fn)  dlsym(sys, "MI_SYS_MMA_Free");
	SysMmap   = (mmap_fn)      dlsym(sys, "MI_SYS_Mmap");
	SysMunmap = (munmap_fn)    dlsym(sys, "MI_SYS_Munmap");
	FlushInv  = (flushinv_fn)  dlsym(sys, "MI_SYS_FlushInvCache");
	if (!SysInit || !MmaAlloc || !SysMmap || !FlushInv) {
		fprintf(stderr, "missing MI_SYS symbols\n");
		return 3;
	}

	MI_S32 r = SysInit(0);
	fprintf(stderr, "[sys] MI_SYS_Init(0) -> %d\n", r);
	if (r != 0) return 4;

	snprintf(p, sizeof(p), "%s/libmi_ive.so", dir);
	void *ivelib = dlopen(p, RTLD_LAZY | RTLD_GLOBAL);
	if (!ivelib) { fprintf(stderr, "dlopen ive: %s\n", dlerror()); return 5; }

	ive_create_fn  Create  = (ive_create_fn)  dlsym(ivelib, "MI_IVE_Create");
	ive_destroy_fn Destroy = (ive_destroy_fn) dlsym(ivelib, "MI_IVE_Destroy");
	ive_shift_fn   Shift   = (ive_shift_fn)   dlsym(ivelib, "MI_IVE_Shift_Detector");
	if (!Create || !Shift) { fprintf(stderr, "missing MI_IVE symbols\n"); return 6; }

	r = Create(0);
	fprintf(stderr, "[ive] MI_IVE_Create(0) -> %d\n", r);
	if (r != 0) return 7;

	build_base();

	IveImage_t src1, src2, dx, dy;
	if (alloc_img(&src1, CROP_W, CROP_H, E_IVE_IMAGE_TYPE_U8C1) != 0 ||
	    alloc_img(&src2, CROP_W, CROP_H, E_IVE_IMAGE_TYPE_U8C1) != 0 ||
	    alloc_img(&dx, 1, 1, E_IVE_IMAGE_TYPE_S8C1) != 0 ||
	    alloc_img(&dy, 1, 1, E_IVE_IMAGE_TYPE_S8C1) != 0) {
		fprintf(stderr, "alloc failed\n");
		return 8;
	}
	fprintf(stderr, "[mma] src1 phy=0x%llx src2 phy=0x%llx dx=0x%llx dy=0x%llx\n",
		(unsigned long long)src1.aphyPhyAddr[0],
		(unsigned long long)src2.aphyPhyAddr[0],
		(unsigned long long)dx.aphyPhyAddr[0],
		(unsigned long long)dy.aphyPhyAddr[0]);

	int left = ((CROP_W - BOX) / 2) & ~1;
	int top  = ((CROP_H - BOX) / 2) & ~1;
	IveShiftCtrl_t ctrl = {
		.enMode = E_IVE_SHIFT_DETECT_MODE_SINGLE,
		.pyramid_level = PYRAMID,
		.search_range  = SEARCH_RANGE,
		.u16Left = (MI_U16)left, .u16Top = (MI_U16)top,
		.u16Width = BOX, .u16Height = BOX,
	};

	static const int shifts[][2] = {{0,0},{2,1},{5,3},{-4,2},{8,-6}};
	int nsh = (int)(sizeof(shifts)/sizeof(shifts[0]));
	int pass = 0;

	printf("\n  expected      raw(dx,dy)   status\n");
	printf("  ------------------------------------\n");
	for (int i = 0; i < nsh; i++) {
		int sx = shifts[i][0], sy = shifts[i][1];
		fill_window(&src1, MARGIN, MARGIN);
		fill_window(&src2, MARGIN + sx, MARGIN + sy);
		memset(dx.apu8VirAddr[0], 0, dx.azu16Stride[0]);
		memset(dy.apu8VirAddr[0], 0, dy.azu16Stride[0]);

		r = Shift(0, &src1, &src2, &dx, &dy, &ctrl, 1);
		if (r != 0) { printf("  (%3d,%3d)     SHIFT ret=%d\n", sx, sy, r); continue; }
		FlushInv(dx.apu8VirAddr[0], dx.azu16Stride[0]);
		FlushInv(dy.apu8VirAddr[0], dy.azu16Stride[0]);
		int rdx = ((int8_t *)dx.apu8VirAddr[0])[0];
		int rdy = ((int8_t *)dy.apu8VirAddr[0])[0];
		/* accept either sign convention, but require |.| to match */
		int ok = (abs(rdx) == abs(sx)) && (abs(rdy) == abs(sy));
		pass += ok;
		printf("  (%3d,%3d)  ->  (%3d,%3d)     %s\n", sx, sy, rdx, rdy,
			ok ? "OK" : "MISMATCH");
	}

	/* timing: worst-case-ish shift, ITERS calls */
	fill_window(&src1, MARGIN, MARGIN);
	fill_window(&src2, MARGIN + 5, MARGIN + 3);
	double t0 = now_ms(), c0 = cpu_ms();
	for (int i = 0; i < ITERS; i++)
		Shift(0, &src1, &src2, &dx, &dy, &ctrl, 1);
	double t1 = now_ms(), c1 = cpu_ms();
	double per = (t1 - t0) / ITERS;
	double cpu_per = (c1 - c0) / ITERS;
	double util = (per > 0) ? (cpu_per / per) * 100.0 : 0.0;

	printf("\n  Shift_Detector: %.3f ms/call wall, %.3f ms/call CPU (%.0f%% core)\n",
		per, cpu_per, util);
	printf("  (%d iters, %dx%d crop, box %d, pyr %d)\n",
		ITERS, CROP_W, CROP_H, BOX, PYRAMID);
	printf("  i6e (CPU fallback) reference: ~19 ms/call\n");
	printf("  verdict: %s\n", (util > 80.0)
		? "CPU-BOUND — software detector, HW MVE not offloading"
		: "HW-OFFLOADED — core mostly idle while IVE runs");
	printf("  accuracy: %d/%d shifts recovered\n\n", pass, nsh);

	printf("IVE_SHIFT_JSON {\"ok\":%s,\"pass\":%d,\"total\":%d,"
	       "\"ms_per_call\":%.3f,\"cpu_ms_per_call\":%.3f,\"core_pct\":%.0f}\n",
		(pass == nsh) ? "true" : "false", pass, nsh, per, cpu_per, util);

	free_img(&src1); free_img(&src2); free_img(&dx); free_img(&dy);
	if (Destroy) Destroy(0);
	if (SysExit) SysExit(0);
	return (pass == nsh) ? 0 : 1;
}
