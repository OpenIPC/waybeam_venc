/* SigmaStar MI API type definitions for Infinity6/6E/6C platforms.
 *
 * Self-contained header — no dependency on the OpenIPC divinus HAL.
 * These struct/enum layouts must match the vendor .so ABI exactly.
 *
 * Covers:  MI_SYS, MI_SNR, MI_VIF, MI_VPE, MI_VENC
 * Sources: reverse-engineered from SigmaStar SDK headers (i6/i6e/i6c).
 */
#ifndef SIGMASTAR_TYPES_H
#define SIGMASTAR_TYPES_H

#include <stdint.h>

/* ====================================================================
 * Common types  (shared across all MI modules)
 * ==================================================================== */

typedef enum {
    I6_BAYER_RG,
    I6_BAYER_GR,
    I6_BAYER_BG,
    I6_BAYER_GB,
    I6_BAYER_R0,
    I6_BAYER_G0,
    I6_BAYER_B0,
    I6_BAYER_G1,
    I6_BAYER_G2,
    I6_BAYER_I0,
    I6_BAYER_G3,
    I6_BAYER_I1,
    I6_BAYER_END
} i6_common_bayer;

typedef enum {
    I6_COMPR_NONE,
    I6_COMPR_SEG,
    I6_COMPR_LINE,
    I6_COMPR_FRAME,
    I6_COMPR_8BIT,
    I6_COMPR_END
} i6_common_compr;

typedef enum {
    I6_EDGE_SINGLE_UP,
    I6_EDGE_SINGLE_DOWN,
    I6_EDGE_DOUBLE,
    I6_EDGE_END
} i6_common_edge;

typedef enum {
    I6_HDR_OFF,
    I6_HDR_VC,
    I6_HDR_DOL,
    I6_HDR_EMBED,
    I6_HDR_LI,
    I6_HDR_END
} i6_common_hdr;

typedef enum {
    I6_INPUT_VUVU = 0,
    I6_INPUT_UVUV,
    I6_INPUT_UYVY = 0,
    I6_INPUT_VYUY,
    I6_INPUT_YUYV,
    I6_INPUT_YVYU,
    I6_INPUT_END
} i6_common_input;

typedef enum {
    I6_INTF_BT656,
    I6_INTF_DIGITAL_CAMERA,
    I6_INTF_BT1120_STANDARD,
    I6_INTF_BT1120_INTERLEAVED,
    I6_INTF_MIPI,
    I6_INTF_END
} i6_common_intf;

typedef enum {
    I6_PREC_8BPP,
    I6_PREC_10BPP,
    I6_PREC_12BPP,
    I6_PREC_14BPP,
    I6_PREC_16BPP,
    I6_PREC_END
} i6_common_prec;

typedef enum {
    I6_PIXFMT_YUV422_YUYV,
    I6_PIXFMT_ARGB8888,
    I6_PIXFMT_ABGR8888,
    I6_PIXFMT_BGRA8888,
    I6_PIXFMT_RGB565,
    I6_PIXFMT_ARGB1555,
    I6_PIXFMT_ARGB4444,
    I6_PIXFMT_I2,
    I6_PIXFMT_I4,
    I6_PIXFMT_I8,
    I6_PIXFMT_YUV422SP,
    I6_PIXFMT_YUV420SP,
    I6_PIXFMT_YUV420SP_NV21,
    I6_PIXFMT_YUV420_MST,
    I6_PIXFMT_YUV422_UYVY,
    I6_PIXFMT_YUV422_YVYU,
    I6_PIXFMT_YUV422_VYUY,
    I6_PIXFMT_YC420_MSTITLE1_H264,
    I6_PIXFMT_YC420_MSTITLE2_H265,
    I6_PIXFMT_YC420_MSTITLE3_H265,
    I6_PIXFMT_RGB_BAYER,
    I6_PIXFMT_RGB_BAYER_END =
        I6_PIXFMT_RGB_BAYER + I6_PREC_END * I6_BAYER_END - 1,
    I6_PIXFMT_RGB888,
    I6_PIXFMT_BGR888,
    I6_PIXFMT_GRAY8,
    I6_PIXFMT_RGB101010,
    I6_PIXFMT_RGB888P,
    I6_PIXFMT_END
} i6_common_pixfmt;

typedef struct {
    unsigned short width;
    unsigned short height;
} i6_common_dim;

typedef struct {
    unsigned short x;
    unsigned short y;
    unsigned short width;
    unsigned short height;
} i6_common_rect;

