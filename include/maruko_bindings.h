#ifndef MARUKO_BINDINGS_H
#define MARUKO_BINDINGS_H

#include "star6e.h"

typedef struct {
	unsigned int rev;
	unsigned int size;
	unsigned char data[64];
} i6c_isp_iqver;

typedef struct {
	unsigned int sensorId;
	i6c_isp_iqver iqVer;
	unsigned int sync3A;
} i6c_isp_chn;

typedef struct {
	i6_common_hdr hdr;
	int level3DNR;
	char mirror;
	char flip;
	int rotate;
	char yuv2BayerOn;
} i6c_isp_para;

typedef struct {
	i6_common_rect crop;
	i6_common_pixfmt pixFmt;
	i6_common_compr compress;
	char multiPlanes;
} i6c_isp_port;

typedef struct {
	void *handle;
	int (*fnCreateDevice)(int device, unsigned int *combo);
	int (*fnDestroyDevice)(int device);
	int (*fnCreateChannel)(int device, int channel, i6c_isp_chn *config);
	int (*fnDestroyChannel)(int device, int channel);
	int (*fnSetChannelParam)(int device, int channel, i6c_isp_para *config);
	int (*fnStartChannel)(int device, int channel);
	int (*fnStopChannel)(int device, int channel);
	int (*fnDisablePort)(int device, int channel, int port);
	int (*fnEnablePort)(int device, int channel, int port);
	int (*fnSetPortConfig)(int device, int channel, int port, i6c_isp_port *config);
} i6c_isp_impl;

typedef struct {
	i6_common_rect crop;
	i6_common_dim output;
	char mirror;
	char flip;
	i6_common_pixfmt pixFmt;
	i6_common_compr compress;
} i6c_scl_port;

typedef struct {
	void *handle;
	int (*fnCreateDevice)(int device, unsigned int *binds);
	int (*fnDestroyDevice)(int device);
	int (*fnAdjustChannelRotation)(int device, int channel, int *rotate);
	int (*fnCreateChannel)(int device, int channel, unsigned int *reserved);
	int (*fnDestroyChannel)(int device, int channel);
	int (*fnStartChannel)(int device, int channel);
	int (*fnStopChannel)(int device, int channel);
	int (*fnDisablePort)(int device, int channel, int port);
	int (*fnEnablePort)(int device, int channel, int port);
	int (*fnSetPortConfig)(int device, int channel, int port, i6c_scl_port *config);
} i6c_scl_impl;

typedef int (*maruko_isp_load_bin_fn_t)(MI_U32 dev_id, MI_U32 channel, char *path, MI_U32 key);
typedef int (*maruko_isp_disable_userspace3a_fn_t)(MI_U32 dev_id, MI_U32 channel);

MI_S32 maruko_mi_venc_create_dev(MI_VENC_DEV dev, i6c_venc_init *init)
	__asm__("MI_VENC_CreateDev");
MI_S32 maruko_mi_venc_destroy_dev(MI_VENC_DEV dev) __asm__("MI_VENC_DestroyDev");
MI_S32 maruko_mi_venc_create_chn(MI_VENC_DEV dev, MI_VENC_CHN chn, i6c_venc_chn *attr)
	__asm__("MI_VENC_CreateChn");
MI_S32 maruko_mi_venc_destroy_chn(MI_VENC_DEV dev, MI_VENC_CHN chn)
	__asm__("MI_VENC_DestroyChn");
MI_S32 maruko_mi_venc_start_recv(MI_VENC_DEV dev, MI_VENC_CHN chn)
	__asm__("MI_VENC_StartRecvPic");
MI_S32 maruko_mi_venc_stop_recv(MI_VENC_DEV dev, MI_VENC_CHN chn)
	__asm__("MI_VENC_StopRecvPic");
MI_S32 maruko_mi_venc_query(MI_VENC_DEV dev, MI_VENC_CHN chn, i6c_venc_stat *stat)
	__asm__("MI_VENC_Query");
MI_S32 maruko_mi_venc_get_stream(MI_VENC_DEV dev, MI_VENC_CHN chn,
	i6c_venc_strm *stream, MI_S32 timeout_ms) __asm__("MI_VENC_GetStream");
MI_S32 maruko_mi_venc_release_stream(MI_VENC_DEV dev, MI_VENC_CHN chn,
	i6c_venc_strm *stream) __asm__("MI_VENC_ReleaseStream");
MI_S32 maruko_mi_venc_set_input_source(MI_VENC_DEV dev, MI_VENC_CHN chn,
	i6c_venc_src_conf *config) __asm__("MI_VENC_SetInputSourceConfig");
MI_S32 maruko_mi_venc_get_chn_attr(MI_VENC_DEV dev, MI_VENC_CHN chn, i6c_venc_chn *attr)
	__asm__("MI_VENC_GetChnAttr");
MI_S32 maruko_mi_venc_set_chn_attr(MI_VENC_DEV dev, MI_VENC_CHN chn, i6c_venc_chn *attr)
	__asm__("MI_VENC_SetChnAttr");
MI_S32 maruko_mi_venc_get_rc_param(MI_VENC_DEV dev, MI_VENC_CHN chn,
	MI_VENC_RcParam_t *param) __asm__("MI_VENC_GetRcParam");
MI_S32 maruko_mi_venc_set_rc_param(MI_VENC_DEV dev, MI_VENC_CHN chn,
	MI_VENC_RcParam_t *param) __asm__("MI_VENC_SetRcParam");
MI_S32 maruko_mi_venc_request_idr(MI_VENC_DEV dev, MI_VENC_CHN chn, MI_BOOL instant)
	__asm__("MI_VENC_RequestIdr");
MI_S32 maruko_mi_venc_set_frame_lost_strategy(MI_VENC_DEV dev, MI_VENC_CHN chn,
	MI_VENC_ParamFrameLost_t *p) __asm__("MI_VENC_SetFrameLostStrategy");
MI_S32 maruko_mi_sys_config_private_pool(MI_U16 soc_id, i6c_sys_pool *config)
	__asm__("MI_SYS_ConfigPrivateMMAPool");

/* Maruko (i6c) uses the UBR rate mode enum layout — the standard
 * I6C_VENC_RATEMODE_* values (H265CBR=9) are rejected by the firmware.
 * The UBR layout inserts an extra UBR slot for H264/H265, shifting
 * all H265 modes up by 1.  These are the values MI_VENC_CreateChn
 * actually accepts. */
enum {
	MARUKO_RC_H264_CBR  = 1,   /* I6C_VENC_RATEMODE_UBR_H264CBR  */
	MARUKO_RC_H264_VBR  = 2,   /* I6C_VENC_RATEMODE_UBR_H264VBR  */
	MARUKO_RC_H264_AVBR = 6,   /* I6C_VENC_RATEMODE_UBR_H264AVBR */
	MARUKO_RC_H265_CBR  = 10,  /* I6C_VENC_RATEMODE_UBR_H265CBR  */
	MARUKO_RC_H265_VBR  = 11,  /* I6C_VENC_RATEMODE_UBR_H265VBR  */
	MARUKO_RC_H265_AVBR = 14,  /* I6C_VENC_RATEMODE_UBR_H265AVBR */
};

#endif
