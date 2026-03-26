#include "OptFlow.h"

#include "star6e_osd_simple.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
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
	OPTFLOW_OSD_CALIBRATION_MODE = 0,
	OPTFLOW_OSD_CALIBRATION_STEP_MS = 2000,
	OPTFLOW_OSD_CENTER_HOLD_MS = 1000,
	OPTFLOW_TRACK_WIDTH = 160, //was 320,
	OPTFLOW_TRACK_PATCH_RADIUS = 6,
	OPTFLOW_TRACK_SEARCH_RANGE = 20, // was 8
	OPTFLOW_LK_POINT_COUNT = 50, // MAX buffer for features, should be > OPTFLOW_LK_CALL_POINT_COUNT
	OPTFLOW_LK_CALL_POINT_COUNT = 14, // crashes with 16 and more!? Reduce to 10 or less if "segmentation fault"
	OPTFLOW_LK_MIN_SURVIVORS = 3,
	OPTFLOW_LK_MIN_POINT_SPACING =24,  // was 12,
	OPTFLOW_LK_CANDIDATE_CAPACITY = 64,
	OPTFLOW_LK_SCAN_STEP = 2,
	OPTFLOW_LK_DUMP_INTERVAL_MS = 1000,
	OPTFLOW_STAGE_SLOT_COUNT = 2,
	OPTFLOW_CRASH_TRACE_SEQ_LIMIT = 12,
	OPTFLOW_KEEP_LAST_VALID_REFERENCE = 1,
	OPTFLOW_USE_REGION_POINT_SELECTOR = 1,
	OPTFLOW_LK_REGION_COLS = 2,
	OPTFLOW_LK_REGION_ROWS = 2,
	OPTFLOW_LK_REGION_COUNT =
		OPTFLOW_LK_REGION_COLS * OPTFLOW_LK_REGION_ROWS,
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
	uint32_t debug_point_count;
	unsigned int debug_point_x[OPTFLOW_LK_CALL_POINT_COUNT];
	unsigned int debug_point_y[OPTFLOW_LK_CALL_POINT_COUNT];
} OptflowLkMotion;

typedef struct {
	uint8_t *data;
	uint64_t seq;
	int ready;
} OptflowCropSlot;

typedef struct {
	int x;
	int y;
	uint64_t score;
} OptflowCornerCandidate;

typedef enum {
	OPTFLOW_LK_FAIL_NONE = 0,
	OPTFLOW_LK_FAIL_NO_RUNTIME,
	OPTFLOW_LK_FAIL_NO_IMAGES,
	OPTFLOW_LK_FAIL_NO_BUFFERS,
	OPTFLOW_LK_FAIL_NO_POINTS,
	OPTFLOW_LK_FAIL_TOO_FEW_POINTS,
	OPTFLOW_LK_FAIL_LK_CALL,
	OPTFLOW_LK_FAIL_TOO_FEW_TRACKS,
} OptflowLkFailureReason;

