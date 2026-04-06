/*
 * Direct ioctl test for ISP IQ parameter setting.
 * Bypasses libmi_isp.so entirely — sends raw ioctl to /dev/mi_isp.
 *
 * From majestic reverse-engineering:
 *   Set ioctl: 0x401c6911 = _IOW('i', 17, 28)
 *   Get ioctl: 0x401c6912 = _IOW('i', 18, 28)
 *
 * The ioctl buffer is 28 bytes (built from 24-byte command struct):
 *   [0]  u32: 0x18 (constant — header type marker)
 *   [4]  u32: data_size
 *   [8]  u32: api_id (e.g. 0x1006 for brightness)
 *   [12] u32: param (0)
 *   [16] u32: channel (0)
 *   [20] u32: 0 (flags)
 *   [24] u32: pointer to data buffer (44-byte ioctl, so buf at offset 24 is a ptr)
 *
 * Run while venc is streaming: /tmp/iq_ioctl_test
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define ISP_IQ_SET  0x401c6911
#define ISP_IQ_GET  0x401c6912

/* 28-byte ioctl command struct */
typedef struct {
	uint32_t type;       /* 0x18 constant */
	uint32_t data_len;   /* size of IQ parameter struct */
	uint32_t api_id;     /* e.g. 0x1006 for brightness */
	uint32_t param;      /* 0 */
	uint32_t channel;    /* 0 */
	uint32_t flags;      /* 0 */
	void    *data_ptr;   /* pointer to parameter data buffer */
} IspIoctlCmd;

int main(void)
{
	int fd = open("/dev/mi_isp", O_RDWR);
	if (fd < 0) {
		perror("open /dev/mi_isp");
		return 1;
	}
	printf("Opened /dev/mi_isp as fd %d\n", fd);

	/* Skip Get — go straight to Set. Build struct from scratch. */
	uint8_t bri_buf[76];
	memset(bri_buf, 0, sizeof(bri_buf));
	*(uint32_t *)&bri_buf[0] = 1;     /* enable */
	*(uint32_t *)&bri_buf[4] = 1;     /* optype = manual */
	*(uint32_t *)&bri_buf[0x48] = 0;  /* value = 0 (dark) */

	IspIoctlCmd cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.type = 0x18;
	cmd.data_len = 76;
	cmd.api_id = 0x1006;  /* brightness */
	cmd.param = 0;
	cmd.channel = 0;
	cmd.flags = 0;
	cmd.data_ptr = bri_buf;

	printf("Sending SET brightness=0 via ioctl...\n");
	int ret = ioctl(fd, ISP_IQ_SET, &cmd);
	printf("SET brightness=0: ret=%d\n", ret);

	sleep(3);
	printf("(check if image went dark)\n");

	/* Test 3: Set brightness to 100 (bright) */
	*(uint32_t *)&bri_buf[0x48] = 100;
	cmd.data_ptr = bri_buf;
	ret = ioctl(fd, ISP_IQ_SET, &cmd);
	printf("SET brightness=100: ret=%d\n", ret);

	sleep(3);
	printf("(check if image went bright)\n");

	/* Test 4: ColorToGray (api_id 0x1004, but wait — 0x1004 is WDR.
	 * ColorToGray api_id is 4100 decimal = 0x1004. Let me check...
	 * From the API ID list: COLORTOGRAY = 0x1000 + 4 = 0x1004 */
	uint32_t ctg_enable = 1;
	cmd.data_len = 4;
	cmd.api_id = 0x1004;  /* ColorToGray = IQ_BASE + 4 */
	cmd.data_ptr = &ctg_enable;
	ret = ioctl(fd, ISP_IQ_SET, &cmd);
	printf("SET color_to_gray=1 (id=0x1004): ret=%d\n", ret);

	sleep(3);
	printf("(check if image is gray)\n");

	/* Restore */
	ctg_enable = 0;
	cmd.data_ptr = &ctg_enable;
	ret = ioctl(fd, ISP_IQ_SET, &cmd);
	printf("SET color_to_gray=0: ret=%d\n", ret);

	close(fd);
	return 0;
}