typedef struct {
    int vsyncInv;
    int hsyncInv;
    int pixclkInv;
    unsigned int vsyncDelay;
    unsigned int hsyncDelay;
    unsigned int pixclkDelay;
} i6_common_sync;

/* ====================================================================
 * MI_SYS  (system binding / channel port)
 * ==================================================================== */

typedef enum {
    I6_SYS_LINK_FRAMEBASE = 0x1,
    I6_SYS_LINK_LOWLATENCY = 0x2,
    I6_SYS_LINK_REALTIME = 0x4,
    I6_SYS_LINK_AUTOSYNC = 0x8,
    I6_SYS_LINK_RING = 0x10
} i6_sys_link;

typedef enum {
    I6_SYS_MOD_IVE,
    I6_SYS_MOD_VDF,
    I6_SYS_MOD_VENC,
    I6_SYS_MOD_RGN,
    I6_SYS_MOD_AI,
    I6_SYS_MOD_AO,
    I6_SYS_MOD_VIF,
    I6_SYS_MOD_VPE,
    I6_SYS_MOD_VDEC,
    I6_SYS_MOD_SYS,
    I6_SYS_MOD_FB,
    I6_SYS_MOD_HDMI,
    I6_SYS_MOD_DIVP,
    I6_SYS_MOD_GFX,
    I6_SYS_MOD_VDISP,
    I6_SYS_MOD_DISP,
    I6_SYS_MOD_OS,
    I6_SYS_MOD_IAE,
    I6_SYS_MOD_MD,
    I6_SYS_MOD_OD,
    I6_SYS_MOD_SHADOW,
    I6_SYS_MOD_WARP,
    I6_SYS_MOD_UAC,
    I6_SYS_MOD_LDC,
    I6_SYS_MOD_SD,
    I6_SYS_MOD_PANEL,
    I6_SYS_MOD_CIPHER,
    I6_SYS_MOD_SNR,
    I6_SYS_MOD_WLAN,
    I6_SYS_MOD_IPU,
    I6_SYS_MOD_MIPITX,
    I6_SYS_MOD_GYRO,
    I6_SYS_MOD_JPD,
    I6_SYS_MOD_ISP,
    I6_SYS_MOD_SCL,
    I6_SYS_MOD_WBC,
    I6_SYS_MOD_DSP,
    I6_SYS_MOD_PCIE,
    I6_SYS_MOD_DUMMY,
    I6_SYS_MOD_NIR,
    I6_SYS_MOD_DPU,
    I6_SYS_MOD_END,
} i6_sys_mod;

typedef struct {
    i6_sys_mod module;
    unsigned int device;
    unsigned int channel;
    unsigned int port;
} i6_sys_bind;

/* ====================================================================
 * MI_SNR  (sensor)
 * ==================================================================== */

typedef enum {
    I6_SNR_HWHDR_NONE,
    I6_SNR_HWHDR_SONY_DOL,
    I6_SNR_HWHDR_DCG,
    I6_SNR_HWHDR_EMBED_RAW8,
    I6_SNR_HWHDR_EMBED_RAW10,
    I6_SNR_HWHDR_EMBED_RAW12,
    I6_SNR_HWHDR_EMBED_RAW16
} i6_snr_hwhdr;

typedef struct {
    unsigned int laneCnt;
    unsigned int rgbFmtOn;
    i6_common_input input;
    unsigned int hsyncMode;
    unsigned int sampDelay;
    i6_snr_hwhdr hwHdr;
    unsigned int virtChn;
    unsigned int packType[2];
} i6_snr_mipi;

typedef struct {
    unsigned int multplxNum;
    i6_common_sync sync;
    i6_common_edge edge;
    int bitswap;
} i6_snr_bt656;

typedef struct {
    i6_common_sync sync;
} i6_snr_par;

typedef union {
    i6_snr_par parallel;
    i6_snr_mipi mipi;
    i6_snr_bt656 bt656;
} i6_snr_intfattr;

typedef struct {
    unsigned int planeCnt;
    i6_common_intf intf;
    i6_common_hdr hdr;
    i6_snr_intfattr intfAttr;
    char earlyInit;
} i6_snr_pad;

typedef struct {
    unsigned int planeId;
    char sensName[32];
    i6_common_rect capt;
    i6_common_bayer bayer;
    i6_common_prec precision;
    int hdrSrc;
    unsigned int shutter;
    unsigned int sensGain;
    unsigned int compGain;
    i6_common_pixfmt pixFmt;
} i6_snr_plane;

