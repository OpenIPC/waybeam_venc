/* Copyright (c) 2018-2019 Sigmastar Technology Corp.
 All rights reserved.

 Unless otherwise stipulated in writing, any and all information contained
herein regardless in any format shall remain the sole proprietary of
Sigmastar Technology Corp. and be kept in strict confidence
(Sigmastar Confidential Information) by the recipient.
Any unauthorized act including without limitation unauthorized disclosure,
copying, use, reproduction, sale, distribution, modification, disassembling,
reverse engineering and compiling of the contents of Sigmastar Confidential
Information is unlawful and strictly prohibited. Sigmastar hereby reserves the
rights to any and all damages, losses, costs and expenses resulting therefrom.
*/

#ifdef __cplusplus
extern "C" {
#endif

#include <drv_sensor.h>
#include <drv_sensor_common.h>
#include <drv_sensor_init_table.h>
#include <sensor_i2c_api.h>
#ifdef __cplusplus
}
#endif

SENSOR_DRV_ENTRY_IMPL_BEGIN_EX(IMX335_HDR);

#ifndef ARRAY_SIZE
#define ARRAY_SIZE CAM_OS_ARRAY_SIZE
#endif

//============================================
#define ENABLE 1
#define DISABLE 0
#undef SENSOR_DBG
#define SENSOR_DBG 0

#define DEBUG_INFO 0

#if SENSOR_DBG == 1
//#define SENSOR_DMSG(args...) printf(args)
#elif SENSOR_DBG == 0
//#define SENSOR_DMSG(args...)
#endif

////////////////////////////////////
// Sensor-If Info                 //
////////////////////////////////////
#define SENSOR_MIPI_LANE_NUM (4)

#define SENSOR_ISP_TYPE ISP_EXT
#define SENSOR_IFBUS_TYPE CUS_SENIF_BUS_MIPI
#define SENSOR_MIPI_HSYNC_MODE PACKET_HEADER_EDGE1
#define SENSOR_DATAPREC CUS_DATAPRECISION_10
#define SENSOR_DATAMODE CUS_SEN_10TO12_9098
#define SENSOR_BAYERID CUS_BAYER_RG
#define SENSOR_RGBIRID CUS_RGBIR_NONE
#define SENSOR_ORIT CUS_ORIT_M0F0

////////////////////////////////////
// MCLK Info                      //
////////////////////////////////////
#define Preview_MCLK_SPEED CUS_CMU_CLK_27MHZ

////////////////////////////////////
// I2C Info                       //
////////////////////////////////////
#define SENSOR_I2C_ADDR 0x34
#define SENSOR_I2C_SPEED 300000
#define SENSOR_I2C_LEGACY I2C_NORMAL_MODE
#define SENSOR_I2C_FMT I2C_FMT_A16D8

////////////////////////////////////
// Sensor Signal                  //
////////////////////////////////////
#define SENSOR_PWDN_POL CUS_CLK_POL_NEG
#define SENSOR_RST_POL CUS_CLK_POL_NEG

////////////////////////////////////
// Sensor ID                      //
////////////////////////////////////
#undef SENSOR_NAME
#define SENSOR_NAME IMX335

#define CHIP_ID 0x0335

////////////////////////////////////
// Mirror-Flip Info               //
////////////////////////////////////
#define REG_MIRROR 0x304E
#define REG_FLIP 0x304F

////////////////////////////////////
// Image Info                     //
////////////////////////////////////
static struct { // LINEAR
    enum { LINEAR_RES_1 = 0,
        LINEAR_RES_END } mode;
    struct _senout {
        s32 width, height, min_fps, max_fps;
    } senout;
    struct _sensif {
        s32 crop_start_X, crop_start_y, preview_w, preview_h;
    } senif;
    struct _senstr {
        const char* strResDesc;
    } senstr;
} imx335_mipi_linear[] = {
    /* Maruko (SSC378QE / I6C) — hardware constraints:
     *
     * 1. Window mode (0x3018=0x04) does NOT work on I6C ISP.
     *    All Star6E IMX335 modes using HTRIMMING/HNUM/Y_OUT_SIZE
     *    cause the ISP pipeline to hang. Only full-readout (all-pixel,
     *    no windowing registers) produces frames.
     *
     * 2. The I6C ISP/encoder cannot process 2560x1920 above 30fps.
     *    Tested 35/45fps with both dynamic VTS and dedicated init
     *    tables — sensor init succeeds but encoder never receives data.
     *
     * 3. The IMX335 has no binning (unlike IMX415's HADD/VADD/ADDMODE),
     *    so there is no way to reduce sensor output resolution without
     *    window mode.
     *
     * Result: single mode — 2592x1944@30fps (all-pixel, HMAX=600).
     * The sensor's native full-pixel output is 2592x1944 (5MP), NOT
     * 2560x1920 as used by the Star6E driver. Using wrong dimensions
     * causes VIF frame parsing failure (ISP fence timeout).
     * Lower output resolutions via ISP/SCL downscaling (video0.size). */
    { LINEAR_RES_1, { 2592, 1944, 3, 30 }, { 0, 0, 2592, 1944 }, { "2592x1944@30fps" } },
};

u32 vts_30fps = 4125;
u32 Preview_line_period = 8080;

////////////////////////////////////
// AE Info                        //
////////////////////////////////////
#define SENSOR_MAX_GAIN (1412 * 1024)
#define SENSOR_MIN_GAIN (1 * 1024)
#define SENSOR_GAIN_DELAY_FRAME_COUNT (2)
#define SENSOR_SHUTTER_DELAY_FRAME_COUNT (2)

#if defined(SENSOR_MODULE_VERSION)
#define TO_STR_NATIVE(e) #e
#define TO_STR_PROXY(m, e) m(e)
#define MACRO_TO_STRING(e) TO_STR_PROXY(TO_STR_NATIVE, e)
static char* sensor_module_version = MACRO_TO_STRING(SENSOR_MODULE_VERSION);
module_param(sensor_module_version, charp, S_IRUGO);
#endif

typedef struct {
    struct {
        u16 pre_div0;
        u16 div124;
        u16 div_cnt7b;
        u16 sdiv0;
        u16 mipi_div0;
        u16 r_divp;
        u16 sdiv1;
        u16 r_seld5;
        u16 r_sclk_dac;
        u16 sys_sel;
        u16 pdac_sel;
        u16 adac_sel;
        u16 pre_div_sp;
        u16 r_div_sp;
        u16 div_cnt5b;
        u16 sdiv_sp;
        u16 div12_sp;
        u16 mipi_lane_sel;
        u16 div_dac;
    } clk_tree;
    struct {
        bool bVideoMode;
        u16 res_idx;
        CUS_CAMSENSOR_ORIT orit;
    } res;
    struct {
        float sclk;
        u32 hts;
        u32 vts;
        u32 ho;
        u32 xinc;
        u32 line_freq;
        u32 us_per_line;
        u32 final_us;
        u32 final_gain;
        u32 back_pv_us;
        u32 fps;
        u32 preview_fps;
        u32 expo_lines;
        u32 expo_lef_us;
    } expo;

    I2C_ARRAY tVts_reg[3];
    I2C_ARRAY tExpo_reg[3];
    I2C_ARRAY tGain_reg[2];
    bool dirty;
    bool orien_dirty;
} imx335_params;

