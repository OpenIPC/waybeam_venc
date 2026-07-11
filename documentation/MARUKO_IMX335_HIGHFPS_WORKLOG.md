# Maruko / IMX335 high-fps driver-mode work — session worklog & next steps

Handoff notes from the 2026-07-05 investigation into full-res IMX335 high-fps on the
Infinity6C (I6C / Maruko, device .12). This is the **pick-up-another-day** record: what was
proven, what was ruled out, what still stands, and the concrete next moves. Companion to
`MAJESTIC_MI_IOCTL_MAP.md` (ioctl-level comparison) and
`MARUKO_IMX335_FRAMEBASE_49FPS.md` (the shippable FRAME_BASE result).

## Headline results

1. **The "~274 MPix/s I6C ISP bandwidth wall" is DEBUNKED** — it was a *sensor-driver
   register-timing artifact*, not an ISP hardware limit. Proven by swapping **only the driver
   `.ko`** (tipoman9's OpenIPC/builder **PR#83**, branch `ssc377qe_fpv`) under the *same*
   majestic binary / same `imx335.bin` / same exposure: full-res 2592×1944@59 runs clean at
   **298 MPix/s**, above the rate that stormed the vendor driver.
2. **There are TWO independent limiters.** (a) the sensor driver's per-mode readout timing, and
   (b) **waybeam's own pipeline** — on the *good* PR#83 driver, majestic sustains 59 fps clean
   while waybeam at the same mode caps at 29.6 fps + ISP P0 FIFO storm under REALTIME.
3. **The waybeam REALTIME gap is not reachable through any MI-SDK config** — config is provably
   byte-identical to majestic; every lever tested is negative (below). **FRAME_BASE @49 fps is
   the clean full-res win** and is the recommended path to ship.

## Driver sweep (majestic, exposure=6, SCL Ring delivered fps + DropCnt)

| Mode | vendor / maruko `.ko` | **PR#83 `.ko`** | MPix/s |
|---|---|---|---|
| 2592×1944@30 (native) | 30 ✓ | 30.0 ✓ | 151 |
| **2592×1944@59** (native) | — | **59.2 ✓ DropCnt=0, zero overflow** | **298** |
| 2560×1920@60 | 30 + FIFO storm ✗ | 55.2 (soft cap, no storm) | 271 |
| ~@90 | 3.3 + storm ✗✗ (vendor 2560×1440) | 89.5 ✓ (PR83 2208×1248) | 248 |
| 1920×1080@120 | 115 ✓ | 119.7 ✓ | 249 |

The ceiling is set by the driver's per-mode readout timing (HMAX/VMAX/MIPI line rate), **not**
the ISP. **dmesg gotcha:** `Fifo=0 Connect DevID=0 …` are benign VIF AFIFO connect
announcements (~5 constant), NOT overflow. Real overflow = `FIFO-FULL` (100+ during storms).
`grep -ic fifo` alone lies — always match `FIFO-FULL`.

## waybeam pipeline gap — elimination trail (all live-measured NEGATIVE)

On the PR#83 driver, waybeam 2592×1944@59 = 29.6 fps + ~122× `[0]ISP P0 FIFO FULL` under
REALTIME. None of these moved it:

| Lever | How tested | Result |
|---|---|---|
| Buffer depths | `WAYBEAM_DEPTH_*` values **and** removal via `WAYBEAM_SKIP_DEPTH` | no effect |
| 3A / AE | `WAYBEAM_NO_3A` (freeze CUS3A via SetRunMode OFF) | no effect |
| 3DNR | `fpv.noiseLevel=0` (`level3DNR=0`) | no effect |
| ISP output format | already 422 (=majestic) | already matched |
| ISP→SCL IFC compress | `WAYBEAM_ISP_COMPR=ifc` | no effect |
| Snapshot / 2nd SCL port | disabled | no effect |
| Bind link type | REALTIME vs FRAME_BASE (`WAYBEAM_VIF_LINK`) | REALTIME storms→29.6; FRAME_BASE clean→**49** (m2m cap) |
| Runtime ISP kick | `WAYBEAM_ISP_KICK=1` (Disable/EnableOutputPort), `=2` (Stop/StartChannel) | fired, but did NOT release half-rate |

**Hardware identical (devmem, read-only):** ISP core clk `0x1F207184`=mux 7=**384 MHz = HW
max** on both (clock hypothesis dead — waybeam already maxes it via sysfs poke,
`maruko_pipeline.c:2062`); DFS off; MIU ISP0 QoS arb `0x1F2CA400`=0x3210/0x7654/0xBA89/0xFEDC
identical; CSI/VIF clk identical.

**Config identical (LD_PRELOAD ioctl diff):** every VIF/ISP/SCL attr + bind + link type is
byte-identical — see `MAJESTIC_MI_IOCTL_MAP.md`. The only inventory difference is workload:
majestic issues ~2 `mi_sensor` calls total; **waybeam continuously polls the sensor** (nr8×8,
nr9×7, plus nr1/5/11/12/14 waybeam-only), and runs a full app (HTTP/mDNS/metrics/OSD/IMU/RTP)
alongside the encode.

## What still stands — the one unequalized difference

The ioctl capture was **bare majestic**; the waybeam runs a **full application**. The SDK FAQ
pins ISP P0 FIFO-FULL on *bandwidth starvation*. So the leading unresolved hypothesis for the
REALTIME half-rate is **DRAM/CPU bandwidth contention** from waybeam's ancillary threads +
continuous sensor poll stealing the margin the ISP needs at full res. This is the **only**
difference not yet equalized.

## Next steps (in priority order)

1. **Contention test (cheap, decisive).** Run waybeam stripped to bare encode — mDNS, OSD,
   metrics, IMU off; HTTP idle; suppress the continuous sensor poll — at 2592×1944@59 REALTIME
   on the PR#83 driver.
   - → ~59 fps ⇒ **contention confirmed**: fix by trimming/optimizing waybeam's thread
     bandwidth (esp. the `mi_sensor` poll cadence) — then REALTIME @59 is reachable.
   - → still 29.6 ⇒ the difference is **driver-internal / unreachable via the SDK**; lock in
     FRAME_BASE @49 as the full-res ship.
2. **Adopt PR#83's driver timing.** Either diff PR#83's `sensor_imx335_mipi.c` HMAX/VMAX/MIPI
   line-rate tables against `drivers/sensor_imx335_maruko.c` and port the mode timings, or ship
   the PR#83 `.ko` directly. This is what unlocks full-res high-fps at the *driver* layer
   (independent of the pipeline gap above).
3. **Ship the FRAME_BASE full-res mode** (per `MARUKO_IMX335_FRAMEBASE_49FPS.md`): add
   2592×1944@49 (or a safer @45) keyed to FRAME_BASE via the `_1485`-style suffix, positioned
   as a quality/record mode distinct from the low-latency REALTIME hero modes. No new pipeline
   code — pipeline already supports the path.
4. **Re-examine IMX415** (`maruko_imx415_1485_lock_isp_wall`): its "1485 ISP wall" may likewise
   be driver-timing rather than hardware; re-run the same driver-swap / FRAME_BASE analysis.
5. **Correct the record** once a mode ships: fix the now-wrong ISP-ceiling conclusion in
   `documentation/MARUKO_IMX335_MODES.md` and the mode-lineup rationale comments in
   `drivers/sensor_imx335_maruko.c` (the false ~274 MPix/s wall).
6. **Strip the bench env knobs** before productionizing: `WAYBEAM_VIF_LINK`, `WAYBEAM_DEPTH_*`,
   `WAYBEAM_SKIP_DEPTH`, `WAYBEAM_ISP_COMPR`, `WAYBEAM_ISP_KICK[_MS]` on branch
   `feature/imx335-2560x1920-framebase-experiment` are bench-only.

## Device state / restore (.12, as left this session)

- **PR#83 driver ACTIVE:** `drv_ms_cus_imx335_MIPI`, md5 `eeb6e816…`, 25520 B at
  `/lib/modules/5.10.61/sigmastar/sensor_imx335_mipi.ko`. `.ko` swap needs a reboot.
- **Backups:** vendor `.ko` → `/root/sensor_imx335_vendor.bak.pretest` (21756 B); maruko `.ko`
  → `/root/sensor_imx335_maruko_active.bak` (+ repo `drivers/sensor_imx335_maruko.ko`); vendor
  also on `/rom/…`. majestic.yaml orig → `/etc/majestic.yaml.waybeambak`. waybeam config →
  `/root/waybeam.json.bak.pr83test`; pre-framebase binary → `/root/waybeam.bak.pre-framebase`.
- **KSRC** (driver rebuilds) = `/home/snokvist/dev/Maruko/SourceCode/kernel/kernel` (5.10.61,
  vermagic `5.10.61 preempt mod_unload ARMv7 thumb2 p2v8`).
- **Hygiene:** SIGTERM only (waybeam zombies in the storm state → reboot between config tests);
  `/tmp` is tmpfs (push helpers after each boot); verify backups before deploy; devmem READS
  only — never write a clock/MIU register (can hang the SoC).

## Method / reproduce

`sh /tmp/pr83_sweep.sh` pattern: per mode, set majestic.yaml size/fps → restart majestic → read
`/proc/mi_modules/mi_scl/mi_scl0` Ring line last field + the DropCnt table. For the waybeam
gap, force the bind with `WAYBEAM_VIF_LINK=realtime|framebase` and read `/api/v1/fps/live`, the
`[verbose] fps` line in `/tmp/waybeam.log`, and `dmesg | grep -i "fifo full"`. The thread-safe,
pointer-following `ioctl()` shim used for the config diff is preserved in the investigation
scratchpad (see memory `maruko_imx335_majestic_isp_wall_crossvalidation`).
