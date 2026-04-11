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
#include <drv_sensor_init_table.h> //TODO: move this header to drv_sensor_common.h
#include <sensor_i2c_api.h>
#ifdef __cplusplus
}
#endif

SENSOR_DRV_ENTRY_IMPL_BEGIN_EX(IMX415_HDR);

#ifndef ARRAY_SIZE
#define ARRAY_SIZE CAM_OS_ARRAY_SIZE
#endif

//#define SENSOR_PAD_GROUP_SET CUS_SENSOR_PAD_GROUP_A
//#define SENSOR_CHANNEL_NUM (0)
#define SENSOR_CHANNEL_MODE CUS_SENSOR_CHANNEL_MODE_REALTIME_NORMAL
#define SENSOR_CHANNEL_MODE_SONY_DOL CUS_SENSOR_CHANNEL_MODE_RAW_STORE_HDR

//============================================
#define ENABLE 1
#define DISABLE 0
#undef SENSOR_DBG
#define SENSOR_DBG 0

#define DEBUG_INFO 0

#if SENSOR_DBG == 1
//#define SENSOR_DMSG(args...) LOGD(args)
//#define SENSOR_DMSG(args...) LOGE(args)
//#define SENSOR_DMSG(args...) printf(args)
#elif SENSOR_DBG == 0
//#define SENSOR_DMSG(args...)
#endif
///////////////////////////////////////////////////////////////
//          @@@                                              //
//         @ @@      ==  S t a r t * H e r e  ==             //
//           @@      ==  S t a r t * H e r e  ==             //
//           @@      ==  S t a r t * H e r e  ==             //
//          @@@@                                             //
//                                                           //
//      Start Step 1 --  show preview on LCM                 //
//                                                           //
//  Fill these #define value and table with correct settings //
//      camera can work and show preview on LCM              //
//                                                           //
///////////////////////////////////////////////////////////////

////////////////////////////////////
// Sensor-If Info                 //
////////////////////////////////////
// MIPI config begin.
#define SENSOR_MIPI_LANE_NUM (4)
#define SENSOR_MIPI_LANE_NUM_DOL (4)
//#define SENSOR_MIPI_HDR_MODE        (0) //0: Non-HDR mode. 1:Sony DOL mode

#define SENSOR_ISP_TYPE ISP_EXT // ISP_EXT, ISP_SOC (Non-used)
//#define SENSOR_DATAFMT             CUS_DATAFMT_BAYER    //CUS_DATAFMT_YUV, CUS_DATAFMT_BAYER
#define SENSOR_IFBUS_TYPE CUS_SENIF_BUS_MIPI // CFG //CUS_SENIF_BUS_PARL, CUS_SENIF_BUS_MIPI
#define SENSOR_MIPI_HSYNC_MODE PACKET_HEADER_EDGE1
#define SENSOR_MIPI_HSYNC_MODE_HDR_DOL PACKET_FOOTER_EDGE
#define SENSOR_DATAPREC CUS_DATAPRECISION_10
#define SENSOR_DATAPREC_DOL CUS_DATAPRECISION_12
#define SENSOR_DATAMODE CUS_SEN_10TO12_9098 // CFG
#define SENSOR_BAYERID CUS_BAYER_GB // 0h: CUS_BAYER_RG, 1h: CUS_BAYER_GR, 2h: CUS_BAYER_BG, 3h: CUS_BAYER_GB
#define SENSOR_BAYERID_HDR_DOL CUS_BAYER_GB
#define SENSOR_RGBIRID CUS_RGBIR_NONE
#define SENSOR_ORIT CUS_ORIT_M0F0 // CUS_ORIT_M0F0, CUS_ORIT_M1F0, CUS_ORIT_M0F1, CUS_ORIT_M1F1,
//#define SENSOR_YCORDER              CUS_SEN_YCODR_YC     //CUS_SEN_YCODR_YC, CUS_SEN_YCODR_CY
//#define long_packet_type_enable     0x00 //UD1~UD8 (user define)

////////////////////////////////////
// MCLK Info                      //
////////////////////////////////////
#define Preview_MCLK_SPEED CUS_CMU_CLK_27MHZ // CUS_CMU_CLK_24MHZ //CUS_CMU_CLK_37P125MHZ//CUS_CMU_CLK_27MHZ
#define Preview_MCLK_SPEED_HDR_DOL CUS_CMU_CLK_27MHZ

////////////////////////////////////
// I2C Info                       //
////////////////////////////////////
#define SENSOR_I2C_ADDR 0x34 // I2C slave address
#define SENSOR_I2C_SPEED 300000 // 200000 //300000 //240000                  //I2C speed, 60000~320000
//#define SENSOR_I2C_CHANNEL           1                 //I2C Channel
//#define SENSOR_I2C_PAD_MODE          2                 //Pad/Mode Number
#define SENSOR_I2C_LEGACY I2C_NORMAL_MODE // usally set CUS_I2C_NORMAL_MODE,  if use old OVT I2C protocol=> set CUS_I2C_LEGACY_MODE
#define SENSOR_I2C_FMT I2C_FMT_A16D8 // CUS_I2C_FMT_A8D8, CUS_I2C_FMT_A8D16, CUS_I2C_FMT_A16D8, CUS_I2C_FMT_A16D16

////////////////////////////////////
// Sensor Signal                  //
////////////////////////////////////
#define SENSOR_PWDN_POL CUS_CLK_POL_NEG // if PWDN pin High can makes sensor in power down, set CUS_CLK_POL_POS
#define SENSOR_RST_POL CUS_CLK_POL_NEG // if RESET pin High can makes sensor in reset state, set CUS_CLK_POL_NEG
                                       // VSYNC/HSYNC POL can be found in data sheet timing diagram,
                                       // Notice: the initial setting may contain VSYNC/HSYNC POL inverse settings so that condition is different.
#define SENSOR_VSYNC_POL CUS_CLK_POL_NEG // if VSYNC pin High and data bus have data, set CUS_CLK_POL_POS
#define SENSOR_HSYNC_POL CUS_CLK_POL_NEG // if HSYNC pin High and data bus have data, set CUS_CLK_POL_POS
#define SENSOR_PCLK_POL CUS_CLK_POL_NEG // depend on sensor setting, sometimes need to try CUS_CLK_POL_POS or CUS_CLK_POL_NEG

////////////////////////////////////
// Sensor ID                      //
////////////////////////////////////
// define SENSOR_ID

#undef SENSOR_NAME
#define SENSOR_NAME IMX415

#define CHIP_ID_r3F12 0x3F12
#define CHIP_ID_r3F13 0x3F13
#define CHIP_ID 0x0415

////////////////////////////////////
// Image Info                     //
////////////////////////////////////
static struct { // LINEAR
    // Modify it based on number of support resolution
    enum { LINEAR_RES_1 = 0,
        LINEAR_RES_2,
        LINEAR_RES_3,
        LINEAR_RES_4,
        LINEAR_RES_END } mode;
    // Sensor Output Image info
    struct _senout {
        s32 width, height, min_fps, max_fps;
    } senout;
    // VIF Get Image Info
    struct _sensif {
        s32 crop_start_X, crop_start_y, preview_w, preview_h;
    } senif;
    // Show resolution string
    struct _senstr {
        const char* strResDesc;
    } senstr;
} imx415_mipi_linear[] = {
    /* Maruko (SSC378QE) — optimized mode table.
     * Mode 0: non-binned near-full sensor, max quality at 30fps (97% FOV).
     * Mode 1: binned 1080p@60fps, full FOV (99%).
     * Mode 2: binned 1080p@90fps, full FOV (99%).
     * Mode 3: binned 816p@120fps, ultra-low latency (76% FOV). */
    { LINEAR_RES_1, { 3760, 2116, 3, 30 }, { 0, 0, 3760, 2116 }, { "3760x2116@30fps" } },  /* non-binned */
    { LINEAR_RES_2, { 1920, 1080, 3, 60 }, { 0, 0, 1920, 1080 }, { "1920x1080@60fps" } },  /* binned */
    { LINEAR_RES_3, { 1920, 1080, 3, 90 }, { 0, 0, 1920, 1080 }, { "1920x1080@90fps" } },  /* binned */
    { LINEAR_RES_4, { 1472, 816, 3, 120 }, { 0, 0, 1472, 816 }, { "1472x816@120fps" } },   /* binned */
};

static struct { // HDR
    // Modify it based on number of support resolution
    enum { HDR_RES_1 = 0,
        HDR_RES_2,
        HDR_RES_3,
        HDR_RES_4,
        HDR_RES_5,
        HDR_RES_6,
        HDR_RES_7,
        HDR_RES_END } mode;
    // Sensor Output Image info
    struct _hsenout {
        s32 width, height, min_fps, max_fps;
    } senout;
    // VIF Get Image Info
    struct _hsensif {
        s32 crop_start_X, crop_start_y, preview_w, preview_h;
    } senif;
    // Show resolution string
    struct _hsenstr {
        const char* strResDesc;
    } senstr;
} imx415_mipi_hdr[] = {
    { HDR_RES_1, { 3864, 2192, 3, 15 }, { 0, 0, 3840, 2160 }, { "3840x2160@15fps_HDR" } }, // Modify it
    { HDR_RES_2, { 3864, 2192, 3, 20 }, { 0, 0, 3840, 2160 }, { "3840x2160@20fps_HDR" } }, // Modify it
    { HDR_RES_3, { 3864, 2192, 3, 30 }, { 0, 0, 3840, 2160 }, { "3840x2160@30fps_HDR" } }, // Modify it
    { HDR_RES_4, { 2592, 1944, 3, 30 }, { 0, 0, 2592, 1944 }, { "2592x1944@30fps_HDR" } }, // Modify it
    { HDR_RES_5, { 2976, 1688, 3, 30 }, { 0, 0, 2944, 1656 }, { "2944x1656@30fps_HDR" } }, // Modify it
    { HDR_RES_6, { 2688, 1520, 3, 30 }, { 0, 0, 2688, 1520 }, { "2688x1520@30fps_HDR" } }, // Modify it
    { HDR_RES_7, { 1920, 1080, 3, 30 }, { 0, 0, 1920, 1080 }, { "1920x1080@30fps_HDR" } }, // Modify it
};

#define IMX415_HDR_BRL 2228

u32 vts_30fps = 2250;
u32 vts_30fps_HDR_DOL_4lane = 2250;
u32 Preview_line_period = 17778;
u32 Preview_line_period_HDR_DOL_4LANE = 29630;

////////////////////////////////////
// AE Info                        //
////////////////////////////////////
#define SENSOR_MAX_GAIN (3981 * 1024) // max sensor again, a-gain * conversion-gain*d-gain
#define SENSOR_MIN_GAIN (1 * 1024)
#define SENSOR_GAIN_DELAY_FRAME_COUNT (2)
#define SENSOR_SHUTTER_DELAY_FRAME_COUNT (2)
#define SENSOR_SHUTTER_DELAY_FRAME_COUNT_HDR_DOL (2)

////////////////////////////////////
// Mirror-Flip Info               //
////////////////////////////////////
#define MIRROR_FLIP 0x3030
#define SENSOR_NOR 0x0
#define SENSOR_MIRROR_EN 0x1 << (0)
#define SENSOR_FLIP_EN 0x1 << (1)
#define SENSOR_MIRROR_FLIP_EN 0x3

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
        u32 expo_sef_us;
    } expo;

    u32 min_shr1;
    u32 min_rhs1;
    u32 min_shr0;
    u32 max_shr0;
    u32 fsc;
    u32 skip_cnt;
    I2C_ARRAY tVts_reg[3];
    I2C_ARRAY tVts_reg_hdr[3];
    I2C_ARRAY tExpo_reg[3];
    I2C_ARRAY tSHR0_reg[3];
    I2C_ARRAY tSHR1_reg[3];
    I2C_ARRAY tRHS1_reg[3];
    I2C_ARRAY tGain_reg[2];
    I2C_ARRAY tGain_hdr_dol_lef_reg[2];
    I2C_ARRAY tGain_hdr_dol_sef_reg[2];
    bool dirty;
    bool orien_dirty;
} imx415_params;

static int pCus_SetAEUSecs(ms_cus_sensor* handle, u32 us);
static int pCus_SetAEUSecsHDR_DOL_SEF(ms_cus_sensor* handle, u32 us);
static int pCus_SetAEUSecsHDR_DOL_LEF(ms_cus_sensor* handle, u32 us);
///////////////////////////////////////////////////////////////
//          @@@                                              //
//         @  @@                                             //
//           @@                                              //
//          @@                                               //
//         @@@@@                                             //
//                                                           //
//      Start Step 2 --  set Sensor initial and              //
//                       adjust parameter                    //
//  Fill these register table with resolution settings       //
//      camera can work and show preview on LCM              //
//                                                           //
///////////////////////////////////////////////////////////////

// 3840x2160@30fps
const static I2C_ARRAY Sensor_8m_30fps_init_table_4lane_linear[] = {
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x3024, 0xCA }, // VMAX
    { 0x3025, 0x08 }, //
    { 0x3028, 0xFE }, // HMAX
    { 0x3029, 0x03 }, //
    { 0x3031, 0x00 }, // ADBIT (10bit)
    { 0x3033, 0x05 }, // SYS_MODE (891Mbps)
    { 0x3050, 0x08 }, // SHR0[19:0]
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x3116, 0x23 }, // INCKSEL2
    { 0x3118, 0xC6 }, // INCKSEL3
    { 0x311A, 0xE7 }, // INCKSEL4
    { 0x311E, 0x23 }, // INCKSEL5
    { 0x32D4, 0x21 }, // -
    { 0x32EC, 0xA1 }, // -
    { 0x3452, 0x7F }, // -
    { 0x3453, 0x03 }, // -
    { 0x358A, 0x04 }, // -
    { 0x35A1, 0x02 }, // -
    { 0x36BC, 0x0C }, // -
    { 0x36CC, 0x53 }, // -
    { 0x36CD, 0x00 }, // -
    { 0x36CE, 0x3C }, // -
    { 0x36D0, 0x8C }, // -
    { 0x36D1, 0x00 }, // -
    { 0x36D2, 0x71 }, // -
    { 0x36D4, 0x3C }, // -
    { 0x36D6, 0x53 }, // -
    { 0x36D7, 0x00 }, // -
    { 0x36D8, 0x71 }, // -
    { 0x36DA, 0x8C }, // -
    { 0x36DB, 0x00 }, // -
    { 0x3701, 0x00 }, // ADBIT1[7:0]
    { 0x3724, 0x02 }, // -
    { 0x3726, 0x02 }, // -
    { 0x3732, 0x02 }, // -
    { 0x3734, 0x03 }, // -
    { 0x3736, 0x03 }, // -
    { 0x3742, 0x03 }, // -
    { 0x3862, 0xE0 }, // -
    { 0x38CC, 0x30 }, // -
    { 0x38CD, 0x2F }, // -
    { 0x395C, 0x0C }, // -
    { 0x3A42, 0xD1 }, // -
    { 0x3A4C, 0x77 }, // -
    { 0x3AE0, 0x02 }, // -
    { 0x3AEC, 0x0C }, // -
    { 0x3B00, 0x2E }, // -
    { 0x3B06, 0x29 }, // -
    { 0x3B98, 0x25 }, // -
    { 0x3B99, 0x21 }, // -
    { 0x3B9B, 0x13 }, // -
    { 0x3B9C, 0x13 }, // -
    { 0x3B9D, 0x13 }, // -
    { 0x3B9E, 0x13 }, // -
    { 0x3BA1, 0x00 }, // -
    { 0x3BA2, 0x06 }, // -
    { 0x3BA3, 0x0B }, // -
    { 0x3BA4, 0x10 }, // -
    { 0x3BA5, 0x14 }, // -
    { 0x3BA6, 0x18 }, // -
    { 0x3BA7, 0x1A }, // -
    { 0x3BA8, 0x1A }, // -
    { 0x3BA9, 0x1A }, // -
    { 0x3BAC, 0xED }, // -
    { 0x3BAD, 0x01 }, // -
    { 0x3BAE, 0xF6 }, // -
    { 0x3BAF, 0x02 }, // -
    { 0x3BB0, 0xA2 }, // -
    { 0x3BB1, 0x03 }, // -
    { 0x3BB2, 0xE0 }, // -
    { 0x3BB3, 0x03 }, // -
    { 0x3BB4, 0xE0 }, // -
    { 0x3BB5, 0x03 }, // -
    { 0x3BB6, 0xE0 }, // -
    { 0x3BB7, 0x03 }, // -
    { 0x3BB8, 0xE0 }, // -
    { 0x3BBA, 0xE0 }, // -
    { 0x3BBC, 0xDA }, // -
    { 0x3BBE, 0x88 }, // -
    { 0x3BC0, 0x44 }, // -
    { 0x3BC2, 0x7B }, // -
    { 0x3BC4, 0xA2 }, // -
    { 0x3BC8, 0xBD }, // -
    { 0x3BCA, 0xBD }, // -
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x400C, 0x00 }, // INCKSEL6
    { 0x4018, 0x7F }, // TCLKPOST
    { 0x401A, 0x37 }, // TCLKPREPARE
    { 0x401C, 0x37 }, // TCLKTRAIL
    { 0x401E, 0xF7 }, // TCLKZERO
    { 0x401F, 0x00 }, //
    { 0x4020, 0x3F }, // THSPREPARE
    { 0x4022, 0x6F }, // THSZERO
    { 0x4024, 0x3F }, // THSTRAIL
    { 0x4026, 0x5F }, // THSEXIT
    { 0x4028, 0x2F }, // TLPX
    { 0x4074, 0x01 }, // INCKSEL7
    { 0xFFFF, 0x24 },
    { 0x3002, 0x00 }, // Master mode start
    { 0xFFFF, 0x10 },
    { 0x3000, 0x00 }, // Operating
};

// 2952x1656@60fps
const static I2C_ARRAY Sensor_5m_60fps_init_table_4lane_linear[] = {
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x301C, 0x04 }, // WINMODE (cropping mode)
    { 0x3024, 0xB8 }, // VMAX
    { 0x3025, 0x06 }, //
    { 0x3028, 0x8C }, // HMAX
    { 0x3029, 0x02 }, //
    { 0x3031, 0x00 }, // ADBIT (10bit)
    { 0x3032, 0x00 }, //
    { 0x3033, 0x08 }, // SYS_MODE
    { 0x3040, 0xBC }, // PIX_HST
    { 0x3041, 0x01 }, //
    { 0x3042, 0x88 }, // PIX_HWIDTH
    { 0x3043, 0x0B }, //
    { 0x3044, 0xF8 }, // PIX_VST
    { 0x3045, 0x01 }, //
    { 0x3046, 0xF0 }, // PIX_VWIDTH
    { 0x3047, 0x0C }, //
    { 0x3050, 0x08 }, // SHR0[19:0]
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x3116, 0x23 }, // INCKSEL2
    { 0x3118, 0xA5 }, // INCKSEL3
    { 0x311A, 0xE7 }, // INCKSEL4
    { 0x311E, 0x23 }, // INCKSEL5
    { 0x32D4, 0x21 }, // -
    { 0x32EC, 0xA1 }, // -
    { 0x3452, 0x7F }, // -
    { 0x3453, 0x03 }, // -
    { 0x358A, 0x04 }, // -
    { 0x35A1, 0x02 }, // -
    { 0x36BC, 0x0C }, // -
    { 0x36CC, 0x53 }, // -
    { 0x36CD, 0x00 }, // -
    { 0x36CE, 0x3C }, // -
    { 0x36D0, 0x8C }, // -
    { 0x36D1, 0x00 }, // -
    { 0x36D2, 0x71 }, // -
    { 0x36D4, 0x3C }, // -
    { 0x36D6, 0x53 }, // -
    { 0x36D7, 0x00 }, // -
    { 0x36D8, 0x71 }, // -
    { 0x36DA, 0x8C }, // -
    { 0x36DB, 0x00 }, // -
    { 0x3701, 0x00 }, // ADBIT1[7:0]
    { 0x3724, 0x02 }, // -
    { 0x3726, 0x02 }, // -
    { 0x3732, 0x02 }, // -
    { 0x3734, 0x03 }, // -
    { 0x3736, 0x03 }, // -
    { 0x3742, 0x03 }, // -
    { 0x3862, 0xE0 }, // -
    { 0x38CC, 0x30 }, // -
    { 0x38CD, 0x2F }, // -
    { 0x395C, 0x0C }, // -
    { 0x3A42, 0xD1 }, // -
    { 0x3A4C, 0x77 }, // -
    { 0x3AE0, 0x02 }, // -
    { 0x3AEC, 0x0C }, // -
    { 0x3B00, 0x2E }, // -
    { 0x3B06, 0x29 }, // -
    { 0x3B98, 0x25 }, // -
    { 0x3B99, 0x21 }, // -
    { 0x3B9B, 0x13 }, // -
    { 0x3B9C, 0x13 }, // -
    { 0x3B9D, 0x13 }, // -
    { 0x3B9E, 0x13 }, // -
    { 0x3BA1, 0x00 }, // -
    { 0x3BA2, 0x06 }, // -
    { 0x3BA3, 0x0B }, // -
    { 0x3BA4, 0x10 }, // -
    { 0x3BA5, 0x14 }, // -
    { 0x3BA6, 0x18 }, // -
    { 0x3BA7, 0x1A }, // -
    { 0x3BA8, 0x1A }, // -
    { 0x3BA9, 0x1A }, // -
    { 0x3BAC, 0xED }, // -
    { 0x3BAD, 0x01 }, // -
    { 0x3BAE, 0xF6 }, // -
    { 0x3BAF, 0x02 }, // -
    { 0x3BB0, 0xA2 }, // -
    { 0x3BB1, 0x03 }, // -
    { 0x3BB2, 0xE0 }, // -
    { 0x3BB3, 0x03 }, // -
    { 0x3BB4, 0xE0 }, // -
    { 0x3BB5, 0x03 }, // -
    { 0x3BB6, 0xE0 }, // -
    { 0x3BB7, 0x03 }, // -
    { 0x3BB8, 0xE0 }, // -
    { 0x3BBA, 0xE0 }, // -
    { 0x3BBC, 0xDA }, // -
    { 0x3BBE, 0x88 }, // -
    { 0x3BC0, 0x44 }, // -
    { 0x3BC2, 0x7B }, // -
    { 0x3BC4, 0xA2 }, // -
    { 0x3BC8, 0xBD }, // -
    { 0x3BCA, 0xBD }, // -
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x400C, 0x01 }, // INCKSEL6
    { 0x4018, 0xA7 }, // TCLKPOST
    { 0x401A, 0x57 }, // TCLKPREPARE
    { 0x401C, 0x5F }, // TCLKTRAIL
    { 0x401E, 0x97 }, // TCLKZERO
    { 0x401F, 0x01 }, //
    { 0x4020, 0x5F }, // THSPREPARE
    { 0x4022, 0xAF }, // THSZERO
    { 0x4024, 0x5F }, // THSTRAIL
    { 0x4026, 0x9F }, // THSEXIT
    { 0x4028, 0x4F }, // TLPX
    { 0x4074, 0x00 }, // INCKSEL7
    { 0xFFFF, 0x24 },
    { 0x3002, 0x00 }, // Master mode start
    { 0xFFFF, 0x10 },
    { 0x3000, 0x00 }, // Operating
};

