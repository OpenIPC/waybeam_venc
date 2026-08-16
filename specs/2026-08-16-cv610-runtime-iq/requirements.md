# CV610 runtime IQ control surface

Status: **SPEC — not started.** Date: 2026-08-16.
Device: `root@192.168.2.181` (Hi3516CV610 DEMO Board, IMX662).
Follows PR #229, which landed the compile-time ISP seeds this would make editable.

## Why

PR #229 fixed the CV610 image by seeding the ISP properly at init: Sony's 131-entry
power-on register sequence, the ISP algorithm blocks (which were *all* disabled),
and the AWB saturation table (which was zeroed). The result is a large, verified
improvement — but every value is a **compile-time constant in the sensor plugin**.

Two consequences make that unsatisfactory:

1. **The operator cannot tune.** SigmaStar has an Image Quality tab driven by
   `/api/v1/iq/*`; CV610 has nothing. Changing saturation today means editing
   `imx662_cmos_param.h`, cross-compiling, deploying an `.so`, and **rebooting the
   board** — the plugin is read once at pipe start, so there is no live path at all.
2. **The tables are borrowed, not calibrated.** bayer_nr, sharpen,
   noise_calibration and DRC come from `smart_sc450ai`, a 4 MP sensor, where IMX662
   is 2 MP STARVIS. They were kept on an operator A/B, not a measurement. Runtime
   knobs are what turns "borrowed defaults" into "a starting point you can correct".

## Current state (measured, 2026-08-16)

```
GET /api/v1/iq/get → {"ok":false,"error":{"code":"not_implemented","message":"IQ query not available"}}
GET /api/v1/iq/set → {"ok":false,"error":{"code":"not_implemented","message":"IQ set not available"}}
```

The endpoints are generic and already routed (`src/venc_api.c:4114`). They dispatch
on backend callbacks and 501 when those are NULL:

- `src/venc_api.c:3131` → `g_cb->query_iq_info`
- `src/venc_api.c:3148,3169` → `g_cb->apply_iq_param`

Declared in `include/venc_api.h:43-45`:

```c
char *(*query_iq_info)(void);                                /* malloc'd JSON, caller frees */
int   (*apply_iq_param)(const char *param, const char *value);   /* 0 ok, -1 error */
```

Both are wired on the other two backends and NULL on CV610:

| backend | registration site | implementation |
|---|---|---|
| Star6E | `src/star6e_controls.c:1772-1773` | `src/star6e_iq.c` (~66 ISP setter calls, dlopens `libmi_isp.so`) |
| Maruko | `src/maruko_controls.c:1496-1497` | `src/maruko_iq.c` |
| **CV610** | `src/cv610_runtime.c:242` (`g_cv610_apply_callbacks`) | **absent** |

The WebUI hides the tab explicitly — `web/dashboard.html:1787-1788`:
```js
const iqTab = document.getElementById('tab-btn-iq');
if(iqTab) iqTab.hidden = true;   // /api/v1/iq|ae|awb are 501 here
```

## Reference implementation already on disk

`/home/snokvist/dev/hisilicon/venc/src/app/cv610_streamer/isp_control.c` — 30 KB,
covering the knob set with getters and setters:

- **setters**: `ss_mpi_isp_set_{ccm,color_tone,csc,nr,saturation,sharpen,wb}_attr`
- **getters**: `ss_mpi_isp_get_{ca,ccm,color_tone,csc,dehaze,drc,gamma,ldci,module_ctrl,nr,saturation,sharpen,wb}_attr`

A `cv610_isp_ctl` CLI built from it is already deployed on `.181`, so the calls are
proven against this silicon. This is a port with a working reference, not research.

**Simpler than Star6E.** `star6e_iq.c` has to `dlopen("libmi_isp.so")` and resolve
`MI_ISP_IQ_*` symbols at runtime. CV610 links `ss_mpi` directly (`cv610_pipeline.c`
already calls `ss_mpi_isp_set_ccm_attr`), so `cv610_iq.c` calls the MPI directly —
no dlopen, no symbol table, no init failure path.

## Scope

### Must

- `src/cv610_iq.c` + `include/cv610_iq.h` exposing `cv610_iq_query()` and
  `cv610_iq_set(param, value)`, matching the `star6e_iq.h` contract (dot-notation
  for sub-fields, comma-separated values for arrays).
- Wire `.query_iq_info` / `.apply_iq_param` into `g_cv610_apply_callbacks`
  (`src/cv610_runtime.c:242`).
- Cover, at minimum, the knobs that PR #229 seeded and the operator will want to
  correct: **saturation** (the one that fixed washed-out colour), **sharpen**,
  **nr**, **ccm**, **wb**, **color_tone**, **csc**.
