# CV610 + IMX662 backend

Tracking: #220

The CV610 port follows the same staged model used for the Star6E-to-Maruko
bring-up. The first slice is deliberately narrow and device-oriented: preserve
the already verified HiSilicon graph, put it behind `BackendOps`, and connect
encoded H.265 frames to Waybeam's shared output contracts before porting the
large control surface.

## Current slice

- `SOC_BUILD=cv610` produces a 32-bit ARMv7 musl `waybeam` binary.
- The backend owns the proven IMX662 MIPI -> VI -> ISP -> VENC lifecycle.
- Linear 1920x1080 modes at 30/60 fps RAW12 and 90/100 fps RAW10 are accepted
  (see "Sensor modes" below — the table in `src/cv610_modes.c` is the only
  place they are written down).
- H.265 CBR, GOP, and I/P QP delta come from the existing `VencConfig` fields.
- Shared resilience presets drive row GDR and reference cadence with strict
  vendor readback; frame-SHM carries GDR/ENHANCE metadata and copied droppable
  NALs are marked TRAIL_N.
- `video0.sliceCount` enables whole-access-unit H.265 slicing. CV610 uses
  32-pixel LCU-row units and keeps early per-slice output disabled.
- UDP and abstract UNIX outputs use the shared HEVC RTP packetizer.
- `frame-shm://` publishes the existing VFRM v1 whole-frame contract.
- The shared HTTP/WebUI API advertises a CV610-specific capability mask and
  applies bitrate, GOP, I/P QP delta, RTP payload size, and IDR requests live.
- Debug OSD reuses the shared palette, font, primitive rasterizer, panel, and
  CPU sampler through a CV610 CLUT4 hardware-region adapter.
- Optional audio preserves the standalone streamer's verified inner-ACODEC ->
  AI -> vendor AENC/Opus graph (48 kHz mono, 10 ms, 32 kbit/s restricted low
  delay) and feeds its encoded access units into the shared RTP packetizer and
  output-socket helpers.
- The source-built `libsns_imx662.so` is included under
  `sensors/cv610/imx662/` and staged separately from the proprietary runtime
  libraries.
- SIGINT/SIGTERM unwind VENC, ISP, sensor callbacks, VI, SYS, and VB in order.

The graph uses VI-online mode, matching the standalone streamer's production
path. The module loader must provide a clean SYS/VB lifecycle; the backend
verifies the requested mode by reading it back before creating the VI pipe.

## Encoder capability delta

CV610 has a substantially broader vendor encoder surface than the current
backend exposes: nine declared RC families, multiple P/CRR GOP structures,
intra refresh, reference prediction, SVC, hierarchical QP, native frame-loss
and super-frame policies, ROI/QP maps, VUI/user data, and slice/low-delay
controls. SSC338Q currently has the broader device-proven Waybeam integration.

The evidence levels, live-device readback, exact comparison and deliberately
deferred controls are documented in
`documentation/SSC338Q_CV610_ENCODER_CAPABILITIES.md`. The resilience and
whole-access-unit slice port is tracked in
`documentation/CV610_RESILIENCE_SLICES_PLAN.md`.

The port is confirmed on device at 1080p30/60/100. Requests for 1, 3, 4, 6, 9,
12 and 17 slices delivered the requested VCL NAL census, reference cadences and
TRAIL_N counts matched, and an IDR-started 1080p60 capture decoded cleanly
under FFmpeg `-xerror`. Native SVC, extra RC/GOP modes and encoder-side loss
policies remain deliberately deferred.

## Build

The public OpenHisilicon headers and OpenIPC CV610 runtime library directory
must already exist locally. They are external inputs and are not copied into
this repository.

```sh
make build SOC_BUILD=cv610 \
  CV610_CC=/path/to/arm-openipc-linux-musleabi-gcc \
  CV610_SDK_INC=/path/to/openhisilicon \
  CV610_SDK_LIB=/path/to/cv610/rootfs/usr/lib

make sensor-cv610 SOC_BUILD=cv610 \
  CV610_CC=/path/to/arm-openipc-linux-musleabi-gcc \
  CV610_SDK_INC=/path/to/openhisilicon \
  CV610_SDK_LIB=/path/to/cv610/rootfs/usr/lib
```

