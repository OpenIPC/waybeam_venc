# History

## [0.10.1] - 2026-05-05

TS recorder: universally-decodable audio in `.ts` files.

The recorder previously muxed audio as private-data with an "LPCM"
registration descriptor that no standard player recognised — VLC,
ffmpeg, and mpv treated it as `bin_data` and either dropped it or
played white noise.  Recordings now carry audio in one of two forms,
selected by `audio.codec`:

- `audio.codec = "pcm"` → SMPTE 302M (BSSD).  Broadcast standard for
  16-bit PCM in MPEG-TS.  Mono inputs are upmixed to stereo per the
  302M requirement.  Universally decoded.
- `audio.codec = "opus"` → Opus-in-MPEG-TS provisional mapping.
  Re-uses the same Opus encoder feeding the RTP path, so no extra CPU
  cost.  Each PES carries one Opus access unit prefixed by the
  `0x7FE0` control header.  Recommended — about 30× smaller audio than
  PCM at the same intelligibility.
- `audio.codec = "g711a"` / `"g711u"` → audio is not muxed into the
  recording (no in-band TS framing that VLC/ffmpeg decode without
  hints).  Video-only file.

Filenames now carry a `_opus` or `_pcm` suffix so the codec is visible
without ffprobe (e.g. `rec_02h23m07s_c9e2_opus.ts`).

ts_mux additions:
- `ts_mux_init` gains an `audio_codec` argument
  (`TS_AUDIO_CODEC_PCM_S302M` / `TS_AUDIO_CODEC_OPUS`)
- PMT writer emits the matching registration descriptor (BSSD or Opus)
  plus the Opus extension descriptor with `channel_config_code`
- `ts_mux_write_audio` dispatches: SMPTE 302M packs raw s16le with
  bit-reversal per the AES3 16-bit layout; Opus path wraps each
  pre-encoded packet in the 11-bit prefix + au_size control header
- `star6e_ts_recorder_init` gains an `audio_codec` argument plumbed
  through from `audio.codec`

The Star6E and Maruko audio threads now route the encoded buffer
(rather than raw PCM) into the recording ring when codec is Opus.

Verification:
- Host: 1520 unit tests pass; offline sine encode round-trips
  bit-exact through ffmpeg's SMPTE 302M and Opus parsers.
- Hardware: Star6E bench (`192.168.1.13`, IMX335).  HEVC + audio
  recordings in both modes play directly in VLC, mpv, and ffmpeg.

## [0.10.0] - 2026-05-03

`video0` digital zoom (Approach C) — Star6E + Maruko parity.

Adds three new `video0` fields driving a 1:1 SCL crop (output dim = crop
dim, no upscale, no bandwidth pressure).  Receivers see the smaller dim
in SPS/PPS; receivers that pin to first SPS render deeper zoom invisibly,
which is why `zoom_pct` is clamped at a 0.25 floor in the parser.

Schema:
- `video0.zoomPct` — `0.0` = zoom OFF (full image); `0.25..1.0` = crop
  fraction (smaller = deeper zoom).  MUT_RESTART (encoder dim change).
- `video0.zoomX`, `video0.zoomY` — crop centre, `0..1` (0 = top/left,
  1 = bottom/right).  MUT_LIVE (no respawn — joystick / head-tracker
  friendly).

Implementation:
- **Star6E**: `MI_VPE_SetPortCrop(0, 0, ...)` on the existing VPE port —
  no new SCL channel, no extra mem.  SCL clock bumped 384 → 432 MHz to
  unblock crop+resize at full sensor input.  Debug OSD canvas stays 1:1
  with the encoded frame (RGN attaches at the post-SCL VPE port output,
  not at VPE input — no per-zoom offset needed).
- **Maruko**: `MI_SCL_SetPortConfig(0,0,0)` carrying both crop and
  output dim atomically.  Output dimensions stay 16-px aligned and crop
  offsets stay 2-px aligned; the smaller dimension drives the crop to
  keep the encoded AR matching the sensor.

Verification:
- Live sweep on both devices: pct ∈ {1.0, 0.7, 0.5, 0.3, 0.25} × pan ∈
  {(0.5,0.5), (0,0), (1,0), (0,1), (1,1), (0.5,0.5)}.  Star6E sustains
  60 fps, Maruko sustains 30 fps across all combinations.  Pan
  confirmed visually on the live RTP stream for both backends.

## [0.9.16] - 2026-05-03

IntraRefresh: single-knob `intraRefreshMode` enum.

Replaces the boolean `intraRefresh` + two zero-sentinel fields from PR #92
with one human-readable mode picker (`off` | `fast` | `balanced` | `robust`).
Each mode targets a self-heal window (150 ms / 500 ms / 1000 ms) and derives
lines, GOP, and QP from it; per-field overrides remain available.

**Breaking change** (no backward compat): `video0.intraRefresh` boolean is
removed. Configs from 0.9.15 and earlier carrying `intraRefresh: true` will
fall through to `intraRefreshMode: "off"` (default) — re-enable explicitly
via the mode field or `POST /api/v1/intra/mode?mode=balanced`.

Highlights:
- New `src/intra_refresh.{c,h}` shared helper (used by both backends)
  computes lines, auto-GOP (one IDR per full GDR pass), and codec-default
  QP (48 H.265 / 45 H.264 — `u32ReqIQp` is never passed as 0).
- New `POST /api/v1/intra/mode?mode=<name>` endpoint: sets mode, clears
  per-field overrides, persists, reinits. One call to switch presets.
- Extended `/api/v1/intra/status` response: returns mode, target_ms,
  total_rows, and per-field requested-vs-effective for lines/qp/gop.
- Both Star6E and Maruko backends share the helper — drift between
  parallel implementations is no longer possible.
- Schema field `video0.intra_refresh_mode` registered (FT_STRING,
  MUT_RESTART) with `intraRefreshMode` camelCase alias.
- Auto-GOP overrides `gopSize` only when user did not pin a value;
  explicit `gopSize > 0` is honored and logged at boot.
- Contract bump 0.8.4 → 0.9.0 (breaking config field rename).
- 44 new unit tests covering parse, compute, override, clamp, edge cases.

## [0.9.15] - 2026-05-02

Maruko parity Phase 5 — audio capture (Opus / G.711 / raw PCM).

Closes the last big standalone parity gap: Maruko now captures PCM via the
i6c MI_AI ABI, encodes via the shared codec helpers, and ships the result
as RTP (or compact UDP) on `outgoing.audioPort`.  The Phase 6 TS recorder
automatically picks up audio when capture is active — PMT advertises an
audio PID and PCM frames are interleaved by the existing `ts_mux_write_audio`
path that Star6E was already wired to.

What ships:
- `vendor-libs/maruko/libmi_ai.so` (110 KB) and `libmi_ao.so` (85 KB),
  pulled from the SDK uClibc bundle.  AO is reserved for a future Phase 5b
  playback path.  MD5SUMS + README updated.
- New `include/maruko_ai_types.h` carrying the small set of MI_AI types
  used at runtime (Attr / Data / Format / SoundMode / SampleRate / If),
  copied verbatim from the SDK headers so the build stays SDK-header-free.
- New `maruko_ai_impl` shim in `include/maruko_mi.h` + `src/maruko_mi.c`.
  Symbol set: `MI_AI_InitDev` / `DeInitDev` / `Open` / `Close` /
  `AttachIf` / `EnableChnGroup` / `DisableChnGroup` / `Read` /
  `ReleaseData` / `SetMute` / `SetGain` / `SetIfGain`.  Loaded with
  `RTLD_LAZY|RTLD_GLOBAL` and tolerates absence — no `libmi_ai.so` →
  audio disabled but the rest of the pipeline runs unchanged.
- New `src/maruko_audio.c` (~510 LOC) — full capture state machine:
  `Open(dev=0)` + `AttachIf(ADC_AB)` + `SetGain` + `SetMute` +
  `SetChnOutputPortDepth` + `EnableChnGroup`; capture thread on
  `SCHED_FIFO` doing `MI_AI_Read(0, 0, &mic, &echo, 50)` → push
  PCM into the shared `audio_ring`; encode thread does Opus / G.711 /
  L16 byte-swap and ships via RTP packetizer or compact UDP.
- New shared helper `src/audio_codec.{c,h}` — extracted Opus / G.711
  encoders + stdout filter (singleton, refcounted) from
  `src/star6e_audio.c`, ~250 LOC moved.  Star6E side switched to the
  shared helpers; `Star6eAudioState.opus_lib` / `opus_enc` replaced
  by the new `AudioCodecOpus opus`.  No behavioural change on Star6E.
- `MarukoBackendConfig` mirrors `vcfg->audio` + `vcfg->outgoing.audio_port`
  + `outgoing.max_payload_size` so the pipeline can call
  `maruko_audio_init` without taking a `VencConfig` dependency.
- `MarukoBackendContext` gains `audio` (state) + `audio_recorder_ring`
  (bridge from audio encode thread to TS recorder).  Init in
  `maruko_pipeline_configure_graph` after `bind_maruko_pipeline`,
  teardown in `maruko_pipeline_teardown_graph` after `stop_dual` and
  `ts_recorder_stop` so no consumer can race the ring teardown.
- `apply_mute = maruko_audio_apply_mute` in `maruko_controls.c`,
  closing the trivial-after-audio-lands gap from the parity matrix.
- Replaced the `maruko_runtime.c:58-60` "audio output is not supported"
  warning with the live init call.

Verify gate:
- `make verify` passes both backends.
- Bench (192.168.2.12, IMX415): `audio.enabled=true`,
  `audio.codec=opus|g711a|pcm` → `[audio] Initialized` + RTP arrives on
  the configured destination port.  Mute toggle via
  `/api/v1/set?audio.mute=true` cuts audio cleanly.

Caveats:
- The SSC378QE bench's analog mic wiring is unverified.  `MI_AI_Open`
  succeeds and `MI_AI_Read` returns frames, but the actual codec on this
  board may not be wired to a microphone — capture may yield silence.
  The init path still completes successfully so the userspace pipeline
  itself is exercised; an analog mic is a separate hardware fix-up.
- AO (playback) is intentionally not exposed yet — reserved for a
  future Phase 5b once a use case appears.

## [0.9.14] - 2026-05-02

Maruko parity Phase 6 — TS recording (`record.mode="mirror"` / `"dual"`).

Lights up on-device TS-mux recording on Maruko by reusing the Star6E
TS recorder state machine.  Two modes wired:

- **mirror**: chn 0 frames are written to the .ts file alongside the
  RTP stream.  Single encoder, simplest case.
- **dual**: chn 1 (created via Phase 7's `start_dual()`) drains into
  the .ts file while chn 0 keeps streaming RTP to the configured
  destination.  The chn 1 drain thread feeds the recorder; the chn 0
  loop is guarded so it never co-writes.

Audio is video-only for now (no audio backend on Maruko — Phase 5).
Raw `.hevc` format is rejected with a warning; only `format="ts"` is
implemented.

Implementation notes:
- Promoted `src/star6e_recorder.c`, `src/star6e_ts_recorder.c`,
  `src/ts_mux.c` from `STAR6E_ONLY_SRC` to a new `RECORDER_SRC` list
  built by both backends.  No `#ifdef PLATFORM_*` was needed — the
  files only depend on type names from `star6e.h`, which Maruko
  already pulls in for `MI_SYS_ChnPort_t`.
- Added a small adapter `src/maruko_ts_recorder.c` that pulls NAL
  units out of `i6c_venc_strm` (Maruko) and feeds the shared
  `star6e_ts_recorder_write_video()` primitive.  Mirrors
  `star6e_ts_recorder_write_stream()` 1:1.
- `MarukoBackendConfigRecord` extended with `dir`, `format`,
  `max_seconds`, `max_mb` (already present on the generic
  `VencConfigRecord`).
- New `Star6eTsRecorderState ts_recorder` field on
  `MarukoBackendContext`; opened in `configure_graph` after
  `start_dual` so the drain thread sees it ready.  Closed in
  `teardown_graph` after `stop_dual` (no race window).

Verified on 192.168.2.12 (OpenIPC SSC378QE / IMX415, no SD card —
written to `/tmp` tmpfs).  Live test pending in the next session.

Out of scope for this PR (deferred):
- Raw `.hevc` recorder on Maruko (the `_write_frame` adapter still
  takes Star6E's `MI_VENC_Stream_t`; will land alongside Phase 5
  audio if anyone wants it).
- HTTP `/api/v1/record/start|stop` for Maruko (daemon-config-driven
  only for now).
- Audio mux into the TS container (Phase 5).

## [0.9.13] - 2026-05-02

Maruko parity Phase 3 — BMI270 IMU port (opt-in via `imu.enabled`).

Wires the existing platform-agnostic `src/imu_bmi270.c` into the
Maruko pipeline so `imu.enabled=true` reads gyro + accel from a
BMI270 over I2C (frame-synced FIFO mode, 200 Hz default ODR).  The
push callback is currently a stub — samples are read and discarded —
so this lands the lifecycle but no consumer yet.  Future telemetry /
sidecar export plugs into the existing callback slot without touching
init/teardown.

Verified on 192.168.2.12 (OpenIPC SSC378QE / IMX415 1472x816@120,
H.265 25 Mbps RTP):
- BMI270 detected at `0x68` on `/dev/i2c-1` (chip_id=`0x24`).
- 400-sample auto-bias (~2 s) completes cleanly; gyro bias ≈
  (0.005, -0.006, -0.001) rad/s on bench.
- 200 Hz FIFO drain runs every video frame; 1963 samples / 9 s of
  streaming at 118 fps, 0 read errors.

- **Maruko-specific ordering constraint.**  IMU init must run BEFORE
  `MI_VENC_StartRecvPic` (i.e. before `bind_maruko_pipeline()`)
  because the auto-bias loop blocks the main thread for ~2 s
  (400 samples @ 200 Hz).  Empirically on Maruko, blocking the main
  thread for 2 s after StartRecvPic leaves the VENC fd in a state
  where `poll()` never returns POLLIN and the stream loop never
  progresses.  Star6E does not exhibit this — IMU init can stay
  post-VENC there.  The constraint is captured inline in
  `maruko_pipeline_configure_graph()` so future re-orders don't
  regress.
- `src/imu_bmi270.c` moved from `STAR6E_ONLY_SRC` to `HELPER_SRC`
  (already platform-agnostic; no `#ifdef PLATFORM_*` in the file).
- New `imu` field on `MarukoBackendContext` (`ImuState *`, NULL when
  disabled).  `MarukoBackendConfig` carries `VencConfigImu imu`
  embedded from `vcfg->imu` so the pipeline does not reach into
  `VencConfig` directly (consistent with the `show_osd` /
  `keep_aspect` bridge fields).
- Per-frame `imu_drain()` runs in `maruko_pipeline_process_stream()`
  before `MI_VENC_GetStream` (Star6E parity at
  `star6e_runtime.c:727`).  No-op when `ctx->imu == NULL`.
- Stop/destroy in `maruko_pipeline_teardown_graph()` ahead of any
  unbind/stop, mirroring Star6E.
- No config-schema change.  `imu.enabled` default remains `false`,
  so existing setups are untouched.

## [0.9.12] - 2026-05-02

Maruko parity Phase 9 — opt-in 3A CPU throttle (`isp.aeMode`).

The 1080p120 H.265 25 Mbps profile burned ~62% of a single Cortex-A7
core on Maruko (SSC378QE).  Per-thread sampling pinned ~17.5% to
libcus3a.so's `3A_Proc_0` worker (spawned by `MI_ISP_EnableUserspace3A`,
runs at sensor frame sync) plus matching kernel ISP/VENC kthread time.
The host pipeline was already lean (~3% main thread) so the ceiling
was the per-frame 3A loop — not anything we packetize or send.