- Un-hide the WebUI tab on CV610 by **capability**, not by backend name — the
  current `hidden = true` is hardcoded. Drive it off whether `/api/v1/iq/get`
  answers, so the same code stays correct if a backend gains or loses the surface.

### Should

- Read-back verification in `cv610_iq_query()` — return what the ISP reports, not
  what was last written, so a rejected set is visible.
- Reject unknown parameter names with `-1` so `/api/v1/iq/set` 400s rather than
  silently accepting.

### Out of scope

- **Persistence.** These are live-only knobs. Whether tuned values survive a reboot
  is a separate decision (config fields vs a profile file vs the PQ `.bin` route)
  and should not be bundled.
- **AE / AWB endpoints.** `/api/v1/ae` and `/api/v1/awb` are also 501 on CV610.
  Related, but a different callback set; do not widen this.
- **PQ `.bin` import.** `libbin.so` / `OT_PQ_BIN_ImportBinData()` is vendored and
  verified loadable on `.181`, but a `.bin` is chip- *and* sensor-locked and must be
  produced with PQTools against the IMX662. Separate workstream.
- **Re-tuning the sc450ai-derived tables.** This spec builds the instrument; using
  it to replace borrowed tuning with measured tuning is the follow-on.

## Verification plan

Per-step checks, each independently confirmable:

1. **Build gate.** `make build SOC_BUILD=cv610 CV610_SDK_INC=/home/snokvist/dev/hisilicon/oh CV610_SDK_LIB=/home/snokvist/dev/hisilicon/firmware/output-waybeam/target/usr/lib`, then `make verify` for Star6E/Maruko regression. The sensor plugin builds `-Wall -Wextra -Werror`.
2. **Endpoint liveness.** `/api/v1/iq/get` returns JSON instead of 501; `/api/v1/iq/set?saturation=<n>` returns ok.
3. **The knob actually moves the image.** Set saturation low and high, capture over
   RTP, and compare **mean chroma magnitude** — the metric that showed +41% for the
   compile-time change (20.19 → 28.46). A set that returns ok but does not move the
   image is the failure mode to look for.
4. **Read-back agrees.** `iq/get` after `iq/set` reports the value the ISP holds.
5. **No regression.** Four-mode cold-boot sweep still nominal: 30.03 / 60.03 /
   90.03 / 100.06 fps, drops frozen.
6. **Control.** A knob left untouched must read unchanged across the session — a
   table where everything moved cannot distinguish a working setter from a reinit.

## Bench traps that will bite (learned the hard way in the PR #229 session)

- **Never `S95waybeam stop` + `start` on CV610.** Two quick cycles hard-hung the
  SoC — no ARP, needed a power/watchdog reboot. `$LOADER stop` unloads the MPP `.ko`
  set and had not finished after 3 s. **Reboot to cycle the daemon.** ~15 reboots in
  that session were all clean.
- **`/api/v1/set?outgoing.server=…` triggers a reinit that stalls the HTTP server
  for seconds.** Rebooting within ~3 s loses the config write. Other fields persist
  to `/etc/waybeam.json` immediately. GET-verify before rebooting.
- **`sensor.mode` pins the mode** and overrides fps-derived selection; setting
  `video0.fps` alone does nothing while it is pinned.
- **`/tmp` on `.181` is tmpfs.** Bench tools vanish on reboot — re-scp
  `tools/cv610_i2c_dump.c`'s binary or a dump silently returns "not found", and an
  empty comparison reads as a pass.
- **The co-resident `waybeam_hub` rewrites venc's config.** Disable
  `/etc/init.d/S97waybeam-hub` for any A/B, or arms will not share settings.
- **Scene motion invalidates image A/B.** Two arms minutes apart showed a "noise
  fix" and a "blue cast fix" that were entirely the camera being repointed. Re-run
  adjacent arms on the same scene before concluding anything.
- **md5 the artifact, not the exit code.** Fixed in PR #229 for the sensor plugin
  (`-MMD -MP`), but the habit matters: two builds from different tuning tables came
  out byte-identical because the Makefile tracked no headers.
- **Objective sharpness metrics mislead on denoising changes.** Variance-of-Laplacian
  counts grain as detail. The operator's verdict on the live stream is the gate for
  image quality; metrics rank candidates and catch pipeline regressions.

## Open questions for the operator

1. Which knobs matter most day to day — is saturation/sharpen enough to start, or is
   CCM/WB needed in the first cut?
2. Should tuned values persist across reboot, and if so via config fields or a
   profile file? (Deliberately out of scope above; needs a decision before anyone
   asks why their tuning vanished.)
