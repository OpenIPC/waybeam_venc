/* SigmaStar trade secret */
/* Copyright (c) [2019~2020] SigmaStar Technology.
All rights reserved.

Unless otherwise stipulated in writing, any and all information contained
herein regardless in any format shall remain the sole proprietary of
SigmaStar and be kept in strict confidence
(SigmaStar Confidential Information) by the recipient.
Any unauthorized act including without limitation unauthorized disclosure,
copying, use, reproduction, sale, distribution, modification, disassembling,
reverse engineering and compiling of the contents of SigmaStar Confidential
Information is unlawful and strictly prohibited. SigmaStar hereby reserves the
rights to any and all damages, losses, costs and expenses resulting therefrom.
*/

#ifdef __cplusplus
extern "C" {
#endif

#include <drv_sensor.h>
#include <drv_sensor_common.h>
#include <sensor_i2c_api.h>

#ifdef __cplusplus
}
#endif

SENSOR_DRV_ENTRY_IMPL_BEGIN_EX(IMX335_HDR);

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
#define SENSOR_MIPI_LANE_NUM (4) // IMX335 Linear mode supports MIPI 2/4 Lane
#define SENSOR_MIPI_LANE_NUM_DOL (4) //(hdr_lane_num)//IMX335 DOL mode supports MIPI 4 Lane
//#define SENSOR_MIPI_HDR_MODE        (0) //0: Non-HDR mode. 1:Sony DOL mode

#define SENSOR_ISP_TYPE ISP_EXT // ISP_EXT, ISP_SOC (Non-used)
#define SENSOR_IFBUS_TYPE CUS_SENIF_BUS_MIPI // CFG //CUS_SENIF_BUS_PARL, CUS_SENIF_BUS_MIPI
#define SENSOR_MIPI_HSYNC_MODE PACKET_HEADER_EDGE1
#define SENSOR_MIPI_HSYNC_MODE_HDR_DOL PACKET_FOOTER_EDGE
#define SENSOR_DATAPREC CUS_DATAPRECISION_10
#define SENSOR_DATAPREC_DOL CUS_DATAPRECISION_10
#define SENSOR_DATAMODE CUS_SEN_10TO12_9098 // CFG
#define SENSOR_BAYERID CUS_BAYER_RG // 0h: CUS_BAYER_RG, 1h: CUS_BAYER_GR, 2h: CUS_BAYER_BG, 3h: CUS_BAYER_GB
#define SENSOR_BAYERID_HDR_DOL CUS_BAYER_RG
#define SENSOR_RGBIRID CUS_RGBIR_NONE
#define SENSOR_ORIT CUS_ORIT_M0F0 // CUS_ORIT_M0F0, CUS_ORIT_M1F0, CUS_ORIT_M0F1, CUS_ORIT_M1F1,

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
#define SENSOR_NAME IMX335

