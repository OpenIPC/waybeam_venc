/*
 * regscan.c — Sony IMX335 / IMX415 sensor register dumper.
 *
 * Vendored from https://github.com/tipoman9/star6c_sensor (src/regscan.c)
 * under the upstream project's terms.  See NOTICE in this directory.
 *
 * Used by scripts/maruko_sensor_init_diff.sh to compare sensor register
 * state across firstboot / venc-cold / majestic-cold / majestic-then-venc
 * runs and identify writes that majestic performs but venc does not.
 *
 * Build:
 *   make regscan SOC_BUILD=maruko   ->  out/maruko/regscan
 *
 * Run examples (on target):
 *   ./regscan -d /dev/i2c-0 -a 0x1a
 *   ./regscan -d /dev/i2c-1 -a 0x1a -q
 *
 * Notes:
 * - Sony IMX335 / IMX415 use 16-bit register addresses and 8-bit values.
 * - 0xFFFF entries in the register list are treated as separators/sentinels.
 * - The default I2C address is 0x1A (7-bit), but it can be overridden.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

static const uint16_t g_regs[] = {

    /* Core control */
    0x3000, 0x3001, 0x3002, 0x3003, 0x3004,
    0x3008, 0x3009, 0x300A, 0x300B,
    0x300C, 0x300D,

    /* Mode / timing */
    0x3018, 0x301C,
    0x3020, 0x3021, 0x3022,
    0x3024, 0x3025, 0x3026,
    0x3028, 0x3029,
    0x302C, 0x302D, 0x302E, 0x302F,
    0x3030, 0x3031, 0x3032, 0x3033, 0x3034, 0x3035,

    /* Window / crop */
    0x3040, 0x3041, 0x3042, 0x3043,
    0x3044, 0x3045, 0x3046, 0x3047,

    /* Exposure */
    0x3050, 0x3051, 0x3052,
    0x3058, 0x3059, 0x305A,

    /* Misc */
    0x3081,
    0x3090, 0x3091,

    /* Sync / clip / black level */
    0x30C0, 0x30C1, 0x30C6,
    0x30CC, 0x30CD, 0x30CE,
    0x30D8, 0x30D9, 0x30DA,
    0x30E2, 0x30E3,

    /* Clocking */
    0x3115, 0x3116,
    0x3118, 0x3119,
    0x311A, 0x311B,
    0x311E,

    /* Sensor blocks */
    0x314C, 0x314D,
    0x315A,
    0x3168, 0x316A,
    0x319D, 0x319E,
    0x31A1, 0x31D7,

    /* ISP / internal */
    0x3288, 0x328A,
    0x32D4, 0x32EC,

    0x3414, 0x3416, 0x341C, 0x341D,
    0x3452, 0x3453,

    0x358A, 0x35A1,

    0x3648, 0x364A, 0x364C,
    0x3678, 0x367C, 0x367E,

    0x36BC,
    0x36CC, 0x36CD, 0x36CE,
    0x36D0, 0x36D1, 0x36D2, 0x36D4,
    0x36D6, 0x36D7, 0x36D8,
    0x36DA, 0x36DB,

    0x3701, 0x3706, 0x3708,
    0x3714, 0x3715, 0x3716, 0x3717,
    0x371C, 0x371D,

    0x3724, 0x3726,
    0x372C, 0x372D, 0x372E, 0x372F,
    0x3730, 0x3731, 0x3732, 0x3733,
    0x3734, 0x3735, 0x3736,
    0x3740, 0x3742,

    0x375D, 0x375E, 0x375F, 0x3760,
    0x3768, 0x3769, 0x376A, 0x376B,
    0x376C, 0x376D, 0x376E,

    0x3776, 0x3777, 0x3778, 0x3779,
    0x377A, 0x377B, 0x377C, 0x377D,
    0x377E, 0x377F,
    0x3780, 0x3781, 0x3782, 0x3783, 0x3784,
    0x3788, 0x378A, 0x378B, 0x378C, 0x378D, 0x378E, 0x378F,
    0x3790, 0x3792, 0x3794, 0x3796,

    0x3862,
    0x38CC, 0x38CD,

    0x395C,

    0x3A18, 0x3A1A, 0x3A1C, 0x3A1E,
    0x3A1F, 0x3A20, 0x3A22, 0x3A24,
    0x3A26, 0x3A28,
    0x3A42, 0x3A4C,
    0x3AE0, 0x3AEC,

    0x3B00, 0x3B06,
    0x3B98, 0x3B99,
    0x3B9B, 0x3B9C, 0x3B9D, 0x3B9E,
    0x3BA1, 0x3BA2, 0x3BA3, 0x3BA4,
    0x3BA5, 0x3BA6, 0x3BA7, 0x3BA8, 0x3BA9,
    0x3BAC, 0x3BAD, 0x3BAE, 0x3BAF,
    0x3BB0, 0x3BB1, 0x3BB2, 0x3BB3,
    0x3BB4, 0x3BB5, 0x3BB6, 0x3BB7, 0x3BB8,
    0x3BBA, 0x3BBC, 0x3BBE,
    0x3BC0, 0x3BC2, 0x3BC4,
    0x3BC8, 0x3BCA,

    /* CSI / output timing */
    0x4001,
    0x4004, 0x4005, 0x400C,
    0x4018, 0x4019, 0x401A, 0x401B,
    0x401C, 0x401D, 0x401E, 0x401F,
    0x4020, 0x4021, 0x4022, 0x4023,
    0x4024, 0x4025, 0x4026, 0x4027,
    0x4028, 0x4029,
    0x4074,

    /* Sentinels */
    0xFFFF,
    0x3002,
    0xFFFF,
    0x3000
};

