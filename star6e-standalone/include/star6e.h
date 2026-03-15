#ifndef STAR6E_PLATFORM_H
#define STAR6E_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SigmaStar MI API type definitions — self-contained, no HAL dependency. */
#include "sigmastar_types.h"

typedef int32_t MI_S32;
typedef uint32_t MI_U32;
typedef uint16_t MI_U16;
typedef uint8_t MI_U8;
typedef uint64_t MI_U64;
typedef bool MI_BOOL;

/* Common SigmaStar handles */
typedef i6_sys_bind MI_SYS_ChnPort_t;

enum {
  E_MI_MODULE_ID_VIF  = I6_SYS_MOD_VIF,
  E_MI_MODULE_ID_VPE  = I6_SYS_MOD_VPE,
  E_MI_MODULE_ID_VENC = I6_SYS_MOD_VENC,
};

typedef i6_snr_pad   MI_SNR_PadInfo_t;
typedef i6_snr_plane MI_SNR_PlaneInfo_t;
typedef i6_snr_res   MI_SNR_Res_t;

#if defined(PLATFORM_MARUKO)
typedef enum {
  E_MI_VIF_MODE_BT656 = I6_INTF_BT656,
  E_MI_VIF_MODE_DIGITAL_CAMERA = I6_INTF_DIGITAL_CAMERA,
  E_MI_VIF_MODE_BT1120_STANDARD = I6_INTF_BT1120_STANDARD,
  E_MI_VIF_MODE_BT1120_INTERLEAVED = I6_INTF_BT1120_INTERLEAVED,
  E_MI_VIF_MODE_MIPI = I6_INTF_MIPI,
  E_MI_VIF_MODE_LVDS = I6_INTF_END,
  E_MI_VIF_MODE_MAX,
} MI_VIF_IntfMode_e;

typedef enum {
  E_MI_VIF_WORK_MODE_1MULTIPLEX = I6_VIF_WORK_1MULTIPLEX,
  E_MI_VIF_WORK_MODE_2MULTIPLEX = I6_VIF_WORK_2MULTIPLEX,
  E_MI_VIF_WORK_MODE_4MULTIPLEX = I6_VIF_WORK_4MULTIPLEX,
  E_MI_VIF_WORK_MODE_MAX,
} MI_VIF_WorkMode_e;

typedef enum {
  E_MI_VIF_HDR_TYPE_OFF = I6_HDR_OFF,
  E_MI_VIF_HDR_TYPE_VC = I6_HDR_VC,
  E_MI_VIF_HDR_TYPE_DOL = I6_HDR_DOL,
  E_MI_VIF_HDR_TYPE_EMBEDDED = I6_HDR_EMBED,
  E_MI_VIF_HDR_TYPE_LI = I6_HDR_LI,
  E_MI_VIF_HDR_TYPE_MAX,
} MI_VIF_HDRType_e;

typedef enum {
  E_MI_VIF_CLK_EDGE_SINGLE_UP = I6_EDGE_SINGLE_UP,
  E_MI_VIF_CLK_EDGE_SINGLE_DOWN = I6_EDGE_SINGLE_DOWN,
  E_MI_VIF_CLK_EDGE_DOUBLE = I6_EDGE_DOUBLE,
  E_MI_VIF_CLK_EDGE_MAX,
} MI_VIF_ClkEdge_e;

typedef enum {
  E_MI_VIF_FRAMERATE_FULL = I6_VIF_FRATE_FULL,
  E_MI_VIF_FRAMERATE_HALF = I6_VIF_FRATE_HALF,
  E_MI_VIF_FRAMERATE_QUARTER = I6_VIF_FRATE_QUART,
  E_MI_VIF_FRAMERATE_OCTANT = I6_VIF_FRATE_OCTANT,
  E_MI_VIF_FRAMERATE_THREE_QUARTERS = I6_VIF_FRATE_3QUARTS,
  E_MI_VIF_FRAMERATE_MAX,
} MI_VIF_FrameRate_e;

