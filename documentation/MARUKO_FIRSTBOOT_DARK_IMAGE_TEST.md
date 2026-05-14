# Maruko firstboot dark-image investigation

## Symptom

On a fresh Infinity6C (Maruko, SSC378QE bench at `192.168.2.12`) firstboot
after deploying the v0.10.7 release tarball + uClibc symlinks
(`scripts/maruko_direct_deploy.sh full`), starting `venc` produces a very
dark image — washed out, almost unviewable.

If `majestic` is started first (from the same stock OpenIPC build), and
then stopped and replaced by `venc`, the image is normal.

## Hypothesis

`majestic` performs some sensor-side initialization that `venc` does not.
The most likely candidates are direct I2C writes to the IMX415 register
file that the MI vendor SDK does not perform automatically when `venc`
goes through its `MI_SNR_SetPlaneMode` / `MI_SNR_SetRes` / `MI_SNR_SetFps`
path — for example analog gain trim, black-level / clip / sync setup,
or a sensor power-state register that the SDK leaves in standby.

Once majestic has written those registers, the values survive across
process exit (the sensor IC keeps state until the I2C bus is reset or
power is cycled) — so a subsequent `venc` run sees the already-initialised
sensor and produces a normal image.

## Test plan

Five scenarios.  E captures the live state without rebooting (use it to lock
in a known-good "restored" stream before disturbing it).  A/B/C/D each start
from a fresh `reboot`:

| ID | Scenario             | Action                                                          | Expected image |
|----|----------------------|-----------------------------------------------------------------|----------------|
| E  | current live         | sample whatever is running; no reboot                           | (whatever it is) |
| A  | firstboot            | reboot, no streamer started before sampling                     | (no stream)    |
| B  | venc cold            | reboot, start `venc`, settle 6s, sample                         | **dark**       |
| C  | majestic cold        | reboot, start `majestic`, settle 6s, sample                     | normal         |
| D  | majestic → venc      | reboot, run `majestic` briefly, kill, start `venc`, settle, sample | normal      |

At each sample point we capture:

1. `regscan` dump of ~250 IMX335/IMX415 registers over I2C
   (`/dev/i2c-N` at addr `0x1a` by default).
2. For B/C/D/E: an HTTP snapshot when the running streamer exposes one.
   Majestic has `/snapshot.jpg`; **venc currently has no JPEG endpoint**,
   so venc-side scenarios record `endpoint_unavailable` for brightness and
   the dark/bright verdict must be eyeballed from the live RTP stream
   (`udp://<ground>:5600`) or inferred from the register diff alone.
3. When a JPEG is available, a mean-luminance number computed via
   ImageMagick `identify -format %[mean]` or ffmpeg `signalstats`, so the
   automated report carries a numeric dark/bright verdict for majestic-side
   scenarios.

## What the diffs answer

| File                 | Question it answers                                                  |
|----------------------|----------------------------------------------------------------------|
| `diff_B_vs_A.txt`    | What does **venc** write to the sensor from cold?                    |
| `diff_C_vs_A.txt`    | What does **majestic** write to the sensor from cold?                |
| `diff_C_vs_B.txt`    | **What does majestic write that venc does not? — the smoking gun.**  |
| `diff_D_vs_B.txt`    | What sticks around when venc inherits the majestic-initialized sensor? |

`diff_C_vs_B.txt` is the headline result.  Every register on that list is
a candidate fix — the minimum set we need venc to write itself for the
firstboot image to come up correctly without majestic.

## How to run