typedef struct {
    i6_common_rect crop;
    i6_common_dim output;
    unsigned int maxFps;
    unsigned int minFps;
    char desc[32];
} __attribute__((packed, aligned(4))) i6_snr_res;

/* ====================================================================
 * MI_VIF  (video input interface)
 * ==================================================================== */

typedef enum {
    I6_VIF_FRATE_FULL,
    I6_VIF_FRATE_HALF,
    I6_VIF_FRATE_QUART,
    I6_VIF_FRATE_OCTANT,
    I6_VIF_FRATE_3QUARTS,
    I6_VIF_FRATE_END
} i6_vif_frate;

typedef enum {
    I6_VIF_WORK_1MULTIPLEX,
    I6_VIF_WORK_2MULTIPLEX,
    I6_VIF_WORK_4MULTIPLEX,
    I6_VIF_WORK_RGB_REALTIME,
    I6_VIF_WORK_RGB_FRAME,
    I6_VIF_WORK_END
} i6_vif_work;

typedef struct {
    i6_common_intf intf;
    i6_vif_work work;
    i6_common_hdr hdr;
    i6_common_edge edge;
    i6_common_input input;
    char bitswap;
    i6_common_sync sync;
} i6_vif_dev;

typedef struct {
    i6_common_rect capt;
    i6_common_dim dest;
    int field;
    int interlaceOn;
    i6_common_pixfmt pixFmt;
    i6_vif_frate frate;
    unsigned int frameLineCnt;
} i6_vif_port;

/* ====================================================================
 * MI_VPE  (video processing engine)
 * ==================================================================== */

typedef enum {
    I6_VPE_MODE_INVALID,
    I6_VPE_MODE_DVR = 0x1,
    I6_VPE_MODE_CAM_TOP = 0x2,
    I6_VPE_MODE_CAM_BOTTOM = 0x4,
    I6_VPE_MODE_CAM = I6_VPE_MODE_CAM_TOP | I6_VPE_MODE_CAM_BOTTOM,
    I6_VPE_MODE_REALTIME_TOP = 0x8,
    I6_VPE_MODE_REALTIME_BOTTOM = 0x10,
    I6_VPE_MODE_REALTIME = I6_VPE_MODE_REALTIME_TOP | I6_VPE_MODE_REALTIME_BOTTOM,
    I6_VPE_MODE_END
} i6_vpe_mode;

typedef enum {
    I6_VPE_SENS_INVALID,
    I6_VPE_SENS_ID0,
    I6_VPE_SENS_ID1,
    I6_VPE_SENS_ID2,
    I6_VPE_SENS_ID3,
    I6_VPE_SENS_END,
} i6_vpe_sens;

typedef struct {
    unsigned int rev;
    unsigned int size;
    unsigned char data[64];
} i6_vpe_iqver;

/* i6e (infinity6e / Star6E) extended lens-correction structs.
 * The driver reads these fields even when lens correction is off;
 * using the smaller i6_ struct causes the driver to read past
 * the struct boundary into stack garbage. */
typedef struct {
    int mode;
    char bypassOn;
    char proj3x3On;
    int proj3x3[9];
    unsigned short userSliceNum;
    unsigned int focalLengthX;
    unsigned int focalLengthY;
    void *configAddr;
    unsigned int configSize;
    int mapType;
    union {
        struct {
            void *xMapAddr, *yMapAddr;
            unsigned int xMapSize, yMapSize;
        } dispInfo;
        struct {
            void *calibPolyBinAddr;
            unsigned int calibPolyBinSize;
        } calibInfo;
    };
    char lensAdjOn;
} i6e_vpe_ildc;

typedef struct {
    char bypassOn;
    char proj3x3On;
    int proj3x3[9];
    unsigned int focalLengthX;
    unsigned int focalLengthY;
    void *configAddr;
    unsigned int configSize;
    union {
        struct {
            void *xMapAddr, *yMapAddr;
            unsigned int xMapSize, yMapSize;
        } dispInfo;
        struct {
            void *calibPolyBinAddr;
            unsigned int calibPolyBinSize;
        } calibInfo;
    };
} i6e_vpe_ldc;

/* Infinity6E (Star6E) extended VPE channel — includes lens init. */
typedef struct {
    i6_common_dim capt;
    i6_common_pixfmt pixFmt;
    i6_common_hdr hdr;
    i6_vpe_sens sensor;
    char noiseRedOn;
    char edgeOn;
    char edgeSmoothOn;
    char contrastOn;
    char invertOn;
    char rotateOn;
    i6_vpe_mode mode;
    i6_vpe_iqver iqparam;
    i6e_vpe_ildc lensInit;
    char lensAdjOn;
    unsigned int chnPort;
} i6e_vpe_chn;

