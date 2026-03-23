#include "OptFlow.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
	OPTFLOW_HAVE_IVE_BINDINGS = 1,
	OPTFLOW_VERBOSE_INTERVAL_MS = 3000,
	OPTFLOW_DEFAULT_INTERVAL_MS = 10000,
	OPTFLOW_PROCESS_INTERVAL_MS = 200,
	OPTFLOW_SAD_BLOCK_SIZE = 8,
	OPTFLOW_SAD_PIXEL_DIFF = 15,
	OPTFLOW_IVE_HANDLE = 2,
};

typedef uint64_t MI_PHY;
typedef int MI_IVE_HANDLE;
typedef int MI_SYS_BUF_HANDLE;
typedef unsigned char OptflowMiBool;

#ifndef MI_SUCCESS
#define MI_SUCCESS 0
#endif

enum {
	OPTFLOW_SYS_FRAME_LAYOUT_REALTIME = 0,
};

#define OPTFLOW_SYS_REALTIME_MAGIC_ADDR ((uintptr_t)0x46414B45u)

typedef enum {
	E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420 = 11,
} MI_SYS_PixelFormat_e;

typedef enum {
	E_MI_SYS_BUFDATA_RAW = 0,
	E_MI_SYS_BUFDATA_FRAME = 1,
	E_MI_SYS_BUFDATA_META = 2,
} MI_SYS_BufDataType_e;

typedef struct {
	uint16_t u16X;
	uint16_t u16Y;
	uint16_t u16Width;
	uint16_t u16Height;
} MI_SYS_WindowRect_t;

typedef struct {
	uint32_t eType;
	union {
		uint32_t u32GlobalGradient;
	} uIspInfo;
} MI_SYS_FrameIspInfo_t;

typedef struct {
	uint32_t eTileMode;
	MI_SYS_PixelFormat_e ePixelFormat;
	uint32_t eCompressMode;
	uint32_t eFrameScanMode;
	uint32_t eFieldType;
	uint32_t ePhylayoutType;
	uint16_t u16Width;
	uint16_t u16Height;
	void *pVirAddr[3];
	MI_PHY phyAddr[3];
	uint32_t u32Stride[3];
	uint32_t u32BufSize;
	uint16_t u16RingBufStartLine;
	uint16_t u16RingBufRealTotalHeight;
	MI_SYS_FrameIspInfo_t stFrameIspInfo;
	MI_SYS_WindowRect_t stContentCropWindow;
} MI_SYS_FrameData_t;

typedef struct {
	void *pVirAddr;
	MI_PHY phyAddr;
	uint32_t u32BufSize;
	uint32_t u32ContentSize;
	OptflowMiBool bEndOfFrame;
	uint64_t u64SeqNum;
} MI_SYS_RawData_t;

typedef struct {
	void *pVirAddr;
	MI_PHY phyAddr;
	uint32_t u32Size;
	uint32_t u32ExtraData;
	uint32_t eDataFromModule;
} MI_SYS_MetaData_t;

typedef struct {
	uint64_t u64Pts;
	uint64_t u64SidebandMsg;
	MI_SYS_BufDataType_e eBufType;
	OptflowMiBool bEndOfStream;
	OptflowMiBool bUsrBuf;
	uint32_t u32SequenceNumber;
	OptflowMiBool bDrop;
	union {
		MI_SYS_FrameData_t stFrameData;
		MI_SYS_RawData_t stRawData;
		MI_SYS_MetaData_t stMetaData;
	};
} MI_SYS_BufInfo_t;

typedef enum {
	E_MI_IVE_IMAGE_TYPE_U8C1 = 0x0,
	E_MI_IVE_IMAGE_TYPE_U16C1 = 0x9,
} MI_IVE_ImageType_e;

typedef struct {
	MI_IVE_ImageType_e eType;
	MI_PHY aphyPhyAddr[3];
	MI_U8 *apu8VirAddr[3];
	MI_U16 azu16Stride[3];
	MI_U16 u16Width;
	MI_U16 u16Height;
	MI_U16 u16Reserved;
} MI_IVE_Image_t;

typedef MI_IVE_Image_t MI_IVE_SrcImage_t;
typedef MI_IVE_Image_t MI_IVE_DstImage_t;

typedef enum {
	E_MI_IVE_SAD_MODE_MB_4X4 = 0x0,
	E_MI_IVE_SAD_MODE_MB_8X8 = 0x1,
	E_MI_IVE_SAD_MODE_MB_16X16 = 0x2,
} MI_IVE_SadMode_e;

