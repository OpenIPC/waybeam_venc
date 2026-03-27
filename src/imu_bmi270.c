#include "imu_bmi270.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

/* ------------------------------------------------------------------ */
/* BMI270 register definitions (from CoRoLab-Berlin/bmi270_c)          */
/* ------------------------------------------------------------------ */

#define BMI270_CHIP_ID_REG     0x00
#define BMI270_CHIP_ID_VAL     0x24
#define BMI270_INTERNAL_STATUS 0x21
#define BMI270_ACC_X_LSB       0x0C  /* 6 bytes: AX_L,AX_H,AY_L,AY_H,AZ_L,AZ_H */
#define BMI270_GYR_X_LSB       0x12  /* 6 bytes: GX_L,GX_H,GY_L,GY_H,GZ_L,GZ_H */
#define BMI270_FIFO_LENGTH_0   0x24
#define BMI270_FIFO_DATA       0x26
#define BMI270_ACC_CONF        0x40
#define BMI270_ACC_RANGE       0x41
#define BMI270_GYR_CONF        0x42
#define BMI270_GYR_RANGE       0x43
#define BMI270_FIFO_CONFIG_0   0x48
#define BMI270_FIFO_CONFIG_1   0x49
#define BMI270_INIT_CTRL       0x59
#define BMI270_INIT_ADDR_0     0x5B
#define BMI270_INIT_ADDR_1     0x5C
#define BMI270_INIT_DATA       0x5E
#define BMI270_CMD             0x7E
#define BMI270_PWR_CONF        0x7C
#define BMI270_PWR_CTRL        0x7D

/* FIFO_CONFIG_1 bits (from Bosch BMI2 API bmi2_defs.h) */
#define FIFO_ACC_EN            0x40  /* bit 6: enable accel data in FIFO */
#define FIFO_GYR_EN            0x80  /* bit 7: enable gyro data in FIFO */
#define FIFO_AUX_EN            0x20  /* bit 5: enable aux data in FIFO */
#define FIFO_HEADER_EN         0x10  /* bit 4: enable frame headers */

/* FIFO geometry */
#define FIFO_COMBINED_SIZE     13    /* 1(header) + 6(acc) + 6(gyr) for 0x8C frame */
#define FIFO_MAX_BYTES         1024  /* BMI270 FIFO depth */

/* FIFO header byte values (from Bosch BMI2 API bmi2_defs.h) */
#define FIFO_HDR_ACC           0x84  /* accel-only frame (6 bytes follow) */
#define FIFO_HDR_GYR           0x88  /* gyro-only frame (6 bytes follow) */
#define FIFO_HDR_ACC_GYR       0x8C  /* combined frame (12 bytes follow) */
#define FIFO_HDR_SENS_TIME     0x44  /* sensortime frame (3 bytes follow) */
#define FIFO_HDR_SKIP          0x40  /* skip/overread frame (1 byte follows) */
#define FIFO_HDR_INPUT_CFG     0x48  /* config change frame (4 bytes follow) */

/* CMD register values */
#define CMD_FIFO_FLUSH         0xB0

/* Range register values */
#define ACC_RANGE_2G           0x00
#define GYR_RANGE_2000         0x00
#define GYR_RANGE_1000         0x01
#define GYR_RANGE_500          0x02
#define GYR_RANGE_250          0x03
#define GYR_RANGE_125          0x04

/* ODR bits [3:0] for CONF registers (BMI270 datasheet Table 17/19) */
#define ODR_25HZ               0x06
#define ODR_50HZ               0x07
#define ODR_100HZ              0x08
#define ODR_200HZ              0x09
#define ODR_400HZ              0x0A
#define ODR_800HZ              0x0B
#define ODR_1600HZ             0x0C

/* Bandwidth bits */
#define ACC_BWP_OSR4           0x00
#define GYR_BWP_OSR4           0x00

/* Bit masks for FIFO_CONFIG_1 */
#define BIT_4                  0x10   /* header enable */
#define FIFO_DATA_EN_MASK      0xE0   /* bits 7,6,5: gyr, acc, aux enables */

/* Performance mode composite values (from reference bmi270.c set_mode) */
#define PERF_PWR_CTRL          0x0E  /* acc + gyr + temp enabled */
#define PERF_PWR_CONF          0x02  /* advanced power save off, FIFO self-wakeup on */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* BMI270 config file (8192 bytes firmware blob)                       */
/* ------------------------------------------------------------------ */

#include "bmi270_config_file.h"

/* ------------------------------------------------------------------ */
/* I2C helpers — matched to reference bmi270.c I2C_RDWR approach       */
/* ------------------------------------------------------------------ */

static int i2c_write_byte(int fd, uint8_t addr, uint8_t reg, uint8_t val)
{
	char buf[2] = { reg, val };
	struct i2c_msg msg = {
		.addr = addr, .flags = 0, .len = 2, .buf = (void *)buf,
	};
	struct i2c_rdwr_ioctl_data rdwr = { .msgs = &msg, .nmsgs = 1 };
	if (ioctl(fd, I2C_RDWR, &rdwr) < 0)
		return -errno;
	return 0;
}

static int i2c_write_block(int fd, uint8_t addr, uint8_t reg,
	const uint8_t *data, int len)
{
	/* write_register_block: buf[0]=reg, buf[1..len]=data (max 32 bytes data) */
	char buf[33];
	if (len > 32)
		return -1;
	buf[0] = reg;
	memcpy(buf + 1, data, len);

	struct i2c_msg msg = {
		.addr = addr, .flags = 0, .len = (uint16_t)(1 + len), .buf = (void *)buf,
	};
	struct i2c_rdwr_ioctl_data rdwr = { .msgs = &msg, .nmsgs = 1 };
	if (ioctl(fd, I2C_RDWR, &rdwr) < 0)
		return -errno;
	return 0;
}

