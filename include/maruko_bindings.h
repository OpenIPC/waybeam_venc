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

/* ISP _impl struct is now in maruko_mi.h (maruko_isp_impl) */

typedef struct {
	i6_common_rect crop;
	i6_common_dim output;
	char mirror;
	char flip;
	i6_common_pixfmt pixFmt;
	i6_common_compr compress;
} i6c_scl_port;

/* SCL _impl struct is now in maruko_mi.h (maruko_scl_impl) */

typedef int (*maruko_isp_load_bin_fn_t)(MI_U32 dev_id, MI_U32 channel, char *path, MI_U32 key);
typedef int (*maruko_isp_disable_userspace3a_fn_t)(MI_U32 dev_id, MI_U32 channel);

/* Maruko VENC/SYS dispatch — routed through dlopen'd function pointers */
#define maruko_mi_venc_create_dev(dev, init)    g_mi_venc.fnCreateDev((dev), (init))
#define maruko_mi_venc_destroy_dev(dev)         g_mi_venc.fnDestroyDev((dev))
#define maruko_mi_venc_create_chn(dev, chn, attr) g_mi_venc.fnCreateChn((dev), (chn), (attr))
#define maruko_mi_venc_destroy_chn(dev, chn)    g_mi_venc.fnDestroyChn((dev), (chn))
#define maruko_mi_venc_start_recv(dev, chn)     g_mi_venc.fnStartRecvPic((dev), (chn))
#define maruko_mi_venc_stop_recv(dev, chn)      g_mi_venc.fnStopRecvPic((dev), (chn))
#define maruko_mi_venc_query(dev, chn, stat)    g_mi_venc.fnQuery((dev), (chn), (stat))
#define maruko_mi_venc_get_stream(dev, chn, strm, ms) \
  g_mi_venc.fnGetStream((dev), (chn), (strm), (ms))
#define maruko_mi_venc_release_stream(dev, chn, strm) \
  g_mi_venc.fnReleaseStream((dev), (chn), (strm))
#define maruko_mi_venc_set_input_source(dev, chn, cfg) \
  g_mi_venc.fnSetInputSourceConfig((dev), (chn), (cfg))
#define maruko_mi_venc_get_chn_attr(dev, chn, attr) \
  g_mi_venc.fnGetChnAttr((dev), (chn), (attr))
#define maruko_mi_venc_set_chn_attr(dev, chn, attr) \
  g_mi_venc.fnSetChnAttr((dev), (chn), (attr))
#define maruko_mi_venc_get_rc_param(dev, chn, param) \
  g_mi_venc.fnGetRcParam((dev), (chn), (param))
#define maruko_mi_venc_set_rc_param(dev, chn, param) \
  g_mi_venc.fnSetRcParam((dev), (chn), (param))
#define maruko_mi_venc_request_idr(dev, chn, inst) \
  g_mi_venc.fnRequestIdr((dev), (chn), (inst))
#define maruko_mi_venc_set_frame_lost(dev, chn, strat) \
  g_mi_venc.fnSetFrameLostStrategy((dev), (chn), (strat))
#define maruko_mi_venc_get_frame_lost(dev, chn, strat) \
  g_mi_venc.fnGetFrameLostStrategy((dev), (chn), (strat))
#define maruko_mi_sys_config_private_pool(soc, cfg) \
  g_mi_sys.fnConfigPrivateMMAPool((soc), (cfg))

/* Maruko (i6c) uses the UBR rate-mode enum layout — the standard
 * I6C_VENC_RATEMODE_* values (H265CBR=9) are rejected by MI_VENC_CreateChn.
 * The UBR layout inserts extra UBR slots for H264/H265, shifting all H265
 * modes up by 1.  These are the values the firmware actually accepts. */
enum {
	MARUKO_VENC_RC_H264_CBR  = 1,   /* UBR_H264CBR  */
	MARUKO_VENC_RC_H264_VBR  = 2,   /* UBR_H264VBR  */
	MARUKO_VENC_RC_H264_AVBR = 6,   /* UBR_H264AVBR */
	MARUKO_VENC_RC_H265_CBR  = 10,  /* UBR_H265CBR  */
	MARUKO_VENC_RC_H265_VBR  = 11,  /* UBR_H265VBR  */
	MARUKO_VENC_RC_H265_AVBR = 14,  /* UBR_H265AVBR */
};

#endif