////////////////////////////////////
// Image Info                     //
////////////////////////////////////
static struct { // LINEAR
    // Modify it based on number of support resolution
    enum { LINEAR_RES_1 = 0,
        LINEAR_RES_2,
        LINEAR_RES_3,
        LINEAR_RES_4,
        LINEAR_RES_5,
        LINEAR_RES_6,
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
} imx335_mipi_linear[] = {
    /* Final Star6E IMX335 lineup — fps-ordered, HDR + 50fps removed.
     * idx 0,1,2,4 are the stock OpenIPC infinity6e tables (verbatim); idx 3
     * (100fps) and idx 5 (144fps) are Maruko-style window crops pushed to the
     * highest FOV the I6E ISP sustains (device-verified on .13, 0 drops):
     * 2176x1224@100 = 2.66 MPix (VIF 99.5), 1600x900@144 = 1.44 MPix (VIF 143). */
    { LINEAR_RES_1, { 2560, 1920, 3,  30 }, { 0, 0, 2560, 1920 }, { "2560x1920@30fps"  } },
    { LINEAR_RES_2, { 2560, 1920, 3,  60 }, { 0, 0, 2560, 1920 }, { "2560x1920@60fps"  } },
    { LINEAR_RES_3, { 2560, 1440, 3,  90 }, { 0, 0, 2560, 1440 }, { "2560x1440@90fps"  } },
    { LINEAR_RES_4, { 2176, 1224, 3, 100 }, { 0, 0, 2176, 1224 }, { "2176x1224@100fps" } },
    { LINEAR_RES_5, { 1920, 1080, 3, 120 }, { 0, 0, 1920, 1080 }, { "1920x1080@120fps" } },
    { LINEAR_RES_6, { 1600,  900, 3, 144 }, { 0, 0, 1600,  900 }, { "1600x900@144fps"  } },
};

static struct { // HDR
    // Modify it based on number of support resolution
    enum { HDR_RES_1 = 0,
        HDR_RES_2,
        HDR_RES_3,
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
} imx335_mipi_hdr[] = {
    { HDR_RES_1, { 2592, 1944, 3, 25 }, { 4, 0, 2592, 1944 }, { "2592x1944@25fps_HDR" } }, // Modify it
    { HDR_RES_2, { 2592, 1944, 3, 20 }, { 4, 0, 2592, 1944 }, { "2592x1944@20fps_HDR" } }, // Modify it
    { HDR_RES_3, { 2592, 1944, 3, 30 }, { 4, 0, 2592, 1944 }, { "2592x1944@30fps_HDR" } }, // Modify it
};

#define IMX335_HDR_BRL 2228

u32 Preview_line_period;
u32 vts_30fps;
u32 Preview_MAX_FPS;
u32 Preview_line_period_HDR_DOL;
u32 vts_30fps_HDR_DOL;

////////////////////////////////////
// AE Info                        //
////////////////////////////////////
#define SENSOR_MAX_GAIN (1412 * 1024) // max sensor again, a-gain * conversion-gain*d-gain
#define SENSOR_MIN_GAIN (1 * 1024)
#define SENSOR_GAIN_DELAY_FRAME_COUNT (2)
#define SENSOR_SHUTTER_DELAY_FRAME_COUNT (2)
#define SENSOR_GAIN_DELAY_FRAME_COUNT_HDR_DOL (1)
#define SENSOR_SHUTTER_DELAY_FRAME_COUNT_HDR_DOL (2)

////////////////////////////////////
// Mirror-Flip Info               //
////////////////////////////////////
#define REG_MIRROR 0x304E
#define REG_FLIP 0x304F
#define SENSOR_NOR 0x0
#define MIRROR_EN 0x1 << (0)
#define FLIP_EN 0x1 << (0)

#if defined(SENSOR_MODULE_VERSION)
#define TO_STR_NATIVE(e) #e
#define TO_STR_PROXY(m, e) m(e)
#define MACRO_TO_STRING(e) TO_STR_PROXY(TO_STR_NATIVE, e)
static char* sensor_module_version = MACRO_TO_STRING(SENSOR_MODULE_VERSION);
module_param(sensor_module_version, charp, S_IRUGO);
#endif

// static int pCus_SetAEGain(ms_cus_sensor *handle, u32 gain);
static int pCus_SetAEUSecs(ms_cus_sensor* handle, u32 us);
static int pCus_SetOrien(ms_cus_sensor* handle, CUS_CAMSENSOR_ORIT orit);
static int pCus_SetAEUSecsHDR_DOL_SEF1(ms_cus_sensor* handle, u32 us);
static int pCus_SetAEUSecsHDR_DOL_LEF(ms_cus_sensor* handle, u32 us);

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
    u32 max_rhs1;
    int sen_init;
    int still_min_fps;
    int video_min_fps;
    bool dirty;
    bool orien_dirty;
    I2C_ARRAY tVts_reg[3];
    I2C_ARRAY tGain_reg[2];
    I2C_ARRAY tExpo_reg[3];
    I2C_ARRAY tExpo_shr_dol1_reg[3];
    I2C_ARRAY tExpo_rhs1_reg[3];
    I2C_ARRAY tGain_hdr_dol_lef_reg[2];
    I2C_ARRAY tGain_hdr_dol_sef_reg[2];
} imx335_params;

// set sensor ID address and data,
const static I2C_ARRAY Sensor_id_table[] = {
    { 0x3003, 0x00 }, // {address of ID, ID },
    { 0x3033, 0x00 }
};

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
    /* All-pixel geometry reset (PR#156 discipline): explicitly return the
     * readout window to full-frame so a WARM switch from a windowed crop
     * mode (idx 4/6: 0x3018=0x04, cropped HTRIM/HNUM/Y_OUT) does not leave
     * stale window geometry.  0x3018=0x00 (all-pixel) is also the power-on
     * default, so on cold boot this block is byte-equivalent to the stock
     * table.  With 0x3018=0x00 the sensor ignores the AREA3 window regs, so
     * only WINMODE/HTRIM/HNUM/Y_OUT need resetting.  Values are the IMX335
     * all-pixel readout (2592x1944); VIF crops to the reported 2560x1920. */
    { 0x3018, 0x00 }, // WINMODE: all-pixel scan
    { 0x302C, 0x30 }, // HTRIMMING_START = 48
    { 0x302D, 0x00 },
    { 0x302E, 0x38 }, // HNUM = 2616 (full-width readout)
    { 0x302F, 0x0A },
    { 0x3056, 0x98 }, // Y_OUT_SIZE = 1944
    { 0x3057, 0x07 },
    { 0x305A, 0x00 },
    { 0x3059, 0x0E },
    { 0x3058, 0x80 },
    { 0x314C, 0xB0 },
    { 0x315A, 0x02 },
    { 0x3168, 0x8F },
    { 0x316A, 0x7E },
    { 0x319D, 0x00 }, // MDBIT 0:10Bit, 1:12Bit
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
    { 0x341C, 0xFF }, // 0x47: 12-bit, 0xFF: 10bit
    { 0x341D, 0x01 }, // 0x00: 12-bit, 0x01: 10bit
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
    { 0x319D, 0x00 }, // MDBIT 0:10Bit, 1:12Bit
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
    { 0x341C, 0xFF }, // 0x47: 12-bit, 0xFF: 10bit
    { 0x341D, 0x01 }, // 0x00: 12-bit, 0x01: 10bit
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
    { 0x319D, 0x00 }, // MDBIT 0:10Bit, 1:12Bit
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
    { 0x341C, 0xFF }, // 0x47: 12-bit, 0xFF: 10bit
    { 0x341D, 0x01 }, // 0x00: 12-bit, 0x01: 10bit
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
    { 0x319D, 0x00 }, // MDBIT 0:10Bit, 1:12Bit
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
    { 0x341C, 0xFF }, // 0x47: 12-bit, 0xFF: 10bit
    { 0x341D, 0x01 }, // 0x00: 12-bit, 0x01: 10bit
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

const static I2C_ARRAY Sensor_init_table_HDR_DOL_4lane5m25fps[] = {
    /*"IMX335LQN All-pixel scan CSI-2_4lane 1188Mbps AD:10bit Output:10bit 27MHz Master Mode DOL HDR 2frameLI 25fps" */
    { 0x3002, 0x01 }, // Master mode stop
    { 0xffff, 0x14 }, // delay
    { 0x3000, 0x01 }, // standby
    { 0xffff, 0x14 }, // delay
    { 0x300C, 0x42 },
    { 0x300D, 0x2E },
    { 0x3030, 0x9A }, // VMAX 1518: 5400 //4500 1194 //4250 109A
    { 0x3031, 0x10 },
    { 0x3032, 0x00 },
    { 0x3034, 0x5D }, // HMAX 113: 275, 14A //15Dh 349
    { 0x3035, 0x01 },
    { 0x3048, 0x01 },
    { 0x3049, 0x01 },
    { 0x304A, 0x04 },
    { 0x304B, 0x03 },
    { 0x304C, 0x00 },
    { 0x3050, 0x00 },
    { 0x3056, 0x07 }, // Y_OUT_SIZE[7:0]
    { 0x3057, 0x08 }, // Y_OUT_SIZE[12:8]
    { 0x3058, 0x88 },
    { 0x3059, 0x1D },
    { 0x305A, 0x00 },
    { 0x305C, 0x12 },
    { 0x305D, 0x00 },
    { 0x305E, 0x00 },
    { 0x3068, 0x22 },
    { 0x3069, 0x01 },
    { 0x306A, 0x00 },
    { 0x314C, 0xB0 },
    { 0x315A, 0x02 },
    { 0x3168, 0x8F },
    { 0x316A, 0x7E },
    { 0x319D, 0x00 },
    { 0x319F, 0x00 }, // 0x2
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
    { 0x31D7, 0x01 },
    { 0x31E4, 0x01 },
    { 0x31F3, 0x01 },
    { 0x3288, 0x21 },
    { 0x328A, 0x02 },
    { 0x3300, 0x00 },
    { 0x3414, 0x05 },
    { 0x3416, 0x18 },
    { 0x341C, 0xFF }, // 0x47--12-bit, 0xff--10bit
    { 0x341D, 0x01 }, //
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
    { 0x375d, 0x00 },
    { 0x375e, 0x00 },
    { 0x375f, 0x11 },
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

const static I2C_ARRAY Sensor_init_table_HDR_DOL_4lane5m20fps[] = {
    /*"IMX335LQN All-pixel scan CSI-2_4lane 1188Mbps AD:10bit Output:10bit 27MHz Master Mode DOL HDR 2frameLI 25fps" */
    { 0x3002, 0x01 }, // Master mode stop
    { 0xffff, 0x14 }, // delay
    { 0x3000, 0x01 }, // standby
    { 0xffff, 0x14 }, // delay
    { 0x300C, 0x42 },
    { 0x300D, 0x2E },
    { 0x3030, 0xC0 }, // VMAX 1518: 5400 //4500 1194 //4250 109A
    { 0x3031, 0x14 },
    { 0x3032, 0x00 },
    { 0x3034, 0x5D }, // HMAX 113: 275, 14A //15Dh 349
    { 0x3035, 0x01 },
    { 0x3048, 0x01 },
    { 0x3049, 0x01 },
    { 0x304A, 0x04 },
    { 0x304B, 0x03 },
    { 0x304C, 0x00 },
    { 0x3050, 0x00 },
    { 0x3056, 0x07 }, // Y_OUT_SIZE[7:0]
    { 0x3057, 0x08 }, // Y_OUT_SIZE[12:8]
    { 0x3058, 0x88 },
    { 0x3059, 0x1D },
    { 0x305A, 0x00 },
    { 0x305C, 0x12 },
    { 0x305D, 0x00 },
    { 0x305E, 0x00 },
    { 0x3068, 0x22 },
    { 0x3069, 0x01 },
    { 0x306A, 0x00 },
    { 0x314C, 0xB0 },
    { 0x315A, 0x02 },
    { 0x3168, 0x8F },
    { 0x316A, 0x7E },
    { 0x319D, 0x00 },
    { 0x319F, 0x00 }, // 0x2
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
    { 0x31D7, 0x01 },
    { 0x31E4, 0x01 },
    { 0x31F3, 0x01 },
    { 0x3288, 0x21 },
    { 0x328A, 0x02 },
    { 0x3300, 0x00 },
    { 0x3414, 0x05 },
    { 0x3416, 0x18 },
    { 0x341C, 0xFF }, // 0x47--12-bit, 0xff--10bit
    { 0x341D, 0x01 }, //
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
    { 0x375d, 0x00 },
    { 0x375e, 0x00 },
    { 0x375f, 0x11 },
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

const static I2C_ARRAY Sensor_init_table_HDR_DOL_4lane5m30fps[] = {
/*"IMX335LQN All-pixel scan CSI-2_4lane 1188Mbps AD:10bit Output:10bit 27MHz Master Mode DOL HDR 2frameLI 30fps"*/
    { 0x3002, 0x01 }, // Master mode stop
    { 0xffff, 0x14 }, // delay
    { 0x3000, 0x01 }, // standby
    { 0xffff, 0x14 }, // delay
    { 0x300C, 0x42 },
    { 0x300D, 0x2E },
    { 0x3030, 0x9A }, // VMAX 4506 x119A //4096 1000h
    { 0x3031, 0x11 },
    { 0x3032, 0x00 },
    { 0x3034, 0x13 }, // HMAX 275 x113 //300 : 12Ch
    { 0x3035, 0x01 },
    { 0x3048, 0x01 },
    { 0x3049, 0x01 },
    { 0x304A, 0x04 },
    { 0x304B, 0x03 },
    { 0x304C, 0x00 },
    { 0x3050, 0x00 },
    { 0x3056, 0x07 }, // Y_OUT_SIZE[7:0]
    { 0x3057, 0x08 }, // Y_OUT_SIZE[12:8]
    { 0x3058, 0x1C },
    { 0x3059, 0x22 },
    { 0x305A, 0x00 },
    { 0x305C, 0x12 },
    { 0x305D, 0x00 },
    { 0x305E, 0x00 },
    { 0x3068, 0x22 },
    { 0x3069, 0x01 },
    { 0x306A, 0x00 },
    { 0x314C, 0xB0 },
    { 0x315A, 0x02 },
    { 0x3168, 0x8F },
    { 0x316A, 0x7E },
    { 0x319D, 0x00 },
    { 0x319F, 0x00 }, // 0x2
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
    { 0x31D7, 0x01 },
    { 0x31E4, 0x01 },
    { 0x31F3, 0x01 },
    { 0x3288, 0x21 },
    { 0x328A, 0x02 },
    { 0x3300, 0x00 },
    { 0x3414, 0x05 },
    { 0x3416, 0x18 },
    { 0x341C, 0xFF }, // 0x47--12-bit, 0xff--10bit
    { 0x341D, 0x01 }, //
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
    { 0x375d, 0x00 },
    { 0x375e, 0x00 },
    { 0x375f, 0x11 },
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

static I2C_ARRAY PatternTbl[] = {
    { 0x308c, 0x20 }, // colorbar pattern, bit 0 to enable
};

const static I2C_ARRAY gain_HDR_DOL_LEF_reg[] = {
    { 0x30E8, 0x00 }, // bit0-7 low
    { 0x30E9, 0x00 }, // bit0-2(8-10)
};

const static I2C_ARRAY gain_HDR_DOL_SEF1_reg[] = {
    { 0x30EA, 0x00 }, // bit0-7 low
    { 0x30EB, 0x00 }, // bit0-2(8-10)
};

const static I2C_ARRAY expo_shr_dol1_reg[] = {
    // SEL
    { 0x305e, 0x00 }, // bit0-3(16-18)
    { 0x305d, 0x00 }, // bit0-7(8-15)
    { 0x305c, 0x12 }, // bit0-7
};

const I2C_ARRAY expo_rhs1_reg[] = {
    // SEL
    { 0x306a, 0x00 }, // bit0-3(16-18)
    { 0x3069, 0x00 }, // bit0-7(8-15)
    { 0x3068, 0xCE }, // bit0-7
};

const static I2C_ARRAY mirr_flip_table[] = {
    { 0x304e, 0x00 }, // M0F0
    { 0x304f, 0x00 }, // M0F0
    { 0x3081, 0x02 },
    { 0x3083, 0x02 },
    { 0x30b6, 0x00 },
    { 0x30b7, 0x00 },
    { 0x3016, 0x08 },

    { 0x304e, 0x01 }, // M1F0
    { 0x304f, 0x00 }, // M1F0
    { 0x3081, 0x02 },
    { 0x3083, 0x02 },
    { 0x30b6, 0x00 },
    { 0x30b7, 0x00 },
    { 0x3016, 0x08 },

    { 0x304e, 0x00 }, // M0F1
    { 0x304f, 0x01 }, // M0F1
    { 0x3081, 0xfe },
    { 0x3083, 0xfe },
    { 0x30b6, 0xfa },
    { 0x30b7, 0x01 },
    { 0x3016, 0x02 },

    { 0x304e, 0x01 }, // M1F1
    { 0x304f, 0x01 }, // M1F1
    { 0x3081, 0xfe },
    { 0x3083, 0xfe },
    { 0x30b6, 0xfa },
    { 0x30b7, 0x01 },
    { 0x3016, 0x02 },
};

const static I2C_ARRAY gain_reg[] = {
    { 0x30E8, 0x00 }, // bit0-7 low
    { 0x30E9, 0x00 }, // bit0-2(8-10)
};

const static I2C_ARRAY expo_reg[] = {
    { 0x305a, 0x00 }, // bit0-3(16-18)
    { 0x3059, 0x00 }, // bit0-7(8-15)
    { 0x3058, 0x09 }, // bit0-7
};

const static I2C_ARRAY vts_reg[] = {
    { 0x3032, 0x00 }, // bit0-3(16-18)
    { 0x3031, 0x11 }, // bit0-7(8-15)
    { 0x3030, 0x94 }, // bit0-7
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

/////////// function definition ///////////////////
#define SensorReg_Read(_reg, _data) (handle->i2c_bus->i2c_rx(handle->i2c_bus, &(handle->i2c_cfg), _reg, _data))
#define SensorReg_Write(_reg, _data) (handle->i2c_bus->i2c_tx(handle->i2c_bus, &(handle->i2c_cfg), _reg, _data))
#define SensorRegArrayW(_reg, _len) (handle->i2c_bus->i2c_array_tx(handle->i2c_bus, &(handle->i2c_cfg), (_reg), (_len)))
#define SensorRegArrayR(_reg, _len) (handle->i2c_bus->i2c_array_rx(handle->i2c_bus, &(handle->i2c_cfg), (_reg), (_len)))

/////////////////// sensor hardware dependent //////////////
static int cus_camsensor_release_handle(ms_cus_sensor* handle)
{
    return SUCCESS;
}

/*******I5/I6 Support MCLK List*******
 *    CUS_CMU_CLK_27MHZ,
 *    CUS_CMU_CLK_21P6MHZ,
 *    CUS_CMU_CLK_12MHZ,
 *    CUS_CMU_CLK_5P4MHZ,
 *    CUS_CMU_CLK_36MHZ,
 *    CUS_CMU_CLK_54MHZ,
 *    CUS_CMU_CLK_43P2MHZ,
 *    CUS_CMU_CLK_61P7MHZ,
 *    CUS_CMU_CLK_72MHZ,
 *    CUS_CMU_CLK_48MHZ,
 *    CUS_CMU_CLK_24MHZ,
 *    CUS_CMU_CLK_37P125MHZ,
 ******End of Support MCLK List*******/
#if 0
static CUS_MCLK_FREQ UseParaMclk(const char *mclk)
{
    if (strcmp(mclk, "27M") == 0) {
        return CUS_CMU_CLK_27MHZ;
    } else if (strcmp(mclk, "12M") == 0) {
        return CUS_CMU_CLK_12MHZ;
    } else if (strcmp(mclk, "36M") == 0) {
        return CUS_CMU_CLK_36MHZ;
    } else if (strcmp(mclk, "48M") == 0) {
        return CUS_CMU_CLK_48MHZ;
    } else if (strcmp(mclk, "54M") == 0) {
        return CUS_CMU_CLK_54MHZ;
    } else if (strcmp(mclk, "24M") == 0) {
        return CUS_CMU_CLK_24MHZ;
    } else if (strcmp(mclk, "37.125M") == 0) {
        return CUS_CMU_CLK_37P125MHZ;
    }
    return Preview_MCLK_SPEED;
}
#endif

static int pCus_poweron(ms_cus_sensor* handle, u32 idx)
{
    ISensorIfAPI* sensor_if = handle->sensor_if_api;
    SENSOR_DMSG("[%s] ", __FUNCTION__);

    // Sensor power on sequence
    sensor_if->PowerOff(idx, !handle->pwdn_POLARITY);
    sensor_if->Reset(idx, !handle->reset_POLARITY);
    sensor_if->SetIOPad(idx, handle->sif_bus, handle->interface_attr.attr_mipi.mipi_lane_num);
    sensor_if->SetCSI_Clk(idx, CUS_CSI_CLK_216M);
    sensor_if->SetCSI_Lane(idx, handle->interface_attr.attr_mipi.mipi_lane_num, 1);
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
    SENSOR_DMSG("Sensor Power On finished\n");
    return SUCCESS;
}

static int pCus_poweroff(ms_cus_sensor* handle, u32 idx)
{
    // power/reset low
    ISensorIfAPI* sensor_if = handle->sensor_if_api;

    SENSOR_DMSG("[%s] reset low\n", __FUNCTION__);
    sensor_if->Reset(idx, handle->reset_POLARITY);
    sensor_if->MCLK(idx, 0, handle->mclk);

    sensor_if->SetCSI_Clk(idx, CUS_CSI_CLK_DISABLE);
    if (handle->interface_attr.attr_mipi.mipi_hdr_mode == CUS_HDR_MODE_SONY_DOL) {
        sensor_if->SetCSI_hdr_mode(idx, handle->interface_attr.attr_mipi.mipi_hdr_mode, 0);
    }
    handle->orient = SENSOR_ORIT;

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

    for (n = 0; n < 4; ++n) // retry , until I2C success
    {
        if (n > 2)
            return FAIL;

        if (/* SensorRegArrayR((I2C_ARRAY*)id_from_sensor,table_length) == */ SUCCESS) // read sensor ID from I2C
            break;
        else
            SENSOR_MSLEEP(1);
    }

    // convert sensor id to u32 format
    for (i = 0; i < table_length; ++i) {
        if (id_from_sensor[i].data != Sensor_id_table[i].data)
            return FAIL;
        *id = id_from_sensor[i].data;
    }

    SENSOR_DMSG("[%s]IMX335 sensor ,Read sensor id, get 0x%x Success\n", __FUNCTION__, (int)*id);
    return SUCCESS;
}

static int imx335_SetPatternMode(ms_cus_sensor* handle, u32 mode)
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
            // MSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
            return FAIL;
        }
    }
    return SUCCESS;
}

/* ── Window-crop geometry (Star6E IMX335 extra tiers) ───────────────────
 * Each crop reuses the proven 5m120fps analog/PLL base and overrides only
 * the readout window + HMAX (line time).  The 120fps stock table IS the
 * geometry formula for W=1920 — verified byte-exact — so the same formula
 * generates every crop:
 *
 *   HTRIM_start = 48 + (2592 - W) / 2    (0x302C/2D)
 *   HNUM        = W + 24                  (0x302E/2F)
 *   Y_OUT_SIZE  = H                       (0x3056/57)
 *   AREA3_start = 176 + (1944 - H)        (0x3074/75)
 *   AREA3_width = 2 * H                   (0x3076/77)
 *   HMAX        = empirical line time     (0x3034/35)   device-tuned on .13
 *
 * 0x3018=0x04 (window mode) is inherited from the reused 120fps base.
 * VMAX is NOT written here — it is applied at runtime via vts_30fps in
 * pCus_SetVideoRes, so one geometry table serves all pacings of the mode. */
/* PROBE candidates (2026-07-05): higher-FOV crops for the 100/144 tiers.
 * All use HMAX=275 (proven for readouts up to ~2584 wide by the stock 90fps
 * mode); the I6E ISP bandwidth ceiling is what these probe.  Geometry from
 * the same formula as the 120fps base. */
static const I2C_ARRAY imx335_geo_2176x1224[] = { // 100fps aggressive, 2.66 MPix
    { 0x302C, 0x00 }, { 0x302D, 0x01 }, // HTRIMMING_START = 256
    { 0x302E, 0x98 }, { 0x302F, 0x08 }, // HNUM = 2200
    { 0x3056, 0xC8 }, { 0x3057, 0x04 }, // Y_OUT_SIZE = 1224
    { 0x3074, 0x80 }, { 0x3075, 0x03 }, // AREA3_ST_Addr = 896
    { 0x3076, 0x90 }, { 0x3077, 0x09 }, // AREA3_WIDTH = 2448
    { 0x3034, 0x13 }, { 0x3035, 0x01 }, // HMAX = 275
};

static const I2C_ARRAY imx335_geo_2048x1152[] = { // 100fps moderate, 2.36 MPix
    { 0x302C, 0x40 }, { 0x302D, 0x01 }, // HTRIMMING_START = 320
    { 0x302E, 0x18 }, { 0x302F, 0x08 }, // HNUM = 2072
    { 0x3056, 0x80 }, { 0x3057, 0x04 }, // Y_OUT_SIZE = 1152
    { 0x3074, 0xC8 }, { 0x3075, 0x03 }, // AREA3_ST_Addr = 968
    { 0x3076, 0x00 }, { 0x3077, 0x09 }, // AREA3_WIDTH = 2304
    { 0x3034, 0x13 }, { 0x3035, 0x01 }, // HMAX = 275
};

static const I2C_ARRAY imx335_geo_1664x936[] = { // 144fps aggressive, 1.56 MPix
    { 0x302C, 0x00 }, { 0x302D, 0x02 }, // HTRIMMING_START = 512
    { 0x302E, 0x98 }, { 0x302F, 0x06 }, // HNUM = 1688
    { 0x3056, 0xA8 }, { 0x3057, 0x03 }, // Y_OUT_SIZE = 936
    { 0x3074, 0xA0 }, { 0x3075, 0x04 }, // AREA3_ST_Addr = 1184
    { 0x3076, 0x50 }, { 0x3077, 0x07 }, // AREA3_WIDTH = 1872
    { 0x3034, 0x13 }, { 0x3035, 0x01 }, // HMAX = 275
};

static const I2C_ARRAY imx335_geo_1600x900[] = { // 144fps moderate, 1.44 MPix
    { 0x302C, 0x20 }, { 0x302D, 0x02 }, // HTRIMMING_START = 544
    { 0x302E, 0x58 }, { 0x302F, 0x06 }, // HNUM = 1624
    { 0x3056, 0x84 }, { 0x3057, 0x03 }, // Y_OUT_SIZE = 900
    { 0x3074, 0xC4 }, { 0x3075, 0x04 }, // AREA3_ST_Addr = 1220
    { 0x3076, 0x08 }, { 0x3077, 0x07 }, // AREA3_WIDTH = 1800
    { 0x3034, 0x13 }, { 0x3035, 0x01 }, // HMAX = 275
};

/* Write the 5m120fps base minus its final standby-exit pair
 * ({0x3000,0x00},{0x3002,0x00}), then the crop geometry override while
 * still in standby, then re-issue the held-back standby-exit.  This latches
 * the window before streaming so a warm mode switch never inherits stale
 * geometry (PR#156 explicit-geometry discipline). */
static int imx335_init_window_crop(ms_cus_sensor* handle,
    const I2C_ARRAY* geo, int geo_len)
{
    int i, cnt;
    int base_len = ARRAY_SIZE(Sensor_init_table_4lane_5m120fps);

    (void)handle;
    SENSOR_DMSG("\n[%s]", __FUNCTION__);

    /* 1. base analog/PLL table, holding back the last two (standby-exit) */
    for (i = 0; i < base_len - 2; i++) {
        if (Sensor_init_table_4lane_5m120fps[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_init_table_4lane_5m120fps[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_init_table_4lane_5m120fps[i].reg,
                       Sensor_init_table_4lane_5m120fps[i].data) != SUCCESS) {
                if (++cnt >= 10)
                    return FAIL;
            }
        }
    }

    /* 2. crop geometry override, still in standby */
    for (i = 0; i < geo_len; i++) {
        cnt = 0;
        while (SensorReg_Write(geo[i].reg, geo[i].data) != SUCCESS) {
            if (++cnt >= 10)
                return FAIL;
        }
    }

    /* 3. re-issue the held-back standby-exit ({0x3000,0x00},{0x3002,0x00}) */
    for (i = base_len - 2; i < base_len; i++) {
        cnt = 0;
        while (SensorReg_Write(Sensor_init_table_4lane_5m120fps[i].reg,
                   Sensor_init_table_4lane_5m120fps[i].data) != SUCCESS) {
            if (++cnt >= 10)
                return FAIL;
        }
    }
    return SUCCESS;
}

static int pCus_init_window_2176x1224(ms_cus_sensor* handle)
{
    return imx335_init_window_crop(handle, imx335_geo_2176x1224,
        ARRAY_SIZE(imx335_geo_2176x1224));
}

static int pCus_init_window_2048x1152(ms_cus_sensor* handle)
{
    return imx335_init_window_crop(handle, imx335_geo_2048x1152,
        ARRAY_SIZE(imx335_geo_2048x1152));
}

static int pCus_init_window_1664x936(ms_cus_sensor* handle)
{
    return imx335_init_window_crop(handle, imx335_geo_1664x936,
        ARRAY_SIZE(imx335_geo_1664x936));
}

static int pCus_init_window_1600x900(ms_cus_sensor* handle)
{
    return imx335_init_window_crop(handle, imx335_geo_1600x900,
        ARRAY_SIZE(imx335_geo_1600x900));
}

static int pCus_init_mipi4lane_5m30fps_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    SENSOR_DMSG("\n[%s]", __FUNCTION__);

    for (i = 0; i < ARRAY_SIZE(Sensor_init_table_4lane_5m30fps); i++) {
        if (Sensor_init_table_4lane_5m30fps[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_init_table_4lane_5m30fps[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_init_table_4lane_5m30fps[i].reg, Sensor_init_table_4lane_5m30fps[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    return FAIL;
                }
                // usleep(10*1000);
            }
        }
    }
    return SUCCESS;
}

static int pCus_init_mipi4lane_5m60fps_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    SENSOR_DMSG("\n[%s]", __FUNCTION__);

    for (i = 0; i < ARRAY_SIZE(Sensor_init_table_4lane_5m60fps); i++) {
        if (Sensor_init_table_4lane_5m60fps[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_init_table_4lane_5m60fps[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_init_table_4lane_5m60fps[i].reg, Sensor_init_table_4lane_5m60fps[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    return FAIL;
                }
                // usleep(10*1000);
            }
        }
    }
    return SUCCESS;
}

static int pCus_init_mipi4lane_5m90fps_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    SENSOR_DMSG("\n[%s]", __FUNCTION__);

    for (i = 0; i < ARRAY_SIZE(Sensor_init_table_4lane_5m90fps); i++) {
        if (Sensor_init_table_4lane_5m90fps[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_init_table_4lane_5m90fps[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_init_table_4lane_5m90fps[i].reg, Sensor_init_table_4lane_5m90fps[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    return FAIL;
                }
                // usleep(10*1000);
            }
        }
    }
    return SUCCESS;
}

static int pCus_init_mipi4lane_5m120fps_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    SENSOR_DMSG("\n[%s]", __FUNCTION__);

    for (i = 0; i < ARRAY_SIZE(Sensor_init_table_4lane_5m120fps); i++) {
        if (Sensor_init_table_4lane_5m120fps[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_init_table_4lane_5m120fps[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_init_table_4lane_5m120fps[i].reg, Sensor_init_table_4lane_5m120fps[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    return FAIL;
                }
                // usleep(10*1000);
            }
        }
    }
    return SUCCESS;
}

static int pCus_init_mipi4lane5m20fps_HDR_DOL(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    SENSOR_DMSG("\n[%s]", __FUNCTION__);

    for (i = 0; i < ARRAY_SIZE(Sensor_init_table_HDR_DOL_4lane5m20fps); i++) {
        if (Sensor_init_table_HDR_DOL_4lane5m20fps[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_init_table_HDR_DOL_4lane5m20fps[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_init_table_HDR_DOL_4lane5m20fps[i].reg, Sensor_init_table_HDR_DOL_4lane5m20fps[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    return FAIL;
                }
                // usleep(10*1000);
            }
        }
    }
    return SUCCESS;
}

static int pCus_init_mipi4lane5m25fps_HDR_DOL(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    SENSOR_DMSG("\n[%s]", __FUNCTION__);

    for (i = 0; i < ARRAY_SIZE(Sensor_init_table_HDR_DOL_4lane5m25fps); i++) {
        if (Sensor_init_table_HDR_DOL_4lane5m25fps[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_init_table_HDR_DOL_4lane5m25fps[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_init_table_HDR_DOL_4lane5m25fps[i].reg, Sensor_init_table_HDR_DOL_4lane5m25fps[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    return FAIL;
                }
                // usleep(10*1000);
            }
        }
    }
    return SUCCESS;
}

static int pCus_init_mipi4lane5m30fps_HDR_DOL(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    SENSOR_DMSG("\n[%s]", __FUNCTION__);

    for (i = 0; i < ARRAY_SIZE(Sensor_init_table_HDR_DOL_4lane5m30fps); i++) {
        if (Sensor_init_table_HDR_DOL_4lane5m30fps[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_init_table_HDR_DOL_4lane5m30fps[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_init_table_HDR_DOL_4lane5m30fps[i].reg, Sensor_init_table_HDR_DOL_4lane5m30fps[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    return FAIL;
                }
                // usleep(10*1000);
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
        return FAIL;
    }

    handle->video_res_supported.ulcur_res = res_idx;

    switch (res_idx) {
    case 0:
        handle->video_res_supported.ulcur_res = 0;
        handle->pCus_sensor_init = pCus_init_mipi4lane_5m30fps_linear;
        vts_30fps = 4125;
        Preview_MAX_FPS = 30;
        Preview_line_period = 8080;
        break;

    case 1:
        handle->video_res_supported.ulcur_res = 1;
        handle->pCus_sensor_init = pCus_init_mipi4lane_5m60fps_linear;
        vts_30fps = 3936;
        Preview_MAX_FPS = 60;
        Preview_line_period = 4234;
        break;

    case 2:
        handle->video_res_supported.ulcur_res = 2;
        handle->pCus_sensor_init = pCus_init_mipi4lane_5m90fps_linear;
        /* Stock 3016 delivered 89.56 (real pixel clock ~0.5% below
         * nominal); 3001 lands 90.0 (device-verified method, mode 5). */
        vts_30fps = 3001;
        Preview_MAX_FPS = 90;
        Preview_line_period = 3684;
        break;

    /* idx 3: 2176x1224 @ 100fps — Maruko-style window crop.
     * vts (VMAX at max fps) = K / (HMAX * max_fps), K = 74,448,000
     * (275*2256*120).  HMAX=275 -> line_period 3694ns; 100fps -> vts 2707. */
    case 3:
        handle->video_res_supported.ulcur_res = 3;
        handle->pCus_sensor_init = pCus_init_window_2176x1224;
        /* Nominal 2707 delivered 99.80; 2701 lands 100.0. */
        vts_30fps = 2701;
        Preview_MAX_FPS = 100;
        Preview_line_period = 3694;
        break;

    case 4:
        handle->video_res_supported.ulcur_res = 4;
        handle->pCus_sensor_init = pCus_init_mipi4lane_5m120fps_linear;
        /* Stock 2256 delivered 119.72; 2250 lands 120.0. */
        vts_30fps = 2250;
        Preview_MAX_FPS = 120;
        Preview_line_period = 3694;
        break;

    /* idx 5: 1600x900 @ 144fps — window crop; HMAX=275 -> vts 1880. */
    case 5:
        handle->video_res_supported.ulcur_res = 5;
        handle->pCus_sensor_init = pCus_init_window_1600x900;
        /* Nominal K/(275*144) = 1880, but the real pixel clock runs
         * ~0.2% below K: measured 143.7fps delivered at vts 1880.
         * 1875 lands the delivered rate at 144.0 (device-verified). */
        vts_30fps = 1875;
        Preview_MAX_FPS = 144;
        Preview_line_period = 3694;
        break;

    default:
        break;
    }

    params->expo.vts = vts_30fps;
    params->expo.fps = Preview_MAX_FPS;

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

    return SUCCESS;
}

static int pCus_SetVideoRes_HDR_DOL(ms_cus_sensor* handle, u32 res_idx)
{
    imx335_params* params = (imx335_params*)handle->private_data;
    // ISensorIfAPI *sensor_if = &handle->sensor_if_api;
    u32 num_res = handle->video_res_supported.num_res;
    if (res_idx >= num_res) {
        return FAIL;
    }
    switch (res_idx) {

    case 0:
        handle->video_res_supported.ulcur_res = 0;
        handle->pCus_sensor_init = pCus_init_mipi4lane5m25fps_HDR_DOL;
        vts_30fps_HDR_DOL = 4250;
        params->expo.vts = vts_30fps_HDR_DOL;
        Preview_MAX_FPS = 25;
        params->expo.fps = Preview_MAX_FPS;
        Preview_line_period_HDR_DOL = 9411; // 8889
        params->max_rhs1 = 290;
        break;

    case 1:
        handle->video_res_supported.ulcur_res = 1;
        handle->pCus_sensor_init = pCus_init_mipi4lane5m20fps_HDR_DOL;
        vts_30fps_HDR_DOL = 5312;
        params->expo.vts = vts_30fps_HDR_DOL;
        Preview_MAX_FPS = 20;
        params->expo.fps = Preview_MAX_FPS;
        Preview_line_period_HDR_DOL = 9411;
        params->max_rhs1 = 290;
        break;

    case 2:
        handle->video_res_supported.ulcur_res = 2;
        handle->pCus_sensor_init = pCus_init_mipi4lane5m30fps_HDR_DOL;
        vts_30fps_HDR_DOL = 4506;
        params->expo.vts = vts_30fps_HDR_DOL;
        Preview_MAX_FPS = 30;
        params->expo.fps = Preview_MAX_FPS;
        Preview_line_period_HDR_DOL = 7398;
        params->max_rhs1 = 290;
        break;

    default:
        break;
    }

    return SUCCESS;
}

static int pCus_GetOrien(ms_cus_sensor* handle, CUS_CAMSENSOR_ORIT* orit)
{
    short Horiz_Inv = 0;
    short Verti_Inv = 0;
    short Orien_Mode = 0;
    SensorReg_Read(0x304e, &Horiz_Inv);
    SensorReg_Read(0x304f, &Verti_Inv);
    Horiz_Inv &= 0x01;
    Verti_Inv &= 0X01;
    Orien_Mode = Horiz_Inv | (Verti_Inv << 2);
    switch (Orien_Mode) {
    case 0x00:
        *orit = CUS_ORIT_M0F0;
        break;
    case 0x01:
        *orit = CUS_ORIT_M1F0;
        break;
    case 0x02:
        *orit = CUS_ORIT_M0F1;
        break;
    case 0x03:
        *orit = CUS_ORIT_M1F1;
        break;
    }
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
    int i, j;

    switch (orit) {
    case CUS_ORIT_M0F0:
        handle->orient = CUS_ORIT_M0F0;
        for (i = 0, j = 0; i < seg_length; i++, j++) {
            SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
        }
        break;
    case CUS_ORIT_M1F0:
        for (i = seg_length, j = 0; i < seg_length * 2; i++, j++) {
            handle->orient = CUS_ORIT_M1F0;
            SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
        }
        break;
    case CUS_ORIT_M0F1:
        for (i = seg_length * 2, j = 0; i < seg_length * 3; i++, j++) {
            handle->orient = CUS_ORIT_M0F1;
            if (handle->video_res_supported.ulcur_res == 0) {
                SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
            } else if (handle->video_res_supported.ulcur_res == 1) {
                SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
                SensorReg_Write(0x3074, 0xF8); // AREA3_ST_ADR 4088
                SensorReg_Write(0x3075, 0x0F);
                SensorReg_Write(0x30C6, 0x00); // BLACK_OFSET_AD
                SensorReg_Write(0x30CE, 0x00); // UNRD_LINE_MAX
                SensorReg_Write(0x30D8, 0x4C); // UNREAD_ED_ADR
                SensorReg_Write(0x30D9, 0x10);
            } else if (handle->video_res_supported.ulcur_res == 2) {
                SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
                SensorReg_Write(0x3074, 0x18); // AREA3_ST_ADR 3606
                SensorReg_Write(0x3075, 0x0E);
                SensorReg_Write(0x30C6, 0x12); // BLACK_OFSET_AD
                SensorReg_Write(0x30CE, 0x64); // UNRD_LINE_MAX
                SensorReg_Write(0x30D8, 0x4C); // UNREAD_ED_ADR
                SensorReg_Write(0x30D9, 0x10);
            } else if (handle->video_res_supported.ulcur_res == 3) {
                /* Flipped AREA3_ST = 4288 - normal ST (pattern verified
                 * against the three stock modes).  2176x1224 crop:
                 * 4288 - 896 = 3392.  The inherited 3248 here was the
                 * STOCK driver's res-3 (1920x1080) value. */
                SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
                SensorReg_Write(0x3074, 0x40); // AREA3_ST_ADR 3392
                SensorReg_Write(0x3075, 0x0D);
                SensorReg_Write(0x30C6, 0x12); // BLACK_OFSET_AD
                SensorReg_Write(0x30CE, 0x64); // UNRD_LINE_MAX
                SensorReg_Write(0x30D8, 0x4C); // UNREAD_ED_ADR
                SensorReg_Write(0x30D9, 0x10);
            } else if (handle->video_res_supported.ulcur_res == 4) {
                /* 1920x1080: 4288 - 1040 = 3248 (stock res-3 values). */
                SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
                SensorReg_Write(0x3074, 0xB0); // AREA3_ST_ADR 3248
                SensorReg_Write(0x3075, 0x0C);
                SensorReg_Write(0x30C6, 0x12); // BLACK_OFSET_AD
                SensorReg_Write(0x30CE, 0x64); // UNRD_LINE_MAX
                SensorReg_Write(0x30D8, 0x4C); // UNREAD_ED_ADR
                SensorReg_Write(0x30D9, 0x10);
            } else if (handle->video_res_supported.ulcur_res == 5) {
                /* 1600x900 crop: 4288 - 1220 = 3068. */
                SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
                SensorReg_Write(0x3074, 0xFC); // AREA3_ST_ADR 3068
                SensorReg_Write(0x3075, 0x0B);
                SensorReg_Write(0x30C6, 0x12); // BLACK_OFSET_AD
                SensorReg_Write(0x30CE, 0x64); // UNRD_LINE_MAX
                SensorReg_Write(0x30D8, 0x4C); // UNREAD_ED_ADR
                SensorReg_Write(0x30D9, 0x10);
            }
        }
        break;
    case CUS_ORIT_M1F1:
        for (i = seg_length * 3, j = 0; i < seg_length * 4; i++, j++) {
            handle->orient = CUS_ORIT_M1F1;
            if (handle->video_res_supported.ulcur_res == 0) {
                SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
            } else if (handle->video_res_supported.ulcur_res == 1) {
                SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
                SensorReg_Write(0x3074, 0xF8); // AREA3_ST_ADR 4088
                SensorReg_Write(0x3075, 0x0F);
                SensorReg_Write(0x30C6, 0x00); // BLACK_OFSET_AD
                SensorReg_Write(0x30CE, 0x00); // UNRD_LINE_MAX
                SensorReg_Write(0x30D8, 0x4C); // UNREAD_ED_ADR
                SensorReg_Write(0x30D9, 0x10);
            } else if (handle->video_res_supported.ulcur_res == 2) {
                SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
                SensorReg_Write(0x3074, 0x18); // AREA3_ST_ADR 3608
                SensorReg_Write(0x3075, 0x0E);
                SensorReg_Write(0x30C6, 0x12); // BLACK_OFSET_AD
                SensorReg_Write(0x30CE, 0x64); // UNRD_LINE_MAX
                SensorReg_Write(0x30D8, 0x4C); // UNREAD_ED_ADR
                SensorReg_Write(0x30D9, 0x10);
            } else if (handle->video_res_supported.ulcur_res == 3) {
                /* See M0F1 case: flipped AREA3_ST = 4288 - normal ST. */
                SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
                SensorReg_Write(0x3074, 0x40); // AREA3_ST_ADR 3392
                SensorReg_Write(0x3075, 0x0D);
                SensorReg_Write(0x30C6, 0x12); // BLACK_OFSET_AD
                SensorReg_Write(0x30CE, 0x64); // UNRD_LINE_MAX
                SensorReg_Write(0x30D8, 0x4C); // UNREAD_ED_ADR
                SensorReg_Write(0x30D9, 0x10);
            } else if (handle->video_res_supported.ulcur_res == 4) {
                SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
                SensorReg_Write(0x3074, 0xB0); // AREA3_ST_ADR 3248
                SensorReg_Write(0x3075, 0x0C);
                SensorReg_Write(0x30C6, 0x12); // BLACK_OFSET_AD
                SensorReg_Write(0x30CE, 0x64); // UNRD_LINE_MAX
                SensorReg_Write(0x30D8, 0x4C); // UNREAD_ED_ADR
                SensorReg_Write(0x30D9, 0x10);
            } else if (handle->video_res_supported.ulcur_res == 5) {
                SensorReg_Write(mirr_flip_table[i].reg, mirr_flip_table[i].data);
                SensorReg_Write(0x3074, 0xFC); // AREA3_ST_ADR 3068
                SensorReg_Write(0x3075, 0x0B);
                SensorReg_Write(0x30C6, 0x12); // BLACK_OFSET_AD
                SensorReg_Write(0x30CE, 0x64); // UNRD_LINE_MAX
                SensorReg_Write(0x30D8, 0x4C); // UNREAD_ED_ADR
                SensorReg_Write(0x30D9, 0x10);
            }
        }
        break;
    default:
        handle->orient = CUS_ORIT_M0F0;
        break;
    }
    return SUCCESS;
}

static int pCus_GetFPS(ms_cus_sensor* handle)
{
    imx335_params* params = (imx335_params*)handle->private_data;
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
    imx335_params* params = (imx335_params*)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 min_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].min_fps;

    SENSOR_DMSG("\n\n[%s]", __FUNCTION__);

    if (fps >= min_fps && fps <= max_fps) {
        params->expo.fps = fps;
        params->expo.vts = (vts_30fps * (max_fps * 1000) + fps * 500) / (fps * 1000);
    } else if ((fps >= (min_fps * 1000)) && (fps <= (max_fps * 1000))) {
        params->expo.fps = fps;
        params->expo.vts = (vts_30fps * (max_fps * 1000) + (fps >> 1)) / fps;
    } else {
        // params->expo.vts=vts_30fps;
        // params->expo.fps=25;
        SENSOR_DMSG("[%s] FPS %d out of range.\n", __FUNCTION__, fps);
        return FAIL;
    }

    /* Fixed-framerate policy: keep the computed vts; pCus_SetAEUSecs()
     * clamps the exposure to the frame budget instead of the stock
     * vts = expo_lines + 8 extension (which held VMAX at 1887 in mode 5
     * whenever AE sat at the 1/144s ceiling -> 143.4fps, never 144.0). */
    pCus_SetAEUSecs(handle, params->expo.expo_lef_us);

    params->dirty = true;
    return SUCCESS;
}

static int pCus_GetFPS_HDR_DOL_SEF1(ms_cus_sensor* handle)
{
    imx335_params* params = (imx335_params*)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 tVts = (params->tVts_reg[0].data << 16) | (params->tVts_reg[1].data << 8) | (params->tVts_reg[2].data << 0);

    if (params->expo.fps >= 1000)
        params->expo.preview_fps = (vts_30fps_HDR_DOL * max_fps * 1000) / tVts;
    else
        params->expo.preview_fps = (vts_30fps_HDR_DOL * max_fps) / tVts;

    return params->expo.preview_fps;
}

static int pCus_SetFPS_HDR_DOL_SEF1(ms_cus_sensor* handle, u32 fps)
{
    imx335_params* params = (imx335_params*)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 min_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].min_fps;

    if (fps >= min_fps && fps <= max_fps) {
        params->expo.fps = fps;
        params->expo.vts = (vts_30fps_HDR_DOL * (max_fps * 1000) + fps * 500) / (fps * 1000);
    } else if ((fps >= (min_fps * 1000)) && (fps <= (max_fps * 1000))) {
        params->expo.fps = fps;
        params->expo.vts = (vts_30fps_HDR_DOL * (max_fps * 1000) + (fps >> 1)) / fps;
    } else {
        // params->expo.vts=vts_30fps;
        // params->expo.fps=30;
        SENSOR_DMSG("[%s] FPS %d out of range.\n", __FUNCTION__, fps);
        return FAIL;
    }
    params->dirty = true; // reg need to update = true;
    pCus_SetAEUSecsHDR_DOL_SEF1(handle, params->expo.expo_sef_us);

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
    imx335_params* params = (imx335_params*)handle->private_data;

    switch (status) {
    case CUS_FRAME_INACTIVE:
        // SensorReg_Write(0x3001,0);
        break;
    case CUS_FRAME_ACTIVE:
        if (params->dirty || params->orien_dirty) {
            SensorReg_Write(0x3001, 1);
            SensorRegArrayW((I2C_ARRAY*)params->tExpo_reg, ARRAY_SIZE(expo_reg));
            SensorRegArrayW((I2C_ARRAY*)params->tGain_reg, ARRAY_SIZE(gain_reg));
            SensorRegArrayW((I2C_ARRAY*)params->tVts_reg, ARRAY_SIZE(vts_reg));

            if (params->orien_dirty) {
                DoOrien(handle, handle->orient);
                params->orien_dirty = false;
            }
            SensorReg_Write(0x3001, 0);
            params->dirty = false;
        }
        break;
    default:
        break;
    }
    return SUCCESS;
}

static int pCus_AEStatusNotifyHDR_DOL_SEF1(ms_cus_sensor* handle, CUS_CAMSENSOR_AE_STATUS_NOTIFY status)
{
    // imx335_params *params = (imx335_params *)handle->private_data;
    switch (status) {
    case CUS_FRAME_INACTIVE:
        break;
    case CUS_FRAME_ACTIVE:
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
    /* Fixed-framerate policy: clamp exposure to the frame budget (SHR
     * floor = 9 blanking lines) instead of extending VMAX.  The stock
     * vts = lines + 1 extension makes delivered fps sag with scene
     * brightness (AE pinned at 1/fps stretches every frame). */
    if (lines > params->expo.vts - 9)
        lines = params->expo.vts - 9;
    params->expo.expo_lines = lines;

    vts = params->expo.vts;

    SENSOR_DMSG("[%s] us %u, lines %u, vts %u\n", __FUNCTION__,
        us,
        lines,
        params->expo.vts);

    activeline = vts - lines;
    if (activeline < 9)
        activeline = 9;

    params->tExpo_reg[0].data = (activeline >> 16) & 0x000f;
    params->tExpo_reg[1].data = (activeline >> 8) & 0x00ff;
    params->tExpo_reg[2].data = (activeline >> 0) & 0x00ff;

    params->tVts_reg[0].data = (vts >> 16) & 0x000f;
    params->tVts_reg[1].data = (vts >> 8) & 0x00ff;
    params->tVts_reg[2].data = (vts >> 0) & 0x00ff;

    params->dirty = true;
    return SUCCESS;
}

static int pCus_SetAEUSecsHDR_DOL_SEF1(ms_cus_sensor* handle, u32 us)
{
    u32 qua_lines = 0, lines = 0, long_lines = 0, vts = 0, fsc = 0;
    u32 rhs1 = 0, shs1 = 0, shs0 = 0;
    imx335_params* params = (imx335_params*)handle->private_data;

    params->expo.expo_sef_us = us;
    qua_lines = (1000 * us) / Preview_line_period_HDR_DOL / 4;
    vts = params->expo.vts;
    shs0 = (params->tExpo_reg[0].data << 16) | (params->tExpo_reg[1].data << 8) | (params->tExpo_reg[2].data << 0);
    fsc = vts * 2;
    long_lines = fsc - shs0;
    params->expo.expo_lines = long_lines;

    // params->max_rhs1 = 290;
    rhs1 = params->max_rhs1; //(params->tExpo_rhs1_reg[0].data << 16) | (params->tExpo_rhs1_reg[1].data << 8) | (params->tExpo_rhs1_reg[2].data << 0);

    if (qua_lines <= 1)
        qua_lines = 1;
    if ((4 * qua_lines) > (rhs1 - 18))
        qua_lines = (rhs1 - 18) / 4;

    lines = 4 * qua_lines;
    if ((rhs1 - 18) <= lines) {
        shs1 = 18;
    } else if ((rhs1 <= params->max_rhs1) && (rhs1 <= shs0 - 18)) {
        shs1 = rhs1 - lines;
        if ((shs1 < 18) || (shs1 > (rhs1 - 4))) { // Check boundary
            // shs1 = 0;
            // UartSendTrace("[SEF1 NG1]");
        }
    } else {
        // UartSendTrace("[SEF1 NG2]");
    }

    params->tExpo_shr_dol1_reg[0].data = (shs1 >> 16) & 0x000f;
    params->tExpo_shr_dol1_reg[1].data = (shs1 >> 8) & 0x00ff;
    params->tExpo_shr_dol1_reg[2].data = (shs1 >> 0) & 0x00ff;
    params->tExpo_rhs1_reg[0].data = (rhs1 >> 16) & 0x000f;
    params->tExpo_rhs1_reg[1].data = (rhs1 >> 8) & 0x00ff;
    params->tExpo_rhs1_reg[2].data = (rhs1 >> 0) & 0x00ff;

    return SUCCESS;
}

// Gain: 1x = 1024
static int pCus_GetAEGain(ms_cus_sensor* handle, u32* gain)
{
    unsigned short temp_gain;
    //  *gain=params->expo.final_gain;
    temp_gain = gain_reg[0].data;

    *gain = (u32)(10 ^ ((temp_gain * 3) / 200)) * 1024;
    if (gain_reg[1].data & 0x10)
        *gain = (*gain) * 2;

    SENSOR_DMSG("[%s] get gain/reg (1024=1X)= %u/0x%x\n", __FUNCTION__, *gain, gain_reg[0].data);
    return SUCCESS;
}

static int pCus_SetAEGain_cal(ms_cus_sensor* handle, u32 gain)
{
    imx335_params* params = (imx335_params*)handle->private_data;
    // double gain_double;
    u64 gain_double;
    params->expo.final_gain = gain;

    if (gain < 1024)
        gain = 1024;
    else if (gain >= 3980 * 1024)
        gain = 3980 * 1024;

    gain_double = 20 * (intlog10(gain) - intlog10(1024));
    params->tGain_reg[0].data = (u16)(((gain_double * 10) >> 24) / 3);

    SENSOR_DMSG("[%s] set gain/reg=%u/0x%x\n", __FUNCTION__, gain, params->tGain_reg[0].data);

    params->dirty = true;
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
    params->tGain_reg[0].data = (u16)(((gain_double * 10) >> 24) / 3) & 0x00ff;
    params->tGain_reg[1].data = (u16)((((gain_double * 10) >> 24) / 3) >> 8) & 0x0007;

    SENSOR_DMSG("[%s] set gain/reg=%u/0x%x\n", __FUNCTION__, gain, params->tGain_reg[0].data);
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
    *gain_reg = (u16)(((gain_double * 10) >> 24) / 3) & 0x07ff;
}

static int pCus_SetAEGainHDR_DOL_SEF1(ms_cus_sensor* handle, u32 gain)
{
    imx335_params* params = (imx335_params*)handle->private_data;
    u16 gain_reg = 0;

    pCus_SetAEGainHDR_DOL_Calculate(gain, &gain_reg);
    params->tGain_hdr_dol_sef_reg[0].data = gain_reg & 0x00ff;
    params->tGain_hdr_dol_sef_reg[1].data = (gain_reg >> 8) & 0x0007;

    SENSOR_DMSG("[%s] set gain/reg=%u/0x%x\n", __FUNCTION__, gain, params->tGain_hdr_dol_sef_reg[0].data);

    params->dirty = true;
    return SUCCESS;
}

static int pCus_GetAEMinMaxUSecs(ms_cus_sensor* handle, u32* min, u32* max)
{
    *min = 1;
    *max = 1000000 / imx335_mipi_linear[0].senout.min_fps;
    return SUCCESS;
}

static int pCus_GetAEMinMaxGain(ms_cus_sensor* handle, u32* min, u32* max)
{
    *min = SENSOR_MIN_GAIN; // handle->sat_mingain;
    *max = SENSOR_MAX_GAIN; // 3980*1024;
    return SUCCESS;
}

static int IMX335_GetShutterInfo(struct __ms_cus_sensor* handle, CUS_SHUTTER_INFO* info)
{
    info->max = 1000000000 / imx335_mipi_linear[0].senout.min_fps;
    info->min = (Preview_line_period * 9);
    info->step = Preview_line_period;
    return SUCCESS;
}

static int pCus_setCaliData_gain_linearity(ms_cus_sensor* handle, CUS_GAIN_GAP_ARRAY* pArray, u32 num)
{

    return SUCCESS;
}

static int IMX335_GetShutterInfoHDR_DOL_SEF1(struct __ms_cus_sensor* handle, CUS_SHUTTER_INFO* info)
{
    imx335_params* params = (imx335_params*)handle->private_data;
    info->max = Preview_line_period_HDR_DOL * params->max_rhs1;
    info->min = (Preview_line_period_HDR_DOL * 4);
    info->step = Preview_line_period_HDR_DOL * 4;
    return SUCCESS;
}

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
    ////////////////////////////////////
    // private data allocation & init //
    ////////////////////////////////////
    if (handle->private_data == NULL) {
        SENSOR_EMSG("[%s] Private data is empty!\n", __FUNCTION__);
        return FAIL;
    }

    params = (imx335_params*)handle->private_data;
    memcpy(params->tGain_reg, gain_reg, sizeof(gain_reg));
    memcpy(params->tExpo_reg, expo_reg, sizeof(expo_reg));
    memcpy(params->tVts_reg, vts_reg, sizeof(vts_reg));
    // memcpy(params->tExpo_rhs1_reg, expo_rhs1_reg, sizeof(expo_rhs1_reg));
    // memcpy(params->tExpo_shr_dol1_reg, expo_shr_dol1_reg, sizeof(expo_shr_dol1_reg));
    // memcpy(params->tGain_hdr_dol_lef_reg, gain_HDR_DOL_LEF_reg, sizeof(gain_HDR_DOL_LEF_reg));
    // memcpy(params->tGain_hdr_dol_sef_reg, gain_HDR_DOL_SEF1_reg, sizeof(gain_HDR_DOL_SEF1_reg));

    ////////////////////////////////////
    //    sensor model ID             //
    ////////////////////////////////////
    sprintf(handle->model_id, "IMX335_MIPI");

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
    handle->mclk = Preview_MCLK_SPEED; // UseParaMclk(SENSOR_DRV_PARAM_MCLK());

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
        handle->video_res_supported.res[res].width = imx335_mipi_linear[res].senif.preview_w;
        handle->video_res_supported.res[res].height = imx335_mipi_linear[res].senif.preview_h;
        handle->video_res_supported.res[res].max_fps = imx335_mipi_linear[res].senout.max_fps;
        handle->video_res_supported.res[res].min_fps = imx335_mipi_linear[res].senout.min_fps;
        handle->video_res_supported.res[res].crop_start_x = imx335_mipi_linear[res].senif.crop_start_X;
        handle->video_res_supported.res[res].crop_start_y = imx335_mipi_linear[res].senif.crop_start_y;
        handle->video_res_supported.res[res].nOutputWidth = imx335_mipi_linear[res].senout.width;
        handle->video_res_supported.res[res].nOutputHeight = imx335_mipi_linear[res].senout.height;
        sprintf(handle->video_res_supported.res[res].strResDesc, imx335_mipi_linear[res].senstr.strResDesc);
    }

    ////////////////////////////////////
    //    Sensor polarity             //
    ////////////////////////////////////
    handle->pwdn_POLARITY = SENSOR_PWDN_POL; // CUS_CLK_POL_NEG;
    handle->reset_POLARITY = SENSOR_RST_POL; // CUS_CLK_POL_NEG;
    // handle->VSYNC_POLARITY              = SENSOR_VSYNC_POL; //CUS_CLK_POL_POS;
    // handle->HSYNC_POLARITY              = SENSOR_HSYNC_POL; //CUS_CLK_POL_POS;
    handle->PCLK_POLARITY = SENSOR_PCLK_POL; // CUS_CLK_POL_POS);    // use '!' to clear board latch error

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
    handle->sat_mingain = SENSOR_MIN_GAIN; // calibration
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
    handle->pCus_sensor_SetAEGain_cal = pCus_SetAEGain_cal;
    handle->pCus_sensor_setCaliData_gain_linearity = pCus_setCaliData_gain_linearity;
    handle->pCus_sensor_GetShutterInfo = IMX335_GetShutterInfo;

    params->expo.vts = vts_30fps;
    params->expo.expo_lines = 5000;
    params->dirty = false;

    return SUCCESS;
}

static int cus_camsensor_init_handle_hdr_dol_sef1(ms_cus_sensor* drv_handle)
{
    ms_cus_sensor* handle = drv_handle;
    imx335_params* params = NULL;
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

    params = (imx335_params*)handle->private_data;
    memcpy(params->tExpo_rhs1_reg, expo_rhs1_reg, sizeof(expo_rhs1_reg));
    memcpy(params->tExpo_shr_dol1_reg, expo_shr_dol1_reg, sizeof(expo_shr_dol1_reg));
    memcpy(params->tGain_hdr_dol_sef_reg, gain_HDR_DOL_SEF1_reg, sizeof(gain_HDR_DOL_SEF1_reg));

    ////////////////////////////////////
    //    sensor model ID             //
    ////////////////////////////////////
    sprintf(handle->model_id, "IMX335_MIPI_HDR_SEF");

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
    handle->mclk = Preview_MCLK_SPEED_HDR_DOL; // UseParaMclk(SENSOR_DRV_PARAM_MCLK());

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

    handle->interface_attr.attr_mipi.mipi_lane_num = SENSOR_MIPI_LANE_NUM_DOL;
    handle->interface_attr.attr_mipi.mipi_data_format = CUS_SEN_INPUT_FORMAT_RGB;
    handle->interface_attr.attr_mipi.mipi_hsync_mode = SENSOR_MIPI_HSYNC_MODE_HDR_DOL;
    handle->interface_attr.attr_mipi.mipi_hdr_mode = CUS_HDR_MODE_SONY_DOL;
    handle->interface_attr.attr_mipi.mipi_hdr_virtual_channel_num = 1; // Short frame

    ////////////////////////////////////
    //    resolution capability       //
    ////////////////////////////////////
    handle->video_res_supported.ulcur_res = 0; // default resolution index is 0.
    for (res = 0; res < HDR_RES_END; res++) {
        handle->video_res_supported.num_res = res + 1;
        handle->video_res_supported.res[res].width = imx335_mipi_hdr[res].senif.preview_w;
        handle->video_res_supported.res[res].height = imx335_mipi_hdr[res].senif.preview_h;
        handle->video_res_supported.res[res].max_fps = imx335_mipi_hdr[res].senout.max_fps;
        handle->video_res_supported.res[res].min_fps = imx335_mipi_hdr[res].senout.min_fps;
        handle->video_res_supported.res[res].crop_start_x = imx335_mipi_hdr[res].senif.crop_start_X;
        handle->video_res_supported.res[res].crop_start_y = imx335_mipi_hdr[res].senif.crop_start_y;
        handle->video_res_supported.res[res].nOutputWidth = imx335_mipi_hdr[res].senout.width;
        handle->video_res_supported.res[res].nOutputHeight = imx335_mipi_hdr[res].senout.height;
        sprintf(handle->video_res_supported.res[res].strResDesc, imx335_mipi_hdr[res].senstr.strResDesc);
    }

    ////////////////////////////////////
    //    Sensor polarity             //
    ////////////////////////////////////
    handle->pwdn_POLARITY = SENSOR_PWDN_POL; // CUS_CLK_POL_NEG;
    handle->reset_POLARITY = SENSOR_RST_POL; // CUS_CLK_POL_NEG;
    // handle->VSYNC_POLARITY              = SENSOR_VSYNC_POL; //CUS_CLK_POL_POS;
    // handle->HSYNC_POLARITY              = SENSOR_HSYNC_POL; //CUS_CLK_POL_POS;
    handle->PCLK_POLARITY = SENSOR_PCLK_POL; // CUS_CLK_POL_POS);    // use '!' to clear board latch error

    ////////////////////////////////////////
    // Sensor Status Control and Get Info //
    ////////////////////////////////////////
    handle->pCus_sensor_release = cus_camsensor_release_handle;
    handle->pCus_sensor_init = pCus_init_mipi4lane5m25fps_HDR_DOL;
    handle->pCus_sensor_poweron = pCus_poweron; // Need to check
    handle->pCus_sensor_poweroff = pCus_poweroff;
    handle->pCus_sensor_GetSensorID = pCus_GetSensorID;
    handle->pCus_sensor_GetVideoResNum = pCus_GetVideoResNum;
    handle->pCus_sensor_SetVideoRes = pCus_SetVideoRes_HDR_DOL;
    handle->pCus_sensor_GetVideoRes = pCus_GetVideoRes;
    handle->pCus_sensor_GetCurVideoRes = pCus_GetCurVideoRes;

    handle->pCus_sensor_GetOrien = pCus_GetOrien; // Need to check
    handle->pCus_sensor_SetOrien = pCus_SetOrien; // Need to check
    handle->pCus_sensor_GetFPS = pCus_GetFPS_HDR_DOL_SEF1;
    handle->pCus_sensor_SetFPS = pCus_SetFPS_HDR_DOL_SEF1;

    ////////////////////////////////////
    //    AE parameters               //
    ////////////////////////////////////
    handle->ae_gain_delay = SENSOR_GAIN_DELAY_FRAME_COUNT_HDR_DOL;
    handle->ae_shutter_delay = SENSOR_SHUTTER_DELAY_FRAME_COUNT_HDR_DOL;
    handle->ae_gain_ctrl_num = 2;
    handle->ae_shutter_ctrl_num = 2;
    handle->sat_mingain = SENSOR_MIN_GAIN; // g_sensor_ae_min_gain;
    // handle->dgain_remainder = 0;

    ////////////////////////////////////
    //  AE Control and Get Info       //
    ////////////////////////////////////
    handle->pCus_sensor_AEStatusNotify = pCus_AEStatusNotifyHDR_DOL_SEF1;
    handle->pCus_sensor_GetAEUSecs = pCus_GetAEUSecs;
    handle->pCus_sensor_SetAEUSecs = pCus_SetAEUSecsHDR_DOL_SEF1;
    handle->pCus_sensor_GetAEGain = pCus_GetAEGain;
    handle->pCus_sensor_SetAEGain = pCus_SetAEGainHDR_DOL_SEF1;
    handle->pCus_sensor_GetAEMinMaxGain = pCus_GetAEMinMaxGain;
    handle->pCus_sensor_GetAEMinMaxUSecs = pCus_GetAEMinMaxUSecs;

    handle->pCus_sensor_GetShutterInfo = IMX335_GetShutterInfoHDR_DOL_SEF1;
    params->expo.vts = vts_30fps_HDR_DOL;
    params->expo.expo_lines = 4250;
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
    *id = 0;
    return SUCCESS;
}

static int pCus_GetFPS_HDR_DOL_LEF(ms_cus_sensor* handle)
{
    imx335_params* params = (imx335_params*)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 tVts = (params->tVts_reg[0].data << 16) | (params->tVts_reg[1].data << 8) | (params->tVts_reg[2].data << 0);

    if (params->expo.fps >= 1000)
        params->expo.preview_fps = (vts_30fps_HDR_DOL * max_fps * 1000) / tVts;
    else
        params->expo.preview_fps = (vts_30fps_HDR_DOL * max_fps) / tVts;

    return params->expo.preview_fps;
}

static int pCus_SetFPS_HDR_DOL_LEF(ms_cus_sensor* handle, u32 fps)
{
    u32 vts = 0;
    imx335_params* params = (imx335_params*)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 min_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].min_fps;

    if (fps >= min_fps && fps <= max_fps) {
        params->expo.fps = fps;
        params->expo.vts = (vts_30fps_HDR_DOL * max_fps) / fps;
    } else if ((fps >= (min_fps * 1000)) && (fps <= (max_fps * 1000))) {
        params->expo.fps = fps;
        params->expo.vts = (vts_30fps_HDR_DOL * (max_fps * 1000)) / fps;
    } else {
        // params->expo.vts=vts_30fps;
        // params->expo.fps=30;
        SENSOR_DMSG("[%s] FPS %d out of range.\n", __FUNCTION__, fps);
        return FAIL;
    }

    if (params->expo.expo_lines > 2 * params->expo.vts - params->max_rhs1 - 18) {
        vts = (params->expo.expo_lines + params->max_rhs1 + 18) / 2;
    } else {
        vts = params->expo.vts;
    }

    pCus_SetAEUSecsHDR_DOL_LEF(handle, params->expo.expo_lef_us);

    return SUCCESS;
}

static int imx335_SetPatternMode_hdr_dol_lef(ms_cus_sensor* handle, u32 mode)
{
    return SUCCESS;
}

static int pCus_AEStatusNotifyHDR_DOL_LEF(ms_cus_sensor* handle, CUS_CAMSENSOR_AE_STATUS_NOTIFY status)
{
    imx335_params* params = (imx335_params*)handle->private_data;

    switch (status) {
    case CUS_FRAME_INACTIVE:
        break;
    case CUS_FRAME_ACTIVE:
        if (params->dirty || params->orien_dirty) {
            SensorReg_Write(0x3001, 1);
            SensorRegArrayW((I2C_ARRAY*)params->tExpo_shr_dol1_reg, ARRAY_SIZE(expo_shr_dol1_reg));
            SensorRegArrayW((I2C_ARRAY*)params->tVts_reg, ARRAY_SIZE(vts_reg));
            SensorRegArrayW((I2C_ARRAY*)params->tExpo_reg, ARRAY_SIZE(expo_reg));
            SensorRegArrayW((I2C_ARRAY*)params->tExpo_rhs1_reg, ARRAY_SIZE(expo_rhs1_reg));
            SensorRegArrayW((I2C_ARRAY*)params->tGain_hdr_dol_lef_reg, ARRAY_SIZE(gain_HDR_DOL_LEF_reg));
            SensorRegArrayW((I2C_ARRAY*)params->tGain_hdr_dol_sef_reg, ARRAY_SIZE(gain_HDR_DOL_SEF1_reg));

            if (params->orien_dirty) {
                DoOrien(handle, handle->orient);
                params->orien_dirty = false;
            }
            SensorReg_Write(0x3001, 0);
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
    u32 qua_lines = 0, lines = 0, half_vts = 0, vts = 0, shr_dol0 = 0, fsc = 0;
    imx335_params* params = (imx335_params*)handle->private_data;

    qua_lines = (1000 * us) / Preview_line_period_HDR_DOL / 4;
    // lines=us/Preview_line_period_HDR_DOL;
    params->expo.expo_lef_us = us;

    if (4 * qua_lines > 2 * params->expo.vts - params->max_rhs1 - 18) { // shs2 > max_rhs1 +2
        half_vts = (4 * qua_lines + params->max_rhs1 + 19) / 4;
    } else {
        half_vts = params->expo.vts / 2;
    }

    SENSOR_DMSG("[%s] us %u, qua_lines %u, vts %u\n", __FUNCTION__,
        us,
        qua_lines,
        params->expo.vts);

    // exposure limit lines = fsc - (shs2 + 1) = fsc - 1 - ( rhs1 + 2 ~ fsc - 2) = 1 ~ fsc - 104 (rhs1 fix to 101)
    vts = half_vts * 2;
    fsc = half_vts * 4;
    if (qua_lines < 1) // shs2 < fsc - 2
        qua_lines = 1;
    if (4 * qua_lines > fsc - params->max_rhs1 - 18)
        qua_lines = (fsc - params->max_rhs1 - 18) / 4;

    lines = 4 * qua_lines;
    params->expo.expo_lines = lines;

    shr_dol0 = fsc - lines;
#if 0
    printk("[%s] us %u, qua_lines %u, vts %u shr_dol0 %u\n", __FUNCTION__,
                us,
                qua_lines,
                params->expo.vts,
                shr_dol0
                );
#endif
    params->tExpo_reg[0].data = (shr_dol0 >> 16) & 0x000f;
    params->tExpo_reg[1].data = (shr_dol0 >> 8) & 0x00ff;
    params->tExpo_reg[2].data = (shr_dol0 >> 0) & 0x00ff;

    params->tVts_reg[0].data = (vts >> 16) & 0x000f;
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
    imx335_params* params = (imx335_params*)handle->private_data;
    u16 gain_reg = 0;

    pCus_SetAEGainHDR_DOL_Calculate(gain, &gain_reg);
    params->tGain_hdr_dol_lef_reg[0].data = gain_reg & 0x00ff;
    params->tGain_hdr_dol_lef_reg[1].data = (gain_reg >> 8) & 0x0007;

    SENSOR_DMSG("[%s] set gain/reg=%u/0x%x\n", __FUNCTION__, gain, params->tGain_hdr_dol_lef_reg[0].data);

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
    *max = 1000000 / imx335_mipi_linear[0].senout.min_fps;
    return SUCCESS;
}

static int pCus_SetAEGain_cal_hdr_dol_lef(ms_cus_sensor* handle, u32 gain)
{
    return SUCCESS;
}

static int pCus_setCaliData_gain_linearity_hdr_dol_lef(ms_cus_sensor* handle, CUS_GAIN_GAP_ARRAY* pArray, u32 num)
{
    return SUCCESS;
}

static int IMX335_GetShutterInfo_hdr_dol_lef(struct __ms_cus_sensor* handle, CUS_SHUTTER_INFO* info)
{
    info->max = 1000000000 / imx335_mipi_linear[0].senout.min_fps;
    info->min = (Preview_line_period_HDR_DOL * 4);
    info->step = Preview_line_period_HDR_DOL * 4;
    return SUCCESS;
}

static int cus_camsensor_init_handle_hdr_dol_lef(ms_cus_sensor* drv_handle)
{
    ms_cus_sensor* handle = drv_handle;
    imx335_params* params;
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
    params = (imx335_params*)handle->private_data;
    memcpy(params->tVts_reg, vts_reg, sizeof(vts_reg));
    memcpy(params->tExpo_reg, expo_reg, sizeof(expo_reg));
    memcpy(params->tGain_hdr_dol_lef_reg, gain_HDR_DOL_LEF_reg, sizeof(gain_HDR_DOL_LEF_reg));

    ////////////////////////////////////
    //    sensor model ID             //
    ////////////////////////////////////
    sprintf(handle->model_id, "IMX335_MIPI_HDR_LEF");

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
    handle->mclk = Preview_MCLK_SPEED_HDR_DOL; // UseParaMclk(SENSOR_DRV_PARAM_MCLK());
    // sensor_if->MCLK(0,1,handle->mclk);

    ////////////////////////////////////
    //    sensor interface info       //
    ////////////////////////////////////
    handle->isp_type = SENSOR_ISP_TYPE; // ISP_SOC;
    // handle->data_fmt               = SENSOR_DATAFMT;   //CUS_DATAFMT_YUV;
    handle->sif_bus = SENSOR_IFBUS_TYPE; // CUS_SENIF_BUS_PARL;
    handle->data_prec = SENSOR_DATAPREC_DOL; // CUS_DATAPRECISION_8;
    handle->data_mode = SENSOR_DATAMODE;
    handle->bayer_id = SENSOR_BAYERID_HDR_DOL; // CUS_BAYER_GB;
    handle->RGBIR_id = SENSOR_RGBIRID;
    handle->orient = SENSOR_ORIT; // CUS_ORIT_M1F1;
    // handle->YC_ODER     = SENSOR_YCORDER;   //CUS_SEN_YCODR_CY;
    handle->interface_attr.attr_mipi.mipi_lane_num = SENSOR_MIPI_LANE_NUM_DOL;
    handle->interface_attr.attr_mipi.mipi_data_format = CUS_SEN_INPUT_FORMAT_RGB; // RGB pattern.
    handle->interface_attr.attr_mipi.mipi_yuv_order = 0; // don't care in RGB pattern.
    handle->interface_attr.attr_mipi.mipi_hsync_mode = SENSOR_MIPI_HSYNC_MODE_HDR_DOL;
    handle->interface_attr.attr_mipi.mipi_hdr_mode = CUS_HDR_MODE_SONY_DOL;
    handle->interface_attr.attr_mipi.mipi_hdr_virtual_channel_num = 0; // Long frame

    ////////////////////////////////////
    //    resolution capability       //
    ////////////////////////////////////
    handle->video_res_supported.ulcur_res = 0; // default resolution index is 0.
    for (res = 0; res < HDR_RES_END; res++) {
        handle->video_res_supported.num_res = res + 1;
        handle->video_res_supported.res[res].width = imx335_mipi_hdr[res].senif.preview_w;
        handle->video_res_supported.res[res].height = imx335_mipi_hdr[res].senif.preview_h;
        handle->video_res_supported.res[res].max_fps = imx335_mipi_hdr[res].senout.max_fps;
        handle->video_res_supported.res[res].min_fps = imx335_mipi_hdr[res].senout.min_fps;
        handle->video_res_supported.res[res].crop_start_x = imx335_mipi_hdr[res].senif.crop_start_X;
        handle->video_res_supported.res[res].crop_start_y = imx335_mipi_hdr[res].senif.crop_start_y;
        handle->video_res_supported.res[res].nOutputWidth = imx335_mipi_hdr[res].senout.width;
        handle->video_res_supported.res[res].nOutputHeight = imx335_mipi_hdr[res].senout.height;
        sprintf(handle->video_res_supported.res[res].strResDesc, imx335_mipi_hdr[res].senstr.strResDesc);
    }

    ////////////////////////////////////
    //    Sensor polarity             //
    ////////////////////////////////////
    handle->pwdn_POLARITY = SENSOR_PWDN_POL; // CUS_CLK_POL_NEG;
    handle->reset_POLARITY = SENSOR_RST_POL; // CUS_CLK_POL_NEG;
    // handle->VSYNC_POLARITY              = SENSOR_VSYNC_POL; //CUS_CLK_POL_POS;
    // handle->HSYNC_POLARITY              = SENSOR_HSYNC_POL; //CUS_CLK_POL_POS;
    handle->PCLK_POLARITY = SENSOR_PCLK_POL; // CUS_CLK_POL_POS);    // use '!' to clear board latch error

    ////////////////////////////////////////
    // Sensor Status Control and Get Info //
    ////////////////////////////////////////
    handle->pCus_sensor_release = cus_camsensor_release_handle;
    handle->pCus_sensor_init = pCus_init_HDR_DOL_LEF;
    handle->pCus_sensor_poweron = pCus_poweron_HDR_DOL_LEF;
    handle->pCus_sensor_poweroff = pCus_poweroff_HDR_DOL_LEF;
    handle->pCus_sensor_GetSensorID = pCus_GetSensorID_HDR_DOL_LEF;
    handle->pCus_sensor_GetVideoResNum = pCus_GetVideoResNum;
    handle->pCus_sensor_SetVideoRes = pCus_SetVideoRes_HDR_DOL_LEF;
    handle->pCus_sensor_GetVideoRes = pCus_GetVideoRes;
    handle->pCus_sensor_GetCurVideoRes = pCus_GetCurVideoRes;

    handle->pCus_sensor_GetOrien = pCus_GetOrien; // pCus_GetOrien_HDR_DOL_LEF
    handle->pCus_sensor_SetOrien = pCus_SetOrien; // pCus_SetOrien_HDR_DOL_LEF
    handle->pCus_sensor_GetFPS = pCus_GetFPS_HDR_DOL_LEF;
    handle->pCus_sensor_SetFPS = pCus_SetFPS_HDR_DOL_LEF;

    handle->pCus_sensor_SetPatternMode = imx335_SetPatternMode_hdr_dol_lef;

    ////////////////////////////////////
    //    AE parameters               //
    ////////////////////////////////////
    handle->ae_gain_delay = SENSOR_GAIN_DELAY_FRAME_COUNT_HDR_DOL;
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

    // sensor calibration
    handle->pCus_sensor_SetAEGain_cal = pCus_SetAEGain_cal_hdr_dol_lef;
    handle->pCus_sensor_setCaliData_gain_linearity = pCus_setCaliData_gain_linearity_hdr_dol_lef;
    handle->pCus_sensor_GetShutterInfo = IMX335_GetShutterInfo_hdr_dol_lef;

    params->expo.vts = vts_30fps_HDR_DOL;
    params->expo.expo_lines = 673;
    params->expo.fps = 25;
    params->dirty = false;

    return SUCCESS;
}

/* HDR/DOL disabled on Star6E — the two HDR handles are NULL so only the
 * linear (fps-ordered) lineup is registered.  The HDR init tables and AE
 * paths remain in-file but unreferenced (dead), mirroring the "linear-only"
 * product decision; a later cleanup can strip them for Maruko source parity. */
SENSOR_DRV_ENTRY_IMPL_END_EX(IMX335_HDR,
    cus_camsensor_init_handle_linear,
    NULL,
    NULL,
    imx335_params);
