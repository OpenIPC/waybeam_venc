/*
 * imx662_cmos.h — Sony IMX662 (STARVIS 2) driver for HiSilicon Hi3516CV610
 *                 (hi3516cv6xx, V5 "OT" SDK, ss_* / ot_* MPI API).
 *
 * SCAFFOLD — original code, no vendor (Shenshu/HiSilicon) source copied. It
 * matches the *public* HiSilicon sensor-driver interface (ot_isp_sns_obj /
 * pfn_cmos_*) that the SDK headers below define. Register addresses/values are
 * from the Sony IMX662 datasheet and the public Raspberry Pi V4L2 driver
 * (will127534/imx662-v4l2-driver, GPL-2.0). Items to confirm on real hardware
 * are marked "VERIFY"; items to complete at integration are marked "TODO".
 *
 * Build: not standalone — compile in-tree under
 *   OpenIPC/openhisilicon/libraries/sensor/hi3516cv6xx/sony_imx662/
 * against the hi3516cv6xx ISP/kernel headers (see Makefile).
 */

#ifndef IMX662_CMOS_H
#define IMX662_CMOS_H

#include "ot_common.h"
#include "ot_common_isp.h"
#include "ot_common_video.h"
#include "ot_sns_ctrl.h"
#include "ot_type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- I2C wiring -----------------------------------------------------------
 * IMX662 has a strap-selectable address: 0x34 (8-bit) / 0x1A (7-bit), or the
 * alternate 0x36 / 0x1B. VERIFY against the CV610 board with i2cdetect/ipctool.
 */
#define IMX662_I2C_ADDR    0x34
#define IMX662_ADDR_BYTE   2      /* 16-bit register address */
#define IMX662_DATA_BYTE   1      /* 8-bit register data     */

/* ---- IMX662 register map (Sony STARVIS 2; shared with IMX585/678/664) ------
 * Source: IMX662 datasheet + will127534/imx662-v4l2-driver. Multi-byte fields
 * are little-endian across consecutive addresses (L, M, H).
 */
#define IMX662_REG_STANDBY     0x3000  /* 0x01 = standby, 0x00 = operating     */
#define IMX662_REG_XMSTA       0x3002  /* 0x01 = master stop, 0x00 = start     */ /* VERIFY */
#define IMX662_REG_INCK_SEL    0x3014  /* master clock select (see table)      */
#define IMX662_REG_DATARATE    0x3015  /* MIPI data-rate select                */
#define IMX662_REG_WINMODE     0x3018  /* windowing/mode                       */ /* VERIFY */
#define IMX662_REG_WDMODE      0x301A  /* 0x00 normal / Clear-HDR select       */
#define IMX662_REG_HREVERSE    0x3020  /* horizontal flip (bit0)               */
#define IMX662_REG_VREVERSE    0x3021  /* vertical flip (bit0)                  */
#define IMX662_REG_ADBIT       0x3022  /* AD bit depth (10/12-bit)             */ /* VERIFY */
#define IMX662_REG_ODBIT       0x3023  /* MIPI output bit depth (10/12-bit)    */
#define IMX662_REG_VMAX_L      0x3028  /* VMAX [19:0], default 1250            */
#define IMX662_REG_VMAX_M      0x3029
#define IMX662_REG_VMAX_H      0x302A
#define IMX662_REG_HMAX_L      0x302C  /* HMAX [15:0]                          */
#define IMX662_REG_HMAX_H      0x302D
#define IMX662_REG_FDG_SEL0    0x3030  /* conversion gain HCG/LCG select       */
#define IMX662_REG_LANEMODE    0x3040  /* 0x01 = 2-lane, 0x03 = 4-lane         */ /* VERIFY */
#define IMX662_REG_SHR0_L      0x3050  /* SHR0 [23:0] exposure                 */
#define IMX662_REG_SHR0_M      0x3051
#define IMX662_REG_SHR0_H      0x3052
#define IMX662_REG_GAIN_L      0x306C  /* analog gain [15:0], 0.3 dB / LSB     */
#define IMX662_REG_GAIN_H      0x306D
#define IMX662_REG_BLKLEVEL_L  0x30DC  /* black level (OFFSET), default 0x32=50 */
#define IMX662_REG_BLKLEVEL_H  0x30DD
#define IMX662_REG_DIG_CLAMP   0x3458  /* digital clamp                        */ /* VERIFY */
#define IMX662_REG_AD10_0      0x3A50  /* ADC timing: differs for 10/12 bit    */
#define IMX662_REG_AD10_1      0x3A51
#define IMX662_REG_AD10_2      0x3A52

/* ---- exposure / gain model ------------------------------------------------
 * Sony SHR model:  integration_lines = VMAX - SHR0   (SHR0 even-aligned)
 *   -> SHR0 = VMAX - int_time, clamped to [MIN_SHR0, VMAX - 1].
 * Gain: single register, 0.3 dB per LSB. HiSilicon passes 'again' as a linear
 *   value where 1024 == 1x, so:  gain_reg = round( 20*log10(again/1024) / 0.3 ).
 */
