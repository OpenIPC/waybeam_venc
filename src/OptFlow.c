#include "OptFlow.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
	OPTFLOW_HAVE_IVE_BINDINGS = 1,
	OPTFLOW_VERBOSE_INTERVAL_MS = 3000,
	OPTFLOW_DEFAULT_INTERVAL_MS = 10000,
	OPTFLOW_DEFAULT_PROCESS_FPS = 5,
	OPTFLOW_IVE_HANDLE = 2,
	OPTFLOW_TRACK_WIDTH = 320,
	OPTFLOW_TRACK_PATCH_RADIUS = 6,
	OPTFLOW_TRACK_SEARCH_RANGE = 8,
	OPTFLOW_LK_POINT_COUNT = 5,
	OPTFLOW_LK_POINT_SPACING = 18,
};

typedef uint64_t MI_PHY;
typedef int MI_IVE_HANDLE;
typedef int MI_SYS_BUF_HANDLE;
typedef unsigned char OptflowMiBool;
typedef signed short MI_S9Q7;
typedef int MI_S25Q7;
typedef unsigned char MI_U0Q8;

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

typedef struct {
	MI_PHY phyPhyAddr;
	MI_U8 *pu8VirAddr;
	MI_U32 u32Size;
} MI_IVE_MemInfo_t;

typedef MI_IVE_MemInfo_t MI_IVE_SrcMemInfo_t;

typedef struct {
	MI_S25Q7 s25q7X;
	MI_S25Q7 s25q7Y;
} MI_IVE_PointS25Q7_t;

typedef struct {
	MI_S32 s32Status;
	MI_S9Q7 s9q7Dx;
	MI_S9Q7 s9q7Dy;
} MI_IVE_MvS9Q7_t;

typedef struct {
	MI_U16 u16CornerNum;
	MI_U0Q8 u0q8MinEigThr;
	MI_U8 u8IterCount;
	MI_U0Q8 u0q8Epsilon;
} MI_IVE_LkOpticalFlowCtrl_t;

typedef MI_S32 (*OptflowIveCreateFn)(MI_IVE_HANDLE hHandle);
typedef MI_S32 (*OptflowIveDestroyFn)(MI_IVE_HANDLE hHandle);
typedef MI_S32 (*OptflowIveLkOpticalFlowFn)(MI_IVE_HANDLE hHandle,
	MI_IVE_SrcImage_t *pstSrcPre, MI_IVE_SrcImage_t *pstSrcCur,
	MI_IVE_SrcMemInfo_t *pstPoint, MI_IVE_MemInfo_t *pstMv,
	MI_IVE_LkOpticalFlowCtrl_t *pstLkOptiFlowCtrl, OptflowMiBool bInstant);

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
	int valid;
	double tx;
	double ty;
	double rz;
	uint32_t points_found;
	uint32_t points_used;
} OptflowLkMotion;

struct OptFlowState {
	uint32_t capture_width;
	uint32_t capture_height;
	uint32_t process_width;
	uint32_t process_height;
	uint32_t track_width;
	uint32_t track_height;
	uint32_t prev_stride;
	uint32_t process_fps;
	uint32_t process_interval_ms;
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
	OptflowIveLkOpticalFlowFn ive_lk_optical_flow;
	MI_IVE_LkOpticalFlowCtrl_t lk_ctrl;
	MI_IVE_Image_t prev_frame;
	MI_IVE_Image_t lk_prev_frame;
	MI_IVE_Image_t lk_curr_frame;
	MI_IVE_MemInfo_t lk_points;
	MI_IVE_MemInfo_t lk_motion_vectors;
	double lk_center_x;
	double lk_center_y;
	int lk_center_ready;
	uint8_t *track_prev;
	uint8_t *track_curr;
	MI_SYS_ChnPort_t vpe_port;
};

__attribute__((visibility("default"))) float __expf_finite(float value)
{
	return expf(value);
}

__attribute__((visibility("default"))) double __log_finite(double value)
{
	return log(value);
}

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

static uint32_t optflow_clamp_fps(uint32_t fps)
{
	if (fps < 1)
		return 1;
	if (fps > 60)
		return 60;
	return fps;
}

