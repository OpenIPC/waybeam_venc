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
- Linear 1920x1080 modes at 30/60 fps RAW12 and 90/100 fps RAW10 are accepted.
- H.265 CBR, GOP, and I/P QP delta come from the existing `VencConfig` fields.
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
loader before starting the daemon and unloads the MPP stack after graceful
termination. Only one service may own the graph; disable the standalone
`S95cv610-streamer` service when enabling `S95waybeam`.

`CV610_SENSOR_PROFILE` picks the sensor clock the modules load with. It is
now only a fallback: the daemon sets the clock for the selected mode during
bring-up (see "Sensor clock" below), so the profile matters only against a
sys_config too old to expose `sns0_clk_hz`.

Audio is opt-in in both layers: set `CV610_AUDIO=1` in
`/etc/waybeam-cv610.conf` so the loader provides `open_aio`, `open_ai`,
`open_aenc`, and `open_acodec`, then set `audio.enabled=true`, 48000 Hz, mono,
Opus, and a non-negative `outgoing.audioPort` in `/etc/waybeam.json`. The
daemon refuses other CV610 audio formats. The init script tracks the daemon
across API-driven process respawns and will not unload modules while any
Waybeam process still owns the graph. `audio.enabled` and `audio.mute` are
restart-required HTTP controls; the rest of the audio group is hardcoded and
advertised unsupported. Enabling audio from a video-only boot still needs
kernel modules that a daemon respawn cannot load, so the daemon warns and
runs without audio rather than failing to start — `/api/v1/audio/status`
reports the running state, not the config flag.
Audio remains a separate RTP/UDP side channel: UDP video uses the configured
remote host, while local `unix://` or `frame-shm://` video sends audio to the
co-located Waybeam Link listener on `127.0.0.1:<audioPort>`.

## Sensor clock

The IMX662 runs 30/60/90 fps on a 37.125 MHz input clock and 100 fps on
27 MHz — the latter feeds 27 MHz while the sensor selects its 24 MHz INCK
profile (register `0x3014 = 0x04`). The two halves must agree: the sensor
keeps its programmed line timing regardless, so a mismatch is silent and
scales the rate by the clock ratio (a 60 fps mode on the 27 MHz clock
delivers 43.6 fps, measured, while every status surface still reports 60).

The clock is one CRG register that only the kernel can write, and the vendor
MIPI ioctls gate it without setting a rate. `mipi_setup()` therefore writes
the frequency for the selected mode to
`/sys/module/open_sys_config/parameters/sns0_clk_hz` during bring-up — after
`ENABLE_SENSOR_CLOCK`, which rewrites the same register, and before
`UNRESET_SENSOR`. The parameter comes from
`0002-hi3516cv6xx-runtime-sensor-clock.patch` in the OpenIPC firmware tree;
against an older module the daemon warns, names the clock the mode needs,
and continues on the boot-time clock.

## Deferred phases

1. Device smoke-test frame-SHM, repeated restart cycles, and a bounded soak.
2. Add live output redirection and encoder output-FPS control.
3. Add frame-SHM pressure metrics/throttling and sidecar parity.
4. Port advanced features selectively: IQ controls and recording, and only
   then consider dual VENC or stabilization. Add live audio gain/mute only
   after the analog control path can be operator-verified.

Unsupported SigmaStar-only fields remain in the common configuration schema
but are marked unsupported in `/api/v1/capabilities` and rejected before
mutation. This keeps the shared dashboard honest while the CV610 control
surface grows feature by feature.

The pull-request CI cross-builds this experimental backend, but the production
release workflow remains limited to Star6E and Maruko until the integrated
binary passes the phase-1 device gate above.

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
deltas were 480, exactly 10 ms. No microphone was connected, so acoustic
content is explicitly not operator-verified. A later service-stop hang was
localized to stale pidfile handling after an API respawn, not audio teardown;
the init-script fix requires a power-cycle and device retest. See
`documentation/CRASH_LOG.md`. Frame-SHM and long-duration soak verification
remain pending.