typedef enum {
	E_MI_IVE_SAD_OUT_CTRL_16BIT_BOTH = 0x0,
	E_MI_IVE_SAD_OUT_CTRL_8BIT_BOTH = 0x1,
	E_MI_IVE_SAD_OUT_CTRL_16BIT_SAD = 0x2,
	E_MI_IVE_SAD_OUT_CTRL_8BIT_SAD = 0x3,
	E_MI_IVE_SAD_OUT_CTRL_THRESH = 0x4,
} MI_IVE_SadOutCtrl_e;

typedef struct {
	MI_IVE_SadMode_e eMode;
	MI_IVE_SadOutCtrl_e eOutCtrl;
	MI_U16 u16Thr;
	MI_U8 u8MinVal;
	MI_U8 u8MaxVal;
} MI_IVE_SadCtrl_t;

typedef MI_S32 (*OptflowIveCreateFn)(MI_IVE_HANDLE hHandle);
typedef MI_S32 (*OptflowIveDestroyFn)(MI_IVE_HANDLE hHandle);
typedef MI_S32 (*OptflowIveSadFn)(MI_IVE_HANDLE hHandle,
	MI_IVE_SrcImage_t *pstSrc1, MI_IVE_SrcImage_t *pstSrc2,
	MI_IVE_DstImage_t *pstSad, MI_IVE_DstImage_t *pstThr,
	MI_IVE_SadCtrl_t *pstSadCtrl, OptflowMiBool bInstant);

MI_S32 MI_SYS_ChnOutputPortGetBuf(MI_SYS_ChnPort_t *pstChnPort,
	MI_SYS_BufInfo_t *pstBufInfo, MI_SYS_BUF_HANDLE *bufHandle);
MI_S32 MI_SYS_ChnOutputPortPutBuf(MI_SYS_BUF_HANDLE hBufHandle);
MI_S32 MI_SYS_MMA_Alloc(MI_U8 *pstMMAHeapName, MI_U32 u32BlkSize,
	MI_PHY *phyAddr);
MI_S32 MI_SYS_MMA_Free(MI_PHY phyAddr);
MI_S32 MI_SYS_Mmap(MI_U64 phyAddr, MI_U32 u32Size,
	void **ppVirtualAddress, OptflowMiBool bCache);
MI_S32 MI_SYS_Munmap(void *pVirtualAddress, MI_U32 u32Size);

typedef struct {
	int x;
	int y;
	int width;
	int height;
} OptflowMotionBox;

struct OptFlowState {
	uint32_t capture_width;
	uint32_t capture_height;
	uint32_t process_width;
	uint32_t process_height;
	uint32_t prev_stride;
	uint32_t sad_stride;
	uint32_t sad_width;
	uint32_t sad_height;
	int verbose;
	int runtime_ive_present;
	int frame_feed_ready;
	int ive_created;
	int have_prev_frame;
	uint64_t frames_seen;
	uint64_t packets_seen;
	long long init_ms;
	long long last_log_ms;
	long long last_process_ms;
	int first_frame_logged;
	MI_IVE_HANDLE ive_handle;
	void *ive_library;
	OptflowIveCreateFn ive_create;
	OptflowIveDestroyFn ive_destroy;
	OptflowIveSadFn ive_sad;
	MI_IVE_SadCtrl_t sad_ctrl;
	MI_IVE_Image_t prev_frame;
	MI_IVE_Image_t sad_result;
	MI_IVE_Image_t thr_result;
	MI_SYS_ChnPort_t vpe_port;
};

static long long monotonic_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000LL +
		(long long)now.tv_nsec / 1000000LL;
}

static int probe_runtime_ive_library(void)
{
	static const char *const names[] = {
		"libive.so",
		"libmi_ive.so",
		NULL,
	};
	size_t index;

	for (index = 0; names[index] != NULL; ++index) {
		void *handle = dlopen(names[index], RTLD_LAZY | RTLD_LOCAL);

		if (!handle)
			continue;
		dlclose(handle);
		return 1;
	}

	return 0;
}