static int pCus_SetAEUSecs(ms_cus_sensor* handle, u32 us);

///////////////////////////////////////////////////////////////
//          @@@                                              //
//         @  @@                                             //
//           @@                                              //
//          @@                                               //
//         @@@@@                                             //
//                                                           //
//      Start Step 2 --  set Sensor initial and              //
//                       adjust parameter                    //
///////////////////////////////////////////////////////////////

/* Mode 0: 2560x1920@30fps — full sensor, all-pixel scan */
const static I2C_ARRAY Sensor_init_table_4lane_5m30fps[] = {
    { 0x3002, 0x01 }, // Master mode stop
    { 0xFFFF, 0x14 }, // delay
    { 0x3000, 0x01 }, // standby
    { 0xFFFF, 0x14 }, // delay
    { 0x300C, 0x42 },
    { 0x300D, 0x2E },
    { 0x3030, 0x1D }, // VMAX
    { 0x3031, 0x10 }, // VMAX
    { 0x3032, 0x00 }, // VMAX
    { 0x3034, 0x58 }, // HMAX
    { 0x3035, 0x02 }, // HMAX
    /* SHR0 writes (0x305A/3059/3058) removed — not in Maruko SDK table.
     * AE sets SHR0 dynamically via AEStatusNotify. */
    { 0x314C, 0xB0 },
    { 0x315A, 0x02 },
    { 0x3168, 0x8F },
    { 0x316A, 0x7E },
    { 0x319D, 0x00 }, // MDBIT 0:10Bit
    { 0x31A0, 0x2A },
    { 0x31A1, 0x00 },
    { 0x31A4, 0x00 },
    { 0x31A5, 0x00 },
    { 0x31A6, 0x00 },
    { 0x31A8, 0x00 },
    { 0x31AC, 0x00 },
    { 0x31AD, 0x00 },
    { 0x31AE, 0x00 },
    { 0x31D4, 0x00 },
    { 0x31D5, 0x00 },
    { 0x31D7, 0x00 },
    { 0x31E4, 0x01 },
    { 0x31F3, 0x01 },
    { 0x3288, 0x21 },
    { 0x328A, 0x02 },
    { 0x3414, 0x05 },
    { 0x3416, 0x18 },
    { 0x341C, 0xFF }, // 10-bit
    { 0x341D, 0x01 }, // 10-bit
    { 0x3648, 0x01 },
    { 0x364A, 0x04 },
    { 0x364C, 0x04 },
    { 0x3678, 0x01 },
    { 0x367C, 0x31 },
    { 0x367E, 0x31 },
    { 0x3706, 0x10 },
    { 0x3708, 0x03 },
    { 0x3714, 0x02 },
    { 0x3715, 0x02 },
    { 0x3716, 0x01 },
    { 0x3717, 0x03 },
    { 0x371C, 0x3D },
    { 0x371D, 0x3F },
    { 0x372C, 0x00 },
    { 0x372D, 0x00 },
    { 0x372E, 0x46 },
    { 0x372F, 0x00 },
    { 0x3730, 0x89 },
    { 0x3731, 0x00 },
    { 0x3732, 0x08 },
    { 0x3733, 0x01 },
    { 0x3734, 0xFE },
    { 0x3735, 0x05 },
    { 0x3740, 0x02 },
    { 0x375D, 0x00 },
    { 0x375E, 0x00 },
    { 0x375F, 0x11 },
    { 0x3760, 0x01 },
    { 0x3768, 0x1B },
    { 0x3769, 0x1B },
    { 0x376A, 0x1B },
    { 0x376B, 0x1B },
    { 0x376C, 0x1A },
    { 0x376D, 0x17 },
    { 0x376E, 0x0F },
    { 0x3776, 0x00 },
    { 0x3777, 0x00 },
    { 0x3778, 0x46 },
    { 0x3779, 0x00 },
    { 0x377A, 0x89 },
    { 0x377B, 0x00 },
    { 0x377C, 0x08 },
    { 0x377D, 0x01 },
    { 0x377E, 0x23 },
    { 0x377F, 0x02 },
    { 0x3780, 0xD9 },
    { 0x3781, 0x03 },
    { 0x3782, 0xF5 },
    { 0x3783, 0x06 },
    { 0x3784, 0xA5 },
    { 0x3788, 0x0F },
    { 0x378A, 0xD9 },
    { 0x378B, 0x03 },
    { 0x378C, 0xEB },
    { 0x378D, 0x05 },
    { 0x378E, 0x87 },
    { 0x378F, 0x06 },
    { 0x3790, 0xF5 },
    { 0x3792, 0x43 },
    { 0x3794, 0x7A },
    { 0x3796, 0xA1 },
    { 0x3000, 0x00 }, // Standby exit (operating)
    { 0x3002, 0x00 }, // Master mode start
};

/* Window-cropped modes — DO NOT WORK on I6C ISP.
 * Kept for reference; the I6C ISP hangs when the sensor uses
 * window mode (0x3018=0x04). All active modes use mode 0's
 * full-readout table above with VTS adjustment for FPS. */
