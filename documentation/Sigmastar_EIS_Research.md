# SigmaStar SSC338Q EIS API roadmap for IMU-driven stabilization

The SSC338Q (Pudding) SDK provides **a dedicated hardware-accelerated gyroscope EIS path** through the MI_LDC module's `MI_LDC_WORKMODE_DIS_GYRO` mode, which accepts per-frame 3×3 projection matrices with rolling-shutter slice support. This is the primary intended mechanism for IMU-based stabilization. For developers who need more control or whose SDK version lacks the standalone MI_LDC module, the SDK also exposes multiple crop-based stabilization paths through VPE port crop, VPE zoom tables, DIVP dynamic crop, and SCL input crop — all callable at runtime with microsecond-precision frame timestamps from MI_SYS for gyro-to-frame synchronization.

The Pudding-0120 SDK exists in two API generations: the **V2 API** embeds LDC inside VPE (the `MI_VPE_LDC*` functions), while the **V3 API** (SSD268 and newer Pudding builds) breaks LDC into a standalone `MI_LDC` module with the explicit `DIS_GYRO` work mode. Both generations share the same VPE crop, MI_SYS timestamp, and callback infrastructure.

---

## MI_LDC: the dedicated gyro-based EIS engine

The MI_LDC module (`mi_ldc.h`, `libmi_ldc.a`, module ID `E_MI_MODULE_ID_LDC = 23`) is the SDK's built-in EIS subsystem. When configured in `MI_LDC_WORKMODE_DIS_GYRO` mode, it computes per-frame motion compensation from gyroscope data and applies translation/rotation warps to stabilize each frame in hardware.

**Core work mode enum:**
```c
typedef enum {
    MI_LDC_WORKMODE_LDC      = 0x01,  // Lens distortion correction
    MI_LDC_WORKMODE_LUT      = 0x02,  // Direct look-up table warp
    MI_LDC_WORKMODE_DIS_GYRO = 0x04,  // Gyroscope-based digital image stabilization
} MI_LDC_WorkMode_e;
```

**Channel attribute struct (Pudding V3 variant):**
```c
typedef struct MI_LDC_ChnAttr_s {
    MI_LDC_WorkMode_e eMode;           // Set to MI_LDC_WORKMODE_DIS_GYRO
    MI_BOOL bUseProjection3x3Matrix;   // Overlay user 3×3 on internal algorithm's matrix
    MI_S32 as32Projection3x3Matrix[LDC_MAXTRIX_NUM]; // Gyro orientation/placement direction
    MI_U16 u16UserSliceNum;            // Vertical slices for rolling-shutter compensation
    MI_U16 u16CropRatio;               // Output crop % (80 = 80% → 10% margin each edge)
    MI_U16 u16FocalLength;             // Lens focal length in mm
    void *pConfigAddr;                 // LDC config blob address
    MI_U32 u32ConfigSize;
    MI_LDC_MapInfoType_e eInfoType;    // DISPMAP or SENSORCALIB
    union {
        MI_LDC_DisplacementMapInfo_t stDispMapInfo;  // X/Y displacement maps
        MI_LDC_SensorCalibInfo_t stCalibInfo;        // Calibration polynomial
    };
} MI_LDC_ChnAttr_t;
```

The **`u16CropRatio`** parameter is critical — it defines the stabilization margin. A value of **80** means the output retains 80% of the sensor field, leaving **10% margin on each edge** for the warp engine to absorb motion. The **`u16UserSliceNum`** splits each frame into vertical slices for rolling-shutter correction, where each slice receives its own 3×3 homography matrix. On the newer Tiramisu variant, `u16FocalLength` is replaced by separate `u32FocalLengthX`/`u32FocalLengthY` in pixels, and a **`bBypass`** flag allows disabling DIS per-frame.