static int load_ive_runtime(OptFlowState *state)
{
	static const char *const names[] = {
		"libive.so",
		"libmi_ive.so",
		NULL,
	};
	size_t index;

	for (index = 0; names[index] != NULL; ++index) {
		state->ive_library = dlopen(names[index], RTLD_LAZY | RTLD_LOCAL);
		if (!state->ive_library)
			continue;

		state->ive_create = (OptflowIveCreateFn)dlsym(state->ive_library,
			"MI_IVE_Create");
		state->ive_destroy = (OptflowIveDestroyFn)dlsym(state->ive_library,
			"MI_IVE_Destroy");
		state->ive_sad = (OptflowIveSadFn)dlsym(state->ive_library,
			"MI_IVE_Sad");
		if (state->ive_create && state->ive_destroy && state->ive_sad)
			return 0;

		dlclose(state->ive_library);
		state->ive_library = NULL;
		state->ive_create = NULL;
		state->ive_destroy = NULL;
		state->ive_sad = NULL;
	}

	return -1;
}

static uint16_t align_up_u16(uint16_t value, uint16_t alignment)
{
	return (uint16_t)(((value + alignment - 1) / alignment) * alignment);
}

static int frame_feed_ready(const OptFlowState *state)
{
	return state->frame_feed_ready;
}

static int should_trace_frame_debug(const OptFlowState *state)
{
	return state->frames_seen <= 2;
}

static void trace_frame_debug(const OptFlowState *state, const char *phase,
	const MI_SYS_BufInfo_t *buf_info, MI_SYS_BUF_HANDLE buf_handle)
{
	const MI_SYS_FrameData_t *frame = &buf_info->stFrameData;

	if (!should_trace_frame_debug(state))
		return;

	printf("[optflow] debug %s frame=%" PRIu64
		" type=%u layout=%u pix=%u wh=%ux%u stride=%u vir0=%p phy0=0x%llx handle=%d\n",
		phase,
		state->frames_seen,
		(unsigned)buf_info->eBufType,
		(unsigned)frame->ePhylayoutType,
		(unsigned)frame->ePixelFormat,
		(unsigned)frame->u16Width,
		(unsigned)frame->u16Height,
		(unsigned)frame->u32Stride[0],
		frame->pVirAddr[0],
		(unsigned long long)frame->phyAddr[0],
		buf_handle);
	fflush(stdout);
}

static int frame_buffer_is_accessible(const MI_SYS_BufInfo_t *buf_info,
	const OptFlowState *state)
{
	const MI_SYS_FrameData_t *frame = &buf_info->stFrameData;

	if (buf_info->eBufType != E_MI_SYS_BUFDATA_FRAME)
		return 0;
	if (frame->ePixelFormat != E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420)
		return 0;
	if (frame->ePhylayoutType == OPTFLOW_SYS_FRAME_LAYOUT_REALTIME)
		return 0;
	if (!frame->pVirAddr[0])
		return 0;
	if ((uintptr_t)frame->pVirAddr[0] == OPTFLOW_SYS_REALTIME_MAGIC_ADDR)
		return 0;
	if ((uintptr_t)frame->phyAddr[0] == OPTFLOW_SYS_REALTIME_MAGIC_ADDR)
		return 0;
	if (frame->u16Width < state->process_width ||
	    frame->u16Height < state->process_height)
		return 0;
	if (frame->u32Stride[0] < state->process_width)
		return 0;
	return 1;
}

static int allocate_image(MI_IVE_Image_t *image, MI_IVE_ImageType_e type,
	uint16_t stride, uint16_t width, uint16_t height)
{
	MI_U32 elem_size = 1;
	MI_U32 plane0_size;
	MI_PHY phy_addr = 0;
	void *virt_addr = NULL;
	MI_S32 ret;

	memset(image, 0, sizeof(*image));
	image->eType = type;
	image->u16Width = width;
	image->u16Height = height;
	image->azu16Stride[0] = stride;

	switch (type) {
	case E_MI_IVE_IMAGE_TYPE_U8C1:
		plane0_size = (MI_U32)stride * height;
		break;
	case E_MI_IVE_IMAGE_TYPE_U16C1:
		elem_size = 2;
		plane0_size = (MI_U32)stride * height * elem_size;
		break;
	default:
		return -1;
	}

	ret = MI_SYS_MMA_Alloc(NULL, plane0_size, &phy_addr);
	if (ret != MI_SUCCESS)
		return -1;

	ret = MI_SYS_Mmap(phy_addr, plane0_size, &virt_addr, 0);
	if (ret != MI_SUCCESS) {
		MI_SYS_MMA_Free(phy_addr);
		return -1;
	}

	image->aphyPhyAddr[0] = phy_addr;
	image->apu8VirAddr[0] = virt_addr;
	memset(image->apu8VirAddr[0], 0, plane0_size);
	return 0;
}