static uint32_t optflow_interval_ms_from_fps(uint32_t fps)
{
	fps = optflow_clamp_fps(fps);
	return (1000u + fps - 1u) / fps;
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
		state->ive_lk_optical_flow = (OptflowIveLkOpticalFlowFn)dlsym(
			state->ive_library, "MI_IVE_LkOpticalFlow");
		if (state->ive_create && state->ive_destroy &&
		    state->ive_lk_optical_flow)
			return 0;

		dlclose(state->ive_library);
		state->ive_library = NULL;
		state->ive_create = NULL;
		state->ive_destroy = NULL;
		state->ive_lk_optical_flow = NULL;
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
	MI_U32 plane0_size;

	if (!image->apu8VirAddr[0] && !image->aphyPhyAddr[0]) {
		memset(image, 0, sizeof(*image));
		return;
	}

	plane0_size = (MI_U32)image->azu16Stride[0] * image->u16Height;
	if (image->apu8VirAddr[0])
		(void)MI_SYS_Munmap(image->apu8VirAddr[0], plane0_size);
	if (image->aphyPhyAddr[0])
		(void)MI_SYS_MMA_Free(image->aphyPhyAddr[0]);
	memset(image, 0, sizeof(*image));
}

static int allocate_mem_info(MI_IVE_MemInfo_t *mem, MI_U32 size)
{
	MI_PHY phy_addr = 0;
	void *virt_addr = NULL;
	MI_S32 ret;

	memset(mem, 0, sizeof(*mem));
	ret = MI_SYS_MMA_Alloc(NULL, size, &phy_addr);
	if (ret != MI_SUCCESS)
		return -1;

	ret = MI_SYS_Mmap(phy_addr, size, &virt_addr, 0);
	if (ret != MI_SUCCESS) {
		MI_SYS_MMA_Free(phy_addr);
		return -1;
	}

	mem->phyPhyAddr = phy_addr;
	mem->pu8VirAddr = virt_addr;
	mem->u32Size = size;
	memset(mem->pu8VirAddr, 0, size);
	return 0;
}

static void free_mem_info(MI_IVE_MemInfo_t *mem)
{
	if (!mem->pu8VirAddr && !mem->phyPhyAddr) {
		memset(mem, 0, sizeof(*mem));
		return;
	}

	if (mem->pu8VirAddr)
		(void)MI_SYS_Munmap(mem->pu8VirAddr, mem->u32Size);
	if (mem->phyPhyAddr)
		(void)MI_SYS_MMA_Free(mem->phyPhyAddr);
	memset(mem, 0, sizeof(*mem));
}

static int prepare_tracking_buffers(OptFlowState *state)
{
	uint32_t width;
	uint32_t height;
	uint32_t track_bytes;

	width = state->process_width;
	if (width > OPTFLOW_TRACK_WIDTH)
		width = OPTFLOW_TRACK_WIDTH;
	if (width < 64)
		width = 64;

	height = (uint32_t)(((uint64_t)state->process_height * width) /
		state->process_width);
	if (height < 64)
		height = 64;
	if (height > state->process_height)
		height = state->process_height;
	if (height & 1u)
		height--;

	state->track_width = width;
	state->track_height = height;
	track_bytes = state->track_width * state->track_height;

	state->track_prev = calloc(track_bytes, 1);
	state->track_curr = calloc(track_bytes, 1);
	if (!state->track_prev || !state->track_curr) {
		free(state->track_prev);
		free(state->track_curr);
		state->track_prev = NULL;
		state->track_curr = NULL;
		state->track_width = 0;
		state->track_height = 0;
		return -1;
	}

	return 0;
}

static void release_tracking_buffers(OptFlowState *state)
{
	free(state->track_prev);
	free(state->track_curr);
	state->track_prev = NULL;
	state->track_curr = NULL;
	state->track_width = 0;
	state->track_height = 0;
}

static int clamp_int(int value, int min_value, int max_value)
{
	if (value < min_value)
		return min_value;
	if (value > max_value)
		return max_value;
	return value;
}

static void downsample_luma(uint8_t *dst, uint32_t dst_width,
	uint32_t dst_height, uint32_t dst_stride, const uint8_t *src, uint32_t src_stride,
	uint32_t src_width, uint32_t src_height)
{
	uint32_t dst_y;

	for (dst_y = 0; dst_y < dst_height; ++dst_y) {
		uint32_t src_y = (uint32_t)(((uint64_t)dst_y * src_height) /
			dst_height);
		uint32_t dst_x;

		if (src_y >= src_height)
			src_y = src_height - 1;
		for (dst_x = 0; dst_x < dst_width; ++dst_x) {
			uint32_t src_x = (uint32_t)(((uint64_t)dst_x * src_width) /
				dst_width);
			if (src_x >= src_width)
				src_x = src_width - 1;
			dst[dst_y * dst_stride + dst_x] =
				src[src_y * src_stride + src_x];
		}
	}
}