#if 0  /* disabled: window mode incompatible with I6C ISP */
const static I2C_ARRAY Sensor_init_table_4lane_5m60fps[] = {
    { 0x3002, 0x01 }, // Master mode stop
    { 0xFFFF, 0x14 }, // delay
    { 0x3000, 0x01 }, // standby
    { 0xFFFF, 0x14 }, // delay
    { 0x300C, 0x42 },
    { 0x300D, 0x2E },
    { 0x3018, 0x04 }, // window mode
    { 0x302C, 0x48 }, // HTRIMMING
    { 0x302D, 0x00 },
    { 0x302E, 0x20 }, // HNUM
    { 0x302F, 0x0A },
    { 0x3030, 0x60 }, // VMAX
    { 0x3031, 0x0F },
    { 0x3032, 0x00 },
    { 0x3034, 0x3A }, // HMAX
    { 0x3035, 0x01 },
    { 0x3056, 0x80 }, // Y_OUT_SIZE
    { 0x3057, 0x07 },
    { 0x3074, 0xC8 }, // AREA3_ST_Addr
    { 0x3075, 0x00 },
    { 0x3076, 0x00 }, // AREA3_WIDTH_1
    { 0x3077, 0x0F },
    { 0x3050, 0x00 },
    { 0x30C6, 0x00 }, // Black Offset
    { 0x30CE, 0x00 }, // UNRD_Line_Max
    { 0x30D8, 0x4C }, // UNREAD_ED_ADR
    { 0x30D9, 0x10 },
    { 0x314C, 0xB0 },
    { 0x315A, 0x02 },
    { 0x3168, 0x8F },
    { 0x316A, 0x7E },
    { 0x319D, 0x00 }, // MDBIT 0:10Bit
    { 0x31A0, 0x2A },
    { 0x31A1, 0x00 },
    { 0x31A4, 0x00 },
    { 0x31A5, 0x00 },
    { 0x31A6, 0x00 },
    { 0x31A8, 0x00 },
    { 0x31AC, 0x00 },
    { 0x31AD, 0x00 },
    { 0x31AE, 0x00 },
    { 0x31D4, 0x00 },
    { 0x31D5, 0x00 },
    { 0x31D7, 0x00 },
    { 0x31E4, 0x01 },
    { 0x31F3, 0x01 },
    { 0x3288, 0x21 },
    { 0x328A, 0x02 },
    { 0x3414, 0x05 },
    { 0x3416, 0x18 },
    { 0x341C, 0xFF }, // 10-bit
    { 0x341D, 0x01 }, // 10-bit
    { 0x3648, 0x01 },
    { 0x364A, 0x04 },
    { 0x364C, 0x04 },
    { 0x3678, 0x01 },
    { 0x367C, 0x31 },
    { 0x367E, 0x31 },
    { 0x3706, 0x10 },
    { 0x3708, 0x03 },
    { 0x3714, 0x02 },
    { 0x3715, 0x02 },
    { 0x3716, 0x01 },
    { 0x3717, 0x03 },
    { 0x371C, 0x3D },
    { 0x371D, 0x3F },
    { 0x372C, 0x00 },
    { 0x372D, 0x00 },
    { 0x372E, 0x46 },
    { 0x372F, 0x00 },
    { 0x3730, 0x89 },
    { 0x3731, 0x00 },
    { 0x3732, 0x08 },
    { 0x3733, 0x01 },
    { 0x3734, 0xFE },
    { 0x3735, 0x05 },
    { 0x3740, 0x02 },
    { 0x375D, 0x00 },
    { 0x375E, 0x00 },
    { 0x375F, 0x11 },
    { 0x3760, 0x01 },
    { 0x3768, 0x1B },
    { 0x3769, 0x1B },
    { 0x376A, 0x1B },
    { 0x376B, 0x1B },
    { 0x376C, 0x1A },
    { 0x376D, 0x17 },
    { 0x376E, 0x0F },
    { 0x3776, 0x00 },
    { 0x3777, 0x00 },
    { 0x3778, 0x46 },
    { 0x3779, 0x00 },
    { 0x377A, 0x89 },
    { 0x377B, 0x00 },
    { 0x377C, 0x08 },
    { 0x377D, 0x01 },
    { 0x377E, 0x23 },
    { 0x377F, 0x02 },
    { 0x3780, 0xD9 },
    { 0x3781, 0x03 },
    { 0x3782, 0xF5 },
    { 0x3783, 0x06 },
    { 0x3784, 0xA5 },
    { 0x3788, 0x0F },
    { 0x378A, 0xD9 },
    { 0x378B, 0x03 },
    { 0x378C, 0xEB },
    { 0x378D, 0x05 },
    { 0x378E, 0x87 },
    { 0x378F, 0x06 },
    { 0x3790, 0xF5 },
    { 0x3792, 0x43 },
    { 0x3794, 0x7A },
    { 0x3796, 0xA1 },
    { 0x3000, 0x00 },
    { 0x3002, 0x00 },
};

/* Mode 2: 2400x1350@90fps — crop(2560x1440) → output(2400x1350) */
const static I2C_ARRAY Sensor_init_table_4lane_5m90fps[] = {
    { 0x3002, 0x01 }, // Master mode stop
    { 0xFFFF, 0x14 }, // delay
    { 0x3000, 0x01 }, // standby
    { 0xFFFF, 0x14 }, // delay
    { 0x300C, 0x42 },
    { 0x300D, 0x2E },
    { 0x3018, 0x04 }, // window mode
    { 0x302C, 0x30 }, // HTRIMMING
    { 0x302D, 0x00 },
    { 0x302E, 0x18 }, // HNUM
    { 0x302F, 0x0A },
    { 0x3030, 0xC8 }, // VMAX
    { 0x3031, 0x0B },
    { 0x3032, 0x00 },
    { 0x3034, 0x13 }, // HMAX
    { 0x3035, 0x01 },
    { 0x3056, 0xB4 }, // Y_OUT_SIZE
    { 0x3057, 0x05 },
    { 0x3074, 0xA8 }, // AREA3_ST_Addr
    { 0x3075, 0x02 },
    { 0x3076, 0x68 }, // AREA3_WIDTH_1
    { 0x3077, 0x0B },
    { 0x3050, 0x00 },
    { 0x30C6, 0x12 }, // Black Offset
    { 0x30CE, 0x64 }, // UNRD_Line_Max
    { 0x30D8, 0xE0 }, // UNREAD_ED_ADR
    { 0x30D9, 0x0E },
    { 0x314C, 0xB0 },
    { 0x315A, 0x02 },
    { 0x3168, 0x8F },
    { 0x316A, 0x7E },
    { 0x319D, 0x00 }, // MDBIT 0:10Bit
    { 0x31A0, 0x2A },
    { 0x31A1, 0x00 },
    { 0x31A4, 0x00 },
    { 0x31A5, 0x00 },
    { 0x31A6, 0x00 },
    { 0x31A8, 0x00 },
    { 0x31AC, 0x00 },
    { 0x31AD, 0x00 },
    { 0x31AE, 0x00 },
    { 0x31D4, 0x00 },
    { 0x31D5, 0x00 },
    { 0x31D7, 0x00 },
    { 0x31E4, 0x01 },
    { 0x31F3, 0x01 },
    { 0x3288, 0x21 },
    { 0x328A, 0x02 },
    { 0x3414, 0x05 },
    { 0x3416, 0x18 },
    { 0x341C, 0xFF }, // 10-bit
    { 0x341D, 0x01 }, // 10-bit
    { 0x3648, 0x01 },
    { 0x364A, 0x04 },
    { 0x364C, 0x04 },
    { 0x3678, 0x01 },
    { 0x367C, 0x31 },
    { 0x367E, 0x31 },
    { 0x3706, 0x10 },
    { 0x3708, 0x03 },
    { 0x3714, 0x02 },
    { 0x3715, 0x02 },
    { 0x3716, 0x01 },
    { 0x3717, 0x03 },
    { 0x371C, 0x3D },
    { 0x371D, 0x3F },
    { 0x372C, 0x00 },
    { 0x372D, 0x00 },
    { 0x372E, 0x46 },
    { 0x372F, 0x00 },
    { 0x3730, 0x89 },
    { 0x3731, 0x00 },
    { 0x3732, 0x08 },
    { 0x3733, 0x01 },
    { 0x3734, 0xFE },
    { 0x3735, 0x05 },
    { 0x3740, 0x02 },
    { 0x375D, 0x00 },
    { 0x375E, 0x00 },
    { 0x375F, 0x11 },
    { 0x3760, 0x01 },
    { 0x3768, 0x1B },
    { 0x3769, 0x1B },
    { 0x376A, 0x1B },
    { 0x376B, 0x1B },
    { 0x376C, 0x1A },
    { 0x376D, 0x17 },
    { 0x376E, 0x0F },
    { 0x3776, 0x00 },
    { 0x3777, 0x00 },
    { 0x3778, 0x46 },
    { 0x3779, 0x00 },
    { 0x377A, 0x89 },
    { 0x377B, 0x00 },
    { 0x377C, 0x08 },
    { 0x377D, 0x01 },
    { 0x377E, 0x23 },
    { 0x377F, 0x02 },
    { 0x3780, 0xD9 },
    { 0x3781, 0x03 },
    { 0x3782, 0xF5 },
    { 0x3783, 0x06 },
    { 0x3784, 0xA5 },
    { 0x3788, 0x0F },
    { 0x378A, 0xD9 },
    { 0x378B, 0x03 },
    { 0x378C, 0xEB },
    { 0x378D, 0x05 },
    { 0x378E, 0x87 },
    { 0x378F, 0x06 },
    { 0x3790, 0xF5 },
    { 0x3792, 0x43 },
    { 0x3794, 0x7A },
    { 0x3796, 0xA1 },
    { 0x3000, 0x00 },
    { 0x3002, 0x00 },
};

