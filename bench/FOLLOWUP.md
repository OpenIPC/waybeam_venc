# Pipeline Performance — Follow-ups

Tracking items intentionally deferred out of this PR
(`feature/pipeline-perf-optimization` / PR #35). Each is a small,
well-scoped piece of work that builds on the landed changes.

## 1. Port Tier C (`sendmmsg` batching) to the Maruko backend

**Why.** Tier C collapses 19 per-frame `sendmsg()` calls into one
`sendmmsg()` call on Star6E. The final validation measured −18% user
CPU on the vehicle with no regression. Maruko still uses per-packet
`sendmsg()` in `src/maruko_video.c::maruko_rtp_write` and would
benefit from the same pattern identically.

**Mechanical changes (mirror of the Star6E implementation):**

- `include/maruko_output.h`: add a `MarukoOutputBatch` struct (same
  shape as `Star6eOutputBatch` — `scratch[64][1616]`, `iov[64*2]`,
  `msgs[64]`, `count`, `active`) embedded in `MarukoOutput`.
- `src/maruko_output.c`: add `maruko_output_begin_frame()` /
  `maruko_output_end_frame()` plus the flush helper.
- `src/maruko_video.c`:
  - Change `MarukoRtpWriteContext` to carry a `MarukoOutput *` (or
    the batch fields directly) instead of raw `socket_handle/dst/
    dst_len`.
  - In `maruko_rtp_write`, enqueue into the batch when active; copy
    header + payload1 into scratch (same aliasing reasons as Star6E
    — `fu_hdr[3]` and `HevcApBuilder` internals are both caller-stack
    buffers reused between packets).
  - In `maruko_send_frame_rtp`, call begin/end around the packetizer.
- `src/maruko_pipeline.c`: no changes — the begin/end are internal
  to the video send layer.

**Verification.** Same procedure as the final Star6E validation in
`bench/README.md`:

- `make build SOC_BUILD=maruko` clean.
- Deploy to a Maruko vehicle (e.g. `root@192.168.2.12`), hub stopped,
  bitrate 25 Mbps persisted in `/etc/venc.json`.
- 60 s `tools/rtp_timing_probe` + 30 s /proc KPIs.
- Expect user-CPU drop similar to Star6E (~−18%) and tail
  send-spread −5-10%.

**Risk.** Same AP/FU-A aliasing bug is the main trap; the Star6E code
shows how to avoid it. Build a branch, bench, don't merge until the
matched control confirms no decode regression.

## 2. Fix `waybeam_hub` `SIGHUP` reload of `venc.bitrate_enabled`

**Why.** During the bench work we discovered that `SIGHUP` to
`waybeam_hub` does not actually re-read the `venc.bitrate_enabled`
flag from `/etc/waybeam_vehicle.conf`. The flag is only sampled at
hub startup. This forced us to fully stop `waybeam_hub` for clean
benches, which introduced a scheduler-jitter confound that we had to
control for with matched runs.

**Consequence.** Until the hub honors live `bitrate_enabled` toggles,
the hub-running A-vs-master bench needed to validate Tier A's early
stream release (`MI_VENC_ReleaseStream` moved before verbose / record
/ OSD work) cannot be run cleanly. Tier A shipped without a production
bench; its value in the hub-running case is theoretical until that
test exists.

**Where to look.** The hub's `mod_venc` reload path. In `waybeam-hub`:

- `src/mod_venc.c` — the module that drives the `/api/v1/set` calls
  into venc based on aalink state.
- `src/hub_module_registry.c` and the generic reload callback —
  confirm `mod_venc_reload()` is wired up and called on SIGHUP.
- `src/mod_aalink.c` — the consumer of `bitrate_enabled`. Does it
  cache the flag at start, or re-read from `hub_config`?

**Expected fix size.** Small. Likely a missing
`cfg->venc.bitrate_enabled = hub_config_get_bool(...)` in the reload
callback, or a cached copy in `mod_aalink` that never re-reads from
`hub_config`.

**Validation after the fix.**

- Set `venc.bitrate_enabled = true`, start hub, verify bitrate gets
  clamped to aalink target.
- Set `venc.bitrate_enabled = false` via file + `kill -HUP`, verify
  bitrate stays wherever set via `/api/v1/set?video0.bitrate=25000`.
- Flip back to `true` via file + SIGHUP, verify aalink clamping
  resumes without a hub restart.

## 3. Optional (larger) — packetizer stable-buffer refactor

Flagged as "Open question for Tier C" in `bench/README.md`. Would
eliminate the ~100 KB `Star6eOutputBatch.scratch` by changing the
RTP packetizer API so the caller provides a stable output buffer
per packet instead of reusing `HevcApBuilder._internal.payload` and
stack-allocated `fu_hdr`.

Not worth doing until the current user-CPU win proves insufficient
under real-world load. Touches `src/hevc_rtp.c`, `src/rtp_packetizer.c`,
and both backends' RTP write callbacks — more scope than Tier A+B+C
combined.
