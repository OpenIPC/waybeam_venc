# Majestic CUS3A Reverse Notes

Date: 2026-03-14
Target: Greg Star6E board (`root@192.168.8.156:2222`)
Binary examined: `/usr/bin/majestic`

## Scope

This note captures focused evidence about whether Majestic uses the SigmaStar
CUS3A path on Greg's IMX335 Star6E board, and what surrounding runtime policy
appears to matter for FPV operation.

Per `gregs_testing_rules.md`, any Majestic runtime experiment must be followed
by a reboot before trusting standalone `venc` results.

## Offline Reverse Findings

Method:

- fetched `/usr/bin/majestic` from the target without launching it
- inspected the local copy with `file`, `readelf`, `objdump`, and `strings`
- inspected target config and init scripts:
  - `/etc/majestic.yaml`
  - `/etc/init.d/S993video_settings`

### Binary shape

- `majestic` is a stripped 32-bit ARM PIE ELF binary
- dynamic dependencies shown by `readelf -d` are generic system libs only
- no direct `libmi_*` or `libcus3a.so` `NEEDED` entries were visible

Interpretation:

- Majestic likely resolves SigmaStar ISP/sensor symbols with `dlopen`/`dlsym`
  or through another runtime-loaded wrapper instead of direct ELF linkage

### Direct CUS3A evidence

The binary contains these relevant strings:

- `MI_ISP_CUS3A_Enable`
- `CUS3A_RegInterfaceEX Ch=%d, Adap=%d, Type=%d`
- `CUS3A_V1.1`
- `/dev/ispmid_cusapi`
- `[%s] AE = %d, AWB = %d, AF = %d`
- `DoAe`
- `_DoAeInit`
- `_MI_AI_DoAecAlgorithm`
- `_MI_AI_DoAed`
- `_MI_AI_DoAenc`
- `[sstar ae_init], FNum = %d, fps = %d`

Interpretation:

- Majestic is definitely using the SigmaStar CUS3A stack on this board
- it is not just toggling a plain internal ISP AE mode
- the presence of `CUS3A_RegInterfaceEX` strongly suggests Majestic uses a
  registered custom 3A interface, not only `MI_ISP_CUS3A_Enable`
- the `DoAe` and `ae_init` strings suggest a real AE algorithm path exists
  behind that registration

This last point is still an inference from strings, not proof of exact call
order.

### ISP / AE control evidence

The binary also contains strings for:

- `Load isp file %s`
- `Cannot set isp anti flicker`
- `Cannot get isp exposure limit`
- `Cannot set isp exposure limit`
- `/metrics/isp`
- `isp_again`
- `isp_dgain`
- `isp_exposure`
- `Set exposure: %dms, analog: %d`
- `pAE_SetExposure`

Interpretation:

- Majestic exposes or tracks live AE-related values internally
- it appears to have enough state to report exposure/gain metrics
- Majestic is likely doing more than one-shot ISP setup; it has a live AE path

### FPV-related config on Greg's image

`/etc/majestic.yaml` currently contains:

- `isp.sensorConfig: /etc/sensors/imx335_greg_fpvVII_colortrans.bin`
- `video0.fps: 90`
- `video0.size: 2400x1344`
- `isp.exposure: 10`
- `fpv.enabled: true`
- `fpv.noiseLevel: 0`
- `fpv.roiRect: 600x0x1200x1344`
- `fpv.roiQp: 0`

`/etc/init.d/S993video_settings` reinforces that policy at boot:

- sets sensor-specific `isp.sensorConfig`
- forces `.fpv.enabled true`
- sets `.fpv.noiseLevel`, `.fpv.roiRect`, `.fpv.roiQp`
- sets `.video0.fps`
- sets `.isp.exposure`
- reloads Majestic

Interpretation:

- `fpv.enabled` is a first-class runtime knob on Greg's OpenIPC image
- it is clearly part of the intended high-FPS operating mode
- but the script/config alone do not prove that `fpv.enabled` changes CUS3A
  cadence or AE sample throttling

### Immediate relevance to standalone `venc`

The offline evidence supports these conclusions:

1. A full custom CUS3A path is plausible on this board.
2. The current standalone refactor is very unlikely to match Majestic by only
   calling `MI_ISP_CUS3A_Enable`.
3. If we need to reproduce 90 fps AE behavior, the likely candidate mechanisms
   are:
   - pre-init CUS3A registration
   - explicit AE init/keepalive policy
   - FPV-mode-specific throttling or selective cadence

## Runtime Findings

### Run method

I ran Majestic directly on the target in a bounded way, captured
`/tmp/majestic-run.log`, queried its local HTTP API, then stopped it.

Because Greg's rules treat any Majestic run as polluting standalone `venc`
results, the board must be rebooted after this investigation.

### First startup collision

The first live run failed early because `venc` still owned port `80`:

- `HTTP server cannot bind to port 0.0.0.0:80`

Even in that failed run, Majestic still began SDK startup and reported:

- `Sensor index 2: 2400x1344@90fps`

After verification against standalone `venc --list-sensor-modes`, this is
much more likely a mislabeled mode index than a true sensor index:

- on Greg's board, `venc --list-sensor-modes --sensor-index 0` reports mode
  `[2]` as the 90 fps overscan path (`2400x1350@90fps`, sourced from
  `2560x1440`)

So Majestic's log line should be read as "mode 2 @ 90 fps on sensor pad 0",
not as evidence that the active sensor pad/index is `2`.

### Successful live startup

After stopping `venc`, Majestic started successfully enough to expose its API
and complete the core startup path.

Observed log sequence:

1. HTTP server starts on port `80`
2. SDK init starts
3. Majestic logs `Sensor index 2: 2400x1344@90fps`
4. verified interpretation: this almost certainly refers to mode `2` on
   sensor pad `0`, not sensor index `2`
5. CUS3A logs:
   - `AE = 1, AWB = 0, AF = 0`
   - `AE = 1, AWB = 1, AF = 0`
   - `AE = 1, AWB = 1, AF = 1`
6. VENC channel 0 created at `2400x1344@90fps`
7. later CUS3A log:
   - `AE = 0, AWB = 0, AF = 0`
8. FPV ROI applied:
   - `roi0: 576x0x1184x1344, qp: 0`
9. JPEG channel 2 created
10. ISP sensor bin loaded:
   - `/etc/sensors/imx335_greg_fpvVII_colortrans.bin`
11. image parameters applied:
   - `Set exposure: 10ms, analog: 29`
12. RTP streaming started to `udp://192.168.0.10:5600`
13. RTSP server started on port `554`

### What this means

This is the strongest runtime evidence collected so far.

- Majestic really does exercise `MI_ISP_CUS3A_Enable` on this board.
- The enable sequence is explicit and observable:
  - `100`
  - `110`
  - `111`
  - later `000`
- That sequence happens before the ISP sensor bin load completes.
- Majestic does not appear to keep CUS3A simply forced on forever from the
  first moment of startup.
- The later `000` suggests a handoff or mode transition after early startup.

This sequence is materially closer to the current refactored standalone path
than expected, but the ordering differs in an important way:

- Majestic creates VENC/JPEG channels before loading the final IMX335 sensor
  bin and applying image/exposure settings.
- Standalone `venc` currently loads the ISP bin much earlier.

That ordering difference is now a concrete candidate explanation for behavior
differences.

### Live API and metrics

Majestic's local API responded during the successful run:

- `GET /api/v1/config.json`
- `GET /metrics/isp`

Confirmed config values during live run:

- `video0.fps: 90`
- `video0.size: 2400x1344`
- `isp.sensorConfig: /etc/sensors/imx335_greg_fpvVII_colortrans.bin`
- `fpv.enabled: true`
- `outgoing.server: udp://192.168.0.10:5600`