/* Mode 3: 1920x1080@120fps — window-cropped */
const static I2C_ARRAY Sensor_init_table_4lane_5m120fps[] = {
    { 0x3002, 0x01 }, // Master mode stop
    { 0xFFFF, 0x14 }, // delay
    { 0x3000, 0x01 }, // standby
    { 0xFFFF, 0x14 }, // delay
    { 0x300C, 0x42 },
    { 0x300D, 0x2E },
    { 0x3018, 0x04 }, // window mode
    { 0x302C, 0x80 }, // HTRIMMING Horiz Start
    { 0x302D, 0x01 },
    { 0x302E, 0x98 }, // HNUM Horiz size
    { 0x302F, 0x07 },
    { 0x3030, 0xD0 }, // VMAX
    { 0x3031, 0x08 },
    { 0x3032, 0x00 },
    { 0x3034, 0x13 }, // HMAX
    { 0x3035, 0x01 },
    { 0x3056, 0x38 }, // Y_OUT_SIZE
    { 0x3057, 0x04 },
    { 0x3074, 0x10 }, // AREA3_ST_Addr
    { 0x3075, 0x04 },
    { 0x3076, 0x70 }, // AREA3_WIDTH_1
    { 0x3077, 0x08 },
    { 0x3050, 0x00 },
    { 0x30C6, 0x12 }, // Black Offset
    { 0x30CE, 0x64 }, // UNRD_Line_Max
    { 0x30D8, 0x50 }, // UNREAD_ED_ADR
    { 0x30D9, 0x0D },
    { 0x314C, 0xB0 },
    { 0x315A, 0x02 },
    { 0x3168, 0x8F },
    { 0x316A, 0x7E },
    { 0x319D, 0x00 }, // MDBIT 0:10Bit
    { 0x31A0, 0x2A },
    { 0x31A1, 0x00 },
    { 0x31A4, 0x00 },
    { 0x31A5, 0x00 },
    { 0x31A6, 0x00 },
    { 0x31A8, 0x00 },
    { 0x31AC, 0x00 },
    { 0x31AD, 0x00 },
    { 0x31AE, 0x00 },
    { 0x31D4, 0x00 },
    { 0x31D5, 0x00 },
    { 0x31D7, 0x00 },
    { 0x31E4, 0x01 },
    { 0x31F3, 0x01 },
    { 0x3288, 0x21 },
    { 0x328A, 0x02 },
    { 0x3414, 0x05 },
    { 0x3416, 0x18 },
    { 0x341C, 0xFF }, // 10-bit
    { 0x341D, 0x01 }, // 10-bit
    { 0x3648, 0x01 },
    { 0x364A, 0x04 },
    { 0x364C, 0x04 },
    { 0x3678, 0x01 },
    { 0x367C, 0x31 },
    { 0x367E, 0x31 },
    { 0x3706, 0x10 },
    { 0x3708, 0x03 },
    { 0x3714, 0x02 },
    { 0x3715, 0x02 },
    { 0x3716, 0x01 },
    { 0x3717, 0x03 },
    { 0x371C, 0x3D },
    { 0x371D, 0x3F },
    { 0x372C, 0x00 },
    { 0x372D, 0x00 },
    { 0x372E, 0x46 },
    { 0x372F, 0x00 },
    { 0x3730, 0x89 },
    { 0x3731, 0x00 },
    { 0x3732, 0x08 },
    { 0x3733, 0x01 },
    { 0x3734, 0xFE },
    { 0x3735, 0x05 },
    { 0x3740, 0x02 },
    { 0x375D, 0x00 },
    { 0x375E, 0x00 },
    { 0x375F, 0x11 },
    { 0x3760, 0x01 },
    { 0x3768, 0x1B },
    { 0x3769, 0x1B },
    { 0x376A, 0x1B },
    { 0x376B, 0x1B },
    { 0x376C, 0x1A },
    { 0x376D, 0x17 },
    { 0x376E, 0x0F },
    { 0x3776, 0x00 },
    { 0x3777, 0x00 },
    { 0x3778, 0x46 },
    { 0x3779, 0x00 },
    { 0x377A, 0x89 },
    { 0x377B, 0x00 },
    { 0x377C, 0x08 },
    { 0x377D, 0x01 },
    { 0x377E, 0x23 },
    { 0x377F, 0x02 },
    { 0x3780, 0xD9 },
    { 0x3781, 0x03 },
    { 0x3782, 0xF5 },
    { 0x3783, 0x06 },
    { 0x3784, 0xA5 },
    { 0x3788, 0x0F },
    { 0x378A, 0xD9 },
    { 0x378B, 0x03 },
    { 0x378C, 0xEB },
    { 0x378D, 0x05 },
    { 0x378E, 0x87 },
    { 0x378F, 0x06 },
    { 0x3790, 0xF5 },
    { 0x3792, 0x43 },
    { 0x3794, 0x7A },
    { 0x3796, 0xA1 },
    { 0x3000, 0x00 },
    { 0x3002, 0x00 },
};
#endif  /* disabled window mode tables */