static void copy_tracking_buffer_to_image(MI_IVE_Image_t *image,
	const uint8_t *src, uint32_t src_width, uint32_t src_height)
{
	uint32_t row;

	for (row = 0; row < src_height; ++row) {
		memcpy(image->apu8VirAddr[0] + row * image->azu16Stride[0],
			src + row * src_width,
			src_width);
	}
}

static void update_tracking_buffers(OptFlowState *state,
	const MI_SYS_BufInfo_t *buf_info)
{
	downsample_luma(state->track_prev, state->track_width,
		state->track_height, state->track_width,
		state->prev_frame.apu8VirAddr[0], state->prev_stride,
		state->process_width, state->process_height);
	downsample_luma(state->track_curr, state->track_width,
		state->track_height, state->track_width,
		buf_info->stFrameData.pVirAddr[0],
		buf_info->stFrameData.u32Stride[0], state->process_width,
		state->process_height);
}

static uint32_t select_lk_points(OptFlowState *state,
	MI_IVE_PointS25Q7_t *points, uint32_t max_points)
{
	static const int offsets[OPTFLOW_LK_POINT_COUNT][2] = {
		{0, 0},
		{-OPTFLOW_LK_POINT_SPACING, 0},
		{OPTFLOW_LK_POINT_SPACING, 0},
		{0, -OPTFLOW_LK_POINT_SPACING},
		{0, OPTFLOW_LK_POINT_SPACING},
	};
	int usable_margin_x;
	int usable_margin_y;
	int center_x;
	int center_y;
	uint32_t index;

	if (!state->track_prev || state->track_width == 0 || state->track_height == 0)
		return 0;
	if (max_points < OPTFLOW_LK_POINT_COUNT)
		return 0;

	usable_margin_x = OPTFLOW_TRACK_PATCH_RADIUS + OPTFLOW_TRACK_SEARCH_RANGE + 1;
	usable_margin_y = OPTFLOW_TRACK_PATCH_RADIUS + OPTFLOW_TRACK_SEARCH_RANGE + 1;
	if ((int)state->track_width <= usable_margin_x * 2 ||
	    (int)state->track_height <= usable_margin_y * 2)
		return 0;

	if (!state->lk_center_ready) {
		state->lk_center_x = (double)state->track_width * 0.5;
		state->lk_center_y = (double)state->track_height * 0.5;
		state->lk_center_ready = 1;
	}

	center_x = clamp_int((int)(state->lk_center_x + 0.5),
		usable_margin_x,
		(int)state->track_width - 1 - usable_margin_x);
	center_y = clamp_int((int)(state->lk_center_y + 0.5),
		usable_margin_y,
		(int)state->track_height - 1 - usable_margin_y);

	for (index = 0; index < OPTFLOW_LK_POINT_COUNT; ++index) {
		int point_x = clamp_int(center_x + offsets[index][0],
			usable_margin_x,
			(int)state->track_width - 1 - usable_margin_x);
		int point_y = clamp_int(center_y + offsets[index][1],
			usable_margin_y,
			(int)state->track_height - 1 - usable_margin_y);

		points[index].s25q7X = (MI_S25Q7)(point_x << 7);
		points[index].s25q7Y = (MI_S25Q7)(point_y << 7);
	}

	return OPTFLOW_LK_POINT_COUNT;
}