**Per-frame dynamic update via `MI_LDC_SetChnParam`:**
```c
MI_S32 MI_LDC_SetChnParam(MI_LDC_DEV devId, MI_LDC_CHN chnId, MI_LDC_ChnParam_t *pstChnParam);
```
This is the key runtime API for EIS. `MI_LDC_ChnParam_t` mirrors the channel attributes and can update `as32Projection3x3Matrix`, `bUseProjection3x3Matrix`, `u16FocalLength`, and `bBypass` while the channel is running. A developer's per-frame EIS loop calls `MI_LDC_SetChnParam()` with the gyro-derived 3×3 matrix before each frame is processed.

**Complete lifecycle:**
```c
MI_LDC_CreateDevice(0, &stDevAttr);
MI_LDC_CreateChannel(0, 0, &stChnAttr);  // eMode = MI_LDC_WORKMODE_DIS_GYRO
MI_LDC_StartChannel(0, 0);
// Per-frame loop:
MI_LDC_SetChnParam(0, 0, &stChnParam);   // Update 3×3 matrix from gyro
// Teardown:
MI_LDC_StopChannel(0, 0);
MI_LDC_DestroyChannel(0, 0);
MI_LDC_DestroyDevice(0);
```

The **`MI_LDC_DisplacementMapInfo_t`** struct allows passing raw X/Y pixel displacement maps (`pXmapAddr`/`pYmapAddr`) for arbitrary warp fields, and **`MI_LDC_DoLutDirectTask`** provides a one-shot LUT warp path with explicit source/destination buffers and blending weight tables — useful for offline or custom warp computation.

**Note on MI_GYRO:** The module enum includes `E_MI_MODULE_ID_GYRO = 31`, but **no public `mi_gyro.h` API exists** in any documented SDK version. The gyro data pipeline is internal to the kernel, presumably reading SPI/I2C-connected IMU data and feeding it to the LDC DIS engine. Developers must implement their own userspace gyro reading and feed the computed 3×3 matrices through `MI_LDC_SetChnParam`.

---

## VPE crop and zoom: the per-frame translation path

The VPE module (`mi_vpe.h`, `libmi_vpe.a`) provides several dynamically callable crop APIs that implement 2-DOF (translation-only) EIS without LDC hardware:

**`MI_VPE_SetPortCrop`** — The simplest per-frame stabilization mechanism:
```c
MI_S32 MI_VPE_SetPortCrop(MI_VPE_CHANNEL VpeCh, MI_VPE_PORT VpePort,
                           MI_SYS_WindowRect_t *pstOutCropInfo);
```
Accepts a `MI_SYS_WindowRect_t` with `{u16X, u16Y, u16Width, u16Height}` fields. **Can be called dynamically while the channel is running** — no stop/restart required. The scaler then resizes the cropped region to the port's output resolution. On Pudding, port2 sources from port1's output, so crop cascading is possible.

**`MI_VPE_SetChannelCrop`** — Global input-level crop before all processing:
```c
MI_S32 MI_VPE_SetChannelCrop(MI_VPE_CHANNEL VpeCh, MI_SYS_WindowRect_t *pstCropInfo);
```
Coordinates are relative to the **original sensor resolution**. On Pudding, only the output port matching `u32ChnPortMode` sees the effect.

**`MI_VPE_LoadPortZoomTable` + `MI_VPE_StartPortZoom`** — The SDK's built-in per-frame crop sequencer:
```c
typedef struct MI_VPE_ZoomEntry_s {
    MI_SYS_WindowRect_t stCropWin;   // Crop window for this entry
    MI_U8 u8ZoomSensorId;            // For multi-sensor switching
} MI_VPE_ZoomEntry_t;

MI_S32 MI_VPE_LoadPortZoomTable(VpeCh, VpePort, &zoomTable);
MI_S32 MI_VPE_StartPortZoom(VpeCh, VpePort, &zoomAttr);
```
Load a table of N crop windows, then `StartPortZoom` applies **one entry per frame** automatically stepping from `u32FromEntryIndex` to `u32ToEntryIndex`. This is designed for smooth digital zoom but can be repurposed for EIS: pre-compute a batch of gyro-derived crop windows, load the table, and let the hardware apply them frame-by-frame. The limitation is that `MI_VPE_SetPortCrop` is disabled while zoom is active, and reloading requires a stop-load-start cycle.

