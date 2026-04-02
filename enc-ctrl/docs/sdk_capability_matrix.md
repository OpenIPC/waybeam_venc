# MI_VENC SDK Capability Matrix

Analysis of SigmaStar MI_VENC SDK capabilities for the SSC338Q (Infinity6E/6C)
platform, derived from `sdk/ssc338q/hal/star/i6c_venc.h` and
`include/sigmastar_types.h`.

## Per-Frame Data from MI_VENC_GetStream

| Field | Source | Available | Notes |
|-------|--------|-----------|-------|
| Frame size (bytes) | `i6c_venc_pack.length` (sum all packs) | Yes | Also in `h264Info.size` / `h265Info.size` |
| Timestamp | `i6c_venc_pack.timestamp` | Yes | Capture timestamp in microseconds |
| Frame type (I/P) | `i6c_venc_pack.naluType` | Yes | H.264: ISLICE=5, PSLICE=1; H.265: ISLICE=19, PSLICE=1 |
| Sequence number | `i6c_venc_strm.sequence` | Yes | Monotonic per-channel |
| Starting QP | `h264Info.startQual` / `h265Info.startQual` | Yes | Closest available to "average QP" |
| Frame QP | `i6c_venc_pack.frameQual` | Yes | Per-pack quality indicator |
| MB/CU distribution | `h264Info.iMb*`, `h264Info.pMb*`, `h265Info.iCu*`, `h265Info.pCu*` | Yes | Macroblock/CU partition statistics |
| Reference type | `h264Info.refType` / `h265Info.refType` | Yes | Reference frame type |
| Gradient | `i6c_venc_pack.gradient` | Yes | Scene gradient metric (undocumented) |

## Per-Frame Data NOT Available

| Metric | Workaround |
|--------|------------|
| Per-frame average QP | Use `startQual` (starting QP); close but not exact |
| Per-frame max QP | Not available; could parse slice headers |
| SAD / variance | Not exposed; approximate from frame size ratio |
| Encode latency | Measure wall-clock time externally |
| Intra/inter MB ratio | Compute from MB distribution fields |

## Channel-Level from MI_VENC_Query

| Field | Source | Notes |
|-------|--------|-------|
| Current bitrate | `i6c_venc_stat.bitrate` | In kbps |
| FPS | `fpsNum` / `fpsDen` | Current encoding framerate |
| Current packs | `curPacks` | Packs available for read |
| Left pics/bytes | `leftPics`, `leftBytes` | Remaining in buffer |

## Dynamic Control APIs

| API | Function | Works Mid-Stream? | Notes |
|-----|----------|-------------------|-------|
| Request IDR | `MI_VENC_RequestIdr(chn, instant)` | Yes | `instant=1` for immediate; Star6E + Maruko |
| Set channel attr | `MI_VENC_SetChnAttr(chn, attr)` | Yes | Bitrate, GOP changes |
| Get channel attr | `MI_VENC_GetChnAttr(chn, attr)` | Yes | Read current config |
| Set RC params | `MI_VENC_SetRcParam(chn, param)` | Yes | QP range; **Star6E only** |
| Get RC params | `MI_VENC_GetRcParam(chn, param)` | Yes | **Star6E only** |
| Set ROI | `MI_VENC_SetRoiCfg(chn, cfg)` | Yes | Up to 8 regions |
| Frame lost strategy | `MI_VENC_SetFrameLostStrategy` | Yes | **Star6E only** |

## Platform Differences

| Feature | Star6E | Maruko |
|---------|--------|--------|
| `MI_VENC_GetRcParam` / `SetRcParam` | Available | Not available |
| `MI_VENC_GetFrameLostStrategy` | Available | Not available |
| IDR request | `MI_VENC_RequestIdr(chn, instant)` | `MI_VENC_RequestIdr(dev, chn, instant)` (via macro) |
| QP range control | Via `MI_VENC_RcParam_t` | Not directly available |

## Implementation Decisions

1. **Frame stats**: Use `startQual` for QP (best available). Sum pack lengths for
   frame size. NAL type from pack metadata (no bitstream parsing needed).

2. **Scene complexity**: Derived from frame size ratio vs. EMA — zero extra
   computation, surprisingly effective.

3. **IDR insertion**: Via `MI_VENC_RequestIdr` with `instant=1`.

4. **QP boost for IDR size control**: Via `MI_VENC_SetRcParam` (Star6E only).
   On Maruko, QP boost is a no-op — IDR size cannot be constrained.

5. **Bitrate changes**: Via `MI_VENC_SetChnAttr` — works on both platforms.