const static I2C_ARRAY Sensor_id_table[] = {
    { 0x3003, 0x00 },
    { 0x3033, 0x00 },
};

static I2C_ARRAY PatternTbl[] = {
    { 0x0000, 0x00 },
};

const static I2C_ARRAY expo_reg[] = {
    // SHR0
    { 0x305A, 0x00 }, // bit0-3 (16-19)
    { 0x3059, 0x00 }, // bit0-7 (8-15)
    { 0x3058, 0x09 }, // bit0-7 (0-7)
};

const static I2C_ARRAY vts_reg[] = {
    // VMAX
    { 0x3032, 0x00 }, // bit0-3 (16-19)
    { 0x3031, 0x10 }, // bit0-7 (8-15)
    { 0x3030, 0x1D }, // bit0-7 (0-7)
};

const static I2C_ARRAY gain_reg[] = {
    { 0x30E8, 0x00 }, // low byte
    { 0x30E9, 0x00 }, // high byte (bit0-2)
};

/* Mirror/flip table — 4 orientations × 7 registers each */
const static I2C_ARRAY mirr_flip_table[] = {
    { 0x304E, 0x00 }, // M0F0
    { 0x304F, 0x00 },
    { 0x3081, 0x02 },
    { 0x3083, 0x02 },
    { 0x30B6, 0x00 },
    { 0x30B7, 0x00 },
    { 0x3016, 0x08 },

    { 0x304E, 0x01 }, // M1F0
    { 0x304F, 0x00 },
    { 0x3081, 0x02 },
    { 0x3083, 0x02 },
    { 0x30B6, 0x00 },
    { 0x30B7, 0x00 },
    { 0x3016, 0x08 },

    { 0x304E, 0x00 }, // M0F1
    { 0x304F, 0x01 },
    { 0x3081, 0xFE },
    { 0x3083, 0xFE },
    { 0x30B6, 0xFA },
    { 0x30B7, 0x01 },
    { 0x3016, 0x02 },

    { 0x304E, 0x01 }, // M1F1
    { 0x304F, 0x01 },
    { 0x3081, 0xFE },
    { 0x3083, 0xFE },
    { 0x30B6, 0xFA },
    { 0x30B7, 0x01 },
    { 0x3016, 0x02 },
};

/////////////////////////////////////////////////////////////////
//      Step 3 --  complete camera features                    //
/////////////////////////////////////////////////////////////////

#define SensorReg_Read(_reg, _data) (handle->i2c_bus->i2c_rx(handle->i2c_bus, &(handle->i2c_cfg), _reg, _data))
#define SensorReg_Write(_reg, _data) (handle->i2c_bus->i2c_tx(handle->i2c_bus, &(handle->i2c_cfg), _reg, _data))
#define SensorRegArrayW(_reg, _len) (handle->i2c_bus->i2c_array_tx(handle->i2c_bus, &(handle->i2c_cfg), (_reg), (_len)))
#define SensorRegArrayR(_reg, _len) (handle->i2c_bus->i2c_array_rx(handle->i2c_bus, &(handle->i2c_cfg), (_reg), (_len)))

/////////////////// sensor hardware dependent ///////////////////
static int cus_camsensor_release_handle(ms_cus_sensor* handle)
{
    return SUCCESS;
}

static int pCus_poweron(ms_cus_sensor* handle, u32 idx)
{
    ISensorIfAPI* sensor_if = handle->sensor_if_api;

    /* On I6C/Maruko, the sensor is already powered and outputting
     * MIPI data from kernel boot. Majestic (working reference) does
     * NOT power-cycle the sensor. Doing the full poweroff→poweron
     * sequence causes MI_SNR_Disable/Enable to break MIPI sync.
     *
     * Only configure the CSI receiver — do NOT toggle power/reset
     * pins or MCLK, keeping the sensor in its boot state. */
    sensor_if->SetIOPad(idx, handle->sif_bus, handle->interface_attr.attr_mipi.mipi_lane_num);
    sensor_if->SetCSI_Clk(idx, CUS_CSI_CLK_216M);
    sensor_if->SetCSI_Lane(idx, handle->interface_attr.attr_mipi.mipi_lane_num, ENABLE);
    sensor_if->SetCSI_LongPacketType(idx, 0, 0x1C00, 0);
    return SUCCESS;
}

static int pCus_poweroff(ms_cus_sensor* handle, u32 idx)
{
    /* No-op: keep sensor powered to preserve MIPI state.
     * See pCus_poweron comment. */
    (void)handle;
    (void)idx;
    return SUCCESS;
}

/* Hardware reset the sensor before each mode init.  Toggles the
 * RESET pin to clear all I2C register state. Without this, switching
 * between modes leaves stale windowing/timing registers that cause
 * incorrect output. */
static void pCus_HardwareReset(ms_cus_sensor* handle)
{
    ISensorIfAPI* sensor_if = handle->sensor_if_api;
    u32 idx = 0;

    sensor_if->Reset(idx, handle->reset_POLARITY);
    SENSOR_MSLEEP(5);
    sensor_if->Reset(idx, !handle->reset_POLARITY);
    SENSOR_MSLEEP(20);
    SENSOR_DMSG("[%s] sensor hardware reset complete\n", __FUNCTION__);
}

/////////////////// Check Sensor Product ID /////////////////////////
static int pCus_CheckSensorProductID(ms_cus_sensor* handle)
{
    u16 sen_id;

    SensorReg_Read(0x3003, (void*)&sen_id);
    return SUCCESS;
}

static int pCus_GetSensorID(ms_cus_sensor* handle, u32* id)
{
    int i, n;
    int table_length = ARRAY_SIZE(Sensor_id_table);
    I2C_ARRAY id_from_sensor[ARRAY_SIZE(Sensor_id_table)];

    for (n = 0; n < table_length; ++n) {
        id_from_sensor[n].reg = Sensor_id_table[n].reg;
        id_from_sensor[n].data = 0;
    }

    *id = 0;
    if (table_length > 8)
        table_length = 8;

    SENSOR_DMSG("\n\n[%s]", __FUNCTION__);

    for (n = 0; n < 4; ++n) {
        if (SensorRegArrayR((I2C_ARRAY*)id_from_sensor, table_length) == SUCCESS)
            break;
        else
            SENSOR_MSLEEP(1);
    }
    if (n >= 4)
        return FAIL;

    for (i = 0; i < table_length; ++i) {
        if (id_from_sensor[i].data != Sensor_id_table[i].data) {
            SENSOR_DMSG("[%s] Please Check IMX335 Sensor Insert!!\n", __FUNCTION__);
            return FAIL;
        }
        *id = id_from_sensor[i].data;
    }

    SENSOR_DMSG("[%s]IMX335 sensor, Read sensor id, get 0x%x Success\n", __FUNCTION__, (int)*id);
    return SUCCESS;
}