**`MI_VPE_SetPortShowPosition`** — Positions the output image within the port buffer with black fill on unused areas, enabling letterbox-style stabilization output.

---

## Frame timing and IMU synchronization

Precise EIS requires correlating IMU samples to exact frame boundaries. The SDK provides three synchronization mechanisms at different privilege levels:

**MI_SYS timestamp APIs** (userspace, `mi_sys.h`):
```c
MI_S32 MI_SYS_GetCurPts(MI_U64 *pu64Pts);           // Read current system PTS (microseconds)
MI_S32 MI_SYS_InitPtsBase(MI_U64 u64PtsBase);        // Set baseline for PTS clock
MI_S32 MI_SYS_SyncPts(MI_U64 u64Pts);                // Synchronize PTS to external clock
```
Every frame buffer carries **`u64Pts`** (presentation timestamp, µs) and **`u32SequenceNumber`** in `MI_SYS_BufInfo_t`. The PTS originates at the sensor/VIF level and propagates through VPE→VENC. Call `MI_SYS_GetCurPts()` when sampling the IMU to place IMU data on the same timebase, then match IMU samples to frames by their `u64Pts`. Use `MI_SYS_InitPtsBase()` to align the MI clock with an external IMU clock if needed.

**`MI_SYS_BufInfo_t`** — the frame metadata carrier:
```c
typedef struct MI_SYS_BufInfo_s {
    MI_U64 u64Pts;              // Frame timestamp (µs)
    MI_U64 u64SidebandMsg;      // Auxiliary sideband channel
    MI_U32 u32SequenceNumber;   // Monotonic frame index
    MI_SYS_BufDataType_e eBufType;
    union {
        MI_SYS_FrameData_t stFrameData;  // Contains stFrameIspInfo
        MI_SYS_MetaData_t stMetaData;    // Metadata buffer
    };
} MI_SYS_BufInfo_t;
```
The **`stFrameIspInfo`** field within `MI_SYS_FrameData_t` carries per-frame ISP metadata. The `u64SidebandMsg` field could carry IMU correlation data as an auxiliary channel.

**VIF frame-start/end callbacks** (kernel-mode only, `mi_vif.h`):
```c
MI_S32 MI_VIF_CallBackTask_Register(MI_VIF_CHN chn, MI_VIF_CallBackParam_t *param);

typedef struct MI_VIF_CallBackParam_s {
    MI_VIF_CallBackMode_e eCallBackMode;  // E_MI_VIF_CALLBACK_ISR or _TASKLET
    MI_VIF_IrqType_e eIrqType;           // FRAMESTART / FRAMEEND / LINEHIT
    MI_VIF_CALLBK_FUNC pfnCallBackFunc;
    MI_U64 u64Data;
} MI_VIF_CallBackParam_t;
```
`E_MI_VIF_IRQ_FRAMESTART` fires at the first pixel of each frame — the ideal moment to timestamp and correlate gyro data. `E_MI_VIF_IRQ_FRAMEEND` confirms frame completion. These require a kernel module.

**VPE ISP callbacks** (kernel-mode only, `mi_vpe.h`):
```c
MI_S32 MI_VPE_CallBackTask_Register(MI_VPE_CallBackParam_t *param);
// eIrqType: E_MI_VPE_IRQ_ISPVSYNC (frame start) or E_MI_VPE_IRQ_ISPFRAMEDONE
```
`E_MI_VPE_IRQ_ISPVSYNC` triggers at frame-start within the ISP, and `E_MI_VPE_IRQ_ISPFRAMEDONE` when ISP write-out completes. Both are interrupt-context callbacks.

**Userspace frame polling** — for applications without kernel access:
```c
MI_SYS_GetFd(&stChnPort, &s32Fd);           // Get pollable fd
select(s32Fd + 1, &read_fds, NULL, NULL, &timeout);
MI_SYS_ChnOutputPortGetBuf(&stChnPort, &stBufInfo, &hHandle);
// stBufInfo.u64Pts contains the frame timestamp
```

---

## ISP per-frame statistics and exposure metadata

