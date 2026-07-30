# Maruko IPU detection

Maruko uses SCL channel 0 port 3 as a raw NV12 detector tap. Ports 0, 1, and
2 remain owned by main VENC, JPEG snapshot, and stabilization respectively.
The host loads the same ABI-3 detector plugin and emits the same RTP sidecar
DETECT trailer as Star6E.

## Runtime rules

- The model must be compiled for I6C. I6E images are not portable.
- The configured `detect.netWidth` and `netHeight` must match the dimensions
  reported by the plugin. The standard Maruko model geometry is 800x448.
- `video0.framing` must be `off` and legacy zoom must be inactive. Port 3 has
  an independent crop; publishing boxes while port 0 moves would violate the
  encoded-frame coordinate contract.
- Failures are non-fatal. A missing plugin/model, ABI mismatch, or bad model
  leaves video running without DETECT.
- Teardown keeps the reader draining while port 3 is disabled, then joins and
  sweeps the FIFO before the plugin destroys the IPU channel.

## Performance review

Device: SSC378QE / Maruko I6C at `192.168.2.233`, 2688x1512@60 main encode,
800x448 one-class person e20 model, confidence 0.20. Measurements use the RTP
sidecar subscriber over 10-15 second windows.

| Mode | Video FRAME/s | Inference/s | Snapshot age median/p95 | waybeam CPU |
|---|---:|---:|---:|---:|
| Detect off | 59.7 | 0 | — | ~26% |
| Maruko interval 1 | 38.0 | 6.9 | not retained | ~42% |
| Maruko interval 2 (final 15 s) | 59.7 | 9.46 | 51/94 ms | ~23-24% |
| Existing Star6E interval 1 | 90 | 9-10 | 52/96 ms | not recorded |

The interval-1 result is a queue interaction, not a faster cadence: the
reader must retain the SCL buffer while the IPU DMA reads it. Asking for every
frame backs up SCL and slows both encode and inference. With interval 2, the
reader releases an alternate frame immediately, preserving the 60 fps video
path while the IPU remains compute-bound at roughly the same 800x448 rate as
Star6E. Repeated interval-2 windows ranged from 8.5 to 10.1 inference/s. For
that reason the Maruko default is 2; Star6E keeps its established default.

The Star6E reference is the 2026-07-23/25 SSC338Q bench recorded in
`waybeam-detect/training/README.md` and `training/NEXT.md`. Its main encode was
90 fps, so video FRAME/s is not a like-for-like SoC throughput comparison.
Inference rate and snapshot age are directly comparable because model input
geometry and detector contract are the same.

## Small-flash deployment

The tested camera has a 5.7 MB writable overlay and no SD block device. Store
the model once as:

```text
/root/models/yolov8n_448x800_ped_e20_i6c.img.xz
```

Configure `modelPath` as:

```text
/tmp/yolov8n_448x800_ped_e20_i6c.img
```

`S95waybeam` resolves that exact basename and inflates the archive before
starting the daemon. Failure to inflate is logged and video still starts.

## Device verification record

Confirmed on device, 2026-07-26:

- standalone model acceptance and a full IPU invoke/decode;
- integrated SCL port-3 inference with real person boxes;
- empty-scene `object_count=0` trailers;
- 800x448 same-geometry live model reload;
- live disable and re-enable with clean `shutdown IPU0`;
- reboot-persistent model staging and detector startup;
- concurrent 1920x1080 JPEG snapshot with detection remaining active;
- final 15-second sample: 59.73 FRAME/s, 9.46 inference/s, and 51/94 ms
  median/p95 snapshot age at `inferInterval=2`.
