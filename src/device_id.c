/*
 * device_id — SoC die ID reader (native /dev/mem RIU register read).
 *
 * Method and register map are taken from OpenIPC ipctool's SigmaStar HAL
 * (src/hal/sstar.c::sstar_get_die_id, src/hal/sstar.h), device-verified
 * against `ipcinfo -i`:
 *
 *   - chip generation tag : low 16 bits of 0x1F003C00 (0xF1 == INFINITY6E)
 *   - INFINITY6E die base : 0x1F203150   (ssc338q / Star6E)
 *   - other generations   : 0x1F004058
 *   - die ID = low16 of base+8, base+4, base+0, MSW-first, each %04X
 *
 * ssc37x / Maruko expose no die ID (registers read all-zero) → reported as
 * unavailable.
 */

#include "device_id.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef PLATFORM_CV610
#define SSTAR_GEN_REG    0x1F003C00u  /* low16 = chip generation tag        */
#define DIEID_BASE_6E    0x1F203150u  /* INFINITY6E (ssc338q) die-ID base   */
#define DIEID_BASE_ALT   0x1F004058u  /* other SigmaStar generations        */
#define GEN_INFINITY6E   0xF1u

/* Read one 32-bit word at a physical address via a page-aligned /dev/mem
 * mmap.  Only the low 16 bits of each RIU register are meaningful. */
static bool read_phys(int fd, uint32_t phys, uint32_t *out)
{
	long pg = sysconf(_SC_PAGESIZE);
	if (pg <= 0)
		pg = 4096;
	uint32_t page = phys & ~(uint32_t)(pg - 1);
	uint32_t off  = phys - page;

	void *map = mmap(NULL, (size_t)pg, PROT_READ, MAP_SHARED, fd,
		(off_t)page);
	if (map == MAP_FAILED)
		return false;
	*out = *(volatile uint32_t *)((uint8_t *)map + off);
	munmap(map, (size_t)pg);
	return true;
}
#endif

bool device_id_serial(char *out, size_t len)
{
	if (out && len > 0)
		out[0] = '\0';
	if (!out || len < 13)
		return false;

#ifdef PLATFORM_CV610
	/* SigmaStar RIU addresses are unsafe on HiSilicon.  CV610 identity will
	 * be added through an OTP-backed provider once its public register/API is
	 * confirmed; mDNS falls back to the configured/host name meanwhile. */
	return false;
#else

	/* O_RDWR|O_SYNC matches ipctool/devmem; least surprise on the chip. */
	int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (fd < 0)
		return false;

	uint32_t gen = 0;
	bool ok = read_phys(fd, SSTAR_GEN_REG, &gen);
	uint32_t base = ((gen & 0xFFFFu) == GEN_INFINITY6E) ? DIEID_BASE_6E
		: DIEID_BASE_ALT;

	uint32_t w0 = 0, w1 = 0, w2 = 0;
	if (ok)
		ok = read_phys(fd, base + 8, &w0) &&
		     read_phys(fd, base + 4, &w1) &&
		     read_phys(fd, base + 0, &w2);
	close(fd);
	if (!ok)
		return false;

	uint32_t a = w0 & 0xFFFFu, b = w1 & 0xFFFFu, c = w2 & 0xFFFFu;
	/* Reject degenerate reads: Maruko/ssc37x reads all-zero; unprogrammed
	 * OTP reads all-ones. */
	if ((a | b | c) == 0)
		return false;
	if (a == 0xFFFFu && b == 0xFFFFu && c == 0xFFFFu)
		return false;

	snprintf(out, len, "%04X%04X%04X", a, b, c);
	return true;
#endif
}

static char        g_serial[16];
static pthread_once_t g_serial_once = PTHREAD_ONCE_INIT;

static void serial_init(void)
{
	device_id_serial(g_serial, sizeof(g_serial));  /* leaves "" on failure */
}

const char *device_id_serial_cached(void)
{
	pthread_once(&g_serial_once, serial_init);
	return g_serial;
}