static int i2c_read_byte(int fd, uint8_t addr, uint8_t reg, uint8_t *val)
{
	uint8_t wr = reg;
	uint8_t rd = 0;
	struct i2c_msg msgs[2] = {
		{ .addr = addr, .flags = 0, .len = 1, .buf = &wr },
		{ .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = &rd },
	};
	struct i2c_rdwr_ioctl_data rdwr = { .msgs = msgs, .nmsgs = 2 };
	if (ioctl(fd, I2C_RDWR, &rdwr) < 0)
		return -errno;
	*val = (uint8_t)rd;
	return 0;
}

static int i2c_read_block(int fd, uint8_t addr, uint8_t reg,
	uint8_t *data, uint8_t len)
{
	uint8_t wr = reg;
	struct i2c_msg msgs[2] = {
		{ .addr = addr, .flags = 0, .len = 1, .buf = &wr },
		{ .addr = addr, .flags = I2C_M_RD, .len = len, .buf = (void *)data },
	};
	struct i2c_rdwr_ioctl_data rdwr = { .msgs = msgs, .nmsgs = 2 };
	if (ioctl(fd, I2C_RDWR, &rdwr) < 0)
		return -errno;
	return 0;
}

/* Bulk read: same as i2c_read_block but supports uint16_t length
 * for FIFO reads up to 1024 bytes.  i2c_msg.len is __u16. */
static int i2c_read_bulk(int fd, uint8_t addr, uint8_t reg,
	uint8_t *data, uint16_t len)
{
	uint8_t wr = reg;
	struct i2c_msg msgs[2] = {
		{ .addr = addr, .flags = 0, .len = 1, .buf = &wr },
		{ .addr = addr, .flags = I2C_M_RD, .len = len, .buf = (void *)data },
	};
	struct i2c_rdwr_ioctl_data rdwr = { .msgs = msgs, .nmsgs = 2 };
	if (ioctl(fd, I2C_RDWR, &rdwr) < 0)
		return -errno;
	return 0;
}

/* ------------------------------------------------------------------ */
/* BMI270 initialization — matches reference load_config_file exactly   */
/* ------------------------------------------------------------------ */