// 1920x1080@90fps
const static I2C_ARRAY Sensor_2m_90fps_init_table_4lane_linear[] = {
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x3020, 0x01 }, // HADD (horizontal binning)
    { 0x3021, 0x01 }, // VADD (vertical binning)
    { 0x3022, 0x01 }, // ADDMODE (binning 2/2)
    { 0x3024, 0xCA }, // VMAX
    { 0x3025, 0x08 }, //
    { 0x3028, 0x6D }, // HMAX
    { 0x3029, 0x01 }, //
    { 0x3031, 0x00 }, // ADBIT (10bit)
    { 0x3033, 0x05 }, // SYS_MODE (891Mbps)
    { 0x3050, 0x08 }, // SHR0[19:0]
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30D9, 0x02 }, // DIG_CLP_VSTART (binning 2/2)
    { 0x30DA, 0x01 }, // DIG_CLP_VNUM (binning 2/2)
    { 0x3116, 0x23 }, // INCKSEL2
    { 0x3118, 0xC6 }, // INCKSEL3
    { 0x311A, 0xE7 }, // INCKSEL4
    { 0x311E, 0x23 }, // INCKSEL5
    { 0x32D4, 0x21 }, // -
    { 0x32EC, 0xA1 }, // -
    { 0x3452, 0x7F }, // -
    { 0x3453, 0x03 }, // -
    { 0x358A, 0x04 }, // -
    { 0x35A1, 0x02 }, // -
    { 0x36BC, 0x0C }, // -
    { 0x36CC, 0x53 }, // -
    { 0x36CD, 0x00 }, // -
    { 0x36CE, 0x3C }, // -
    { 0x36D0, 0x8C }, // -
    { 0x36D1, 0x00 }, // -
    { 0x36D2, 0x71 }, // -
    { 0x36D4, 0x3C }, // -
    { 0x36D6, 0x53 }, // -
    { 0x36D7, 0x00 }, // -
    { 0x36D8, 0x71 }, // -
    { 0x36DA, 0x8C }, // -
    { 0x36DB, 0x00 }, // -
    { 0x3701, 0x00 }, // ADBIT1[7:0]
    { 0x3724, 0x02 }, // -
    { 0x3726, 0x02 }, // -
    { 0x3732, 0x02 }, // -
    { 0x3734, 0x03 }, // -
    { 0x3736, 0x03 }, // -
    { 0x3742, 0x03 }, // -
    { 0x3862, 0xE0 }, // -
    { 0x38CC, 0x30 }, // -
    { 0x38CD, 0x2F }, // -
    { 0x395C, 0x0C }, // -
    { 0x3A42, 0xD1 }, // -
    { 0x3A4C, 0x77 }, // -
    { 0x3AE0, 0x02 }, // -
    { 0x3AEC, 0x0C }, // -
    { 0x3B00, 0x2E }, // -
    { 0x3B06, 0x29 }, // -
    { 0x3B98, 0x25 }, // -
    { 0x3B99, 0x21 }, // -
    { 0x3B9B, 0x13 }, // -
    { 0x3B9C, 0x13 }, // -
    { 0x3B9D, 0x13 }, // -
    { 0x3B9E, 0x13 }, // -
    { 0x3BA1, 0x00 }, // -
    { 0x3BA2, 0x06 }, // -
    { 0x3BA3, 0x0B }, // -
    { 0x3BA4, 0x10 }, // -
    { 0x3BA5, 0x14 }, // -
    { 0x3BA6, 0x18 }, // -
    { 0x3BA7, 0x1A }, // -
    { 0x3BA8, 0x1A }, // -
    { 0x3BA9, 0x1A }, // -
    { 0x3BAC, 0xED }, // -
    { 0x3BAD, 0x01 }, // -
    { 0x3BAE, 0xF6 }, // -
    { 0x3BAF, 0x02 }, // -
    { 0x3BB0, 0xA2 }, // -
    { 0x3BB1, 0x03 }, // -
    { 0x3BB2, 0xE0 }, // -
    { 0x3BB3, 0x03 }, // -
    { 0x3BB4, 0xE0 }, // -
    { 0x3BB5, 0x03 }, // -
    { 0x3BB6, 0xE0 }, // -
    { 0x3BB7, 0x03 }, // -
    { 0x3BB8, 0xE0 }, // -
    { 0x3BBA, 0xE0 }, // -
    { 0x3BBC, 0xDA }, // -
    { 0x3BBE, 0x88 }, // -
    { 0x3BC0, 0x44 }, // -
    { 0x3BC2, 0x7B }, // -
    { 0x3BC4, 0xA2 }, // -
    { 0x3BC8, 0xBD }, // -
    { 0x3BCA, 0xBD }, // -
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x400C, 0x00 }, // INCKSEL6
    { 0x4018, 0x7F }, // TCLKPOST
    { 0x401A, 0x37 }, // TCLKPREPARE
    { 0x401C, 0x37 }, // TCLKTRAIL
    { 0x401E, 0xF7 }, // TCLKZERO
    { 0x401F, 0x00 }, //
    { 0x4020, 0x3F }, // THSPREPARE
    { 0x4022, 0x6F }, // THSZERO
    { 0x4024, 0x3F }, // THSTRAIL
    { 0x4026, 0x5F }, // THSEXIT
    { 0x4028, 0x2F }, // TLPX
    { 0x4074, 0x01 }, // INCKSEL7
    { 0xFFFF, 0x24 },
    { 0x3002, 0x00 }, // Master mode start
    { 0xFFFF, 0x10 },
    { 0x3000, 0x00 }, // Operating
};

// 1472x816@120fps
const static I2C_ARRAY Sensor_1m_120fps_init_table_4lane_linear[] = {
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x301C, 0x04 }, // WINMODE (cropping mode)
    { 0x3020, 0x01 }, // HADD (horizontal binning)
    { 0x3021, 0x01 }, // VADD (vertical binning)
    { 0x3022, 0x01 }, // ADDMODE (binning 2/2)
    { 0x3024, 0xA4 }, // VMAX
    { 0x3025, 0x06 }, //
    { 0x3028, 0x6D }, // HMAX
    { 0x3029, 0x01 }, //
    { 0x3031, 0x00 }, // ADBIT (10bit)
    { 0x3033, 0x05 }, // SYS_MODE (891Mbps)
    { 0x3040, 0xD4 }, // PIX_HST
    { 0x3041, 0x01 }, //
    { 0x3042, 0x88 }, // PIX_HWIDTH
    { 0x3043, 0x0B }, //
    { 0x3044, 0x24 }, // PIX_VST
    { 0x3045, 0x02 }, //
    { 0x3046, 0xD8 }, // PIX_VWIDTH
    { 0x3047, 0x0C }, //
    { 0x3050, 0x08 }, // SHR0[19:0]
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30D9, 0x02 }, // DIG_CLP_VSTART (binning 2/2)
    { 0x30DA, 0x01 }, // DIG_CLP_VNUM (binning 2/2)
    { 0x3116, 0x23 }, // INCKSEL2[7:0]
    { 0x3118, 0xC6 }, // INCKSEL3[10:0]
    { 0x311A, 0xE7 }, // INCKSEL4[10:0]
    { 0x311E, 0x23 }, // INCKSEL5[7:0]
    { 0x32D4, 0x21 }, // -
    { 0x32EC, 0xA1 }, // -
    { 0x3452, 0x7F }, // -
    { 0x3453, 0x03 }, // -
    { 0x358A, 0x04 }, // -
    { 0x35A1, 0x02 }, // -
    { 0x36BC, 0x0C }, // -
    { 0x36CC, 0x53 }, // -
    { 0x36CD, 0x00 }, // -
    { 0x36CE, 0x3C }, // -
    { 0x36D0, 0x8C }, // -
    { 0x36D1, 0x00 }, // -
    { 0x36D2, 0x71 }, // -
    { 0x36D4, 0x3C }, // -
    { 0x36D6, 0x53 }, // -
    { 0x36D7, 0x00 }, // -
    { 0x36D8, 0x71 }, // -
    { 0x36DA, 0x8C }, // -
    { 0x36DB, 0x00 }, // -
    { 0x3701, 0x00 }, // ADBIT1[7:0]
    { 0x3724, 0x02 }, // -
    { 0x3726, 0x02 }, // -
    { 0x3732, 0x02 }, // -
    { 0x3734, 0x03 }, // -
    { 0x3736, 0x03 }, // -
    { 0x3742, 0x03 }, // -
    { 0x3862, 0xE0 }, // -
    { 0x38CC, 0x30 }, // -
    { 0x38CD, 0x2F }, // -
    { 0x395C, 0x0C }, // -
    { 0x3A42, 0xD1 }, // -
    { 0x3A4C, 0x77 }, // -
    { 0x3AE0, 0x02 }, // -
    { 0x3AEC, 0x0C }, // -
    { 0x3B00, 0x2E }, // -
    { 0x3B06, 0x29 }, // -
    { 0x3B98, 0x25 }, // -
    { 0x3B99, 0x21 }, // -
    { 0x3B9B, 0x13 }, // -
    { 0x3B9C, 0x13 }, // -
    { 0x3B9D, 0x13 }, // -
    { 0x3B9E, 0x13 }, // -
    { 0x3BA1, 0x00 }, // -
    { 0x3BA2, 0x06 }, // -
    { 0x3BA3, 0x0B }, // -
    { 0x3BA4, 0x10 }, // -
    { 0x3BA5, 0x14 }, // -
    { 0x3BA6, 0x18 }, // -
    { 0x3BA7, 0x1A }, // -
    { 0x3BA8, 0x1A }, // -
    { 0x3BA9, 0x1A }, // -
    { 0x3BAC, 0xED }, // -
    { 0x3BAD, 0x01 }, // -
    { 0x3BAE, 0xF6 }, // -
    { 0x3BAF, 0x02 }, // -
    { 0x3BB0, 0xA2 }, // -
    { 0x3BB1, 0x03 }, // -
    { 0x3BB2, 0xE0 }, // -
    { 0x3BB3, 0x03 }, // -
    { 0x3BB4, 0xE0 }, // -
    { 0x3BB5, 0x03 }, // -
    { 0x3BB6, 0xE0 }, // -
    { 0x3BB7, 0x03 }, // -
    { 0x3BB8, 0xE0 }, // -
    { 0x3BBA, 0xE0 }, // -
    { 0x3BBC, 0xDA }, // -
    { 0x3BBE, 0x88 }, // -
    { 0x3BC0, 0x44 }, // -
    { 0x3BC2, 0x7B }, // -
    { 0x3BC4, 0xA2 }, // -
    { 0x3BC8, 0xBD }, // -
    { 0x3BCA, 0xBD }, // -
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x400C, 0x00 }, // INCKSEL6
    { 0x4018, 0x7F }, // TCLKPOST[15:0]
    { 0x401A, 0x37 }, // TCLKPREPARE[15:0]
    { 0x401C, 0x37 }, // TCLKTRAIL[15:0]
    { 0x401E, 0xF7 }, // TCLKZERO[15:0]
    { 0x401F, 0x00 }, //
    { 0x4020, 0x3F }, // THSPREPARE[15:0]
    { 0x4022, 0x6F }, // THSZERO[15:0]
    { 0x4024, 0x3F }, // THSTRAIL[15:0]
    { 0x4026, 0x5F }, // THSEXIT[15:0]
    { 0x4028, 0x2F }, // TLPX[15:0]
    { 0x4074, 0x01 }, // INCKSEL7 [2:0]
    { 0xFFFF, 0x24 },
    { 0x3002, 0x00 }, // Master mode start
    { 0xFFFF, 0x10 },
    { 0x3000, 0x00 }, // Operating
};

const static I2C_ARRAY Sensor_init_table_4lane_HDR_DOL[] = {
    /*
    IMX415-AAQR All-pixel scan CSI-2_4lane 27MHz AD:12bit Output:12bit 891Mbps Master Mode DOL HDR 2frame VC 15fps Integration Time LEF:9.988ms SEF:0.121ms
    Tool ver : Ver6.0  vts:2250 hts:1100
    */
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x3028, 0x4C }, // HMAX[15:0]
    { 0x3029, 0x04 }, //
    { 0x302C, 0x01 }, // WDMODE[1:0]
    { 0x302D, 0x01 }, // WDSEL[1:0]
    { 0x3033, 0x05 }, // SYS_MODE[3:0]
    { 0x3050, 0xF2 }, // SHR0[19:0]
    { 0x3051, 0x0E }, //
    { 0x3054, 0x09 }, // SHR1[19:0]
    { 0x3060, 0x11 }, // RHS1[19:0]
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30CF, 0x01 }, // XVSMSKCNT_INT[1:0]
    { 0x3116, 0x23 }, // INCKSEL2[7:0]
    { 0x3118, 0xC6 }, // INCKSEL3[10:0]
    { 0x311A, 0xE7 }, // INCKSEL4[10:0]
    { 0x311E, 0x23 }, // INCKSEL5[7:0]
    { 0x32D4, 0x21 }, // -
    { 0x32EC, 0xA1 }, // -
    { 0x3452, 0x7F }, // -
    { 0x3453, 0x03 }, // -
    { 0x358A, 0x04 }, // -
    { 0x35A1, 0x02 }, // -
    { 0x36BC, 0x0C }, // -
    { 0x36CC, 0x53 }, // -
    { 0x36CD, 0x00 }, // -
    { 0x36CE, 0x3C }, // -
    { 0x36D0, 0x8C }, // -
    { 0x36D1, 0x00 }, // -
    { 0x36D2, 0x71 }, // -
    { 0x36D4, 0x3C }, // -
    { 0x36D6, 0x53 }, // -
    { 0x36D7, 0x00 }, // -
    { 0x36D8, 0x71 }, // -
    { 0x36DA, 0x8C }, // -
    { 0x36DB, 0x00 }, // -
    { 0x3724, 0x02 }, // -
    { 0x3726, 0x02 }, // -
    { 0x3732, 0x02 }, // -
    { 0x3734, 0x03 }, // -
    { 0x3736, 0x03 }, // -
    { 0x3742, 0x03 }, // -
    { 0x3862, 0xE0 }, // -
    { 0x38CC, 0x30 }, // -
    { 0x38CD, 0x2F }, // -
    { 0x395C, 0x0C }, // -
    { 0x3A42, 0xD1 }, // -
    { 0x3A4C, 0x77 }, // -
    { 0x3AE0, 0x02 }, // -
    { 0x3AEC, 0x0C }, // -
    { 0x3B00, 0x2E }, // -
    { 0x3B06, 0x29 }, // -
    { 0x3B98, 0x25 }, // -
    { 0x3B99, 0x21 }, // -
    { 0x3B9B, 0x13 }, // -
    { 0x3B9C, 0x13 }, // -
    { 0x3B9D, 0x13 }, // -
    { 0x3B9E, 0x13 }, // -
    { 0x3BA1, 0x00 }, // -
    { 0x3BA2, 0x06 }, // -
    { 0x3BA3, 0x0B }, // -
    { 0x3BA4, 0x10 }, // -
    { 0x3BA5, 0x14 }, // -
    { 0x3BA6, 0x18 }, // -
    { 0x3BA7, 0x1A }, // -
    { 0x3BA8, 0x1A }, // -
    { 0x3BA9, 0x1A }, // -
    { 0x3BAC, 0xED }, // -
    { 0x3BAD, 0x01 }, // -
    { 0x3BAE, 0xF6 }, // -
    { 0x3BAF, 0x02 }, // -
    { 0x3BB0, 0xA2 }, // -
    { 0x3BB1, 0x03 }, // -
    { 0x3BB2, 0xE0 }, // -
    { 0x3BB3, 0x03 }, // -
    { 0x3BB4, 0xE0 }, // -
    { 0x3BB5, 0x03 }, // -
    { 0x3BB6, 0xE0 }, // -
    { 0x3BB7, 0x03 }, // -
    { 0x3BB8, 0xE0 }, // -
    { 0x3BBA, 0xE0 }, // -
    { 0x3BBC, 0xDA }, // -
    { 0x3BBE, 0x88 }, // -
    { 0x3BC0, 0x44 }, // -
    { 0x3BC2, 0x7B }, // -
    { 0x3BC4, 0xA2 }, // -
    { 0x3BC8, 0xBD }, // -
    { 0x3BCA, 0xBD }, // -
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x400C, 0x00 }, // INCKSEL6
    { 0x4018, 0x7F }, // TCLKPOST[15:0]
    { 0x401A, 0x37 }, // TCLKPREPARE[15:0]
    { 0x401C, 0x37 }, // TCLKTRAIL[15:0]
    { 0x401E, 0xF7 }, // TCLKZERO[15:0]
    { 0x401F, 0x00 }, //
    { 0x4020, 0x3F }, // THSPREPARE[15:0]
    { 0x4022, 0x6F }, // THSZERO[15:0]
    { 0x4024, 0x3F }, // THSTRAIL[15:0]
    { 0x4026, 0x5F }, // THSEXIT[15:0]
    { 0x4028, 0x2F }, // TLPX[15:0]
    { 0x4074, 0x01 }, // INCKSEL7 [2:0]
    { 0xffff, 0x10 },
    { 0x3000, 0x00 }, // Operating
    { 0xffff, 0x10 },
    { 0x3002, 0x00 }, // Master mode start
    { 0x3260, 0x00 }, // Gain_Pgc_Fidmd
};