struct OptFlowState {
	uint32_t capture_width;
	uint32_t capture_height;
	uint32_t osd_space_width;
	uint32_t osd_space_height;
	uint32_t process_width;
	uint32_t process_height;
	uint32_t track_crop_x;
	uint32_t track_crop_width;
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
	long long last_lk_dump_ms;
	long long last_log_ms;
	long long last_process_ms;
	long long perf_window_start_ms;
	int first_frame_logged;
	uint64_t perf_scale_ms;
	uint64_t perf_lk_ms;
	uint64_t perf_total_ms;
	uint64_t perf_corner_sum;
	uint32_t perf_runs;
	uint32_t osd_dot_x;
	uint32_t osd_dot_y;
	uint32_t stage_stride;
	uint32_t stage_height;
	uint32_t stage_published_slot;
	uint32_t stage_write_slot;
	int show_osd;
	int osd_calibration_anchor;
	int osd_region_ready;
	int osd_region_disabled;
	int worker_running;
	int worker_started;
	int worker_have_prev_crop;
	MI_IVE_HANDLE ive_handle;
	void *ive_library;
	OptflowIveCreateFn ive_create;
	OptflowIveDestroyFn ive_destroy;
	OptflowIveLkOpticalFlowFn ive_lk_optical_flow;
	MI_IVE_LkOpticalFlowCtrl_t lk_ctrl;
	MI_IVE_Image_t lk_prev_frame;
	MI_IVE_Image_t lk_curr_frame;
	MI_IVE_MemInfo_t lk_points;
	MI_IVE_MemInfo_t lk_motion_vectors;
	double lk_center_x;
	double lk_center_y;
	int lk_center_ready;
	uint64_t stage_next_seq;
	uint64_t stage_published_seq;
	uint64_t applied_motion_seq;
	uint64_t latest_motion_seq;
	uint8_t *track_prev;
	uint8_t *track_curr;
	uint8_t *worker_prev_crop;
	uint8_t *worker_curr_crop;
	OptflowCropSlot stage_slots[OPTFLOW_STAGE_SLOT_COUNT];
	OptflowLkMotion latest_motion;
	pthread_t worker_thread;
	pthread_mutex_t stage_lock;
	pthread_cond_t stage_cond;
	pthread_mutex_t result_lock;
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

static void swap_buffers(uint8_t **left, uint8_t **right)
{
	uint8_t *tmp = *left;

	*left = *right;
	*right = tmp;
}

static void write_u16le(FILE *file, uint16_t value)
{
	unsigned char bytes[2];

	bytes[0] = (unsigned char)(value & 0xffu);
	bytes[1] = (unsigned char)((value >> 8) & 0xffu);
	(void)fwrite(bytes, sizeof(bytes), 1, file);
}

static void write_u32le(FILE *file, uint32_t value)
{
	unsigned char bytes[4];

	bytes[0] = (unsigned char)(value & 0xffu);
	bytes[1] = (unsigned char)((value >> 8) & 0xffu);
	bytes[2] = (unsigned char)((value >> 16) & 0xffu);
	bytes[3] = (unsigned char)((value >> 24) & 0xffu);
	(void)fwrite(bytes, sizeof(bytes), 1, file);
}

static int write_grayscale_bmp(const char *path, const MI_IVE_Image_t *image)
{
	enum {
		bmp_file_header_size = 14,
		bmp_info_header_size = 40,
		bmp_palette_entries = 256,
		bmp_palette_size = bmp_palette_entries * 4,
		bmp_pixel_offset = bmp_file_header_size + bmp_info_header_size +
			bmp_palette_size,
	};
	FILE *file;
	uint32_t row_stride;
	uint32_t pixel_array_size;
	uint32_t file_size;
	unsigned char padding[3] = {0, 0, 0};
	uint32_t row;
	uint32_t palette_index;

	if (!path || !image || !image->apu8VirAddr[0])
		return -1;

	row_stride = (uint32_t)(((uint32_t)image->u16Width + 3u) & ~3u);
	pixel_array_size = row_stride * image->u16Height;
	file_size = bmp_pixel_offset + pixel_array_size;

	file = fopen(path, "wb");
	if (!file)
		return -1;

	if (fputc('B', file) == EOF || fputc('M', file) == EOF)
		goto fail;
	write_u32le(file, file_size);
	write_u16le(file, 0);
	write_u16le(file, 0);
	write_u32le(file, bmp_pixel_offset);

	write_u32le(file, bmp_info_header_size);
	write_u32le(file, image->u16Width);
	write_u32le(file, image->u16Height);
	write_u16le(file, 1);
	write_u16le(file, 8);
	write_u32le(file, 0);
	write_u32le(file, pixel_array_size);
	write_u32le(file, 0);
	write_u32le(file, 0);
	write_u32le(file, bmp_palette_entries);
	write_u32le(file, bmp_palette_entries);

	for (palette_index = 0; palette_index < bmp_palette_entries; ++palette_index) {
		unsigned char entry[4];

		entry[0] = (unsigned char)palette_index;
		entry[1] = (unsigned char)palette_index;
		entry[2] = (unsigned char)palette_index;
		entry[3] = 0;
		if (fwrite(entry, sizeof(entry), 1, file) != 1)
			goto fail;
	}

	for (row = image->u16Height; row > 0; --row) {
		const unsigned char *src_row =
			image->apu8VirAddr[0] + (row - 1u) * image->azu16Stride[0];
		uint32_t padding_size = row_stride - image->u16Width;

		if (fwrite(src_row, image->u16Width, 1, file) != 1)
			goto fail;
		if (padding_size > 0 &&
		    fwrite(padding, padding_size, 1, file) != 1)
			goto fail;
	}

	if (fclose(file) != 0)
		return -1;
	return 0;

fail:
	(void)fclose(file);
	return -1;
}

static void maybe_dump_lk_inputs(OptFlowState *state)
{
	if (!state)
		return;
	if (!state->lk_prev_frame.apu8VirAddr[0] || !state->lk_curr_frame.apu8VirAddr[0])
		return;

	if (write_grayscale_bmp("/tmp/optflow_lk_prev.bmp",
			&state->lk_prev_frame) != 0) {
		fprintf(stderr,
			"[optflow] failed to write /tmp/optflow_lk_prev.bmp\n");
		fflush(stderr);
		return;
	}
	if (write_grayscale_bmp("/tmp/optflow_lk_curr.bmp",
			&state->lk_curr_frame) != 0) {
		fprintf(stderr,
			"[optflow] failed to write /tmp/optflow_lk_curr.bmp\n");
		fflush(stderr);
		return;
	}
}

static const char *optflow_lk_failure_name(OptflowLkFailureReason reason)
{
	switch (reason) {
	case OPTFLOW_LK_FAIL_NONE:
		return "none";
	case OPTFLOW_LK_FAIL_NO_RUNTIME:
		return "no-runtime";
	case OPTFLOW_LK_FAIL_NO_IMAGES:
		return "no-images";
	case OPTFLOW_LK_FAIL_NO_BUFFERS:
		return "no-buffers";
	case OPTFLOW_LK_FAIL_NO_POINTS:
		return "no-points";
	case OPTFLOW_LK_FAIL_TOO_FEW_POINTS:
		return "too-few-points";
	case OPTFLOW_LK_FAIL_LK_CALL:
		return "lk-call";
	case OPTFLOW_LK_FAIL_TOO_FEW_TRACKS:
		return "too-few-tracks";
	default:
		return "unknown";
	}
}

static int should_trace_crash_debug(uint64_t seq)
{
	return seq > 0 && seq <= OPTFLOW_CRASH_TRACE_SEQ_LIMIT;
}

static void log_worker_trace(uint64_t seq, const char *phase)
{
	if (!should_trace_crash_debug(seq))
		return;

	printf("[optflow] trace seq=%" PRIu64 " %s\n", seq, phase);
	fflush(stdout);
}

static void format_point_statuses(char *buffer, size_t buffer_size,
	const MI_S32 *point_statuses, uint32_t point_count)
{
	size_t used = 0;
	uint32_t index;

	if (!buffer || buffer_size == 0)
		return;

	buffer[0] = '\0';
	for (index = 0; index < point_count; ++index) {
		int written = snprintf(buffer + used, buffer_size - used,
			index == 0 ? "%d" : ",%d", point_statuses[index]);

		if (written < 0)
			break;
		if ((size_t)written >= buffer_size - used) {
			used = buffer_size - 1;
			break;
		}
		used += (size_t)written;
	}
}

static void log_lk_failure(const OptFlowState *state, uint64_t seq,
	uint64_t dropped_samples, OptflowLkFailureReason reason,
	const OptflowLkMotion *motion, uint32_t corner_count,
	uint64_t lk_elapsed_ms, const MI_S32 *point_statuses)
{
	char status_buffer[192];

	format_point_statuses(status_buffer, sizeof(status_buffer),
		point_statuses, corner_count);
	printf("[optflow] lk-debug reason=%s seq=%" PRIu64
		" dropped=%" PRIu64 " found=%u used=%u corners=%u lk_ms=%" PRIu64
		" track=%ux%u crop=%ux%u center=%.1f,%.1f status=%s\n",
		optflow_lk_failure_name(reason),
		seq,
		dropped_samples,
		motion->points_found,
		motion->points_used,
		corner_count,
		lk_elapsed_ms,
		state->track_width,
		state->track_height,
		state->track_crop_width,
		state->process_height,
		state->lk_center_x,
		state->lk_center_y,
		status_buffer);
	fflush(stdout);
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

static int prepare_crop_staging_buffers(OptFlowState *state)
{
	uint32_t crop_bytes;
	uint32_t index;

	state->stage_stride = state->track_crop_width;
	state->stage_height = state->process_height;
	crop_bytes = state->stage_stride * state->stage_height;
	for (index = 0; index < OPTFLOW_STAGE_SLOT_COUNT; ++index) {
		state->stage_slots[index].data = calloc(crop_bytes, 1);
		if (!state->stage_slots[index].data)
			goto fail;
	}
	state->worker_prev_crop = calloc(crop_bytes, 1);
	if (!state->worker_prev_crop)
		goto fail;
	state->worker_curr_crop = calloc(crop_bytes, 1);
	if (!state->worker_curr_crop)
		goto fail;
	return 0;

fail:
	free(state->worker_curr_crop);
	state->worker_curr_crop = NULL;
	free(state->worker_prev_crop);
	state->worker_prev_crop = NULL;
	for (index = 0; index < OPTFLOW_STAGE_SLOT_COUNT; ++index) {
		free(state->stage_slots[index].data);
		state->stage_slots[index].data = NULL;
		state->stage_slots[index].seq = 0;
		state->stage_slots[index].ready = 0;
	}
	state->stage_stride = 0;
	state->stage_height = 0;
	return -1;
}

static void release_crop_staging_buffers(OptFlowState *state)
{
	uint32_t index;

	free(state->worker_curr_crop);
	free(state->worker_prev_crop);
	state->worker_curr_crop = NULL;
	state->worker_prev_crop = NULL;
	for (index = 0; index < OPTFLOW_STAGE_SLOT_COUNT; ++index) {
		free(state->stage_slots[index].data);
		state->stage_slots[index].data = NULL;
		state->stage_slots[index].seq = 0;
		state->stage_slots[index].ready = 0;
	}
	state->stage_stride = 0;
	state->stage_height = 0;
	state->stage_write_slot = 0;
	state->worker_have_prev_crop = 0;
}

static int prepare_tracking_buffers(OptFlowState *state)
{
	uint32_t width;
	uint32_t height;
	uint32_t crop_width;
	uint32_t track_bytes;

	crop_width = state->process_width / 2;
	if (crop_width < 64)
		crop_width = state->process_width;
	if (crop_width > state->process_width)
		crop_width = state->process_width;
	state->track_crop_width = crop_width;
	state->track_crop_x = (state->process_width - crop_width) / 2;

	width = crop_width;
	if (width > OPTFLOW_TRACK_WIDTH)
		width = OPTFLOW_TRACK_WIDTH;
	if (width < 64)
		width = 64;

	height = (uint32_t)(((uint64_t)state->process_height * width) /
		crop_width);
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
	state->track_crop_x = 0;
	state->track_crop_width = 0;
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

static uint32_t optflow_osd_max_x(const OptFlowState *state)
{
	if (state->osd_space_width <= STAR6E_OSD_DOT_W)
		return 0;
	return state->osd_space_width - STAR6E_OSD_DOT_W;
}

static uint32_t optflow_osd_max_y(const OptFlowState *state)
{
	if (state->osd_space_height <= STAR6E_OSD_DOT_H)
		return 0;
	return state->osd_space_height - STAR6E_OSD_DOT_H;
}

static void optflow_osd_center_marker(OptFlowState *state)
{
	state->osd_dot_x = optflow_osd_max_x(state) / 2;
	state->osd_dot_y = optflow_osd_max_y(state) / 2;
}

static void optflow_osd_apply_motion(OptFlowState *state,
	const OptflowLkMotion *lk_motion)
{
	int next_x;
	int next_y;
	double scaled_tx;
	double scaled_ty;

	if (!lk_motion->valid)
		return;
	if (state->process_width == 0 || state->process_height == 0)
		return;

	scaled_tx = lk_motion->tx *
		((double)state->osd_space_width / (double)state->process_width);
	scaled_ty = lk_motion->ty *
		((double)state->osd_space_height / (double)state->process_height);
	next_x = (int)state->osd_dot_x + (int)lround(scaled_tx);
	next_y = (int)state->osd_dot_y + (int)lround(scaled_ty);
	state->osd_dot_x = (uint32_t)clamp_int(next_x, 0,
		(int)optflow_osd_max_x(state));
	state->osd_dot_y = (uint32_t)clamp_int(next_y, 0,
		(int)optflow_osd_max_y(state));
}

static const char *optflow_osd_anchor_name(int anchor)
{
	switch (anchor) {
	case 0:
		return "top-left";
	case 1:
		return "center";
	case 2:
		return "bottom-right";
	default:
		return "unknown";
	}
}

static void optflow_osd_set_anchor(OptFlowState *state, int anchor)
{
	switch (anchor) {
	case 0:
		state->osd_dot_x = 0;
		state->osd_dot_y = 0;
		break;
	case 1:
		optflow_osd_center_marker(state);
		break;
	case 2:
		state->osd_dot_x = optflow_osd_max_x(state);
		state->osd_dot_y = optflow_osd_max_y(state);
		break;
	default:
		optflow_osd_center_marker(state);
		break;
	}
}

static void optflow_osd_apply_calibration(OptFlowState *state,
	long long frame_start_ms)
{
	int anchor;
	double norm_x;
	double norm_y;

	anchor = (int)(((frame_start_ms - state->init_ms) /
		OPTFLOW_OSD_CALIBRATION_STEP_MS) % 3);
	optflow_osd_set_anchor(state, anchor);
	if (anchor == state->osd_calibration_anchor)
		return;

	state->osd_calibration_anchor = anchor;
	norm_x = state->osd_space_width > STAR6E_OSD_DOT_W ?
		(double)state->osd_dot_x /
		(double)(state->osd_space_width - STAR6E_OSD_DOT_W) : 0.0;
	norm_y = state->osd_space_height > STAR6E_OSD_DOT_H ?
		(double)state->osd_dot_y /
		(double)(state->osd_space_height - STAR6E_OSD_DOT_H) : 0.0;
	printf("[optflow] osd-cal anchor=%s req=%u,%u max=%u,%u norm=%.3f,%.3f\n",
		optflow_osd_anchor_name(anchor),
		state->osd_dot_x, state->osd_dot_y,
		optflow_osd_max_x(state), optflow_osd_max_y(state),
		norm_x, norm_y);
	fflush(stdout);
}

static unsigned int optflow_osd_cycle_color(uint32_t cycle_ms)
{
	(void)cycle_ms;
	return STAR6E_OSD_COLOR_RED;
}

static void downsample_luma(uint8_t *dst, uint32_t dst_width,
	uint32_t dst_height, uint32_t dst_stride, const uint8_t *src, uint32_t src_stride,
	uint32_t src_width, uint32_t src_height)
{
	uint32_t *x_map;
	uint32_t *y_map;
	uint32_t dst_y;
	uint32_t dst_x;

	x_map = malloc(dst_width * sizeof(*x_map));
	y_map = malloc(dst_height * sizeof(*y_map));
	if (!x_map || !y_map)
		goto fallback;

	for (dst_x = 0; dst_x < dst_width; ++dst_x) {
		x_map[dst_x] = (uint32_t)(((uint64_t)dst_x * src_width) / dst_width);
		if (x_map[dst_x] >= src_width)
			x_map[dst_x] = src_width - 1;
	}

	for (dst_y = 0; dst_y < dst_height; ++dst_y) {
		y_map[dst_y] = (uint32_t)(((uint64_t)dst_y * src_height) / dst_height);
		if (y_map[dst_y] >= src_height)
			y_map[dst_y] = src_height - 1;
	}

	for (dst_y = 0; dst_y < dst_height; ++dst_y) {
		uint32_t src_y = y_map[dst_y];
		uint8_t *dst_row = dst + dst_y * dst_stride;
		const uint8_t *src_row = src + src_y * src_stride;

		for (dst_x = 0; dst_x < dst_width; ++dst_x)
			dst_row[dst_x] = src_row[x_map[dst_x]];
	}

	free(y_map);
	free(x_map);
	return;

fallback:
	free(y_map);
	free(x_map);
	for (dst_y = 0; dst_y < dst_height; ++dst_y) {
		uint32_t src_y = (uint32_t)(((uint64_t)dst_y * src_height) /
			dst_height);

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

static void update_tracking_buffers_from_crops(OptFlowState *state,
	const uint8_t *prev_crop, const uint8_t *curr_crop)
{
	downsample_luma(state->track_prev, state->track_width,
		state->track_height, state->track_width,
		prev_crop, state->stage_stride, state->track_crop_width,
		state->process_height);
	downsample_luma(state->track_curr, state->track_width,
		state->track_height, state->track_width,
		curr_crop, state->stage_stride, state->track_crop_width,
		state->process_height);
}

static unsigned int map_track_x_to_process_x(const OptFlowState *state,
	double track_x)
{
	int process_x;

	process_x = (int)lround(track_x *
		((double)state->track_crop_width / (double)state->track_width));
	process_x += (int)state->track_crop_x;
	return (unsigned int)clamp_int(process_x, 0,
		(int)state->process_width - 1);
}

static unsigned int map_track_y_to_process_y(const OptFlowState *state,
	double track_y)
{
	int process_y;

	process_y = (int)lround(track_y *
		((double)state->process_height / (double)state->track_height));
	return (unsigned int)clamp_int(process_y, 0,
		(int)state->process_height - 1);
}

static void fill_motion_debug_points(OptFlowState *state,
	OptflowLkMotion *motion, const MI_IVE_PointS25Q7_t *points,
	uint32_t point_count)
{
	uint32_t index;

	if (point_count > OPTFLOW_LK_CALL_POINT_COUNT)
		point_count = OPTFLOW_LK_CALL_POINT_COUNT;
	motion->debug_point_count = point_count;
	for (index = 0; index < point_count; ++index) {
		double track_x = (double)points[index].s25q7X / 128.0;
		double track_y = (double)points[index].s25q7Y / 128.0;

		motion->debug_point_x[index] =
			map_track_x_to_process_x(state, track_x);
		motion->debug_point_y[index] =
			map_track_y_to_process_y(state, track_y);
	}
	for (; index < OPTFLOW_LK_CALL_POINT_COUNT; ++index) {
		motion->debug_point_x[index] = 0;
		motion->debug_point_y[index] = 0;
	}
}

static uint64_t compute_corner_score(const uint8_t *image, uint32_t stride,
	int x, int y)
{
	uint64_t sum_xx = 0;
	uint64_t sum_yy = 0;
	int64_t sum_xy = 0;
	int64_t det;
	int64_t trace;
	int64_t trace_penalty;
	int64_t response;
	int window_y;
	int window_x;

	for (window_y = -2; window_y <= 2; ++window_y) {
		const uint8_t *row_prev = image + (y + window_y - 1) * stride;
		const uint8_t *row_curr = image + (y + window_y) * stride;
		const uint8_t *row_next = image + (y + window_y + 1) * stride;

		for (window_x = -2; window_x <= 2; ++window_x) {
			int sample_x = x + window_x;
			int gx = (int)row_curr[sample_x + 1] -
				(int)row_curr[sample_x - 1];
			int gy = (int)row_next[sample_x] -
				(int)row_prev[sample_x];

			sum_xx += (uint64_t)(gx * gx);
			sum_yy += (uint64_t)(gy * gy);
			sum_xy += (int64_t)gx * gy;
		}
	}

	trace = (int64_t)(sum_xx + sum_yy);
	det = (int64_t)(sum_xx * sum_yy) - (sum_xy * sum_xy);
	trace_penalty = (trace * trace) / 16;
	response = det - trace_penalty;
	if (response <= 0)
		return 0;
	return (uint64_t)response;
}

static void insert_corner_candidate(OptflowCornerCandidate *candidates,
	uint32_t *candidate_count, int x, int y, uint64_t score)
{
	uint32_t limit = OPTFLOW_LK_CANDIDATE_CAPACITY;
	uint32_t insert_index;

	if (score == 0)
		return;

	if (*candidate_count < limit) {
		insert_index = *candidate_count;
		(*candidate_count)++;
	} else {
		insert_index = limit - 1u;
		if (score <= candidates[insert_index].score)
			return;
	}

	candidates[insert_index].x = x;
	candidates[insert_index].y = y;
	candidates[insert_index].score = score;
	while (insert_index > 0 &&
	       candidates[insert_index].score > candidates[insert_index - 1u].score) {
		OptflowCornerCandidate tmp = candidates[insert_index - 1u];

		candidates[insert_index - 1u] = candidates[insert_index];
		candidates[insert_index] = tmp;
		insert_index--;
	}
}

static int corner_has_min_spacing(const OptflowCornerCandidate *selected,
	uint32_t selected_count, int x, int y)
{
	uint32_t index;
	int min_distance_sq = OPTFLOW_LK_MIN_POINT_SPACING *
		OPTFLOW_LK_MIN_POINT_SPACING;

	for (index = 0; index < selected_count; ++index) {
		int dx = x - selected[index].x;
		int dy = y - selected[index].y;

		if ((dx * dx) + (dy * dy) < min_distance_sq)
			return 0;
	}

	return 1;
}

static int corner_is_distinct(const OptflowCornerCandidate *selected,
	uint32_t selected_count, int x, int y, int min_distance)
{
	uint32_t index;
	int min_distance_sq = min_distance * min_distance;

	for (index = 0; index < selected_count; ++index) {
		int dx = x - selected[index].x;
		int dy = y - selected[index].y;

		if ((dx * dx) + (dy * dy) < min_distance_sq)
			return 0;
	}

	return 1;
}

static uint32_t append_fallback_points(OptFlowState *state,
	OptflowCornerCandidate *selected, uint32_t selected_count,
	MI_IVE_PointS25Q7_t *points, int usable_margin_x, int usable_margin_y)
{
	static const int fallback_offsets[][2] = {
		{0, 0},
		{-12, 0},
		{12, 0},
		{0, -12},
		{0, 12},
		{-12, -12},
		{12, -12},
		{-12, 12},
		{12, 12},
	};
	uint32_t index;
	int center_x;
	int center_y;

	if (selected_count > 0) {
		uint64_t sum_x = 0;
		uint64_t sum_y = 0;

		for (index = 0; index < selected_count; ++index) {
			sum_x += (uint64_t)selected[index].x;
			sum_y += (uint64_t)selected[index].y;
		}
		center_x = (int)(sum_x / selected_count);
		center_y = (int)(sum_y / selected_count);
	} else if (state->lk_center_ready) {
		center_x = (int)(state->lk_center_x + 0.5);
		center_y = (int)(state->lk_center_y + 0.5);
	} else {
		center_x = (int)state->track_width / 2;
		center_y = (int)state->track_height / 2;
	}

	for (index = 0;
	     index < (sizeof(fallback_offsets) / sizeof(fallback_offsets[0]));
	     ++index) {
		int point_x = clamp_int(center_x + fallback_offsets[index][0],
			usable_margin_x,
			(int)state->track_width - 1 - usable_margin_x);
		int point_y = clamp_int(center_y + fallback_offsets[index][1],
			usable_margin_y,
			(int)state->track_height - 1 - usable_margin_y);

		if (!corner_is_distinct(selected, selected_count,
			point_x, point_y, 4))
			continue;

		selected[selected_count].x = point_x;
		selected[selected_count].y = point_y;
		selected[selected_count].score = 0;
		points[selected_count].s25q7X = (MI_S25Q7)(point_x << 7);
		points[selected_count].s25q7Y = (MI_S25Q7)(point_y << 7);
		selected_count++;
		if (selected_count == OPTFLOW_LK_CALL_POINT_COUNT)
			break;
	}

	return selected_count;
}

static uint32_t point_region_index(int x, int y, int min_x, int min_y,
	int usable_width, int usable_height)
{
	int col;
	int row;

	col = ((x - min_x) * OPTFLOW_LK_REGION_COLS) / usable_width;
	row = ((y - min_y) * OPTFLOW_LK_REGION_ROWS) / usable_height;
	col = clamp_int(col, 0, OPTFLOW_LK_REGION_COLS - 1);
	row = clamp_int(row, 0, OPTFLOW_LK_REGION_ROWS - 1);
	return (uint32_t)(row * OPTFLOW_LK_REGION_COLS + col);
}

static uint32_t select_lk_points(OptFlowState *state,
	MI_IVE_PointS25Q7_t *points, uint32_t max_points)
{
	OptflowCornerCandidate candidates[OPTFLOW_LK_CANDIDATE_CAPACITY];
	OptflowCornerCandidate best_regions[OPTFLOW_LK_REGION_COUNT];
	OptflowCornerCandidate selected[OPTFLOW_LK_CALL_POINT_COUNT];
	int usable_margin_x;
	int usable_margin_y;
	int usable_min_x;
	int usable_min_y;
	int usable_width;
	int usable_height;
	uint64_t sum_x = 0;
	uint64_t sum_y = 0;
	uint32_t candidate_count = 0;
	uint32_t selected_count = 0;
	uint32_t index;
	int y;
	int x;

	if (!state->track_prev || state->track_width == 0 || state->track_height == 0)
		return 0;
	if (max_points < OPTFLOW_LK_CALL_POINT_COUNT)
		return 0;

	usable_margin_x = OPTFLOW_TRACK_PATCH_RADIUS + OPTFLOW_TRACK_SEARCH_RANGE + 1;
	usable_margin_y = OPTFLOW_TRACK_PATCH_RADIUS + OPTFLOW_TRACK_SEARCH_RANGE + 1;
	if ((int)state->track_width <= usable_margin_x * 2 ||
	    (int)state->track_height <= usable_margin_y * 2)
		return 0;
	usable_min_x = usable_margin_x;
	usable_min_y = usable_margin_y;
	usable_width = (int)state->track_width - (usable_margin_x * 2);
	usable_height = (int)state->track_height - (usable_margin_y * 2);
	memset(candidates, 0, sizeof(candidates));
	memset(selected, 0, sizeof(selected));
	memset(best_regions, 0, sizeof(best_regions));
	for (y = usable_margin_y; y < (int)state->track_height - usable_margin_y;
	     y += OPTFLOW_LK_SCAN_STEP) {
		for (x = usable_margin_x;
		     x < (int)state->track_width - usable_margin_x;
		     x += OPTFLOW_LK_SCAN_STEP) {
			uint64_t score = compute_corner_score(state->track_prev,
				state->track_width, x, y);

			insert_corner_candidate(candidates, &candidate_count,
				x, y, score);
			if (OPTFLOW_USE_REGION_POINT_SELECTOR && score > 0) {
				uint32_t region_index = point_region_index(x, y,
					usable_min_x, usable_min_y,
					usable_width, usable_height);

				if (score > best_regions[region_index].score)
					best_regions[region_index] =
						(OptflowCornerCandidate){x, y, score};
			}
		}
	}

	if (!OPTFLOW_USE_REGION_POINT_SELECTOR) {
		for (index = 0; index < candidate_count; ++index) {
			if (!corner_has_min_spacing(selected, selected_count,
				candidates[index].x, candidates[index].y))
				continue;

			selected[selected_count] = candidates[index];
			sum_x += (uint64_t)candidates[index].x;
			sum_y += (uint64_t)candidates[index].y;
			points[selected_count].s25q7X =
				(MI_S25Q7)(candidates[index].x << 7);
			points[selected_count].s25q7Y =
				(MI_S25Q7)(candidates[index].y << 7);
			selected_count++;
			if (selected_count == OPTFLOW_LK_CALL_POINT_COUNT)
				break;
		}

		if (selected_count < OPTFLOW_LK_CALL_POINT_COUNT) {
			selected_count = append_fallback_points(state, selected,
				selected_count, points, usable_margin_x, usable_margin_y);
		}

		if (selected_count > 0) {
			sum_x = 0;
			sum_y = 0;
			for (index = 0; index < selected_count; ++index) {
				sum_x += (uint64_t)(points[index].s25q7X >> 7);
				sum_y += (uint64_t)(points[index].s25q7Y >> 7);
			}
			state->lk_center_x = (double)sum_x / (double)selected_count;
			state->lk_center_y = (double)sum_y / (double)selected_count;
			state->lk_center_ready = 1;
		}

		return selected_count;
	}

	for (index = 0; index < OPTFLOW_LK_REGION_COUNT; ++index) {
		if (best_regions[index].score == 0)
			continue;
		if (!corner_has_min_spacing(selected, selected_count,
			best_regions[index].x, best_regions[index].y))
			continue;

		selected[selected_count] = best_regions[index];
		points[selected_count].s25q7X =
			(MI_S25Q7)(best_regions[index].x << 7);
		points[selected_count].s25q7Y =
			(MI_S25Q7)(best_regions[index].y << 7);
		selected_count++;
	}

	for (index = 0;
	     index < candidate_count && selected_count < OPTFLOW_LK_CALL_POINT_COUNT;
	     ++index) {
		if (!corner_has_min_spacing(selected, selected_count,
			candidates[index].x, candidates[index].y))
			continue;

		selected[selected_count] = candidates[index];
		points[selected_count].s25q7X =
			(MI_S25Q7)(candidates[index].x << 7);
		points[selected_count].s25q7Y =
			(MI_S25Q7)(candidates[index].y << 7);
		selected_count++;
	}

	if (selected_count < OPTFLOW_LK_CALL_POINT_COUNT) {
		selected_count = append_fallback_points(state, selected,
			selected_count, points, usable_margin_x, usable_margin_y);
	}

	if (selected_count > 0) {
		sum_x = 0;
		sum_y = 0;
		for (index = 0; index < selected_count; ++index) {
			sum_x += (uint64_t)(points[index].s25q7X >> 7);
			sum_y += (uint64_t)(points[index].s25q7Y >> 7);
		}
		state->lk_center_x = (double)sum_x / (double)selected_count;
		state->lk_center_y = (double)sum_y / (double)selected_count;
		state->lk_center_ready = 1;
	}

	return selected_count;
}

static int compute_lk_motion(OptFlowState *state, OptflowLkMotion *motion,
	uint64_t seq,
	uint64_t *lk_elapsed_ms, uint32_t *corner_count,
	OptflowLkFailureReason *failure_reason,
	MI_S32 *point_statuses)
{
	MI_IVE_PointS25Q7_t *points;
	MI_IVE_MvS9Q7_t *vectors;
	double average_x = 0.0;
	double average_y = 0.0;
	double rotation_sum = 0.0;
	uint32_t point_count;
	uint32_t used_count = 0;
	uint32_t index;
	long long lk_start_ms;
	MI_S32 ret;

	memset(motion, 0, sizeof(*motion));
	if (failure_reason)
		*failure_reason = OPTFLOW_LK_FAIL_NONE;
	if (point_statuses) {
		for (index = 0; index < OPTFLOW_LK_POINT_COUNT; ++index)
			point_statuses[index] = -999;
	}
	if (lk_elapsed_ms)
		*lk_elapsed_ms = 0;
	if (corner_count)
		*corner_count = 0;
	if (!state->ive_lk_optical_flow) {
		if (failure_reason)
			*failure_reason = OPTFLOW_LK_FAIL_NO_RUNTIME;
		return 0;
	}
	if (!state->lk_prev_frame.apu8VirAddr[0] || !state->lk_curr_frame.apu8VirAddr[0]) {
		if (failure_reason)
			*failure_reason = OPTFLOW_LK_FAIL_NO_IMAGES;
		return 0;
	}
	if (!state->lk_points.pu8VirAddr || !state->lk_motion_vectors.pu8VirAddr) {
		if (failure_reason)
			*failure_reason = OPTFLOW_LK_FAIL_NO_BUFFERS;
		return 0;
	}

	copy_tracking_buffer_to_image(&state->lk_prev_frame, state->track_prev,
		state->track_width, state->track_height);
	copy_tracking_buffer_to_image(&state->lk_curr_frame, state->track_curr,
		state->track_width, state->track_height);

	points = (MI_IVE_PointS25Q7_t *)state->lk_points.pu8VirAddr;
	vectors = (MI_IVE_MvS9Q7_t *)state->lk_motion_vectors.pu8VirAddr;
	memset(points, 0, state->lk_points.u32Size);
	memset(vectors, 0, state->lk_motion_vectors.u32Size);

	log_worker_trace(seq, "compute:select-begin");
	point_count = select_lk_points(state, points,
		state->lk_points.u32Size / sizeof(points[0]));
	if (should_trace_crash_debug(seq)) {
		printf("[optflow] trace seq=%" PRIu64
			" compute:select-end count=%u center=%.1f,%.1f\n",
			seq, point_count, state->lk_center_x, state->lk_center_y);
		fflush(stdout);
	}
	motion->points_found = point_count;
	fill_motion_debug_points(state, motion, points, point_count);
	if (corner_count)
		*corner_count = point_count;
	if (point_count < OPTFLOW_LK_MIN_SURVIVORS) {
		if (failure_reason) {
			*failure_reason = point_count == 0 ?
				OPTFLOW_LK_FAIL_NO_POINTS :
				OPTFLOW_LK_FAIL_TOO_FEW_POINTS;
		}
		return 0;
	}

	state->lk_ctrl.u16CornerNum = (MI_U16)point_count;
	lk_start_ms = monotonic_ms();
	log_worker_trace(seq, "compute:lk-begin");
	ret = state->ive_lk_optical_flow(state->ive_handle,
		&state->lk_prev_frame, &state->lk_curr_frame,
		&state->lk_points, &state->lk_motion_vectors,
		&state->lk_ctrl, 0);
	if (should_trace_crash_debug(seq)) {
		printf("[optflow] trace seq=%" PRIu64
			" compute:lk-end ret=%d\n",
			seq, ret);
		fflush(stdout);
	}
	if (lk_elapsed_ms)
		*lk_elapsed_ms = (uint64_t)(monotonic_ms() - lk_start_ms);
	if (point_statuses) {
		for (index = 0; index < point_count; ++index)
			point_statuses[index] = vectors[index].s32Status;
	}
	if (ret != MI_SUCCESS) {
		if (failure_reason)
			*failure_reason = OPTFLOW_LK_FAIL_LK_CALL;
		return 0;
	}

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
	if (used_count < OPTFLOW_LK_MIN_SURVIVORS) {
		if (failure_reason)
			*failure_reason = OPTFLOW_LK_FAIL_TOO_FEW_TRACKS;
		return 0;
	}

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
	motion->tx = average_x *
		((double)state->track_crop_width / state->track_width);
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

	if (prepare_tracking_buffers(state) != 0)
		return -1;
	if (prepare_crop_staging_buffers(state) != 0)
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
	release_crop_staging_buffers(state);
	release_tracking_buffers(state);
	return -1;
}

static void release_ive_buffers(OptFlowState *state)
{
	free_mem_info(&state->lk_motion_vectors);
	free_mem_info(&state->lk_points);
	free_image(&state->lk_curr_frame);
	free_image(&state->lk_prev_frame);
	release_crop_staging_buffers(state);
	release_tracking_buffers(state);
}

static void copy_tracking_crop(uint8_t *dst, const OptFlowState *state,
	const MI_SYS_BufInfo_t *buf_info)
{
	uint32_t row;
	const uint8_t *src = buf_info->stFrameData.pVirAddr[0];
	uint32_t src_stride = buf_info->stFrameData.u32Stride[0];

	for (row = 0; row < state->process_height; ++row)
		memcpy(dst + row * state->stage_stride,
			src + row * src_stride + state->track_crop_x,
			state->track_crop_width);
}

static int stage_tracking_crop(OptFlowState *state)
{
	union {
		MI_SYS_BufInfo_t info;
		unsigned char raw[512];
	} buf_storage;
	MI_SYS_BufInfo_t *buf_info = &buf_storage.info;
	MI_SYS_BUF_HANDLE buf_handle = 0;
	uint32_t slot_index;
	uint8_t *slot_data;
	MI_S32 ret;

	memset(&buf_storage, 0, sizeof(buf_storage));
	slot_index = state->stage_write_slot;
	slot_data = state->stage_slots[slot_index].data;
	if (!slot_data)
		return -1;

	ret = MI_SYS_ChnOutputPortGetBuf(&state->vpe_port, buf_info, &buf_handle);
	if (ret != MI_SUCCESS)
		return -1;
	trace_frame_debug(state, "getbuf", buf_info, buf_handle);

	if (!frame_buffer_is_accessible(buf_info, state)) {
		trace_frame_debug(state, "reject", buf_info, buf_handle);
		(void)MI_SYS_ChnOutputPortPutBuf(buf_handle);
		return -1;
	}

	copy_tracking_crop(slot_data, state, buf_info);
	(void)MI_SYS_ChnOutputPortPutBuf(buf_handle);

	pthread_mutex_lock(&state->stage_lock);
	state->stage_next_seq++;
	state->stage_slots[slot_index].seq = state->stage_next_seq;
	state->stage_slots[slot_index].ready = 1;
	state->stage_published_slot = slot_index;
	state->stage_published_seq = state->stage_slots[slot_index].seq;
	state->stage_write_slot = (slot_index + 1u) % OPTFLOW_STAGE_SLOT_COUNT;
	pthread_cond_signal(&state->stage_cond);
	pthread_mutex_unlock(&state->stage_lock);
	return 0;
}

static int fetch_latest_motion(OptFlowState *state, OptflowLkMotion *motion,
	uint64_t *seq)
{
	if (!state || !motion || !seq)
		return -1;

	pthread_mutex_lock(&state->result_lock);
	*motion = state->latest_motion;
	*seq = state->latest_motion_seq;
	pthread_mutex_unlock(&state->result_lock);
	return 0;
}

static void publish_motion_result(OptFlowState *state,
	const OptflowLkMotion *motion, uint64_t seq, uint64_t scale_ms,
	uint64_t lk_ms, uint32_t corners)
{
	pthread_mutex_lock(&state->result_lock);
	state->latest_motion = *motion;
	state->latest_motion_seq = seq;
	state->perf_runs++;
	state->perf_scale_ms += scale_ms;
	state->perf_lk_ms += lk_ms;
	state->perf_total_ms += scale_ms + lk_ms;
	state->perf_corner_sum += corners;
	pthread_mutex_unlock(&state->result_lock);
}

static void *optflow_worker_thread(void *arg)
{
	OptFlowState *state = arg;
	uint64_t consumed_seq = 0;

	while (1) {
		uint64_t seq;
		uint64_t dropped_samples;
		uint32_t slot;
		uint64_t scale_ms;
		uint64_t lk_ms;
		uint32_t corners;
		OptflowLkMotion motion;
		OptflowLkFailureReason failure_reason;
		long long scale_start_ms;
		int valid;
		MI_S32 point_statuses[OPTFLOW_LK_POINT_COUNT];

		pthread_mutex_lock(&state->stage_lock);
		while (state->worker_running &&
		       state->stage_published_seq == consumed_seq)
			pthread_cond_wait(&state->stage_cond, &state->stage_lock);
		if (!state->worker_running) {
			pthread_mutex_unlock(&state->stage_lock);
			break;
		}
		seq = state->stage_published_seq;
		dropped_samples = seq > consumed_seq ? seq - consumed_seq - 1u : 0u;
		slot = state->stage_published_slot;
		memcpy(state->worker_curr_crop, state->stage_slots[slot].data,
			state->stage_stride * state->stage_height);
		pthread_mutex_unlock(&state->stage_lock);

		consumed_seq = seq;
		if (!state->worker_have_prev_crop) {
			memcpy(state->worker_prev_crop, state->worker_curr_crop,
				state->stage_stride * state->stage_height);
			state->worker_have_prev_crop = 1;
			continue;
		}

		scale_start_ms = monotonic_ms();
		log_worker_trace(seq, "worker:scale-begin");
		update_tracking_buffers_from_crops(state,
			state->worker_prev_crop, state->worker_curr_crop);
		log_worker_trace(seq, "worker:scale-end");
		scale_ms = (uint64_t)(monotonic_ms() - scale_start_ms);
		log_worker_trace(seq, "worker:compute-begin");
		valid = compute_lk_motion(state, &motion, seq, &lk_ms, &corners,
			&failure_reason, point_statuses);
		log_worker_trace(seq, "worker:compute-end");
		if (!valid) {
			maybe_dump_lk_inputs(state);
			log_lk_failure(state, seq, dropped_samples, failure_reason,
				&motion, corners, lk_ms, point_statuses);
		}
		log_worker_trace(seq, "worker:publish-begin");
		publish_motion_result(state, &motion, seq, scale_ms, lk_ms, corners);
		log_worker_trace(seq, "worker:publish-end");
		if (valid || !OPTFLOW_KEEP_LAST_VALID_REFERENCE)
			swap_buffers(&state->worker_prev_crop, &state->worker_curr_crop);
		log_worker_trace(seq, "worker:swap-end");
	}

	return NULL;
}

static void apply_latest_motion_result(OptFlowState *state, long long now_ms)
{
	OptflowLkMotion motion;
	uint64_t seq;
	uint32_t cycle_ms;
	unsigned int osd_color;
	MI_S32 ret;

	if (fetch_latest_motion(state, &motion, &seq) != 0)
		return;
	if (seq == 0 || seq == state->applied_motion_seq)
		return;
	if (state->show_osd) {
		(void)star6e_osd_set_track_points(motion.debug_point_x,
			motion.debug_point_y, motion.debug_point_count);
	}

	cycle_ms = (uint32_t)(now_ms - state->init_ms);
	osd_color = optflow_osd_cycle_color(cycle_ms);
	if (state->show_osd) {
		if (OPTFLOW_OSD_CALIBRATION_MODE)
			optflow_osd_apply_calibration(state, now_ms);
		else if (cycle_ms < OPTFLOW_OSD_CENTER_HOLD_MS)
			optflow_osd_center_marker(state);
		else
			optflow_osd_apply_motion(state, &motion);
	}

	if (state->show_osd && !state->osd_region_ready &&
	    !state->osd_region_disabled) {
		printf("[optflow] osd: creating region on frame=%" PRIu64 "\n",
			state->frames_seen);
		fflush(stdout);
		ret = star6e_osd_add_dot_region(state->osd_dot_x,
			state->osd_dot_y, osd_color);
		if (ret == MI_SUCCESS) {
			state->osd_region_ready = 1;
			printf("[optflow] osd: region ready\n");
			fflush(stdout);
		} else {
			state->osd_region_disabled = 1;
			fprintf(stderr,
				"[optflow] osd: disabling region after create failure ret=%d\n",
				ret);
			fflush(stderr);
		}
	}

	if (state->show_osd && state->osd_region_ready) {
		ret = star6e_osd_move_dot_region(state->osd_dot_x,
			state->osd_dot_y, osd_color);
		if (ret != MI_SUCCESS) {
			fprintf(stderr,
				"[optflow] osd: move failed ret=%d frame=%" PRIu64
				" x=%u y=%u\n",
				ret, state->frames_seen, state->osd_dot_x, state->osd_dot_y);
			fflush(stderr);
			state->osd_region_ready = 0;
			state->osd_region_disabled = 1;
		}
	}

	state->applied_motion_seq = seq;
	if (motion.valid) {
		printf("[optflow] lk tx=%.1f ty=%.1f rz=%.5f tracks=%u/%u dot=%u,%u frames=%" PRIu64 "\n",
			motion.tx, motion.ty, motion.rz,
			motion.points_used, motion.points_found,
			state->osd_dot_x, state->osd_dot_y,
			state->frames_seen);
		fflush(stdout);
	} else {
		printf("[optflow] lk invalid tracks=%u/%u dot=%u,%u frames=%" PRIu64 "\n",
			motion.points_used, motion.points_found,
			state->osd_dot_x, state->osd_dot_y,
			state->frames_seen);
		fflush(stdout);
	}
}

static void log_perf_summary(OptFlowState *state, long long now_ms)
{
	long long window_ms;
	double avg_corners;
	double rate_hz;
	uint64_t perf_scale_ms;
	uint64_t perf_lk_ms;
	uint64_t perf_total_ms;
	uint64_t perf_corner_sum;
	uint32_t perf_runs;

	window_ms = now_ms - state->perf_window_start_ms;
	if (window_ms <= 0)
		window_ms = 1;
	pthread_mutex_lock(&state->result_lock);
	perf_runs = state->perf_runs;
	perf_scale_ms = state->perf_scale_ms;
	perf_lk_ms = state->perf_lk_ms;
	perf_total_ms = state->perf_total_ms;
	perf_corner_sum = state->perf_corner_sum;
	state->perf_corner_sum = 0;
	state->perf_scale_ms = 0;
	state->perf_lk_ms = 0;
	state->perf_total_ms = 0;
	state->perf_runs = 0;
	pthread_mutex_unlock(&state->result_lock);
	avg_corners = perf_runs ?
		(double)perf_corner_sum / (double)perf_runs : 0.0;
	rate_hz = ((double)perf_runs * 1000.0) / (double)window_ms;

	printf("[optflow] perf: window_ms=%lld runs=%u rate_hz=%.2f scale_ms=%" PRIu64
		" lk_ms=%" PRIu64 " total_ms=%" PRIu64 " avg_corners=%.2f\n",
		window_ms,
		perf_runs,
		rate_hz,
		perf_scale_ms,
		perf_lk_ms,
		perf_total_ms,
		avg_corners);
	fflush(stdout);

	state->perf_window_start_ms = now_ms;
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
		printf("[optflow] lk: tx/ty are image-space pixels; osd-space=%ux%u"
			" track=%ux%u fps=%u\n",
			state->osd_space_width, state->osd_space_height,
			state->track_width, state->track_height, state->process_fps);
	}

	fflush(stdout);
}

OptFlowState *optflow_create(uint32_t capture_width, uint32_t capture_height,
	uint32_t osd_space_width, uint32_t osd_space_height,
	int verbose, uint32_t fps, int show_osd,
	const void *vpe_port, const void *osd_port)
{
	OptFlowState *state = calloc(1, sizeof(*state));
	MI_S32 ret;

	if (!state)
		return NULL;

	state->capture_width = capture_width;
	state->capture_height = capture_height;
	state->osd_space_width = osd_space_width ? osd_space_width : capture_width;
	state->osd_space_height = osd_space_height ? osd_space_height : capture_height;
	state->verbose = verbose ? 1 : 0;
	state->show_osd = show_osd ? 1 : 0;
	state->process_fps = optflow_clamp_fps(fps ? fps : OPTFLOW_DEFAULT_PROCESS_FPS);
	state->process_interval_ms = optflow_interval_ms_from_fps(state->process_fps);
	state->runtime_ive_present = probe_runtime_ive_library();
	state->init_ms = monotonic_ms();
	state->last_log_ms = state->init_ms;
	state->last_process_ms = state->init_ms;
	state->perf_window_start_ms = state->init_ms;
	state->osd_calibration_anchor = -1;
	optflow_osd_center_marker(state);
	state->ive_handle = OPTFLOW_IVE_HANDLE;
	if (pthread_mutex_init(&state->stage_lock, NULL) != 0 ||
	    pthread_cond_init(&state->stage_cond, NULL) != 0 ||
	    pthread_mutex_init(&state->result_lock, NULL) != 0) {
		fprintf(stderr, "[optflow] ERROR: pthread init failed\n");
		state->runtime_ive_present = 0;
	}
	state->lk_ctrl.u0q8MinEigThr = 8; //was 32
	state->lk_ctrl.u8IterCount = 20; //was 10
	state->lk_ctrl.u0q8Epsilon = 4; //was 4
	if (vpe_port) {
		memcpy(&state->vpe_port, vpe_port, sizeof(state->vpe_port));
	}
	if (state->show_osd) {
		(void)star6e_osd_set_canvas_size(state->osd_space_width,
			state->osd_space_height);
		if (osd_port)
			(void)star6e_osd_set_vpe_target(osd_port);
	}

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
	if (state->frame_feed_ready) {
		state->worker_running = 1;
		if (pthread_create(&state->worker_thread, NULL,
		    optflow_worker_thread, state) == 0) {
			state->worker_started = 1;
		} else {
			fprintf(stderr,
				"[optflow] ERROR: worker pthread_create failed\n");
			state->worker_running = 0;
			state->frame_feed_ready = 0;
		}
	}

done:

	log_capability_summary(state, "init");
	return state;
}

void optflow_on_stream(OptFlowState *state, uint32_t pack_count)
{
	long long now_ms;
	int interval_ms;

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
		(void)stage_tracking_crop(state);
	}
	apply_latest_motion_result(state, now_ms);

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
		log_perf_summary(state, now_ms);
	} else {
		printf("[optflow] standby: encoded_frames=%" PRIu64 " packets=%" PRIu64
			" 6dof=unavailable\n",
			state->frames_seen,
			state->packets_seen);
		log_perf_summary(state, now_ms);
	}

	fflush(stdout);
}

void optflow_destroy(OptFlowState *state)
{
	if (!state)
		return;

	if (state->worker_started) {
		pthread_mutex_lock(&state->stage_lock);
		state->worker_running = 0;
		pthread_cond_signal(&state->stage_cond);
		pthread_mutex_unlock(&state->stage_lock);
		pthread_join(state->worker_thread, NULL);
	}
	if (state->show_osd && state->osd_region_ready)
		(void)star6e_osd_remove_dot_region();
	release_ive_buffers(state);
	if (state->ive_created)
		(void)state->ive_destroy(state->ive_handle);
	if (state->ive_library)
		dlclose(state->ive_library);
	pthread_mutex_destroy(&state->result_lock);
	pthread_cond_destroy(&state->stage_cond);
	pthread_mutex_destroy(&state->stage_lock);

	printf("[optflow] stop: uptime_ms=%lld encoded_frames=%" PRIu64
		" packets=%" PRIu64 "\n",
		monotonic_ms() - state->init_ms,
		state->frames_seen,
		state->packets_seen);
	fflush(stdout);
	free(state);
}