/* Standard Infinity6 VPE channel (no lens init). */
typedef struct {
    i6_common_dim capt;
    i6_common_pixfmt pixFmt;
    i6_common_hdr hdr;
    i6_vpe_sens sensor;
    char noiseRedOn;
    char edgeOn;
    char edgeSmoothOn;
    char contrastOn;
    char invertOn;
    char rotateOn;
    i6_vpe_mode mode;
    i6_vpe_iqver iqparam;
    char lensAdjOn;
    unsigned int chnPort;
} i6_vpe_chn;

/* Infinity6E extended VPE parameters. */
typedef struct {
    char reserved[16];
    i6e_vpe_ldc lensAdj;
    i6_common_hdr hdr;
    int level3DNR;
    char mirror;
    char flip;
    char reserved2;
    char lensAdjOn;
} i6e_vpe_para;

/* Standard Infinity6 VPE parameters. */
typedef struct {
    char reserved[16];
    i6_common_hdr hdr;
    int level3DNR;
    char mirror;
    char flip;
    char reserved2;
    char lensAdjOn;
} i6_vpe_para;

typedef struct {
    i6_common_dim output;
    char mirror;
    char flip;
    i6_common_pixfmt pixFmt;
    i6_common_compr compress;
} i6_vpe_port;

/* ====================================================================
 * MI_VENC  (video encoder) — Infinity6 / Infinity6E
 * ==================================================================== */

#define I6_VENC_CHN_NUM 9

typedef enum {
    I6_VENC_CODEC_H264 = 2,
    I6_VENC_CODEC_H265,
    I6_VENC_CODEC_MJPG,
    I6_VENC_CODEC_END
} i6_venc_codec;

typedef enum {
    I6_VENC_NALU_H264_PSLICE = 1,
    I6_VENC_NALU_H264_ISLICE = 5,
    I6_VENC_NALU_H264_SEI,
    I6_VENC_NALU_H264_SPS,
    I6_VENC_NALU_H264_PPS,
    I6_VENC_NALU_H264_IPSLICE,
    I6_VENC_NALU_H264_PREFIX = 14,
    I6_VENC_NALU_H264_END
} i6_venc_nalu_h264;

typedef enum {
    I6_VENC_NALU_H265_PSLICE = 1,
    I6_VENC_NALU_H265_ISLICE = 19,
    I6_VENC_NALU_H265_VPS = 32,
    I6_VENC_NALU_H265_SPS,
    I6_VENC_NALU_H265_PPS,
    I6_VENC_NALU_H265_SEI = 39,
    I6_VENC_NALU_H265_END
} i6_venc_nalu_h265;

typedef enum {
    I6_VENC_NALU_MJPG_ECS = 5,
    I6_VENC_NALU_MJPG_APP,
    I6_VENC_NALU_MJPG_VDO,
    I6_VENC_NALU_MJPG_PIC,
    I6_VENC_NALU_MJPG_END
} i6_venc_nalu_mjpg;

typedef enum {
    I6_VENC_SRC_CONF_NORMAL,
    I6_VENC_SRC_CONF_RING_ONE,
    I6_VENC_SRC_CONF_RING_HALF,
    I6_VENC_SRC_CONF_END
} i6_venc_src_conf;

typedef enum {
    I6_VENC_RATEMODE_H264CBR = 1,
    I6_VENC_RATEMODE_H264VBR,
    I6_VENC_RATEMODE_H264ABR,
    I6_VENC_RATEMODE_H264QP,
    I6_VENC_RATEMODE_H264AVBR,
    I6_VENC_RATEMODE_MJPGCBR,
    I6_VENC_RATEMODE_MJPGQP,
    I6_VENC_RATEMODE_H265CBR,
    I6_VENC_RATEMODE_H265VBR,
    I6_VENC_RATEMODE_H265QP,
    I6_VENC_RATEMODE_H265AVBR,
    I6_VENC_RATEMODE_END
} i6_venc_ratemode;

typedef struct {
    unsigned int maxWidth;
    unsigned int maxHeight;
    unsigned int bufSize;
    unsigned int profile;
    char byFrame;
    unsigned int width;
    unsigned int height;
    unsigned int bFrameNum;
    unsigned int refNum;
} i6_venc_attr_h26x;

typedef struct {
    unsigned int maxWidth;
    unsigned int maxHeight;
    unsigned int bufSize;
    char byFrame;
    unsigned int width;
    unsigned int height;
    char dcfThumbs;
    unsigned int markPerRow;
} i6_venc_attr_mjpg;