const static I2C_ARRAY Sensor_8m_20fps_init_table_4lane_HDR_DOL[] = {
    /*
        IMX415-AAQRAll-pixel scan
        CSI-2_4lane
        27MHz
        AD:10bit Output:10bit
        1485Mbps
        Master Mode DOL HDR 2frame VC
        20.002fps
        Integration Time LEF:1.002ms SEF:0.118ms
    */
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x3024, 0x7A }, // VMAX[15:0]
    { 0x3025, 0x0D }, //
    { 0x3028, 0x1A }, // HMAX[15:0]
    { 0x3029, 0x02 }, //
    { 0x302C, 0x01 }, // WDMODE[1:0]
    { 0x302D, 0x01 }, // WDSEL[1:0]
    { 0x3031, 0x00 }, //
    { 0x3032, 0x00 }, //
    { 0x3033, 0x08 }, // SYS_MODE[3:0]
    { 0x3050, 0x6A }, // SHR0[19:0]
    { 0x3051, 0x1A }, //
    { 0x3054, 0x09 }, // SHR1[19:0]
    { 0x3060, 0x19 }, // RHS1[19:0]
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30CF, 0x01 }, // XVSMSKCNT_INT[1:0]
    { 0x3116, 0x23 }, // INCKSEL2[7:0]
    { 0x3118, 0xA5 }, // INCKSEL3[10:0]
    { 0x311A, 0xE7 }, // INCKSEL4[10:0]
    { 0x311E, 0x23 }, // INCKSEL5[7:0]
    { 0x32D4, 0x21 }, // -
    { 0x32EC, 0xA1 }, // -
    { 0x3452, 0x7F }, // -
    { 0x3453, 0x03 }, // -
    { 0x358A, 0x04 }, // -
    { 0x35A1, 0x02 }, // -
    { 0x36BC, 0x0C }, // -
    { 0x36CC, 0x53 }, // -
    { 0x36CD, 0x00 }, // -
    { 0x36CE, 0x3C }, // -
    { 0x36D0, 0x8C }, // -
    { 0x36D1, 0x00 }, // -
    { 0x36D2, 0x71 }, // -
    { 0x36D4, 0x3C }, // -
    { 0x36D6, 0x53 }, // -
    { 0x36D7, 0x00 }, // -
    { 0x36D8, 0x71 }, // -
    { 0x36DA, 0x8C }, // -
    { 0x36DB, 0x00 }, // -
    { 0x3701, 0x00 }, // -
    { 0x3724, 0x02 }, // -
    { 0x3726, 0x02 }, // -
    { 0x3732, 0x02 }, // -
    { 0x3734, 0x03 }, // -
    { 0x3736, 0x03 }, // -
    { 0x3742, 0x03 }, // -
    { 0x3862, 0xE0 }, // -
    { 0x38CC, 0x30 }, // -
    { 0x38CD, 0x2F }, // -
    { 0x395C, 0x0C }, // -
    { 0x3A42, 0xD1 }, // -
    { 0x3A4C, 0x77 }, // -
    { 0x3AE0, 0x02 }, // -
    { 0x3AEC, 0x0C }, // -
    { 0x3B00, 0x2E }, // -
    { 0x3B06, 0x29 }, // -
    { 0x3B98, 0x25 }, // -
    { 0x3B99, 0x21 }, // -
    { 0x3B9B, 0x13 }, // -
    { 0x3B9C, 0x13 }, // -
    { 0x3B9D, 0x13 }, // -
    { 0x3B9E, 0x13 }, // -
    { 0x3BA1, 0x00 }, // -
    { 0x3BA2, 0x06 }, // -
    { 0x3BA3, 0x0B }, // -
    { 0x3BA4, 0x10 }, // -
    { 0x3BA5, 0x14 }, // -
    { 0x3BA6, 0x18 }, // -
    { 0x3BA7, 0x1A }, // -
    { 0x3BA8, 0x1A }, // -
    { 0x3BA9, 0x1A }, // -
    { 0x3BAC, 0xED }, // -
    { 0x3BAD, 0x01 }, // -
    { 0x3BAE, 0xF6 }, // -
    { 0x3BAF, 0x02 }, // -
    { 0x3BB0, 0xA2 }, // -
    { 0x3BB1, 0x03 }, // -
    { 0x3BB2, 0xE0 }, // -
    { 0x3BB3, 0x03 }, // -
    { 0x3BB4, 0xE0 }, // -
    { 0x3BB5, 0x03 }, // -
    { 0x3BB6, 0xE0 }, // -
    { 0x3BB7, 0x03 }, // -
    { 0x3BB8, 0xE0 }, // -
    { 0x3BBA, 0xE0 }, // -
    { 0x3BBC, 0xDA }, // -
    { 0x3BBE, 0x88 }, // -
    { 0x3BC0, 0x44 }, // -
    { 0x3BC2, 0x7B }, // -
    { 0x3BC4, 0xA2 }, // -
    { 0x3BC8, 0xBD }, // -
    { 0x3BCA, 0xBD }, // -
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x4018, 0xA7 }, // TCLKPOST[15:0]
    { 0x401A, 0x57 }, // TCLKPREPARE[15:0]
    { 0x401C, 0x5F }, // TCLKTRAIL[15:0]
    { 0x401E, 0x97 }, // TCLKZERO[15:0]
    { 0x4020, 0x5F }, // THSPREPARE[15:0]
    { 0x4022, 0xAF }, // THSZERO[15:0]
    { 0x4024, 0x5F }, // THSTRAIL[15:0]
    { 0x4026, 0x9F }, // THSEXIT[15:0]
    { 0x4028, 0x4F }, // TLPX[15:0]
    { 0xffff, 0x10 },
    { 0x3000, 0x00 }, // Operating
    { 0xffff, 0x10 },
    { 0x3002, 0x00 }, // Master mode start
    { 0x3260, 0x00 }, // Gain_Pgc_Fidmd
};

const static I2C_ARRAY Sensor_8m_30fps_init_table_4lane_HDR_DOL[] = {
    /*
        IMX415-AAQRAll-pixel scan
        CSI-2_4lane
        27MHz
        AD:10bit Output:10bit
        1485Mbps
        Master Mode DOL HDR 2frame VC
        30.002fps
        Integration Time LEF:1.002ms SEF:0.118ms
    */
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x3024, 0xFC }, // VMAX[15:0]
    { 0x3025, 0x08 }, //
    { 0x3028, 0x1A }, // HMAX[15:0]
    { 0x3029, 0x02 }, //
    { 0x302C, 0x01 }, // WDMODE[1:0]
    { 0x302D, 0x01 }, // WDSEL[1:0]
    { 0x3031, 0x00 }, //
    { 0x3032, 0x00 }, //
    { 0x3033, 0x08 }, // SYS_MODE[3:0]
    { 0x3050, 0x6E }, // SHR0[19:0]
    { 0x3051, 0x11 }, //
    { 0x3054, 0x09 }, // SHR1[19:0]
    { 0x3060, 0x19 }, // RHS1[19:0]
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30CF, 0x01 }, // XVSMSKCNT_INT[1:0]
    { 0x3116, 0x23 }, // INCKSEL2[7:0]
    { 0x3118, 0xA5 }, // INCKSEL3[10:0]
    { 0x311A, 0xE7 }, // INCKSEL4[10:0]
    { 0x311E, 0x23 }, // INCKSEL5[7:0]
    { 0x32D4, 0x21 }, // -
    { 0x32EC, 0xA1 }, // -
    { 0x3452, 0x7F }, // -
    { 0x3453, 0x03 }, // -
    { 0x358A, 0x04 }, // -
    { 0x35A1, 0x02 }, // -
    { 0x36BC, 0x0C }, // -
    { 0x36CC, 0x53 }, // -
    { 0x36CD, 0x00 }, // -
    { 0x36CE, 0x3C }, // -
    { 0x36D0, 0x8C }, // -
    { 0x36D1, 0x00 }, // -
    { 0x36D2, 0x71 }, // -
    { 0x36D4, 0x3C }, // -
    { 0x36D6, 0x53 }, // -
    { 0x36D7, 0x00 }, // -
    { 0x36D8, 0x71 }, // -
    { 0x36DA, 0x8C }, // -
    { 0x36DB, 0x00 }, // -
    { 0x3701, 0x00 }, // -
    { 0x3724, 0x02 }, // -
    { 0x3726, 0x02 }, // -
    { 0x3732, 0x02 }, // -
    { 0x3734, 0x03 }, // -
    { 0x3736, 0x03 }, // -
    { 0x3742, 0x03 }, // -
    { 0x3862, 0xE0 }, // -
    { 0x38CC, 0x30 }, // -
    { 0x38CD, 0x2F }, // -
    { 0x395C, 0x0C }, // -
    { 0x3A42, 0xD1 }, // -
    { 0x3A4C, 0x77 }, // -
    { 0x3AE0, 0x02 }, // -
    { 0x3AEC, 0x0C }, // -
    { 0x3B00, 0x2E }, // -
    { 0x3B06, 0x29 }, // -
    { 0x3B98, 0x25 }, // -
    { 0x3B99, 0x21 }, // -
    { 0x3B9B, 0x13 }, // -
    { 0x3B9C, 0x13 }, // -
    { 0x3B9D, 0x13 }, // -
    { 0x3B9E, 0x13 }, // -
    { 0x3BA1, 0x00 }, // -
    { 0x3BA2, 0x06 }, // -
    { 0x3BA3, 0x0B }, // -
    { 0x3BA4, 0x10 }, // -
    { 0x3BA5, 0x14 }, // -
    { 0x3BA6, 0x18 }, // -
    { 0x3BA7, 0x1A }, // -
    { 0x3BA8, 0x1A }, // -
    { 0x3BA9, 0x1A }, // -
    { 0x3BAC, 0xED }, // -
    { 0x3BAD, 0x01 }, // -
    { 0x3BAE, 0xF6 }, // -
    { 0x3BAF, 0x02 }, // -
    { 0x3BB0, 0xA2 }, // -
    { 0x3BB1, 0x03 }, // -
    { 0x3BB2, 0xE0 }, // -
    { 0x3BB3, 0x03 }, // -
    { 0x3BB4, 0xE0 }, // -
    { 0x3BB5, 0x03 }, // -
    { 0x3BB6, 0xE0 }, // -
    { 0x3BB7, 0x03 }, // -
    { 0x3BB8, 0xE0 }, // -
    { 0x3BBA, 0xE0 }, // -
    { 0x3BBC, 0xDA }, // -
    { 0x3BBE, 0x88 }, // -
    { 0x3BC0, 0x44 }, // -
    { 0x3BC2, 0x7B }, // -
    { 0x3BC4, 0xA2 }, // -
    { 0x3BC8, 0xBD }, // -
    { 0x3BCA, 0xBD }, // -
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x4018, 0xA7 }, // TCLKPOST[15:0]
    { 0x401A, 0x57 }, // TCLKPREPARE[15:0]
    { 0x401C, 0x5F }, // TCLKTRAIL[15:0]
    { 0x401E, 0x97 }, // TCLKZERO[15:0]
    { 0x4020, 0x5F }, // THSPREPARE[15:0]
    { 0x4022, 0xAF }, // THSZERO[15:0]
    { 0x4024, 0x5F }, // THSTRAIL[15:0]
    { 0x4026, 0x9F }, // THSEXIT[15:0]
    { 0x4028, 0x4F }, // TLPX[15:0]
    { 0xffff, 0x10 },
    { 0x3000, 0x00 }, // Operating
    { 0xffff, 0x10 },
    { 0x3002, 0x00 }, // Master mode start
    { 0x3260, 0x00 }, // Gain_Pgc_Fidmd
};

const static I2C_ARRAY Sensor_5m_30fps_init_table_4lane_HDR_DOL[] = {
    /*
        IMX415-AAQR
        Window cropping2592x1944
        CSI-2_4lane27MHz
        AD:10bit Output:10bit
        1485Mbps
        Master Mode DOL HDR 2frame VC
        30.002fps
        Integration Time LEF:1.002ms SEF:0.118ms

    */
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x301C, 0x04 }, //
    { 0x3024, 0xFC }, // VMAX[15:0]
    { 0x3025, 0x08 }, //
    { 0x3028, 0x1A }, // HMAX[15:0]
    { 0x3029, 0x02 }, //
    { 0x302C, 0x01 }, // WDMODE[1:0]
    { 0x302D, 0x01 }, // WDSEL[1:0]
    { 0x3031, 0x00 }, //
    { 0x3032, 0x00 }, //
    { 0x3033, 0x08 }, // SYS_MODE[3:0]
    { 0x3040, 0x7C }, //
    { 0x3041, 0x02 }, //
    { 0x3042, 0x20 }, //
    { 0x3043, 0x0A }, //
    { 0x3044, 0xF8 }, //
    { 0x3045, 0x00 }, //
    { 0x3046, 0x30 }, //
    { 0x3047, 0x0F }, //
    { 0x3050, 0x6E }, // SHR0[19:0]
    { 0x3051, 0x11 }, //
    { 0x3054, 0x09 }, // SHR1[19:0]
    { 0x3060, 0x19 }, // RHS1[19:0]
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30CF, 0x01 }, // XVSMSKCNT_INT[1:0]
    { 0x3116, 0x23 }, // INCKSEL2[7:0]
    { 0x3118, 0xA5 }, // INCKSEL3[10:0]
    { 0x311A, 0xE7 }, // INCKSEL4[10:0]
    { 0x311E, 0x23 }, // INCKSEL5[7:0]
    { 0x32D4, 0x21 }, // -
    { 0x32EC, 0xA1 }, // -
    { 0x3452, 0x7F }, // -
    { 0x3453, 0x03 }, // -
    { 0x358A, 0x04 }, // -
    { 0x35A1, 0x02 }, // -
    { 0x36BC, 0x0C }, // -
    { 0x36CC, 0x53 }, // -
    { 0x36CD, 0x00 }, // -
    { 0x36CE, 0x3C }, // -
    { 0x36D0, 0x8C }, // -
    { 0x36D1, 0x00 }, // -
    { 0x36D2, 0x71 }, // -
    { 0x36D4, 0x3C }, // -
    { 0x36D6, 0x53 }, // -
    { 0x36D7, 0x00 }, // -
    { 0x36D8, 0x71 }, // -
    { 0x36DA, 0x8C }, // -
    { 0x36DB, 0x00 }, // -
    { 0x3701, 0x00 }, // -
    { 0x3724, 0x02 }, // -
    { 0x3726, 0x02 }, // -
    { 0x3732, 0x02 }, // -
    { 0x3734, 0x03 }, // -
    { 0x3736, 0x03 }, // -
    { 0x3742, 0x03 }, // -
    { 0x3862, 0xE0 }, // -
    { 0x38CC, 0x30 }, // -
    { 0x38CD, 0x2F }, // -
    { 0x395C, 0x0C }, // -
    { 0x3A42, 0xD1 }, // -
    { 0x3A4C, 0x77 }, // -
    { 0x3AE0, 0x02 }, // -
    { 0x3AEC, 0x0C }, // -
    { 0x3B00, 0x2E }, // -
    { 0x3B06, 0x29 }, // -
    { 0x3B98, 0x25 }, // -
    { 0x3B99, 0x21 }, // -
    { 0x3B9B, 0x13 }, // -
    { 0x3B9C, 0x13 }, // -
    { 0x3B9D, 0x13 }, // -
    { 0x3B9E, 0x13 }, // -
    { 0x3BA1, 0x00 }, // -
    { 0x3BA2, 0x06 }, // -
    { 0x3BA3, 0x0B }, // -
    { 0x3BA4, 0x10 }, // -
    { 0x3BA5, 0x14 }, // -
    { 0x3BA6, 0x18 }, // -
    { 0x3BA7, 0x1A }, // -
    { 0x3BA8, 0x1A }, // -
    { 0x3BA9, 0x1A }, // -
    { 0x3BAC, 0xED }, // -
    { 0x3BAD, 0x01 }, // -
    { 0x3BAE, 0xF6 }, // -
    { 0x3BAF, 0x02 }, // -
    { 0x3BB0, 0xA2 }, // -
    { 0x3BB1, 0x03 }, // -
    { 0x3BB2, 0xE0 }, // -
    { 0x3BB3, 0x03 }, // -
    { 0x3BB4, 0xE0 }, // -
    { 0x3BB5, 0x03 }, // -
    { 0x3BB6, 0xE0 }, // -
    { 0x3BB7, 0x03 }, // -
    { 0x3BB8, 0xE0 }, // -
    { 0x3BBA, 0xE0 }, // -
    { 0x3BBC, 0xDA }, // -
    { 0x3BBE, 0x88 }, // -
    { 0x3BC0, 0x44 }, // -
    { 0x3BC2, 0x7B }, // -
    { 0x3BC4, 0xA2 }, // -
    { 0x3BC8, 0xBD }, // -
    { 0x3BCA, 0xBD }, // -
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x4018, 0xA7 }, // TCLKPOST[15:0]
    { 0x401A, 0x57 }, // TCLKPREPARE[15:0]
    { 0x401C, 0x5F }, // TCLKTRAIL[15:0]
    { 0x401E, 0x97 }, // TCLKZERO[15:0]
    { 0x4020, 0x5F }, // THSPREPARE[15:0]
    { 0x4022, 0xAF }, // THSZERO[15:0]
    { 0x4024, 0x5F }, // THSTRAIL[15:0]
    { 0x4026, 0x9F }, // THSEXIT[15:0]
    { 0x4028, 0x4F }, // TLPX[15:0]
    { 0xffff, 0x10 },
    { 0x3000, 0x00 }, // Operating
    { 0xffff, 0x10 },
    { 0x3002, 0x00 }, // Master mode start
    { 0x3260, 0x00 }, // Gain_Pgc_Fidmd
};

const static I2C_ARRAY Sensor_4p8m_30fps_init_table_4lane_HDR_DOL[] = {
    /*
    "IMX415-AAQR Window cropping 2976x1686 CSI-2_4lane 27MHz
     AD:10bit Output:10bit 1485Mbps
     Master Mode DOL HDR 2frame VC 30.002fps
     Integration Time LEF:1.002ms SEF:0.118ms"
    */
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x301C, 0x04 }, //
    { 0x3024, 0xFC }, // VMAX[15:0] FC
    { 0x3025, 0x08 }, //
    { 0x3028, 0x1A }, // HMAX[15:0] 1A
    { 0x3029, 0x02 }, //
    { 0x302C, 0x01 }, // WDMODE[1:0]
    { 0x302D, 0x01 }, // WDSEL[1:0]
    { 0x3031, 0x00 }, //
    { 0x3032, 0x00 }, //
    { 0x3033, 0x08 }, // SYS_MODE[3:0]
    { 0x3040, 0xBC }, //
    { 0x3041, 0x01 }, //            01
    { 0x3042, 0xA0 }, //
    { 0x3043, 0x0B }, //
    { 0x3044, 0xF8 }, //
    { 0x3045, 0x01 }, //            01
    { 0x3046, 0x30 }, //
    { 0x3047, 0x0D }, //
    { 0x3050, 0x6E }, // SHR0[19:0]
    { 0x3051, 0x11 }, //
    { 0x3054, 0x09 }, // SHR1[19:0]
    { 0x3060, 0x19 }, // RHS1[19:0]
    { 0x30C0, 0x2A },
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30CC, 0x00 },
    { 0x30CD, 0x00 },
    { 0x30CF, 0x01 }, // XVSMSKCNT_INT[1:0]
    { 0x3116, 0x23 }, // INCKSEL2[7:0]
    { 0x3118, 0xA5 }, // INCKSEL3[10:0]
    { 0x311A, 0xE7 }, // INCKSEL4[10:0]
    { 0x311E, 0x23 }, // INCKSEL5[7:0]
    { 0x32D4, 0x21 }, // -
    { 0x32EC, 0xA1 }, // -
    { 0x3452, 0x7F }, // -
    { 0x3453, 0x03 }, // -
    { 0x358A, 0x04 }, // -
    { 0x35A1, 0x02 }, // -
    { 0x36BC, 0x0C }, // -
    { 0x36CC, 0x53 }, // -
    { 0x36CD, 0x00 }, // -
    { 0x36CE, 0x3C }, // -
    { 0x36D0, 0x8C }, // -
    { 0x36D1, 0x00 }, // -
    { 0x36D2, 0x71 }, // -
    { 0x36D4, 0x3C }, // -
    { 0x36D6, 0x53 }, // -
    { 0x36D7, 0x00 }, // -
    { 0x36D8, 0x71 }, // -
    { 0x36DA, 0x8C }, // -
    { 0x36DB, 0x00 }, // -
    { 0x3701, 0x00 }, // -
    { 0x3724, 0x02 }, // -
    { 0x3726, 0x02 }, // -
    { 0x3732, 0x02 }, // -
    { 0x3734, 0x03 }, // -
    { 0x3736, 0x03 }, // -
    { 0x3742, 0x03 }, // -
    { 0x3862, 0xE0 }, // -
    { 0x38CC, 0x30 }, // -
    { 0x38CD, 0x2F }, // -
    { 0x395C, 0x0C }, // -
    { 0x3A42, 0xD1 }, // -
    { 0x3A4C, 0x77 }, // -
    { 0x3AE0, 0x02 }, // -
    { 0x3AEC, 0x0C }, // -
    { 0x3B00, 0x2E }, // -
    { 0x3B06, 0x29 }, // -
    { 0x3B98, 0x25 }, // -
    { 0x3B99, 0x21 }, // -
    { 0x3B9B, 0x13 }, // -
    { 0x3B9C, 0x13 }, // -
    { 0x3B9D, 0x13 }, // -
    { 0x3B9E, 0x13 }, // -
    { 0x3BA1, 0x00 }, // -
    { 0x3BA2, 0x06 }, // -
    { 0x3BA3, 0x0B }, // -
    { 0x3BA4, 0x10 }, // -
    { 0x3BA5, 0x14 }, // -
    { 0x3BA6, 0x18 }, // -
    { 0x3BA7, 0x1A }, // -
    { 0x3BA8, 0x1A }, // -
    { 0x3BA9, 0x1A }, // -
    { 0x3BAC, 0xED }, // -
    { 0x3BAD, 0x01 }, // -
    { 0x3BAE, 0xF6 }, // -
    { 0x3BAF, 0x02 }, // -
    { 0x3BB0, 0xA2 }, // -
    { 0x3BB1, 0x03 }, // -
    { 0x3BB2, 0xE0 }, // -
    { 0x3BB3, 0x03 }, // -
    { 0x3BB4, 0xE0 }, // -
    { 0x3BB5, 0x03 }, // -
    { 0x3BB6, 0xE0 }, // -
    { 0x3BB7, 0x03 }, // -
    { 0x3BB8, 0xE0 }, // -
    { 0x3BBA, 0xE0 }, // -
    { 0x3BBC, 0xDA }, // -
    { 0x3BBE, 0x88 }, // -
    { 0x3BC0, 0x44 }, // -
    { 0x3BC2, 0x7B }, // -
    { 0x3BC4, 0xA2 }, // -
    { 0x3BC8, 0xBD }, // -
    { 0x3BCA, 0xBD }, // -
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x4018, 0xA7 }, // TCLKPOST[15:0]
    { 0x401A, 0x57 }, // TCLKPREPARE[15:0]
    { 0x401C, 0x5F }, // TCLKTRAIL[15:0]
    { 0x401E, 0x97 }, // TCLKZERO[15:0]
    { 0x4020, 0x5F }, // THSPREPARE[15:0]
    { 0x4022, 0xAF }, // THSZERO[15:0]
    { 0x4024, 0x5F }, // THSTRAIL[15:0]
    { 0x4026, 0x9F }, // THSEXIT[15:0]
    { 0x4028, 0x4F }, // TLPX[15:0]
    { 0xffff, 0x10 },
    { 0x3000, 0x00 }, // Operating
    { 0xffff, 0x10 },
    { 0x3002, 0x00 }, // Master mode start
    { 0x3260, 0x00 }, // Gain_Pgc_Fidmd
};

