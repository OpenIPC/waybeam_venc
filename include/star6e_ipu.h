#ifndef STAR6E_IPU_H
#define STAR6E_IPU_H

/*
 * Star6E IPU (NPU) dynamic loader.
 *
 * Loads the SigmaStar Infinity6E IPU vendor library (libmi_ipu.so) via
 * dlopen at runtime, matching the star6e_mi.h binding idiom (function-
 * pointer table + global instance).  Only compiled into detection builds
 * (DETECT=1) and into the tools/ipu_probe bring-up tool.
 *
 * The tensor/attr structs are the SigmaStar MI_IPU ABI, laid out to match
 * libmi_ipu.so.  Field order is copied from the in-repo reference header
 * sdk/ssc338q/include/i6_ipu.h — keep them in sync if a SDK refresh changes
 * the layout (same discipline as include/maruko_ai_types.h).
 */

#define IPU_MAX_TENSORS      61
#define IPU_TENSOR_NAME_LEN  256

/* Model-byte reader callback.  The SDK calls it to pull firmware/network
 * bytes; `ctx` is the path forwarded from CreateDevice/CreateCHN.  Returns
 * the number of bytes read, or negative on error.  This is the seam a future
 * decrypt path hooks into (feed plaintext from an in-memory buffer). */
typedef int (*IpuReadFn)(void *data, int offset, int size, char *ctx);

typedef enum {
	IPU_FMT_U8 = 0,
	IPU_FMT_NV12,
	IPU_FMT_INT16,
	IPU_FMT_INT32,
	IPU_FMT_INT8,
	IPU_FMT_FP32,
	IPU_FMT_UNKNOWN,
	IPU_FMT_ARGB8888,
	IPU_FMT_ABGR8888
} IpuFmt;

typedef struct {
	unsigned int subnet;
	unsigned int usr_depth;
	unsigned int buf_depth;
} IpuChnAttr;

typedef struct {
	unsigned int max_dyn_buf_size;
	unsigned int yuv420_walign;
	unsigned int yuv420_halign;
	unsigned int rgb_walign;
} IpuDevAttr;

typedef struct {
	unsigned int dimension;
	IpuFmt       format;
	unsigned int shape[8];
	char         name[IPU_TENSOR_NAME_LEN];
	unsigned int inner_most;
	float        scalar;      /* dequant scale */
	long long    zero_pnt;    /* dequant zero point */
	int          aligned_buf_size;
} IpuTensorDesc;

typedef struct {
	unsigned int  in_count;
	unsigned int  out_count;
	IpuTensorDesc in_desc[IPU_MAX_TENSORS];
	IpuTensorDesc out_desc[IPU_MAX_TENSORS];
} IpuTensorIODesc;

typedef struct {
	void               *data[2];
	unsigned long long  phy_addr[2];
} IpuTensor;

typedef struct {
	unsigned int count;
	IpuTensor    tensor[IPU_MAX_TENSORS];
} IpuTensorVector;

typedef struct {
	unsigned int var_buf_size;   /* u32VariableBufferSize */
	unsigned int model_size;     /* u32OfflineModelSize */
} IpuOfflineInfo;

typedef struct {
	void *handle;
	void *cam_os_handle;         /* libcam_os_wrapper.so (libmi_ipu dep) */
	void *cam_fs_handle;         /* libcam_fs_wrapper.so (libmi_ipu dep) */
	void *mi_sys_handle;         /* libmi_sys.so (libmi_ipu dep) */
	int (*fnSysInit)(unsigned short dev);
	int (*fnSysConfigPool)(void *conf);  /* MI_SYS_ConfigPrivateMMAPool */
	int (*fnCreateDevice)(IpuDevAttr *cfg, IpuReadFn rd, char *fw_path,
		unsigned int fw_size);
	int (*fnDestroyDevice)(void);
	int (*fnCreateChannel)(unsigned int *chn, IpuChnAttr *cfg, IpuReadFn rd,
		char *net_path);
	int (*fnDestroyChannel)(unsigned int chn);
	int (*fnGetIODesc)(unsigned int chn, IpuTensorIODesc *desc);
	int (*fnGetInputTensors)(unsigned int chn, IpuTensorVector *ins);
	int (*fnGetOutputTensors)(unsigned int chn, IpuTensorVector *outs);
	int (*fnPutInputTensors)(unsigned int chn, IpuTensorVector *ins);
	int (*fnPutOutputTensors)(unsigned int chn, IpuTensorVector *outs);
	int (*fnInvoke)(unsigned int chn, IpuTensorVector *ins,
		IpuTensorVector *outs);
	/* Parses a compiled-network blob header; fills the variable-buffer
	 * size CreateDevice requires (a zero size is rejected). */
	int (*fnGetOfflineInfo)(IpuReadFn rd, char *net_path,
		IpuOfflineInfo *info);
} Star6eIpuImpl;

/* Global instance — defined in star6e_ipu.c. */
extern Star6eIpuImpl g_mi_ipu;

/** Load libmi_ipu.so and bind all symbols.  Returns 0 on success, -1 on
 *  failure (library missing or a required symbol not found). */
int star6e_ipu_load(void);

/** Close the library and zero the binding table. */
void star6e_ipu_unload(void);

/** Reconcile stale NPU driver state with a bare CreateDevice+DestroyDevice
 *  cycle.  A predecessor process that ran the IPU detector can leave kernel-
 *  side state that permanently wedges the successor's ISP CMDQ (stuck
 *  mid-WAIT on ISP_TRIG, endless `ISP_IRQ_WQ_FRAME_START add WQ error!`
 *  storm) — device-localized on .232: an IPU device create resets that state,
 *  and nothing else short of it reliably does.  Must run BEFORE VIF/VPE/ISP
 *  bring-up; a scrub after the ISP has wedged does not recover it.  No-op
 *  (returns 0) when /dev/mi_ipu is absent.  Failures are soft (-1, logged) —
 *  the pipeline proceeds without the scrub. */
int star6e_ipu_scrub(void);

/** Human-readable name for an IpuFmt (for diagnostics). */
const char *star6e_ipu_fmt_name(IpuFmt fmt);

#endif /* STAR6E_IPU_H */