typedef struct {
    i6_venc_codec codec;
    union {
        i6_venc_attr_h26x h264;
        i6_venc_attr_mjpg mjpg;
        i6_venc_attr_h26x h265;
    };
} i6_venc_attrib;

typedef struct {
    unsigned int gop;
    unsigned int statTime;
    unsigned int fpsNum;
    unsigned int fpsDen;
    unsigned int bitrate;
    unsigned int avgLvl;
} i6_venc_rate_h26xcbr;

typedef struct {
    unsigned int gop;
    unsigned int statTime;
    unsigned int fpsNum;
    unsigned int fpsDen;
    unsigned int maxBitrate;
    unsigned int maxQual;
    unsigned int minQual;
} i6_venc_rate_h26xvbr;

typedef struct {
    unsigned int gop;
    unsigned int fpsNum;
    unsigned int fpsDen;
    unsigned int interQual;
    unsigned int predQual;
} i6_venc_rate_h26xqp;

typedef struct {
    unsigned int gop;
    unsigned int statTime;
    unsigned int fpsNum;
    unsigned int fpsDen;
    unsigned int avgBitrate;
    unsigned int maxBitrate;
} i6_venc_rate_h26xabr;

typedef struct {
    unsigned int bitrate;
    unsigned int fpsNum;
    unsigned int fpsDen;
} i6_venc_rate_mjpgcbr;

typedef struct {
    unsigned int fpsNum;
    unsigned int fpsDen;
    unsigned int quality;
} i6_venc_rate_mjpgqp;

typedef struct {
    i6_venc_ratemode mode;
    union {
        i6_venc_rate_h26xcbr h264Cbr;
        i6_venc_rate_h26xvbr h264Vbr;
        i6_venc_rate_h26xqp h264Qp;
        i6_venc_rate_h26xabr h264Abr;
        i6_venc_rate_h26xvbr h264Avbr;
        i6_venc_rate_mjpgcbr mjpgCbr;
        i6_venc_rate_mjpgqp mjpgQp;
        i6_venc_rate_h26xcbr h265Cbr;
        i6_venc_rate_h26xvbr h265Vbr;
        i6_venc_rate_h26xqp h265Qp;
        i6_venc_rate_h26xvbr h265Avbr;
    };
    void *extend;
} i6_venc_rate;

typedef struct {
    i6_venc_attrib attrib;
    i6_venc_rate rate;
} i6_venc_chn;

typedef struct {
    unsigned int quality;
    unsigned char qtLuma[64];
    unsigned char qtChroma[64];
    unsigned int mcuPerEcs;
} i6_venc_jpg;

typedef union {
    i6_venc_nalu_h264 h264Nalu;
    i6_venc_nalu_mjpg mjpgNalu;
    i6_venc_nalu_h265 h265Nalu;
} i6_venc_nalu;

typedef struct {
    i6_venc_nalu packType;
    unsigned int offset;
    unsigned int length;
    unsigned int sliceId;
} i6_venc_packinfo;

typedef struct {
    unsigned long long addr;
    unsigned char *data;
    unsigned int length;
    unsigned long long timestamp;
    char endFrame;
    i6_venc_nalu naluType;
    unsigned int offset;
    unsigned int packNum;
    i6_venc_packinfo packetInfo[8];
} i6_venc_pack;

typedef struct {
    unsigned int leftPics;
    unsigned int leftBytes;
    unsigned int leftFrames;
    unsigned int leftMillis;
    unsigned int curPacks;
    unsigned int leftRecvPics;
    unsigned int leftEncPics;
    unsigned int fpsNum;
    unsigned int fpsDen;
    unsigned int bitrate;
} i6_venc_stat;

typedef struct {
    unsigned int size;
    unsigned int skipMb;
    unsigned int ipcmMb;
    unsigned int iMb16x8;
    unsigned int iMb16x16;
    unsigned int iMb8x16;
    unsigned int iMb8x8;
    unsigned int pMb16;
    unsigned int pMb8;
    unsigned int pMb4;
    unsigned int refSliceType;
    unsigned int refType;
    unsigned int updAttrCnt;
    unsigned int startQual;
} i6_venc_strminfo_h264;

typedef struct {
    unsigned int size;
    unsigned int iCu64x64;
    unsigned int iCu32x32;
    unsigned int iCu16x16;
    unsigned int iCu8x8;
    unsigned int pCu32x32;
    unsigned int pCu16x16;
    unsigned int pCu8x8;
    unsigned int pCu4x4;
    unsigned int refType;
    unsigned int updAttrCnt;
    unsigned int startQual;
} i6_venc_strminfo_h265;