```sh
# Build the i2c register dumper (vendored from tipoman9/star6c_sensor)
make regscan SOC_BUILD=maruko

# Ensure the box has venc, majestic, libs, sensor.ko, ISP bin, json_cli, etc.
# (one-time per fresh device — only needs --with-libs/-drivers/-isp-bins/-json-cli):
scripts/maruko_direct_deploy.sh full

# Capture the *current* live state — no reboots, no scenario teardown.
# Use this to lock in a known-good "restored" snapshot before kicking off
# the full sweep that will reboot the bench four times:
scripts/maruko_sensor_init_diff.sh --current-only

# Run the full four-scenario sweep (A/B/C/D) — auto-probes i2c-0/1/2,
# takes ~8 min, reboots the bench between each scenario:
scripts/maruko_sensor_init_diff.sh

# Or override anything:
scripts/maruko_sensor_init_diff.sh \
    --host root@192.168.2.12 \
    --i2c-dev /dev/i2c-0 \
    --i2c-addr 0x1a \
    --settle 6 \
    --maj-brief 4 \
    --out /tmp/sensor-init-$(date +%s)
```

When the script finishes, open `<out>/report.md`.  The top-line answer is
in the "Registers majestic touches that venc does not" table.

Scenario E results are most useful when compared against B/D from a later
full sweep — they should agree (same sensor state under "restored venc").
If E and D diverge, the sensor IC has drifted somehow between runs (e.g.
because a reboot lost state that majestic had injected).

## Test-bed assumptions

- Target is at `root@192.168.2.12` and reachable via SSH key
  (`BatchMode=yes` with a 10s connect timeout, per repo convention).
- Sensor is IMX415 at I2C addr `0x1A` (default for the SSC378QE bench).
  For IMX335, override with `--i2c-addr` if the alternate address is
  used on the board.
- I2C bus is one of `/dev/i2c-0..2` and the sensor is readable as long
  as the sensor driver kmod is loaded (it is, after firstboot).
- Stock OpenIPC firmware on the device ships `/usr/bin/majestic` and the
  matching `/etc/majestic.yaml` + `/etc/sensors/<sensor>.bin`.
- The test does not modify `/etc/venc.json` or `/etc/majestic.yaml` — it
  uses whatever sensor config is already present, so make sure both
  configs target the same sensor before running.

## What to do with the result

1. **Identify the functional block.**  Map the addresses in
   `diff_C_vs_B.txt` against the IMX415 datasheet (or against the
   register-name tables in
   [`drv_ms_cus_imx415_MIPI.c`](https://github.com/tipoman9/star6c_sensor/blob/main/src/drv_ms_cus_imx415_MIPI.c)
   for sensor-driver context).  Likely buckets:
   - Analog gain / digital gain trim — explains a brightness offset.
   - Black-level / clip — explains crushed shadows / dark midtones.
   - Exposure-related registers if the SDK's `MI_SNR_SetFps` /
     `SetExposureLimit` path didn't land on the sensor IC.
   - Sync / standby (`0x3000`, `0x3002`) — should both be clear after
     SetRes; if they're not the same in B vs C, the SDK left the
     sensor in standby.
2. **Decide where the fix lives.**  Options, in increasing scope:
   a. Add a one-time set of register writes in `src/sensor_select.c`
      after `MI_SNR_SetRes` but before pipeline build.
   b. Add a "sensor warmup" helper that drives I2C directly during
      first start.
   c. File an upstream fix in the sensor `.ko` (the
      `sensor_imx415_maruko.ko` we build from
      `drivers/sensor_imx415_maruko.c`) — most durable, but requires
      a kernel module rebuild + reboot per change.
3. **Re-run the test plan** after the fix — scenario B should now look
   like scenario C/D in both brightness and the diff tables.
4. **Add an automated regression.**  Once the fix lands, the same script
   can run on every release as a smoke test: any future regression that
   undoes the sensor init will show up as scenario B getting darker
   again.

## Repo additions for this investigation

| Path                                       | Purpose                                                                    |
|--------------------------------------------|----------------------------------------------------------------------------|
| `tools/regscan/regscan.c`                  | Vendored sensor register dumper                                            |
| `tools/regscan/NOTICE`                     | Upstream attribution                                                       |
| `Makefile` (`regscan` target)              | Cross-compile via `make regscan SOC_BUILD=maruko`                          |
| `scripts/maruko_sensor_init_diff.sh`       | Four-scenario test orchestrator (this plan in script form)                 |
| `documentation/MARUKO_FIRSTBOOT_DARK_IMAGE_TEST.md` | This document                                                     |