static int bmi270_upload_config(int fd, uint8_t addr)
{
	uint8_t status = 0;
	if (i2c_read_byte(fd, addr, BMI270_INTERNAL_STATUS, &status) < 0)
		return -1;

	if (status & 0x01) {
		printf("  - IMU   : already initialized (status=0x%02x)\n", status);
		return 0;
	}

	if (status & 0x02) {
		fprintf(stderr, "IMU: sensor needs power cycle (status=0x%02x)\n", status);
		return -1;
	}

	printf("  - IMU   : uploading firmware...\n");

	/* Reference sequence: PWR_CONF=0, wait 450us, INIT_CTRL=0 */
	if (i2c_write_byte(fd, addr, BMI270_PWR_CONF, 0x00) < 0)
		return -1;
	usleep(450);
	if (i2c_write_byte(fd, addr, BMI270_INIT_CTRL, 0x00) < 0)
		return -1;

	/* Upload 8192 bytes in 256 chunks of 32 bytes.
	 * Reference addressing: INIT_ADDR_0=0x00, INIT_ADDR_1=chunk_index */
	for (int i = 0; i < 256; i++) {
		if (i2c_write_byte(fd, addr, BMI270_INIT_ADDR_0, 0x00) < 0)
			return -1;
		if (i2c_write_byte(fd, addr, BMI270_INIT_ADDR_1, (uint8_t)i) < 0)
			return -1;
		if (i2c_write_block(fd, addr, BMI270_INIT_DATA,
				&bmi270_config_file[i * 32], 32) < 0)
			return -1;
		usleep(20);
	}

	/* Finalize: INIT_CTRL=1, wait 20ms */
	if (i2c_write_byte(fd, addr, BMI270_INIT_CTRL, 0x01) < 0)
		return -1;
	usleep(20000);

	/* Verify */
	if (i2c_read_byte(fd, addr, BMI270_INTERNAL_STATUS, &status) < 0)
		return -1;

	printf("  - IMU   : init status=0x%02x (%s)\n", status,
		(status & 0x01) ? "OK" : "FAILED");

	return (status & 0x01) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* BMI270 sensor configuration — matches reference openipc_imu_rad     */
/* ------------------------------------------------------------------ */

static uint8_t sample_rate_to_odr(int hz)
{
	if (hz <= 25)   return ODR_25HZ;
	if (hz <= 50)   return ODR_50HZ;
	if (hz <= 100)  return ODR_100HZ;
	if (hz <= 200)  return ODR_200HZ;
	if (hz <= 400)  return ODR_400HZ;
	if (hz <= 800)  return ODR_800HZ;
	return ODR_1600HZ;
}

/* Return the actual ODR frequency for the given requested rate */
static int actual_sample_rate(int hz)
{
	if (hz <= 25)   return 25;
	if (hz <= 50)   return 50;
	if (hz <= 100)  return 100;
	if (hz <= 200)  return 200;
	if (hz <= 400)  return 400;
	if (hz <= 800)  return 800;
	return 1600;
}

static uint8_t gyro_range_to_reg(int dps)
{
	if (dps <= 125)  return GYR_RANGE_125;
	if (dps <= 250)  return GYR_RANGE_250;
	if (dps <= 500)  return GYR_RANGE_500;
	if (dps <= 1000) return GYR_RANGE_1000;
	return GYR_RANGE_2000;
}

static float gyro_range_to_scale(int dps)
{
	/* raw * (range_dps / 32768) * (PI / 180) → rad/s
	 * For 1000 dps: same as reference GSCALE = 0.00122173047 * PI/180 */
	float range;
	if (dps <= 125)  range = 125.0f;
	else if (dps <= 250)  range = 250.0f;
	else if (dps <= 500)  range = 500.0f;
	else if (dps <= 1000) range = 1000.0f;
	else range = 2000.0f;
	return range / 32768.0f * ((float)M_PI / 180.0f);
}

static int bmi270_configure(int fd, uint8_t addr, int gyro_range_dps,
	int sample_rate_hz)
{
	uint8_t rd;
	uint8_t odr = sample_rate_to_odr(sample_rate_hz);

	/* Performance mode: enable accel + gyro + temp (matches reference) */
	if (i2c_write_byte(fd, addr, BMI270_PWR_CTRL, PERF_PWR_CTRL) < 0) return -1;

	/* ACC_CONF: performance mode base (0xA8) with selected ODR in low nibble */
	if (i2c_write_byte(fd, addr, BMI270_ACC_CONF, 0xA8 | odr) < 0) return -1;

	/* ACC_RANGE: ±2g */
	if (i2c_write_byte(fd, addr, BMI270_ACC_RANGE, ACC_RANGE_2G) < 0) return -1;

	/* GYR_CONF: performance mode base (0xE9) with selected ODR in low nibble */
	if (i2c_write_byte(fd, addr, BMI270_GYR_CONF, 0xE9 | odr) < 0) return -1;

	/* GYR_RANGE */
	if (i2c_write_byte(fd, addr, BMI270_GYR_RANGE, gyro_range_to_reg(gyro_range_dps)) < 0) return -1;

	/* Disable FIFO header (reference: FIFO_CONFIG_1 &= ~BIT_4) */
	if (i2c_read_byte(fd, addr, BMI270_FIFO_CONFIG_1, &rd) < 0) return -1;
	if (i2c_write_byte(fd, addr, BMI270_FIFO_CONFIG_1, rd & ~BIT_4) < 0) return -1;

	/* Enable data streaming (reference: FIFO_CONFIG_1 &= ~LAST_3_BITS) */
	if (i2c_read_byte(fd, addr, BMI270_FIFO_CONFIG_1, &rd) < 0) return -1;
	if (i2c_write_byte(fd, addr, BMI270_FIFO_CONFIG_1, rd & ~FIFO_DATA_EN_MASK) < 0) return -1;

	/* ACC filter perf (reference: ACC_CONF |= BIT_7) */
	if (i2c_read_byte(fd, addr, BMI270_ACC_CONF, &rd) < 0) return -1;
	if (i2c_write_byte(fd, addr, BMI270_ACC_CONF, rd | 0x80) < 0) return -1;

	/* GYR noise perf (reference: GYR_CONF |= BIT_6) */
	if (i2c_read_byte(fd, addr, BMI270_GYR_CONF, &rd) < 0) return -1;
	if (i2c_write_byte(fd, addr, BMI270_GYR_CONF, rd | 0x40) < 0) return -1;

	/* GYR filter perf (reference: GYR_CONF |= BIT_7) */
	if (i2c_read_byte(fd, addr, BMI270_GYR_CONF, &rd) < 0) return -1;
	if (i2c_write_byte(fd, addr, BMI270_GYR_CONF, rd | 0x80) < 0) return -1;

	/* Advanced power save with FIFO self-wakeup (matches reference) */
	if (i2c_write_byte(fd, addr, BMI270_PWR_CONF, PERF_PWR_CONF) < 0) return -1;

	return 0;
}

/* ------------------------------------------------------------------ */
/* BMI270 FIFO configuration                                           */
/* ------------------------------------------------------------------ */

/* Enable FIFO for accel + gyro in header mode.
 * Header mode tags each frame (0x8C=combined, 0x88=gyr, 0x84=acc).
 * Flushes the FIFO before enabling to start clean. */
static int bmi270_enable_fifo(int fd, uint8_t addr, int gyro_range_dps,
	int sample_rate_hz)
{
	/* Re-assert power and sensor configuration.
	 * PWR_CTRL must be written for FIFO to fill, but writing it resets
	 * the ODR to defaults.  Re-apply the full configuration afterward. */
	if (i2c_write_byte(fd, addr, BMI270_PWR_CTRL, PERF_PWR_CTRL) < 0)
		return -1;
	if (i2c_write_byte(fd, addr, BMI270_PWR_CONF, PERF_PWR_CONF) < 0)
		return -1;
	usleep(450);

	/* Re-apply ODR and range configuration (lost after PWR_CTRL write) */
	if (bmi270_configure(fd, addr, gyro_range_dps, sample_rate_hz) < 0)
		return -1;

	/* Flush any stale data */
	if (i2c_write_byte(fd, addr, BMI270_CMD, CMD_FIFO_FLUSH) < 0)
		return -1;
	usleep(1000);

	/* FIFO_CONFIG_0: stop_on_full=0 (overwrite oldest on overflow) */
	if (i2c_write_byte(fd, addr, BMI270_FIFO_CONFIG_0, 0x00) < 0)
		return -1;

	/* FIFO_CONFIG_1: acc_en + gyr_en + header enabled.
	 * Header mode adds a 1-byte tag per sensor pair, needed to
	 * reliably delimit frames (headerless byte order depends on
	 * which sensors are enabled). */
	if (i2c_write_byte(fd, addr, BMI270_FIFO_CONFIG_1,
			FIFO_ACC_EN | FIFO_GYR_EN | FIFO_HEADER_EN) < 0)
		return -1;

	return 0;
}

static int bmi270_disable_fifo(int fd, uint8_t addr)
{
	/* Disable FIFO data sources */
	if (i2c_write_byte(fd, addr, BMI270_FIFO_CONFIG_1, 0x00) < 0)
		return -1;
	/* Flush remaining data */
	if (i2c_write_byte(fd, addr, BMI270_CMD, CMD_FIFO_FLUSH) < 0)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* IMU state                                                           */
/* ------------------------------------------------------------------ */

struct ImuState {
	ImuConfig cfg;
	int fd;               /* I2C file descriptor */
	float gyro_scale;     /* raw → rad/s */
	float accel_scale;    /* raw → m/s^2 */

	/* Calibration: gyro bias (rad/s) subtracted from raw samples */
	float gyro_bias[3];

	/* Calibration: rotation matrix (row-major 3x3) applied after bias.
	 * Maps sensor frame → camera frame.  Identity when no cal file. */
	float R[3][3];
	int have_rotation;     /* 1 if R loaded from cal file */

	/* Reader thread */
	pthread_t thread;
	volatile int running;
	int thread_started;
	int use_fifo;          /* 1 = FIFO batch reader, 0 = polling reader */
	int needs_thread;      /* 1 = imu-test mode, need thread even for FIFO */

	/* Stats (updated from reader thread) */
	pthread_mutex_t stats_lock;
	uint64_t samples_read;
	uint64_t read_errors;
	float last_gyro[3];
};

/* ------------------------------------------------------------------ */
/* Calibration: load /etc/imu.cal (wfb_openipc compatible format)      */
/*                                                                     */
/* File format (ASCII):                                                */
/*   flat: <ax> <ay> <az>                                              */
/*   tilt: <ax> <ay> <az>                                              */
/*   gyrobias: <gx> <gy> <gz>           (rad/s)                       */
/*   R: <r00> <r01> <r02> <r10> ... <r22>  (3x3 rotation matrix)      */
/* ------------------------------------------------------------------ */

static int load_cal_file(ImuState *st, const char *path)
{
	FILE *fp = fopen(path, "r");
	if (!fp)
		return -1;

	char line[256];
	int got_bias = 0, got_rotation = 0;

	while (fgets(line, sizeof(line), fp)) {
		float v[9];
		if (sscanf(line, "gyrobias: %f %f %f", &v[0], &v[1], &v[2]) == 3) {
			st->gyro_bias[0] = v[0];
			st->gyro_bias[1] = v[1];
			st->gyro_bias[2] = v[2];
			got_bias = 1;
		} else if (sscanf(line, "R: %f %f %f %f %f %f %f %f %f",
				&v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
				&v[6], &v[7], &v[8]) == 9) {
			st->R[0][0] = v[0]; st->R[0][1] = v[1]; st->R[0][2] = v[2];
			st->R[1][0] = v[3]; st->R[1][1] = v[4]; st->R[1][2] = v[5];
			st->R[2][0] = v[6]; st->R[2][1] = v[7]; st->R[2][2] = v[8];
			st->have_rotation = 1;
			got_rotation = 1;
		}
	}
	fclose(fp);

	if (got_bias)
		printf("  - IMU   : cal bias=(%.6f, %.6f, %.6f) rad/s\n",
			st->gyro_bias[0], st->gyro_bias[1], st->gyro_bias[2]);
	if (got_rotation)
		printf("  - IMU   : cal rotation matrix loaded\n");

	return (got_bias || got_rotation) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Calibration: auto-bias — average gyro at startup while stationary   */
/* ------------------------------------------------------------------ */

static int auto_bias_calibrate(ImuState *st, int n_samples)
{
	uint8_t addr = st->cfg.i2c_addr;
	int fd = st->fd;

	printf("  - IMU   : auto-bias calibration (%d samples, hold still)...\n",
		n_samples);

	double sum[3] = {0, 0, 0};
	int good = 0;
	long period_ns = 1000000000L / st->cfg.sample_rate_hz;

	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);

	for (int i = 0; i < n_samples; i++) {
		uint8_t gbuf[6];
		if (i2c_read_block(fd, addr, BMI270_GYR_X_LSB, gbuf, 6) < 0) {
			usleep(1000);
			continue;
		}

		int16_t gx = (int16_t)((gbuf[1] << 8) | gbuf[0]);
		int16_t gy = (int16_t)((gbuf[3] << 8) | gbuf[2]);
		int16_t gz = (int16_t)((gbuf[5] << 8) | gbuf[4]);

		sum[0] += (double)gx * st->gyro_scale;
		sum[1] += (double)gy * st->gyro_scale;
		sum[2] += (double)gz * st->gyro_scale;
		good++;

		next.tv_nsec += period_ns;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}

	if (good < 10) {
		fprintf(stderr, "IMU: auto-bias failed, only %d good samples\n", good);
		return -1;
	}

	st->gyro_bias[0] = (float)(sum[0] / good);
	st->gyro_bias[1] = (float)(sum[1] / good);
	st->gyro_bias[2] = (float)(sum[2] / good);

	printf("  - IMU   : auto-bias=(%.6f, %.6f, %.6f) rad/s from %d samples\n",
		st->gyro_bias[0], st->gyro_bias[1], st->gyro_bias[2], good);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Reader thread — matches reference openipc_imu read loop             */
/* ------------------------------------------------------------------ */

static void *imu_reader_thread(void *arg)
{
	ImuState *st = (ImuState *)arg;
	uint8_t addr = st->cfg.i2c_addr;
	int fd = st->fd;

	/* Period in nanoseconds (200 Hz → 5ms) */
	long period_ns = 1000000000L / st->cfg.sample_rate_hz;

	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);

	while (st->running) {
		/* Single 12-byte burst read: accel (0x0C-0x11) + gyro (0x12-0x17)
		 * are contiguous.  One I2C transaction instead of two halves the
		 * bus usage and ioctl overhead. */
		uint8_t buf[12];
		if (i2c_read_block(fd, addr, BMI270_ACC_X_LSB, buf, 12) < 0) {
			pthread_mutex_lock(&st->stats_lock);
			st->read_errors++;
			pthread_mutex_unlock(&st->stats_lock);
			usleep(1000);
			clock_gettime(CLOCK_MONOTONIC, &next);
			continue;
		}

		/* Parse little-endian 16-bit: buf[0..5]=accel, buf[6..11]=gyro */
		int16_t ax = (int16_t)((buf[1]  << 8) | buf[0]);
		int16_t ay = (int16_t)((buf[3]  << 8) | buf[2]);
		int16_t az = (int16_t)((buf[5]  << 8) | buf[4]);
		int16_t gx = (int16_t)((buf[7]  << 8) | buf[6]);
		int16_t gy = (int16_t)((buf[9]  << 8) | buf[8]);
		int16_t gz = (int16_t)((buf[11] << 8) | buf[10]);

		/* Scale raw to physical units */
		float raw_gx = (float)gx * st->gyro_scale - st->gyro_bias[0];
		float raw_gy = (float)gy * st->gyro_scale - st->gyro_bias[1];
		float raw_gz = (float)gz * st->gyro_scale - st->gyro_bias[2];
		float raw_ax = (float)ax * st->accel_scale;
		float raw_ay = (float)ay * st->accel_scale;
		float raw_az = (float)az * st->accel_scale;

		ImuSample sample;
		clock_gettime(CLOCK_MONOTONIC, &sample.ts);

		/* Apply rotation matrix if loaded from cal file.
		 * Scalar is faster than NEON for 3x3 rotation on Cortex-A7
		 * (measured: scalar 5978us vs NEON 7706us for 100K iterations)
		 * due to horizontal reduction overhead with 3-element vectors. */
		if (st->have_rotation) {
			sample.gyro_x = st->R[0][0]*raw_gx + st->R[0][1]*raw_gy + st->R[0][2]*raw_gz;
			sample.gyro_y = st->R[1][0]*raw_gx + st->R[1][1]*raw_gy + st->R[1][2]*raw_gz;
			sample.gyro_z = st->R[2][0]*raw_gx + st->R[2][1]*raw_gy + st->R[2][2]*raw_gz;
			sample.accel_x = st->R[0][0]*raw_ax + st->R[0][1]*raw_ay + st->R[0][2]*raw_az;
			sample.accel_y = st->R[1][0]*raw_ax + st->R[1][1]*raw_ay + st->R[1][2]*raw_az;
			sample.accel_z = st->R[2][0]*raw_ax + st->R[2][1]*raw_ay + st->R[2][2]*raw_az;
		} else {
			sample.gyro_x = raw_gx;
			sample.gyro_y = raw_gy;
			sample.gyro_z = raw_gz;
			sample.accel_x = raw_ax;
			sample.accel_y = raw_ay;
			sample.accel_z = raw_az;
		}

		st->cfg.push_fn(st->cfg.push_ctx, &sample);

		pthread_mutex_lock(&st->stats_lock);
		st->samples_read++;
		st->last_gyro[0] = sample.gyro_x;
		st->last_gyro[1] = sample.gyro_y;
		st->last_gyro[2] = sample.gyro_z;
		pthread_mutex_unlock(&st->stats_lock);

		/* Rate-limit: absolute time stepping (avoids drift) */
		next.tv_nsec += period_ns;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}

	return NULL;
}

/* ------------------------------------------------------------------ */
/* FIFO helpers — shared by imu_drain() (frame-synced) and imu-test    */
/* ------------------------------------------------------------------ */

/* Parse a combined 0x8C FIFO frame: 12 bytes = 6B gyro + 6B accel.
 * BMI270 FIFO wire order is gyr-first, acc-second (verified empirically).
 * Apply bias subtraction and rotation.  ts must be pre-set by caller. */
static void parse_fifo_combined(ImuState *st, const uint8_t *data,
	ImuSample *sample)
{
	int16_t gx = (int16_t)((data[1]  << 8) | data[0]);
	int16_t gy = (int16_t)((data[3]  << 8) | data[2]);
	int16_t gz = (int16_t)((data[5]  << 8) | data[4]);
	int16_t ax = (int16_t)((data[7]  << 8) | data[6]);
	int16_t ay = (int16_t)((data[9]  << 8) | data[8]);
	int16_t az = (int16_t)((data[11] << 8) | data[10]);

	float raw_gx = (float)gx * st->gyro_scale - st->gyro_bias[0];
	float raw_gy = (float)gy * st->gyro_scale - st->gyro_bias[1];
	float raw_gz = (float)gz * st->gyro_scale - st->gyro_bias[2];
	float raw_ax = (float)ax * st->accel_scale;
	float raw_ay = (float)ay * st->accel_scale;
	float raw_az = (float)az * st->accel_scale;

	if (st->have_rotation) {
		sample->gyro_x = st->R[0][0]*raw_gx + st->R[0][1]*raw_gy + st->R[0][2]*raw_gz;
		sample->gyro_y = st->R[1][0]*raw_gx + st->R[1][1]*raw_gy + st->R[1][2]*raw_gz;
		sample->gyro_z = st->R[2][0]*raw_gx + st->R[2][1]*raw_gy + st->R[2][2]*raw_gz;
		sample->accel_x = st->R[0][0]*raw_ax + st->R[0][1]*raw_ay + st->R[0][2]*raw_az;
		sample->accel_y = st->R[1][0]*raw_ax + st->R[1][1]*raw_ay + st->R[1][2]*raw_az;
		sample->accel_z = st->R[2][0]*raw_ax + st->R[2][1]*raw_ay + st->R[2][2]*raw_az;
	} else {
		sample->gyro_x = raw_gx;
		sample->gyro_y = raw_gy;
		sample->gyro_z = raw_gz;
		sample->accel_x = raw_ax;
		sample->accel_y = raw_ay;
		sample->accel_z = raw_az;
	}
}

/* Parse a gyro-only 0x88 FIFO frame: 6 bytes.
 * Apply bias and rotation, zero out accel.  ts must be pre-set. */
static void parse_fifo_gyro_only(ImuState *st, const uint8_t *data,
	ImuSample *sample)
{
	int16_t gx = (int16_t)((data[1] << 8) | data[0]);
	int16_t gy = (int16_t)((data[3] << 8) | data[2]);
	int16_t gz = (int16_t)((data[5] << 8) | data[4]);

	float raw_gx = (float)gx * st->gyro_scale - st->gyro_bias[0];
	float raw_gy = (float)gy * st->gyro_scale - st->gyro_bias[1];
	float raw_gz = (float)gz * st->gyro_scale - st->gyro_bias[2];

	sample->accel_x = sample->accel_y = sample->accel_z = 0.0f;

	if (st->have_rotation) {
		sample->gyro_x = st->R[0][0]*raw_gx + st->R[0][1]*raw_gy + st->R[0][2]*raw_gz;
		sample->gyro_y = st->R[1][0]*raw_gx + st->R[1][1]*raw_gy + st->R[1][2]*raw_gz;
		sample->gyro_z = st->R[2][0]*raw_gx + st->R[2][1]*raw_gy + st->R[2][2]*raw_gz;
	} else {
		sample->gyro_x = raw_gx;
		sample->gyro_y = raw_gy;
		sample->gyro_z = raw_gz;
	}
}

/* Interpolate timestamp: sample i of n_total, spread backward from now.
 * sample 0 = oldest (now - (n-1)*period), sample n-1 = newest (now). */
static struct timespec fifo_interpolate_ts(struct timespec now,
	long long sample_period_ns, int i, int n_total)
{
	long long offset_ns = (long long)(n_total - 1 - i) * sample_period_ns;
	struct timespec ts;
	ts.tv_sec = now.tv_sec - (time_t)(offset_ns / 1000000000LL);
	ts.tv_nsec = now.tv_nsec - (long)(offset_ns % 1000000000LL);
	if (ts.tv_nsec < 0) {
		ts.tv_nsec += 1000000000L;
		ts.tv_sec--;
	}
	return ts;
}

/* Count gyro-bearing frames in FIFO buffer (first pass).
 * Returns count; also validates frame headers. */
static int fifo_count_gyro_frames(const uint8_t *buf, uint16_t len)
{
	int count = 0;
	uint16_t pos = 0;
	while (pos < len) {
		uint8_t hdr = buf[pos];
		if (hdr == FIFO_HDR_ACC_GYR && pos + 13 <= len) {
			count++;
			pos += 13;
		} else if (hdr == FIFO_HDR_GYR && pos + 7 <= len) {
			count++;
			pos += 7;
		} else if (hdr == FIFO_HDR_ACC && pos + 7 <= len) {
			pos += 7;
		} else if (hdr == FIFO_HDR_INPUT_CFG && pos + 5 <= len) {
			pos += 5;
		} else if (hdr == FIFO_HDR_SENS_TIME && pos + 4 <= len) {
			pos += 4;
		} else if (hdr == FIFO_HDR_SKIP && pos + 2 <= len) {
			pos += 2;
		} else {
			break;
		}
	}
	return count;
}

/* Parse FIFO buffer (second pass): extract samples with interpolated
 * timestamps, call push_fn for each gyro-bearing frame.
 * Returns number of samples pushed. */
static int fifo_parse_and_push(ImuState *st, const uint8_t *buf,
	uint16_t len, struct timespec now, int n_gyro_frames,
	long long sample_period_ns)
{
	ImuSample sample = {0};
	int frame_idx = 0;
	uint16_t pos = 0;

	while (pos < len) {
		uint8_t hdr = buf[pos];
		if (hdr == FIFO_HDR_ACC_GYR && pos + 13 <= len) {
			sample.ts = fifo_interpolate_ts(now, sample_period_ns,
				frame_idx, n_gyro_frames);
			parse_fifo_combined(st, &buf[pos + 1], &sample);
			st->cfg.push_fn(st->cfg.push_ctx, &sample);
			frame_idx++;
			pos += 13;
		} else if (hdr == FIFO_HDR_GYR && pos + 7 <= len) {
			sample.ts = fifo_interpolate_ts(now, sample_period_ns,
				frame_idx, n_gyro_frames);
			parse_fifo_gyro_only(st, &buf[pos + 1], &sample);
			st->cfg.push_fn(st->cfg.push_ctx, &sample);
			frame_idx++;
			pos += 7;
		} else if (hdr == FIFO_HDR_ACC && pos + 7 <= len) {
			pos += 7;
		} else if (hdr == FIFO_HDR_INPUT_CFG && pos + 5 <= len) {
			pos += 5;
		} else if (hdr == FIFO_HDR_SENS_TIME && pos + 4 <= len) {
			pos += 4;
		} else if (hdr == FIFO_HDR_SKIP && pos + 2 <= len) {
			pos += 2;
		} else {
			break;
		}
	}

	/* Update stats with last sample */
	if (frame_idx > 0) {
		pthread_mutex_lock(&st->stats_lock);
		st->samples_read += frame_idx;
		st->last_gyro[0] = sample.gyro_x;
		st->last_gyro[1] = sample.gyro_y;
		st->last_gyro[2] = sample.gyro_z;
		pthread_mutex_unlock(&st->stats_lock);
	}

	return frame_idx;
}

/* ------------------------------------------------------------------ */
/* FIFO drain — called synchronously from video frame callback         */
/*                                                                     */
/* Reads FIFO_LENGTH, burst-reads data, parses, pushes samples.        */
/* Two I2C ioctls per call regardless of sample count.                 */
/* ------------------------------------------------------------------ */

static int fifo_drain_internal(ImuState *st)
{
	uint8_t addr = st->cfg.i2c_addr;
	int fd = st->fd;

	/* Read FIFO byte count (14-bit, registers 0x24-0x25) */
	uint8_t len_buf[2];
	if (i2c_read_block(fd, addr, BMI270_FIFO_LENGTH_0, len_buf, 2) < 0) {
		pthread_mutex_lock(&st->stats_lock);
		st->read_errors++;
		pthread_mutex_unlock(&st->stats_lock);
		return 0;
	}

	uint16_t fifo_bytes = ((uint16_t)(len_buf[1] & 0x3F) << 8) | len_buf[0];

	if (fifo_bytes < 7)  /* minimum useful frame: 1 header + 6 data */
		return 0;

	if (fifo_bytes > FIFO_MAX_BYTES)
		fifo_bytes = FIFO_MAX_BYTES;

	/* Burst-read all available FIFO data */
	uint8_t fifo_buf[FIFO_MAX_BYTES];
	if (i2c_read_bulk(fd, addr, BMI270_FIFO_DATA,
			fifo_buf, fifo_bytes) < 0) {
		pthread_mutex_lock(&st->stats_lock);
		st->read_errors++;
		pthread_mutex_unlock(&st->stats_lock);
		return 0;
	}

	/* Count gyro frames for timestamp interpolation */
	int n_gyro = fifo_count_gyro_frames(fifo_buf, fifo_bytes);
	if (n_gyro == 0)
		return 0;

	/* Parse and push with interpolated timestamps */
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	long long sample_period_ns = 1000000000LL / st->cfg.sample_rate_hz;

	return fifo_parse_and_push(st, fifo_buf, fifo_bytes, now,
		n_gyro, sample_period_ns);
}

/* ------------------------------------------------------------------ */
/* FIFO reader thread — for imu-test mode (no video frame callback)    */
/*                                                                     */
/* When running --imu-test, there is no frame loop to call imu_drain() */
/* so we still need a thread.  Reuses the same FIFO drain logic.       */
/* ------------------------------------------------------------------ */

static void *imu_fifo_reader_thread(void *arg)
{
	ImuState *st = (ImuState *)arg;
	int odr = st->cfg.sample_rate_hz;

	/* Poll interval: drain at ~50% FIFO capacity for good batching */
	long poll_ms = (1024L * 500L) / ((long)odr * FIFO_COMBINED_SIZE);
	if (poll_ms < 5)   poll_ms = 5;
	if (poll_ms > 100)  poll_ms = 100;

	printf("  - IMU   : FIFO reader: poll every %ld ms, ODR=%d Hz\n",
		poll_ms, odr);

	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);

	while (st->running) {
		fifo_drain_internal(st);

		next.tv_nsec += poll_ms * 1000000L;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}

	return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Open I2C device, verify chip ID, upload firmware.
 * Returns fd on success, -1 on failure. */
static int bmi270_open_and_init(const char *dev, uint8_t addr)
{
	int fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "IMU: failed to open %s: %s\n", dev, strerror(errno));
		return -1;
	}

	if (ioctl(fd, I2C_SLAVE, addr) < 0) {
		fprintf(stderr, "IMU: failed to set I2C addr 0x%02x: %s\n",
			addr, strerror(errno));
		close(fd);
		return -1;
	}

	uint8_t chip_id = 0;
	if (i2c_read_byte(fd, addr, BMI270_CHIP_ID_REG, &chip_id) < 0) {
		fprintf(stderr, "IMU: failed to read chip ID from %s addr 0x%02x\n",
			dev, addr);
		close(fd);
		return -1;
	}

	if (chip_id != BMI270_CHIP_ID_VAL) {
		fprintf(stderr, "IMU: chip ID 0x%02x (expected 0x%02x)\n",
			chip_id, BMI270_CHIP_ID_VAL);
		close(fd);
		return -1;
	}

	printf("  - IMU   : BMI270 on %s addr 0x%02x (chip_id=0x%02x)\n",
		dev, addr, chip_id);

	if (bmi270_upload_config(fd, addr) < 0) {
		fprintf(stderr, "IMU: firmware upload failed\n");
		close(fd);
		return -1;
	}

	return fd;
}