Install `out/cv610/waybeam` as `/usr/bin/waybeam`, the plugin as
`/usr/lib/sensors/libsns_imx662.so`, and start with
`config/waybeam.default.cv610.json`. `make stage` also emits the tested CV610
init script and platform profile:

```sh
cp out/cv610/S95waybeam /etc/init.d/S95waybeam
cp out/cv610/load-cv610-online /usr/bin/load-cv610-online
cp out/cv610/waybeam-cv610.conf /etc/waybeam-cv610.conf
chmod +x /etc/init.d/S95waybeam /usr/bin/load-cv610-online
```

The init script invokes the staged `/usr/bin/load-cv610-online` module
loader before starting the daemon. It does **not** unload the MPP stack on
stop: the HiSilicon drivers take no module reference for an open fd, so an
`rmmod` under a live consumer (typically `waybeam_hub` holding `/dev/rgn`)
frees file operations still in use and wedges the SoC hard enough to need a
physical power cycle. Unloading is done only by an explicit
`load-cv610-online stop`, which refuses while any consumer holds an MPP
device. Only one service may own the graph; disable the standalone
`S95cv610-streamer` service when enabling `S95waybeam`.

Three operator rules follow from that:

- **Recovery after an abnormal exit is a plain `S95waybeam start`.** The
  modules are still loaded and the daemon re-initialises against them; there
  is no `reload` action and no module rollback on a failed start.
- **Changing `CV610_AUDIO` or `CV610_SENSOR_PROFILE` needs a reboot.** The
  loader refuses to re-load a differing module set over a live one.
- **A boot that has seen a hard kill cannot be unloaded cleanly.** Reboot
  rather than fighting it.

`CV610_SENSOR_PROFILE` picks the sensor clock the modules load with. It is
now only a fallback: the daemon sets the clock for the selected mode during
bring-up (see "Sensor clock" below), so the profile matters only against a
sys_config too old to expose `sns0_clk_hz`.

Audio is opt-in in both layers: set `CV610_AUDIO=1` in
`/etc/waybeam-cv610.conf` so the loader provides `open_aio`, `open_ai`,
`open_aenc`, and `open_acodec`, then set `audio.enabled=true`, 48000 Hz, mono,
Opus, and a non-negative `outgoing.audioPort` in `/etc/waybeam.json`. The
daemon refuses other CV610 audio formats. The init script tracks the daemon
across API-driven process respawns; module unloading is guarded separately in
`load-cv610-online`, which refuses while any process holds an MPP device. `audio.enabled` and `audio.mute` are
restart-required HTTP controls; the rest of the audio group is hardcoded and
advertised unsupported. Enabling audio from a video-only boot still needs
kernel modules that a daemon respawn cannot load, so the daemon warns and
runs without audio rather than failing to start — `/api/v1/audio/status`
reports the running state, not the config flag.
Audio remains a separate RTP/UDP side channel: UDP video uses the configured
remote host, while local `unix://` or `frame-shm://` video sends audio to the
co-located Waybeam Link listener on `127.0.0.1:<audioPort>`.

## Sensor modes

`video0.fps` selects a sensor mode; `video0.size` is the encoded size. They
are independent because VPSS sits between VI and VENC and scales: the sensor
always captures 1920x1080, and any smaller encoded geometry is produced by
the scaler. This is the same split SigmaStar gets from its VPE SCL ports.

VPSS is created even when no scaling is asked for, so the 1:1 path and a
scaled one exercise the same graph and the same teardown order.

Two attribute values are not optional, both established by probing the
hardware (`ss_mpi_vpss_set_chn_attr` returns `0xa0078007`,
`OT_ERR_ILLEGAL_PARAM`, otherwise):

- `chn_mode` must be `OT_VPSS_CHN_MODE_USER`. `AUTO` makes the channel follow
  the group's input size and rejects an explicit geometry — `USER` is what
  makes the channel a scaler.
- `frame_rate` must stay `-1/-1`. `0/0` is rejected with the same code.

Pixel format is *not* one of them: VPSS accepts both NV21 and NV12 here, so
the chain stays on NV21 to match what VI is willing to emit.

Output geometry is validated against the selected mode: no upscaling past the
capture size, and both dimensions required. Note the shared validator
additionally requires a multiple of 8 in each dimension, so 960x540 is
rejected on the height.