enum {
  E_MI_VIF_GROUPMASK_ID0 = 0x0001,
};

enum {
  E_MI_SYS_COMPRESS_MODE_NONE = I6_COMPR_NONE,
};

typedef MI_U32 MI_VIF_GROUP;

typedef struct {
  MI_VIF_IntfMode_e eIntfMode;
  MI_VIF_WorkMode_e eWorkMode;
  MI_VIF_HDRType_e eHDRType;
  MI_VIF_ClkEdge_e eClkEdge;
  MI_U32 eMclk;
  MI_U32 eScanMode;
  MI_U32 u32GroupStitchMask;
} MI_VIF_GroupAttr_t;

typedef struct {
  i6_common_pixfmt eInputPixel;
  i6_common_rect stInputRect;
  MI_U32 eField;
  MI_BOOL bEnH2T1PMode;
} MI_VIF_DevAttr_t;

typedef struct {
  i6_common_rect stCapRect;
  i6_common_dim stDestSize;
  i6_common_pixfmt ePixFormat;
  MI_VIF_FrameRate_e eFrameRate;
  MI_U32 eCompressMode;
} MI_VIF_OutputPortAttr_t;

typedef MI_VIF_OutputPortAttr_t MI_VIF_PortAttr_t;
#else
typedef i6_vif_dev   MI_VIF_DevAttr_t;
typedef i6_vif_port  MI_VIF_PortAttr_t;
#endif

/* infinity6e (Star6E) driver expects the larger i6e_ struct variants which
   include additional lens-correction fields.  Using the smaller i6_ structs
   causes the driver to read past the struct boundary into stack garbage,
   corrupting VPE init and breaking the VIF→VPE REALTIME link. */
#if defined(PLATFORM_STAR6E)
typedef i6e_vpe_chn  MI_VPE_ChannelAttr_t;
typedef i6e_vpe_para MI_VPE_ChannelParam_t;
#else
typedef i6_vpe_chn   MI_VPE_ChannelAttr_t;
typedef i6_vpe_para  MI_VPE_ChannelParam_t;
#endif
typedef i6_vpe_port  MI_VPE_PortAttr_t;

typedef i6_venc_chn   MI_VENC_ChnAttr_t;
typedef i6_venc_pack  MI_VENC_Pack_t;
typedef i6_venc_stat  MI_VENC_Stat_t;
typedef i6_venc_strm  MI_VENC_Stream_t;

#if defined(PLATFORM_MARUKO)
typedef MI_U32 MI_VIF_DEV;
typedef MI_U32 MI_VIF_CHN;
typedef MI_U32 MI_VIF_PORT;
#else
typedef int MI_VIF_DEV;
typedef int MI_VIF_CHN;
typedef int MI_VIF_PORT;
#endif

typedef int MI_VPE_CHANNEL;
typedef int MI_VPE_PORT;

typedef int MI_VENC_CHN;
typedef int MI_VENC_DEV;

/* MI_SYS ------------------------------------------------------------------ */
#if defined(PLATFORM_MARUKO)
MI_S32 MI_SYS_Init_MARUKO(MI_U16 soc_id) __asm__("MI_SYS_Init");
MI_S32 MI_SYS_Exit_MARUKO(MI_U16 soc_id) __asm__("MI_SYS_Exit");
MI_S32 MI_SYS_BindChnPort_MARUKO(MI_U16 soc_id, MI_SYS_ChnPort_t* src, MI_SYS_ChnPort_t* dst,
  MI_U32 src_fps, MI_U32 dst_fps, MI_U32 link_type, MI_U32 link_param) __asm__("MI_SYS_BindChnPort2");
MI_S32 MI_SYS_UnBindChnPort_MARUKO(MI_U16 soc_id, MI_SYS_ChnPort_t* src, MI_SYS_ChnPort_t* dst)
  __asm__("MI_SYS_UnBindChnPort");