ImuState *imu_init(const ImuConfig *cfg)
{
	if (!cfg || !cfg->push_fn) {
		fprintf(stderr, "IMU: invalid config (missing push_fn)\n");
		return NULL;
	}

	const char *dev = cfg->i2c_device ? cfg->i2c_device : "/dev/i2c-1";
	uint8_t addr = cfg->i2c_addr ? cfg->i2c_addr : 0x68;

	int fd = bmi270_open_and_init(dev, addr);
	if (fd < 0)
		return NULL;

	ImuState *st = calloc(1, sizeof(*st));
	if (!st) {
		close(fd);
		return NULL;
	}

	st->cfg = *cfg;
	if (!st->cfg.i2c_device) st->cfg.i2c_device = "/dev/i2c-1";
	if (st->cfg.i2c_addr == 0) st->cfg.i2c_addr = 0x68;
	if (st->cfg.sample_rate_hz <= 0) st->cfg.sample_rate_hz = 200;
	if (st->cfg.gyro_range_dps <= 0) st->cfg.gyro_range_dps = 1000;

	st->fd = fd;
	st->gyro_scale = gyro_range_to_scale(st->cfg.gyro_range_dps);
	st->accel_scale = 2.0f * 9.80665f / 32768.0f;  /* ±2g */

	/* Identity rotation matrix (no-op transform) */
	st->R[0][0] = 1.0f; st->R[1][1] = 1.0f; st->R[2][2] = 1.0f;
	st->needs_thread = cfg->use_thread;

	pthread_mutex_init(&st->stats_lock, NULL);

	/* Snap sample rate to nearest valid ODR */
	st->cfg.sample_rate_hz = actual_sample_rate(st->cfg.sample_rate_hz);

	/* Configure sensor (matches reference set_mode + set_*_range + set_*_odr etc.) */
	if (bmi270_configure(fd, addr, st->cfg.gyro_range_dps,
			st->cfg.sample_rate_hz) < 0) {
		fprintf(stderr, "IMU: sensor configuration failed\n");
		close(fd);
		pthread_mutex_destroy(&st->stats_lock);
		free(st);
		return NULL;
	}

	printf("  - IMU   : gyro ±%d dps, accel ±2g, %d Hz, scale=%.8f rad/s/LSB\n",
		st->cfg.gyro_range_dps, st->cfg.sample_rate_hz, st->gyro_scale);

	/* Try to load calibration file (gyro bias + rotation matrix) */
	const char *cal_path = st->cfg.cal_file ? st->cfg.cal_file : "/etc/imu.cal";
	if (load_cal_file(st, cal_path) == 0) {
		printf("  - IMU   : calibration loaded from %s\n", cal_path);
	} else {
		/* No cal file — run auto-bias calibration (hold still at startup) */
		int cal_n = st->cfg.cal_samples > 0 ? st->cfg.cal_samples : 400;
		auto_bias_calibrate(st, cal_n);
	}

	return st;
}