static void free_image(MI_IVE_Image_t *image)
{
	MI_U32 elem_size = 1;
	MI_U32 plane0_size;

	if (!image->apu8VirAddr[0] && !image->aphyPhyAddr[0]) {
		memset(image, 0, sizeof(*image));
		return;
	}

	if (image->eType == E_MI_IVE_IMAGE_TYPE_U16C1)
		elem_size = 2;
	plane0_size = (MI_U32)image->azu16Stride[0] * image->u16Height * elem_size;
	if (image->apu8VirAddr[0])
		(void)MI_SYS_Munmap(image->apu8VirAddr[0], plane0_size);
	if (image->aphyPhyAddr[0])
		(void)MI_SYS_MMA_Free(image->aphyPhyAddr[0]);
	memset(image, 0, sizeof(*image));
}

static int prepare_ive_buffers(OptFlowState *state)
{
	state->process_width = state->capture_width -
		(state->capture_width % OPTFLOW_SAD_BLOCK_SIZE);
	state->process_height = state->capture_height -
		(state->capture_height % OPTFLOW_SAD_BLOCK_SIZE);
	if (state->process_width < 64 || state->process_height < 64)
		return -1;

	state->prev_stride = align_up_u16((uint16_t)state->process_width, 16);
	state->sad_width = state->process_width / OPTFLOW_SAD_BLOCK_SIZE;
	state->sad_height = state->process_height / OPTFLOW_SAD_BLOCK_SIZE;
	state->sad_stride = align_up_u16((uint16_t)state->sad_width, 16);

	if (allocate_image(&state->prev_frame, E_MI_IVE_IMAGE_TYPE_U8C1,
			(uint16_t)state->prev_stride,
			(uint16_t)state->process_width,
			(uint16_t)state->process_height) != 0)
		return -1;

	if (allocate_image(&state->sad_result, E_MI_IVE_IMAGE_TYPE_U16C1,
			(uint16_t)state->sad_stride,
			(uint16_t)state->sad_width,
			(uint16_t)state->sad_height) != 0)
		goto fail;

	if (allocate_image(&state->thr_result, E_MI_IVE_IMAGE_TYPE_U8C1,
			(uint16_t)state->sad_stride,
			(uint16_t)state->sad_width,
			(uint16_t)state->sad_height) != 0)
		goto fail;

	return 0;

fail:
	free_image(&state->thr_result);
	free_image(&state->sad_result);
	free_image(&state->prev_frame);
	return -1;
}

static void release_ive_buffers(OptFlowState *state)
{
	free_image(&state->thr_result);
	free_image(&state->sad_result);
	free_image(&state->prev_frame);
}

static void copy_luma_to_prev(OptFlowState *state, const MI_SYS_BufInfo_t *buf_info)
{
	uint32_t row;
	const uint8_t *src = buf_info->stFrameData.pVirAddr[0];
	uint8_t *dst = state->prev_frame.apu8VirAddr[0];
	uint32_t src_stride = buf_info->stFrameData.u32Stride[0];

	for (row = 0; row < state->process_height; ++row) {
		memcpy(dst + row * state->prev_stride,
			src + row * src_stride,
			state->process_width);
	}
}

static void wrap_current_luma(const OptFlowState *state,
	const MI_SYS_BufInfo_t *buf_info, MI_IVE_Image_t *image)
{
	memset(image, 0, sizeof(*image));
	image->eType = E_MI_IVE_IMAGE_TYPE_U8C1;
	image->u16Width = (uint16_t)state->process_width;
	image->u16Height = (uint16_t)state->process_height;
	image->azu16Stride[0] = (MI_U16)buf_info->stFrameData.u32Stride[0];
	image->apu8VirAddr[0] = buf_info->stFrameData.pVirAddr[0];
	image->aphyPhyAddr[0] = buf_info->stFrameData.phyAddr[0];
}

