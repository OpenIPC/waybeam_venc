# Optical Flow

## Purpose

The current Star6E optflow path is a lightweight motion estimator that runs
alongside the encoder stream loop.

It currently produces one motion output:

1. An LK optical-flow estimate:
  - `lk tx`: horizontal image translation in full-frame pixels
  - `lk ty`: vertical image translation in full-frame pixels
  - `lk rz`: image-plane rotation proxy derived from tracked feature motion

2. A SAD-based planar estimate:
  - `tx`: horizontal image translation in full-frame pixels
  - `ty`: vertical image translation in full-frame pixels
  - `tz`: relative image-plane scale proxy derived from the residual flow field

The implementation is selected by `optflow.mode` in `venc.json`.

- `"lk"` (default): vendor LK tracking on a downsampled image pair
- `"sad"`: hardware SAD ROI detection plus CPU planar translation/scale estimate

This is not a full 6-DoF visual odometry system and it is not a strict sparse
Lucas-Kanade implementation. Both modes are available at runtime through
`optflow.mode`. LK uses SigmaStar vendor optical flow on a reduced luma pair.
SAD uses `MI_IVE_Sad(...)` for same-position motion energy plus a CPU matcher
that reduces selected patch displacements into planar `tx`, `ty`, and `tz`.

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

Current mode:
- configured by `optflow.mode` in `venc.json`
- default value: `"lk"`

The debug OSD overlay is controlled separately by `optflow.showOSD` in
`venc.json`.

- default value: `true`
- when `false`, optflow tracking still runs but the Star6E RGN OSD module is
  not configured and no marker/debug points are drawn
- for `optflow.mode = "lk"`, the overlay is a red marker plus green tracked
  feature points
- for `optflow.mode = "sad"`, the overlay is a red marker moved by SAD
  `tx`/`ty`; the LK green debug points are not used

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
- the SAD path also seeds its downsampled previous tracking buffer from the
  same frame
- `have_prev_frame` is set
- the buffer is returned immediately
- no LK or SAD motion is emitted for this first frame

This is required because both LK and SAD compare the current frame against the
previous frame.

### 3. Prepare the tracking images

Both modes keep a reduced luma representation for CPU-side tracking work.

Current LK implementation:
- center-crop the source frame to exactly half of the full frame width
- keep the full frame height
- then scale that cropped image to a target width up to 160 pixels
- height scaled to preserve the cropped aspect ratio
- minimum size clamped to avoid degenerate cases

Current SAD implementation:
- scales the full processing luma plane into a persistent tracking image up to
  `OPTFLOW_TRACK_WIDTH` pixels wide
- preserves aspect ratio
- seeds the previous tracking buffer once, then only downsamples the current
  frame on subsequent solves and promotes it after processing

The goal is to reduce CPU cost while keeping enough structure for stable
matching and reduction.

Example:
- `1920x1080` source frame
- centered crop to `960x1080`
- scaled tracking image to `160x180`

### 4. Run the selected motion solver

If `optflow.mode = "lk"` and the runtime exports `MI_IVE_LkOpticalFlow(...)`,
the tracker runs the LK path on the reduced luma pair.

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

The selected LK input points are also mapped back from tracking-space into
full-frame coordinates and rendered on the OSD as green `4x4` rectangles.
This compensation accounts for both the center crop and the downscaled
tracking image.

Point selection can optionally use a corner-seeded layout. In that mode the
selector first chooses one best-scoring corner per usable-area quadrant
(top-left, top-right, bottom-left, bottom-right), then fills the remaining
slots with the normal global strongest-corners path subject to the same
minimum-spacing rule.

The current LK call path is configured to submit 10 points per solve while
the backing point and motion-vector buffers still reserve space for up to 16.

`lk rz` is an image-plane rotation proxy from the residual flow field around
the image center. It is not a full 3D yaw/pitch/roll estimate.

If `optflow.mode = "sad"`, the tracker runs two stages:

1. Hardware SAD stage:
  - `MI_IVE_Sad(...)` compares the previous full-resolution luma plane against
    the current one at the same coordinates
  - the threshold output is reduced into a motion ROI for debugging/logging

2. CPU planar stage:
  - a set of candidate tracking patches is evaluated on the reduced tracking
    images
  - candidates are ranked by texture strength
  - the selected points prefer strong texture, quadrant coverage, and minimum
    spacing between points
  - each selected patch is matched with a coarse-to-fine local search
  - the inner patch SAD loop uses an early-exit cutoff and a Star6E NEON fast
    path when available
  - the winning integer offset is refined with sub-pixel interpolation
  - the resulting displacements are reduced into planar `tx`, `ty`, and `tz`
    using confidence weighting and adaptive outlier rejection

`tx` and `ty` are full-frame image-plane translation estimates. `tz` is a
relative scale proxy derived from how the residual flow expands or contracts
with radius from the image center.

### 5. Update previous-frame history

The worker advances the persistent previous full-resolution luma frame after
each processed solve. The SAD path also promotes the current downsampled track
image so the next solve can reuse it directly.

This keeps the next comparison aligned with the most recent processed frame and
avoids repeatedly re-downsampling the previous tracking image on the SAD path.

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

#### Perf log

Example shape:
- `[optflow] perf: window_ms=... runs=... rate_hz=... scale_ms=... lk_ms=... total_ms=... avg_corners=...`

Meaning:
- `window_ms`: aggregation window length in milliseconds
- `runs`: how many processed optflow passes ran in that window
- `rate_hz`: processed optflow execution rate over the window
- `scale_ms`: total time spent downsampling previous and current frames
- `lk_ms`: total time spent inside `MI_IVE_LkOpticalFlow(...)`
- `total_ms`: total time spent in the processed per-frame path
- `avg_corners`: average `state->lk_ctrl.u16CornerNum` value used in the window

