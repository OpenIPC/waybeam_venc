# ROI (Region of Interest) QP — Horizontal Bands

Status: **Implemented**. Signed center-emphasis ROI with stepped horizontal
bands, verified on SSC30KQ (Infinity6E) at 1080p.

## Implementation

Full-height horizontal bands centered on the frame, with signed delta QP
tapering from center (strongest) to edges (weakest). Higher-index ROI
regions override lower ones in overlap zones, so the innermost (narrowest)
band gets the full QP delta.

```
┌─────┬─────┬──────────┬─────┬─────┐
│     │     │          │     │     │
│ qp  │ qp  │   full   │ qp  │ qp  │   full height
│ /4  │ /2  │   qp     │ /2  │ /4  │   bands
│     │     │ (center) │     │     │
└─────┴─────┴──────────┴─────┴─────┘
  edge  mid    center    mid   edge     (2 steps shown)
```

- Negative `roiQp`: higher quality in center (sharper, lower QP)
- Positive `roiQp`: lower quality in center (softer, higher QP)
- `0`: ROI disabled (all regions cleared)

All rect coordinates are aligned to 32 pixels for H.265 CTU compatibility.

## Config

```json
"fpv": {
    "roiEnabled": true,
    "roiQp": -18,
    "roiSteps": 2,
    "roiCenter": 0.25
}
```

- `roiEnabled`: bool, MUT_LIVE — enable/disable ROI bands
- `roiQp`: int, MUT_LIVE, range `-30..30`
- `roiSteps`: uint16, MUT_LIVE, range `1..4`
- `roiCenter`: double, MUT_LIVE, range `0.1..0.9`

## Example

With `roiCenter=0.33`, `roiQp=-18`, `roiSteps=2`:

| Band | Width | QP delta |
|------|-------|----------|
| Outer (index 0) | ~67% of frame | -9 |
| Center (index 1) | ~33% of frame | -18 |

The outer band covers the full frame width. The center band overlaps
the middle third. Since higher-index regions override, the center gets
-18 and the edges get -9.

## Live API

```text
GET /api/v1/set?fpv.roi_enabled=true
GET /api/v1/set?fpv.roi_qp=-18
GET /api/v1/set?fpv.roi_steps=2
GET /api/v1/set?fpv.roi_center=0.25
GET /api/v1/set?fpv.roi_qp=0          # disable
```

## ABI Findings

### MI_VENC_RoiCfg_t

```c
typedef struct {
    MI_U32  u32Index;
    MI_BOOL bEnable;
    MI_BOOL bAbsQp;
    MI_S32  s32Qp;
    MI_VENC_Rect_t stRect;
} MI_VENC_RoiCfg_t;
```

- Up to 4 ROI regions used (index 0-3)
- `MI_VENC_Rect_t` uses `MI_U32` fields
- Unused regions are explicitly disabled

### Functions

```c
MI_S32 MI_VENC_SetRoiCfg(MI_VENC_CHN chn, MI_VENC_RoiCfg_t *cfg);
MI_S32 MI_VENC_GetRoiCfg(MI_VENC_CHN chn, MI_U32 idx, MI_VENC_RoiCfg_t *cfg);
```

On Maruko, both functions take an additional `MI_VENC_DEV dev` parameter.

## Constraints

- 32-pixel alignment required for ROI rects
- Signed delta range: `-30..30` (SDK rejects values outside this)
- Full-frame height must be 32-pixel aligned (1080 → 1056 used)

## Previous Implementation

The original implementation used concentric centered rectangles (centerbox
mode) with unsigned QP and up to 8 regions. This was replaced with the
simpler horizontal band layout which better matches the FPV use case
(center of frame is always the most important regardless of vertical
position).