Confirmed ISP metrics during live run:

- `isp_again 1695`
- `isp_dgain 1024`
- `isp_exposure 9`
- `isp_fps 90`

These values stayed stable across three polls taken about one second apart.
That is not enough to conclude AE is static or broken, because the scene was
not intentionally changed during the run. It does confirm that Majestic's
metrics endpoint exposes the exact kind of AE diagnostics we want in
standalone `venc`.

### Process mapping note

During the successful run, `/proc/<pid>/maps` showed the Majestic binary,
generic system libraries, and device mappings such as `/dev/mem`, but it did
not show `libcus3a.so` or `libmi_*` user-space libraries.

Interpretation:

- this absence should not be over-interpreted
- Majestic may be using a wrapper or load/unload path that is not visible in
  the short sample window
- the runtime log evidence for CUS3A use is much stronger than the maps view

## Practical Takeaways

For the standalone 90 fps AE reintroduction, the most important Majestic clues
now documented are:

1. Majestic definitely uses the CUS3A path on this board.
2. The observed startup sequence is `100 -> 110 -> 111`, then later `000`.
3. Majestic applies FPV ROI before final ISP bin load completes.

## Standalone AE Recovery

After the Majestic investigation, I verified the standalone Star6E AE
recovery on Greg's IMX335 target.

Baseline before the recovery:

- `/api/v1/ae` showed `state=normal`, `expo_mode=auto`
- but reported effectively inert values:
  - `long_us=33333`
  - `sensor_gain_x1024=1024`
  - `isp_gain_x1024=1024`
  - `luma_y=0`
- `avg_y=0`
- `stable=false`
- real encoder rate was still correct at about `89.5 fps`

Recovered behavior:

- startup primes CUS3A with `100 -> 110 -> 111`
- steady state no longer forces periodic `110`
- a delayed one-shot `000` handoff returns control to a live AE state

Verified runtime results:

- `90 fps`, IMX335 mode `2`
  - `/api/v1/ae` reports `stable=true`, `long_us=9999`,
    `sensor_gain_x1024=1673`, `luma_y=235..237`, `avg_y=246..247`
  - `/proc/mi_modules/mi_venc/mi_venc0` reports `SrcFrmrate 90/1`,
    `DstFrmrate 90/1`, measured about `89.5 fps`
- `120 fps`, IMX335 mode `3`
  - `/api/v1/ae` reports `stable=true`, `long_us=8330`,
    `sensor_gain_x1024=2706`, `fps=120`
  - `/proc/mi_modules/mi_venc/mi_venc0` reports `SrcFrmrate 120/1`,
    `DstFrmrate 120/1`, measured about `114 fps`
- `60 fps`, IMX335 mode `1`
  - `/api/v1/ae` reports `stable=true`, `long_us=9997`,
    `sensor_gain_x1024=1954`, `fps=60`
  - `/proc/mi_modules/mi_venc/mi_venc0` reports `SrcFrmrate 60/1`,
    `DstFrmrate 60/1`, measured about `60.1 fps`
- `30 fps`, IMX335 mode `0`
  - `/api/v1/ae` reports `stable=true`, `long_us=9995`,
    `sensor_gain_x1024=1885`, `fps=30`
  - `/proc/mi_modules/mi_venc/mi_venc0` reports `SrcFrmrate 30/1`,
    `DstFrmrate 30/1`, measured `30.0 fps`

Interpretation:

- the key missing behavior was the later `000` handoff, not just startup
  `111`
- the same startup-plus-handoff policy worked on all four tested IMX335
  modes on Greg's Star6E target
4. Majestic creates encoder channels before final IMX335 ISP bin load.
5. Majestic already exposes a compact live AE metric surface through
   `/metrics/isp`, which is a good model for standalone diagnostics.