const static I2C_ARRAY Sensor_4m_30fps_init_table_4lane_HDR_DOL[] = {
    /*
        "IMX415-AAQR Window cropping 2688x1520
        CSI-2_4lane 27MHz AD:10bit Output:10bit 1485Mbps
        Master Mode DOL HDR 2frame VC
        30.002fps Integration Time LEF:1.002ms SEF:0.118ms"
    */
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x301C, 0x04 }, //
    { 0x3024, 0xFC }, // VMAX[15:0]
    { 0x3025, 0x08 }, //
    { 0x3028, 0x1A }, // HMAX[15:0]
    { 0x3029, 0x02 }, //
    { 0x302C, 0x01 }, // WDMODE[1:0]
    { 0x302D, 0x01 }, // WDSEL[1:0]
    { 0x3031, 0x00 }, //
    { 0x3032, 0x00 }, //
    { 0x3033, 0x08 }, // SYS_MODE[3:0]
    { 0x3040, 0x4C }, //
    { 0x3041, 0x01 }, //
    { 0x3042, 0x80 }, //
    { 0x3043, 0x0A }, //
    { 0x3044, 0xA0 }, //
    { 0x3045, 0x01 }, //
    { 0x3046, 0xE0 }, //
    { 0x3047, 0x0B }, //
    { 0x3050, 0x6E }, // SHR0[19:0]
    { 0x3051, 0x11 }, //
    { 0x3054, 0x09 }, // SHR1[19:0]
    { 0x3060, 0x19 }, // RHS1[19:0]
    { 0x30C0, 0x2A },
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30CC, 0x00 },
    { 0x30CD, 0x00 },
    { 0x30CF, 0x01 }, // XVSMSKCNT_INT[1:0]
    { 0x3116, 0x23 }, // INCKSEL2[7:0]
    { 0x3118, 0xA5 }, // INCKSEL3[10:0]
    { 0x311A, 0xE7 }, // INCKSEL4[10:0]
    { 0x311E, 0x23 }, // INCKSEL5[7:0]
    { 0x32D4, 0x21 }, // -
    { 0x32EC, 0xA1 }, // -
    { 0x3452, 0x7F }, // -
    { 0x3453, 0x03 }, // -
    { 0x358A, 0x04 }, // -
    { 0x35A1, 0x02 }, // -
    { 0x36BC, 0x0C }, // -
    { 0x36CC, 0x53 }, // -
    { 0x36CD, 0x00 }, // -
    { 0x36CE, 0x3C }, // -
    { 0x36D0, 0x8C }, // -
    { 0x36D1, 0x00 }, // -
    { 0x36D2, 0x71 }, // -
    { 0x36D4, 0x3C }, // -
    { 0x36D6, 0x53 }, // -
    { 0x36D7, 0x00 }, // -
    { 0x36D8, 0x71 }, // -
    { 0x36DA, 0x8C }, // -
    { 0x36DB, 0x00 }, // -
    { 0x3701, 0x00 }, // -
    { 0x3724, 0x02 }, // -
    { 0x3726, 0x02 }, // -
    { 0x3732, 0x02 }, // -
    { 0x3734, 0x03 }, // -
    { 0x3736, 0x03 }, // -
    { 0x3742, 0x03 }, // -
    { 0x3862, 0xE0 }, // -
    { 0x38CC, 0x30 }, // -
    { 0x38CD, 0x2F }, // -
    { 0x395C, 0x0C }, // -
    { 0x3A42, 0xD1 }, // -
    { 0x3A4C, 0x77 }, // -
    { 0x3AE0, 0x02 }, // -
    { 0x3AEC, 0x0C }, // -
    { 0x3B00, 0x2E }, // -
    { 0x3B06, 0x29 }, // -
    { 0x3B98, 0x25 }, // -
    { 0x3B99, 0x21 }, // -
    { 0x3B9B, 0x13 }, // -
    { 0x3B9C, 0x13 }, // -
    { 0x3B9D, 0x13 }, // -
    { 0x3B9E, 0x13 }, // -
    { 0x3BA1, 0x00 }, // -
    { 0x3BA2, 0x06 }, // -
    { 0x3BA3, 0x0B }, // -
    { 0x3BA4, 0x10 }, // -
    { 0x3BA5, 0x14 }, // -
    { 0x3BA6, 0x18 }, // -
    { 0x3BA7, 0x1A }, // -
    { 0x3BA8, 0x1A }, // -
    { 0x3BA9, 0x1A }, // -
    { 0x3BAC, 0xED }, // -
    { 0x3BAD, 0x01 }, // -
    { 0x3BAE, 0xF6 }, // -
    { 0x3BAF, 0x02 }, // -
    { 0x3BB0, 0xA2 }, // -
    { 0x3BB1, 0x03 }, // -
    { 0x3BB2, 0xE0 }, // -
    { 0x3BB3, 0x03 }, // -
    { 0x3BB4, 0xE0 }, // -
    { 0x3BB5, 0x03 }, // -
    { 0x3BB6, 0xE0 }, // -
    { 0x3BB7, 0x03 }, // -
    { 0x3BB8, 0xE0 }, // -
    { 0x3BBA, 0xE0 }, // -
    { 0x3BBC, 0xDA }, // -
    { 0x3BBE, 0x88 }, // -
    { 0x3BC0, 0x44 }, // -
    { 0x3BC2, 0x7B }, // -
    { 0x3BC4, 0xA2 }, // -
    { 0x3BC8, 0xBD }, // -
    { 0x3BCA, 0xBD }, // -
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x4018, 0xA7 }, // TCLKPOST[15:0]
    { 0x401A, 0x57 }, // TCLKPREPARE[15:0]
    { 0x401C, 0x5F }, // TCLKTRAIL[15:0]
    { 0x401E, 0x97 }, // TCLKZERO[15:0]
    { 0x4020, 0x5F }, // THSPREPARE[15:0]
    { 0x4022, 0xAF }, // THSZERO[15:0]
    { 0x4024, 0x5F }, // THSTRAIL[15:0]
    { 0x4026, 0x9F }, // THSEXIT[15:0]
    { 0x4028, 0x4F }, // TLPX[15:0]
    { 0xffff, 0x10 },
    { 0x3000, 0x00 }, // Operating
    { 0xffff, 0x10 },
    { 0x3002, 0x00 }, // Master mode start
    { 0x3260, 0x00 }, // Gain_Pgc_Fidmd
};

const static I2C_ARRAY Sensor_2m_30fps_init_table_4lane_HDR_DOL[] = {
    /*
    "IMX415-AAQR 2/2-line binning CSI-2_4lane 27MHz
     AD:10bit Output:12bit 891Mbps
     Master Mode DOL HDR 2frame VC
     29.999fps Integration Time LEF:1.004ms SEF:0.118ms"
    */
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]

    { 0x3020, 0x01 }, // 01h: Horizontal 2 binning
    { 0x3021, 0x01 }, // 01h: Vertical 2 binning
    { 0x3022, 0x01 }, // 01h: Horizontal/Vertical 2x2-line binning(RGB only)
    { 0x3024, 0xF8 }, // MAX[15:0]
    { 0x3025, 0x08 }, //
    { 0x3028, 0x1B }, // HMAX[15:0]
    { 0x3029, 0x02 }, //
    { 0x302C, 0x01 }, // WDMODE[1:0]
    { 0x302D, 0x01 }, // WDSEL[1:0]
    { 0x3031, 0x00 }, //
    { 0x3033, 0x05 }, // SYS_MODE[3:0]
    { 0x3050, 0x66 }, // SHR0[19:0]
    { 0x3051, 0x11 }, //
    { 0x3054, 0x09 }, // SHR1[19:0]
    { 0x3060, 0x19 }, // RHS1[19:0]
    { 0x30C0, 0x2A },
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30CC, 0x00 },
    { 0x30CD, 0x00 },
    { 0x30CF, 0x01 }, // XVSMSKCNT_INT[1:0]
    { 0x30D9, 0x02 },
    { 0x30DA, 0x01 },
    { 0x3116, 0x23 }, // INCKSEL2[7:0]
    { 0x3118, 0xC6 }, // INCKSEL3[10:0]
    { 0x311A, 0xE7 }, // INCKSEL4[10:0]
    { 0x311E, 0x23 }, // INCKSEL5[7:0]
    { 0x32D4, 0x21 }, // --
    { 0x32EC, 0xA1 }, // --
    { 0x3452, 0x7F }, // --
    { 0x3453, 0x03 }, // --
    { 0x358A, 0x04 }, // --
    { 0x35A1, 0x02 }, // --
    { 0x36BC, 0x0C }, // --
    { 0x36CC, 0x53 }, // --
    { 0x36CD, 0x00 }, // --
    { 0x36CE, 0x3C }, // --
    { 0x36D0, 0x8C }, // --
    { 0x36D1, 0x00 }, // --
    { 0x36D2, 0x71 }, // --
    { 0x36D4, 0x3C }, // --
    { 0x36D6, 0x53 }, // --
    { 0x36D7, 0x00 }, // --
    { 0x36D8, 0x71 }, // --
    { 0x36DA, 0x8C }, // --
    { 0x36DB, 0x00 }, // --
    { 0x3701, 0x00 }, // --
    { 0x3724, 0x02 }, // --
    { 0x3726, 0x02 }, // --
    { 0x3732, 0x02 }, // --
    { 0x3734, 0x03 }, // --
    { 0x3736, 0x03 }, // --
    { 0x3742, 0x03 }, // --
    { 0x3862, 0xE0 }, // --
    { 0x38CC, 0x30 }, // --
    { 0x38CD, 0x2F }, // --
    { 0x395C, 0x0C }, // --
    { 0x3A42, 0xD1 }, // --
    { 0x3A4C, 0x77 }, // --
    { 0x3AE0, 0x02 }, // --
    { 0x3AEC, 0x0C }, // --
    { 0x3B00, 0x2E }, // --
    { 0x3B06, 0x29 }, // --
    { 0x3B98, 0x25 }, // --
    { 0x3B99, 0x21 }, // --
    { 0x3B9B, 0x13 }, // --
    { 0x3B9C, 0x13 }, // --
    { 0x3B9D, 0x13 }, // --
    { 0x3B9E, 0x13 }, // --
    { 0x3BA1, 0x00 }, // --
    { 0x3BA2, 0x06 }, // --
    { 0x3BA3, 0x0B }, // --
    { 0x3BA4, 0x10 }, // --
    { 0x3BA5, 0x14 }, // --
    { 0x3BA6, 0x18 }, // --
    { 0x3BA7, 0x1A }, // --
    { 0x3BA8, 0x1A }, // --
    { 0x3BA9, 0x1A }, // --
    { 0x3BAC, 0xED }, // --
    { 0x3BAD, 0x01 }, // --
    { 0x3BAE, 0xF6 }, // --
    { 0x3BAF, 0x02 }, // --
    { 0x3BB0, 0xA2 }, // --
    { 0x3BB1, 0x03 }, // --
    { 0x3BB2, 0xE0 }, // --
    { 0x3BB3, 0x03 }, // --
    { 0x3BB4, 0xE0 }, // --
    { 0x3BB5, 0x03 }, // --
    { 0x3BB6, 0xE0 }, // --
    { 0x3BB7, 0x03 }, // --
    { 0x3BB8, 0xE0 }, // --
    { 0x3BBA, 0xE0 }, // --
    { 0x3BBC, 0xDA }, // --
    { 0x3BBE, 0x88 }, // --
    { 0x3BC0, 0x44 }, // --
    { 0x3BC2, 0x7B }, // --
    { 0x3BC4, 0xA2 }, // --
    { 0x3BC8, 0xBD }, // --
    { 0x3BCA, 0xBD }, // --
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x400C, 0x00 },
    { 0x4018, 0x7F }, // TCLKPOST[15:0]
    { 0x401A, 0x37 }, // TCLKPREPARE[15:0]
    { 0x401C, 0x37 }, // TCLKTRAIL[15:0]
    { 0x401E, 0xF7 }, // TCLKZERO[15:0]
    { 0x401F, 0x00 },
    { 0x4020, 0x3F }, // THSPREPARE[15:0]
    { 0x4022, 0x6F }, // THSZERO[15:0]
    { 0x4024, 0x3F }, // THSTRAIL[15:0]
    { 0x4026, 0x5F }, // THSEXIT[15:0]
    { 0x4028, 0x2F }, // TLPX[15:0]
    { 0x4074, 0x01 }, // TLPX[15:0]
    { 0xffff, 0x10 },
    { 0x3000, 0x00 }, // Operating
    { 0xffff, 0x10 },
    { 0x3002, 0x00 }, // Master mode start
    { 0x3260, 0x00 }, // Gain_Pgc_Fidmd
};

const static I2C_ARRAY Sensor_id_table[] = {
    { 0x3F12, 0x14 }, // {address of ID, ID },
    { 0x3F13, 0x75 }, // {address of ID, ID },
};

static I2C_ARRAY PatternTbl[] = {
    { 0x0000, 0x00 }, // colorbar pattern , bit 0 to enable
};

const static I2C_ARRAY expo_reg[] = {
    // SHS0 (For Linear)
    { 0x3052, 0x00 },
    { 0x3051, 0x00 },
    { 0x3050, 0x08 },
};

const static I2C_ARRAY expo_SHR0_reg[] = {
    // SHS0 (For LEF)
    { 0x3052, 0x00 },
    { 0x3051, 0x16 },
    { 0x3050, 0x22 },
};

const static I2C_ARRAY expo_SHR1_reg[] = {
    // SHS1 (For SEF)
    // decreasing exposure ratio version.
    { 0x3056, 0x00 },
    { 0x3055, 0x00 },
    { 0x3054, 0x09 },
};

const static I2C_ARRAY vts_reg[] = {
    // VMAX
    { 0x3026, 0x00 }, // bit0-3-->MSB
    { 0x3025, 0x08 },
    { 0x3024, 0xCA },
};

const static I2C_ARRAY vts_reg_hdr[] = {
    { 0x3026, 0x00 }, // bit0-3-->MSB
    { 0x3025, 0x0B },
    { 0x3024, 0x3B },
};

const I2C_ARRAY expo_RHS1_reg[] = {
    // decreasing exposure ratio version.
    { 0x3062, 0x00 },
    { 0x3061, 0x00 },
    { 0x3060, 0x11 },
};

const static I2C_ARRAY gain_reg[] = {
    { 0x3090, 0x2A }, // low bit
    { 0x3091, 0x00 }, // hcg mode,bit 4
};

const static I2C_ARRAY gain_HDR_DOL_LEF_reg[] = {
    { 0x3090, 0x2A },
    { 0x3091, 0x00 },
};

const static I2C_ARRAY gain_HDR_DOL_SEF_reg[] = {
    { 0x3092, 0x20 },
    { 0x3093, 0x00 },
};

// static int g_sensor_ae_min_gain = 1024;
static CUS_GAIN_GAP_ARRAY gain_gap_compensate[16] = { // compensate  gain gap
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 }
};

/////////////////////////////////////////////////////////////////
//       @@@@@@@                                               //
//           @@                                                //
//          @@                                                 //
//          @@@                                                //
//       @   @@                                                //
//        @@@@                                                 //
//                                                             //
//      Step 3 --  complete camera features                    //
//                                                             //
//  camera set EV, MWB, orientation, contrast, sharpness       //
//   , saturation, and Denoise can work correctly.             //
//                                                             //
/////////////////////////////////////////////////////////////////

/////////////////// I2C function definition ///////////////////
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

    // Sensor power on sequence
    sensor_if->PowerOff(idx, handle->pwdn_POLARITY); // Powerdn Pull Low
    sensor_if->Reset(idx, handle->reset_POLARITY); // Rst Pull Low
    sensor_if->SetIOPad(idx, handle->sif_bus, handle->interface_attr.attr_mipi.mipi_lane_num);
    sensor_if->SetCSI_Clk(idx, CUS_CSI_CLK_216M);
    sensor_if->SetCSI_Lane(idx, handle->interface_attr.attr_mipi.mipi_lane_num, ENABLE);
    sensor_if->SetCSI_LongPacketType(idx, 0, 0x1C00, 0);

    if (handle->interface_attr.attr_mipi.mipi_hdr_mode == CUS_HDR_MODE_SONY_DOL) {
        sensor_if->SetCSI_hdr_mode(idx, handle->interface_attr.attr_mipi.mipi_hdr_mode, 1);
    }

    sensor_if->PowerOff(idx, !handle->pwdn_POLARITY);
    // Sensor board PWDN Enable, 1.8V & 2.9V need 30ms then Pull High
    SENSOR_MSLEEP(31);
    sensor_if->Reset(idx, !handle->reset_POLARITY);
    SENSOR_UDELAY(1);
    sensor_if->MCLK(idx, 1, handle->mclk);
    SENSOR_UDELAY(20);
    SENSOR_DMSG("Sensor Power On finished\n");
    return SUCCESS;
}

static int pCus_poweroff(ms_cus_sensor* handle, u32 idx)
{
    // power/reset low
    ISensorIfAPI* sensor_if = handle->sensor_if_api;

    SENSOR_DMSG("[%s] reset low\n", __FUNCTION__);
    sensor_if->Reset(idx, handle->reset_POLARITY); // Rst Pull Low
    SENSOR_MSLEEP(1);
    sensor_if->PowerOff(idx, handle->pwdn_POLARITY); // Powerdn Pull Low
    SENSOR_MSLEEP(1);
    sensor_if->MCLK(idx, 0, handle->mclk);

    sensor_if->SetCSI_Clk(idx, CUS_CSI_CLK_DISABLE);
    if (handle->interface_attr.attr_mipi.mipi_hdr_mode == CUS_HDR_MODE_SONY_DOL) {
        sensor_if->SetCSI_hdr_mode(idx, handle->interface_attr.attr_mipi.mipi_hdr_mode, 0);
    }

    handle->orient = SENSOR_ORIT;

    return SUCCESS;
}

/* Hardware reset the sensor before each mode init.  Toggles the
 * RESET pin via poweroff→poweron to clear all I2C register state.
 * Without this, switching between binned and non-binned modes leaves
 * stale state (e.g. HADD/VADD, DIG_CLP) that causes dark image or
 * incorrect output — even across soft reboots since the sensor chip
 * stays powered. */
static void pCus_HardwareReset(ms_cus_sensor* handle)
{
    ISensorIfAPI* sensor_if = handle->sensor_if_api;
    u32 idx = 0;

    /* Assert reset (active low for IMX415) */
    sensor_if->Reset(idx, handle->reset_POLARITY);
    SENSOR_MSLEEP(5);

    /* Deassert reset */
    sensor_if->Reset(idx, !handle->reset_POLARITY);
    SENSOR_MSLEEP(20);

    SENSOR_DMSG("[%s] sensor hardware reset complete\n", __FUNCTION__);
}

/////////////////// Check Sensor Product ID /////////////////////////
static int pCus_CheckSensorProductID(ms_cus_sensor* handle)
{
    u16 sen_id_msb, sen_id_lsb;

    /* Read Product ID */
    SensorReg_Read(0x3f12, (void*)&sen_id_lsb);
    SensorReg_Read(0x3f13, (void*)&sen_id_msb); // CHIP_ID_r3F13

    return SUCCESS;
}

// Get and check sensor ID
// if i2c error or sensor id does not match then return FAIL
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

    for (n = 0; n < 4; ++n) // retry , until I2C success
    {
        if (n > 3)
            return FAIL;

        if (SensorRegArrayR((I2C_ARRAY*)id_from_sensor, table_length) == SUCCESS) // read sensor ID from I2C
            break;
        else
            SENSOR_MSLEEP(1);
    }

    // convert sensor id to u32 format
    for (i = 0; i < table_length; ++i) {
        if (id_from_sensor[i].data != Sensor_id_table[i].data) {
            SENSOR_DMSG("[%s] Please Check IMX415 Sensor Insert!!\n", __FUNCTION__);
            return FAIL;
        }
        *id = id_from_sensor[i].data;
    }

    SENSOR_DMSG("[%s]IMX415 sensor ,Read sensor id, get 0x%x Success\n", __FUNCTION__, (int)*id);
    return SUCCESS;
}

