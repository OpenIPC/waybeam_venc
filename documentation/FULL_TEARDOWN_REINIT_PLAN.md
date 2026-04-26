# Full Teardown + Reinit on SIGHUP / Restart — Plan

Status: proposed, not implemented. This is a Phase 1 (Spec) document per
the workflow in `AGENTS.md`.

## Goal

Replace the current partial reinit path with a single full teardown +
reload + start path so that SIGHUP and `/api/v1/restart` can switch
sensor mode without a process restart. Live `/api/v1/set` paths for
`MUT_LIVE` fields are unchanged.

Operator-facing contract:

| Trigger | Action |
|---|---|
| `SIGHUP` (`killall -1 venc`) | reload `/etc/venc.json`, full teardown + start |
| `GET /api/v1/restart` | same as SIGHUP (no save) |
| `GET /api/v1/set?<MUT_RESTART>` | save to disk, full teardown + start |
| `GET /api/v1/defaults` | reset to defaults, save, full teardown + start |
| `GET /api/v1/set?<MUT_LIVE>` | unchanged — in-place hooks in `star6e_controls.c`, no reinit |

## Design constraints (from operator)

- **No state machine.** One reinit path. Either it works consistently
  across all sensor modes or this work is reverted and we keep today's
  partial reinit limitation.
- **No diff detection.** A SIGHUP/restart unconditionally tears down to
  the sensor and rebuilds, regardless of whether the new config differs.
- **Hard fail.** On any teardown or start failure, `exit` non-zero. No
  silent partial fallback. The existing watchdog (`star6e_runner_teardown`
  fork + `kill -9` + `sysrq-b`) is the worst-case safety net.
- **Faster, not safer.** Cycle time target ≤ 2 s (Maruko cold start is
  ~1 s; Star6E rebuilds more state). Reduce timeouts where today's values
  were padded for safety.

## Background — why partial reinit exists today

HISTORY 0.3.2 backed out a full teardown path because
`MI_SNR_Disable` + `MI_SNR_Enable` cycles on the I6E MIPI D-PHY put VIF
into a fault state, hung `MI_VENC_StopRecvPic`, and put the process in
D-state. Since then the codebase has gained:

- Drain-before-`StopRecvPic` + thread-join ordering in
  `star6e_pipeline_stop` (`src/star6e_pipeline.c:1165`).
- Watchdog with `kill -9` + `sysrq-b` fallback
  (`src/star6e_runtime.c:965`).
- VPE SCL preset workaround at shutdown (`src/star6e_pipeline.c:60`).
- `pre_init_teardown` cleanup of stale module state on the next start
  (`src/star6e_pipeline.c:79`).
- Single-mode sensor `skip_reinit` branch in `sensor_select.c:408`.

The load-bearing assumption is that these collectively make
`MI_SNR_Disable` survivable. The validation matrix below is the test of
that assumption.

## File-by-file changes

### `src/star6e_runtime.c`

- `star6e_runtime_restart_pipeline()` — replace body with:
  1. `star6e_pipeline_stop(&ctx->ps)`
  2. `venc_config_defaults(vcfg)` + `venc_config_load(VENC_CONFIG_DEFAULT_PATH, vcfg)`
  3. `star6e_pipeline_start(&ctx->ps, vcfg, &g_sdk_quiet)`
  4. `star6e_runtime_apply_startup_controls(ctx)`
  - On any non-zero return, propagate it so `star6e_runner_run` exits and
    the binary terminates with a non-zero code.
- Drop the `prev_max_fps` clamp block. Sensor mode is allowed to change.
- `star6e_runtime_handle_reinit()` — drop the 200 ms debounce
  `usleep(200000)` and the mode-1 vs mode-2 dispatch. One path.
- `handle_signal()` — `alarm(5)` → `alarm(2)`.
- `star6e_runner_teardown()` watchdog — `for (i=0; i<8; i++) sleep(1)`
  → `for (i=0; i<6; i++) usleep(500000)`. Post-`SIGKILL` grace
  `sleep(3)` → `sleep(1)`.

### `src/star6e_pipeline.c`

- Add `star6e_pipeline_reset_persist()` that clears `g_isp_initialized`,
  `g_last_isp_bin_path[0] = '\0'`, `g_cus3a_handoff_done`. Call it at
  the top of `star6e_pipeline_stop()` so the next `star6e_pipeline_start`
  is genuinely cold.
- `star6e_pipeline_wait_isp_channel()` — 2000 ms cap → 500 ms.
- `drain_venc_channel()` — callers pass 500 → 150.
- ISP `dlopen` fallback `usleep(100000)` (twice in
  `star6e_pipeline_wait_isp_channel`) → `usleep(20000)`.