MI_S32 MI_SYS_SetChnOutputPortDepth_MARUKO(MI_U16 soc_id, MI_SYS_ChnPort_t* chn_port,
  MI_U32 user_depth, MI_U32 buf_depth) __asm__("MI_SYS_SetChnOutputPortDepth");

#define MI_SYS_Init() MI_SYS_Init_MARUKO(0)
#define MI_SYS_Exit() MI_SYS_Exit_MARUKO(0)
#define MI_SYS_BindChnPort(src, dst, src_fps, dst_fps) \
  MI_SYS_BindChnPort_MARUKO(0, (MI_SYS_ChnPort_t*)(src), (MI_SYS_ChnPort_t*)(dst), \
    (src_fps), (dst_fps), 0, 0)
#define MI_SYS_UnBindChnPort(src, dst) \
  MI_SYS_UnBindChnPort_MARUKO(0, (MI_SYS_ChnPort_t*)(src), (MI_SYS_ChnPort_t*)(dst))
#define MI_SYS_BindChnPort2(src, dst, src_fps, dst_fps, link_type, link_param) \
  MI_SYS_BindChnPort_MARUKO(0, (MI_SYS_ChnPort_t*)(src), (MI_SYS_ChnPort_t*)(dst), \
    (src_fps), (dst_fps), (link_type), (link_param))
#define MI_SYS_SetChnOutputPortDepth(chn_port, user_depth, buf_depth) \
  MI_SYS_SetChnOutputPortDepth_MARUKO(0, (MI_SYS_ChnPort_t*)(chn_port), (user_depth), (buf_depth))
#else
MI_S32 MI_SYS_Init(void);
MI_S32 MI_SYS_Exit(void);
MI_S32 MI_SYS_SetChnOutputPortDepth(const MI_SYS_ChnPort_t* chn_port,
  MI_U32 user_depth, MI_U32 buf_depth);
MI_S32 MI_SYS_BindChnPort(const MI_SYS_ChnPort_t* src, const MI_SYS_ChnPort_t* dst,
  MI_U32 src_fps, MI_U32 dst_fps);
MI_S32 MI_SYS_UnBindChnPort(const MI_SYS_ChnPort_t* src, const MI_SYS_ChnPort_t* dst);
MI_S32 MI_SYS_BindChnPort2(const MI_SYS_ChnPort_t* src, const MI_SYS_ChnPort_t* dst,
  MI_U32 src_fps, MI_U32 dst_fps, MI_U32 link_type, MI_U32 link_param);
#endif

/* MI_SNR ------------------------------------------------------------------ */
typedef enum {
  E_MI_SNR_PAD_ID_0 = 0,
  E_MI_SNR_PAD_ID_1 = 1,
  E_MI_SNR_PAD_ID_2 = 2,
  E_MI_SNR_PAD_ID_3 = 3,
} MI_SNR_PAD_ID_e;

typedef enum {
  E_MI_SNR_PLANE_MODE_LINEAR = 0,
} MI_SNR_PlaneMode_e;

typedef enum {
  E_MI_SNR_CUSTDATA_TO_DRIVER = 0,
  E_MI_SNR_CUSTDATA_TO_USER = 1,
} MI_SNR_CustDir_e;

typedef struct {
  MI_U32 u32DevId;
  MI_U8* u8Data;
} MI_SNR_InitParam_t;

MI_S32 MI_SNR_InitDev(MI_SNR_InitParam_t* init);
MI_S32 MI_SNR_DeInitDev(void);
MI_S32 MI_SNR_SetPlaneMode(MI_SNR_PAD_ID_e pad_id, MI_SNR_PlaneMode_e mode);
MI_S32 MI_SNR_GetPlaneMode(MI_SNR_PAD_ID_e pad_id, MI_BOOL* mode);
MI_S32 MI_SNR_SetRes(MI_SNR_PAD_ID_e pad_id, MI_U32 res_idx);
MI_S32 MI_SNR_GetCurRes(MI_SNR_PAD_ID_e pad_id, MI_U8* res_idx, MI_SNR_Res_t* res);
MI_S32 MI_SNR_SetFps(MI_SNR_PAD_ID_e pad_id, MI_U32 fps);
MI_S32 MI_SNR_GetFps(MI_SNR_PAD_ID_e pad_id, MI_U32* fps);
MI_S32 MI_SNR_SetOrien(MI_SNR_PAD_ID_e pad_id, MI_U8 mirror, MI_U8 flip);
MI_S32 MI_SNR_CustFunction(MI_SNR_PAD_ID_e pad_id, MI_U32 cmd_id,
  MI_U32 data_size, void* data, MI_SNR_CustDir_e dir);