The cut: keep `MI_ISP_EnableUserspace3A` so the IQ→HW pump (the same
`3A_Proc_0` thread) keeps writing saturation/sharpness/brightness to
silicon, but swap the SDK's NATIVE AE algorithm for a no-op stub via
`MI_ISP_CUS3A_RegInterfaceEX(ADAPTOR_1)`.  AWB stays NATIVE so white
balance still tracks the scene (the 4096/1024/1024 R/G/B gains in the
SDK demo turned out to be smoke-test values, not calibrated daylight
gains — letting the bin drive AWB gives a usable picture).  AE is
then driven by a 15 Hz supervisory thread (`src/maruko_cus3a.c`) that
reads the 128x90 luminance grid via `MI_ISP_AE_GetAeHwAvgStats` and
applies a three-stage cascade (shutter → sensor gain → ISP digital
gain) via `MI_ISP_CUS3A_SetAeParam`.

- New config field **`isp.aeMode`** (`"native"` default,
  `"throttle"` opt-in).  Default preserves existing behaviour and
  gives a safety hatch if a different sensor / firmware breaks the
  no-op adaptor.  Live mode change requires restart (the adaptor swap
  and `CUS3A_Enable` flags are init-only); SIGHUP at runtime is a
  documented limitation.
- New module **`src/maruko_cus3a.c` + `include/maruko_cus3a.h`**
  (≈700 lines).  `_install_noop_adaptor()` registers the AE stub;
  `_start()` launches the 15 Hz controller thread; `_stop()` joins
  on teardown.  `MarukoCus3aConfig.throttle_mode` gates the
  AE-control law — when 0 the thread still runs cap-enforcement +
  stats reads (Star6E-equivalent behaviour).
- **Pipeline integration** (`src/maruko_pipeline.c`): adaptor install
  is gated on `cfg.ae_mode == "throttle"` after `MI_SNR_SetFps`;
  thread starts when `ae_fps > 0` regardless of mode.  Default-on bin
  gain ceiling switched to `bin_max_sensor_gain` (8192 on IMX415)
  when user sets `gainMax=0`, fixing the previous over-bright bias
  (the old default capped at a 32× synthetic ceiling).
- **Config / fixture**: `config/venc.default.json` adds
  `"aeMode": "native"` so the round-trip layout test passes.

Verified on 192.168.2.12 (SSC378QE / IMX415 1472x816@120fps,
H.265 25 Mbps RTP):
- `aeMode=native` (default): unchanged behaviour, ~50% sys CPU.
- `aeMode=throttle`: ~36% sys CPU (≈24 percentage-point drop on a
  single-core SoC), `3A_Proc_0` ticks 89→36 per 3 s sample, AE
  responds visibly to scene changes at 15 Hz, IQ knobs
  (saturation / sharpness / brightness) still hot-apply.

## [0.9.11] - 2026-05-02

Maruko parity Phase 2b — debug OSD now functional (kernel-oops cured).

Verified on 192.168.2.12 (OpenIPC SSC378QE, kernel 5.10.61): with
`debug.showOsd=true`, RGN init/create/attach/getcanvas all succeed,
encode loop runs at ~117 fps, no kernel taint, OSD canvas mapped at
1472x816 stride 736.

- **Root cause was a build-time conditional bug, not a kernel/lib
  mismatch.**  The Maruko build defines BOTH `-DPLATFORM_STAR6E` and
  `-DPLATFORM_MARUKO` (the Star6E backend's MI shim headers are reused
  for type compatibility; see `Makefile:39`).  In Phase 2,
  `src/debug_osd.c` started with `#ifdef PLATFORM_STAR6E`, so the
  Star6E branch was compiled into the Maruko binary too — and the
  Star6E ABI (1-arg `MI_RGN_Init(palette*)`, mod_id 0 = VPE, 3-arg
  `AttachToChn`) ran against the Maruko kernel/lib pair.  That
  ABI mismatch produced the `MI_DEVICE_Ioctl → kfree → compound_head`
  oops with a userspace-shaped pointer (`r0=0x0f9c0900`) reaching
  kfree.  Fixed by changing the first conditional to
  `#if defined(PLATFORM_STAR6E) && !defined(PLATFORM_MARUKO)` so
  Maruko binaries enter the proper Maruko branch.
- **`debug_osd`: Maruko ABI branch (now active).**  Targets the
  OpenIPC libmi_rgn.so v3 API as documented in
  `Maruko_work_dir/SourceCode/project/release/include/mi_rgn.h` and
  used by the official IPC demo at
  `Maruko_work_dir/ipc_demo/maruko/common/osd/osd.cpp`:
  `MI_RGN_Init(soc_id, palette*)` (palette as direct arg, not wrapped),
  3-arg `MI_RGN_Create(soc_id, handle, attr*)`, 4-arg
  `MI_RGN_AttachToChn(soc_id, handle, chnport*, param*)`, 64-bit
  `MI_PHY` / pointer-width `MI_VIRT` in `MI_RGN_CanvasInfo_t`,
  module ID 34 (`E_MI_MODULE_ID_SCL` — RGN is attached to SCL/0/0/0).
- **`maruko_mi`: pre-load `libmi_rgn.so`.**  Added to the existing
  RTLD_GLOBAL dep chain in `maruko_mi_init()` (alongside
  `libcam_os_wrapper`, `libmi_common`, `libispalgo`, `libcus3a`) so the
  later `dlopen` from `debug_osd.c` finds the dependency graph fully
  resolved.  (`src/maruko_mi.c`)
- **`maruko_pipeline`: init-before-kthread ordering.**  Moved
  `debug_osd_create()` ahead of `bind_maruko_pipeline()` so it runs
  after the SCL channel exists (`maruko_start_vpe`) but BEFORE
  `MI_VENC_StartRecvPic` spawns the encoder kthread.  The v5.10
  OpenIPC kernel mi_rgn driver requires the singlethread workqueue
  to be created from the main task.  Dropped the Phase 2 safety-gate
  WARN-and-skip — the runtime is now real.  (`src/maruko_pipeline.c`)

Recipe cross-referenced with `waybeam-hub/src/rgn_backend_maruko.c`,
which had already verified the dep preload + module-ID-34 pattern
against the same kernel/lib pair (different `MI_RGN_OsdChnPortParam_t`
trailing field — the Maruko SDK header in
`Maruko_work_dir/SourceCode/project/release/include/mi_rgn_datatype.h`
omits `stColorInvertAttr` that the hub's older vendored header
includes; both work because the kernel reads only the union prefix).

## [0.9.10] - 2026-05-02

Maruko parity Phase 2 — debug OSD overlay wired to both backends.

- **`maruko_pipeline`: debug OSD plumbed.**  Mirrors Star6E
  (`star6e_runtime.c:825-849`):
  `debug_osd_create()` runs at the end of
  `maruko_pipeline_configure_graph()` after VENC bind+start (gated on
  the new `cfg.show_osd`, sourced from `debug.showOsd`),
  `debug_osd_begin_frame / sample_cpu / text / end_frame` runs
  per frame inside `maruko_pipeline_process_stream()` showing fps + cpu,
  and `debug_osd_destroy()` runs at the top of
  `maruko_pipeline_teardown_graph()` before any unbind/stop.  Both
  backends now share `src/debug_osd.c` + `src/debug_osd_draw.c`
  (moved from `STAR6E_ONLY_SRC` to `HELPER_SRC`).  When
  `debug.showOsd=false` (default) no OSD code runs on either backend.
- **Maruko runtime path safety-gated.**  On the test target
  (192.168.2.12, OpenIPC SSC378QE), invoking `MI_RGN_Init` triggers a
  kernel Oops in `MI_DEVICE_Ioctl` (kfree path) and wedges the encode
  loop — the same lib/kernel SDK vintage mismatch documented in
  `memory/maruko_osd_render_bringup.md`.  Until Phase 2b ships the cure
  (RTLD_GLOBAL dep preload, `MI_MODULE_ID_SCL`=34, init-before-worker-
  thread; tracked in `documentation/MARUKO_PARITY_PLAN.md`),
  `debug.showOsd=true` on Maruko emits a one-time warning and skips the
  attach so a stale config never hangs venc.  Star6E behaviour is
  unchanged.  (`src/maruko_pipeline.c`, `src/maruko_config.c`,
  `include/maruko_config.h`, `include/maruko_pipeline.h`, `Makefile`)

## [0.9.9] - 2026-05-02

Maruko parity Phase 1 — aspect-ratio precrop on the SCL stage.

- **`maruko_pipeline`: SCL precrop wired up.**  `configure_maruko_scl()`
  now writes a centered crop rect into `scl_port.crop` instead of zero
  when `isp.keepAspect=true` and the encode aspect ratio differs from the
  sensor's effective output.  The rect is computed via the existing
  `pipeline_common_compute_precrop()` helper (Star6E parity) against the
  post-binning effective input (`sensor.plane.capt` clamped by
  `mode.output`), so it always matches the surface that actually feeds
  the SCL stage.  Falls back to zero crop (full source area, downstream
  stretch) when `isp.keepAspect=false`.  `venc_api_set_active_precrop()`
  is called on success for `/api/v1/config` visibility, and
  `venc_api_clear_active_precrop()` runs in
  `maruko_pipeline_teardown_graph()` for symmetry with Star6E.  Verified
  on bench (192.168.2.12, IMX415): 960x720 (4:3) on 1920x1080 sensor
  mode → `Precrop: 1920x1080 -> 1440x1080 (offset 240,0)`, encoding
  89 fps @ 25 Mbps without stretching; 1280x720 (16:9) and
  `keepAspect=false` paths both produce the legacy zero-crop output.
  (`src/maruko_pipeline.c`, `src/maruko_config.c`,
  `include/maruko_config.h`, `include/venc_config.h`,
  `documentation/PRECROP_ASPECT_RATIO.md`)

## [0.9.8] - 2026-05-02

Frame-drop fix: relax the SDK FrameLost rate-control threshold from 120% to
150% of target bitrate.  Affects both Star6E and Maruko (shared
`pipeline_common_frame_lost_threshold`).

On Maruko at 5 Mbps / 60 fps, hand-wave motion routinely caused the
encoder's measured output to spike to ~6.0–6.4 Mbps — past the old 120%
floor (6.144 Mbps).  `MI_VENC_SetFrameLostStrategy(NORMAL)` then dropped
whole frames as a safety net, costing 5–10 fps under motion even though
CBR rate control would have absorbed the overshoot in the next few frames
via QP feedback.

Confirmed via per-frame timing on device: user-space loop work stayed
≤230 µs avg / ≤365 µs max (well under the 16.67 ms 60-fps budget) in both
modes; the missing frames were skipped at the SDK encoder layer, not by
FIFO eviction.  Disabling `frameLost` entirely on test eliminated the
drop; raising the threshold to 150% restores the safety net for genuine
sustained network overload (>50% over target) while letting motion bursts
through.