static int compute_lk_motion(OptFlowState *state, OptflowLkMotion *motion)
{
	MI_IVE_PointS25Q7_t *points;
	MI_IVE_MvS9Q7_t *vectors;
	double average_x = 0.0;
	double average_y = 0.0;
	double rotation_sum = 0.0;
	uint32_t point_count;
	uint32_t used_count = 0;
	uint32_t index;
	MI_S32 ret;

	memset(motion, 0, sizeof(*motion));
	if (!state->ive_lk_optical_flow)
		return 0;
	if (!state->lk_prev_frame.apu8VirAddr[0] || !state->lk_curr_frame.apu8VirAddr[0])
		return 0;
	if (!state->lk_points.pu8VirAddr || !state->lk_motion_vectors.pu8VirAddr)
		return 0;

	copy_tracking_buffer_to_image(&state->lk_prev_frame, state->track_prev,
		state->track_width, state->track_height);
	copy_tracking_buffer_to_image(&state->lk_curr_frame, state->track_curr,
		state->track_width, state->track_height);

	points = (MI_IVE_PointS25Q7_t *)state->lk_points.pu8VirAddr;
	vectors = (MI_IVE_MvS9Q7_t *)state->lk_motion_vectors.pu8VirAddr;
	memset(points, 0, state->lk_points.u32Size);
	memset(vectors, 0, state->lk_motion_vectors.u32Size);

	point_count = select_lk_points(state, points,
		state->lk_points.u32Size / sizeof(points[0]));
	motion->points_found = point_count;
	if (point_count < 4)
		return 0;

	state->lk_ctrl.u16CornerNum = (MI_U16)point_count;
	ret = state->ive_lk_optical_flow(state->ive_handle,
		&state->lk_prev_frame, &state->lk_curr_frame,
		&state->lk_points, &state->lk_motion_vectors,
		&state->lk_ctrl, 0);
	if (ret != MI_SUCCESS)
		return 0;

	for (index = 0; index < point_count; ++index) {
		double dx;
		double dy;

		if (vectors[index].s32Status != 0)
			continue;

		dx = (double)vectors[index].s9q7Dx / 128.0;
		dy = (double)vectors[index].s9q7Dy / 128.0;
		average_x += dx;
		average_y += dy;
		used_count++;
	}

	motion->points_used = used_count;
	if (used_count < 4)
		return 0;

	average_x /= used_count;
	average_y /= used_count;

	for (index = 0; index < point_count; ++index) {
		double dx;
		double dy;
		double residual_x;
		double residual_y;
		double radius_x;
		double radius_y;
		double radius_sq;

		if (vectors[index].s32Status != 0)
			continue;

		dx = (double)vectors[index].s9q7Dx / 128.0;
		dy = (double)vectors[index].s9q7Dy / 128.0;
		residual_x = dx - average_x;
		residual_y = dy - average_y;
		radius_x = ((double)points[index].s25q7X / 128.0) -
			((double)state->track_width * 0.5);
		radius_y = ((double)points[index].s25q7Y / 128.0) -
			((double)state->track_height * 0.5);
		radius_sq = radius_x * radius_x + radius_y * radius_y;
		if (radius_sq < 100.0)
			continue;

		rotation_sum += (radius_x * residual_y - radius_y * residual_x) /
			radius_sq;
	}

	motion->valid = 1;
	motion->tx = average_x * ((double)state->process_width / state->track_width);
	motion->ty = average_y * ((double)state->process_height / state->track_height);
	motion->rz = rotation_sum / used_count;
	state->lk_center_x = clamp_int((int)(state->lk_center_x + average_x + 0.5),
		0, (int)state->track_width - 1);
	state->lk_center_y = clamp_int((int)(state->lk_center_y + average_y + 0.5),
		0, (int)state->track_height - 1);
	return 1;
}

static int prepare_ive_buffers(OptFlowState *state)
{
	state->process_width = state->capture_width;
	state->process_height = state->capture_height;
	if (state->process_width < 64 || state->process_height < 64)
		return -1;

	state->prev_stride = align_up_u16((uint16_t)state->process_width, 16);

	if (allocate_image(&state->prev_frame, E_MI_IVE_IMAGE_TYPE_U8C1,
			(uint16_t)state->prev_stride,
			(uint16_t)state->process_width,
			(uint16_t)state->process_height) != 0)
		return -1;

	if (prepare_tracking_buffers(state) != 0)
		goto fail;

	if (state->ive_lk_optical_flow) {
		uint16_t lk_stride = align_up_u16((uint16_t)state->track_width, 16);

		if (allocate_image(&state->lk_prev_frame, E_MI_IVE_IMAGE_TYPE_U8C1,
				lk_stride,
				(uint16_t)state->track_width,
				(uint16_t)state->track_height) != 0)
			goto fail;
		if (allocate_image(&state->lk_curr_frame, E_MI_IVE_IMAGE_TYPE_U8C1,
				lk_stride,
				(uint16_t)state->track_width,
				(uint16_t)state->track_height) != 0)
			goto fail;
		if (allocate_mem_info(&state->lk_points,
				(MI_U32)(OPTFLOW_LK_POINT_COUNT *
				sizeof(MI_IVE_PointS25Q7_t))) != 0)
			goto fail;
		if (allocate_mem_info(&state->lk_motion_vectors,
				(MI_U32)(OPTFLOW_LK_POINT_COUNT *
				sizeof(MI_IVE_MvS9Q7_t))) != 0)
			goto fail;
	}

	return 0;

fail:
	free_mem_info(&state->lk_motion_vectors);
	free_mem_info(&state->lk_points);
	free_image(&state->lk_curr_frame);
	free_image(&state->lk_prev_frame);
	release_tracking_buffers(state);
	free_image(&state->prev_frame);
	return -1;
}