int imu_start(ImuState *st)
{
	if (!st || st->thread_started)
		return -1;

	/* Try to enable FIFO mode — falls back to polling on failure */
	if (bmi270_enable_fifo(st->fd, st->cfg.i2c_addr,
			st->cfg.gyro_range_dps, st->cfg.sample_rate_hz) == 0) {
		st->use_fifo = 1;
		printf("  - IMU   : FIFO mode enabled (frame-synced, no thread)\n");
	} else {
		st->use_fifo = 0;
		printf("  - IMU   : FIFO init failed, using polling thread\n");
	}

	/* FIFO mode without a test callback: frame-synced, no thread needed.
	 * The caller drives reads via imu_drain() at each video frame.
	 * Only launch a thread for polling mode or imu-test (noop callback). */
	if (st->use_fifo && !st->needs_thread) {
		printf("  - IMU   : ready for frame-synced drain (%d Hz ODR)\n",
			st->cfg.sample_rate_hz);
		return 0;
	}

	void *(*thread_fn)(void *) = st->use_fifo
		? imu_fifo_reader_thread
		: imu_reader_thread;

	st->running = 1;

	pthread_attr_t attr;
	pthread_attr_init(&attr);

	/* Try RT scheduling for low-jitter sampling */
	struct sched_param sched = { .sched_priority = 20 };
	pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
	pthread_attr_setschedparam(&attr, &sched);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

	int ret = pthread_create(&st->thread, &attr, thread_fn, st);
	if (ret != 0) {
		/* Fall back to default scheduling */
		pthread_attr_destroy(&attr);
		pthread_attr_init(&attr);
		ret = pthread_create(&st->thread, &attr, thread_fn, st);
	}
	pthread_attr_destroy(&attr);

	if (ret != 0) {
		fprintf(stderr, "IMU: thread create failed: %s\n", strerror(ret));
		st->running = 0;
		if (st->use_fifo)
			bmi270_disable_fifo(st->fd, st->cfg.i2c_addr);
		return -1;
	}

	st->thread_started = 1;
	printf("  - IMU   : %s thread started (%d Hz)\n",
		st->use_fifo ? "FIFO" : "polling", st->cfg.sample_rate_hz);
	return 0;
}