- **`pipeline_common.c::pipeline_common_frame_lost_threshold`**: change
  margin from `bits / 5U` (20%) to `bits / 2U` (50%).  Floor of 524288
  bits (~512 kbps headroom) preserved for low-bitrate streams.
- No config-schema change.  `frameLost` default remains `true`.

Verified on 192.168.2.12 (SSC378QE / IMX415 @1920x1080 / 60 fps,
H.265 5 Mbps RTP): under continuous hand-wave motion, app-observed FPS
stays at 59 (vs 54–55 before); on-device VENC frame-arrival jitter goes
from 33.3 ms (one-frame skip) to a flat 16.7 ms.

## [0.9.7] - 2026-05-02

May 2026 code-review follow-up bundle (PRs P1+P2+P3+P4+P5 squashed).

- **P2 — `httpd`: Content-Length parser anchored.**  The HTTP request
  parser previously located `Content-Length:` via an unanchored
  case-insensitive substring search across the entire header block,
  which would latch onto the literal substring inside an arbitrary
  header value (e.g. `X-Forwarded: content-length:99`).  Fix walks
  the header block line by line and only matches at the start of a
  line.  Eliminates a request-smuggling vector against the live
  config / set endpoints; also drops the now-unused
  `httpd_strcasestr` helper.  (`src/venc_httpd.c`)

- **P4 — Move orphaned `snr_*` harnesses to `tools/`.**
  `snr_sequence_probe.c` and `snr_toggle_test.c` were never wired
  into the build (orphaned since 0.6.2).  Relocated under `tools/`
  to match the rest of the standalone diagnostic harnesses; updated
  the path reference in `documentation/REFACTORING_PLAN.md`.  No
  Makefile changes required.

- **P3 — `main`: pidfile + flock single-instance gate.**  The legacy
  guard scanned `/proc/*/comm` for a process named `venc`, which is
  inherently racy: two near-simultaneous launches each see no peer
  and both proceed to grab the SHM ring.  New `acquire_pidfile_lock()`
  takes an exclusive non-blocking `flock(LOCK_EX | LOCK_NB)` on
  `/var/run/venc.pid` (falling back to `/tmp/venc.pid`) before the
  legacy `/proc` scan; `EWOULDBLOCK` exits cleanly with rc=1.  The
  fd is held with `O_CLOEXEC` and intentionally leaked so the kernel
  releases the lock at process exit even on SIGKILL.  Old `/proc`
  fallback retained as defence-in-depth for the case where pidfile
  lock acquisition itself fails (e.g. read-only fs).  Updated comment
  in `src/venc_ring.c` to reference the new mechanism.
  (`src/main.c`, `src/venc_ring.c`)

- **P1 — `venc_api`: shrink `g_cfg_mutex` hold time.**  The live-set
  apply path held `g_cfg_mutex` across `stage_params_into_cfg` and
  `preflight_live_group_callbacks` even though both operate purely
  on stack-local config copies and do not read or mutate the shared
  `g_cfg` pointer.  Moved both calls outside the mutex region; the
  mutex is now only held during `apply_live_group_sequence_locked`,
  which is the irreducible window where backend `apply_*` callbacks
  read additional vcfg fields beyond their parameters via the
  registered `&ctx->vcfg`.  No behavior change; reduces serialization
  in the rapid-fire `/api/v1/set` path used by the link_controller.
  Documented the irreducible hold-time contract in the mutex
  declaration's doc comment.  (`src/venc_api.c`)

  Note on scope: the original CR claim of "torn-string reads at
  120 fps" did not match any reachable code path — backend reads
  of `vcfg->...` strings happen only at init/teardown or inside
  the `apply_*` callbacks (under the mutex).  The deeper deferred
  work-queue rework deferred until the apply-callback contract is
  changed.

- **P5 — `maruko_pipeline_run()` split.**  Reduced from 301 lines
  to ~50 by extracting `maruko_pipeline_init_streaming`,
  `maruko_pipeline_cleanup_streaming`,
  `maruko_pipeline_check_idle_abort`,
  `maruko_pipeline_await_frame`,
  `maruko_pipeline_process_stream`, and
  `maruko_pipeline_log_verbose_frame` into `MarukoStreamRuntime`
  helpers.  Outer loop now mirrors the Star6E shape
  (`star6e_runner_run` + `star6e_runtime_process_stream`).
  Behavior preserved: idle-abort timer (`MARUKO_IDLE_ABORT_US`),
  idle-warn timer (`MARUKO_IDLE_WARN_US`), FPS-kick, pressure-gating,
  verbose-cadence (`MARUKO_PKTZR_VERBOSE_ACTIVE`), cached pack
  reuse, POLLERR fallback.  (`src/maruko_pipeline.c`)

- **Tests.**  `tests/test_venc_httpd.c` gains coverage of the new
  Content-Length walker (anchored match, case-insensitive,
  multi-header, header-value-confused-with-body, missing/oversized,
  negative).  `scripts/test_pidfile_lock.sh` exercises the flock
  gate by launching two short-lived test processes and asserting
  the second exits with `EWOULDBLOCK`.  Existing `test_multi_set_*`
  cases continue to cover the now-mutex-free stage/preflight path
  in `apply_live_set_query`.

## [0.9.2] - 2026-04-28

Transport-pressure observability (the prior "Level 2 — local FPS skip"
plan was rolled back; see the post-encode-skip note at the end of this
section).

- **Universal fill source.** Producer-local backpressure now works
  uniformly for every output transport with a queue model:
    `shm://`   `(write_idx - read_idx) / slot_count`
    `unix://`  `SIOCOUTQ / SO_SNDBUF`
    `udp://`   `SIOCOUTQ / SO_SNDBUF`
  For UDP-over-WiFi the kernel send queue rarely fills (NIC drains
  fast) so the gate is a no-op in practice and the radio-link layer
  (`waybeam_wfb_ng/link_controller`) does the actual adaptive control.
  For local UDP and unix:// (e.g. gstreamer consumer) a slow consumer
  fills the kernel queue and the gate trips correctly — empirically
  validated on Linux 6.x by sending raw datagrams to a non-reading
  receiver and watching SIOCOUTQ rise to SO_SNDBUF cap.
- **Level 1 — observability.** New `GET /api/v1/transport/status`
  endpoint returns the active output transport (`"shm"` / `"udp"` /
  `"unix"` discriminator), queue fill, lifetime delivery counters
  (SHM only — `packetsSent`, `transportDrops`, `oversizeDrops`),
  live watermark config, current hysteresis state (`inPressure`),
  and the producer-local `pressureDrops` counter (all transports).
  Also new `query_transport_status` callback in `VencApplyCallbacks`
  (Star6E + Maruko both implement).  For unix:// / udp:// the
  socket-side lifetime counters are not yet tracked — `transportDrops`
  and `packetsSent` are absent from the JSON for those transports;
  the sidecar trailer carries 0s.  Future work: count
  sendmsg(EAGAIN/ENOBUFS) and successful sends in `output_socket_send_parts`.
- **Sidecar trailer.** `RTP_SIDECAR_FLAG_TRANSPORT_INFO` (0x04) +
  `RtpSidecarTransportInfoWire` (16 bytes).  Appended after the
  optional ENC_INFO trailer when any non-zero transport is active.
  Forward-compat: probes that don't recognise the flag read just the
  base frame (and ENC_INFO if present) and ignore the trailing bytes
  — no protocol version bump.
- **Hysteresis-driven pressure flag (telemetry only).**  Hardcoded
  watermarks `VENC_PRESSURE_HIGH_WATER_PCT=75` /
  `VENC_PRESSURE_LOW_WATER_PCT=50` in `venc_ring.h` — no live config
  surface.  Enter pressure when fill_pct ≥ HIGH, exit when fill_pct
  < LOW.  The flag flows out through the sidecar trailer
  (`in_pressure`) and through `/api/v1/transport/status`.  The
  `pressureDrops` counter increments while the flag is asserted and
  serves as a "frames-spent-in-pressure" metric for adaptive
  consumers.  **The producer never skips a frame on the basis of
  this flag** — see the rollback note below.
- **Auto-gated observation.** `*_observe_pressure` is only called
  per-frame when a sidecar probe is subscribed
  (`rtp_sidecar_is_subscribed`); when nobody is listening, the SIOCOUTQ
  ioctl / ring-fill load is skipped entirely.  Observation caches its
  fill_pct + lifetime stats into the output struct so the sidecar emit
  in the same frame reads the cache instead of re-querying — one
  query per frame on the producer hot path instead of two.  No live
  config surface for backpressure: prior `outgoing.backpressure /
  highWaterPct / lowWaterPct` knobs were dropped (the value never
  affected anything outside the trailer once frame-skip was rolled
  back, and exposing tuning knobs for a passive telemetry signal was
  noise).
- **Architecture note.** The hysteresis state machine sits on a
  pre-computed `fill_pct` (`venc_observe_pressure` in `venc_ring.h`)
  so each backend's `*_observe_pressure` is a transport-dispatch
  helper: pick the fill source, hand it to the shared state machine,
  cache the result.  Adding a new transport is one branch in
  `observe_pressure` + one branch in `query_transport_status`.
- **Internal/wire renames.**  Identifiers were scoped from "shm_*" to
  "transport_*" / "backpressure_*" before any consumer shipped, on
  the basis that the model is equally meaningful for any transport
  with a queue.  No deprecated aliases retained — PR isn't merged
  yet so churn cost is zero.
- **Post-encode frame-skip rolled back.** The original PR also shipped
  a producer-side skip path: when the hysteresis flag was asserted,
  `star6e_runtime` and `maruko_pipeline` would bypass
  `*_video_send_frame` entirely, advance the RTP timestamp, and emit
  a sidecar message with `seq_count=0`.  Hardware testing showed the
  approach was fundamentally broken for inter-frame-coded video:
  H.264 / H.265 P-frames reference the previous frame in the GOP, so
  dropping one P-frame leaves every following P-frame in that GOP
  undecodable at the receiver.  With `gopSize=5` at 120 fps, a
  pressure storm that "looked clean" on producer-side counters
  (`packetsSent` and `transport_drops` matched `bp=ON` baseline)
  produced unreconstructable garbage at the decoder.  The skip path
  has been removed.  Adaptation under link saturation belongs
  upstream of encode — either lowering `video0.bitrate` (which
  link_controller already does from radio stats and the trailer
  signal) or lowering encoder fps (sensor-side / `MI_SYS_BindChnPort2`
  divider).  The trailer / status endpoint / hysteresis state machine
  remain valuable as a fast pressure signal for adaptive consumers;
  they just no longer pretend to act on it locally.
- **Tests added / kept:**
  - Star6E hysteresis state machine — telemetry assertions only
    (`test_star6e_output_backpressure_hysteresis`)
  - UNIX datagram pressure observation with a non-reading receiver
    (`test_star6e_output_unix_backpressure`)
  - Always-send invariant under pressure
    (`test_star6e_output_always_sends_under_pressure`)
  - Wire-layout for {enc, transport} combinations
    (`test_star6e_video_sidecar_transport_layouts`)
- **Stale-ring hardening.** `venc_ring_create()` now `shm_unlink()`s
  the name before `O_EXCL`-creating a fresh inode, instead of
  `O_CREAT|O_TRUNC` reusing the existing one. After a SIGKILL'd venc
  is restarted while the old wfb_tx still has the ring mmapped, the
  old consumer keeps reading the orphaned inode (no SIGBUS, no race
  through magic=0/init_complete=0) until its watchdog detaches and
  re-attaches by name to the new inode. Pairs with the consumer-side
  per-iter epoch guard in `waybeam_wfb_ng/poc/shm-input.patch`. New
  regression test: `test_producer_restart_orphans_old_inode`.

## [0.9.1] - 2026-04-28

Live `outgoing.max_payload_size` (`/api/v1/set?outgoing.maxPayloadSize=...`):

- **Promoted from `MUT_RESTART` to `MUT_LIVE`.** The new size takes effect
  on the next encoded frame; the in-flight frame finishes packetizing at
  the old size, so a switch can never tear a single frame's FU/AP
  fragmentation. Composes with other live fields in a single multi-set
  request, e.g. `?video0.bitrate=8000&outgoing.maxPayloadSize=4000`.
- **Range validated to `[576, 4000]`** in `validate_field_cfg()`. Same
  validation now also gates boot via `venc_api_validate_loaded_config()`,
  so a bad on-disk config refuses to start instead of crashing later.
  4000 is sized for jumbo-frame links such as Realtek's 3993-byte MTU
  (4000 + 12 RTP + 8 UDP + 20 IP = 4040, fits comfortably).
- **Per-slot scratch bumped from 1616 → 4096 bytes** in both backends
  (`STAR6E_OUTPUT_BATCH_SLOT_SCRATCH`, `MARUKO_OUTPUT_BATCH_SLOT_SCRATCH`).
  Required so the sendmmsg() batch can hold an AP packet up to the new
  4000-byte limit. ~159 KiB extra per backend (64 slots × 2.4 KiB delta).
