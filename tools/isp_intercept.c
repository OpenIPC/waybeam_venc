/*
 * LD_PRELOAD interceptor for ISP IQ API calls.
 * Logs all MI_ISP_IQ_SetAll, MI_ISP_IQ_GetAll, MI_ISP_GENERAL_*,
 * MI_ISP_SetIQApiData, and ioctl calls to /dev/isp* devices.
 *
 * Build: arm-openipc-linux-musleabihf-gcc -shared -fPIC -o isp_intercept.so isp_intercept.c -ldl
 * Run:   LD_PRELOAD=/tmp/isp_intercept.so majestic
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

static FILE *g_log;

static void log_init(void)
{
	if (!g_log) {
		g_log = fopen("/tmp/isp_intercept.log", "w");
		if (g_log)
			setbuf(g_log, NULL); /* unbuffered */
	}
}

static void log_hex(const char *label, const void *buf, int len)
{
	if (!g_log) return;
	const uint8_t *p = buf;
	fprintf(g_log, "  %s[%d]: ", label, len);
	int n = len > 64 ? 64 : len;
	for (int i = 0; i < n; i++)
		fprintf(g_log, "%02x ", p[i]);
	if (len > 64) fprintf(g_log, "...");
	fprintf(g_log, "\n");
}

/* Intercept open() to track ISP device fds */
static int g_isp_fds[32];
static int g_isp_fd_count;

typedef int (*real_open_t)(const char *, int, ...);
int open(const char *path, int flags, ...)
{
	real_open_t real_open = (real_open_t)dlsym(RTLD_NEXT, "open");
	int fd;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		fd = real_open(path, flags, va_arg(ap, int));
		va_end(ap);
	} else {
		fd = real_open(path, flags);
	}
	if (fd >= 0 && path && strstr(path, "isp")) {
		log_init();
		if (g_log)
			fprintf(g_log, "OPEN %s -> fd %d\n", path, fd);
		if (g_isp_fd_count < 32)
			g_isp_fds[g_isp_fd_count++] = fd;
	}
	return fd;
}

/* Intercept ioctl to see what goes to ISP devices */
typedef int (*real_ioctl_t)(int, int, ...);
int ioctl(int fd, int request, ...)
{
	real_ioctl_t real_ioctl = (real_ioctl_t)dlsym(RTLD_NEXT, "ioctl");
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	int is_isp = 0;
	for (int i = 0; i < g_isp_fd_count; i++)
		if (g_isp_fds[i] == fd) { is_isp = 1; break; }

	int ret = real_ioctl(fd, request, arg);

	if (is_isp) {
		log_init();
		if (g_log)
			fprintf(g_log, "IOCTL fd=%d req=0x%x arg=%p ret=%d\n",
				fd, (unsigned)request, arg, ret);
	}
	return ret;
}

/* Intercept MI_ISP_IQ_SetAll */
int MI_ISP_IQ_SetAll(uint32_t dev, uint32_t ch, uint16_t api_id,
	uint32_t len, uint8_t *buf)
{
	typedef int (*fn_t)(uint32_t, uint32_t, uint16_t, uint32_t, uint8_t *);
	fn_t real_fn = (fn_t)dlsym(RTLD_NEXT, "MI_ISP_IQ_SetAll");
	log_init();
	if (g_log) {
		fprintf(g_log, "MI_ISP_IQ_SetAll(dev=%u ch=%u id=%u len=%u)\n",
			dev, ch, api_id, len);
		if (buf)
			log_hex("buf", buf, len);
	}
	int ret = real_fn ? real_fn(dev, ch, api_id, len, buf) : -1;
	if (g_log)
		fprintf(g_log, "  -> ret=%d (0x%x)\n", ret, ret);
	return ret;
}