int imu_drain(ImuState *st)
{
	if (!st || !st->use_fifo || st->thread_started)
		return 0;  /* no-op for polling mode or threaded FIFO */
	return fifo_drain_internal(st);
}

void imu_stop(ImuState *st)
{
	if (!st || !st->thread_started)
		return;
	st->running = 0;
	pthread_join(st->thread, NULL);
	st->thread_started = 0;
	printf("  - IMU   : reader thread stopped\n");
}

void imu_destroy(ImuState *st)
{
	if (!st)
		return;
	if (st->thread_started)
		imu_stop(st);

	/* Disable FIFO if it was enabled */
	if (st->use_fifo)
		bmi270_disable_fifo(st->fd, st->cfg.i2c_addr);

	/* Power down sensor */
	i2c_write_byte(st->fd, st->cfg.i2c_addr, BMI270_PWR_CTRL, 0x00);
	i2c_write_byte(st->fd, st->cfg.i2c_addr, BMI270_PWR_CONF, 0x03);

	close(st->fd);
	pthread_mutex_destroy(&st->stats_lock);

	printf("  - IMU   : shutdown, %lu samples, %lu errors\n",
		(unsigned long)st->samples_read,
		(unsigned long)st->read_errors);

	free(st);
}

void imu_get_stats(ImuState *st, ImuStats *out)
{
	if (!st || !out) {
		if (out) memset(out, 0, sizeof(*out));
		return;
	}
	pthread_mutex_lock(&st->stats_lock);
	out->samples_read = st->samples_read;
	out->read_errors = st->read_errors;
	out->last_gyro_x = st->last_gyro[0];
	out->last_gyro_y = st->last_gyro[1];
	out->last_gyro_z = st->last_gyro[2];
	pthread_mutex_unlock(&st->stats_lock);
}