static int imx335_SetPatternMode(ms_cus_sensor* handle, u32 mode)
{
    int i;
    switch (mode) {
    case 1:
        PatternTbl[0].data = 0x21;
        break;
    case 0:
    default:
        PatternTbl[0].data &= 0xFE;
        break;
    }
    for (i = 0; i < ARRAY_SIZE(PatternTbl); i++) {
        if (SensorReg_Write(PatternTbl[i].reg, PatternTbl[i].data) != SUCCESS) {
            SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
            return FAIL;
        }
    }
    return SUCCESS;
}

/////////////////// Mode init functions ///////////////////

static int pCus_init_mipi4lane_5m30fps_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    /* Use the Maruko SDK register table (matches stock
     * drv_ms_cus_imx335_MIPI.c exactly — no SHR0 writes,
     * no hardware reset). */
    if (pCus_CheckSensorProductID(handle) == FAIL)
        return FAIL;

    for (i = 0; i < ARRAY_SIZE(Sensor_init_table_4lane_5m30fps); i++) {
        if (Sensor_init_table_4lane_5m30fps[i].reg == 0xFFFF) {
            SENSOR_MSLEEP(Sensor_init_table_4lane_5m30fps[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_init_table_4lane_5m30fps[i].reg,
                    Sensor_init_table_4lane_5m30fps[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
            }
        }
    }
    return SUCCESS;
}

/////////////////// Resolution functions ///////////////////

static int pCus_GetVideoResNum(ms_cus_sensor* handle, u32* ulres_num)
{
    *ulres_num = handle->video_res_supported.num_res;
    return SUCCESS;
}

static int pCus_GetVideoRes(ms_cus_sensor* handle, u32 res_idx, cus_camsensor_res** res)
{
    u32 num_res = handle->video_res_supported.num_res;

    if (res_idx >= num_res) {
        SENSOR_EMSG("[%s] Please check the number of resolutions supported by the sensor!\n", __FUNCTION__);
        return FAIL;
    }

    *res = &handle->video_res_supported.res[res_idx];
    return SUCCESS;
}

static int pCus_GetCurVideoRes(ms_cus_sensor* handle, u32* cur_idx, cus_camsensor_res** res)
{
    u32 num_res = handle->video_res_supported.num_res;

    *cur_idx = handle->video_res_supported.ulcur_res;

    if (*cur_idx >= num_res) {
        SENSOR_EMSG("[%s] Please check the number of resolutions supported by the sensor!\n", __FUNCTION__);
        return FAIL;
    }

    *res = &handle->video_res_supported.res[*cur_idx];
    return SUCCESS;
}

static int pCus_SetVideoRes(ms_cus_sensor* handle, u32 res_idx)
{
    imx335_params* params = (imx335_params*)handle->private_data;
    u32 num_res = handle->video_res_supported.num_res;

    if (res_idx >= num_res) {
        SENSOR_EMSG("[%s] Please check the number of resolutions supported by the sensor!\n", __FUNCTION__);
        return FAIL;
    }

    handle->video_res_supported.ulcur_res = res_idx;

    /* Single mode: 2560x1920@30fps full-readout, HMAX=600 */
    handle->video_res_supported.ulcur_res = 0;
    handle->pCus_sensor_init = pCus_init_mipi4lane_5m30fps_linear;
    vts_30fps = 4125;
    params->expo.vts = vts_30fps;
    params->expo.fps = 30;
    Preview_line_period = 8080;

    return SUCCESS;
}

/////////////////// Orientation ///////////////////

static int pCus_GetOrien(ms_cus_sensor* handle, CUS_CAMSENSOR_ORIT* orit)
{
    short Horiz_Inv = 0;
    short Verti_Inv = 0;

    SensorReg_Read(REG_MIRROR, &Horiz_Inv);
    SensorReg_Read(REG_FLIP, &Verti_Inv);
    Horiz_Inv &= 0x01;
    Verti_Inv &= 0x01;

    if (!Horiz_Inv && !Verti_Inv)
        *orit = CUS_ORIT_M0F0;
    else if (Horiz_Inv && !Verti_Inv)
        *orit = CUS_ORIT_M1F0;
    else if (!Horiz_Inv && Verti_Inv)
        *orit = CUS_ORIT_M0F1;
    else
        *orit = CUS_ORIT_M1F1;

    return SUCCESS;
}

static int pCus_SetOrien(ms_cus_sensor* handle, CUS_CAMSENSOR_ORIT orit)
{
    imx335_params* params = (imx335_params*)handle->private_data;

    handle->orient = orit;
    params->orien_dirty = true;
    return SUCCESS;
}

static int DoOrien(ms_cus_sensor* handle, CUS_CAMSENSOR_ORIT orit)
{
    int table_length = ARRAY_SIZE(mirr_flip_table);
    int seg_length = table_length / 4;
    int i, start;

    switch (orit) {
    case CUS_ORIT_M0F0:
        start = 0;
        handle->orient = CUS_ORIT_M0F0;
        break;
    case CUS_ORIT_M1F0:
        start = seg_length;
        handle->orient = CUS_ORIT_M1F0;
        break;
    case CUS_ORIT_M0F1:
        start = seg_length * 2;
        handle->orient = CUS_ORIT_M0F1;
        break;
    case CUS_ORIT_M1F1:
        start = seg_length * 3;
        handle->orient = CUS_ORIT_M1F1;
        break;
    default:
        start = 0;
        handle->orient = CUS_ORIT_M0F0;
        break;
    }

    for (i = start; i < start + seg_length; i++) {
        SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
    }

    return SUCCESS;
}

/////////////////// AE functions ///////////////////

static int pCus_GetFPS(ms_cus_sensor* handle)
{
    imx335_params* params = (imx335_params*)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 tVts = (params->tVts_reg[0].data << 16) | (params->tVts_reg[1].data << 8) | (params->tVts_reg[2].data << 0);

    if (params->expo.fps >= 1000)
        params->expo.preview_fps = (u32)(((u64)vts_30fps * max_fps * 1000) / tVts);
    else
        params->expo.preview_fps = (u32)(((u64)vts_30fps * max_fps) / tVts);

    return params->expo.preview_fps;
}

static int pCus_SetFPS(ms_cus_sensor* handle, u32 fps)
{
    imx335_params* params = (imx335_params*)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 min_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].min_fps;

    SENSOR_DMSG("\n\n[%s]", __FUNCTION__);

    if (fps >= min_fps && fps <= max_fps) {
        params->expo.fps = fps;
        params->expo.vts = (u32)(((u64)vts_30fps * max_fps * 1000 + fps * 500) / (fps * 1000));
    } else if ((fps >= (min_fps * 1000)) && (fps <= (max_fps * 1000))) {
        params->expo.fps = fps;
        params->expo.vts = (u32)(((u64)vts_30fps * max_fps * 1000 + (fps >> 1)) / fps);
    } else {
        SENSOR_DMSG("[%s] FPS %d out of range.\n", __FUNCTION__, fps);
        return FAIL;
    }

    if (params->expo.expo_lines > params->expo.vts - 2) {
        params->expo.vts = params->expo.expo_lines + 8;
    }

    pCus_SetAEUSecs(handle, params->expo.expo_lef_us);
    params->dirty = true;
    return SUCCESS;
}