#define IMX662_VMAX_1080P30_LINEAR   1250u
#define IMX662_HMAX_1080P30_LINEAR   1980u   /* 37.125 MHz INCK, 30 fps        */
#define IMX662_HMAX_1080P60_LINEAR    990u   /* 37.125 MHz INCK, 60 fps        */
#define IMX662_HMAX_1080P90_LINEAR    660u   /* 37.125 MHz INCK, 90 fps        */
#define IMX662_HMAX_1080P100_LINEAR   668u   /* 27 MHz overclocked 24 MHz mode */
#define IMX662_FULL_LINES_MAX        0xFFFFFu
#define IMX662_MIN_SHR0_LINEAR       8u       /* min shutter (linear)           */
#define IMX662_MIN_SHR0_HDR          10u      /* min shutter (Clear-HDR)        */

#define IMX662_AGAIN_MIN             1024u                  /* 1x  (0 dB)        */
#define IMX662_AGAIN_MAX             (1024u * 3981u / 10u)  /* ~72 dB, VERIFY    */
#define IMX662_GAIN_STEP_MDB         30       /* 0.3 dB, in milli-dB            */
#define IMX662_GAIN_REG_MAX          240u     /* 72 dB / 0.3 dB                 */

/* Dual conversion gain.  Sony's SRM restricts the GAIN register to 22h..F0h
 * while FDG_SEL0 = 1 (HCG) against 00h..F0h in LCG, and the register is
 * "Gain [dB] x 10/3" in both modes -- so the code carries the same total dB
 * either way and the switch at 34 is seamless in brightness.  HCG lowers read
 * noise; its cost is a smaller saturation signal (SRM: "Saturation signal gets
 * smaller according to Vsat"), which is why bright scenes must stay in LCG.
 *
 * The two thresholds give hysteresis so AE hunting around the boundary cannot
 * toggle the register every frame.  HCG_MIN is Sony's floor, not a choice. */
#define IMX662_HCG_GAIN_REG_MIN      34u      /* 10.2 dB; SRM lower bound       */
#define IMX662_HCG_ON_REG            40u      /* 12.0 dB: enter HCG above this  */
#define IMX662_HCG_OFF_REG           34u      /* 10.2 dB: fall back to LCG here */
#define IMX662_FDG_LCG               0x00u
#define IMX662_FDG_HCG               0x01u

/* ---- resolution modes -----------------------------------------------------
 * Bring linear up first. Add Clear-HDR (2-frame) as a second mode later.
 */
typedef enum {
    IMX662_SENSOR_2M_30FPS_12BIT_LINEAR_MODE = 0,
    IMX662_SENSOR_2M_60FPS_12BIT_LINEAR_MODE,
    IMX662_SENSOR_2M_90FPS_10BIT_LINEAR_MODE,
    IMX662_SENSOR_2M_100FPS_10BIT_LINEAR_MODE,
    IMX662_MODE_BUTT
} imx662_res_mode;

typedef struct {
    td_u32      ver_lines;      /* VMAX at nominal fps        */
    td_u32      max_ver_lines;  /* FULL_LINES_MAX             */
    td_float    max_fps;
    td_float    min_fps;
    td_u32      width;
    td_u32      height;
    td_u8       sns_mode;
    ot_wdr_mode wdr_mode;
    const char *mode_name;
} imx662_video_mode_tbl;

/* ---- context accessors + transport (imx662_sensor_ctl.c) ------------------ */
ot_isp_sns_state   *imx662_get_ctx(ot_vi_pipe vi_pipe);
ot_isp_sns_commbus *imx662_get_bus_info(ot_vi_pipe vi_pipe);

td_void imx662_init(ot_vi_pipe vi_pipe);
td_void imx662_exit(ot_vi_pipe vi_pipe);
td_void imx662_standby(ot_vi_pipe vi_pipe);
td_void imx662_restart(ot_vi_pipe vi_pipe);
td_s32  imx662_write_register(ot_vi_pipe vi_pipe, td_u32 addr, td_u32 data);
td_s32  imx662_read_register(ot_vi_pipe vi_pipe, td_u32 addr);
td_void imx662_mirror_flip(ot_vi_pipe vi_pipe, ot_isp_sns_mirrorflip_type sns_mirror_flip);
td_void imx662_blc_clamp(ot_vi_pipe vi_pipe, ot_isp_sns_blc_clamp blc_clamp);
td_s32  imx662_i2c_init(ot_vi_pipe vi_pipe);
td_s32  imx662_i2c_exit(ot_vi_pipe vi_pipe);

/* exported registration object (imx662_cmos.c) */
extern ot_isp_sns_obj g_sns_imx662_obj;

#ifdef __cplusplus
}
#endif
#endif /* IMX662_CMOS_H */