- Delete `star6e_pipeline_reinit()` and
  `star6e_pipeline_stop_venc_level()`. They are unreachable after the
  runtime change above. Confirm via `grep` before deletion to avoid
  orphaning callers.

### `src/star6e_audio.c`

- Reset `g_ai_persist` state in `star6e_audio_teardown()`. The persist
  hack only existed to survive partial reinit; with full teardown each
  `star6e_audio_init()` is a true cold init.

### `src/venc_api.c`

- `venc_api_request_reinit(int mode)` → `venc_api_request_reinit(void)`.
  Drop the `if (mode > g_reinit)` priority check. `venc_api_get_reinit()`
  returns bool.
- All call sites collapse: previously `request_reinit(1)` and
  `request_reinit(2)` both become `request_reinit()`.
- Field/alias mutability tables unchanged. `MUT_RESTART` fields keep
  triggering reinit; their save-to-disk path is unchanged.

### `include/venc_api.h`

- Update prototype + comment block describing the reinit flag (currently
  documents modes 0/1/2 — collapses to a boolean).

### Documentation

- `documentation/SIGHUP_REINIT.md` — rewrite. Drop the "MIPI PHY must
  not be cycled" rationale (or move to a "Historical" section noting
  fixed in vN.N). Update the diagram to show full teardown.
- `documentation/LIVE_FPS_CONTROL.md:87` — drop the "Mode Switching
  Limitation" section. `video0.fps` change still uses the live bind
  decimation path; sensor mode change now works via SIGHUP/restart.
- `HISTORY.md` + `VERSION` bump per project policy
  (one bump per PR).

### Maruko

Out of scope for this change. Per the backend split policy in
`AGENTS.md`, Star6E is implemented and validated first. The clamp at
`src/maruko_runtime.c:99` stays. Port follows once Star6E is verified.

## Timeouts, before / after

| Where | Current | Proposed |
|---|---|---|
| `src/star6e_runtime.c` `alarm()` shutdown | 5 s | 2 s |
| watchdog poll loop | 8 × 1 s | 6 × 500 ms |
| watchdog post-`SIGKILL` grace | 3 s | 1 s |
| `star6e_pipeline_wait_isp_channel` cap | 2000 ms | 500 ms |
| `drain_venc_channel` (×2) | 500 ms each | 150 ms each |
| ISP `dlopen` fallback (×2) | 100 ms | 20 ms |
| reinit-coalesce debounce | 200 ms | removed |

Net: ~0.8–1.5 s shaved off a successful cycle, plus faster hang
detection.

## Validation matrix

Run after `make verify` passes both backends.

1. `scripts/star6e_direct_deploy.sh cycle` on `root@192.168.1.13` —
   baseline boot.
2. **Same-mode SIGHUP cycle.** For each of imx335 modes 0/1/2/3, set
   in `/etc/venc.json` via `json_cli`, send 5 SIGHUPs at 2 s intervals.
   Pass = stream alive after each, dmesg clean.
3. **Cross-mode SIGHUP cycle (the gate).** Rotate 0 → 1 → 2 → 3 → 0
   via `json_cli` + SIGHUP, no process restart between hops. 3 full
   rotations. Pass = stream alive after each hop, no D-state, no
   `device_unresponsive` from `remote_test.sh --json-summary`.
4. **`/api/v1/set?sensor.mode=N`.** Same matrix as step 3 via the API
   path instead of SIGHUP.
5. **Cycle time.** Time SIGHUP → first frame on output. Target ≤ 2 s.

If step 3 or 4 hangs or D-states the bench:

- Log it in `documentation/CRASH_LOG.md` with exact mode transition,
  dmesg, and `remote_test.sh` JSON summary.
- Revert the PR. Keep today's partial reinit. Update
  `documentation/SIGHUP_REINIT.md` with a "tried, didn't fly on this
  BSP" note so future readers don't re-attempt without new evidence.

## Risk

The single load-bearing assumption is that the post-0.3.2 fixes
(drain ordering, watchdog, `pre_init_teardown`, SCL preset) have made
`MI_SNR_Disable` survivable. If wrong, validation step 3 hangs the
bench and the work is reverted. No partial-success fallback path is
left in code.

## Out of scope

- Maruko backend (port follows after Star6E validates).
- Any new HTTP API surface — `/api/v1/restart` and `/api/v1/set` keep
  their existing URLs and semantics.
- Codec / stream-mode change paths beyond what already routes through
  reinit.