static int pCus_AEStatusNotify(ms_cus_sensor* handle, CUS_CAMSENSOR_AE_STATUS_NOTIFY status)
{
    imx335_params* params = (imx335_params*)handle->private_data;

    switch (status) {
    case CUS_FRAME_INACTIVE:
        break;
    case CUS_FRAME_ACTIVE:
        if (params->dirty || params->orien_dirty) {
            SensorReg_Write(0x3001, 1); // group hold
            SensorRegArrayW((I2C_ARRAY*)params->tExpo_reg, ARRAY_SIZE(expo_reg));
            SensorRegArrayW((I2C_ARRAY*)params->tGain_reg, ARRAY_SIZE(gain_reg));
            SensorRegArrayW((I2C_ARRAY*)params->tVts_reg, ARRAY_SIZE(vts_reg));

            if (params->orien_dirty) {
                DoOrien(handle, handle->orient);
                params->orien_dirty = false;
            }
            SensorReg_Write(0x3001, 0); // group hold release
            params->dirty = false;
        }
        break;
    default:
        break;
    }
    return SUCCESS;
}

static int pCus_GetAEUSecs(ms_cus_sensor* handle, u32* us)
{
    u32 lines = 0;
    imx335_params* params = (imx335_params*)handle->private_data;

    lines |= (u32)(params->tExpo_reg[0].data & 0xff) << 16;
    lines |= (u32)(params->tExpo_reg[1].data & 0xff) << 8;
    lines |= (u32)(params->tExpo_reg[2].data & 0xff) << 0;

    *us = (lines * Preview_line_period) / 1000;
    SENSOR_DMSG("[%s] sensor expo lines/us %u,%u us\n", __FUNCTION__, lines, *us);
    return SUCCESS;
}

static int pCus_SetAEUSecs(ms_cus_sensor* handle, u32 us)
{
    u32 lines = 0, vts = 0, activeline = 0;
    imx335_params* params = (imx335_params*)handle->private_data;

    params->expo.expo_lef_us = us;

    lines = (1000 * us) / Preview_line_period;
    if (lines < 9)
        lines = 9;
    params->expo.expo_lines = lines;

    if (lines > params->expo.vts - 1)
        vts = lines + 1;
    else
        vts = params->expo.vts;

    SENSOR_DMSG("[%s] us %u, lines %u, vts %u\n", __FUNCTION__, us, lines, params->expo.vts);

    activeline = vts - lines;
    if (activeline < 9)
        activeline = 9;

    params->tExpo_reg[0].data = (activeline >> 16) & 0x000F;
    params->tExpo_reg[1].data = (activeline >> 8) & 0x00FF;
    params->tExpo_reg[2].data = (activeline >> 0) & 0x00FF;

    params->tVts_reg[0].data = (vts >> 16) & 0x000F;
    params->tVts_reg[1].data = (vts >> 8) & 0x00FF;
    params->tVts_reg[2].data = (vts >> 0) & 0x00FF;

    params->dirty = true;
    return SUCCESS;
}

static int pCus_GetAEGain(ms_cus_sensor* handle, u32* gain)
{
    imx335_params* params = (imx335_params*)handle->private_data;
    *gain = params->expo.final_gain;
    SENSOR_DMSG("[%s] get gain %u\n", __FUNCTION__, *gain);
    return SUCCESS;
}

static int pCus_SetAEGain(ms_cus_sensor* handle, u32 gain)
{
    imx335_params* params = (imx335_params*)handle->private_data;
    u64 gain_double;

    params->expo.final_gain = gain;
    if (gain < SENSOR_MIN_GAIN)
        gain = SENSOR_MIN_GAIN;
    else if (gain >= SENSOR_MAX_GAIN)
        gain = SENSOR_MAX_GAIN;

    gain_double = 20 * (intlog10(gain) - intlog10(1024));
    params->tGain_reg[0].data = (u16)(((gain_double * 10) >> 24) / 3) & 0x00FF;
    params->tGain_reg[1].data = (u16)((((gain_double * 10) >> 24) / 3) >> 8) & 0x0007;

    SENSOR_DMSG("[%s] set gain/reg=%u/0x%x 0x%x\n", __FUNCTION__, gain,
        params->tGain_reg[0].data, params->tGain_reg[1].data);
    params->dirty = true;
    return SUCCESS;
}

static int pCus_GetAEMinMaxUSecs(ms_cus_sensor* handle, u32* min, u32* max)
{
    u32 cur = handle->video_res_supported.ulcur_res;
    *min = 1;
    *max = 1000000 / imx335_mipi_linear[cur].senout.max_fps;
    return SUCCESS;
}

static int pCus_GetAEMinMaxGain(ms_cus_sensor* handle, u32* min, u32* max)
{
    *min = SENSOR_MIN_GAIN;
    *max = SENSOR_MAX_GAIN;
    return SUCCESS;
}

static int IMX335_GetShutterInfo(struct __ms_cus_sensor* handle, CUS_SHUTTER_INFO* info)
{
    u32 cur = handle->video_res_supported.ulcur_res;
    info->max = 1000000000 / imx335_mipi_linear[cur].senout.max_fps;
    info->min = (Preview_line_period * 1);
    info->step = Preview_line_period;
    return SUCCESS;
}

/////////////////// Init handle ///////////////////