MI_S32 MI_SNR_QueryResCount(MI_SNR_PAD_ID_e pad_id, MI_U32* count);
MI_S32 MI_SNR_GetRes(MI_SNR_PAD_ID_e pad_id, MI_U32 res_idx, MI_SNR_Res_t* res);
MI_S32 MI_SNR_GetPadInfo(MI_SNR_PAD_ID_e pad_id, MI_SNR_PadInfo_t* info);
MI_S32 MI_SNR_GetPlaneInfo(MI_SNR_PAD_ID_e pad_id, MI_U32 plane_idx, MI_SNR_PlaneInfo_t* info);
MI_S32 MI_SNR_Enable(MI_SNR_PAD_ID_e pad_id);
MI_S32 MI_SNR_Disable(MI_SNR_PAD_ID_e pad_id);

/* MI_VIF ------------------------------------------------------------------ */
MI_S32 MI_VIF_SetDevAttr(MI_VIF_DEV dev, MI_VIF_DevAttr_t* attr);
MI_S32 MI_VIF_EnableDev(MI_VIF_DEV dev);
MI_S32 MI_VIF_DisableDev(MI_VIF_DEV dev);

#if defined(PLATFORM_MARUKO)
MI_S32 MI_VIF_CreateDevGroup(MI_VIF_GROUP group, MI_VIF_GroupAttr_t* attr);
MI_S32 MI_VIF_DestroyDevGroup(MI_VIF_GROUP group);
MI_S32 MI_VIF_SetOutputPortAttr(MI_VIF_DEV dev, MI_VIF_PORT port, MI_VIF_OutputPortAttr_t* attr);
MI_S32 MI_VIF_EnableOutputPort(MI_VIF_DEV dev, MI_VIF_PORT port);
MI_S32 MI_VIF_DisableOutputPort(MI_VIF_DEV dev, MI_VIF_PORT port);

#define MI_VIF_SetChnPortAttr(chn, port, attr) \
  MI_VIF_SetOutputPortAttr((MI_VIF_DEV)(chn), (MI_VIF_PORT)(port), (MI_VIF_OutputPortAttr_t*)(attr))
#define MI_VIF_EnableChnPort(chn, port) \
  MI_VIF_EnableOutputPort((MI_VIF_DEV)(chn), (MI_VIF_PORT)(port))
#define MI_VIF_DisableChnPort(chn, port) \
  MI_VIF_DisableOutputPort((MI_VIF_DEV)(chn), (MI_VIF_PORT)(port))
#else
MI_S32 MI_VIF_SetChnPortAttr(MI_VIF_CHN chn, MI_VIF_PORT port, MI_VIF_PortAttr_t* attr);
MI_S32 MI_VIF_EnableChnPort(MI_VIF_CHN chn, MI_VIF_PORT port);
MI_S32 MI_VIF_DisableChnPort(MI_VIF_CHN chn, MI_VIF_PORT port);
#endif