typedef struct {
    unsigned int size;
    unsigned int updAttrCnt;
    unsigned int quality;
} i6_venc_strminfo_mjpg;

typedef struct {
    i6_venc_pack *packet;
    unsigned int count;
    unsigned int sequence;
    int handle;
    union {
        i6_venc_strminfo_h264 h264Info;
        i6_venc_strminfo_mjpg mjpgInfo;
        i6_venc_strminfo_h265 h265Info;
    };
} i6_venc_strm;

/* ====================================================================
 * Infinity6C (Maruko) — MI_SYS extensions
 * ==================================================================== */

typedef enum {
    I6C_SYS_MOD_IVE,
    I6C_SYS_MOD_VDF,
    I6C_SYS_MOD_VENC,
    I6C_SYS_MOD_RGN,
    I6C_SYS_MOD_AI,
    I6C_SYS_MOD_AO,
    I6C_SYS_MOD_VIF,
    I6C_SYS_MOD_VPE,
    I6C_SYS_MOD_VDEC,
    I6C_SYS_MOD_SYS,
    I6C_SYS_MOD_FB,
    I6C_SYS_MOD_HDMI,
    I6C_SYS_MOD_DIVP,
    I6C_SYS_MOD_GFX,
    I6C_SYS_MOD_VDISP,
    I6C_SYS_MOD_DISP,
    I6C_SYS_MOD_OS,
    I6C_SYS_MOD_IAE,
    I6C_SYS_MOD_MD,
    I6C_SYS_MOD_OD,
    I6C_SYS_MOD_SHADOW,
    I6C_SYS_MOD_WARP,
    I6C_SYS_MOD_UAC,
    I6C_SYS_MOD_LDC,
    I6C_SYS_MOD_SD,
    I6C_SYS_MOD_PANEL,
    I6C_SYS_MOD_CIPHER,
    I6C_SYS_MOD_SNR,
    I6C_SYS_MOD_WLAN,
    I6C_SYS_MOD_IPU,
    I6C_SYS_MOD_MIPITX,
    I6C_SYS_MOD_GYRO,
    I6C_SYS_MOD_JPD,
    I6C_SYS_MOD_ISP,
    I6C_SYS_MOD_SCL,
    I6C_SYS_MOD_WBC,
    I6C_SYS_MOD_DSP,
    I6C_SYS_MOD_PCIE,
    I6C_SYS_MOD_DUMMY,
    I6C_SYS_MOD_NIR,
    I6C_SYS_MOD_DPU,
    I6C_SYS_MOD_END,
} i6c_sys_mod;

typedef enum {
    I6C_SYS_POOL_ENCODER_RING,
    I6C_SYS_POOL_CHANNEL,
    I6C_SYS_POOL_DEVICE,
    I6C_SYS_POOL_OUTPUT,
    I6C_SYS_POOL_DEVICE_RING
} i6c_sys_pooltype;

typedef struct {
    i6c_sys_mod module;
    unsigned int device;
    unsigned int channel;
    unsigned int port;
} i6c_sys_bind;

typedef struct {
    i6c_sys_mod module;
    unsigned int device;
    unsigned int channel;
    unsigned char heapName[32];
    unsigned int heapSize;
} i6c_sys_poolchn;

typedef struct {
    i6c_sys_mod module;
    unsigned int device;
    unsigned int reserved;
    unsigned char heapName[32];
    unsigned int heapSize;
} i6c_sys_pooldev;

typedef struct {
    unsigned int ringSize;
    unsigned char heapName[32];
} i6c_sys_poolenc;

typedef struct {
    i6c_sys_mod module;
    unsigned int device;
    unsigned int channel;
    unsigned int port;
    unsigned char heapName[32];
    unsigned int heapSize;
} i6c_sys_poolout;

typedef struct {
    i6c_sys_mod module;
    unsigned int device;
    unsigned short maxWidth;
    unsigned short maxHeight;
    unsigned short ringLine;
    unsigned char heapName[32];
} i6c_sys_poolring;

typedef struct {
    i6c_sys_pooltype type;
    char create;
    union {
        i6c_sys_poolchn channel;
        i6c_sys_pooldev device;
        i6c_sys_poolenc encode;
        i6c_sys_poolout output;
        i6c_sys_poolring ring;
    } config;
} i6c_sys_pool;

/* ====================================================================
 * Infinity6C (Maruko) — MI_VENC
 * ==================================================================== */

