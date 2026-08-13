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
- The source-built `libsns_imx662.so` is included under
  `sensors/cv610/imx662/` and staged separately from the proprietary runtime
  libraries.
- SIGINT/SIGTERM unwind VENC, ISP, sensor callbacks, VI, SYS, and VB in order.

The initial graph uses VI-offline mode, matching the standalone streamer's
safe default. VI-online requires the OpenIPC PM-module fix described in #220
and is intentionally not hidden inside the daemon.

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
cp out/cv610/waybeam-cv610.conf /etc/waybeam-cv610.conf
chmod +x /etc/init.d/S95waybeam
```

The init script invokes the existing `/usr/bin/load-cv610-online` module
loader before starting the daemon and unloads the MPP stack after graceful
termination. Set `CV610_SENSOR_PROFILE=sc4336p` for 100 fps or `imx662` for
30/60/90 fps. Only one service may own the graph; disable the standalone
`S95cv610-streamer` service when enabling `S95waybeam`.

The target module loader must provide the proven CV610 MPP stack and the
matching sensor clock. The 100 fps mode specifically needs the 27 MHz clock
profile; 30/60/90 fps use the IMX662 37.125 MHz profile.

## Deferred phases

1. Device smoke-test frame-SHM, repeated restart cycles, and a bounded soak.
2. Add the HTTP API with a CV610 capability mask and live bitrate, FPS, GOP,
   QP, IDR, and output callbacks.
3. Add frame-SHM pressure metrics/throttling and sidecar parity.
4. Make VI-online production-safe through the OpenIPC module layer.
5. Port advanced features selectively: audio, IQ controls, recording, OSD,
   and only then consider dual VENC or stabilization.

Until phase 2, the CV610 build intentionally does not link the HTTP/WebUI
routes. This avoids exposing SigmaStar-only endpoints or claiming controls
that have not yet been implemented on HiSilicon.

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
eight seconds. Frame-SHM and long-duration soak verification remain pending.
