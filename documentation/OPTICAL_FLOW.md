# Optical Flow

## Purpose

The current Star6E optflow path is a lightweight motion estimator that runs
alongside the encoder stream loop.

It produces two outputs:

1. A coarse motion ROI based on SigmaStar IVE SAD block differences.
2. A relative planar motion estimate:
   - `tx`: horizontal image translation in full-frame pixels
   - `ty`: vertical image translation in full-frame pixels
   - `tz`: unitless relative surface-scale term
     - positive means the camera appears to move closer to the scene
     - negative means the camera appears to move farther away

This is not a full 6-DoF visual odometry system and it is not a strict sparse
Lucas-Kanade implementation. It is a hybrid of:
- hardware-assisted SAD change detection
- CPU-side patch matching on downsampled luma frames

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
- one processing pass every 200 ms
- effective processing rate: about 5 Hz

So:
- encoded stream rate can be around 60 fps
- motion estimation still only runs around 5 times per second

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

### 2. Wrap the current luma plane as an IVE image

`wrap_current_luma(...)` builds a lightweight `MI_IVE_Image_t` view over the
current frame.

Important details:
- image type is single-channel luma (`U8C1`)
- width and height are the processed frame dimensions
- stride and addresses point directly at the current frame buffer
- no copy happens here

This gives the IVE SAD path direct access to the current luma plane.

### 3. Bootstrap the previous-frame history on first use

If there is no previous frame yet:
- `copy_luma_to_prev(...)` copies the current luma plane into the persistent
  previous-frame buffer
- `have_prev_frame` is set
- the buffer is returned immediately
- no ROI or planar motion is emitted for this first frame

This is required because both motion paths compare current frame against the
previous frame.

### 4. Run SigmaStar IVE SAD between previous and current luma

For non-bootstrap frames, the code calls:

- `state->ive_sad(...)`

Inputs:
- previous frame
- current frame

Outputs:
- `sad_result`
- `thr_result`

Configuration:
- 8x8 SAD mode
- thresholded output enabled
- threshold derived from:
  - block size
  - configured per-pixel difference threshold

Conceptually:
- the hardware computes block-wise absolute differences
- blocks whose summed difference exceeds the threshold are marked as motion

If the SAD call fails:
- current luma is still copied into the previous-frame buffer
- the frame buffer is returned
- processing exits with failure for that cycle

### 5. Convert the thresholded SAD map into a motion ROI

`compute_motion_box(...)` scans the thresholded SAD output.

What it does:
- searches all non-zero motion blocks
- finds min/max block coordinates
- expands the box by one block of padding when possible
- scales block coordinates back to full-frame pixel coordinates

Output:
- `x`, `y`, `width`, `height`

If no thresholded blocks are active:
- ROI becomes zero-sized
- this is treated as no coarse motion region

This ROI is useful as a cheap where-did-motion-happen signal, but by itself it
does not estimate camera translation.

### 6. Run CPU-side planar motion estimation

If the frame got far enough for ROI evaluation, the code then calls:

- `compute_planar_motion(...)`

This is the path that produces `tx`, `ty`, and `tz`.

#### 6.1. Downsample previous and current luma

Both frames are downsampled into persistent tracking buffers.

Current implementation:
- target width up to 320 pixels
- height scaled to preserve aspect ratio
- minimum size clamped to avoid degenerate cases

The goal is to reduce CPU cost while keeping enough structure for patch
tracking.

#### 6.2. Build a fixed tracking grid

The downsampled frame is sampled on a fixed grid:

- 8 columns
- 5 rows

This yields up to 40 candidate tracking points.

The grid avoids the image borders by keeping a margin large enough for:
- patch radius
- local search window

#### 6.3. Reject low-texture patches

For each candidate point, `patch_texture_score(...)` computes a simple texture
score from local horizontal and vertical intensity changes.

If a patch is too flat, it is skipped.

Reason:
- flat regions do not provide stable matching
- this avoids noisy or ambiguous motion vectors

#### 6.4. Search locally for the best patch match

Each accepted previous-frame patch is matched against the current frame using a
small brute-force search window.

Current behavior:
- patch radius: 6 pixels
- search range: +/-8 pixels

For every candidate offset:
- `patch_sad_score(...)` computes patch SAD
- the offset with the smallest score is kept

This produces one local displacement vector per tracked patch:
- `dx`
- `dy`

#### 6.5. Robustly reject outliers

The set of patch displacements is filtered in two stages.

First:
- median `dx`
- median `dy`

Then:
- each track is compared against the median motion
- tracks with large residual error are discarded

This protects the estimate from:
- unstable matches
- independently moving objects
- local texture failures

#### 6.6. Estimate image-plane translation

The remaining inlier vectors are averaged.

The resulting average displacement on the downsampled frame is then scaled back
to full-frame coordinates.

This becomes:
- `tx`
- `ty`

Interpretation:
- positive or negative sign depends on image motion direction
- units are full-frame pixels per processed interval
- values are relative, not metric world-space units

#### 6.7. Estimate relative scale term (`tz`)

After translation is estimated, the code looks at the residual motion field.

For each inlier track:
- subtract the average translation
- compute the point's radius from the image center
- project residual motion onto that radius

This approximates radial expansion or contraction:
- outward residual flow -> positive scale -> camera approaching scene
- inward residual flow -> negative scale -> camera receding from scene

The result is averaged into `tz`.

Important limitation:
- `tz` is not metric depth
- it is only a relative image-scale indicator
- it is most meaningful when the scene is roughly planar and dominant motion is
  camera motion rather than object motion

### 7. Update previous-frame history

After ROI and planar motion are computed, the current luma plane is copied into
the persistent previous-frame buffer with `copy_luma_to_prev(...)`.

This prepares the state for the next processing cycle.

### 8. Return the frame buffer

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
- SAD grid dimensions
- block size
- planar tracking buffer size

This confirms the tracker is alive even when no strong motion is happening.

### 3. Motion outputs

#### ROI log

Example shape:
- `[optflow] motion roi x=... y=... w=... h=...`

Meaning:
- bounding box of thresholded SAD motion blocks
- full-frame pixel coordinates

#### Planar log

Example shape:
- `[optflow] planar tx=... ty=... tz=... tracks=used/found ...`

Meaning:
- `tx`, `ty`: estimated image-plane translation
- `tz`: relative scale term
- `found`: textured candidate patches matched
- `used`: inliers remaining after outlier rejection

A high `used/found` ratio usually means the frame pair is coherent and the
estimate is more trustworthy.

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
- processing cadence is only about 5 Hz
- `tz` is only a heuristic scale term, not real Z distance

Because of that, the current output should be interpreted as:
- a useful relative planar motion signal
- not a final navigation-grade pose estimator

## Summary

After a frame is retrieved from the VPE output port, the current logic does:

1. validate the buffer
2. wrap its luma plane
3. bootstrap previous frame if needed
4. run hardware SAD against the previous frame
5. extract a coarse motion ROI
6. run CPU patch matching on downsampled luma
7. estimate robust `tx` and `ty`
8. derive heuristic radial scale `tz`
9. copy current luma into previous-frame state
10. return the SYS buffer

This gives the project a low-cost relative motion signal that is already useful
for debugging and future stabilization or guidance work, while staying within
the current Star6E pipeline architecture.