The CUS3A (Custom 3A) framework in `mi_isp.h` is the most powerful per-frame data source. The ISP calls registered `run()` callbacks at each frame interrupt, delivering hardware-computed statistics:

- **AE statistics**: 32×32 blocks on Pudding (128×90 on other platforms), each containing `{R_avg, G_avg, B_avg, Y_avg}` as `ISP_AE_SAMPLE`
- **AWB statistics**: Per-block `{R, G, B}` center-pixel samples as `ISP_AWB_SAMPLE`
- **Per-frame exposure**: `MI_ISP_AE_QueryExposureInfo()` returns current shutter time, sensor gain, ISP gain
- **Sensor-level exposure**: `MI_SNR_GetPlaneInfo()` returns `u32ShutterUs` and `u32SensorGainX1024`

Registration via `CUS3A_RegInterface()` (called before `MI_SYS_Init()`) enables frame-synchronized callbacks where EIS algorithms can correlate frame statistics with IMU data. The exposure time from `u32ShutterUs` is essential for modeling motion blur extent in the stabilization algorithm.

---

## DIVP and SCL: alternative dynamic crop paths

**MI_DIVP** (`mi_divp.h`, V2 SDK) provides a standalone crop+scale+rotate module:
```c
MI_S32 MI_DIVP_SetChnAttr(MI_DIVP_CHN chn, MI_DIVP_ChnAttr_t *pstAttr);

typedef struct MI_DIVP_ChnAttr_s {
    MI_U32 u32MaxWidth;
    MI_U32 u32MaxHeight;
    MI_SYS_Rotate_e eRotateType;       // 0/90/180/270
    MI_SYS_WindowRect_t stCropRect;    // Dynamic crop window
    MI_BOOL bHorMirror;
    MI_BOOL bVerMirror;
} MI_DIVP_ChnAttr_t;
```
`MI_DIVP_SetChnAttr()` can be called per-frame to shift the `stCropRect`, implementing translation-based EIS. DIVP also supports rotation, enabling 3-DOF stabilization when combined with crop. The `MI_DIVP_StretchBuf()` function provides one-shot crop+scale on arbitrary physical memory buffers.

**MI_SCL** (`mi_scl.h`, V3 SDK) replaces DIVP on newer platforms:
```c
MI_S32 MI_SCL_SetInputPortCrop(MI_SCL_DEV DevId, MI_SCL_CHANNEL ChnId,
                                MI_SYS_WindowRect_t *pstCropInfo);
MI_S32 MI_SCL_SetOutputPortParam(DevId, ChnId, PortId, MI_SCL_OutPortParam_t *param);
```
`MI_SCL_SetInputPortCrop` is the V3 equivalent of DIVP crop for per-frame EIS compensation.

---

## VENC crop and pipeline binding considerations

**`MI_VENC_SetCrop`** (`mi_venc.h`) applies a crop rectangle at the encoder input:
```c
MI_S32 MI_VENC_SetCrop(MI_VENC_CHN VeChn, MI_VENC_CropCfg_t *pstCropCfg);
```
This can serve as a last-resort EIS crop point, but introduces encoding-level artifacts and is better suited for static ROI extraction than per-frame stabilization.

**Binding mode selection** is critical for EIS. Use **`E_MI_SYS_BIND_TYPE_FRAME_BASE`** between VPE and VENC when frame interception is needed — this places full frames in DRAM where timestamps are accessible. `E_MI_SYS_BIND_TYPE_REALTIME` (direct HW connection, zero DRAM copy) and `E_MI_SYS_BIND_TYPE_HW_RING` (shared ring buffer) do **not** support `MI_SYS_ChnOutputPortGetBuf/PutBuf`, making frame-level EIS interception impossible in those modes.

---

## Recommended EIS implementation strategy

For a developer implementing IMU-driven EIS on the SSC338Q, the optimal approach depends on available SDK version and stabilization quality requirements:

- **Best path (V3 SDK with MI_LDC):** Configure `MI_LDC_WORKMODE_DIS_GYRO`, set `u16CropRatio` to ~80 for 10% stabilization margin, set `u16UserSliceNum` for rolling-shutter slice count matching the IMU sample rate per frame. In the main loop, read gyroscope data, compute a 3×3 homography matrix per frame (or per slice), and call `MI_LDC_SetChnParam()` to feed the matrix to the hardware warp engine. The LDC engine handles sub-pixel warp, distortion correction, and rolling-shutter compensation in a single hardware pass.

- **Translation-only path (V2 SDK or simpler EIS):** Use `MI_VPE_SetPortCrop()` called per-frame from userspace, with frames timestamped via `MI_SYS_ChnOutputPortGetBuf()` → `u64Pts` for IMU correlation. Oversample the sensor by 10–20% beyond the output resolution to create a stabilization margin. For batch processing, load crop sequences via `MI_VPE_LoadPortZoomTable`.

- **Frame synchronization:** In userspace, use `MI_SYS_GetCurPts()` when sampling the IMU and match against frame `u64Pts`. In kernel space, register `MI_VIF_CallBackTask_Register()` with `E_MI_VIF_IRQ_FRAMESTART` to capture the exact moment each frame begins, enabling sub-frame gyro integration windows.

- **Exposure-aware stabilization:** Query `MI_ISP_AE_QueryExposureInfo()` or `MI_SNR_GetPlaneInfo()` per-frame to obtain shutter time (`u32ShutterUs`), which determines the gyro integration window for motion blur modeling.

| SDK Module | Header | Library | Primary EIS API | Capability |
|---|---|---|---|---|
| **MI_LDC** | `mi_ldc.h` | `libmi_ldc.a` | `MI_LDC_SetChnParam()` | HW warp, 3×3 matrix, RS slices, DIS_GYRO mode |
| **MI_VPE** | `mi_vpe.h` | `libmi_vpe.a` | `MI_VPE_SetPortCrop()` | Per-frame dynamic crop (translation EIS) |
| **MI_VPE** | `mi_vpe.h` | `libmi_vpe.a` | `MI_VPE_LoadPortZoomTable()` | Batch per-frame crop table (auto-stepped) |
| **MI_SYS** | `mi_sys.h` | `libmi_sys.a` | `MI_SYS_GetCurPts()` | Frame timestamps (µs) for IMU sync |
| **MI_VIF** | `mi_vif.h` | `libmi_vif.a` | `MI_VIF_CallBackTask_Register()` | Frame-start/end IRQ callbacks (kernel) |
| **MI_ISP** | `mi_isp.h` | `libmi_isp.a` | `MI_ISP_AE_QueryExposureInfo()` | Per-frame exposure/gain metadata |
| **MI_DIVP** | `mi_divp.h` | `libmi_divp.a` | `MI_DIVP_SetChnAttr()` | Dynamic crop + rotate (V2 SDK) |
| **MI_SCL** | `mi_scl.h` | `libmi_scl.a` | `MI_SCL_SetInputPortCrop()` | Dynamic crop (V3 SDK) |
| **MI_VENC** | `mi_venc.h` | `libmi_venc.a` | `MI_VENC_SetCrop()` | Encoder-level crop (static ROI) |

## Conclusion

The SSC338Q SDK's EIS capability is more mature than its sparse documentation suggests. The `MI_LDC_WORKMODE_DIS_GYRO` path provides **hardware-accelerated, per-frame 3×3 homography warp with rolling-shutter slice support** — comparable to what commercial action cameras and drones use. The internal `E_MI_MODULE_ID_GYRO` module handles gyro-to-LDC data flow in kernel space, but since no public `mi_gyro.h` API exists, developers must implement their own userspace IMU driver (SPI/I2C to MPU6050/ICM-series), compute the per-frame transformation matrices, and feed them through `MI_LDC_SetChnParam()`. The `u64Pts` timestamp from `MI_SYS_BufInfo_t` and `MI_SYS_GetCurPts()` provide the microsecond-resolution clock needed to correlate IMU samples with video frames. For simpler deployments, `MI_VPE_SetPortCrop()` with a 10–20% overscan margin delivers translation-only stabilization with zero additional hardware cost.