struct options_t {
    const char *devnode;
    uint8_t i2c_addr;
    bool quiet_errors;
};

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-d /dev/i2c-X] [-a 0x1a] [-q]\n"
        "\n"
        "Options:\n"
        "  -d, --device   I2C device node (default: /dev/i2c-0)\n"
        "  -a, --addr     7-bit sensor I2C address in hex/dec (default: 0x1a)\n"
        "  -q, --quiet    Print compact read errors\n"
        "  -h, --help     Show this help\n",
        prog);
}

static int parse_u8(const char *s, uint8_t *out)
{
    char *end = NULL;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 0);
    if (errno || end == s || *end != '\0' || v > 0x7f)
        return -1;

    *out = (uint8_t)v;
    return 0;
}

static int i2c_read_reg8(int fd, uint8_t addr, uint16_t reg, uint8_t *val)
{
    uint8_t regbuf[2];
    uint8_t databuf[1];
    struct i2c_msg msgs[2];
    struct i2c_rdwr_ioctl_data xfer;

    regbuf[0] = (uint8_t)((reg >> 8) & 0xff);
    regbuf[1] = (uint8_t)(reg & 0xff);

    msgs[0].addr  = addr;
    msgs[0].flags = 0;
    msgs[0].len   = sizeof(regbuf);
    msgs[0].buf   = regbuf;

    msgs[1].addr  = addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len   = sizeof(databuf);
    msgs[1].buf   = databuf;

    xfer.msgs  = msgs;
    xfer.nmsgs = 2;

    if (ioctl(fd, I2C_RDWR, &xfer) < 0)
        return -1;

    *val = databuf[0];
    return 0;
}

static const char *special_reg_name(uint16_t reg)
{
    switch (reg) {
    case 0x3000: return "STANDBY";
    case 0x3002: return "MASTER_MODE_START";
    default:     return "";
    }
}

int main(int argc, char **argv)
{
    static const struct option long_opts[] = {
        { "device", required_argument, 0, 'd' },
        { "addr",   required_argument, 0, 'a' },
        { "quiet",  no_argument,       0, 'q' },
        { "help",   no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    struct options_t opt = {
        .devnode = "/dev/i2c-0",
        .i2c_addr = 0x1a,
        .quiet_errors = false
    };

    int fd = -1;
    int c;

    while ((c = getopt_long(argc, argv, "d:a:qh", long_opts, NULL)) != -1) {
        switch (c) {
        case 'd':
            opt.devnode = optarg;
            break;
        case 'a':
            if (parse_u8(optarg, &opt.i2c_addr) < 0) {
                fprintf(stderr, "Invalid I2C address: %s\n", optarg);
                return 2;
            }
            break;
        case 'q':
            opt.quiet_errors = true;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (c == 'h') ? 0 : 2;
        }
    }

    fd = open(opt.devnode, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", opt.devnode, strerror(errno));
        return 1;
    }

    printf("Sensor register dump\n");
    printf("  device : %s\n", opt.devnode);
    printf("  addr   : 0x%02X\n", opt.i2c_addr);
    printf("\n");

    for (size_t i = 0; i < ARRAY_SIZE(g_regs); ++i) {
        uint16_t reg = g_regs[i];

        if (reg == 0xFFFF) {
            printf("----\n");
            continue;
        }

        uint8_t val = 0;
        int ret = i2c_read_reg8(fd, opt.i2c_addr, reg, &val);
        if (ret == 0) {
            const char *name = special_reg_name(reg);
            if (name[0] != '\0')
                printf("0x%04X = 0x%02X    (%s)\n", reg, val, name);
            else
                printf("0x%04X = 0x%02X\n", reg, val);
        } else {
            if (opt.quiet_errors) {
                printf("0x%04X = <read error>\n", reg);
            } else {
                printf("0x%04X = <read error: %s>\n", reg, strerror(errno));
            }
        }
    }

    close(fd);
    return 0;
}