`src/cv610_modes.c` holds the whole table — capture geometry, frame rate,
MIPI RAW bit depth, and the input clock the mode's line timing assumes.
Everything derives from it: `cv610_validate_config()` rejects anything that
does not resolve, `cv610_prepare()` takes the bit depth and clock from the
resolved mode, and `GET /api/v1/modes` is generated from it, so what that
endpoint lists is exactly what `/api/v1/set` accepts. It mirrors
`g_imx662_mode_tbl` in `sensors/cv610/imx662/imx662_cmos.c`, which is the
sensor-side original; adding a mode means editing both. The geometry listed
by `/api/v1/modes` is what the sensor captures, not the encoded size.

A zero `video0.size` (`auto`) means the mode's own capture size. A half-set
one (`1920x0`) is rejected rather than completed — it is a typo, not a
request.

## Sensor clock

The IMX662 runs 30/60/90 fps on a 37.125 MHz input clock and 100 fps on
27 MHz — the latter feeds 27 MHz while the sensor selects its 24 MHz INCK
profile (register `0x3014 = 0x04`). The two halves must agree: the sensor
keeps its programmed line timing regardless, so a mismatch is silent and
scales the rate by the clock ratio (a 60 fps mode on the 27 MHz clock
delivers 43.6 fps, measured, while every status surface still reports 60).

The clock is one CRG register that only the kernel can write, and the vendor
MIPI ioctls gate it without setting a rate. `mipi_setup()` therefore writes
the frequency carried by the resolved mode to
`/sys/module/open_sys_config/parameters/sns0_clk_hz` during bring-up — after
`ENABLE_SENSOR_CLOCK`, which rewrites the same register, and before
`UNRESET_SENSOR`. The parameter comes from
`0002-hi3516cv6xx-runtime-sensor-clock.patch` in the OpenIPC firmware tree;
against an older module the daemon warns, names the clock the mode needs,
and continues on the boot-time clock.

## Deferred phases

1. Run a longer-duration soak beyond the bounded device matrices already
   completed.
2. Add live output redirection and encoder output-FPS control.
3. Add frame-SHM pressure metrics/throttling and sidecar parity.
4. Port recording selectively, and only then consider dual VENC or
   stabilization. Add live audio gain/mute only after the analog control path
   can be operator-verified.

Unsupported SigmaStar-only fields remain in the common configuration schema
but are marked unsupported in `/api/v1/capabilities` and rejected before
mutation. This keeps the shared dashboard honest while the CV610 control
surface grows feature by feature.

Pull-request verification cross-builds this backend. Release promotion remains
gated on the CV610 build plus the focused device matrix documented in
`documentation/CV610_RESILIENCE_SLICES_PLAN.md`.

## Hardware verification

The integrated daemon was deployed to the SIP-K662C6S CV610 target on
2026-08-13 and streamed 1920x1080 RAW10/H.265 at 100 fps to the x86 Waybeam
Hub decoder. An independent FFmpeg receive pass decoded 1,479 frames in 15
seconds (about 99 fps) at 1920x1080. Producer logs held 100 fps; the four UDP
drops observed during initial receiver synchronization did not increase over
the following 1,800 frames, and a subsequent init-service restart streamed
600 frames with zero drops. SIGTERM completed full ISP/VENC/SYS/VB teardown in
about 200 ms, and the installed service completed a cold module restart in
eight seconds.

The integrated audio path initialized on the same target, resolved the known
ACODEC power-up race after its expected 100 ms retry, and produced about 100
Opus access units/s with zero reported send drops. A host receiver checked 12
consecutive RTP v2/PT98 packets: sequence deltas were 1 and 48 kHz timestamp
deltas were 960, exactly 20 ms (`CV610_AUDIO_POINT_NUM` 960; the 480/10 ms
figure predates the Opus frame-size parity fix). No microphone was connected, so acoustic
content is explicitly not operator-verified.

On 2026-08-22 the resilience/slice branch was also confirmed through
frame-SHM at 1080p30/60/100. Slice requests 1, 3, 4, 6, 9, 12 and 17 produced
the requested VCL census; a 380-frame IDR-started 1080p60 capture decoded
cleanly with FFmpeg `-xerror`; and GDR/ENHANCE metadata, TRAIL_N rewriting and
one-second loss recovery passed. Only a longer-duration soak remains pending.