- **SHM parity with UDP/Unix.** SHM rings are sized at startup to fit
  the validated ceiling (`VENC_OUTPUT_PAYLOAD_CEILING_BYTES + 12` =
  4012 bytes per slot, 8-byte aligned), so `shm://` accepts the full
  live range without a restart-to-grow caveat. Costs ~1.3 MiB extra SHM
  per ring vs. the previous "size to configured starting value" scheme,
  but this is paid only when SHM output is actually configured. UDP and
  Unix datagram transports have no transport-level cap — only the
  validated range and scratch ceiling apply.
- **wfb_tx (or any SHM consumer) compatibility note.** The published
  `slot_data_size` in the ring header changes from
  `startup_max_payload + 12` (typically 1412) to a fixed 4012 after this
  release. Well-behaved consumers using `venc_ring_attach()` already
  read `slot_data_size` from the header and compute slot stride from
  it, so they handle the change automatically. A consumer that hard-
  codes a 1412-byte slot stride or uses a fixed-size read buffer below
  4012 will need to be updated.
- **Audio path tracks live updates too.** `Star6eAudioOutput.max_payload_size`
  is now updated in the live apply alongside the video state, so audio
  compact-mode chunking uses the new value on the next audio frame
  (RTP audio doesn't fragment, so the field is unused there but kept in
  sync for future-proofing).
- New optional callback `VencApplyCallbacks.apply_max_payload_size`,
  implemented in `star6e_controls.c` (covers dual-stream second channel)
  and `maruko_controls.c`.
- Cleanups while in the area:
  - `Star6eOutputSetup.max_frame_size` field and the
    `max_payload` parameter to `maruko_output_init_shm` were both
    rendered dead by sizing SHM rings to the ceiling; removed along
    with the now-redundant `*_output_max_payload_cap` helpers and
    SHM cap checks (validation is the single gate).
  - Removed a stale `outgoing.max_payload_size` paragraph in
    `HTTP_API_CONTRACT.md` that described an "adaptive algorithm" no
    longer in the codebase.

## [0.9.0] - 2026-04-26

Two themes shipped together:

### SIGHUP / `/api/v1/restart` cold restart via fork+exec (Star6E)

SIGHUP / `/api/v1/restart` rebuild the full pipeline including a
sensor-mode change, via process-level fork+exec respawn (Star6E).

- **Single reinit path.**  `SIGHUP`, `GET /api/v1/restart`,
  `GET /api/v1/defaults`, and `MUT_RESTART` `/api/v1/set` all enqueue
  the same request: clean teardown of the running venc, fork a child
  that execv's `/proc/self/exe`, parent exits.  The child inherits
  zero MI/ISP/sensor state from the kernel driver — it's a true cold
  boot at the SDK level, identical to a `killall venc; venc &` cycle.
- **`venc_api_request_reinit()` collapsed** from `int mode` (0/1/2
  priority queueing) to bool.  Every call site — SIGHUP handler,
  `/api/v1/restart`, `/api/v1/defaults`, `MUT_RESTART` set — enqueues
  the same request.
- **Removed:** `star6e_pipeline_reinit()` and
  `star6e_pipeline_stop_venc_level()` (partial-reinit codepath
  replaced by process-level respawn).  See git history for the
  abandoned escape hatch.
- **New helpers:**
  - `star6e_runtime_respawn_pending()` — `main()` checks this after
    backend teardown.
  - `star6e_runtime_respawn_after_exit()` — fork+execv successor.
  - `prctl(PR_SET_NAME, "venc-wd")` in the watchdog fork so the new
    venc's `is_another_venc_running()` skips it.
- **Audio `g_ai_persist` hack kept** — pipeline_stop's
  MI_AI_Disable cycle deadlocks `CamOsMutexLock` and would hang the
  parent's teardown past the watchdog window.  Kernel cleans up AI
  state on process exit anyway.
- **Watchdog timeouts** (process exit only): `alarm()` 5 → 2 s,
  watchdog poll 8 × 1 s → 6 × 500 ms, post-`SIGKILL` grace 3 → 1 s,
  VENC drain 500 → 150 ms.  ISP channel wait kept at 2000 ms (bench
  testing showed cuts cause "ISP channel readiness timeout" warnings).
- Maruko backend untouched (the rcvalue-vs-bool ripple from the
  reinit signature change is the only edit; the Maruko in-process
  reinit path stays).
- Docs: `documentation/SIGHUP_REINIT.md` rewritten with the
  fork+exec design and full bench evidence;
  `documentation/LIVE_FPS_CONTROL.md` "Mode Switching Limitation"
  section removed; `documentation/CRASH_LOG.md` added with the
  sysrq-b remote-recovery trick.

**Bench validation (2026-04-26, imx335 @ 192.168.1.13):** 24/24
consecutive cross-mode SIGHUPs (modes 0→1→2→3, 6 rounds) with no
degradation, no zombies, no dmesg faults.  Cycle time 393–795 ms to
respawn marker; ~13 s to new venc HTTP up (cold start dominates).
Phase 1 plan's in-process `MI_SYS_Exit` + `MI_SYS_Init` approach was
empirically disproven (PID-tied "already_inited" flags trip
`MI_DEVICE_Open` hangs); partial-reinit-without-MI_SYS-Exit survives
~4 cycles before VIF bindmode sync errors.  Process-level respawn is
the only path that scales.

### Hand-rolled config pretty printer (stable disk layout for `/etc/venc.json`)

- **Replace `cJSON_Print` in `venc_config_save`** with a hand-rolled
  emitter (`config_render_pretty` in `src/venc_config.c`). Every WebUI
  `/api/v1/set` save and every `record.path` save now produces the same
  canonical layout: 2-space indent, one key per line, `": "` separator,
  no blank lines between sections, single trailing newline. cJSON's
  tab-indented "shattered" pretty print is gone from the disk path.
  Parsing and HTTP API responses still use cJSON unchanged.
- **Unified layout for `config/venc.default.json`.** The hand-authored
  irregular layout (some sections one-line, others multi-line, mixed
  per-section indent rules) is replaced by the same canonical layout the
  printer emits, so the default file matches what venc actually writes
  on first save.
- **Self-policing round-trip test** (`test_save_layout_byte_equal` in
  `tests/test_venc_config.c`): loads `config/venc.default.json`, saves
  via `venc_config_save`, asserts the saved bytes are byte-equal to the
  original. Any future config field added to the struct/parser/serializer
  but missing from the printer (or the default file) trips the test.
- **`AGENTS.md` sync rules updated**: the per-section `render_*` helper
  in `src/venc_config.c` is now an explicit sync point alongside the
  struct, parser/serializer, API field+alias tables, WebUI `SECTIONS[]`,
  and `config/venc.default.json`.
## [0.8.1] - 2026-04-25

SD-card recording browser (dashboard tab + JSON API):

- **New `Recordings` tab on the dashboard** (`web/dashboard.html`).
  Fourth tab next to `Settings | API Reference | Image Quality`; the
  REC indicator in the top bar is now clickable and switches to it.
  Lists `.ts`/`.hevc` files in the configured `record.dir`, shows
  free/total bytes, current `record.mode`, live `recording:` state
  with frames + bytes + segment counter, start/stop buttons, per-file
  download + delete.  A 2 s poll of `/api/v1/recordings` +
  `/api/v1/record/status` runs while the tab is visible so the active
  recording's counters tick live; interval is cleared on tab switch.
- **Mode-aware start/stop gating.**  When `record.mode` is `dual` or
  `dual-stream` and `record.enabled` is true, the dedicated recording
  thread owns the recorder and the runtime silently skips the HTTP
  `/api/v1/record/start|stop` poll (`star6e_runtime.c`'s `if (!ps->dual)`
  guard).  The tab now reads `record.mode` from `/api/v1/config` and
  disables the Start/Stop buttons with a reason note in those cases,
  instead of letting clicks succeed but produce nothing.  Full truth
  table in `README.md`.