static int imx415_SetPatternMode(ms_cus_sensor* handle, u32 mode)
{
    int i;
    switch (mode) {
    case 1:
        PatternTbl[0].data = 0x21; // enable
        break;
    case 0:
        PatternTbl[0].data &= 0xFE; // disable
        break;
    default:
        PatternTbl[0].data &= 0xFE; // disable
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

static int pCus_init_8m_30fps_mipi4lane_linear(ms_cus_sensor* handle)
{
    // imx415_params *params = (imx415_params *)handle->private_data;
    int i, cnt = 0;
    // s16 sen_data;

    if (pCus_CheckSensorProductID(handle) == FAIL) {
        return FAIL;
    }

    for (i = 0; i < ARRAY_SIZE(Sensor_8m_30fps_init_table_4lane_linear); i++) {
        if (Sensor_8m_30fps_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_8m_30fps_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_8m_30fps_init_table_4lane_linear[i].reg, Sensor_8m_30fps_init_table_4lane_linear[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
                // SENSOR_UDELAY(1);
            }
        }
    }

    return SUCCESS;
}

static int pCus_init_5m_60fps_mipi4lane_linear(ms_cus_sensor* handle)
{
    // imx415_params *params = (imx415_params *)handle->private_data;
    int i, cnt = 0;
    // s16 sen_data;

    if (pCus_CheckSensorProductID(handle) == FAIL) {
        return FAIL;
    }

    for (i = 0; i < ARRAY_SIZE(Sensor_5m_60fps_init_table_4lane_linear); i++) {
        if (Sensor_5m_60fps_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_5m_60fps_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_5m_60fps_init_table_4lane_linear[i].reg, Sensor_5m_60fps_init_table_4lane_linear[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
                // SENSOR_UDELAY(1);
            }
        }
    }

    return SUCCESS;
}

static int pCus_init_2m_90fps_mipi4lane_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    if (pCus_CheckSensorProductID(handle) == FAIL) {
        return FAIL;
    }

    for (i = 0; i < ARRAY_SIZE(Sensor_2m_90fps_init_table_4lane_linear); i++) {
        if (Sensor_2m_90fps_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_2m_90fps_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_2m_90fps_init_table_4lane_linear[i].reg, Sensor_2m_90fps_init_table_4lane_linear[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
                // SENSOR_UDELAY(1);
            }
        }
    }

    return SUCCESS;
}

/* Non-binned base register table — HMAX=1100, WINMODE=crop, no PIX regs.
 * PIX registers are computed dynamically from the mode table resolution. */
const static I2C_ARRAY Sensor_nobinned_base_init_table[] = {
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME
    { 0x300A, 0x42 }, // CPWAIT_TIME
    { 0x301C, 0x04 }, // WINMODE (crop)
    { 0x3020, 0x00 }, // HADD = no binning
    { 0x3021, 0x00 }, // VADD = no binning
    { 0x3022, 0x00 }, // ADDMODE = no binning
    { 0x3024, 0xCA }, // VMAX default (overridden by SetFPS)
    { 0x3025, 0x08 },
    { 0x3028, 0x4C }, // HMAX = 1100 = 0x044C
    { 0x3029, 0x04 },
    { 0x3031, 0x00 }, // ADBIT = 10-bit ADC
    { 0x3033, 0x05 }, // SYS_MODE (891Mbps)
    { 0x3050, 0x08 }, // SHR0
    { 0x30C1, 0x00 },
    { 0x3116, 0x23 }, // INCKSEL2
    { 0x3118, 0xC6 }, // INCKSEL3 (I6C clock)
    { 0x311A, 0xE7 }, // INCKSEL4
    { 0x311E, 0x23 }, // INCKSEL5
    { 0x32D4, 0x21 }, { 0x32EC, 0xA1 },
    { 0x3452, 0x7F }, { 0x3453, 0x03 },
    { 0x358A, 0x04 }, { 0x35A1, 0x02 }, { 0x36BC, 0x0C },
    { 0x36CC, 0x53 }, { 0x36CD, 0x00 }, { 0x36CE, 0x3C },
    { 0x36D0, 0x8C }, { 0x36D1, 0x00 }, { 0x36D2, 0x71 },
    { 0x36D4, 0x3C }, { 0x36D6, 0x53 }, { 0x36D7, 0x00 },
    { 0x36D8, 0x71 }, { 0x36DA, 0x8C }, { 0x36DB, 0x00 },
    { 0x3701, 0x00 },
    { 0x3724, 0x02 }, { 0x3726, 0x02 }, { 0x3732, 0x02 },
    { 0x3734, 0x03 }, { 0x3736, 0x03 }, { 0x3742, 0x03 },
    { 0x3862, 0xE0 }, { 0x38CC, 0x30 }, { 0x38CD, 0x2F },
    { 0x395C, 0x0C }, { 0x3A42, 0xD1 }, { 0x3A4C, 0x77 },
    { 0x3AE0, 0x02 }, { 0x3AEC, 0x0C },
    { 0x3B00, 0x2E }, { 0x3B06, 0x29 },
    { 0x3B98, 0x25 }, { 0x3B99, 0x21 },
    { 0x3B9B, 0x13 }, { 0x3B9C, 0x13 }, { 0x3B9D, 0x13 }, { 0x3B9E, 0x13 },
    { 0x3BA1, 0x00 }, { 0x3BA2, 0x06 }, { 0x3BA3, 0x0B }, { 0x3BA4, 0x10 },
    { 0x3BA5, 0x14 }, { 0x3BA6, 0x18 }, { 0x3BA7, 0x1A }, { 0x3BA8, 0x1A },
    { 0x3BA9, 0x1A },
    { 0x3BAC, 0xED }, { 0x3BAD, 0x01 }, { 0x3BAE, 0xF6 }, { 0x3BAF, 0x02 },
    { 0x3BB0, 0xA2 }, { 0x3BB1, 0x03 }, { 0x3BB2, 0xE0 }, { 0x3BB3, 0x03 },
    { 0x3BB4, 0xE0 }, { 0x3BB5, 0x03 }, { 0x3BB6, 0xE0 }, { 0x3BB7, 0x03 },
    { 0x3BB8, 0xE0 }, { 0x3BBA, 0xE0 }, { 0x3BBC, 0xDA }, { 0x3BBE, 0x88 },
    { 0x3BC0, 0x44 }, { 0x3BC2, 0x7B }, { 0x3BC4, 0xA2 },
    { 0x3BC8, 0xBD }, { 0x3BCA, 0xBD },
    { 0x4004, 0xC0 }, { 0x4005, 0x06 }, { 0x400C, 0x00 },
    { 0x4018, 0x7F }, { 0x401A, 0x37 }, { 0x401C, 0x37 },
    { 0x401E, 0xF7 }, { 0x401F, 0x00 }, { 0x4020, 0x3F },
    { 0x4022, 0x6F }, { 0x4024, 0x3F }, { 0x4026, 0x5F },
    { 0x4028, 0x2F }, { 0x4074, 0x01 },
    { 0xFFFF, 0x24 }, { 0x3002, 0x00 }, { 0xFFFF, 0x10 }, { 0x3000, 0x00 },
};

/* Dynamic non-binned init — computes PIX crop from mode table resolution.
 * Non-binned: PIX_HWIDTH=W, PIX_VWIDTH=H*2, centered and 8-aligned. */
static int pCus_init_nobinned_dynamic(ms_cus_sensor* handle)
{
    int i, cnt = 0;
    u32 res_idx = handle->video_res_supported.ulcur_res;
    u32 w = imx415_mipi_linear[res_idx].senout.width;
    u32 h = imx415_mipi_linear[res_idx].senout.height;
    u16 pix_hwidth = w;
    u16 pix_vwidth = h * 2;
    u16 pix_hst = ((3864 - pix_hwidth) / 2) & ~7; /* 8-aligned center */
    u16 pix_vst = (pix_vwidth < 4384) ? (((4384 - pix_vwidth) / 2) & ~7) : 0;

    pCus_HardwareReset(handle);
    if (pCus_CheckSensorProductID(handle) == FAIL)
        return FAIL;

    /* Write base non-binned registers */
    for (i = 0; i < ARRAY_SIZE(Sensor_nobinned_base_init_table); i++) {
        if (Sensor_nobinned_base_init_table[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_nobinned_base_init_table[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_nobinned_base_init_table[i].reg,
                    Sensor_nobinned_base_init_table[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
            }
        }
    }

    /* Write computed PIX crop registers */
    SensorReg_Write(0x3040, pix_hst & 0xFF);
    SensorReg_Write(0x3041, (pix_hst >> 8) & 0xFF);
    SensorReg_Write(0x3042, pix_hwidth & 0xFF);
    SensorReg_Write(0x3043, (pix_hwidth >> 8) & 0xFF);
    SensorReg_Write(0x3044, pix_vst & 0xFF);
    SensorReg_Write(0x3045, (pix_vst >> 8) & 0xFF);
    SensorReg_Write(0x3046, pix_vwidth & 0xFF);
    SensorReg_Write(0x3047, (pix_vwidth >> 8) & 0xFF);

    return SUCCESS;
}

static int pCus_init_1m_120fps_mipi4lane_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;
    pCus_HardwareReset(handle);
    if (pCus_CheckSensorProductID(handle) == FAIL) {
        return FAIL;
    }

    for (i = 0; i < ARRAY_SIZE(Sensor_1m_120fps_init_table_4lane_linear); i++) {
        if (Sensor_1m_120fps_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_1m_120fps_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_1m_120fps_init_table_4lane_linear[i].reg, Sensor_1m_120fps_init_table_4lane_linear[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
                // SENSOR_UDELAY(1);
            }
        }
    }

    return SUCCESS;
}

/* Maruko 1920x1080@30fps binned — full 1080p via crop+binning.
 * PIX_HWIDTH=3840(1920×2), PIX_VWIDTH=4320(1080×4).
 * PIX_HST=12, PIX_VST=0. */
const static I2C_ARRAY Sensor_1080p_binned_init_table_4lane_linear[] = {
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME
    { 0x300A, 0x42 }, // CPWAIT_TIME
    { 0x301C, 0x04 }, // WINMODE (crop)
    { 0x3020, 0x01 }, // HADD (binning)
    { 0x3021, 0x01 }, // VADD (binning)
    { 0x3022, 0x01 }, // ADDMODE (2/2)
    { 0x3024, 0xA4 }, // VMAX (reuse 120fps, SetFPS adjusts)
    { 0x3025, 0x06 },
    { 0x3028, 0x6D }, // HMAX (same as 120fps)
    { 0x3029, 0x01 },
    { 0x3031, 0x00 }, // ADBIT
    { 0x3033, 0x05 }, // SYS_MODE (891Mbps)
    { 0x3040, 0x0C }, // PIX_HST = 12 = 0x000C (centered 3840 in 3864)
    { 0x3041, 0x00 },
    { 0x3042, 0x00 }, // PIX_HWIDTH = 3840 = 0x0F00
    { 0x3043, 0x0F },
    { 0x3044, 0x00 }, // PIX_VST = 0
    { 0x3045, 0x00 },
    { 0x3046, 0xE0 }, // PIX_VWIDTH = 4320 = 0x10E0 (1080×4)
    { 0x3047, 0x10 },
    { 0x3050, 0x08 }, // SHR0
    { 0x30C1, 0x00 },
    { 0x30D9, 0x02 }, // DIG_CLP (binning)
    { 0x30DA, 0x01 },
    { 0x3116, 0x23 }, // INCKSEL2
    { 0x3118, 0xC6 }, // INCKSEL3 (I6C pixel clock)
    { 0x311A, 0xE7 }, // INCKSEL4
    { 0x311E, 0x23 }, // INCKSEL5
    { 0x32D4, 0x21 }, { 0x32EC, 0xA1 },
    { 0x3452, 0x7F }, { 0x3453, 0x03 },
    { 0x358A, 0x04 }, { 0x35A1, 0x02 }, { 0x36BC, 0x0C },
    { 0x36CC, 0x53 }, { 0x36CD, 0x00 }, { 0x36CE, 0x3C },
    { 0x36D0, 0x8C }, { 0x36D1, 0x00 }, { 0x36D2, 0x71 },
    { 0x36D4, 0x3C }, { 0x36D6, 0x53 }, { 0x36D7, 0x00 },
    { 0x36D8, 0x71 }, { 0x36DA, 0x8C }, { 0x36DB, 0x00 },
    { 0x3701, 0x00 },
    { 0x3724, 0x02 }, { 0x3726, 0x02 }, { 0x3732, 0x02 },
    { 0x3734, 0x03 }, { 0x3736, 0x03 }, { 0x3742, 0x03 },
    { 0x3862, 0xE0 }, { 0x38CC, 0x30 }, { 0x38CD, 0x2F },
    { 0x395C, 0x0C }, { 0x3A42, 0xD1 }, { 0x3A4C, 0x77 },
    { 0x3AE0, 0x02 }, { 0x3AEC, 0x0C },
    { 0x3B00, 0x2E }, { 0x3B06, 0x29 },
    { 0x3B98, 0x25 }, { 0x3B99, 0x21 },
    { 0x3B9B, 0x13 }, { 0x3B9C, 0x13 }, { 0x3B9D, 0x13 }, { 0x3B9E, 0x13 },
    { 0x3BA1, 0x00 }, { 0x3BA2, 0x06 }, { 0x3BA3, 0x0B }, { 0x3BA4, 0x10 },
    { 0x3BA5, 0x14 }, { 0x3BA6, 0x18 }, { 0x3BA7, 0x1A }, { 0x3BA8, 0x1A },
    { 0x3BA9, 0x1A },
    { 0x3BAC, 0xED }, { 0x3BAD, 0x01 }, { 0x3BAE, 0xF6 }, { 0x3BAF, 0x02 },
    { 0x3BB0, 0xA2 }, { 0x3BB1, 0x03 }, { 0x3BB2, 0xE0 }, { 0x3BB3, 0x03 },
    { 0x3BB4, 0xE0 }, { 0x3BB5, 0x03 }, { 0x3BB6, 0xE0 }, { 0x3BB7, 0x03 },
    { 0x3BB8, 0xE0 }, { 0x3BBA, 0xE0 }, { 0x3BBC, 0xDA }, { 0x3BBE, 0x88 },
    { 0x3BC0, 0x44 }, { 0x3BC2, 0x7B }, { 0x3BC4, 0xA2 },
    { 0x3BC8, 0xBD }, { 0x3BCA, 0xBD },
    { 0x4004, 0xC0 }, { 0x4005, 0x06 }, { 0x400C, 0x00 },
    { 0x4018, 0x7F }, { 0x401A, 0x37 }, { 0x401C, 0x37 },
    { 0x401E, 0xF7 }, { 0x401F, 0x00 }, { 0x4020, 0x3F },
    { 0x4022, 0x6F }, { 0x4024, 0x3F }, { 0x4026, 0x5F },
    { 0x4028, 0x2F }, { 0x4074, 0x01 },
    { 0xFFFF, 0x24 }, { 0x3002, 0x00 }, { 0xFFFF, 0x10 }, { 0x3000, 0x00 },
};

static int pCus_init_1080p_binned_mipi4lane_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;
    pCus_HardwareReset(handle);
    if (pCus_CheckSensorProductID(handle) == FAIL)
        return FAIL;
    for (i = 0; i < ARRAY_SIZE(Sensor_1080p_binned_init_table_4lane_linear); i++) {
        if (Sensor_1080p_binned_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_1080p_binned_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_1080p_binned_init_table_4lane_linear[i].reg,
                    Sensor_1080p_binned_init_table_4lane_linear[i].data) != SUCCESS) {
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

/* Maruko 1080p@60fps binning — tipoman9 I6C init table.
 * Uses I6C-tuned INCKSEL3=0xC6, HMAX=0x1B, VTS=2296. */
const static I2C_ARRAY Sensor_2m_60fps_init_table_4lane_linear[] = {
    {0x3000, 0x01}, {0x3002, 0x01}, {0x3008, 0x5D}, {0x300A, 0x42},
    {0x3020, 0x01}, {0x3021, 0x01}, {0x3022, 0x01},
    {0x3024, 0xF8}, {0x3028, 0x1B},
    {0x3031, 0x00}, {0x3033, 0x05}, {0x3050, 0x08},
    {0x30C1, 0x00}, {0x30D9, 0x02}, {0x30DA, 0x01},
    {0x3116, 0x23}, {0x3118, 0xC6}, {0x311A, 0xE7}, {0x311E, 0x23},
    {0x32D4, 0x21}, {0x32EC, 0xA1}, {0x3452, 0x7F}, {0x3453, 0x03},
    {0x358A, 0x04}, {0x35A1, 0x02}, {0x36BC, 0x0C},
    {0x36CC, 0x53}, {0x36CD, 0x00}, {0x36CE, 0x3C},
    {0x36D0, 0x8C}, {0x36D1, 0x00}, {0x36D2, 0x71},
    {0x36D4, 0x3C}, {0x36D6, 0x53}, {0x36D7, 0x00},
    {0x36D8, 0x71}, {0x36DA, 0x8C}, {0x36DB, 0x00},
    {0x3701, 0x00}, {0x3724, 0x02}, {0x3726, 0x02},
    {0x3732, 0x02}, {0x3734, 0x03}, {0x3736, 0x03}, {0x3742, 0x03},
    {0x3862, 0xE0}, {0x38CC, 0x30}, {0x38CD, 0x2F}, {0x395C, 0x0C},
    {0x3A42, 0xD1}, {0x3A4C, 0x77}, {0x3AE0, 0x02}, {0x3AEC, 0x0C},
    {0x3B00, 0x2E}, {0x3B06, 0x29},
    {0x3B98, 0x25}, {0x3B99, 0x21},
    {0x3B9B, 0x13}, {0x3B9C, 0x13}, {0x3B9D, 0x13}, {0x3B9E, 0x13},
    {0x3BA1, 0x00}, {0x3BA2, 0x06}, {0x3BA3, 0x0B}, {0x3BA4, 0x10},
    {0x3BA5, 0x14}, {0x3BA6, 0x18}, {0x3BA7, 0x1A}, {0x3BA8, 0x1A},
    {0x3BA9, 0x1A},
    {0x3BAC, 0xED}, {0x3BAD, 0x01}, {0x3BAE, 0xF6}, {0x3BAF, 0x02},
    {0x3BB0, 0xA2}, {0x3BB1, 0x03}, {0x3BB2, 0xE0}, {0x3BB3, 0x03},
    {0x3BB4, 0xE0}, {0x3BB5, 0x03}, {0x3BB6, 0xE0}, {0x3BB7, 0x03},
    {0x3BB8, 0xE0}, {0x3BBA, 0xE0}, {0x3BBC, 0xDA}, {0x3BBE, 0x88},
    {0x3BC0, 0x44}, {0x3BC2, 0x7B}, {0x3BC4, 0xA2},
    {0x3BC8, 0xBD}, {0x3BCA, 0xBD},
    {0x4004, 0xC0}, {0x4005, 0x06}, {0x400C, 0x00},
    {0x4018, 0x7F}, {0x401A, 0x37}, {0x401C, 0x37},
    {0x401E, 0xF7}, {0x401F, 0x00}, {0x4020, 0x3F},
    {0x4022, 0x6F}, {0x4024, 0x3F}, {0x4026, 0x5F},
    {0x4028, 0x2F}, {0x4074, 0x01},
    {0xffff, 0x24}, {0x3002, 0x00}, {0xffff, 0x10}, {0x3000, 0x00},
};

static int pCus_init_2m_60fps_mipi4lane_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;
    if (pCus_CheckSensorProductID(handle) == FAIL)
        return FAIL;
    for (i = 0; i < ARRAY_SIZE(Sensor_2m_60fps_init_table_4lane_linear); i++) {
        if (Sensor_2m_60fps_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_2m_60fps_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_2m_60fps_init_table_4lane_linear[i].reg,
                    Sensor_2m_60fps_init_table_4lane_linear[i].data) != SUCCESS) {
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

static int pCus_init_mipi4lane_HDR_DOL(ms_cus_sensor* handle)
{
    // imx415_params *params = (imx415_params *)handle->private_data;
    int i, cnt = 0;
    // s16 sen_data;

    if (pCus_CheckSensorProductID(handle)) {
        return FAIL;
    }

    for (i = 0; i < ARRAY_SIZE(Sensor_init_table_4lane_HDR_DOL); i++) {
        if (Sensor_init_table_4lane_HDR_DOL[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_init_table_4lane_HDR_DOL[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_init_table_4lane_HDR_DOL[i].reg, Sensor_init_table_4lane_HDR_DOL[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
                // SENSOR_UDELAY(1);
            }
        }
    }

    SENSOR_DMSG("Sensor IMX415 HDR MODE Initial Finished\n");
    return SUCCESS;
}

static int pCus_init_8m_20fps_mipi4lane_HDR_DOL(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    if (pCus_CheckSensorProductID(handle)) {
        return FAIL;
    }

    for (i = 0; i < ARRAY_SIZE(Sensor_8m_20fps_init_table_4lane_HDR_DOL); i++) {
        if (Sensor_8m_20fps_init_table_4lane_HDR_DOL[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_8m_20fps_init_table_4lane_HDR_DOL[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_8m_20fps_init_table_4lane_HDR_DOL[i].reg, Sensor_8m_20fps_init_table_4lane_HDR_DOL[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
                // SENSOR_UDELAY(1);
            }
        }
    }

    return SUCCESS;
}

static int pCus_init_8m_30fps_mipi4lane_HDR_DOL(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    if (pCus_CheckSensorProductID(handle)) {
        return FAIL;
    }

    for (i = 0; i < ARRAY_SIZE(Sensor_8m_30fps_init_table_4lane_HDR_DOL); i++) {
        if (Sensor_8m_30fps_init_table_4lane_HDR_DOL[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_8m_30fps_init_table_4lane_HDR_DOL[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_8m_30fps_init_table_4lane_HDR_DOL[i].reg, Sensor_8m_30fps_init_table_4lane_HDR_DOL[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
                // SENSOR_UDELAY(1);
            }
        }
    }

    return SUCCESS;
}

static int pCus_init_5m_30fps_mipi4lane_HDR_DOL(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    if (pCus_CheckSensorProductID(handle)) {
        return FAIL;
    }

    for (i = 0; i < ARRAY_SIZE(Sensor_5m_30fps_init_table_4lane_HDR_DOL); i++) {
        if (Sensor_5m_30fps_init_table_4lane_HDR_DOL[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_5m_30fps_init_table_4lane_HDR_DOL[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_5m_30fps_init_table_4lane_HDR_DOL[i].reg, Sensor_5m_30fps_init_table_4lane_HDR_DOL[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
                // SENSOR_UDELAY(1);
            }
        }
    }

    return SUCCESS;
}

static int pCus_init_4p8m_30fps_mipi4lane_HDR_DOL(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    if (pCus_CheckSensorProductID(handle)) {
        return FAIL;
    }
    for (i = 0; i < ARRAY_SIZE(Sensor_4p8m_30fps_init_table_4lane_HDR_DOL); i++) {
        if (Sensor_4p8m_30fps_init_table_4lane_HDR_DOL[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_4p8m_30fps_init_table_4lane_HDR_DOL[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_4p8m_30fps_init_table_4lane_HDR_DOL[i].reg, Sensor_4p8m_30fps_init_table_4lane_HDR_DOL[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
                // SENSOR_UDELAY(1);
            }
        }
    }
    return SUCCESS;
}

static int pCus_init_4m_30fps_mipi4lane_HDR_DOL(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    if (pCus_CheckSensorProductID(handle)) {
        return FAIL;
    }
    for (i = 0; i < ARRAY_SIZE(Sensor_4m_30fps_init_table_4lane_HDR_DOL); i++) {
        if (Sensor_4m_30fps_init_table_4lane_HDR_DOL[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_4m_30fps_init_table_4lane_HDR_DOL[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_4m_30fps_init_table_4lane_HDR_DOL[i].reg, Sensor_4m_30fps_init_table_4lane_HDR_DOL[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
                // SENSOR_UDELAY(1);
            }
        }
    }
    return SUCCESS;
}

static int pCus_init_2m_30fps_mipi4lane_HDR_DOL(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    if (pCus_CheckSensorProductID(handle)) {
        return FAIL;
    }
    for (i = 0; i < ARRAY_SIZE(Sensor_2m_30fps_init_table_4lane_HDR_DOL); i++) {
        if (Sensor_2m_30fps_init_table_4lane_HDR_DOL[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_2m_30fps_init_table_4lane_HDR_DOL[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_2m_30fps_init_table_4lane_HDR_DOL[i].reg, Sensor_2m_30fps_init_table_4lane_HDR_DOL[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
                // SENSOR_UDELAY(1);
            }
        }
    }
    return SUCCESS;
}

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
    imx415_params* params = (imx415_params*)handle->private_data;
    u32 num_res = handle->video_res_supported.num_res;

    if (res_idx >= num_res) {
        SENSOR_EMSG("[%s] Please check the number of resolutions supported by the sensor!\n", __FUNCTION__);
        return FAIL;
    }

    handle->video_res_supported.ulcur_res = res_idx;

    handle->data_prec = CUS_DATAPRECISION_12;

    switch (res_idx) {
    case 0: // 3760x2116@30fps — non-binned, 97% FOV, best quality
        handle->video_res_supported.ulcur_res = 0;
        handle->pCus_sensor_init = pCus_init_nobinned_dynamic;
        vts_30fps = 2250; // VTS at 30fps with HMAX=1100
        params->expo.vts = vts_30fps;
        params->expo.fps = 30;
        Preview_line_period = 14815; // HMAX=1100 at INCKSEL3=0xC6
        break;

    case 1: // 1920x1080@60fps — binned, 99% FOV
        handle->video_res_supported.ulcur_res = 1;
        handle->pCus_sensor_init = pCus_init_1080p_binned_mipi4lane_linear;
        vts_30fps = 3400; // 1700 * 120/60
        params->expo.vts = vts_30fps;
        params->expo.fps = 60;
        Preview_line_period = 4882;
        break;

    case 2: // 1920x1080@90fps — binned, 99% FOV
        handle->video_res_supported.ulcur_res = 2;
        handle->pCus_sensor_init = pCus_init_1080p_binned_mipi4lane_linear;
        vts_30fps = 2267; // 1700 * 120/90
        params->expo.vts = vts_30fps;
        params->expo.fps = 90;
        Preview_line_period = 4882;
        break;

    case 3: // 1472x816@120fps — binned, 76% FOV, ultra-low latency
        handle->video_res_supported.ulcur_res = 3;
        handle->pCus_sensor_init = pCus_init_1m_120fps_mipi4lane_linear;
        vts_30fps = 1700;
        params->expo.vts = vts_30fps;
        params->expo.fps = 120;
        Preview_line_period = 4882;
        break;

    default:
        break;
    }

    return SUCCESS;
}

static int pCus_SetVideoRes_HDR_DOL_LEF(ms_cus_sensor* handle, u32 res_idx)
{
    u32 num_res = handle->video_res_supported.num_res;

    if (res_idx >= num_res) {
        SENSOR_EMSG("[%s] Please check the number of resolutions supported by the sensor!\n", __FUNCTION__);
        return FAIL;
    }

    handle->video_res_supported.ulcur_res = res_idx;

    switch (res_idx) {
    case HDR_RES_1:
    case HDR_RES_7:
        handle->data_prec = CUS_DATAPRECISION_12;
        break;
    case HDR_RES_2:
    case HDR_RES_3:
    case HDR_RES_4:
    case HDR_RES_5:
    case HDR_RES_6:
        handle->data_prec = CUS_DATAPRECISION_10;
        break;
    default:
        break;
    }
    return SUCCESS;
}

static int pCus_SetVideoRes_HDR_DOL_SEF(ms_cus_sensor* handle, u32 res_idx)
{
    imx415_params* params = (imx415_params*)handle->private_data;
    u32 num_res = handle->video_res_supported.num_res;

    if (res_idx >= num_res) {
        SENSOR_EMSG("[%s] Please check the number of resolutions supported by the sensor!\n", __FUNCTION__);
        return FAIL;
    }

    handle->video_res_supported.ulcur_res = res_idx;

    switch (res_idx) {
    case HDR_RES_1:
        handle->pCus_sensor_init = pCus_init_mipi4lane_HDR_DOL;
        vts_30fps_HDR_DOL_4lane = 2250;
        params->expo.vts = vts_30fps_HDR_DOL_4lane;
        params->expo.fps = 15;
        Preview_line_period_HDR_DOL_4LANE = 29630;
        handle->data_prec = CUS_DATAPRECISION_12;
        params->min_rhs1 = 437; // 4n+1 fix to 269, 337 //5ms: 429
        break;

    case HDR_RES_2:
        handle->pCus_sensor_init = pCus_init_8m_20fps_mipi4lane_HDR_DOL;
        vts_30fps_HDR_DOL_4lane = 3450;
        params->expo.vts = vts_30fps_HDR_DOL_4lane;
        params->expo.fps = 20;
        Preview_line_period_HDR_DOL_4LANE = 14493;
        handle->data_prec = CUS_DATAPRECISION_10;
        params->min_rhs1 = 437; // 4n+1 fix to 269, 337 //5ms: 429
        break;

    case HDR_RES_3:
        handle->pCus_sensor_init = pCus_init_8m_30fps_mipi4lane_HDR_DOL;
        vts_30fps_HDR_DOL_4lane = 2300;
        params->expo.vts = vts_30fps_HDR_DOL_4lane;
        params->expo.fps = 30;
        Preview_line_period_HDR_DOL_4LANE = 14493;
        handle->data_prec = CUS_DATAPRECISION_10;
        params->min_rhs1 = 437; // 4n+1 fix to 269, 337 //5ms: 429
        break;

    case HDR_RES_4:
        handle->pCus_sensor_init = pCus_init_5m_30fps_mipi4lane_HDR_DOL;
        vts_30fps_HDR_DOL_4lane = 2300;
        params->expo.vts = vts_30fps_HDR_DOL_4lane;
        params->expo.fps = 30;
        Preview_line_period_HDR_DOL_4LANE = 14493;
        handle->data_prec = CUS_DATAPRECISION_10;
        params->min_rhs1 = 437; // 4n+1 fix to 269, 337 //5ms: 429
        break;

    case HDR_RES_5:
        handle->pCus_sensor_init = pCus_init_4p8m_30fps_mipi4lane_HDR_DOL;
        vts_30fps_HDR_DOL_4lane = 2300;
        params->expo.vts = vts_30fps_HDR_DOL_4lane;
        params->expo.fps = 30;
        Preview_line_period_HDR_DOL_4LANE = 14493;
        handle->data_prec = CUS_DATAPRECISION_10;
        params->min_rhs1 = 365; // 4n+1 fix to 269, 337 //5ms: 429
        break;

    case HDR_RES_6:
        handle->pCus_sensor_init = pCus_init_4m_30fps_mipi4lane_HDR_DOL;
        vts_30fps_HDR_DOL_4lane = 2300;
        params->expo.vts = vts_30fps_HDR_DOL_4lane;
        params->expo.fps = 30;
        Preview_line_period_HDR_DOL_4LANE = 14493;
        handle->data_prec = CUS_DATAPRECISION_10;
        params->min_rhs1 = 365; // 4ms: 343 //5ms: 429
        break;

    case HDR_RES_7:
        handle->pCus_sensor_init = pCus_init_2m_30fps_mipi4lane_HDR_DOL;
        vts_30fps_HDR_DOL_4lane = 2296;
        params->expo.vts = vts_30fps_HDR_DOL_4lane;
        params->expo.fps = 30;
        Preview_line_period_HDR_DOL_4LANE = 14518;
        handle->data_prec = CUS_DATAPRECISION_12;
        params->min_rhs1 = 437; // 4n+1 fix to 269, 337 //5ms: 429
        break;

    default:
        break;
    }

    return SUCCESS;
}

static int pCus_GetOrien(ms_cus_sensor* handle, CUS_CAMSENSOR_ORIT* orit)
{
    s16 sen_data;

    // Read SENSOR MIRROR-FLIP STATUS
    SensorReg_Read(MIRROR_FLIP, (void*)&sen_data);

    switch (sen_data & SENSOR_MIRROR_FLIP_EN) {
    case SENSOR_NOR:
        *orit = CUS_ORIT_M0F0;
        break;
    case SENSOR_FLIP_EN:
        *orit = CUS_ORIT_M0F1;
        break;
    case SENSOR_MIRROR_EN:
        *orit = CUS_ORIT_M1F0;
        break;
    case SENSOR_MIRROR_FLIP_EN:
        *orit = CUS_ORIT_M1F1;
        break;
    }
    return SUCCESS;
}

static int pCus_SetOrien(ms_cus_sensor* handle, CUS_CAMSENSOR_ORIT orit)
{
    imx415_params* params = (imx415_params*)handle->private_data;

    handle->orient = orit;
    params->orien_dirty = true;

    return SUCCESS;
}

static int DoOrien(ms_cus_sensor* handle, CUS_CAMSENSOR_ORIT orit)
{
    s16 sen_data;
    // Read SENSOR MIRROR-FLIP STATUS
    SensorReg_Read(MIRROR_FLIP, (void*)&sen_data);
    sen_data &= ~(SENSOR_MIRROR_FLIP_EN);

    switch (orit) {
    case CUS_ORIT_M0F0:
        // sen_data |= SENSOR_NOR;
        handle->orient = CUS_ORIT_M0F0;
        break;
    case CUS_ORIT_M1F0:
        sen_data |= SENSOR_MIRROR_EN;
        handle->orient = CUS_ORIT_M1F0;
        break;
    case CUS_ORIT_M0F1:
        sen_data |= SENSOR_FLIP_EN;
        handle->orient = CUS_ORIT_M0F1;
        break;
    case CUS_ORIT_M1F1:
        sen_data |= SENSOR_MIRROR_FLIP_EN;
        handle->orient = CUS_ORIT_M1F1;
        break;
    default:
        handle->orient = CUS_ORIT_M0F0;
        break;
    }
    // Write SENSOR MIRROR-FLIP STATUS
    SensorReg_Write(MIRROR_FLIP, sen_data);

    return SUCCESS;
}

static int pCus_GetFPS(ms_cus_sensor* handle)
{
    imx415_params* params = (imx415_params*)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 tVts = (params->tVts_reg[0].data << 16) | (params->tVts_reg[1].data << 8) | (params->tVts_reg[2].data << 0);

    if (params->expo.fps >= 1000)
        params->expo.preview_fps = (vts_30fps * max_fps * 1000) / tVts;
    else
        params->expo.preview_fps = (vts_30fps * max_fps) / tVts;

    return params->expo.preview_fps;
}

static int pCus_SetFPS(ms_cus_sensor* handle, u32 fps)
{
    // u32 vts = 0, cur_vts_30fps = 0;
    imx415_params* params = (imx415_params*)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 min_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].min_fps;
    // pr_info("[%s]  leslie_fps,maxfps,minfps : %d,%d,%d\n\n", __FUNCTION__,fps,max_fps,min_fps);
    // cur_vts_30fps = vts_30fps;
    // pr_info("[%s]  leslie_vts_30fps : %u\n\n", __FUNCTION__,vts_30fps);
    if (fps >= min_fps && fps <= max_fps) {
        if (CUS_CMU_CLK_36MHZ == handle->mclk) {
            fps = fps > 29 ? 29 : fps; // limit fps at 29 fps due to MCLK=36MHz
            params->expo.vts = (vts_30fps * 29091 + fps * 500) / (fps * 1000);
        } else
            params->expo.vts = (vts_30fps * (max_fps * 1000) + fps * 500) / (fps * 1000);
    } else if (fps >= (min_fps * 1000) && fps <= (max_fps * 1000)) {
        if (CUS_CMU_CLK_36MHZ == handle->mclk) {
            fps = fps > 29091 ? 29091 : fps; // limit fps at 29.091 fps due to MCLK=36MHz
            params->expo.vts = (vts_30fps * 29091 + (fps >> 1)) / fps;
        } else
            params->expo.vts = (vts_30fps * (max_fps * 1000) + (fps >> 1)) / fps;
    } else {
        SENSOR_DMSG("[%s] FPS %d out of range.\n", __FUNCTION__, fps);
        return FAIL;
    }
    // pr_info("[%s]  leslie_vts : %u\n\n", __FUNCTION__,params->expo.vts);
    if (params->expo.expo_lines > params->expo.vts - 4) {
        // vts = params->expo.expo_lines + 4;
#if 0 // Update FPS Status
        if(fps>=3 && fps <= 30)
            fps = (vts_30fps*30000)/(params->expo.vts * 1000 - 500);
        else if(fps>=3000 && fps <= 30000)
            fps = (vts_30fps*30000)/(params->expo.vts - (500 / 1000));
#endif
    } else {
        // vts = params->expo.vts;
    }

    params->expo.fps = fps;
    params->dirty = true;

    pCus_SetAEUSecs(handle, params->expo.expo_lef_us);

    return SUCCESS;
}

static int pCus_GetFPS_HDR_DOL_SEF(ms_cus_sensor* handle)
{
    imx415_params* params = (imx415_params*)handle->private_data;
    u32 cur_vts_30fps = vts_30fps_HDR_DOL_4lane;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 tVts = (params->tVts_reg[0].data << 16) | (params->tVts_reg[1].data << 8) | (params->tVts_reg[2].data << 0);

    if (params->expo.fps >= 1000)
        params->expo.preview_fps = (cur_vts_30fps * max_fps * 1000) / tVts;
    else
        params->expo.preview_fps = (cur_vts_30fps * max_fps) / tVts;

    return params->expo.preview_fps;
}

static int pCus_SetFPS_HDR_DOL_SEF(ms_cus_sensor* handle, u32 fps)
{
    u32 cur_vts_30fps = 0;
    imx415_params* params = (imx415_params*)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 min_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].min_fps;
    // cur_vts_30fps = imx415_mipi_hdr[0].senout.height;
    cur_vts_30fps = vts_30fps_HDR_DOL_4lane;
    if (fps >= min_fps && fps <= max_fps) {
        if (CUS_CMU_CLK_36MHZ == handle->mclk) {
            fps = fps > 14 ? 14 : fps; // limit fps at 29 fps due to MCLK=36MHz
            params->expo.vts = (cur_vts_30fps * 14545 + fps * 500) / (fps * 1000);
        } else
            params->expo.vts = (cur_vts_30fps * (max_fps * 1000) + fps * 500) / (fps * 1000);

    } else if (fps >= (min_fps * 1000) && fps <= (max_fps * 1000)) {
        if (CUS_CMU_CLK_36MHZ == handle->mclk) {
            fps = fps > 14545 ? 14545 : fps; // limit fps at 29.091 fps due to MCLK=36MHz
            params->expo.vts = (cur_vts_30fps * 14545 + (fps >> 1)) / fps;
        } else
            params->expo.vts = (cur_vts_30fps * (max_fps * 1000) + (fps >> 1)) / fps;
    } else {
        SENSOR_DMSG("[%s] FPS %d out of range.\n", __FUNCTION__, fps);
        return FAIL;
    }

    if (params->expo.expo_lines > params->expo.vts - 4) {
        // vts = params->expo.expo_lines + 4;
#if 0 // Update FPS Status
        if(fps>=3 && fps <= 30)
            fps = (vts_30fps*30000)/(params->expo.vts * 1000 - 500);
        else if(fps>=3000 && fps <= 30000)
            fps = (vts_30fps*30000)/(params->expo.vts - (500 / 1000));
#endif
    } else {
        // vts = params->expo.vts;
    }

    params->expo.fps = fps;
    // params->expo.vts = vts;
    params->dirty = true;

    pCus_SetAEUSecsHDR_DOL_SEF(handle, params->expo.expo_sef_us);

    return SUCCESS;
}

int cus_camsensor_init_handle_linear(ms_cus_sensor* drv_handle);

///////////////////////////////////////////////////////////////////////
// auto exposure
///////////////////////////////////////////////////////////////////////
// unit: micro seconds
// AE status notification
static int pCus_AEStatusNotify(ms_cus_sensor* handle, CUS_CAMSENSOR_AE_STATUS_NOTIFY status)
{
    imx415_params* params = (imx415_params*)handle->private_data;
    // ISensorIfAPI2 *sensor_if1 = handle->sensor_if_api2;

    switch (status) {
    case CUS_FRAME_INACTIVE:
        break;
    case CUS_FRAME_ACTIVE:
        if (params->dirty || params->orien_dirty) {
            SensorReg_Write(0x3001, 1); // Global hold on
            if (params->dirty) {
                SensorRegArrayW((I2C_ARRAY*)params->tExpo_reg, ARRAY_SIZE(expo_reg));
                SensorRegArrayW((I2C_ARRAY*)params->tGain_reg, ARRAY_SIZE(gain_reg));
                SensorRegArrayW((I2C_ARRAY*)params->tVts_reg, ARRAY_SIZE(vts_reg));
                params->dirty = false;
            }

            if (params->orien_dirty) {
                DoOrien(handle, handle->orient);
                params->orien_dirty = false;
            }
            SensorReg_Write(0x3001, 0); // Global hold off
        }
        break;
    default:
        break;
    }
    return SUCCESS;
}

static int pCus_AEStatusNotifyHDR_DOL_SEF(ms_cus_sensor* handle, CUS_CAMSENSOR_AE_STATUS_NOTIFY status)
{
    // imx415_params *params = (imx415_params *)handle->private_data;

    switch (status) {
    case CUS_FRAME_INACTIVE:
        break;
    case CUS_FRAME_ACTIVE:
#if 0
            if(params->dirty || params->orien_dirty) {
                SensorReg_Write(0x3001,1);

                if(params->dirty) {
                    //SensorRegArrayW((I2C_ARRAY*)params->tExpo_reg, ARRAY_SIZE(expo_reg));
                    //SensorRegArrayW((I2C_ARRAY*)params->tGain_reg, ARRAY_SIZE(gain_reg));
                    //SensorRegArrayW((I2C_ARRAY*)params->tVts_reg, ARRAY_SIZE(vts_reg));
                    params->dirty = false;
                }
                if(params->orien_dirty) {
                    DoOrien(handle, handle->orient);
                    params->orien_dirty = false;
                }
                SensorReg_Write(0x3001,0);
            }
#endif
        break;
    default:
        break;
    }
    return SUCCESS;
}

static int pCus_GetAEUSecs(ms_cus_sensor* handle, u32* us)
{
    u32 lines = 0;
    imx415_params* params = (imx415_params*)handle->private_data;

    lines |= (u32)(params->tExpo_reg[0].data & 0x03) << 16;
    lines |= (u32)(params->tExpo_reg[1].data & 0xff) << 8;
    lines |= (u32)(params->tExpo_reg[2].data & 0xff) << 0;

    *us = (lines * Preview_line_period) / 1000;

    SENSOR_DMSG("[%s] sensor expo lines/us %u,%u us\n", __FUNCTION__, lines, *us);

    return SUCCESS;
}

static int pCus_SetAEUSecs(ms_cus_sensor* handle, u32 us)
{
    u32 expo_lines = 0, vts = 0, SHR0 = 0;
    imx415_params* params = (imx415_params*)handle->private_data;

    params->expo.expo_lef_us = us;
    expo_lines = (1000 * us) / Preview_line_period;

    if (expo_lines > params->expo.vts) {
        vts = expo_lines + 8;
    } else
        vts = params->expo.vts;
    SHR0 = vts - expo_lines;

    if (SHR0 <= 12) // 8+4
        SHR0 = 8;
    else
        SHR0 -= 4;

    params->expo.expo_lines = expo_lines;
    // params->expo.vts = vts;

    SENSOR_DMSG("[%s] us %u, SHR0 %u, vts %u\n", __FUNCTION__,
        us,
        SHR0,
        vts);
    // pr_info("[%s]  leslie_shutter,expo_lines,params_expo_lines : %d,%d,%d\n\n", __FUNCTION__,us,expo_lines,params->expo.expo_lines);
    // pr_info("[%s]  leslie_shutter_vts : %u,%u\n\n", __FUNCTION__,params->expo.vts,vts);
    params->tExpo_reg[0].data = (SHR0 >> 16) & 0x0003;
    params->tExpo_reg[1].data = (SHR0 >> 8) & 0x00ff;
    params->tExpo_reg[2].data = (SHR0 >> 0) & 0x00ff;

    params->tVts_reg[0].data = (vts >> 16) & 0x0003;
    params->tVts_reg[1].data = (vts >> 8) & 0x00ff;
    params->tVts_reg[2].data = (vts >> 0) & 0x00ff;

    params->dirty = true;
    return SUCCESS;
}

static int pCus_SetAEUSecsHDR_DOL_SEF(ms_cus_sensor* handle, u32 us)
{
    u32 expo_line_sef = 0;
    u32 cur_line_period = Preview_line_period_HDR_DOL_4LANE;
    // u32 cur_vts_30fps = vts_30fps_HDR_DOL_4lane;
    imx415_params* params = (imx415_params*)handle->private_data;

    cur_line_period = Preview_line_period_HDR_DOL_4LANE;
    // cur_vts_30fps =vts_30fps_HDR_DOL_4lane;
    expo_line_sef = (1000 * us + (cur_line_period >> 1)) / cur_line_period;
    params->expo.expo_sef_us = us;
    // params->min_rhs1 = 437;     // 4n+1 fix to 269, 337 //5ms: 429

    params->min_shr1 = params->min_rhs1 - expo_line_sef;
    if ((expo_line_sef > params->min_rhs1) || ((params->min_shr1) < 9))
        params->min_shr1 = 9;
    params->min_shr1 = ((params->min_shr1 >> 1) << 1) + 1;

    SENSOR_DMSG("[%s] us %u, expo_line_sef %u rhs %u shr1 %u\n", __FUNCTION__,
        us,
        expo_line_sef,
        params->min_rhs1,
        params->min_shr1);

    params->tRHS1_reg[0].data = (params->min_rhs1 >> 16) & 0x03;
    params->tRHS1_reg[1].data = (params->min_rhs1 >> 8) & 0xff;
    params->tRHS1_reg[2].data = (params->min_rhs1 >> 0) & 0xff;

    params->tSHR1_reg[0].data = (params->min_shr1 >> 16) & 0x0003;
    params->tSHR1_reg[1].data = (params->min_shr1 >> 8) & 0x00ff;
    params->tSHR1_reg[2].data = (params->min_shr1 >> 0) & 0x00ff;

    params->dirty = true;
    return SUCCESS;
}

// Gain: 1x = 1024
static int pCus_GetAEGain(ms_cus_sensor* handle, u32* gain)
{
    imx415_params* params = (imx415_params*)handle->private_data;
#if 0
    u16 temp_gain;

    temp_gain=gain_reg[0].data;
    *gain=(u32)(10^((temp_gain*3)/200))*1024;
    if (gain_reg[1].data & 0x10)
       *gain = (*gain) * 2;
#endif
    *gain = params->expo.final_gain;
    SENSOR_DMSG("[%s] get gain %u\n", __FUNCTION__, *gain);

    return SUCCESS;
}

static int pCus_SetAEGain(ms_cus_sensor* handle, u32 gain)
{
    imx415_params* params = (imx415_params*)handle->private_data;
    u32 i;
    CUS_GAIN_GAP_ARRAY* Sensor_Gain_Linearity;
    u64 gain_double;

    params->expo.final_gain = gain;
    if (gain < SENSOR_MIN_GAIN)
        gain = SENSOR_MIN_GAIN;
    else if (gain >= SENSOR_MAX_GAIN)
        gain = SENSOR_MAX_GAIN;

    Sensor_Gain_Linearity = gain_gap_compensate;

    for (i = 0; i < sizeof(gain_gap_compensate) / sizeof(CUS_GAIN_GAP_ARRAY); i++) {
        if (Sensor_Gain_Linearity[i].gain == 0)
            break;
        if ((gain > Sensor_Gain_Linearity[i].gain) && (gain < (Sensor_Gain_Linearity[i].gain + Sensor_Gain_Linearity[i].offset))) {
            gain = Sensor_Gain_Linearity[i].gain;
            break;
        }
    }
    gain_double = 20 * (intlog10(gain) - intlog10(1024));
    gain_double = (u16)((gain_double * 10) >> 24) / 3;

    params->tGain_reg[0].data = gain_double & 0xff;
    params->tGain_reg[1].data = (gain_double >> 8) & 0xff;

#if DEBUG_INFO
    SENSOR_DMSG("[%s]gain %u gain_double %llu\n", __FUNCTION__, gain, gain_double);
#endif

    SENSOR_DMSG("[%s] set gain/reg=%u/0x%x 0x%x\n", __FUNCTION__, gain, params->tGain_reg[0].data, gain, params->tGain_reg[1].dat);
    params->dirty = true;
    return SUCCESS;
}

static void pCus_SetAEGainHDR_DOL_Calculate(u32 gain, u16* gain_reg)
{
    // double gain_double;
    u64 gain_double;

    if (gain < SENSOR_MIN_GAIN) {
        gain = SENSOR_MIN_GAIN;
    } else if (gain >= SENSOR_MAX_GAIN) {
        gain = SENSOR_MAX_GAIN;
    }
    gain_double = 20 * (intlog10(gain) - intlog10(1024));
    *gain_reg = (u16)((gain_double * 10) >> 24) / 3;
}

static int pCus_SetAEGainHDR_DOL_SEF1(ms_cus_sensor* handle, u32 gain)
{
    imx415_params* params = (imx415_params*)handle->private_data;
    u16 gain_reg = 0;

    pCus_SetAEGainHDR_DOL_Calculate(gain, &gain_reg);
    params->tGain_hdr_dol_sef_reg[0].data = gain_reg & 0xff;
    params->tGain_hdr_dol_sef_reg[1].data = (gain_reg >> 8) & 0xff;

    SENSOR_DMSG("[%s] set gain/reg=%u/0x%x 0x%x\n", __FUNCTION__, gain, params->tGain_hdr_dol_sef_reg[0].data, params->tGain_hdr_dol_sef_reg[1].data);

    params->dirty = true;
    return SUCCESS;
}

static int pCus_GetAEMinMaxUSecs(ms_cus_sensor* handle, u32* min, u32* max)
{
    *min = 1;
    *max = 1000000 / imx415_mipi_linear[0].senout.min_fps;
    return SUCCESS;
}

static int pCus_GetAEMinMaxGain(ms_cus_sensor* handle, u32* min, u32* max)
{
    *min = SENSOR_MIN_GAIN; // handle->sat_mingain;
    *max = SENSOR_MAX_GAIN; // 10^(72db/20)*1024;
    return SUCCESS;
}

static int IMX415_GetShutterInfo(struct __ms_cus_sensor* handle, CUS_SHUTTER_INFO* info)
{
    info->max = 1000000000 / imx415_mipi_linear[0].senout.min_fps; //
    info->min = (Preview_line_period * 1);
    info->step = Preview_line_period;
    return SUCCESS;
}

static int IMX415_GetShutterInfo_HDR_DOL_SEF(struct __ms_cus_sensor* handle, CUS_SHUTTER_INFO* info)
{
    imx415_params* params = (imx415_params*)handle->private_data;
    info->max = Preview_line_period_HDR_DOL_4LANE * params->min_rhs1;
    info->min = (Preview_line_period_HDR_DOL_4LANE * 5);
    info->step = Preview_line_period_HDR_DOL_4LANE;
    return SUCCESS;
}

int cus_camsensor_init_handle_linear(ms_cus_sensor* drv_handle)
{
    ms_cus_sensor* handle = drv_handle;
    imx415_params* params;
    int res;

    if (!handle) {
        SENSOR_DMSG("[%s] not enough memory!\n", __FUNCTION__);
        return FAIL;
    }
    SENSOR_DMSG("[%s]", __FUNCTION__);
    ////////////////////////////////////
    // private data allocation & init //
    ////////////////////////////////////
    if (handle->private_data == NULL) {
        SENSOR_EMSG("[%s] Private data is empty!\n", __FUNCTION__);
        return FAIL;
    }

    params = (imx415_params*)handle->private_data;
    memcpy(params->tVts_reg, vts_reg, sizeof(vts_reg));
    memcpy(params->tGain_reg, gain_reg, sizeof(gain_reg));
    memcpy(params->tExpo_reg, expo_reg, sizeof(expo_reg));

    ////////////////////////////////////
    //    sensor model ID             //
    ////////////////////////////////////
    sprintf(handle->model_id, "IMX415_MIPI");

    ////////////////////////////////////
    //    i2c config                  //
    ////////////////////////////////////
    handle->i2c_cfg.mode = SENSOR_I2C_LEGACY; //(CUS_ISP_I2C_MODE) FALSE;
    handle->i2c_cfg.fmt = SENSOR_I2C_FMT; // CUS_I2C_FMT_A16D8;
    handle->i2c_cfg.address = SENSOR_I2C_ADDR; // 0x34;
    handle->i2c_cfg.speed = SENSOR_I2C_SPEED; // 300000;

    ////////////////////////////////////
    //    mclk                        //
    ////////////////////////////////////
    // handle->mclk                  = UseParaMclk(SENSOR_DRV_PARAM_MCLK());
    handle->mclk = Preview_MCLK_SPEED;

    ////////////////////////////////////
    //    sensor interface info       //
    ////////////////////////////////////
    handle->isp_type = SENSOR_ISP_TYPE;
    // handle->data_fmt              = SENSOR_DATAFMT;
    handle->sif_bus = SENSOR_IFBUS_TYPE;
    handle->data_prec = SENSOR_DATAPREC;
    handle->data_mode = SENSOR_DATAMODE;
    handle->bayer_id = SENSOR_BAYERID;
    handle->RGBIR_id = SENSOR_RGBIRID;
    handle->orient = SENSOR_ORIT;
    // handle->YC_ODER               = SENSOR_YCORDER;   //CUS_SEN_YCODR_CY;
    handle->interface_attr.attr_mipi.mipi_lane_num = SENSOR_MIPI_LANE_NUM;
    handle->interface_attr.attr_mipi.mipi_data_format = CUS_SEN_INPUT_FORMAT_RGB; // RGB pattern.
    handle->interface_attr.attr_mipi.mipi_yuv_order = 0; // don't care in RGB pattern.
    handle->interface_attr.attr_mipi.mipi_hsync_mode = SENSOR_MIPI_HSYNC_MODE;
    handle->interface_attr.attr_mipi.mipi_hdr_mode = CUS_HDR_MODE_NONE;
    handle->interface_attr.attr_mipi.mipi_hdr_virtual_channel_num = 0; // Short frame

    ////////////////////////////////////
    //    resolution capability       //
    ////////////////////////////////////
    handle->video_res_supported.ulcur_res = 0; // default resolution index is 0.
    // handle->video_res_supported.num_res = LINEAR_RES_END;
    for (res = 0; res < LINEAR_RES_END; res++) {
        handle->video_res_supported.num_res = res + 1;
        handle->video_res_supported.res[res].width = imx415_mipi_linear[res].senif.preview_w;
        handle->video_res_supported.res[res].height = imx415_mipi_linear[res].senif.preview_h;
        handle->video_res_supported.res[res].max_fps = imx415_mipi_linear[res].senout.max_fps;
        handle->video_res_supported.res[res].min_fps = imx415_mipi_linear[res].senout.min_fps;
        handle->video_res_supported.res[res].crop_start_x = imx415_mipi_linear[res].senif.crop_start_X;
        handle->video_res_supported.res[res].crop_start_y = imx415_mipi_linear[res].senif.crop_start_y;
        handle->video_res_supported.res[res].nOutputWidth = imx415_mipi_linear[res].senout.width;
        handle->video_res_supported.res[res].nOutputHeight = imx415_mipi_linear[res].senout.height;
        sprintf(handle->video_res_supported.res[res].strResDesc, imx415_mipi_linear[res].senstr.strResDesc);
    }

    ////////////////////////////////////
    //    Sensor polarity             //
    ////////////////////////////////////
    handle->pwdn_POLARITY = SENSOR_PWDN_POL; // CUS_CLK_POL_NEG;
    handle->reset_POLARITY = SENSOR_RST_POL; // CUS_CLK_POL_NEG;
    // handle->VSYNC_POLARITY             = SENSOR_VSYNC_POL; //CUS_CLK_POL_POS;
    // handle->HSYNC_POLARITY             = SENSOR_HSYNC_POL; //CUS_CLK_POL_POS;
    // handle->PCLK_POLARITY              = SENSOR_PCLK_POL;  //CUS_CLK_POL_POS);    // use '!' to clear board latch error

    ////////////////////////////////////////
    // Sensor Status Control and Get Info //
    ////////////////////////////////////////
    handle->pCus_sensor_release = cus_camsensor_release_handle;
    handle->pCus_sensor_init = pCus_init_8m_30fps_mipi4lane_linear;
    // handle->pCus_sensor_powerupseq     = pCus_powerupseq;
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
    // handle->pCus_sensor_GetSensorCap    = pCus_GetSensorCap;
    handle->pCus_sensor_SetPatternMode = imx415_SetPatternMode; // NONE

    ////////////////////////////////////
    //    AE parameters               //
    ////////////////////////////////////
    handle->ae_gain_delay = SENSOR_GAIN_DELAY_FRAME_COUNT;
    handle->ae_shutter_delay = SENSOR_SHUTTER_DELAY_FRAME_COUNT;
    handle->ae_gain_ctrl_num = 1;
    handle->ae_shutter_ctrl_num = 1;
    handle->sat_mingain = SENSOR_MIN_GAIN; // g_sensor_ae_min_gain;
    // handle->dgain_remainder = 0;

    ////////////////////////////////////
    //  AE Control and Get Info       //
    ////////////////////////////////////
    // unit: micro seconds
    // handle->pCus_sensor_GetAETrigger_mode      = pCus_GetAETrigger_mode;
    // handle->pCus_sensor_SetAETrigger_mode      = pCus_SetAETrigger_mode;
    handle->pCus_sensor_AEStatusNotify = pCus_AEStatusNotify;
    handle->pCus_sensor_GetAEUSecs = pCus_GetAEUSecs;
    handle->pCus_sensor_SetAEUSecs = pCus_SetAEUSecs;
    handle->pCus_sensor_GetAEGain = pCus_GetAEGain;
    handle->pCus_sensor_SetAEGain = pCus_SetAEGain;

    handle->pCus_sensor_GetAEMinMaxGain = pCus_GetAEMinMaxGain;
    handle->pCus_sensor_GetAEMinMaxUSecs = pCus_GetAEMinMaxUSecs;
    // handle->pCus_sensor_GetDGainRemainder = pCus_GetDGainRemainder;

    // sensor calibration
    // handle->pCus_sensor_SetAEGain_cal   = pCus_SetAEGain_cal;
    // handle->pCus_sensor_setCaliData_gain_linearity=pCus_setCaliData_gain_linearity;
    handle->pCus_sensor_GetShutterInfo = IMX415_GetShutterInfo;

    params->expo.vts = vts_30fps;

    return SUCCESS;
}

int cus_camsensor_init_handle_hdr_dol_sef(ms_cus_sensor* drv_handle)
{
    ms_cus_sensor* handle = drv_handle;
    imx415_params* params = NULL;
    int res;

    if (!handle) {
        SENSOR_DMSG("[%s] not enough memory!\n", __FUNCTION__);
        return FAIL;
    }
    SENSOR_DMSG("[%s]", __FUNCTION__);
    ////////////////////////////////////
    // private data allocation & init //
    ////////////////////////////////////
    if (handle->private_data == NULL) {
        SENSOR_EMSG("[%s] Private data is empty!\n", __FUNCTION__);
        return FAIL;
    }

    params = (imx415_params*)handle->private_data;
    memcpy(params->tVts_reg, vts_reg_hdr, sizeof(vts_reg_hdr));
    memcpy(params->tRHS1_reg, expo_RHS1_reg, sizeof(expo_RHS1_reg));
    memcpy(params->tSHR1_reg, expo_SHR1_reg, sizeof(expo_SHR1_reg));
    memcpy(params->tGain_hdr_dol_sef_reg, gain_HDR_DOL_SEF_reg, sizeof(gain_HDR_DOL_SEF_reg));

    ////////////////////////////////////
    //    sensor model ID             //
    ////////////////////////////////////
    sprintf(handle->model_id, "IMX415_MIPI_HDR_SEF");

    ////////////////////////////////////
    //    i2c config                  //
    ////////////////////////////////////
    handle->i2c_cfg.mode = SENSOR_I2C_LEGACY; //(CUS_ISP_I2C_MODE) FALSE;
    handle->i2c_cfg.fmt = SENSOR_I2C_FMT; // CUS_I2C_FMT_A16D16;
    handle->i2c_cfg.address = SENSOR_I2C_ADDR; // 0x5a;
    handle->i2c_cfg.speed = SENSOR_I2C_SPEED; // 320000;

    ////////////////////////////////////
    //    mclk                        //
    ////////////////////////////////////
    handle->mclk = Preview_MCLK_SPEED_HDR_DOL;

    ////////////////////////////////////
    //    sensor interface info       //
    ////////////////////////////////////
    // handle->isp_type              = SENSOR_ISP_TYPE;
    // handle->data_fmt              = SENSOR_DATAFMT;
    handle->sif_bus = SENSOR_IFBUS_TYPE;
    handle->data_prec = SENSOR_DATAPREC_DOL;
    handle->data_mode = SENSOR_DATAMODE;
    handle->bayer_id = SENSOR_BAYERID_HDR_DOL;
    handle->RGBIR_id = SENSOR_RGBIRID;

    handle->interface_attr.attr_mipi.mipi_lane_num = 4; // hdr_lane_num;
    handle->interface_attr.attr_mipi.mipi_data_format = CUS_SEN_INPUT_FORMAT_RGB;
    handle->interface_attr.attr_mipi.mipi_hsync_mode = SENSOR_MIPI_HSYNC_MODE_HDR_DOL;
    handle->interface_attr.attr_mipi.mipi_hdr_mode = CUS_HDR_MODE_DCG; // SONY IMX415 as VC mode
    handle->interface_attr.attr_mipi.mipi_hdr_virtual_channel_num = 1; // Short frame

    ////////////////////////////////////
    //    resolution capability       //
    ////////////////////////////////////
    handle->video_res_supported.ulcur_res = 0; // default resolution index is 0.
    // handle->video_res_supported.num_res = HDR_RES_END;
    for (res = 0; res < HDR_RES_END; res++) {
        handle->video_res_supported.num_res = res + 1;
        handle->video_res_supported.res[res].width = imx415_mipi_hdr[res].senif.preview_w;
        handle->video_res_supported.res[res].height = imx415_mipi_hdr[res].senif.preview_h;
        handle->video_res_supported.res[res].max_fps = imx415_mipi_hdr[res].senout.max_fps;
        handle->video_res_supported.res[res].min_fps = imx415_mipi_hdr[res].senout.min_fps;
        handle->video_res_supported.res[res].crop_start_x = imx415_mipi_hdr[res].senif.crop_start_X;
        handle->video_res_supported.res[res].crop_start_y = imx415_mipi_hdr[res].senif.crop_start_y;
        handle->video_res_supported.res[res].nOutputWidth = imx415_mipi_hdr[res].senout.width;
        handle->video_res_supported.res[res].nOutputHeight = imx415_mipi_hdr[res].senout.height;
        sprintf(handle->video_res_supported.res[res].strResDesc, imx415_mipi_hdr[res].senstr.strResDesc);
    }

    ////////////////////////////////////
    //    Sensor polarity             //
    ////////////////////////////////////
    handle->pwdn_POLARITY = SENSOR_PWDN_POL; // CUS_CLK_POL_NEG;
    handle->reset_POLARITY = SENSOR_RST_POL; // CUS_CLK_POL_NEG;
    // handle->VSYNC_POLARITY             = SENSOR_VSYNC_POL;     //CUS_CLK_POL_POS;
    // handle->HSYNC_POLARITY             = SENSOR_HSYNC_POL;     //CUS_CLK_POL_POS;
    handle->PCLK_POLARITY = SENSOR_PCLK_POL; // CUS_CLK_POL_POS);  // use '!' to clear board latch error

    ////////////////////////////////////////
    // Sensor Status Control and Get Info //
    ////////////////////////////////////////
    handle->pCus_sensor_release = cus_camsensor_release_handle;
    handle->pCus_sensor_init = pCus_init_mipi4lane_HDR_DOL;
    handle->pCus_sensor_poweron = pCus_poweron; // Need to check
    handle->pCus_sensor_poweroff = pCus_poweroff; // Need to check
    handle->pCus_sensor_GetSensorID = pCus_GetSensorID; // Need to check
    handle->pCus_sensor_GetVideoResNum = pCus_GetVideoResNum;
    handle->pCus_sensor_SetVideoRes = pCus_SetVideoRes_HDR_DOL_SEF; // Need to check
    handle->pCus_sensor_GetVideoRes = pCus_GetVideoRes;
    handle->pCus_sensor_GetCurVideoRes = pCus_GetCurVideoRes;

    handle->pCus_sensor_GetOrien = pCus_GetOrien; // Need to check
    handle->pCus_sensor_SetOrien = pCus_SetOrien; // Need to check
    handle->pCus_sensor_GetFPS = pCus_GetFPS_HDR_DOL_SEF; // Need to check
    handle->pCus_sensor_SetFPS = pCus_SetFPS_HDR_DOL_SEF; // Need to check

    ////////////////////////////////////
    //    AE parameters               //
    ////////////////////////////////////
    handle->ae_gain_delay = SENSOR_GAIN_DELAY_FRAME_COUNT;
    handle->ae_shutter_delay = SENSOR_SHUTTER_DELAY_FRAME_COUNT_HDR_DOL;
    handle->ae_gain_ctrl_num = 2;
    handle->ae_shutter_ctrl_num = 2;
    handle->sat_mingain = SENSOR_MIN_GAIN; // g_sensor_ae_min_gain;
    // handle->dgain_remainder = 0;

    ////////////////////////////////////
    //  AE Control and Get Info       //
    ////////////////////////////////////
    handle->pCus_sensor_AEStatusNotify = pCus_AEStatusNotifyHDR_DOL_SEF; // Need to check
    handle->pCus_sensor_GetAEUSecs = pCus_GetAEUSecs;
    handle->pCus_sensor_SetAEUSecs = pCus_SetAEUSecsHDR_DOL_SEF; // Need to check
    handle->pCus_sensor_GetAEGain = pCus_GetAEGain;
    handle->pCus_sensor_SetAEGain = pCus_SetAEGainHDR_DOL_SEF1; // Need to check
    handle->pCus_sensor_GetAEMinMaxGain = pCus_GetAEMinMaxGain;
    handle->pCus_sensor_GetAEMinMaxUSecs = pCus_GetAEMinMaxUSecs;
    // handle->pCus_sensor_GetDGainRemainder = pCus_GetDGainRemainder;

    handle->pCus_sensor_GetShutterInfo = IMX415_GetShutterInfo_HDR_DOL_SEF;
    params->expo.vts = vts_30fps_HDR_DOL_4lane;

    return SUCCESS;
}

// lef functions
static int pCus_init_HDR_DOL_LEF(ms_cus_sensor* handle)
{
    return SUCCESS;
}

static int pCus_poweron_HDR_DOL_LEF(ms_cus_sensor* handle, u32 idx)
{
    return SUCCESS;
}

static int pCus_poweroff_HDR_DOL_LEF(ms_cus_sensor* handle, u32 idx)
{
    return SUCCESS;
}

static int pCus_GetSensorID_HDR_DOL_LEF(ms_cus_sensor* handle, u32* id)
{
    return SUCCESS;
}

static int pCus_GetOrien_HDR_DOL_LEF(ms_cus_sensor* handle, CUS_CAMSENSOR_ORIT* orit)
{
    return SUCCESS;
}

static int pCus_SetOrien_HDR_DOL_LEF(ms_cus_sensor* handle, CUS_CAMSENSOR_ORIT orit)
{
    imx415_params* params = (imx415_params*)handle->private_data;

    handle->orient = orit;
    params->orien_dirty = true;

    return SUCCESS;
}

static int pCus_GetFPS_HDR_DOL_LEF(ms_cus_sensor* handle)
{
    imx415_params* params = (imx415_params*)handle->private_data;
    u32 cur_vts_30fps = vts_30fps_HDR_DOL_4lane;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 tVts = (params->tVts_reg[0].data << 16) | (params->tVts_reg[1].data << 8) | (params->tVts_reg[2].data << 0);

    if (params->expo.fps >= 1000)
        params->expo.preview_fps = (cur_vts_30fps * max_fps * 1000) / tVts;
    else
        params->expo.preview_fps = (cur_vts_30fps * max_fps) / tVts;

    return params->expo.preview_fps;
}

static int pCus_SetFPS_HDR_DOL_LEF(ms_cus_sensor* handle, u32 fps)
{
    u32 cur_vts_30fps = 0; // vts = 0,
    imx415_params* params = (imx415_params*)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 min_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].min_fps;
    // cur_vts_30fps = imx415_mipi_hdr[0].senout.height;
    cur_vts_30fps = vts_30fps_HDR_DOL_4lane;
    if (fps >= min_fps && fps <= max_fps) {
        if (CUS_CMU_CLK_36MHZ == handle->mclk) {
            fps = fps > 14 ? 14 : fps; // limit fps at 29 fps due to MCLK=36MHz
            params->expo.vts = (cur_vts_30fps * 14545 + fps * 500) / (fps * 1000);
        } else
            params->expo.vts = (cur_vts_30fps * (max_fps * 1000) + fps * 500) / (fps * 1000);

    } else if (fps >= (min_fps * 1000) && fps <= (max_fps * 1000)) {
        if (CUS_CMU_CLK_36MHZ == handle->mclk) {
            fps = fps > 14 ? 14 : fps; // limit fps at 29.091 fps due to MCLK=36MHz
            params->expo.vts = (cur_vts_30fps * 14545 + (fps >> 1)) / fps;
        } else
            params->expo.vts = (cur_vts_30fps * (max_fps * 1000) + (fps >> 1)) / fps;

    } else {
        SENSOR_DMSG("[%s] FPS %d out of range.\n", __FUNCTION__, fps);
        return FAIL;
    }

    if (params->expo.expo_lines > params->expo.vts - 4) {
        // vts = params->expo.expo_lines + 4;
    } else {
        // vts = params->expo.vts;
    }

    params->dirty = true;
    params->expo.fps = fps;
    pCus_SetAEUSecsHDR_DOL_LEF(handle, params->expo.expo_sef_us);

    return SUCCESS;
}

static int pCus_AEStatusNotifyHDR_DOL_LEF(ms_cus_sensor* handle, CUS_CAMSENSOR_AE_STATUS_NOTIFY status)
{
    imx415_params* params = (imx415_params*)handle->private_data;
    ISensorIfAPI* sensor_if = handle->sensor_if_api;

    switch (status) {
    case CUS_FRAME_INACTIVE:

        break;
    case CUS_FRAME_ACTIVE:

        if (params->dirty || params->orien_dirty) {
            SensorReg_Write(0x3001, 1);
            if (params->dirty) {
                SensorRegArrayW((I2C_ARRAY*)params->tVts_reg, ARRAY_SIZE(vts_reg_hdr));
                SensorRegArrayW((I2C_ARRAY*)params->tSHR0_reg, ARRAY_SIZE(expo_SHR0_reg));
                SensorRegArrayW((I2C_ARRAY*)params->tSHR1_reg, ARRAY_SIZE(expo_SHR1_reg));
                SensorRegArrayW((I2C_ARRAY*)params->tRHS1_reg, ARRAY_SIZE(expo_RHS1_reg));
                SensorRegArrayW((I2C_ARRAY*)params->tGain_hdr_dol_lef_reg, ARRAY_SIZE(gain_HDR_DOL_LEF_reg));
                SensorRegArrayW((I2C_ARRAY*)params->tGain_hdr_dol_sef_reg, ARRAY_SIZE(gain_HDR_DOL_SEF_reg));
                params->dirty = false;
            }
            if (params->orien_dirty) {
                DoOrien(handle, handle->orient);
                params->orien_dirty = false;
                params->skip_cnt = true;
            }
            SensorReg_Write(0x3001, 0);
        }
        if (params->skip_cnt) {
            sensor_if->SetSkipFrame(handle->snr_pad_group, params->expo.fps, params->skip_cnt);
            params->skip_cnt = 0;
        }
        break;
    default:
        break;
    }
    return SUCCESS;
}

static int pCus_GetAEUSecs_HDR_DOL_LEF(ms_cus_sensor* handle, u32* us)
{
    *us = 0;
    return SUCCESS;
}

static int pCus_SetAEUSecsHDR_DOL_LEF(ms_cus_sensor* handle, u32 us)
{
    u32 expo_lines_lef = 0, vts = 0, fsc = 0;
    u32 cur_line_period = Preview_line_period_HDR_DOL_4LANE;
    // u32 cur_vts_30fps = vts_30fps_HDR_DOL_4lane;
    imx415_params* params = (imx415_params*)handle->private_data;
    cur_line_period = Preview_line_period_HDR_DOL_4LANE;
    // cur_vts_30fps =vts_30fps_HDR_DOL_4lane;
    expo_lines_lef = (1000 * us + (cur_line_period >> 1)) / cur_line_period;

    params->expo.expo_lef_us = us;
    fsc = params->expo.vts * 2;
    params->fsc = ((fsc >> 2) << 2) + 4; // 4n

    // params->max_shr0 = fsc - expo_lines_lef - 8;
    params->max_shr0 = (fsc - 8) - expo_lines_lef;
    if (params->max_shr0 < (params->min_rhs1 + 9))
        params->max_shr0 = params->min_rhs1 + 9;
    params->max_shr0 = ((params->max_shr0 >> 1) << 1) + 2;

    if (expo_lines_lef > (fsc - params->min_rhs1 - 9)) {
        vts = (expo_lines_lef + params->min_rhs1 + 9) / 2;
    } else {
        vts = params->expo.vts;
    }
    params->expo.expo_lines = expo_lines_lef;
    // params->expo.vts = vts;

    SENSOR_DMSG("[%s] us %u, expo_lines_lef %u, vts %u, SHR0 %u \n", __FUNCTION__,
        us,
        expo_lines_lef,
        vts,
        params->max_shr0);
    params->tSHR0_reg[0].data = (params->max_shr0 >> 16) & 0x0003;
    params->tSHR0_reg[1].data = (params->max_shr0 >> 8) & 0x00ff;
    params->tSHR0_reg[2].data = (params->max_shr0 >> 0) & 0x00ff;

    params->tVts_reg[0].data = (vts >> 16) & 0x0003;
    params->tVts_reg[1].data = (vts >> 8) & 0x00ff;
    params->tVts_reg[2].data = (vts >> 0) & 0x00ff;

    params->dirty = true;
    return SUCCESS;
}

static int pCus_GetAEGain_HDR_DOL_LEF(ms_cus_sensor* handle, u32* gain)
{
    *gain = 0;
    return SUCCESS;
}

static int pCus_SetAEGainHDR_DOL_LEF(ms_cus_sensor* handle, u32 gain)
{
    imx415_params* params = (imx415_params*)handle->private_data;
    u16 gain_reg = 0;

    pCus_SetAEGainHDR_DOL_Calculate(gain, &gain_reg);
    params->tGain_hdr_dol_lef_reg[0].data = gain_reg & 0xff;
    params->tGain_hdr_dol_lef_reg[1].data = (gain_reg >> 8) & 0xff;

    SENSOR_DMSG("[%s] set gain/reg=%u/0x%x\n", __FUNCTION__, gain, params->tGain_hdr_dol_lef_reg[0].data, params->tGain_hdr_dol_lef_reg[1].data);

    params->dirty = true;
    return SUCCESS;
}

static int pCus_GetAEMinMaxGain_HDR_DOL_LEF(ms_cus_sensor* handle, u32* min, u32* max)
{
    *min = handle->sat_mingain;
    *max = SENSOR_MAX_GAIN;
    return SUCCESS;
}

static int pCus_GetAEMinMaxUSecs_HDR_DOL_LEF(ms_cus_sensor* handle, u32* min, u32* max)
{
    *min = 1;
    *max = 1000000 / imx415_mipi_hdr[0].senout.min_fps;
    return SUCCESS;
}

#if 0
//static int pCus_GetDGainRemainder(ms_cus_sensor *handle, u32 *dgain_remainder)
//{
//    *dgain_remainder = handle->dgain_remainder;

//    return SUCCESS;
//}

static int pCus_SetAEGain_cal_hdr_dol_lef(ms_cus_sensor *handle, u32 gain)
{
    return SUCCESS;
}

static int pCus_setCaliData_gain_linearity_hdr_dol_lef(ms_cus_sensor* handle, CUS_GAIN_GAP_ARRAY* pArray, u32 num)
{
    return SUCCESS;
}
#endif

static int IMX415_GetShutterInfo_HDR_DOL_LEF(struct __ms_cus_sensor* handle, CUS_SHUTTER_INFO* info)
{
    info->max = 1000000000 / imx415_mipi_hdr[0].senout.min_fps;
    info->min = (Preview_line_period_HDR_DOL_4LANE * 5);
    info->step = Preview_line_period_HDR_DOL_4LANE;
    return SUCCESS;
}

static int cus_camsensor_init_handle_hdr_dol_lef(ms_cus_sensor* drv_handle)
{
    ms_cus_sensor* handle = drv_handle;
    imx415_params* params;
    s32 res;
    if (!handle) {
        SENSOR_DMSG("[%s] not enough memory!\n", __FUNCTION__);
        return FAIL;
    }

    // private data allocation & init
    if (handle->private_data == NULL) {
        SENSOR_EMSG("[%s] Private data is empty!\n", __FUNCTION__);
        return FAIL;
    }
    params = (imx415_params*)handle->private_data;
    memcpy(params->tVts_reg, vts_reg_hdr, sizeof(vts_reg_hdr));
    memcpy(params->tSHR0_reg, expo_SHR0_reg, sizeof(expo_SHR0_reg));
    memcpy(params->tGain_hdr_dol_lef_reg, gain_HDR_DOL_LEF_reg, sizeof(gain_HDR_DOL_LEF_reg));

    ////////////////////////////////////
    //    sensor model ID             //
    ////////////////////////////////////
    sprintf(handle->model_id, "IMX415_MIPI_HDR_LEF");

    ////////////////////////////////////
    //    i2c config                  //
    ////////////////////////////////////
    handle->i2c_cfg.mode = SENSOR_I2C_LEGACY; //(CUS_ISP_I2C_MODE) FALSE;
    handle->i2c_cfg.fmt = SENSOR_I2C_FMT; // CUS_I2C_FMT_A16D16;
    handle->i2c_cfg.address = SENSOR_I2C_ADDR; // 0x5a;
    handle->i2c_cfg.speed = SENSOR_I2C_SPEED; // 320000;

    ////////////////////////////////////
    //    mclk                        //
    ////////////////////////////////////
    // handle->mclk                   = UseParaMclk(SENSOR_DRV_PARAM_MCLK());
    handle->mclk = Preview_MCLK_SPEED_HDR_DOL;
    ////////////////////////////////////
    //    sensor interface info       //
    ////////////////////////////////////
    // handle->isp_type             = SENSOR_ISP_TYPE;    //ISP_SOC;
    // handle->data_fmt             = SENSOR_DATAFMT;     //CUS_DATAFMT_YUV;
    handle->sif_bus = SENSOR_IFBUS_TYPE; // CUS_SENIF_BUS_PARL;
    handle->data_prec = SENSOR_DATAPREC_DOL; // CUS_DATAPRECISION_8;
    handle->data_mode = SENSOR_DATAMODE;
    handle->bayer_id = SENSOR_BAYERID_HDR_DOL; // CUS_BAYER_GB;
    handle->RGBIR_id = SENSOR_RGBIRID;
    handle->orient = SENSOR_ORIT; // CUS_ORIT_M1F1;
    // handle->YC_ODER                = SENSOR_YCORDER;   //CUS_SEN_YCODR_CY;
    handle->interface_attr.attr_mipi.mipi_lane_num = 4; // hdr_lane_num;
    handle->interface_attr.attr_mipi.mipi_data_format = CUS_SEN_INPUT_FORMAT_RGB; // RGB pattern.
    handle->interface_attr.attr_mipi.mipi_yuv_order = 0; // don't care in RGB pattern.
    handle->interface_attr.attr_mipi.mipi_hsync_mode = SENSOR_MIPI_HSYNC_MODE_HDR_DOL;
    handle->interface_attr.attr_mipi.mipi_hdr_mode = CUS_HDR_MODE_DCG;
    handle->interface_attr.attr_mipi.mipi_hdr_virtual_channel_num = 0; // Long frame

    ////////////////////////////////////
    //    resolution capability       //
    ////////////////////////////////////

    handle->video_res_supported.ulcur_res = 0; // default resolution index is 0.
    // handle->video_res_supported.num_res = HDR_RES_END;
    for (res = 0; res < HDR_RES_END; res++) {
        handle->video_res_supported.num_res = res + 1;
        handle->video_res_supported.res[res].width = imx415_mipi_hdr[res].senif.preview_w;
        handle->video_res_supported.res[res].height = imx415_mipi_hdr[res].senif.preview_h;
        handle->video_res_supported.res[res].max_fps = imx415_mipi_hdr[res].senout.max_fps;
        handle->video_res_supported.res[res].min_fps = imx415_mipi_hdr[res].senout.min_fps;
        handle->video_res_supported.res[res].crop_start_x = imx415_mipi_hdr[res].senif.crop_start_X;
        handle->video_res_supported.res[res].crop_start_y = imx415_mipi_hdr[res].senif.crop_start_y;
        handle->video_res_supported.res[res].nOutputWidth = imx415_mipi_hdr[res].senout.width;
        handle->video_res_supported.res[res].nOutputHeight = imx415_mipi_hdr[res].senout.height;
        sprintf(handle->video_res_supported.res[res].strResDesc, imx415_mipi_hdr[res].senstr.strResDesc);
    }

    ////////////////////////////////////
    //    Sensor polarity             //
    ////////////////////////////////////
    handle->pwdn_POLARITY = SENSOR_PWDN_POL; // CUS_CLK_POL_NEG;
    handle->reset_POLARITY = SENSOR_RST_POL; // CUS_CLK_POL_NEG;
    // handle->VSYNC_POLARITY             = SENSOR_VSYNC_POL;     //CUS_CLK_POL_POS;
    // handle->HSYNC_POLARITY             = SENSOR_HSYNC_POL;     //CUS_CLK_POL_POS;
    // handle->PCLK_POLARITY              = SENSOR_PCLK_POL;      //CUS_CLK_POL_POS);    // use '!' to clear board latch error
    /////////////////////////////////////////////////////

    ////////////////////////////////////////
    // Sensor Status Control and Get Info //
    ////////////////////////////////////////
    // LOGD("[%s:%d]\n", __FUNCTION__, __LINE__);
    handle->pCus_sensor_release = cus_camsensor_release_handle;
    handle->pCus_sensor_init = pCus_init_HDR_DOL_LEF;
    // handle->pCus_sensor_powerupseq      = pCus_powerupseq   ;
    handle->pCus_sensor_poweron = pCus_poweron_HDR_DOL_LEF;
    handle->pCus_sensor_poweroff = pCus_poweroff_HDR_DOL_LEF;
    handle->pCus_sensor_GetSensorID = pCus_GetSensorID_HDR_DOL_LEF;
    // handle->pCus_sensor_GetVideoResNum  = NULL;
    // handle->pCus_sensor_GetVideoRes     = NULL;
    // handle->pCus_sensor_GetCurVideoRes  = NULL;
    handle->pCus_sensor_SetVideoRes = pCus_SetVideoRes_HDR_DOL_LEF; // Need to check

    handle->pCus_sensor_GetOrien = pCus_GetOrien_HDR_DOL_LEF;
    handle->pCus_sensor_SetOrien = pCus_SetOrien_HDR_DOL_LEF;
    handle->pCus_sensor_GetFPS = pCus_GetFPS_HDR_DOL_LEF;
    handle->pCus_sensor_SetFPS = pCus_SetFPS_HDR_DOL_LEF;
    // handle->pCus_sensor_GetSensorCap    = pCus_GetSensorCap_hdr_dol_lef;
    // handle->pCus_sensor_SetPatternMode  = imx415_SetPatternMode_hdr_dol_lef;

    ////////////////////////////////////
    //    AE parameters               //
    ////////////////////////////////////
    handle->ae_gain_delay = SENSOR_GAIN_DELAY_FRAME_COUNT;
    handle->ae_shutter_delay = SENSOR_SHUTTER_DELAY_FRAME_COUNT_HDR_DOL;
    handle->ae_gain_ctrl_num = 2;
    handle->ae_shutter_ctrl_num = 2;
    handle->sat_mingain = SENSOR_MIN_GAIN;
    // handle->dgain_remainder = 0;

    ////////////////////////////////////
    //  AE Control and Get Info       //
    ////////////////////////////////////
    handle->pCus_sensor_AEStatusNotify = pCus_AEStatusNotifyHDR_DOL_LEF;
    handle->pCus_sensor_GetAEUSecs = pCus_GetAEUSecs_HDR_DOL_LEF;
    handle->pCus_sensor_SetAEUSecs = pCus_SetAEUSecsHDR_DOL_LEF;
    handle->pCus_sensor_GetAEGain = pCus_GetAEGain_HDR_DOL_LEF;
    handle->pCus_sensor_SetAEGain = pCus_SetAEGainHDR_DOL_LEF;

    handle->pCus_sensor_GetAEMinMaxGain = pCus_GetAEMinMaxGain_HDR_DOL_LEF;
    handle->pCus_sensor_GetAEMinMaxUSecs = pCus_GetAEMinMaxUSecs_HDR_DOL_LEF;
    // handle->pCus_sensor_GetDGainRemainder = pCus_GetDGainRemainder;

    handle->pCus_sensor_GetShutterInfo = IMX415_GetShutterInfo_HDR_DOL_LEF;

    params->expo.vts = vts_30fps_HDR_DOL_4lane;

    params->dirty = false;

    return SUCCESS;
}

SENSOR_DRV_ENTRY_IMPL_END_EX(IMX415_HDR,
    cus_camsensor_init_handle_linear,
    cus_camsensor_init_handle_hdr_dol_sef,
    cus_camsensor_init_handle_hdr_dol_lef,
    imx415_params);