static void release_ive_buffers(OptFlowState *state)
{
	free_mem_info(&state->lk_motion_vectors);
	free_mem_info(&state->lk_points);
	free_image(&state->lk_curr_frame);
	free_image(&state->lk_prev_frame);
	release_tracking_buffers(state);
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

static int grab_frame_and_detect(OptFlowState *state, OptflowLkMotion *lk_motion)
{
	union {
		MI_SYS_BufInfo_t info;
		unsigned char raw[512];
	} buf_storage;
	MI_SYS_BufInfo_t *buf_info = &buf_storage.info;
	MI_SYS_BUF_HANDLE buf_handle = 0;
	MI_S32 ret;

	memset(&buf_storage, 0, sizeof(buf_storage));
	memset(lk_motion, 0, sizeof(*lk_motion));

	ret = MI_SYS_ChnOutputPortGetBuf(&state->vpe_port, buf_info, &buf_handle);
	if (ret != MI_SUCCESS)
		return -1;
	trace_frame_debug(state, "getbuf", buf_info, buf_handle);

	if (!frame_buffer_is_accessible(buf_info, state)) {
		trace_frame_debug(state, "reject", buf_info, buf_handle);
		(void)MI_SYS_ChnOutputPortPutBuf(buf_handle);
		return -1;
	}

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
		return 0;
	}

	update_tracking_buffers(state, buf_info);
	ret = compute_lk_motion(state, lk_motion);
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
	} else {
		printf("[optflow] lk: tx/ty are full-frame pixels; rz is image-plane rotation proxy"
			" using %ux%u tracking at %u fps\n",
			state->track_width, state->track_height, state->process_fps);
	}

	fflush(stdout);
}

OptFlowState *optflow_create(uint32_t capture_width, uint32_t capture_height,
	int verbose, uint32_t fps, const void *vpe_port)
{
	OptFlowState *state = calloc(1, sizeof(*state));
	MI_S32 ret;

	if (!state)
		return NULL;

	state->capture_width = capture_width;
	state->capture_height = capture_height;
	state->verbose = verbose ? 1 : 0;
	state->process_fps = optflow_clamp_fps(fps ? fps : OPTFLOW_DEFAULT_PROCESS_FPS);
	state->process_interval_ms = optflow_interval_ms_from_fps(state->process_fps);
	state->runtime_ive_present = probe_runtime_ive_library();
	state->init_ms = monotonic_ms();
	state->last_log_ms = state->init_ms;
	state->last_process_ms = state->init_ms;
	state->ive_handle = OPTFLOW_IVE_HANDLE;
	state->lk_ctrl.u0q8MinEigThr = 32;
	state->lk_ctrl.u8IterCount = 10;
	state->lk_ctrl.u0q8Epsilon = 4;
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
	OptflowLkMotion lk_motion;

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
	    (now_ms - state->last_process_ms) >= (long long)state->process_interval_ms) {
		state->last_process_ms = now_ms;
		(void)grab_frame_and_detect(state, &lk_motion);
		if (lk_motion.valid) {
			printf("[optflow] lk tx=%.1f ty=%.1f rz=%.5f tracks=%u/%u frames=%" PRIu64 "\n",
				lk_motion.tx, lk_motion.ty, lk_motion.rz,
				lk_motion.points_used, lk_motion.points_found,
				state->frames_seen);
			fflush(stdout);
		}
	}

	if ((now_ms - state->last_log_ms) < interval_ms)
		return;

	state->last_log_ms = now_ms;

	if (OPTFLOW_HAVE_IVE_BINDINGS && state->runtime_ive_present &&
	    frame_feed_ready(state)) {
		printf("[optflow] active: encoded_frames=%" PRIu64 " packets=%" PRIu64
			" track=%ux%u lk=%s\n",
			state->frames_seen,
			state->packets_seen,
			state->track_width,
			state->track_height,
			state->ive_lk_optical_flow ? "yes" : "no");
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