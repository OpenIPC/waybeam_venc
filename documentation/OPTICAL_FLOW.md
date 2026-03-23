# Optical Flow

## Purpose

The current Star6E optflow path is a lightweight motion estimator that runs
alongside the encoder stream loop.

It currently produces one motion output:

1. An LK optical-flow estimate:
  - `lk tx`: horizontal image translation in full-frame pixels
  - `lk ty`: vertical image translation in full-frame pixels
  - `lk rz`: image-plane rotation proxy derived from tracked feature motion

This is not a full 6-DoF visual odometry system and it is not a strict sparse
Lucas-Kanade implementation. The current hardcoded mode disables the older SAD
ROI path and the CPU planar estimator and keeps only the vendor LK optical
flow path on a downsampled `U8C1` image pair.

## Where It Runs

The runtime hook is in the Star6E encoder loop in `src/star6e_runtime.c`.

After each successful encoded-frame fetch with `MI_VENC_GetStream(...)`, the
runtime calls:

- `optflow_on_stream(ps->optflow, stream.count);`

This means optflow is driven by the encoded stream cadence, not by a separate
sensor thread.

## Processing Cadence

`optflow_on_stream(...)` is called for every encoded frame, but the expensive
motion processing is rate-limited.

Current interval:
- configured by `optflow.fps` in `venc.json`
- default value: 5
- default effective interval: about 200 ms
- default effective processing rate: about 5 Hz

So:
- encoded stream rate can be around 60 fps
- motion estimation runs at the configured `optflow.fps` rate, bounded by the
  actual incoming frame cadence

## Frame Source

The optflow module does not read raw sensor memory directly from userspace.

Instead, it requests a frame from the configured VPE output port through:

- `MI_SYS_ChnOutputPortGetBuf(...)`

That means the motion logic works on the luma plane of the video pipeline frame
that is already flowing through SigmaStar SYS/VPE, downstream of the sensor.

## Post-GetBuf Pipeline

The core per-frame logic lives in `grab_frame_and_detect(...)`.

Once `MI_SYS_ChnOutputPortGetBuf(...)` succeeds, the following steps happen.

### 1. Validate the returned frame buffer

The buffer is checked with `frame_buffer_is_accessible(...)`.

This rejects frames that are not safe or useful for processing, for example
when:
- the virtual luma pointer is missing
- stride is too small
- frame dimensions do not match the configured processing assumptions

If validation fails:
- the buffer is returned with `MI_SYS_ChnOutputPortPutBuf(...)`
- the function returns without producing motion output

### 2. Bootstrap the previous-frame history on first use

If there is no previous frame yet:
- `copy_luma_to_prev(...)` copies the current luma plane into the persistent
  previous-frame buffer
- `have_prev_frame` is set
- the buffer is returned immediately
- no LK motion is emitted for this first frame

This is required because the LK path compares the current frame against the
previous frame.

### 3. Downsample previous and current luma

Both frames are downsampled into persistent tracking buffers.

Current implementation:
- target width up to 320 pixels
- height scaled to preserve aspect ratio
- minimum size clamped to avoid degenerate cases

The goal is to reduce CPU cost while keeping enough structure for LK tracking.

### 4. Run LK optical flow

If the runtime exports `MI_IVE_LkOpticalFlow(...)`, the tracker runs the LK
path on the downsampled luma pair.

Important implementation constraints from the SigmaStar SDK:
- LK input size must stay within `64x64` to `720x576`
- input and output physical addresses must be 16-byte aligned
- stride must be 16-pixel aligned

To satisfy that:
- the existing downsampled tracking view is copied into dedicated IVE images
- a small persistent point cluster around a tracked image-center estimate is
  written into an IVE memory buffer
- the LK motion-vector output is written into a separate IVE memory buffer

The current LK path follows a simplified single-layer pattern closer to the
standalone SDK samples:
- it does not use a separate hardware corner detector
- it tracks a small center-following point cluster
- it reduces the resulting LK vectors into translation and an image-plane
  rotation proxy

The returned per-point LK motion vectors are then reduced into:
- `lk tx`
- `lk ty`
- `lk rz`

`lk rz` is an image-plane rotation proxy from the residual flow field around
the image center. It is not a full 3D yaw/pitch/roll estimate.

### 5. Update previous-frame history

After LK motion is computed, the current luma plane is copied into the
persistent previous-frame buffer with `copy_luma_to_prev(...)`.

This prepares the state for the next processing cycle.

### 6. Return the frame buffer

The borrowed buffer is always returned through:

- `MI_SYS_ChnOutputPortPutBuf(...)`

The module does not retain the live SYS buffer after the function returns.

## Logging Behavior

`optflow_on_stream(...)` emits three kinds of log lines.

### 1. Stream-online startup message

Printed when the first encoded frame is observed.

Purpose:
- confirms the stream loop is feeding the tracker
- reports whether the tracker is active or only in standby

### 2. Periodic active or standby status

Printed on a slower heartbeat.

Active example fields:
- encoded frame count
- packet count
- LK tracking buffer size

This confirms the tracker is alive even when no strong motion is happening.

### 3. Motion outputs

#### LK log

Example shape:
- `[optflow] lk tx=... ty=... rz=... tracks=used/found ...`

Meaning:
- `lk tx`, `lk ty`: LK-derived image-plane translation
- `lk rz`: LK-derived image-plane rotation proxy
- `found`: candidate textured points passed to LK
- `used`: LK tracks with successful status used in reduction

## Current Limitations

The current implementation is intentionally simple.

Known limits:
- no true 3D reconstruction
- no metric depth or metric XYZ output
- no camera intrinsics or homography solve
- no IMU fusion
- no rotation compensation
- no feature identity persistence across long sequences
- no explicit moving-object segmentation
- configured processing cadence is clamped to the supported range
- `lk rz` is only an image-plane rotation proxy, not full 3D attitude

Because of that, the current output should be interpreted as:
- a useful relative image-motion signal
- not a final navigation-grade pose estimator

## Summary

After a frame is retrieved from the VPE output port, the current logic does:

1. validate the buffer
2. bootstrap previous frame if needed
3. downsample previous and current luma into tracking buffers
4. run LK optical flow on the reduced image pair
5. derive `lk tx`, `lk ty`, and `lk rz`
6. copy current luma into previous-frame state
7. return the SYS buffer

This gives the project a low-cost relative motion signal that is already useful
for debugging and future stabilization or guidance work, while staying within
the current Star6E pipeline architecture.