These are aggregated millisecond-resolution timings for the current logging
window, not per-frame microbenchmarks.

SAD emits a similar aggregated perf log. Its fields summarize the SAD path
over a rolling window of about 3 seconds or 100 SAD executions, whichever
comes first.

Example shape:
- `[optflow][SAD] perf: window_ms=... runs=... rate_hz=... getbuf_ms=... sad_ms=... box_ms=... planar_ms=... copy_prev_ms=... osd_ms=... total_ms=... avg_tracks=used/found`

Meaning:
- `getbuf_ms`: total time spent borrowing SYS/VPE buffers
- `sad_ms`: total time spent inside `MI_IVE_Sad(...)`
- `box_ms`: total time spent deriving the motion ROI from the threshold map
- `planar_ms`: total time spent in the CPU planar tracking and reduction path
- `copy_prev_ms`: total time spent updating the persistent previous-frame buffer
- `osd_ms`: total time spent updating the red RGN marker
- `total_ms`: total time spent in the processed SAD path
- `avg_tracks=used/found`: average inlier/initial tracked sample counts over the window

### 3. Motion outputs

#### LK log

Example shape:
- `[optflow] lk tx=... ty=... rz=... tracks=used/found ...`

Meaning:
- `lk tx`, `lk ty`: LK-derived image-plane translation
- `lk rz`: LK-derived image-plane rotation proxy
- `found`: candidate textured points passed to LK
- `used`: LK tracks with successful status used in reduction

#### SAD log

Example shape:
- `[optflow][SAD] planar tx=... ty=... tz=... tracks=used/found ...`

Meaning:
- `tx`, `ty`: SAD-derived image-plane translation
- `tz`: SAD-derived relative scale proxy
- `found`: selected candidate patches passed into the planar reduction
- `used`: inlier matches that survived adaptive outlier rejection and were used
  in the weighted reduction

## SAD Tuning Knobs

The main SAD tuning constants currently live near the top of
`src/opt_flow_sad.c`.

The most important ones are:

- `OPTFLOW_TRACK_WIDTH`
  - target width of the reduced SAD tracking image
  - higher values keep more detail and can improve accuracy
  - lower values reduce CPU cost

- `OPTFLOW_TRACK_CANDIDATE_COLS`
- `OPTFLOW_TRACK_CANDIDATE_ROWS`
  - size of the dense candidate grid used before feature selection
  - higher values search more possible feature locations and can improve point
    quality and coverage
  - higher values also increase `planar_ms`

- `OPTFLOW_TRACK_SELECT_COUNT`
  - maximum number of selected candidate patches passed into the matcher
  - higher values can improve robustness when the scene is rich in texture
  - lower values reduce cost and can reduce contamination from weak features

- `OPTFLOW_TRACK_PATCH_RADIUS`
  - half-size of each local SAD patch
  - larger patches are often more stable on noisy or weak-texture scenes
  - smaller patches can react better to fine local structure and reduce cost

- `OPTFLOW_TRACK_SEARCH_RANGE`
  - maximum per-axis local displacement searched for each patch
  - larger values help when frame-to-frame motion is bigger
  - smaller values reduce cost and reduce false matches in quiet scenes

- `OPTFLOW_TRACK_MIN_TEXTURE`
  - minimum patch texture score required before a candidate can be selected
  - raising it filters weak or flat patches more aggressively
  - lowering it increases point count in low-detail scenes but can reduce match quality

- `OPTFLOW_TRACK_MIN_SPACING`
  - minimum spacing between selected candidate patches
  - larger values improve spatial diversity and reduce clustering
  - smaller values allow denser sampling in locally textured regions

- `OPTFLOW_TRACK_MAX_RESIDUAL_SQ`
  - retained as a hard residual clamp on top of the adaptive inlier logic
  - acts as a safety bound against obviously bad matches

Practical tuning order:

1. Start with `OPTFLOW_TRACK_MIN_TEXTURE` and `OPTFLOW_TRACK_MIN_SPACING`
2. Then tune `OPTFLOW_TRACK_CANDIDATE_COLS`, `OPTFLOW_TRACK_CANDIDATE_ROWS`, and `OPTFLOW_TRACK_SELECT_COUNT`
3. Only after that adjust `OPTFLOW_TRACK_PATCH_RADIUS`, `OPTFLOW_TRACK_SEARCH_RANGE`, or `OPTFLOW_TRACK_WIDTH`

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
- `tz` is only a relative image-plane scale proxy, not metric depth
- the SAD matcher still uses local patch matching, not a full homography or
  dense optical-flow solve
- the SAD sub-pixel refinement is axis-wise, not a full 2D quadratic surface fit

Because of that, the current output should be interpreted as:
- a useful relative image-motion signal
- not a final navigation-grade pose estimator

## Summary

After a frame is retrieved from the VPE output port, the current logic does:

1. validate the buffer
2. bootstrap previous-frame state if needed
3. prepare the reduced tracking image for the selected mode
4. run either LK vendor optical flow or the SAD ROI plus CPU planar matcher
5. derive either `lk tx`/`lk ty`/`lk rz` or `tx`/`ty`/`tz`
6. update previous-frame state and optional SAD tracking buffers
7. update the optional OSD marker
8. return the SYS buffer

This gives the project a low-cost relative motion signal that is already useful
for debugging and future stabilization or guidance work, while staying within
the current Star6E pipeline architecture.