/* Intercept MI_ISP_SetAll (CUS3A variant) */
int MI_ISP_SetAll(uint32_t dev, uint32_t ch, uint16_t api_id,
	uint32_t len, uint8_t *buf)
{
	typedef int (*fn_t)(uint32_t, uint32_t, uint16_t, uint32_t, uint8_t *);
	fn_t real_fn = (fn_t)dlsym(RTLD_NEXT, "MI_ISP_SetAll");
	log_init();
	if (g_log) {
		fprintf(g_log, "MI_ISP_SetAll(dev=%u ch=%u id=%u len=%u)\n",
			dev, ch, api_id, len);
		if (buf)
			log_hex("buf", buf, len);
	}
	int ret = real_fn ? real_fn(dev, ch, api_id, len, buf) : -1;
	if (g_log)
		fprintf(g_log, "  -> ret=%d (0x%x)\n", ret, ret);
	return ret;
}

/* Intercept MI_ISP_GENERAL_SetIspApiData */
int MI_ISP_GENERAL_SetIspApiData(void *header, void *data)
{
	typedef int (*fn_t)(void *, void *);
	fn_t real_fn = (fn_t)dlsym(RTLD_NEXT, "MI_ISP_GENERAL_SetIspApiData");
	log_init();
	if (g_log) {
		fprintf(g_log, "MI_ISP_GENERAL_SetIspApiData(hdr=%p data=%p)\n",
			header, data);
		if (header)
			log_hex("header", header, 24);
		if (data) {
			uint32_t data_len = 0;
			memcpy(&data_len, (uint8_t *)header + 4, 4);
			log_hex("data", data, data_len > 256 ? 256 : data_len);
		}
	}
	int ret = real_fn ? real_fn(header, data) : -1;
	if (g_log)
		fprintf(g_log, "  -> ret=%d (0x%x)\n", ret, ret);
	return ret;
}

/* Intercept MI_ISP_SetIQApiData */
int MI_ISP_SetIQApiData(void *header, void *data)
{
	typedef int (*fn_t)(void *, void *);
	fn_t real_fn = (fn_t)dlsym(RTLD_NEXT, "MI_ISP_SetIQApiData");
	log_init();
	if (g_log) {
		fprintf(g_log, "MI_ISP_SetIQApiData(hdr=%p data=%p)\n",
			header, data);
		if (header)
			log_hex("header", header, 24);
	}
	int ret = real_fn ? real_fn(header, data) : -1;
	if (g_log)
		fprintf(g_log, "  -> ret=%d (0x%x)\n", ret, ret);
	return ret;
}

/* Intercept MI_ISP_IQ_SetColorToGray specifically */
int MI_ISP_IQ_SetColorToGray(uint32_t dev, uint32_t ch, void *data)
{
	typedef int (*fn_t)(uint32_t, uint32_t, void *);
	fn_t real_fn = (fn_t)dlsym(RTLD_NEXT, "MI_ISP_IQ_SetColorToGray");
	log_init();
	if (g_log) {
		fprintf(g_log, "MI_ISP_IQ_SetColorToGray(dev=%u ch=%u)\n",
			dev, ch);
		if (data)
			log_hex("data", data, 4);
	}
	int ret = real_fn ? real_fn(dev, ch, data) : -1;
	if (g_log)
		fprintf(g_log, "  -> ret=%d (0x%x)\n", ret, ret);
	return ret;
}

/* Intercept MI_ISP_IQ_SetBrightness */
int MI_ISP_IQ_SetBrightness(uint32_t dev, uint32_t ch, void *data)
{
	typedef int (*fn_t)(uint32_t, uint32_t, void *);
	fn_t real_fn = (fn_t)dlsym(RTLD_NEXT, "MI_ISP_IQ_SetBrightness");
	log_init();
	if (g_log) {
		fprintf(g_log, "MI_ISP_IQ_SetBrightness(dev=%u ch=%u)\n",
			dev, ch);
		if (data)
			log_hex("data", data, 76);
	}
	int ret = real_fn ? real_fn(dev, ch, data) : -1;
	if (g_log)
		fprintf(g_log, "  -> ret=%d (0x%x)\n", ret, ret);
	return ret;
}
