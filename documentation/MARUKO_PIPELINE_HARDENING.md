# Maruko pipeline hardening — audit & plan

Purpose: evaluate whether the "majestic teardown lessons" (from
`MAJESTIC_MI_IOCTL_MAP.md`) are worth adopting into the Maruko (I6C) pipeline to reduce the
known teardown / reinit / zombie / wedge class. **Method: audit the current code first, then
propose only changes tied to a concrete problem the current code does not already solve.**

**Headline result: the current Maruko pipeline already implements — or deliberately supersedes —
essentially every majestic hardening lesson.** The teardown is *more* defensive than majestic's,
and the reinit model was intentionally chosen to eliminate the zombie class. The genuine residual
items are minor cleanups (dead code, stale comments), not zombie fixes. The most valuable output
of this audit is therefore what to **not** do — several majestic-parity items would *regress* the
current design.

---

## 1. Current teardown / reinit lifecycle (the map)

All line refs are `src/maruko_pipeline.c` unless noted.

### 1a. Reinit model — always respawn (fork+exec), never in-process reconfigure
`maruko_runtime.c:118-156` (`maruko_runner_run`): on any reinit request the run loop exits and
calls `venc_respawn_request()` — Maruko **forks+execs a fresh process** for every MUT_RESTART.
This was a deliberate, evidence-based decision (comment `maruko_runtime.c:132-151`): in-process
reinit page-faulted inside `MI_SYS_IMPL_FlushInputPortTasks` during teardown of *any* MUT_RESTART
transition and zombied the process (S1 bench 2026-05-15). Trading ~2 s respawn latency for
elimination of the zombie regime. **Consequence: every bind runs in a virgin process with no
prior MI_SYS state.**

### 1b. Teardown is guaranteed on *every* exit path, including init failure
`backend.c:6-19` (`backend_run_pipeline`): `init()` failure → `teardown()` then return;
`run()` always → `teardown()`. So `maruko_runner_teardown → maruko_pipeline_teardown` runs even
when `maruko_pipeline_configure_graph` fails partway (`maruko_runtime.c:97-99`).

### 1c. `maruko_pipeline_teardown_graph` — ordered, drained, flag-guarded (`4016-4176`)
A documented **8-step sequence** (`4095-4106`) enforcing the invariant *each consumer is stopped
before its input port is unbound* — added precisely because the naive "unbind-all-first" order
page-faulted in `MI_SYS_IMPL_FlushInputPortTasks` and zombied ~14 % of reinits (`4085-4093`).
Steps: drain SCL→VENC RING while VENC still consumes → `StopRecvPic` → UnBind VPE→VENC → destroy
VENC → stop VPE channels → UnBind ISP→VPE → stop VIF → UnBind VIF→ISP → sensor disable. Every
UnBind is gated on a `ctx->bound_*` flag (`4127/4152/4167`), every stop on a `*_started` flag.

### 1d. Drain-before-unbind — a refinement majestic does *not* have
`maruko_wait_output_idle` (`995-1041`) polls the mi_sys proc node for in-flight tasks and only
then enters the kernel's **unbounded uninterruptible** `MI_SYS_IMPL_FlushRealTimeOutputBuf`
(`987-994`). Called before the RING/REALTIME unbinds (`4118/4120/4157`) and inside
`maruko_stop_vpe_channels` (`1064`). This is the fix for the exact D-state hang the majestic map
flagged as an open bug — it is already closed here.

### 1e. Bind path sets the teardown flags as it goes (`bind_maruko_pipeline`, 2242-2310)
Each successful `MI_SYS_BindChnPort2` sets its `bound_*` flag (`2277/2286/2295`), so a partial
bind failure (`return -1`) leaves exactly the state `teardown_graph` expects and unwinds.

---

## 2. Majestic "lessons" vs. reality

Verdict for each item from the `MAJESTIC_MI_IOCTL_MAP.md` adoption checklist:

| Majestic lesson | Verdict | Evidence |
|---|---|---|
| Ordered teardown (Disable→Stop→Destroy per module) | **Already done, and better** | 8-step documented sequence `4095-4176`; adds drain steps majestic lacks |
| `UnBindChnPort` all binds on teardown | **Already done** | `4128/4158/4168`, each flag-guarded; symbol required at `maruko_mi.c:81` |
| Defensive UnBind-*before*-Bind in setup | **Not needed** | Respawn model (§1a): bind always runs in a fresh process, nothing is ever pre-bound. Adds a syscall for a state that cannot occur. |
| Live runtime reconfigure (Disable→SetParam→Enable, no teardown) | **Deliberately rejected** | In-process reconfigure is the exact thing that zombied the SoC (`maruko_runtime.c:132-151`). Adopting it would **regress**. |
| Partial-init unwind | **Already guaranteed** | `backend.c:10-14` calls teardown on init failure; `teardown_graph` is flag-guarded (§1b/1e) |
| Drain in-flight before flush/unbind | **Already exceeds majestic** | `maruko_wait_output_idle` (§1d) — majestic does not drain at all |