static int compute_motion_box(const OptFlowState *state, OptflowMotionBox *box)
{
	int x_min = -1;
	int y_min = -1;
	int x_max = -1;
	int y_max = -1;
	uint32_t row;
	uint32_t col;
	const uint8_t *thr = state->thr_result.apu8VirAddr[0];

	for (row = 0; row < state->sad_height; ++row) {
		const uint8_t *line = thr + row * state->sad_stride;
		for (col = 0; col < state->sad_width; ++col) {
			if (!line[col])
				continue;
			if (x_min < 0 || (int)col < x_min)
				x_min = (int)col;
			if (x_max < 0 || (int)col > x_max)
				x_max = (int)col;
			if (y_min < 0 || (int)row < y_min)
				y_min = (int)row;
			if (y_max < 0 || (int)row > y_max)
				y_max = (int)row;
		}
	}

	if (x_min < 0 || y_min < 0 || x_max < x_min || y_max < y_min) {
		box->x = 0;
		box->y = 0;
		box->width = 0;
		box->height = 0;
		return 0;
	}

	if (x_min > 0)
		x_min--;
	if ((uint32_t)x_max + 1 < state->sad_width)
		x_max++;
	if (y_min > 0)
		y_min--;
	if ((uint32_t)y_max + 1 < state->sad_height)
		y_max++;

	box->x = x_min * OPTFLOW_SAD_BLOCK_SIZE;
	box->y = y_min * OPTFLOW_SAD_BLOCK_SIZE;
	box->width = (x_max - x_min + 1) * OPTFLOW_SAD_BLOCK_SIZE;
	box->height = (y_max - y_min + 1) * OPTFLOW_SAD_BLOCK_SIZE;
	return 1;
}

static int grab_frame_and_detect(OptFlowState *state, OptflowMotionBox *box)
{
	union {
		MI_SYS_BufInfo_t info;
		unsigned char raw[512];
	} buf_storage;
	MI_SYS_BufInfo_t *buf_info = &buf_storage.info;
	MI_SYS_BUF_HANDLE buf_handle = 0;
	MI_IVE_Image_t current_frame;
	MI_S32 ret;

	memset(&buf_storage, 0, sizeof(buf_storage));
	memset(&current_frame, 0, sizeof(current_frame));

	ret = MI_SYS_ChnOutputPortGetBuf(&state->vpe_port, buf_info, &buf_handle);
	if (ret != MI_SUCCESS)
		return -1;
	trace_frame_debug(state, "getbuf", buf_info, buf_handle);

	if (!frame_buffer_is_accessible(buf_info, state)) {
		trace_frame_debug(state, "reject", buf_info, buf_handle);
		(void)MI_SYS_ChnOutputPortPutBuf(buf_handle);
		return -1;
	}

	wrap_current_luma(state, buf_info, &current_frame);
	if (!state->have_prev_frame) {
		if (should_trace_frame_debug(state)) {
			printf("[optflow] debug copy-prev-start frame=%" PRIu64 "\n",
				state->frames_seen);
			fflush(stdout);
		}
		copy_luma_to_prev(state, buf_info);
		if (should_trace_frame_debug(state)) {
			printf("[optflow] debug copy-prev-done frame=%" PRIu64 "\n",
				state->frames_seen);
			fflush(stdout);
		}
		state->have_prev_frame = 1;
		if (should_trace_frame_debug(state)) {
			printf("[optflow] debug putbuf-first frame=%" PRIu64 "\n",
				state->frames_seen);
			fflush(stdout);
		}
		(void)MI_SYS_ChnOutputPortPutBuf(buf_handle);
		box->x = 0;
		box->y = 0;
		box->width = 0;
		box->height = 0;
		return 0;
	}

	ret = state->ive_sad(state->ive_handle, &state->prev_frame, &current_frame,
		&state->sad_result, &state->thr_result, &state->sad_ctrl, 0);
	if (ret != MI_SUCCESS) {
		copy_luma_to_prev(state, buf_info);
		(void)MI_SYS_ChnOutputPortPutBuf(buf_handle);
		return -1;
	}

	ret = compute_motion_box(state, box);
	copy_luma_to_prev(state, buf_info);
	(void)MI_SYS_ChnOutputPortPutBuf(buf_handle);
	return ret;
}

static void log_capability_summary(const OptFlowState *state, const char *phase)
{
	printf("[optflow] %s: capture=%ux%u build_ive=%s runtime_ive=%s frame_feed=%s\n",
		phase,
		state->capture_width,
		state->capture_height,
		OPTFLOW_HAVE_IVE_BINDINGS ? "yes" : "no",
		state->runtime_ive_present ? "yes" : "no",
		frame_feed_ready(state) ? "yes" : "no");

	if (!OPTFLOW_HAVE_IVE_BINDINGS || !state->runtime_ive_present ||
	    !frame_feed_ready(state)) {
		printf("[optflow] disabled: IVE tracking is not available in this build/runtime yet"
			" (missing bindings, runtime library, or raw-frame feed path)\n");
	}

	fflush(stdout);
}

