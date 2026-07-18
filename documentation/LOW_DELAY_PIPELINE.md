# Low-Delay VENC Pipeline (Ring Buffer Mode)

**Priority: HIGH** | **Effort: HIGH** | **Status: Superseded** — see
`REALTIME_PIPELINE_INVESTIGATION.md` (2026-07-17) for the current
cross-backend analysis and the Star6E/IMX335 MVP plan. The measurements
and the RING+FRAMEBASE-combo finding below remain valid reference.

## Goal

Reduce capture-to-wire latency by switching the VPE-to-VENC link from
FRAMEBASE to REALTIME with ring buffer input mode, allowing the encoder
to begin encoding rows before the full frame is available.

## Measured baseline

Device: SSC30KQ, imx335 1440x1080 @ 90fps, H.265 CBR 8192 kbps.

| Configuration | Avg latency | Notes |
|---|---|---|
| Current (FRAMEBASE) | **5.9 ms** | Encoder waits for complete VPE output |
| VIF frameLineCnt only | 5.8 ms | No measurable improvement (+108 us) |
| Full REALTIME + ring (target) | **~3 ms** (estimated) | Encoder starts before full frame |

Measurement method: `monotonic_us() - stream->packet[0].timestamp` at the
point of RTP transmission, averaged over 1-second intervals (~90 samples).

## Expected gain

At 90fps (11.1 ms frame period), the current 5.9 ms latency means the
encoder pipeline consumes ~53% of the frame budget. With full REALTIME
ring buffer mode, the encoder can start after ~1/4 frame scan time
(~2.8 ms), yielding an estimated 2-3 ms savings:

- **Current**: capture(0) -> VIF full(~11ms) -> VPE(~1ms) -> VENC wait -> encode -> wire = 5.9ms
- **Target**:  capture(0) -> VIF 1/4(~2.8ms) -> VPE streams -> VENC ring -> encode overlapped -> wire = ~3ms
- **Savings**: ~2.8 ms (25% of frame period, ~47% latency reduction)

For FPV, this is significant: 3 ms saved at the encoder compounds with
WiFi link latency and decoder latency on the ground station.

## Implementation plan

### 1. Switch VPE-to-VENC bind to REALTIME

In `bind_and_finalize_pipeline()` (star6e_pipeline.c), change:

```c
// Current:
MI_SYS_BindChnPort2(&vpe_port, &venc_port,
    bind_src_fps, bind_dst_fps, I6_SYS_LINK_FRAMEBASE, 0);

// Target:
MI_SYS_BindChnPort2(&vpe_port, &venc_port,
    bind_src_fps, bind_dst_fps, I6_SYS_LINK_REALTIME, 0);
```

### 2. Enable VENC ring buffer input

After `MI_VENC_CreateChn`, before `MI_VENC_StartRecvPic`:

```c
MI_VENC_InputSourceConfig_t src = {
    .eInputSrcBufferMode = E_MI_VENC_INPUT_MODE_RING_ONE_FRM,
};
MI_VENC_SetInputSourceConfig(chn, &src);
```

API confirmed accepted on I6E firmware (returns 0). The pipeline stall
observed during testing was caused by using RING_ONE_FRM with FRAMEBASE
binding — the combination is invalid. With REALTIME binding, this should
work correctly.

### 3. Set VIF frameLineCnt

```c
port.frameLineCnt = precrop->h / 4;
```

Only meaningful with full REALTIME chain. Alone, it provides no benefit.

### 4. Adjust output port depth

REALTIME binding changes buffer management semantics. The current
`MI_SYS_SetChnOutputPortDepth(&venc_port, 2, 6)` may need tuning:

- REALTIME has tighter coupling — fewer buffers needed for the fast path
- But the recording thread (dual VENC ch1) relies on deep buffers (56 frames)
  for SD card write stall tolerance with FRAMEBASE

## Risks and unknowns

### Dual VENC compatibility

The dual VENC channel (ch1, recording) currently uses FRAMEBASE with a
56-frame deep buffer to absorb SD card write stalls. If VPE→VENC is
globally REALTIME, ch1 may also become REALTIME, reducing stall tolerance.

Options:
- Bind ch0 (streaming) as REALTIME, ch1 (recording) as FRAMEBASE
  (if SigmaStar allows per-channel bind types from the same VPE port)
- Keep ch1 FRAMEBASE with separate bind call
- Accept tighter timing for ch1 with adjusted buffer depth

### GetStream timing

With REALTIME binding, `MI_VENC_GetStream` may have different latency
characteristics. The main loop poll/sleep timing may need adjustment.

### Firmware compatibility

Only tested on one firmware version (SSC30KQ with imx335). Different
SigmaStar firmware versions may behave differently with REALTIME VENC
binding. Should be tested on all target hardware before release.

## Verification plan

1. Switch bind type + ring mode + frameLineCnt
2. Measure latency with same method (capture_us vs ready_us)
3. Verify 89-90 fps maintained (no frame drops)
4. Verify dual VENC recording still works
5. Stress test: high bitrate scene + SD card recording simultaneously
6. Test pipeline reinit (SIGHUP) — REALTIME unbind may have different
   teardown requirements
