# Investigation: Maruko gyro-fed hardware stabilization (MI_LDC warp engine + BMI270)

**Status: INVESTIGATION — not scheduled.** This is a research spec capturing
SDK findings and a de-risking plan. No implementation has started.

## Motivation

The shipped Maruko stabilization (`video0.framing = stab` / `stab-fill`,
v0.35.0–v0.37.0) measures motion **visually** (IVE NCC detector) and composes
**on the CPU** (`MI_SYS_BufBlitPa` for fill). Measured cost at 1080x720@50,
accuracy=low: detect 5.2 ms + compose 3.0 ms ≈ 5.8 ms/frame — **~29% of the
single Cortex-A7**. Known limitations, inherent to the approach:

- **2-DOF only** (x/y translation). No roll correction, no rolling-shutter
  correction.
- **Encode-domain sensing** (fill preset): at small encode sizes (e.g.
  640x360 from a 2952-wide sensor) sub-pixel shake quantizes to zero
  measurement — stabilization goes inert (operator-observed 2026-07-09).
- Visual measurement degrades in low light / low texture, and can false-lock
  on large moving scene content.

The i6c SoC has an **LDC hardware warp engine** with a gyro-fed DIS mode.
Combined with the BMI270 IMU support already live in waybeam_venc
(`imu.*` config block, `src/imu_bmi270.c`, wired into the Maruko backend),
this could replace both the visual measurement (gyro is better and ~free)
and the CPU compose (hardware warp, roll included).

Hardware note: the Maruko board has **no onboard gyro** — a BMI270 module
must be attached to an exposed I2C bus (same arrangement as the Star6E .13
bench). A BMI270 is available for this.

## SDK inventory (verified 2026-07-09, Maruko ILS00_TINY V1.4.0 SDK at `~/dev/Maruko`)

### LDC engine (`MI_LDC_*`)

- `include/mi_ldc_datatype.h`: `MI_LDC_WorkMode_e` = `LDC` (0x01, lens
  distortion), `LUT` (0x02, user displacement map), **`DIS_GYRO` (0x04)**.
- `MI_LDC_ChnAttr_t` / `MI_LDC_ChnParam_t` carry
  `bUseProjection3x3Matrix` + `as32Projection3x3Matrix[9]` +
  `u16FocalLength` + an opaque `pConfigAddr/u32ConfigSize` blob.
  **`MI_LDC_SetChnParam` exists as a runtime API** → per-frame 3x3
  projection updates from userspace are at least nominally supported.
- **Topology is inline attach, not a graph element**: demo
  (`ipc_demo/maruko/source/ldc/ldc.c`) calls
  `MI_LDC_AttachToChn(dev, chn, &ispOutPort)` onto an existing ISP output
  port. Our ISP→SCL graph shape would not change.
- Config blob is a **plain malloc'd userspace buffer** read from a file and
  freed right after `CreateChannel` — unlike the i6e path (see prior-art
  caveat below).
- Blobs shipped in the BSP (must be version-matched, same as the libmi_ive
  lesson): `libmi_ldc.so` + `mi_ldc.ko` (kernel 5.10) under
  `SourceCode/project/.../chip/i6c/ipc/common/{uclibc/9.1.0,glibc/11.1.0}/`.

### Gyro module (kernel + userspace)

- Kernel framework `drivers/sstar/gyro/` ("Support gyro for **disalgo**"):
  I2C or SPI transport, exposes `/dev/mi_gyro`; the in-kernel DIS algorithm
  consumes timestamped samples directly (gyro↔frame sync below userspace).
- **Supported chips: InvenSense ICG20660 and ICM40607 only.** No BMI270
  driver exists. Chip drivers are per-file ops modules
  (`gyro_sensor_icg20660.c`, `gyro_sensor_icm40607.c`) — a
  `gyro_sensor_bmi270.c` is writable (we know the register map from
  `src/imu_bmi270.c`).