#define I6C_VENC_CHN_NUM 12
#define I6C_VENC_DEV_H26X_0 0
#define I6C_VENC_DEV_MJPG_0 8

typedef enum {
    I6C_VENC_CODEC_H264 = 2,
    I6C_VENC_CODEC_H265,
    I6C_VENC_CODEC_MJPG,
    I6C_VENC_CODEC_END
} i6c_venc_codec;

typedef enum {
    I6C_VENC_NALU_H264_PSLICE = 1,
    I6C_VENC_NALU_H264_ISLICE = 5,
    I6C_VENC_NALU_H264_SEI,
    I6C_VENC_NALU_H264_SPS,
    I6C_VENC_NALU_H264_PPS,
    I6C_VENC_NALU_H264_IPSLICE,
    I6C_VENC_NALU_H264_PREFIX = 14,
    I6C_VENC_NALU_H264_END
} i6c_venc_nalu_h264;

typedef enum {
    I6C_VENC_NALU_H265_PSLICE = 1,
    I6C_VENC_NALU_H265_ISLICE = 19,
    I6C_VENC_NALU_H265_VPS = 32,
    I6C_VENC_NALU_H265_SPS,
    I6C_VENC_NALU_H265_PPS,
    I6C_VENC_NALU_H265_SEI = 39,
    I6C_VENC_NALU_H265_END
} i6c_venc_nalu_h265;

typedef enum {
    I6C_VENC_NALU_MJPG_ECS = 5,
    I6C_VENC_NALU_MJPG_APP,
    I6C_VENC_NALU_MJPG_VDO,
    I6C_VENC_NALU_MJPG_PIC,
    I6C_VENC_NALU_MJPG_END
} i6c_venc_nalu_mjpg;

typedef enum {
    I6C_VENC_SRC_CONF_NORMAL,
    I6C_VENC_SRC_CONF_RING_ONE,
    I6C_VENC_SRC_CONF_RING_HALF,
    I6C_VENC_SRC_CONF_HW_SYNC,
    I6C_VENC_SRC_CONF_RING_DMA,
    I6C_VENC_SRC_CONF_END
} i6c_venc_src_conf;

typedef enum {
    I6C_VENC_RATEMODE_H264CBR = 1,
    I6C_VENC_RATEMODE_H264VBR,
    I6C_VENC_RATEMODE_H264ABR,
    I6C_VENC_RATEMODE_H264QP,
    I6C_VENC_RATEMODE_H264AVBR,
    I6C_VENC_RATEMODE_MJPGCBR,
    I6C_VENC_RATEMODE_MJPGVBR,
    I6C_VENC_RATEMODE_MJPGQP,
    I6C_VENC_RATEMODE_H265CBR,
    I6C_VENC_RATEMODE_H265VBR,
    I6C_VENC_RATEMODE_H265QP,
    I6C_VENC_RATEMODE_H265AVBR,
    I6C_VENC_RATEMODE_END
} i6c_venc_ratemode;

typedef struct {
    unsigned int maxWidth;
    unsigned int maxHeight;
    unsigned int bufSize;
    unsigned int profile;
    char byFrame;
    unsigned int width;
    unsigned int height;
    unsigned int bFrameNum;
    unsigned int refNum;
} i6c_venc_attr_h26x;

typedef struct {
    unsigned int maxWidth;
    unsigned int maxHeight;
    unsigned int bufSize;
    char byFrame;
    unsigned int width;
    unsigned int height;
    char dcfThumbs;
    unsigned int markPerRow;
} i6c_venc_attr_mjpg;

typedef struct {
    i6c_venc_codec codec;
    union {
        i6c_venc_attr_h26x h264;
        i6c_venc_attr_mjpg mjpg;
        i6c_venc_attr_h26x h265;
    };
} i6c_venc_attrib;

typedef struct {
    unsigned int gop;
    unsigned int statTime;
    unsigned int fpsNum;
    unsigned int fpsDen;
    unsigned int bitrate;
    unsigned int avgLvl;
} i6c_venc_rate_h26xcbr;

typedef struct {
    unsigned int gop;
    unsigned int statTime;
    unsigned int fpsNum;
    unsigned int fpsDen;
    unsigned int maxBitrate;
    unsigned int maxQual;
    unsigned int minQual;
} i6c_venc_rate_h26xvbr;

typedef struct {
    unsigned int gop;
    unsigned int fpsNum;
    unsigned int fpsDen;
    unsigned int interQual;
    unsigned int predQual;
} i6c_venc_rate_h26xqp;