int cus_camsensor_init_handle_linear(ms_cus_sensor* drv_handle)
{
    ms_cus_sensor* handle = drv_handle;
    imx335_params* params;
    int res;

    if (!handle) {
        SENSOR_DMSG("[%s] not enough memory!\n", __FUNCTION__);
        return FAIL;
    }
    SENSOR_DMSG("[%s]", __FUNCTION__);

    if (handle->private_data == NULL) {
        SENSOR_EMSG("[%s] Private data is empty!\n", __FUNCTION__);
        return FAIL;
    }

    params = (imx335_params*)handle->private_data;
    memcpy(params->tVts_reg, vts_reg, sizeof(vts_reg));
    memcpy(params->tGain_reg, gain_reg, sizeof(gain_reg));
    memcpy(params->tExpo_reg, expo_reg, sizeof(expo_reg));

    ////////////////////////////////////
    //    sensor model ID             //
    ////////////////////////////////////
    snprintf(handle->model_id, sizeof(handle->model_id), "IMX335_MIPI");

    ////////////////////////////////////
    //    i2c config                  //
    ////////////////////////////////////
    handle->i2c_cfg.mode = SENSOR_I2C_LEGACY;
    handle->i2c_cfg.fmt = SENSOR_I2C_FMT;
    handle->i2c_cfg.address = SENSOR_I2C_ADDR;
    handle->i2c_cfg.speed = SENSOR_I2C_SPEED;

    ////////////////////////////////////
    //    mclk                        //
    ////////////////////////////////////
    handle->mclk = Preview_MCLK_SPEED;

    ////////////////////////////////////
    //    sensor interface info       //
    ////////////////////////////////////
    handle->isp_type = SENSOR_ISP_TYPE;
    handle->sif_bus = SENSOR_IFBUS_TYPE;
    handle->data_prec = SENSOR_DATAPREC;
    handle->data_mode = SENSOR_DATAMODE;
    handle->bayer_id = SENSOR_BAYERID;
    handle->RGBIR_id = SENSOR_RGBIRID;
    handle->orient = SENSOR_ORIT;
    handle->interface_attr.attr_mipi.mipi_lane_num = SENSOR_MIPI_LANE_NUM;
    handle->interface_attr.attr_mipi.mipi_data_format = CUS_SEN_INPUT_FORMAT_RGB;
    handle->interface_attr.attr_mipi.mipi_yuv_order = 0;
    handle->interface_attr.attr_mipi.mipi_hsync_mode = SENSOR_MIPI_HSYNC_MODE;
    handle->interface_attr.attr_mipi.mipi_hdr_mode = CUS_HDR_MODE_NONE;
    handle->interface_attr.attr_mipi.mipi_hdr_virtual_channel_num = 0;

    ////////////////////////////////////
    //    resolution capability       //
    ////////////////////////////////////
    handle->video_res_supported.ulcur_res = 0;
    for (res = 0; res < LINEAR_RES_END; res++) {
        handle->video_res_supported.num_res = res + 1;
        handle->video_res_supported.res[res].width = imx335_mipi_linear[res].senif.preview_w;
        handle->video_res_supported.res[res].height = imx335_mipi_linear[res].senif.preview_h;
        handle->video_res_supported.res[res].max_fps = imx335_mipi_linear[res].senout.max_fps;
        handle->video_res_supported.res[res].min_fps = imx335_mipi_linear[res].senout.min_fps;
        handle->video_res_supported.res[res].crop_start_x = imx335_mipi_linear[res].senif.crop_start_X;
        handle->video_res_supported.res[res].crop_start_y = imx335_mipi_linear[res].senif.crop_start_y;
        handle->video_res_supported.res[res].nOutputWidth = imx335_mipi_linear[res].senout.width;
        handle->video_res_supported.res[res].nOutputHeight = imx335_mipi_linear[res].senout.height;
        snprintf(handle->video_res_supported.res[res].strResDesc,
            sizeof(handle->video_res_supported.res[res].strResDesc),
            "%s", imx335_mipi_linear[res].senstr.strResDesc);
    }

    ////////////////////////////////////
    //    Sensor polarity             //
    ////////////////////////////////////
    handle->pwdn_POLARITY = SENSOR_PWDN_POL;
    handle->reset_POLARITY = SENSOR_RST_POL;

    ////////////////////////////////////////
    // Sensor Status Control and Get Info //
    ////////////////////////////////////////
    handle->pCus_sensor_release = cus_camsensor_release_handle;
    handle->pCus_sensor_init = pCus_init_mipi4lane_5m30fps_linear;
    handle->pCus_sensor_poweron = pCus_poweron;
    handle->pCus_sensor_poweroff = pCus_poweroff;
    handle->pCus_sensor_GetSensorID = pCus_GetSensorID;
    handle->pCus_sensor_GetVideoResNum = pCus_GetVideoResNum;
    handle->pCus_sensor_GetVideoRes = pCus_GetVideoRes;
    handle->pCus_sensor_GetCurVideoRes = pCus_GetCurVideoRes;
    handle->pCus_sensor_SetVideoRes = pCus_SetVideoRes;

    handle->pCus_sensor_GetOrien = pCus_GetOrien;
    handle->pCus_sensor_SetOrien = pCus_SetOrien;
    handle->pCus_sensor_GetFPS = pCus_GetFPS;
    handle->pCus_sensor_SetFPS = pCus_SetFPS;
    handle->pCus_sensor_SetPatternMode = imx335_SetPatternMode;

    ////////////////////////////////////
    //    AE parameters               //
    ////////////////////////////////////
    handle->ae_gain_delay = SENSOR_GAIN_DELAY_FRAME_COUNT;
    handle->ae_shutter_delay = SENSOR_SHUTTER_DELAY_FRAME_COUNT;
    handle->ae_gain_ctrl_num = 1;
    handle->ae_shutter_ctrl_num = 1;
    handle->sat_mingain = SENSOR_MIN_GAIN;

    ////////////////////////////////////
    //  AE Control and Get Info       //
    ////////////////////////////////////
    handle->pCus_sensor_AEStatusNotify = pCus_AEStatusNotify;
    handle->pCus_sensor_GetAEUSecs = pCus_GetAEUSecs;
    handle->pCus_sensor_SetAEUSecs = pCus_SetAEUSecs;
    handle->pCus_sensor_GetAEGain = pCus_GetAEGain;
    handle->pCus_sensor_SetAEGain = pCus_SetAEGain;
    handle->pCus_sensor_GetAEMinMaxGain = pCus_GetAEMinMaxGain;
    handle->pCus_sensor_GetAEMinMaxUSecs = pCus_GetAEMinMaxUSecs;
    handle->pCus_sensor_GetShutterInfo = IMX335_GetShutterInfo;

    params->expo.vts = vts_30fps;

    return SUCCESS;
}

SENSOR_DRV_ENTRY_IMPL_END_EX(IMX335_HDR,
    cus_camsensor_init_handle_linear,
    NULL,
    NULL,
    imx335_params);