/* MI_VPE ------------------------------------------------------------------ */
MI_S32 MI_VPE_CreateChannel(MI_VPE_CHANNEL chn, MI_VPE_ChannelAttr_t* attr);
MI_S32 MI_VPE_SetChannelAttr(MI_VPE_CHANNEL chn, MI_VPE_ChannelAttr_t* attr);
MI_S32 MI_VPE_DestroyChannel(MI_VPE_CHANNEL chn);
MI_S32 MI_VPE_StartChannel(MI_VPE_CHANNEL chn);
MI_S32 MI_VPE_StopChannel(MI_VPE_CHANNEL chn);
MI_S32 MI_VPE_SetChannelParam(MI_VPE_CHANNEL chn, MI_VPE_ChannelParam_t* param);
MI_S32 MI_VPE_SetPortMode(MI_VPE_CHANNEL chn, MI_VPE_PORT port, MI_VPE_PortAttr_t* attr);
MI_S32 MI_VPE_EnablePort(MI_VPE_CHANNEL chn, MI_VPE_PORT port);
MI_S32 MI_VPE_DisablePort(MI_VPE_CHANNEL chn, MI_VPE_PORT port);
MI_S32 MI_VPE_SetPortCrop(MI_VPE_CHANNEL chn, MI_VPE_PORT port,
	i6_common_rect *crop);

/* MI_VENC ROI -------------------------------------------------------------- */
typedef struct {
  MI_U32 u32Left;
  MI_U32 u32Top;
  MI_U32 u32Width;
  MI_U32 u32Height;
} MI_VENC_Rect_t;

typedef struct {
  MI_U32  u32Index;   /* ROI region index: 0-7 */
  MI_BOOL bEnable;
  MI_BOOL bAbsQp;     /* true=absolute QP, false=delta QP */
  MI_S32  s32Qp;      /* QP value (absolute) or QP offset (delta) */
  MI_VENC_Rect_t stRect;
} MI_VENC_RoiCfg_t;

#define RC_TEXTURE_THR_SIZE 1

typedef struct {
	MI_U32 u32MaxQp;
	MI_U32 u32MinQp;
	MI_S32 s32IPQPDelta;
	MI_U32 u32MaxIQp;
	MI_U32 u32MinIQp;
	MI_U32 u32MaxIPProp;
	MI_U32 u32MaxISize;
	MI_U32 u32MaxPSize;
} MI_VENC_ParamH264Cbr_t;

typedef struct {
	MI_S32 s32IPQPDelta;
	MI_S32 s32ChangePos;
	MI_U32 u32MaxIQp;
	MI_U32 u32MinIQP;
	MI_U32 u32MaxIPProp;
	MI_U32 u32MaxISize;
	MI_U32 u32MaxPSize;
} MI_VENC_ParamH264Vbr_t;

typedef struct {
	MI_S32 s32IPQPDelta;
	MI_S32 s32ChangePos;
	MI_U32 u32MinIQp;
	MI_U32 u32MaxIPProp;
	MI_U32 u32MaxIQp;
	MI_U32 u32MaxISize;
	MI_U32 u32MaxPSize;
	MI_U32 u32MinStillPercent;
	MI_U32 u32MaxStillQp;
} MI_VENC_ParamH264Avbr_t;

typedef struct {
	MI_U32 u32MaxQp;
	MI_U32 u32MinQp;
	MI_S32 s32IPQPDelta;
	MI_U32 u32MaxIQp;
	MI_U32 u32MinIQp;
	MI_U32 u32MaxIPProp;
	MI_U32 u32MaxISize;
	MI_U32 u32MaxPSize;
} MI_VENC_ParamH265Cbr_t;

typedef struct {
	MI_S32 s32IPQPDelta;
	MI_S32 s32ChangePos;
	MI_U32 u32MaxIQp;
	MI_U32 u32MinIQP;
	MI_U32 u32MaxIPProp;
	MI_U32 u32MaxISize;
	MI_U32 u32MaxPSize;
} MI_VENC_ParamH265Vbr_t;

typedef struct {
	MI_S32 s32IPQPDelta;
	MI_S32 s32ChangePos;
	MI_U32 u32MinIQp;
	MI_U32 u32MaxIPProp;
	MI_U32 u32MaxIQp;
	MI_U32 u32MaxISize;
	MI_U32 u32MaxPSize;
	MI_U32 u32MinStillPercent;
	MI_U32 u32MaxStillQp;
} MI_VENC_ParamH265Avbr_t;