typedef struct {
    unsigned int gop;
    unsigned int statTime;
    unsigned int fpsNum;
    unsigned int fpsDen;
    unsigned int avgBitrate;
    unsigned int maxBitrate;
} i6c_venc_rate_h26xabr;

typedef struct {
    unsigned int bitrate;
    unsigned int fpsNum;
    unsigned int fpsDen;
} i6c_venc_rate_mjpgbr;

typedef struct {
    unsigned int fpsNum;
    unsigned int fpsDen;
    unsigned int quality;
} i6c_venc_rate_mjpgqp;

typedef struct {
    int mode;
    union {
        i6c_venc_rate_h26xcbr h264Cbr;
        i6c_venc_rate_h26xvbr h264Vbr;
        i6c_venc_rate_h26xqp h264Qp;
        i6c_venc_rate_h26xabr h264Abr;
        i6c_venc_rate_h26xvbr h264Avbr;
        i6c_venc_rate_mjpgbr mjpgCbr;
        i6c_venc_rate_mjpgbr mjpgVbr;
        i6c_venc_rate_mjpgqp mjpgQp;
        i6c_venc_rate_h26xcbr h265Cbr;
        i6c_venc_rate_h26xvbr h265Vbr;
        i6c_venc_rate_h26xqp h265Qp;
        i6c_venc_rate_h26xvbr h265Avbr;
    };
    void *extend;
} i6c_venc_rate;

typedef struct {
    i6c_venc_attrib attrib;
    i6c_venc_rate rate;
} i6c_venc_chn;

typedef struct {
    unsigned int maxWidth;
    unsigned int maxHeight;
} i6c_venc_init;

typedef struct {
    unsigned int quality;
    unsigned char qtLuma[64];
    unsigned char qtChroma[64];
    unsigned int mcuPerEcs;
} i6c_venc_jpg;

typedef union {
    i6c_venc_nalu_h264 h264Nalu;
    i6c_venc_nalu_mjpg mjpgNalu;
    i6c_venc_nalu_h265 h265Nalu;
} i6c_venc_nalu;

typedef struct {
    i6c_venc_nalu packType;
    unsigned int offset;
    unsigned int length;
    unsigned int sliceId;
} i6c_venc_packinfo;

typedef struct {
    unsigned long long addr;
    unsigned char *data;
    unsigned int length;
    unsigned long long timestamp;
    char endFrame;
    i6c_venc_nalu naluType;
    unsigned int offset;
    unsigned int packNum;
    unsigned char frameQual;
    int picOrder;
    unsigned int gradient;
    i6c_venc_packinfo packetInfo[8];
} i6c_venc_pack;

typedef struct {
    unsigned int leftPics;
    unsigned int leftBytes;
    unsigned int leftFrames;
    unsigned int leftMillis;
    unsigned int curPacks;
    unsigned int leftRecvPics;
    unsigned int leftEncPics;
    unsigned int fpsNum;
    unsigned int fpsDen;
    unsigned int bitrate;
} i6c_venc_stat;

typedef struct {
    unsigned int size;
    unsigned int skipMb;
    unsigned int ipcmMb;
    unsigned int iMb16x8;
    unsigned int iMb16x16;
    unsigned int iMb8x16;
    unsigned int iMb8x8;
    unsigned int pMb16;
    unsigned int pMb8;
    unsigned int pMb4;
    unsigned int refSliceType;
    unsigned int refType;
    unsigned int updAttrCnt;
    unsigned int startQual;
} i6c_venc_strminfo_h264;

typedef struct {
    unsigned int size;
    unsigned int iCu64x64;
    unsigned int iCu32x32;
    unsigned int iCu16x16;
    unsigned int iCu8x8;
    unsigned int pCu32x32;
    unsigned int pCu16x16;
    unsigned int pCu8x8;
    unsigned int pCu4x4;
    unsigned int refType;
    unsigned int updAttrCnt;
    unsigned int startQual;
} i6c_venc_strminfo_h265;

typedef struct {
    unsigned int size;
    unsigned int updAttrCnt;
    unsigned int quality;
} i6c_venc_strminfo_mjpg;

typedef struct {
    i6c_venc_pack *packet;
    unsigned int count;
    unsigned int sequence;
    unsigned long handle;
    union {
        i6c_venc_strminfo_h264 h264Info;
        i6c_venc_strminfo_mjpg mjpgInfo;
        i6c_venc_strminfo_h265 h265Info;
    };
} i6c_venc_strm;

#endif /* SIGMASTAR_TYPES_H */
