# AWB (Auto White Balance) Investigation

Status: **Implemented and verified** on SSC30KQ (Infinity6E) at 1080p90.

## Findings

AWB is fully functional. The ISP bin provides calibration data and the
CUS3A framework drives AWB at ~15 Hz via `MI_ISP_CUS3A_Enable`.

Despite the ISP library's debug log showing `AWB = 0` on periodic refresh
calls, the AWB algorithm IS running — gains are non-unity and color
temperature is being tracked.

### Measured AWB State (auto mode, indoor lighting)

| Metric | Value |
|--------|-------|
| Color temperature | 5000K |
| R gain | 2094 (2.04x) |
| G gain | 1024 (1.0x) |
| B gain | 2531 (2.47x) |
| Stable | yes |
| HW statistics | 128x90 blocks |
| Stabilizer | disabled |

## Implementation

### Diagnostic Endpoint

`GET /api/v1/awb` — returns full AWB state as JSON:

```json
{
  "ok": true,
  "data": {
    "query_info": {
      "ret": 0, "stable": true,
      "r_gain": 2094, "gr_gain": 1024, "gb_gain": 1024, "b_gain": 2531,
      "color_temp": 5000, "wp_index": 3,
      "multi_ls": false, "ls1_idx": 0, "ls2_idx": 0
    },
    "cus3a_status": {
      "ret": 0,
      "avg_blk_x": 128, "avg_blk_y": 90,
      "cur_r_gain": 2094, "cur_g_gain": 1024, "cur_b_gain": 2531,
      "hdr_mode": 0, "bv_x16384": 0, "weight_y": 0
    },
    "stabilizer": {
      "ret": 0, "enabled": false,
      "glb_gain_thd": 160, "count_thd": 31, "force_tri_thd": 2555
    },
    "attr": {
      "ret": 0, "state": 0, "mode": "auto", "mode_raw": 0,
      "mwb_r": 2082, "mwb_gr": 1024, "mwb_gb": 1024, "mwb_b": 2907
    }
  }
}
```

Queries four SDK APIs via dlopen:
- `MI_ISP_AWB_QueryInfo` — gains, color temp, stability, white point
- `MI_ISP_CUS3A_GetAwbStatus` — HW statistics block info, current gains
- `MI_ISP_AWB_GetStabilizer` — stabilizer configuration
- `MI_ISP_AWB_GetAttr` — mode (auto/manual/ct_manual), MWB gains

### AWB Mode Control

```json
"isp": {
    "awbMode": "auto",
    "awbCt": 5500
}
```

- `awbMode`: string, MUT_LIVE — `"auto"` or `"ct_manual"`
- `awbCt`: uint32, MUT_LIVE — color temperature in Kelvin (for ct_manual mode)

### Live API

```
GET /api/v1/awb                          # query full AWB state
GET /api/v1/set?isp.awb_mode=ct_manual   # lock to specific color temp
GET /api/v1/set?isp.awb_ct=3000          # warm (3000K)
GET /api/v1/set?isp.awb_ct=5500          # daylight (5500K)
GET /api/v1/set?isp.awb_ct=10000         # cold/blue (10000K)
GET /api/v1/set?isp.awb_mode=auto        # return to auto
```

### SDK APIs Used

| Function | Direction | Purpose |
|----------|-----------|---------|
| `MI_ISP_AWB_QueryInfo` | Read | Gains, CT, stability |
| `MI_ISP_CUS3A_GetAwbStatus` | Read | HW stats summary |
| `MI_ISP_AWB_GetStabilizer` | Read | Stabilizer config |
| `MI_ISP_AWB_GetAttr` | Read/Write | Mode, algorithm settings |
| `MI_ISP_AWB_SetAttr` | Write | Switch AWB mode |
| `MI_ISP_AWB_SetCTMwbAttr` | Write | Set target color temp |

## Test Results (SSC30KQ, 1080p90 H.265 CBR 8192kbps)

| Test | Result |
|------|--------|
| Auto AWB running at boot | Pass — gains non-unity, CT tracked |
| `/api/v1/awb` diagnostic | Pass — all 4 queries return ret=0 |
| Set ct_manual 5500K | Pass — CT=5500, gains shift |
| Set ct_manual 3000K | Pass — R=1170, B=4452 (warm) |
| Set ct_manual 10000K | Pass — R=2250, B=1402 (cold) |
| Return to auto | Pass — gains restore to auto-computed |
| Stream stability during mode switch | Pass — no frame drops, 90fps |

## CUS3A "AWB = 0" Debug Log

The ISP library prints `[MI_ISP_CUS3A_Enable] AE = 1, AWB = 0, AF = 0`
on periodic refresh calls. This does NOT mean AWB is disabled — it appears
to be the library's internal return value indicating no new AWB computation
was triggered on that specific call (the AWB algorithm runs asynchronously
in the ISP hardware and converges independently). The gains and CT reported
by `MI_ISP_AWB_QueryInfo` confirm AWB is active and producing valid output.

## Struct Layouts

From the SigmaStar ISP 3A datatype header:

```c
/* Query result */
typedef struct {
    MI_ISP_BOOL_e bIsStable;
    MI_U16 u16Rgain, u16Grgain, u16Gbgain, u16Bgain;
    MI_U16 u16ColorTemp;
    MI_U8  u8WPInd;
    MI_ISP_BOOL_e bMultiLSDetected;
    MI_U8  u8FirstLSInd, u8SecondLSInd;
} MI_ISP_AWB_QUERY_INFO_TYPE_t;

/* CUS3A HW stats info (packed) */
typedef struct {
    MI_U32 Size, AvgBlkX, AvgBlkY;
    MI_U32 CurRGain, CurGGain, CurBGain;
    void *avgs;
    MI_U8  HDRMode;
    void  **pAwbStatisShort;
    MI_U32 u4BVx16384;
    MI_S32 WeightY;
} __attribute__((packed, aligned(1))) CusAWBInfo_t;

/* CT manual white balance */
typedef struct { MI_U32 u32CT; } MI_ISP_AWB_CTMWB_PARAM_t;

/* AWB mode */
typedef enum {
    SS_AWB_MODE_AUTO = 0,
    SS_AWB_MODE_MANUAL = 1,
    SS_AWB_MODE_CTMANUAL = 2,
} MI_ISP_AWB_MODE_TYPE_e;
```

## Future Work

- Stabilizer tuning — currently disabled; `MI_ISP_AWB_SetStabilizer` can
  adjust convergence behavior for outdoor/changing light
- Manual MWB (raw gain control) — `SS_AWB_MODE_MANUAL` with direct R/G/B
  gain values via `MI_ISP_AWB_SetAttr.stManualParaAPI`
- AWB speed tuning — `AWB_ATTR_PARAM_t.u8Speed` (1..100) controls convergence rate