OptFlowState *optflow_create(uint32_t capture_width, uint32_t capture_height,
	int verbose, const void *vpe_port)
{
	OptFlowState *state = calloc(1, sizeof(*state));
	MI_S32 ret;

	if (!state)
		return NULL;

	state->capture_width = capture_width;
	state->capture_height = capture_height;
	state->verbose = verbose ? 1 : 0;
	state->runtime_ive_present = probe_runtime_ive_library();
	state->init_ms = monotonic_ms();
	state->last_log_ms = state->init_ms;
	state->last_process_ms = state->init_ms;
	state->ive_handle = OPTFLOW_IVE_HANDLE;
	state->sad_ctrl.eMode = E_MI_IVE_SAD_MODE_MB_8X8;
	state->sad_ctrl.eOutCtrl = E_MI_IVE_SAD_OUT_CTRL_16BIT_BOTH;
	state->sad_ctrl.u16Thr = OPTFLOW_SAD_BLOCK_SIZE * OPTFLOW_SAD_BLOCK_SIZE *
		OPTFLOW_SAD_PIXEL_DIFF;
	state->sad_ctrl.u8MinVal = 0;
	state->sad_ctrl.u8MaxVal = 255;
	if (vpe_port)
		memcpy(&state->vpe_port, vpe_port, sizeof(state->vpe_port));

	if (!state->runtime_ive_present)
		goto done;

	if (load_ive_runtime(state) != 0)
		goto done;

	ret = state->ive_create(state->ive_handle);
	if (ret != MI_SUCCESS)
		goto done;
	state->ive_created = 1;

	if (prepare_ive_buffers(state) != 0)
		goto done;

	ret = MI_SYS_SetChnOutputPortDepth(&state->vpe_port, 2, 6);
	if (ret == MI_SUCCESS)
		state->frame_feed_ready = 1;

done:

	log_capability_summary(state, "init");
	return state;
}

void optflow_on_stream(OptFlowState *state, uint32_t pack_count)
{
	long long now_ms;
	int interval_ms;
	OptflowMotionBox box;
	int ret;

	if (!state)
		return;

	state->frames_seen++;
	state->packets_seen += pack_count;
	now_ms = monotonic_ms();
	interval_ms = state->verbose ? OPTFLOW_VERBOSE_INTERVAL_MS :
		OPTFLOW_DEFAULT_INTERVAL_MS;

	if (!state->first_frame_logged) {
		printf("[optflow] stream-online: first encoded frame observed; tracker state=%s\n",
			(OPTFLOW_HAVE_IVE_BINDINGS && state->runtime_ive_present &&
			 frame_feed_ready(state)) ? "active" : "standby");
		fflush(stdout);
		state->first_frame_logged = 1;
	}

	if (OPTFLOW_HAVE_IVE_BINDINGS && state->runtime_ive_present &&
	    frame_feed_ready(state) &&
	    (now_ms - state->last_process_ms) >= OPTFLOW_PROCESS_INTERVAL_MS) {
		state->last_process_ms = now_ms;
		ret = grab_frame_and_detect(state, &box);
		if (ret > 0 && box.width > 0 && box.height > 0) {
			printf("[optflow] motion roi x=%d y=%d w=%d h=%d frames=%" PRIu64 "\n",
				box.x, box.y, box.width, box.height, state->frames_seen);
			fflush(stdout);
		}
	}

	if ((now_ms - state->last_log_ms) < interval_ms)
		return;

	state->last_log_ms = now_ms;

	if (OPTFLOW_HAVE_IVE_BINDINGS && state->runtime_ive_present &&
	    frame_feed_ready(state)) {
		printf("[optflow] active: encoded_frames=%" PRIu64 " packets=%" PRIu64
			" sad=%ux%u block=%u\n",
			state->frames_seen,
			state->packets_seen,
			state->sad_width,
			state->sad_height,
			OPTFLOW_SAD_BLOCK_SIZE);
	} else {
		printf("[optflow] standby: encoded_frames=%" PRIu64 " packets=%" PRIu64
			" 6dof=unavailable\n",
			state->frames_seen,
			state->packets_seen);
	}

	fflush(stdout);
}

void optflow_destroy(OptFlowState *state)
{
	if (!state)
		return;

	release_ive_buffers(state);
	if (state->ive_created)
		(void)state->ive_destroy(state->ive_handle);
	if (state->ive_library)
		dlclose(state->ive_library);

	printf("[optflow] stop: uptime_ms=%lld encoded_frames=%" PRIu64
		" packets=%" PRIu64 "\n",
		monotonic_ms() - state->init_ms,
		state->frames_seen,
		state->packets_seen);
	fflush(stdout);
	free(state);
}