typedef struct {
	MI_U32 u32MaxQfactor;
	MI_U32 u32MinQfactor;
} MI_VENC_ParamMjpegCbr_t;

typedef struct {
	MI_U32 u32ThrdI[RC_TEXTURE_THR_SIZE];
	MI_U32 u32ThrdP[RC_TEXTURE_THR_SIZE];
	MI_U32 u32RowQpDelta;
	union {
		MI_VENC_ParamH264Cbr_t stParamH264Cbr;
		MI_VENC_ParamH264Vbr_t stParamH264VBR;
		MI_VENC_ParamH264Avbr_t stParamH264Avbr;
		MI_VENC_ParamMjpegCbr_t stParamMjpegCbr;
		MI_VENC_ParamH265Cbr_t stParamH265Cbr;
		MI_VENC_ParamH265Vbr_t stParamH265Vbr;
		MI_VENC_ParamH265Avbr_t stParamH265Avbr;
	};
	void *pRcParam;
} MI_VENC_RcParam_t;

/* MI_VENC ----------------------------------------------------------------- */
#if defined(PLATFORM_MARUKO)
MI_S32 MI_VENC_CreateChn_MARUKO(MI_VENC_DEV dev, MI_VENC_CHN chn, MI_VENC_ChnAttr_t* attr)
  __asm__("MI_VENC_CreateChn");
MI_S32 MI_VENC_DestroyChn_MARUKO(MI_VENC_DEV dev, MI_VENC_CHN chn) __asm__("MI_VENC_DestroyChn");
MI_S32 MI_VENC_StartRecvPic_MARUKO(MI_VENC_DEV dev, MI_VENC_CHN chn) __asm__("MI_VENC_StartRecvPic");
MI_S32 MI_VENC_StopRecvPic_MARUKO(MI_VENC_DEV dev, MI_VENC_CHN chn) __asm__("MI_VENC_StopRecvPic");
MI_S32 MI_VENC_GetStream_MARUKO(MI_VENC_DEV dev, MI_VENC_CHN chn, MI_VENC_Stream_t* stream, MI_S32 timeout_ms)
  __asm__("MI_VENC_GetStream");
MI_S32 MI_VENC_ReleaseStream_MARUKO(MI_VENC_DEV dev, MI_VENC_CHN chn, MI_VENC_Stream_t* stream)
  __asm__("MI_VENC_ReleaseStream");
MI_S32 MI_VENC_Query_MARUKO(MI_VENC_DEV dev, MI_VENC_CHN chn, MI_VENC_Stat_t* stat) __asm__("MI_VENC_Query");
int MI_VENC_GetFd_MARUKO(MI_VENC_DEV dev, MI_VENC_CHN chn) __asm__("MI_VENC_GetFd");
MI_S32 MI_VENC_CloseFd_MARUKO(MI_VENC_DEV dev, MI_VENC_CHN chn) __asm__("MI_VENC_CloseFd");
MI_S32 MI_VENC_GetChnAttr_MARUKO(MI_VENC_DEV dev, MI_VENC_CHN chn, MI_VENC_ChnAttr_t* attr)
  __asm__("MI_VENC_GetChnAttr");
MI_S32 MI_VENC_SetChnAttr_MARUKO(MI_VENC_DEV dev, MI_VENC_CHN chn, MI_VENC_ChnAttr_t* attr)
  __asm__("MI_VENC_SetChnAttr");
MI_S32 MI_VENC_RequestIdr_MARUKO(MI_VENC_DEV dev, MI_VENC_CHN chn, MI_BOOL instant)
  __asm__("MI_VENC_RequestIdr");