- **New JSON endpoints** (documented in `HTTP_API_CONTRACT.md`):
  - `GET /api/v1/recordings` — list files with `name`, `size`, `mtime`
    plus `free_bytes` / `total_bytes`.  Built in a growing heap buffer
    with proper JSON escaping (no silent truncation, no corruption of
    filenames containing `"` or `\`).  Capped at 512 entries; the
    response includes a `truncated` flag that the UI surfaces as a
    warning so the user doesn't assume old files vanished.
  - `GET /api/v1/recordings/download?file=<name>` — streams the file
    via the new shared `httpd_send_file()` helper (`send(MSG_NOSIGNAL)`
    so a mid-download client disconnect can't kill the server; RFC 5987
    `filename*=UTF-8''…` header).
  - `GET /api/v1/recordings/delete?file=<name>` — `unlink()`.  Refuses
    the currently-recording file by comparing inodes via `(dev, ino)`
    (a path-string compare would be defeated by trailing slashes or
    symlinks); returns 409 `record_active` in that case.
- **Shared httpd plumbing**, reusable by future endpoints:
  - `httpd_query_param()` — URL-decoding (percent + `+`) query parser
    in `venc_httpd.c`.
  - `httpd_send_file()` — streams a file with proper `Content-Length`
    and `Content-Disposition`; shares socket helpers already used by
    the rest of the server.
  - `venc_api_get_record_dir()` + `venc_api_fill_record_status()` —
    mutex-safe accessors exposing the active config directory + live
    recorder state (used to safely read config from the httpd thread).
- **Safety.**  Filenames validated (no path separators, no leading `.`,
  no control bytes); all JSON output properly escaped; no large stack
  frames on the httpd thread (entries buffer is `calloc`'d).
- **HTTP API contract** bumped `0.6.2` → `0.6.3` (non-breaking: three
  new endpoints, one new error code `record_active`).
- **Tests.**  `tests/test_venc_httpd.c` +11 cases for
  `httpd_query_param` covering percent-decoding, `+` → space, UTF-8,
  prefix collisions, empty values, trailing / invalid `%`, and buffer
  truncation safety.
- **Build.**  `tools/build_webui.py` and Makefile updated: dashboard
  blob re-regenerated from `web/dashboard.html`; new `src/venc_recordings.c`
  added to `CONFIG_SRC` and `TEST_LIB_SRCS`.

Implements the intent of PR #48 (PaddyP90) while fixing the safety
bugs flagged in that review: filename validation including `..`, JSON
escaping, heap buffer sizing, disconnect handling, and mutex-safe
config access.  Star6E only; Maruko lacks the `MI_VENC_TS_RECORDER`
plumbing so `/api/v1/record/status` reports `active: false`
there — but the list/download/delete endpoints still work for files
placed in `record.dir` by other means.

Originally drafted as a 0.7.12 fork-only release before the v0.8.0
upstream catch-up; rebased onto v0.8.0 and re-released as 0.8.1.

## [0.8.0] - 2026-04-25

Drop the EIS module and migrate the Star6E debug OSD from
`MI_RGN_PIXFMT_ARGB4444` (16 bpp) to `MI_RGN_PIXFMT_I4` (4 bpp,
two pixels per byte).  Major bump because `eis.*` config fields and
the EIS internal headers are removed; nothing else in the public HTTP
API or config schema breaks.

### EIS removal

An empirical sensor-mode sweep on Star6E IMX335 established that EIS
only worked in one validated config (sensor mode 3 native 1920x1080
+ real IMU + ≤90 fps); every other combination silently stalled the
encoder via `MI_VPE_SetPortCrop` interactions with VPE scaling,
VIF-side crop, or pixel-rate ceilings.  Increasingly elaborate guards
were added to refuse EIS in the broken cases, but the surface area
isn't worth maintaining for a single-config feature, and a future
LDC-warp rewrite (Phase C in `documentation/EIS_INTEGRATION_PLAN.md`)
would replace this code anyway.

- **Removed:** `src/eis.c`, `src/eis_gyroglide.c`, `include/eis.h`,
  `include/eis_gyroglide.h`, `include/eis_ring.h`,
  `tests/test_eis_gyroglide.c` (~1100 LoC).
- **Pipeline init:** EIS init/teardown blocks deleted from
  `bind_and_finalize_pipeline()` and `pipeline_stop()` /
  `pipeline_stop_venc_level()`.  All VPE-scaling / VIF-crop / testMode /
  pixel-rate refusal guards are gone with them — they only existed to
  make EIS misconfiguration loud.
- **Per-frame:** `eis_update()` removed from
  `star6e_runtime_process_stream()`.  `imu_drain()` still runs
  per-frame so a future telemetry consumer slots in cheaply.
- **OSD:** EIS visualization (1/3-scale crop miniature in
  bottom-right + crop/off/margin text rows) removed from
  `star6e_runtime.c`.  Debug OSD now shows only fps and CPU.
- **Config:** `eis.*` fields dropped from `VencConfig`,
  `venc.default.json`, the `/api/v1/set` snake-case alias map,
  and the WebUI dashboard (tab + tooltips + enum).
- **IMU module retained.**  `src/imu_bmi270.c` + `include/imu_bmi270.h`
  stay in the build; `imu.enabled` defaults to `false` so the BMI270
  is never opened unless explicitly enabled.  The push callback
  `star6e_pipeline_imu_push()` is now a stub — samples are discarded
  unless a future consumer (telemetry export, gcsv-style file logging
  for Gyroflow post-process) is wired in.

### Debug OSD: ARGB4444 → I4 format migration

Two-step migration delivered in one release.  First the rasterizer is
extracted into a pure host-testable module, then the MI_RGN backing
format is dropped from 16 bpp ARGB4444 → 8 bpp I8 → 4 bpp I4
palette-indexed.  Canvas footprint at 1920x1080 goes from 4.0 MB to
1.0 MB; OSD-on CPU drops by 74 % on Star6E IMX335.  Encoder hot path
is unchanged — the win is entirely in the OSD-on cost.

- **Pure rasterizer extracted.**  New `src/debug_osd_draw.{c,h}` holds
  the font, palette, dirty-rect logic, and drawing primitives.  The
  MI_RGN glue in `src/debug_osd.c` is now a thin wrapper.  The pure
  module compiles on the host and is exercised by
  `tests/test_debug_osd.c` (76 assertions covering every primitive,
  clipping, dirty-rect expansion, glyph rendering, and hashed
  composite-scene goldens).
- **OsdCanvas API.**  `stride_px` renamed to `stride_bytes` (now bytes
  per row regardless of pixel format).  `width` is still logical
  pixels.  `osd_fill_pixels(canvas, x, y, count, color)` handles I4
  nibble alignment internally:
    - Unaligned start (odd x): RMW the high nibble of byte (x/2).
    - Byte-aligned middle: `memset` the doubled-nibble byte
      `(color << 4) | color` over `(end - x) / 2` bytes.
    - Unaligned tail (end odd): RMW the low nibble of the last byte.
  Drawing primitives (`osd_draw_rect`, `osd_draw_char`) call this in
  place of their old byte-pointer inline math.  `osd_get_pixel` reads
  back the unpacked nibble through the same code path the rasterizer
  uses; production drawing never reads back.
- **Palette: 16 entries** (was 256 in I8, ARGB4444 in 16 bpp).
  Entries 1..8 map to `DEBUG_OSD_*` color constants; semi-transparent
  entries reuse the 4-bit ARGB4444 codes (0x4 → 68, 0xA → 170) so
  visual output is unchanged vs. the ARGB4444 implementation.  Entries
  9..15 are zeroed reserved.
- **MI_RGN region pixfmt:** `MI_RGN_PIXFMT_I4`.  Wire stride at
  1920x1080 is 960 bytes (was 1920 for I8; 3840 for ARGB4444).
- **Track-points use case** (filled small rects from upstream PR #23,
  motion-vector markers): supported unchanged via existing
  `debug_osd_rect` API; dirty-rect tracking gives sub-canvas clear,
  more efficient than the upstream's full-canvas `memset(0xFF)`.
- **Public API source-compatible.**  `debug_osd_rect/point/line/text`
  signatures unchanged; `DEBUG_OSD_*` constants are still palette
  indices (now 0..15 max).

Hardware comparison (Star6E IMX335 @ 90 fps, 30 s samples):

| Format          | Actual fps | CPU/core   | OSD-on cost vs baseline |
|-----------------|-----------:|-----------:|------------------------:|
| ARGB4444 (pre)  |      87.00 |    26.80 % |              +20.53 pp |
| I8 (intermediate) |    90.00 |    17.43 % |              +11.30 pp |
| **I4 (this)**   |  **90.00** | **11.70 %** |          **+5.43 pp** |

I4 cuts OSD CPU −74 % vs the original ARGB4444 path, and hits 2.36×
the fps-per-CPU% of ARGB4444 (7.69 vs 3.25).

### Bundles fork-only history

This release subsumes fork-only intermediate tags 0.7.12 (OSD I8
extraction), 0.7.13 (OSD I8 → I4), and 0.7.14 (EIS removal).  Bundled
because they share a hardware test footprint and were never released
upstream as separate tags.

## [0.7.11] - 2026-04-19

Pre-merge review fixes prior to upstream sync.  Two functional bugs
plus four polish items, all verified end-to-end on Star6E (IMX335 at
192.168.1.13) and Maruko (IMX415 at 192.168.2.12).

- **B1 — Maruko: split ISP-bin gate from CUS3A gate.**  v0.7.10's
  auto-detect fallback was silently a no-op on Maruko reinit because
  both `pipeline_common_resolve_isp_bin` and `maruko_load_isp_bin`
  were gated under `g_mi_isp_initialized`, which is set once and never
  cleared.  Resolve+load now runs every configure with a Star6E-style
  `g_last_isp_bin_path` cache; CUS3A enable + cold-boot exposure cap
  stay one-shot under the deadlock-protection gate.  Verified: 3
  successive `/api/v1/restart` cycles transitioned configured ->
  auto-detect fallback -> new configured -> restored.
- **B2 — IDR rate limiter: CAS loop for thread-safe spacing.**  The
  load-then-store pattern on `last_us` left a race where two
  concurrent producers could both pass the spacing check on the same
  `last` value and both honor an IDR inside the window — the exact
  storm-coalescing guarantee the gate was added for.  Replaced with
  `__atomic_compare_exchange_n` so exactly one caller wins each
  window.  ACQ_REL on the winning store synchronizes with the next
  caller's ACQUIRE load.  Verified: 100 concurrent `/request/idr` ->
  9 honored / 91 dropped (~10 honored/s, matches 100 ms spacing over
  the ~1 s curl burst).  All 18 idr_rate_limit unit tests still pass.
- **M1 — `venc_config_save`: preserve symlinks + mode bits.**  Resolve
  `path` via `readlink()` before writing the temp file so a symlinked
  `/etc/venc.json` is replaced in-place rather than being replaced by
  a regular file.  Preserve the existing target's mode bits via
  `stat()`+`fchmod()` so saves no longer silently widen 0600/0640 to
  0644.  Open the directory with `O_DIRECTORY`, propagate dir-fsync
  errors, retry trailing-newline write on EINTR.  Verified live:
  symlink intact, target mode 0640 preserved.
- **M2 — `/api/v1/defaults`: pick reinit mode based on save success.**
  The handler unconditionally requested reinit mode 1
  (reload-from-disk).  On disk-save failure (`EROFS` / `ENOSPC` /
  perm), the reload silently overlaid the stale on-disk config onto
  the in-memory defaults and reverted most of them.  Use mode 2
  (apply in-memory) when `save_rc != 0` so the operator at least gets
  the defaults they asked for at runtime.
- **M3 / L1 / L3 — Star6E hygiene.**
  - `prepare_pipeline_config`: stale comment about isp_bin_path
    resolution location refreshed (v0.7.10 moved it from
    `select_and_configure_sensor` to `bind_and_finalize_pipeline`).
  - `stop_venc_level`: stop and join `dual_rec_thread` BEFORE
    `star6e_output_teardown(&dual->output)`.  The thread calls
    `star6e_video_send_frame(&dual->output, ...)` inside its loop;
    tearing down output first left a use-after-close window.
  - `dual_rec_thread_fn`: always `usleep` after Query-empty (100 us on
    the fd path, 1 ms on the fallback) to prevent a runaway spin if
    the kernel ever signals POLLIN spuriously without a matching
    packet.

## [0.7.10] - 2026-04-19

Discoverable defaults + automatic ISP-bin selection:

- **`config/venc.default.json` lists `video0.size`.**  The field defaulted
  to `"auto"` in the parser but wasn't in the reference JSON, so users
  copying the file as a template never saw it.  Added with the same
  `"auto"` value.
- **Automatic ISP-bin fallback (both backends).**  New
  `pipeline_common_resolve_isp_bin()` runs after `sensor_select` and:
  1. Uses `isp.sensorBin` if non-empty and readable.
  2. Otherwise tries `/etc/sensors/<lowercase prefix>.bin` keyed off the
     live sensor name (`IMX335_MIPI` → `imx335`).
  3. Falls back to "no bin" (driver defaults).

  Stock devices that already ship `/etc/sensors/imx335.bin`,
  `/etc/sensors/imx415.bin`, etc. now run without per-host config.  A
  configured-but-missing path logs a warning and uses the fallback so
  typos and renamed bin files no longer cripple AE/AWB.  Logs the
  resolution decision once per pipeline start: `> ISP bin: %s
  (configured | auto-detected for sensor 'imx335') | none (no fallback…)`.
- **Star6E `Star6ePipelineConfig.isp_bin_path`** changed from
  `const char *` to `char[256]` to hold the resolved path.  Maruko
  `MarukoBackendConfig.isp_bin_path` got the same treatment.
- **Tests.**  10 new cases in `test_pipeline_common`: configured +
  readable, configured + missing, NULL/empty sensor name, no-alnum-prefix
  sensor name, NULL/zero output buffer (1190 tests, was 1180).

## [0.7.9] - 2026-04-19

Aspect-ratio crop is now opt-out (Star6E):

- **`isp.keepAspect` config (default `true`).** When `false`, VIF
  captures the full sensor area and VPE stretches it to the encode
  dimensions instead of center-cropping to preserve geometry. New
  parameter on `pipeline_common_compute_precrop()` keeps both call sites
  branch-free. Maruko parses but ignores the field until SCL crop port
  lands.
- **Reinit path now tracks the precrop currently programmed in VIF**
  (`state->active_precrop`) and compares against the freshly computed
  rect. A `keepAspect` toggle that doesn't change image dimensions now
  correctly triggers a VIF+VPE reconfigure. The previous code only
  re-armed precrop when `image_width`/`height` differed from the prior
  config.
- **Reinit branch reorganized.** `if (precrop_changed)` runs the full
  VIF+VPE rebuild; `else if (dims_changed)` falls through to the
  VPE-port-only resize. Equal-on-both-counts skips the block. The prior
  shape (outer `if (dims_changed)` then inner precrop check) couldn't
  express the keepAspect-toggle case, and the obvious patch-style fix
  would have re-fired the VPE-port resize on no-change reinits.
- **Reinit log shows precrop.** `> Reinit: VIF+VPE reconfigure %ux%u
  -> %ux%u (precrop %ux%u+%u+%u)` so an operator can tell whether the
  trigger was a resolution change, an AR change, or a keepAspect toggle.
- **HTTP API contract bumped to 0.6.2.** `isp.keepAspect` field added to
  `/api/v1/config`, `/api/v1/set` accepts both snake_case and the
  Majestic-style camelCase alias.
- **WebUI dashboard exposes the toggle** under the ISP section; embedded
  gzip regenerated via `make webui`.  Also drops the stale `isp.exposure`
  entry that lingered after the field was removed in 0.7.0.
- **Active precrop visible via API.** New
  `venc_api_set_active_precrop()` / `venc_api_get_active_precrop()` are
  called from the Star6E pipeline whenever VIF is (re)programmed.
  `/api/v1/config` gains a `runtime.active_precrop` block alongside the
  config; Star6E `/api/v1/ae` includes the same rect under
  `data.runtime.active_precrop`.  Maruko reports nothing until it gains
  precrop support.  Useful for confirming a `keepAspect` toggle landed
  without grepping the log.
- **Unit tests.** New `compute_precrop` cases cover both `keep_aspect`
  values plus the 2-pixel alignment guarantee, and a new
  `test_active_precrop_setter` exercises the venc_api setter/getter
  including the cleared-store, overwrite, and NULL-out-pointer paths
  (1180 tests, was 1139).

## [0.7.8] - 2026-04-18

Pre-merge review fixes folded in (see PR-47 review notes):

- **Atomic config write.** `venc_config_save()` now writes to `<path>.tmp`,
  fsyncs the file, renames over the target, and fsyncs the containing
  directory.  Power cut mid-write (a real failure mode on FPV hardware)
  no longer truncates `/etc/venc.json` — you always get either the old
  or the new copy, never a partial.
- **Flash-write guard.** `venc_api_save_config_to_disk()` caches the
  last successfully-saved VencConfig and skips the write when the
  candidate is byte-identical.  Hot loops (adaptive-link re-asserting
  the same kbps, WebUI sliders landing on their current value) no
  longer wear flash.
- **Save errors surface.** `venc_config_save()` return value is now
  honored.  `/api/v1/defaults` response gains `"saved":bool`.  LIVE /
  RESTART `/api/v1/set` paths log a `WARNING: config save to X failed
  — in-memory change committed but on-disk copy is stale` to stderr so
  operators catch disk-full / readonly-FS conditions from the venc log.
- **SDK call return values logged.** `MI_SNR_SetOrien`,
  `MI_VPE_SetChannelParam` (reinit), and `MI_SNR_SetFps` (reinit) now
  log non-zero returns so BSP regressions surface instead of silently
  leaving the image upside-down or the sensor stuck at the wrong FPS.
- **Dashboard source tracked.** HTML authored in `web/dashboard.html`;
  `tools/build_webui.py` regenerates the embedded gzip deterministically
  (mtime=0, compresslevel=9).  New `make webui` and `make webui-check`
  targets; `make verify` runs `webui-check` to catch drift.

- **WebUI reinit + IDR fixes.** Four related bugs in the reinit/save
  path and one missing IDR-on-bitrate behaviour.
- **Fix #1 — FPS kick on live reinit.** `star6e_pipeline_reinit`
  (`src/star6e_pipeline.c`) now re-kicks `MI_SNR_SetFps` at the end so
  a live FPS change actually reconfigures sensor timing.  Previously
  the kick only fired during the initial `star6e_pipeline_start_vpe`
  legacyAe branch and the once-per-process CUS3A `fps_kick_done`
  gate — neither re-armed on reinit, so the sensor stayed stuck at
  its cold-boot timing (e.g. 100 fps when 120 was requested).
- **Fix #2 — Save & Restart actually saves.** Added
  `venc_api_set_config_path()` (called by star6e_runtime and
  maruko_runtime with `VENC_CONFIG_DEFAULT_PATH`).  Both LIVE
  (`apply_live_set_query`) and RESTART (`process_restart_set_query`)
  set paths now call `venc_config_save()` before returning, so every
  `/api/v1/set` round-trip persists to `/etc/venc.json`.  `handle_restart`
  is intentionally left pure (reload-from-disk only, matching SIGHUP);
  the per-set save takes care of persistence so the WebUI "Save &
  Restart" flow ends with the on-disk copy already matching memory.
  Bonus: manual file swaps (e.g. scp of a config backup) followed by
  `/api/v1/restart` reload exactly what was written.
- **Fix #3 — Restore Defaults actually restores.** New
  `GET /api/v1/defaults` endpoint (`handle_defaults`) writes
  compiled-in defaults to disk and triggers reinit.  WebUI JS
  `restoreDefaults()` rewired from `/api/v1/restart` to the new
  endpoint (embedded gzip regenerated).  Previously the button just
  reloaded the on-disk config — misleading, and did nothing if the
  file already matched in-memory state.
- **Fix #5 — IDR on bitrate change.** `apply_bitrate` in both
  `src/star6e_controls.c` and `src/maruko_controls.c` now issues
  `MI_VENC_RequestIdr` after `MI_VENC_SetChnAttr`, gated through the
  existing `idr_rate_limit_allow` so storm callers stay coalesced.
  The decoder now gets a fresh keyframe to resync against the new
  rate-control state instead of drifting on stale P-frames.
- **Fix #4 — image.mirror / image.flip** now re-apply on reinit.
  `MI_VPE_SetChannelParam` is only invoked during VPE creation inside
  `star6e_pipeline_start_vpe`; the non-aspect-ratio reinit path skipped
  VPE rebuild, so a mirror/flip toggle would persist to disk and log
  "Pipeline reinit complete" but never actually change the output.
  Added an unconditional `MI_VPE_SetChannelParam(0, ...)` at the end
  of `star6e_pipeline_reinit` carrying the current mirror/flip/3DNR
  params.  Config round-trip verified on 192.168.1.13; visual flip
  verification requires a live decoder (operator check).

## [0.7.7] - 2026-04-18

- **Perf-series PR-C.1 — port MI_VENC_GetFd + poll() blocking wait to
  the Maruko main encoder loop.** Follow-up to PR-C (Star6E dual_rec
  only).  Maruko's main encoder loop in `maruko_pipeline_run`
  (`src/maruko_pipeline.c:1291`) was spinning on `maruko_mi_venc_query
  + usleep(500)` — ~2000 syscalls/s during idle gaps.  Replaced with
  `poll(MI_VENC_GetFd, 1000 ms)` and a wall-clock idle-abort timer
  (20 s of no frames → abort, preserved from the original
  `idle_counter * 500us` logic).
- **Fallback preserved:** if `MI_VENC_GetFd` returns < 0 on an unknown
  BSP variant, the loop falls back to the original Query+usleep(500)
  spin.  POLLERR/POLLHUP/POLLNVAL on the fd path drops into the
  fallback for the rest of the run.
- **Lifecycle:** `MI_VENC_CloseFd` called at cleanup when the fd was
  acquired.  The fd function pointers were already loaded by
  `maruko_mi.c` (dlsym'd but unused before this PR).
- **New bindings** (`include/maruko_bindings.h`): `maruko_mi_venc_get_fd`
  and `maruko_mi_venc_close_fd` macros alongside the existing
  MI_VENC_* wrappers.
- **Wall-clock idle timeout** consolidates the old dual idle paths
  (500us-keyed counter) into a single `wb_monotonic_us()`-based
  deadline that works identically on both the fd path (rare wakeups)
  and the fallback spin path (frequent wakeups).

## [0.7.6] - 2026-04-18

- **Perf-series PR-C — dual_rec_thread blocking wait via MI_VENC_GetFd.**
  Third of the 2026-04-18 perf series.  Replaces the 1-ms `usleep` spin
  in the dual-recorder thread with a `poll()` on the VENC channel's
  kernel fd (`MI_VENC_GetFd`).  The fd signals `POLLIN` when a frame is
  ready, so the thread wakes once per frame (~120/s at 120 fps) instead
  of ~1000/s from the old 1 ms spin — ~88 % fewer syscalls during
  recording.
- **Fallback preserved:** if `MI_VENC_GetFd` returns < 0 on an unknown
  BSP variant, the thread falls back to the original
  `MI_VENC_Query + usleep(1000)` loop — zero behaviour change on SDKs
  that don't expose the fd.
- **Lifecycle:** `MI_VENC_CloseFd` is called on thread exit when the fd
  was acquired.  The fd function pointers were already loaded by
  `star6e_mi.c` / `maruko_mi.c` (dlsym'd but unused before this PR).

## [0.7.5] - 2026-04-18

- **Perf-series PR-B — IDR request rate-limit gate.** Second of the
  2026-04-18 perf series.  Addresses the latent stability hazard where
  five independent IDR producers (scene detector, HTTP `/request/idr`
  and `/api/v1/dual/idr`, controls-apply, recorder-start) could storm
  `MI_VENC_RequestIdr` without coordination — a bug-driven burst
  (mis-tuned scene threshold during a camera pan) can crater per-frame
  bitrate by chaining forced keyframes.
- **New module (`include/idr_rate_limit.h`, `src/idr_rate_limit.c`):**
  per-channel (up to 8) last-honored timestamp + honored/dropped
  counters.  `idr_rate_limit_allow(chn)` enforces a compile-time
  `IDR_RATE_LIMIT_MIN_SPACING_US` of 100 ms — at 120 fps that is 12
  frames between honored forced IDRs, well below the GOP period
  (~83 ms at GOP=10, which auto-inserts an IDR without RequestIdr).
  State is lock-free (`__atomic_` load/store on `uint64_t`/`uint32_t`).
- **Wired through the 5 producer sites:**
  - `src/star6e_runtime.c` — `star6e_scene_request_idr`,
    `runtime_request_idr`
  - `src/star6e_controls.c` — `request_idr` (backend callback for HTTP
    `/request/idr`)
  - `src/venc_api.c` — `handle_dual_idr` (HTTP `/api/v1/dual/idr`);
    coalesced response returns `{"coalesced":true}`
  - `src/maruko_pipeline.c` — `maruko_scene_request_idr`
  - `src/maruko_controls.c` — `apply_qp_delta` IDR reissue
- **New endpoint `GET /api/v1/idr/stats`** returns per-channel honored
  and dropped counts plus the configured `min_spacing_us`.  Used by
  `tools/idr_storm.sh` to validate the gate.
- **Unit tests (`tests/test_idr_rate_limit.c`, 20 cases):** first-call
  honored, burst coalescing, per-channel independence, out-of-range
  bypass, post-spacing honored, reset semantics.  1139 tests pass
  (up from 1119).

## [0.7.4] - 2026-04-18

- **Perf-series PR-A — clock wrapper + dual_rec Query dedup + bench infra.**
  First of a five-PR series landing the post-review performance findings
  from 2026-04-18 (see `bench/perf-series/README.md`).
- **Clock reads via vDSO (`include/timing.h`, `src/timing.c`):** New
  `wb_monotonic_us()` helper using `CLOCK_MONOTONIC` (vDSO fast path on
  ARMv7, ~100 ns/call) instead of `CLOCK_MONOTONIC_RAW` (real syscall,
  ~1500 ns/call on A7).  Replaces three duplicated local wrappers —
  `monotonic_us` in `star6e_video.c` and `maruko_pipeline.c`, and
  `now_us` in `rtp_sidecar.c`.  NTP slew is <500 ppm → <4 us drift over
  a 60 s bench window, well inside frame-timing measurement error.
- **dual_rec backpressure signal (`src/star6e_runtime.c`):** Replaced the
  post-`MI_VENC_ReleaseStream` peek `MI_VENC_Query` with an inspection of
  the pre-`GetStream` `stat.curPacks >= 2` condition.  Equivalent
  semantics (queue had a backlog before we consumed) at one fewer syscall
  per recorded frame (~120/s at 120 fps).
- **Perf-series bench harness (`bench/perf-series/`):** New
  `run_bench.sh` drives the Tier A/B/C bench recipe end-to-end (deploy,
  probe, collect); `compare.py` emits a markdown Delta table between two
  labels with a 1.5×sigma regression flag.  Baseline tag
  `perf-series-baseline` pinned at master `40b8435`.
- **Host microbench (`tools/clock_bench.c`):** 1 M-iteration loop over
  `CLOCK_MONOTONIC_RAW`, `CLOCK_MONOTONIC`, `CLOCK_MONOTONIC_COARSE` to
  validate the vDSO assumption on A7 before deploying the PR.
- **IDR-storm stress (`tools/idr_storm.sh`):** Infrastructure for PR-B
  validation; fires N `POST /api/v1/dual/idr` back-to-back and reports
  the honored:fired ratio.

## [0.7.3] - 2026-04-14

- **Star6E sidecar gate (parity with Maruko PR #37):** Gated the per-frame
  `rtp_sidecar_poll` / `monotonic_us` / `rtp_sidecar_send_frame` work in
  `star6e_video_send_frame` on `state->sidecar.fd >= 0`.  When the
  sidecar feature is disabled (port 0), these calls are now skipped
  entirely rather than relying on each callee's early return.
- **SHM write: iovec-style 3-segment ring put (`venc_ring.h`, both backends):**
  Added `venc_ring_write3(hdr, p1, p2)` so the producer no longer has to
  pre-flatten `payload1 + payload2` into an 8 KB `flat[]` stack buffer
  before calling `venc_ring_write`.  Drops one memcpy per fragmented RTP
  packet (H.265 FU), removes the 8 KB stack allocation, and eliminates
  the `RTP_BUFFER_MAX` size clamp on the SHM write path.
  Applied to `src/star6e_output.c::star6e_output_send_rtp_parts` and
  `src/maruko_video.c::maruko_video_send_rtp_parts`.
  `venc_ring_write` is preserved as a thin wrapper for existing callers
  (C and C++, including the wfb_tx patched consumer).

## [0.7.1] - 2026-04-12

- **Phase 5 — Maruko HEVC RTP parity (PR #32):** Extracted the HEVC RTP
  output stage into a shared `hevc_rtp` module (`include/hevc_rtp.h` +
  `src/hevc_rtp.c`). Both Star6E and Maruko now go through the same
  Aggregation Packet (type 48) builder, FU-A fragmentation, VPS/SPS/PPS
  prepend-on-IDR, and per-frame `HevcRtpStats`. `star6e_hevc_rtp.c` is
  now a thin stream-iteration wrapper (227 lines → 111 lines);
  `Star6eHevcRtpStats` becomes a typedef alias of `HevcRtpStats` so
  existing call sites are unchanged. Maruko's RTP output gets standards-
  compliant AP aggregation for the first time: hardware-validated on
  SSC378QE at H.265 CBR 118 fps / 8 Mbps — IDR frames pack
  VPS+SPS+PPS+IDR as a single AP packet (`ap 1/6` in `[pktzr]` verbose
  line) instead of 4 separate RTP datagrams.
- **`[pktzr]` verbose line on Maruko:** Matches Star6E's exact format
  (`nals N | rtp N | fill N B | single N | ap N/N | fu N`) so log
  tooling works across both backends.
- **H.264 RTP output removed from Maruko:** Maruko ships H.265-only on
  the RTP wire path. The H.264 path was never hardware-verified and
  Maruko's FPV use case is H.265 exclusive. Channel creation still
  accepts `codec=h264` for forward compatibility, but the frame sender
  emits a warning and drops output. Net -~130 lines in `maruko_video.c`.
- **New `test_hevc_rtp` suite** (3 tests, 16 assertions): AP packing of
  small NALs, AP→FU-A fallback on oversized NALs, VPS/SPS/PPS prepend
  behavior — uses a capture-callback harness (no sockets) so tests run
  in <1 ms. Existing Star6E AP/FU-A test still passes unchanged as
  regression guard.

## [0.7.0] - 2026-04-11

- **dlopen migration (both backends):** Both Star6E and Maruko now load all
  MI vendor libraries (SYS, VIF, VPE, VENC, ISP, SCL, SNR) at runtime via
  dlopen/dlsym instead of direct linking. Function pointers are dispatched
  through `_impl` structs (`g_mi_sys`, `g_mi_vif`, etc.) with macro wrappers
  so call sites are unchanged. Three-way preprocessor guards
  (`PLATFORM_STAR6E` / `PLATFORM_MARUKO` / test stubs) keep all paths clean.
  - Star6E: dependency-ordered loading (cam_os_wrapper → SYS → ISP/CUS3A
    with RTLD_LAZY for circular deps → VIF/VPE/SNR/VENC with RTLD_NOW).
  - Maruko: eliminated uClibc shim and 3+ MB of redundant libs on device.
  - New files: `star6e_mi.h/.c`, `maruko_mi.h/.c`.
  - Removed: `-lmi_*` link flags, `MARUKO_UCLIBC_DIR`, shim build rules.
- **Maruko IQ parameter system:** Full 60-parameter ISP image quality API
  for Maruko (Phase 2), matching Star6E's existing IQ support. Includes
  multi-field struct params (colortrans, r2y, OBC, etc.), dot-notation
  set, and export/import. New files: `maruko_iq.h/.c`.
- **Maruko sensor mode diagnostics:** Auto-cap exposure to sensor FPS
  for reliable 120fps cold-boot. Fix SCL clock configuration. Gain
  control and exposure callback improvements.
- **Disable AF in CUS3A:** Fixed-focus cameras (IMX415) no longer trigger
  AF motor init errors. All CUS3A enable sequences changed from
  `{1,1,1}` (AE+AWB+AF) to `{1,1,0}` (AE+AWB only). Post-override
  after EnableUserspace3A which internally re-enables AF.
- **Star6E VPE exit(127) fix:** Under dlopen, vendor MI_VPE_DisablePort
  calls exit(127) on non-existent channel. Fixed by probing channel
  with MI_VPE_GetChannelAttr before VPE teardown.
- **Bool cast safety:** MI_SNR_GetPlaneMode vendor function writes 4 bytes
  through a `_Bool*` pointer. Fixed with temp-int wrappers on both backends.
- **Known issues documented:** Maruko encoder stall after output
  disable/re-enable (`documentation/KNOWN_ISSUES.md`).
- **Build cleanup:** Removed dead `snr_toggle_test` and `snr_sequence_probe`
  build recipes (unbuildable without direct MI linking). Removed stale
  uClibc references from deploy scripts and docs.
- Added `sensors-src` submodule pointing to OpenIPC/sensors for sensor
  driver source reference.
- Added IMX335 IQ profile (`iq-profiles/imx335_greg_fpvVII-gpt200.json`).

## [0.6.1] - 2026-04-03

- Fix cold-boot 54fps lock with legacyAe: call MI_SNR_SetFps during
  pipeline startup to force sensor timing compliance when CUS3A is not
  active.
- Fix sidecar telemetry NULL pointer: enriched encoder feedback was
  never sent (enc_ptr was NULL instead of &enc_info).
- Scene detector: saturate frame_count to prevent EMA warmup re-entry
  after ~13h; cache frame_size/type to avoid redundant packet walks;
  skip spike logic entirely when disabled (threshold=0).
- Change video0.scene_threshold and scene_holdoff to MUT_RESTART
  (no live-apply pathway exists).
- Remove all enc_ctrl/encCtrl references from code and documentation.

## [0.6.0] - 2026-04-02

- Add inline scene detector in star6e_runtime.c (~150 lines) behind
  `video0.scene_threshold` config field.
  - Tracks frame size EMA, computes complexity (0-255).
  - Detects spikes above configurable threshold for holdoff consecutive frames.
  - Waits for spike to subside before requesting IDR (when threshold>0).
  - Two config fields: `video0.scene_threshold` (uint16, 0=off, 150=1.5x EMA
    spike detection), `video0.scene_holdoff` (uint8, default 2).
  - Default off (`scene_threshold=0`): no IDR injection — zero-risk default.
- Enrich RTP timing sidecar with per-frame encoder telemetry:
  `frame_type`, `complexity`, `scene_change`, `idr_inserted`,
  `frames_since_idr`.
- Add multi-field set to HTTP API: `GET /api/v1/set?a=1&b=2` applies
  multiple live fields atomically in one request.
- Add field capabilities endpoint with backend-specific support filtering:
  `GET /api/v1/capabilities` reports mutability and per-backend support.
- API improvements: camelCase alias table for Majestic-compatible clients,
  duplicate-field rejection in multi-set, mixed live/restart rejection.

## [0.5.0] - 2026-04-01

- Add debug OSD overlay for encoder diagnostics and EIS crop visualization.
  Disabled by default (`debug.showOsd`), zero runtime cost when off.
  - MI_RGN canvas overlay via dlopen — ARGB4444 pixel format, full-frame canvas
    with dirty-rect tracking (only clears/draws changed areas per frame).
  - Stats panel (top-left): fps counter, CPU% from /proc/stat, 3x scaled 8x8
    bitmap font with semi-transparent background.
  - EIS crop visualization (bottom-right): 1/3 scale miniature showing sensor
    area (white), margin boundary (yellow), and moving crop window (green fill).
  - NEON-accelerated row fill (vst1q_u16, 8 pixels per store, 2.4x vs naive).
  - Mutually exclusive with waybeam-hub `mod_osd_render` — both use MI_RGN
    global state on VPE channel 0.
  - Config: `"debug": { "showOsd": true }`, API: `debug.show_osd` (MUT_RESTART).
  - New files: `include/debug_osd.h`, `src/debug_osd.c`.

## [0.4.1] - 2026-03-27

- Fix IMU webui fields invisible: rename config keys `sampleRate` →
  `sampleRateHz`, `gyroRange` → `gyroRangeDps` to match dashboard SECTIONS.
- Add 5 missing default config keys: `eis.mode`, `record.bitrate`,
  `record.fps`, `record.gopSize`, `record.server`.
- Fix camelCase capabilities lookup for `swapXY` and `maxMB` in webui.
- Remove legacy `sendFeedback` outgoing config alias.
- Document config/webui/API four-layer sync rules in AGENTS.md.
- Fix cold-boot sensor framerate lock: poll ISP exposure limits up to 500 ms
  instead of skipping the shutter cap when struct is all-zero. Apply synthetic
  gain defaults as fallback so AE cannot converge on exposure > frame period.
- Add IQ enable/disable toggle: virtual `.enabled` field for non-bool params
  (e.g. `colortrans.enabled=0`). Import respects `enabled` JSON field.
  Dashboard shows toggle switch in expanded form for applicable params.

## [0.4.0] - 2026-03-22

- Add built-in web dashboard at `/` with Settings, API Reference, and
  Image Quality tabs. Served as pre-compressed gzip (14KB on the wire).
- Add multi-field IQ parameter descriptors: colortrans (3 offsets + 3x3
  matrix), r2y, obc, demosaic, false_color, crosstalk, wdr_curve_adv now
  expose all sub-fields via dot-notation set API and `"fields"` JSON object.
- Add IQ export/import: `GET /api/v1/iq` exports all 62 ISP params as JSON,
  `POST /api/v1/iq/import` restores them. Partial imports supported.
- Add all missing config sections to the API: record (including dual channel
  bitrate/fps/gopSize/server), EIS (12 params), IMU (7 params), full audio
  (6 params), and ISP extras (legacyAe, aeFps). Total: 75 controllable fields.
- Add FT_FLOAT field type for EIS float params with `%.6g` precision to
  prevent artifacts like `0.001` displaying as `0.0010000000474974513`.
- Add FT_UINT8 field type for `imu.i2c_addr` — fixes memory corruption where
  `FT_UINT` wrote 4 bytes to a 1-byte field.
- Consolidate frame-loss threshold into shared function with minimum 512 kbit/s
  absolute margin for low-bitrate streams and 200 Mbps overflow clamp.
- Add `g_iq_mutex` for thread-safe IQ query/set operations.
- Add `g_dual_mutex` for thread-safe dual channel HTTP handlers.
- Fix `#ifdef` to `#if HAVE_BACKEND_STAR6E` in dual_apply_bitrate (Maruko
  link error from upstream PR #18).
- Fix stream_packs memory leak in SIGHUP reinit path.
- Fix diagnostic JSON trailing comma when dlsym lookups partially resolve.
- Add snprintf overflow protection (`JSON_CLAMP` macro) in IQ query output.
- Add EINTR handling in httpd read loops.
- Move dual channel settings from raw JSON file parsing to VencConfigRecord
  struct fields, simplifying star6e_runtime.c.
- Increase HTTPD_MAX_ROUTES to 64, HTTPD_MAX_BODY to 8192.

## [0.3.4] - 2026-03-22

- Refresh the Star6E frame-loss threshold on live bitrate changes so
  `/api/v1/set?video0.bitrate=...` keeps frame dropping aligned with the
  updated main-channel bitrate.
- Refresh the Star6E dual-channel frame-loss threshold on
  `/api/v1/dual/set?bitrate=...` so ch1 live bitrate changes keep the same
  overflow protection policy as channel creation.

## [0.3.3] - 2026-03-18

- Add Opus audio codec via `libopus.so` (loaded at runtime; graceful fallback
  to PCM if absent). RTP payload type PT=120, 48kHz nominal clock per RFC 7587.
- Fix 48kHz audio on SSC338Q — three root causes:
  - I2S clock misconfiguration: `i2s.clock` must be `0` (MCLK disabled; I2S
    master generates clock from internal PLL). Setting clock=1 caused hardware
    to deliver 16kHz data regardless of `rate` field. Source: SDK reference
    `audio_all_test_case.c` which uses `eMclk=0, bSyncClock=TRUE`.
  - Ring buffer too small: `AUDIO_RING_PCM_MAX` was 1280 (16kHz stereo
    headroom). 48kHz mono frames are 1920 bytes; silent truncation produced
    invalid Opus frame sizes → `OPUS_BAD_ARG`. Increased to 3840 (48kHz
    stereo 20ms = 960×2×2).
  - `bSyncClock` was 0; set to 1 per SDK reference.
- Fix stdout filter not active on SIGHUP reinit: `stdout_filter_start()` was
  inside `start_ai_capture()` which is skipped when AI device persists across
  reinit. Moved to `star6e_audio_init()` to run on every init cycle.
- Fix `stdout_filter_stop()` ordering: `close(pipe_read)` moved after
  `pthread_join` to avoid closing the fd while the filter thread may still
  be reading from it.
- Add `stdout_filter_stop()` to fail path and libmi_ai unavailable early-return
  to prevent filter leaks on audio init failure.
- Remove dead `star6e_audio_clock_for_rate()` function.
- Increase DMA ring: `frmNum` 8→20 (400ms), prevents data loss under ISP/AE
  preemption bursts.
- Reduce output port depth to `user=1, buf=2` (was 2,4), saving ~40ms latency.
- Audio init survives SIGHUP reinit: AI device/channel state is persisted in
  `g_ai_persist` across reinit cycles to avoid `CamOsMutexLock` deadlock after
  2+ VPE create/destroy cycles.

## [0.3.2] - 2026-03-17
- Fix SIGHUP reinit D-state: switch from full pipeline_stop/start to partial
  teardown that keeps sensor/VIF/VPE running. The SigmaStar MIPI PHY does not
  recover from MI_SNR_Disable/Enable cycles — partial teardown avoids touching
  it entirely. VENC, output, audio, IMU/EIS are torn down and rebuilt; the
  VIF→VPE REALTIME bind stays active across reinit.
- Live resolution switching: `video0.size` API change now reconfigures the
  pipeline in-process without a process restart.
  - Same-aspect-ratio changes (e.g. 1920x1080 → 1280x720): VPE output port
    resize only — VIF and VIF→VPE bind are untouched.
  - Aspect-ratio changes (e.g. 1920x1080 → 1920x1440): full VIF crop
    reconfiguration + VPE destroy/recreate. VIF device stays running;
    MIPI PHY is never touched.
  - Overscan correction applied during reinit precrop: uses `mode.output`
    (usable area) rather than `plane.capt` (raw MIPI frame) for sensors that
    report overscan in the MIPI frame dimensions.
- Guard VIF→VPE bind in `bind_and_finalize_pipeline` to prevent double-bind
  on reinit. Without the guard, re-binding an already-live VIF→VPE port
  caused continuous `IspApiGet channel not created` dmesg errors.
- ISP channel readiness poll (`star6e_pipeline_wait_isp_channel`) called
  immediately after every new VIF→VPE bind. The ISP channel initialises
  asynchronously after `MI_VPE_CreateChannel`; the poll (up to 2000 ms,
  1 ms intervals) ensures the ISP is ready before the bin load and exposure
  cap APIs probe it, eliminating `IspApiGet` dmesg errors on both cold boot
  and AR-change reinit.
- `__attribute__((flatten))` on `star6e_pipeline_reinit`: forces GCC -Os to
  inline all static callees, preserving the stack layout that the SigmaStar
  ISP driver requires for `MI_VPE_CreateChannel` to succeed.
- Error-path state consistency in VIF+VPE reconfiguration: on failure after
  VPE is destroyed, `MI_VIF_DisableDev` is called to leave the pipeline in a
  cleanly-stopped state rather than a partially-configured one.
- Details: `documentation/SIGHUP_REINIT.md`

## [0.3.1] - 2026-03-16
- Reduce G.711 audio latency: scale frame size to `sample_rate/50` (~20ms)
  instead of hardcoded 320. Reduce MI_AI ring (frmNum 16→8), output port
  depth (4,16)→(2,4), fnGetFrame timeout 100→50ms.
- Add dynamic RTP payload types: PT=112 (PCMU non-8kHz), PT=113 (PCMA
  non-8kHz). Standard PTs (0, 8, 11) still used when rate matches RFC 3551.
- Clamp audio sample_rate to 8000-48000 in config parser.
- Default audio codec changed from `pcm` to `g711a` in venc.default.json.
- Remove `slicesEnabled`/`sliceSize`/`lowDelay` config fields (no firmware support on I6E).
- Add `frameLost` config field for frame-lost strategy (default: true).
- Fix kbps verbose overflow on 32-bit ARM (displayed ~400 instead of ~13000 at high bitrates).

## [0.3.0] - 2026-03-15
- Custom 3A thread for Star6E — replaces ISP internal AE/AWB with a
  dedicated 15 Hz thread (default, no config change needed):
  - AE: proportional controller with shutter-first priority, configurable
    target luma (100-140), convergence rate (10%), and gain ceiling (20x).
  - AWB: grey-world algorithm with IIR smoothing (70/30) and 2% dead-band.
  - Pauses ISP AE via `MI_ISP_AE_SetState(PAUSE)`, disables CUS3A AWB
    callback via `MI_ISP_CUS3A_Enable(1,0,0)`.
  - Periodic ISP AE state verification with automatic re-pause.
  - Manual AWB (`ct_manual`) pauses custom AWB; `auto` resumes it.
  - `isp.exposure` API syncs max shutter to the custom AE thread.
  - Set `isp.legacyAe: true` to revert to old ISP AE + handoff behavior.
- New config fields: `aeFps`, `legacyAe` in the `isp` section.
  Gain/shutter limits now seeded from ISP bin (`MI_ISP_AE_GetExposureLimit`).
- HW verified: all 4 imx335 sensor modes (30/60/90/120fps), cold-boot,
  live FPS switching, gemini dual recording, manual AWB transitions.

## [0.2.3] - 2026-03-14
- Restored working Star6E AE across IMX335 modes `30`, `60`, `90`, and `120 fps`:
  - Startup now primes CUS3A with `100 -> 110 -> 111`.
  - Steady state no longer forces periodic `110` refreshes.
  - A delayed one-shot `000` handoff returns the pipeline to a live AE state
    while preserving the requested encoder rate.
- Added Star6E AE diagnostics for live verification:
  - `GET /api/v1/ae`
  - `GET /metrics/isp`
  - Existing `GET /api/v1/awb` remains available for AWB inspection.
- Documented the verified AE recovery and updated the HTTP API contract for
  the diagnostics endpoints.

## [0.2.2] - 2026-03-11
- Fixed GOP keyframe interval to be relative to FPS (seconds, not raw frames):
  - `gopSize` is now a float representing seconds between keyframes.
  - `1.0` = 1 keyframe/second (GOP = fps frames). `0.5` = every 0.5s. `0` = all-intra.
  - Example: `gopSize: 0.33` at 90fps = keyframe every ~30 frames.
  - Changing FPS now automatically recalculates GOP frame count.
  - Default changed from `3` (frames) to `1.0` (seconds).
- Fixed autoexposure not restoring via HTTP API:
  - Setting `isp.exposure=0` via API now correctly restores auto-exposure
    (caps max shutter to frame period). Previously it was a no-op due to
    both args being zero in `cap_exposure_for_fps(0, 0)`.
- Known issue: AWB (Auto White Balance) behavior unverified on device.
  - CUS3A enables AWB (`params[1]=1`) but actual color correction depends on
    ISP bin calibration data. Requires on-device testing. See
    `documentation/AWB_INVESTIGATION.md`.
- Known issue: ROI QP not yet wired to encoder backend.
  - Config plumbing and HTTP API exist but `apply_roi_qp` callback is NULL.
  - SDK supports overlapping ROI regions with delta QP via
    `MI_VENC_SetRoiCfg`. Implemented as horizontal bands with signed QP
    (1-4 steps). See `documentation/ROI_INVESTIGATION.md`.

## [0.2.1] - 2026-03-10
- Added audio output via UDP with configurable codec and port:
  - Supported codecs: raw PCM, G.711 A-law, G.711 μ-law (software encoding).
  - Audio captured via MI_AI SDK (dlopen at runtime, graceful degradation if unavailable).
  - New `audio` config section: `enabled`, `sampleRate`, `channels`, `codec`, `volume`, `mute`.
  - New `outgoing.audioPort` field: 0 = share video port, >0 = dedicated audio port (default 5601).
  - Audio runs in a separate thread from the video streaming loop.
  - Dual packetization: compact mode (0xAA magic header) and RTP mode (PT 110, distinct SSRC).
  - Live mute/unmute via HTTP API (`audio.mute`, MUT_LIVE).
  - Star6E backend: full implementation. Maruko backend: warning stub.
- RTP mode now reads `maxPayloadSize` from config (was hardcoded to 1200):
  - Both star6e and maruko backends respect `outgoing.maxPayloadSize` for
    RTP FU-A/FU fragmentation threshold. Default 1400.
  - Config values above 1400 are supported for jumbo-frame networks.
- Added adaptive RTP payload sizing to reduce CPU churn from packet overhead:
  - EWMA tracks average P-frame size; IDR-like spikes (>3x average) are
    excluded to prevent distortion.
  - Target payload = avg_frame * fps / targetPacketRate, aiming for ~850
    packets/sec by default across all bitrates (adaptive bitrate up to 50 Mbit).
  - `outgoing.targetPacketRate` config field (default 850, MUT_RESTART).
    Set to 0 to disable adaptive sizing and use fixed maxPayloadSize.
  - 15% hysteresis prevents oscillation on frame-to-frame jitter.
  - Payload clamped to [1000, maxPayloadSize]. The 1000-byte floor keeps
    packet rate under ~500 pkt/s on low-MCS WiFi links (MCS0 slot budget).

## [0.2.0] - 2026-03-10
- Added output enable/disable control (`outgoing.enabled`, MUT_LIVE):
  - When disabled: FPS reduces to 5fps idle rate, frames encoded and discarded.
  - When enabled: FPS restores to previous value, IDR keyframe issued.
  - Default: `false` (no more implicit localhost:5000 fallback).
- Added live destination redirect (`outgoing.server`, MUT_LIVE):
  - Change UDP destination without pipeline restart.
  - IDR keyframe issued on destination change for stream continuity.
  - Re-connects UDP socket when `connectedUdp` is enabled.
- Added stream mode config field (`outgoing.streamMode`, MUT_RESTART):
  - Values: `"rtp"` (default) or `"compact"`.
  - Replaces scheme-derived mode detection; URI scheme must be `udp://`.
- Added connected UDP (`outgoing.connectedUdp`, MUT_RESTART):
  - When true: `connect()` called on UDP socket, skips per-packet routing
    lookup and enables kernel ICMP error feedback.
- Added IDR request after live bitrate change for immediate quality update.
- Updated HTTP API contract to v0.2.0.

## [0.1.7] - 2026-02-26
- Fixed ISP FIFO stall on overscan sensor modes (imx335 mode 2 @ 90fps):
  - Added periodic CUS3A refresh (~15 Hz) in stream loop to keep ISP event
    loop alive; runs in both idle and active paths so a stalled pipeline
    can recover.
  - Fixed overscan detection: removed 10% threshold that skipped correction
    for single-axis overscan (imx335 mode 2: crop 2560x1440, output 2400x1350).
    Changed to per-axis independent clamping.
- Simplified ISP 3A management (Star6E + Maruko):
  - Replaced per-frame AE cadence toggling and ISP3AHandle/ISP3AState machinery
    with one-shot `enable_cus3a()` at pipeline init + periodic `cus3a_tick()`.
  - Removed CLI flags: `--ae-on/off`, `--awb-on/off`, `--af-on/off`, `--ae-cadence`.
- Added ISP/SCL clock boost (384 MHz) after pipeline setup.
- Added `--oc-level` for hardware overclocking:
  - Level 1: VENC clock boost to 480 MHz.
  - Level 2: Level 1 + CPU pinned to 1200 MHz with performance governor.

## [0.1.6] - 2026-02-25
- Added AE cadence control (`--ae-cadence N`) for high-FPS throughput recovery:
  - Toggles CUS3A processing on/off every N frames to reduce per-frame CPU overhead.
  - Auto mode: when FPS >60, cadence defaults to fps/15 (e.g. cadence=8 at 120fps).
  - Manual override via `--ae-cadence N` for fine-tuning.
- Moved ISP bin load earlier in pipeline setup (after start_vpe, before streaming)
  to ensure correct ae_init state before first frame.
- Added overscan crop detection for sensor modes where mode.output < mode.crop:
  - When overscan exceeds 10% on both axes, VIF center-crops to the usable output area.
  - Fixes imx415 mode 1 hang (crop=2952x1656, output=2560x1440).
  - Threshold prevents false positives from driver metadata quirks.
- Enhanced `--list-sensor-modes` to show crop/output details when they differ.
- Cleaned up pipeline summary prints: explicit MIPI frame vs cropped dimensions,
  precrop line only shown for actual aspect-ratio cropping.

## [0.1.5] - 2026-02-25
- Improved agentic coding workflow in AGENTS.md:
  - Added structured error recovery loop (observe → diagnose → repair → re-verify → document).
  - Added incremental verification guidance: run `make lint` after each logical change.
  - Added long-session guidance: progress checkpoints, decision stability, scope control.
  - Added error diagnosis reference table for compiler, linker, runtime, and timeout failures.
  - Added deployment test interpretation: exit codes, JSON summary, dmesg guidance, agent decision flow.
  - Added "Mistakes to Avoid" entries for stacking unverified changes and mid-task approach switching.
- Added `make lint` target: fast compile-only check with `-Wall -Wextra -Werror` for both backends.
- Added lint step to CI workflow (runs before build).
- Synced dual-agent infrastructure (Claude Code + OpenAI Codex):
  - Updated all `.agents/skills/` and `.claude/commands/` with decision documentation,
    error recovery loop, and incremental lint steps.
  - Added `Bash(make lint*)` to Claude permissions; switched PostToolUse hook
    from full build to fast lint for tighter feedback loop.
  - Enhanced `.codex/config.toml` with `sandbox_mode = "workspace-write"`.
- Improved `remote_test.sh`:
  - Added SSH ControlMaster multiplexing for persistent connections.
  - Removed runtime lib deployment (libs already in `/usr/lib` on target).
  - Added `--json-summary`, `--skip-build`, `--skip-deploy` flags.
  - Added strict exit codes (0=success, 1=failed, 2=unresponsive, 124=timeout).
- Added `documentation/TARGET_AGENT_ARCHITECTURE.md` design doc (deferred implementation).

## [0.1.4] - 2026-02-23
- Added automatic precrop for Star6E: when encode resolution has a different aspect ratio
  than the sensor mode, the VIF center-crops the sensor frame to match the target aspect
  ratio before the VPE scales, eliminating non-uniform scaling distortion.
- Precrop uses integer cross-multiplication (no floats) with 2-pixel alignment enforcement.
- Informational log line printed when precrop is active (e.g. `Precrop: 1920x1080 -> 1440x1080 (offset 240,0)`).
- Fixed high-FPS throttling when AE is disabled: caps exposure to frame period after ISP bin
  load, preventing default 10ms shutter from limiting 120fps mode to ~99fps.

## [0.1.3] - 2026-02-23
- Added duplicate-process guard: venc now detects and exits if another instance is already running.
- Added `--version` / `-v` flag to print version and backend name.
- Added `--verbose` flag to gate per-frame stats output (previously always printed).
- Removed obsolete HiSilicon/Goke `-v [Version]` hardware presets from Star6E backend and help text.
- Simplified sensor mode selection: prioritize FPS match over resolution fit in both backends.
- Fixed Star6E cleanup ordering: socket and ISP 3A handle now properly released on all exit paths.
- Added informational prints for FPS mismatch, resolution clamping, and VPE scaling.
- Embedded build-time version from VERSION file via Makefile (`VENC_VERSION`).
- Updated help text branding from "HiSilicon/Goke" to "SigmaStar".
- Added crash/hang tracking policy and initial crash log (`documentation/CRASH_LOG.md`).
- Added SigmaStar Pudding SDK API reference link to proc reference and documentation index.

## [0.1.2] - 2026-02-22
- Added low-risk ISP CPU-control knobs in both standalone backends:
  - `--ae-off/--ae-on`
  - `--awb-off/--awb-on`
  - `--af-off/--af-on` (default AF off)
  - `--vpe-3dnr 0..7`
- Updated ISP bin load/reapply behavior to honor requested AE/AWB/AF state.
- Added documentation for CPU/latency tuning profiles and usage:
  - `documentation/AE_AWB_CPU_TUNING.md`
- Updated status/index docs to reflect implemented 3A/3DNR tuning controls.

## [0.1.1] - 2026-02-22
- Added formal HTTP API contract source-of-truth document:
  - `documentation/HTTP_API_CONTRACT.md`
- Added repository PR checklist template with explicit contract/version/doc gates:
  - `.github/pull_request_template.md`
- Added default JSON config template and planning artifacts for config/API migration:
  - `config/venc.default.json`
  - `documentation/CONFIG_HTTP_API_ROADMAP.md`
- Updated documentation/plan/process files to enforce:
  - Star6E-first rollout for SigmaStar API-touching features,
  - contract sync on HTTP changes,
  - SemVer + changelog workflow.

## [0.1.0] - 2026-02-22
- Baseline established for standalone-only repository scope.
- Targeted dual-backend builds in place (`SOC_BUILD=star6e`, `SOC_BUILD=maruko`).
- Runtime SoC autodetect removed from `venc`; backend is selected at build time.
- Default stream behavior aligned to RTP + H.265 CBR.
- Planning updates introduced for JSON config migration and HTTP control API roadmap.