Net: of six checklist items, four are already implemented (two of them more thoroughly than
majestic), one is unnecessary under the respawn model, and one would actively regress the design.

---

## 3. Genuine residual findings

Each is tied to a concrete problem; none is a zombie/wedge fix (there is no residual zombie gap).

| # | Finding | Location | Problem it causes | Severity |
|---|---|---|---|---|
| F1 | `maruko_stop_vpe()` is **dead code** — zero callers (`teardown_graph` calls `maruko_stop_vpe_channels` directly at `4148`; ISP/SCL devices are reclaimed by `MI_SYS_Exit` on full teardown) | `1076-1089` | Dead ~14 LOC that *looks* like the shutdown device-destroy path; a future edit may wire it in and double-destroy | Low (maintenance) |
| F2 | `ctx->bound_vif_vpe` flags the **VIF→ISP** bind but is named `_vif_vpe` (VPE = SCL) | struct `maruko_pipeline.h:37`; set `2277`, unbind `4167-4169` | Latent edit-hazard: a future teardown change could pair the flag with the wrong port | Nit — **do not change**: the name is shared with `star6e_pipeline.h:45` where the topology differs; renaming Maruko-side breaks cross-backend parity for ~zero gain. Document only. |
| F3 | `maruko_stop_vpe_channels` header comments say "Used during reinit to avoid kernel mutex destruction" (`1046-1049`), but Maruko no longer does in-process reinit (§1a) — the skip-`ISP DestroyChannel` rationale (`1062-1063`) now only applies to the full-teardown path | `1046-1063` | Stale comments describe a dead code path; misleads the next reader about why the ISP channel is not destroyed | Low (docs) |

---

## 4. Plan (cherry-picked — only F1 and F3)

Both are safe, self-contained, verifiable, and reuse the existing (robust) teardown. F2 is a
documented nit, not a change.

1. **Remove `maruko_stop_vpe()`** (`1076-1089`).
   - *Problem:* dead code that shadows the real teardown path (F1).
   - *Fix:* delete the function. Confirm no reference (`grep -rn 'maruko_stop_vpe\b'` → only the
     definition today).
   - *Verify:* `make build SOC_BUILD=maruko` clean; on .12 run a start→SIGHUP-respawn→start cycle
     and a full SIGTERM shutdown, watch `/proc/mi_modules` for leaked ISP/SCL channels and confirm
     no MI_SYS zombie (`ps` state, load avg) — behaviour identical to before (it was never called).

2. **Refresh the stale reinit comments** in `maruko_stop_vpe_channels` / `maruko_stop_vpe` headers
   (`1046-1063`).
   - *Problem:* comments describe an in-process reinit path Maruko no longer takes (F3).
   - *Fix:* re-word to state the function runs on full teardown only, and that the ISP
     `DestroyChannel` skip guards the CUS3A-mutex crash on the shutdown path; note devices are
     reclaimed by `MI_SYS_Exit`.
   - *Verify:* docs-only; build clean.

3. **Correct the hardening section of `MAJESTIC_MI_IOCTL_MAP.md`.**
   - *Problem:* that doc's "Lessons for waybeam" / "Adoption checklist" was written from the
     experiment branch and asserts waybeam "never tears down cleanly in-process" and proposes items
     already implemented — now shown false by this audit.
   - *Fix:* replace the checklist with a pointer to this audit's §2 verdict table.
   - *Verify:* docs-only.

**Explicitly NOT in the plan (would regress or solve nothing):** defensive UnBind-before-Bind
(no pre-bound state exists under respawn); in-process live reconfigure (the zombie cause);
renaming `bound_vif_vpe` (breaks cross-backend parity).

---

## 5. Verification harness (for any future teardown change)

Reinit/teardown correctness is only observable on hardware. Standard loop on .12 (Maruko):
start → drive a MUT_RESTART (e.g. `sensor.mode` or `zoom_pct` change via `/api/v1/set` +
`/api/v1/restart`) → confirm fork+exec respawn → repeat N× → full SIGTERM. Pass = no MI_SYS
zombie (`ps` shows no `Z`/`D` waybeam, load avg returns to baseline), no leaked rows in
`/proc/mi_modules/mi_{isp,scl,vif}`, `dmesg` clean of page-fault / `FIFO-FULL`. SigmaStar hygiene:
SIGTERM only (never SIGKILL), `/tmp` is tmpfs, verify backups before deploy, `reboot -f` only on
a confirmed D-state wedge.