- **`CONFIG_SS_GYRO` is NOT set** in the shipped Maruko kernel config. The
  framework is tristate → buildable as a loadable `.ko` from the SDK kernel
  tree (builder-side work, cf. builder#24).
- Userspace `libmi_gyro.so`: `MI_Gyro_InitDev`, `GyroSensor_ReadFifoData`,
  `GyroSensor_Read{Gyro,Accel}_XYZ`, range/rate setters.

### Prior art / caveats

- The exhaustive standalone-LDC failure in `eis-ldc-research.md` (error 513
  on every config, kernel NULL-deref crash) was **Star6E/i6e** — different
  chip, different SDK generation, different config-passing mechanism
  (`mi_sys_Vmap` of a phys pointer vs i6c's plain user buffer). It does NOT
  condemn i6c, but LDC is **unproven on-device** here.
- IVE precedent: i6c BSP blobs work when version-matched
  (`maruko_ive_hardware_blocked.md`) — encouraging for the LDC blob set.
- i6e per-frame VPE param pushes were choppy — per-frame
  `MI_LDC_SetChnParam` cost on i6c is a key unknown.

## Two candidate architectures

### A. "LDC as warp engine" (userspace gyro — cheaper, incremental)

Keep the **existing live BMI270 userspace IMU code** (frame-synced FIFO
drain, ring buffer). Each frame: integrate gyro → compute 2-DOF+roll
homography (reuse the framing Kalman) → `MI_LDC_SetChnParam` with the 3x3
matrix. LDC does the warp in hardware; no kernel driver, no calibration
blob, no DIS algorithm.

- Pros: smallest delta from what we have; measurement + smoothing stay in
  code we own; no kernel work.
- Cons: per-frame param-push cost unknown; gyro↔frame timestamp alignment
  is ours to get right (we already frame-sync the FIFO drain for Star6E EIS).

### B. Full `DIS_GYRO` mode (kernel gyro — the "proper" SigmaStar path)

Build `SS_GYRO` framework as `.ko`, write `gyro_sensor_bmi270.c`, load
`mi_gyro` + `mi_ldc`, run the LDC channel in `WORKMODE_DIS_GYRO` with a
SigmaStar calibration blob (focal length, gyro↔camera alignment, gyro
delay, readout time — generator tooling to be located in the SDK
Tools/docs archives).

- Pros: vendor-tuned DIS incl. likely rolling-shutter correction; zero
  per-frame userspace work.
- Cons: kernel driver work + builder packaging; opaque algorithm; blob
  tooling availability unknown; kernel driver owns the I2C device (venc's
  `imu.*` userspace path must yield the bus or share via `/dev/mi_gyro`).

## De-risking plan (phased, each gate cheap)

- **Phase 0 — LDC liveness probe** (~1 bench day): load version-matched
  `mi_ldc.ko` + `libmi_ldc.so` on .233, create a pass-through LDC channel
  (`WORKMODE_LDC`, identity), `AttachToChn` to the ISP output port, confirm
  frames still flow to VENC. Measure added latency + memory-bandwidth
  impact at 1080p@50. **Kills the i6e-513 ghost or the whole idea, cheaply.**
  Teardown discipline: apply the two-phase-stop lessons
  (`maruko_teardown_wedge_watchdog.md`); the reboot watchdog is our net.
- **Phase 1 — per-frame warp rate test**: static 3x3 first, then a
  sinusoidal test warp (à la `--eis-test`) via `MI_LDC_SetChnParam` at
  50 fps. Gate: no fps drop, no choppiness, visible smooth warp.
- **Phase 2 — live gyro loop (architecture A)**: BMI270 on the exposed
  I2C bus, wire `imu_bmi270.c` output → homography → SetChnParam.
  Compare side-by-side vs IVE `stab` (CPU, latency, small-encode behavior,
  roll). Gate: beats IVE stab on ≥2 of those with none worse.
- **Phase 3 — only if A proves out and vendor DIS is still wanted**:
  architecture B (kernel `gyro_sensor_bmi270.c`, calibration tooling hunt,
  `WORKMODE_DIS_GYRO`). May be skipped entirely if A is good enough.

## Open questions

1. Does i6c LDC accept configs / pass frames at all? (Phase 0)
2. Per-frame `SetChnParam` cost at 50–100 fps? (Phase 1)
3. Where does LDC attach relative to our SCL crop + stab-fill direct-VENC
   push — does it compose with `stab-fill`, replace it, or become a new
   `framing` preset (`stab-gyro`)?
4. FOV cost: hardware DIS presumably crops a margin (like `stab`); can the
   LUT/projection path do fill-style borders instead?
5. Which physical I2C bus/pins are exposed on the Maruko board for the
   BMI270, and at what bus speed (cf. `venc_imu_rate_i2c_bound.md`)?
6. Calibration blob generator availability in the SDK Tools archives
   (Phase 3 only).
7. Interaction with teardown/reinit hardening — LDC attach/detach ordering
   in `maruko_pipeline_teardown_graph`.

## Verified fact sources

- `~/dev/Maruko/SourceCode/project/project/release/include/mi_ldc.h`,
  `mi_ldc_datatype.h`
- `~/dev/Maruko/ipc_demo/ipc_demo/maruko/source/ldc/ldc.c`
- `~/dev/Maruko/SourceCode/kernel/kernel/drivers/sstar/gyro/` + kernel
  `.config` (`CONFIG_SS_GYRO` unset)
- `libmi_{ldc,gyro}.so` symbol dumps (`nm -D`), `mi_ldc.ko` strings
  (`E_MI_MODULE_ID_GYRO`, `dis-gyro`, `E_MI_LDC_TRACE_DISGYRO`)
- Coordination memory: `eis-ldc-research.md` (i6e prior art),
  `maruko_ive_hardware_blocked.md` (blob-matching precedent),
  `venc_imu_rate_i2c_bound.md` (BMI270 I2C cost model),
  `maruko_teardown_wedge_watchdog.md` (reinit discipline)
