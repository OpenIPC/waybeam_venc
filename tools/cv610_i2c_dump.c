/*
 * cv610_i2c_dump — read IMX662 registers over /dev/i2c on the CV610 bench.
 *
 * The sensor plugin's imx662_read_register() is a stub (the ISP only ever
 * writes), so there is no way to confirm from the daemon that an init
 * sequence actually landed.  This does the 16-bit-address read the sensor
 * needs, from a second process, while the pipeline is running.
 *
 * Derived from the i2c_dump.c bench tool in diegok3/firmware
 * (feat/imx662-driver, utils/i2c_dump.c).
 *
 * DEV-ONLY.  Not installed into any image; push it to /tmp and delete it.
 *
 * Build:
 *   toolchain/toolchain.hisilicon-hi3516cv6xx/bin/arm-openipc-linux-musleabi-gcc \
 *       -O2 -s -Wall -Wextra -std=c99 -o out/cv610_i2c_dump tools/cv610_i2c_dump.c
 *
 * Run:
 *   scp -O out/cv610_i2c_dump root@192.168.2.181:/tmp/
 *   ssh root@192.168.2.181 /tmp/cv610_i2c_dump            # the default range set
 *   ssh root@192.168.2.181 /tmp/cv610_i2c_dump 0x3014 0x3015 0x302c:0x302d
 *
 * Output is one register per line, "0xADDR 0xVV" or "0xADDR ERR", so a diff
 * of two dumps is the whole comparison.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Linux I2C_SLAVE_FORCE; the sensor plugin binds the same address, so the
 * non-FORCE ioctl would be refused as busy. */
#define I2C_SLAVE_FORCE 0x0706

#define DEFAULT_DEV  "/dev/i2c-0"
#define DEFAULT_ADDR 0x1a /* IMX662_I2C_ADDR 0x34 >> 1 */

/* The ranges that matter for the power-on init block: mode/timing page,
 * gain + reserved page, AD timing, and the reserved tail that Sony's
 * sequence ends at (0x4549). */
static const unsigned short g_default_ranges[][2] = {
	{ 0x3000, 0x30ff },
	{ 0x3400, 0x34ff },
	{ 0x3a50, 0x3a52 },
	{ 0x3b00, 0x3c50 },
	{ 0x3e00, 0x3eff },
	{ 0x4490, 0x44e0 },
	{ 0x4530, 0x4550 },
};

static int g_fd = -1;

static int reg_read(unsigned short reg, unsigned char *val)
{
	unsigned char buf[2] = { (unsigned char)(reg >> 8), (unsigned char)(reg & 0xff) };

	if (write(g_fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
		return -1;
	if (read(g_fd, val, 1) != 1)
		return -1;
	return 0;
}

static void dump_range(unsigned short start, unsigned short end)
{
	unsigned int reg; /* wider than u16 so end == 0xffff terminates */

	for (reg = start; reg <= end; reg++) {
		unsigned char val;

		if (reg_read((unsigned short)reg, &val) == 0)
			printf("0x%04X 0x%02X\n", reg, val);
		else
			printf("0x%04X ERR\n", reg);
	}
}

/* "0x3014" or "0x3000:0x30ff" */
static int parse_range(const char *arg, unsigned short *start, unsigned short *end)
{
	char *sep;
	long a, b;

	a = strtol(arg, &sep, 0);
	if (sep == arg || a < 0 || a > 0xffff)
		return -1;

	if (*sep == '\0') {
		b = a;
	} else if (*sep == ':') {
		const char *tail = sep + 1;
		b = strtol(tail, &sep, 0);
		if (sep == tail || *sep != '\0' || b < a || b > 0xffff)
			return -1;
	} else {
		return -1;
	}

	*start = (unsigned short)a;
	*end   = (unsigned short)b;
	return 0;
}

int main(int argc, char *argv[])
{
	const char *dev = getenv("I2C_DEV");
	const char *addr_env = getenv("I2C_ADDR");
	long addr = DEFAULT_ADDR;
	int i;

	if (dev == NULL)
		dev = DEFAULT_DEV;
	if (addr_env != NULL)
		addr = strtol(addr_env, NULL, 0);

	g_fd = open(dev, O_RDWR);
	if (g_fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	if (ioctl(g_fd, I2C_SLAVE_FORCE, addr) < 0) {
		fprintf(stderr, "I2C_SLAVE_FORCE 0x%02lx: %s\n", addr, strerror(errno));
		close(g_fd);
		return 1;
	}

	if (argc > 1) {
		for (i = 1; i < argc; i++) {
			unsigned short start, end;

			if (parse_range(argv[i], &start, &end) != 0) {
				fprintf(stderr, "bad range '%s' (want 0xNNNN or 0xNNNN:0xNNNN)\n",
						argv[i]);
				close(g_fd);
				return 2;
			}
			dump_range(start, end);
		}
	} else {
		for (i = 0; i < (int)(sizeof(g_default_ranges) / sizeof(g_default_ranges[0])); i++)
			dump_range(g_default_ranges[i][0], g_default_ranges[i][1]);
	}

	close(g_fd);
	return 0;
}