#define MI_VENC_CreateChn(chn, attr) MI_VENC_CreateChn_MARUKO(0, (chn), (attr))
#define MI_VENC_DestroyChn(chn) MI_VENC_DestroyChn_MARUKO(0, (chn))
#define MI_VENC_StartRecvPic(chn) MI_VENC_StartRecvPic_MARUKO(0, (chn))
#define MI_VENC_StopRecvPic(chn) MI_VENC_StopRecvPic_MARUKO(0, (chn))
#define MI_VENC_GetStream(chn, stream, timeout_ms) MI_VENC_GetStream_MARUKO(0, (chn), (stream), (timeout_ms))
#define MI_VENC_ReleaseStream(chn, stream) MI_VENC_ReleaseStream_MARUKO(0, (chn), (stream))
#define MI_VENC_Query(chn, stat) MI_VENC_Query_MARUKO(0, (chn), (stat))
#define MI_VENC_GetFd(chn) MI_VENC_GetFd_MARUKO(0, (chn))
#define MI_VENC_CloseFd(chn) MI_VENC_CloseFd_MARUKO(0, (chn))
#define MI_VENC_GetChnAttr(chn, attr) MI_VENC_GetChnAttr_MARUKO(0, (chn), (attr))
#define MI_VENC_SetChnAttr(chn, attr) MI_VENC_SetChnAttr_MARUKO(0, (chn), (attr))
MI_S32 MI_VENC_SetRoiCfg_MARUKO(MI_VENC_DEV dev, MI_VENC_CHN chn, MI_VENC_RoiCfg_t *cfg)
  __asm__("MI_VENC_SetRoiCfg");
MI_S32 MI_VENC_GetRoiCfg_MARUKO(MI_VENC_DEV dev, MI_VENC_CHN chn, MI_U32 idx, MI_VENC_RoiCfg_t *cfg)
  __asm__("MI_VENC_GetRoiCfg");

#define MI_VENC_RequestIdr(chn, instant) MI_VENC_RequestIdr_MARUKO(0, (chn), (instant))
#define MI_VENC_SetRoiCfg(chn, cfg) MI_VENC_SetRoiCfg_MARUKO(0, (chn), (cfg))
#define MI_VENC_GetRoiCfg(chn, idx, cfg) MI_VENC_GetRoiCfg_MARUKO(0, (chn), (idx), (cfg))
#else
MI_S32 MI_VENC_CreateChn(MI_VENC_CHN chn, MI_VENC_ChnAttr_t* attr);
MI_S32 MI_VENC_DestroyChn(MI_VENC_CHN chn);
MI_S32 MI_VENC_StartRecvPic(MI_VENC_CHN chn);
MI_S32 MI_VENC_StopRecvPic(MI_VENC_CHN chn);
MI_S32 MI_VENC_GetStream(MI_VENC_CHN chn, MI_VENC_Stream_t* stream, MI_S32 timeout_ms);
MI_S32 MI_VENC_ReleaseStream(MI_VENC_CHN chn, MI_VENC_Stream_t* stream);
MI_S32 MI_VENC_Query(MI_VENC_CHN chn, MI_VENC_Stat_t* stat);
int    MI_VENC_GetFd(MI_VENC_CHN chn);
MI_S32 MI_VENC_CloseFd(MI_VENC_CHN chn);
MI_S32 MI_VENC_GetChnAttr(MI_VENC_CHN chn, MI_VENC_ChnAttr_t* attr);
MI_S32 MI_VENC_SetChnAttr(MI_VENC_CHN chn, MI_VENC_ChnAttr_t* attr);
MI_S32 MI_VENC_GetRcParam(MI_VENC_CHN chn, MI_VENC_RcParam_t *param);
MI_S32 MI_VENC_SetRcParam(MI_VENC_CHN chn, MI_VENC_RcParam_t *param);
MI_S32 MI_VENC_RequestIdr(MI_VENC_CHN chn, MI_BOOL instant);
MI_S32 MI_VENC_SetRoiCfg(MI_VENC_CHN chn, MI_VENC_RoiCfg_t *cfg);
MI_S32 MI_VENC_GetRoiCfg(MI_VENC_CHN chn, MI_U32 idx, MI_VENC_RoiCfg_t *cfg);
#if !defined(PLATFORM_MARUKO)
MI_S32 MI_VENC_GetChnDevid(MI_VENC_CHN chn, MI_U32* device_id);
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif
