# QR scanning backend

`qr_decode` is a freestanding consumer of waybeam's snapshot endpoint. It
reads a JPEG (or a binary P5 PGM), identifies and decodes a QR symbol, checks
the minimal Waybeam transport envelope, and prints the 16-character payload.
It does not interpret, authorize, persist, or execute that payload.

The waybeam process remains responsible only for the snapshot endpoint,
`GET /api/v1/snapshot.jpg` (the raw-PGM endpoint was retired in 0.60.0 — its
per-request scaler tap could wedge the SoC).
Pairing, commands, boot scheduling, and service integration belong to a later
standalone shell/action work package.

## Pieces

| File | Role |
|---|---|
| `qr_decode.c` | Freestanding JPEG/PGM decoder and scan-pass orchestration. |
| `waybeam_qr_format.c` | Minimal Version-1/Q transport-envelope validation. |
| `quirc/` | Vendored quirc library plus required outer-frame identification. |
| `generate_qr.py` | Creates valid bounded SVG, PNG, or PGM markers. |
| `qr_watch.sh` | Standalone polling helper; prints valid envelopes and performs no action. |
| `tests/test_qr_marker.c` | Deterministic host perspective corpus and envelope tests. |
| `tests/test_qr_cli.sh` | Decoder CLI, loader hardening, and fixture regression tests. |

## Minimal format

The default decoder accepts only the locked transport envelope:

```text
PXXXXXXXXXXXXXXX
```

- QR Version 1: 21 × 21 modules
- error correction Q
- QR alphanumeric mode
- exactly 16 uppercase QR-alphanumeric characters
- leading type `P` or `C`
- remaining 15 characters are opaque to the decoder

The example uses `P`; `CXXXXXXXXXXXXXXX` is the equivalent second transport
type.

The accepted alphabet is:

```text
0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:
```

`P` and `C` reserve transport types for future pairing/passphrase and
command/settings logic. Their meanings and the structure of the remaining
characters are intentionally not implemented here.

The outer frame is mandatory for discovery. A standards-compliant but unframed
QR is deliberately ignored, which avoids global finder-pattern scans and makes
the transport marker unambiguous. Use `--raw` for bench diagnostics when any
decodable payload inside that frame should be printed. Standard finder
localization is used only as refinement inside a tight ROI projected from an
accepted frame; the deterministic corpus also retains it as a comparison
baseline.

## Continuous outer-frame profile

Every accepted QR symbol must use this marker:

```text
33 × 33 marker units total
2-module continuous black outer frame
4-module white quiet zone inside the frame
21 × 21 Version-1 QR at marker coordinate (6, 6)
```

The full connected border supplies four stable projective corners. The
modified quirc path maps those corners directly into the existing QR grid; it
does not allocate a rectified image or add another camera pipeline stage.
Framed grids use bounded 3 × 3 majority sampling in the original source image.
Stock quirc behavior is unchanged unless a caller enables marker mode.

Render every module with integer scaling and no interpolation. The compressed
edge still needs real sensor resolution: use roughly 3 px/module as a lab
minimum and 4 px/module as a real-camera target.

`snapshot.jpg` is sized by the `snapshot.width`/`snapshot.height` config
(default: inherit the main stream), so those px/module budgets are spent
against the MJPEG channel's resolution — set the channel larger when markers
must decode from further away. JPEG q80 luma decodes markers reliably;
`qr_decode` extracts the luma plane directly and its own tiling passes cover
small codes anywhere in the frame.

## Generate a marker

The included generator locks the QR metadata and outer-frame geometry. It
accepts only the minimal 16-character `P`/`C` envelope and writes vector SVG,
two-color PNG, or decoder-ready PGM:

```bash
python3 -m pip install -r tools/qr/requirements-generator.txt
python3 tools/qr/generate_qr.py P23456789ABCDEFG waybeam-pair.svg
python3 tools/qr/generate_qr.py CRES1080P60A0030 command.png --scale 30
python3 tools/qr/generate_qr.py P23456789ABCDEFG bench.pgm --scale 6
```

`--scale` is the integer number of pixels per marker unit for raster output.
The generated image includes four white presentation units outside the 33×33
marker so the connected black border does not touch the image edge.

## Capture robustness

The scan sequence is ordered by measured cost and likelihood:

1. sharp full frame;
2. lightly denoised full frame;
3. sharp half-scale frame;
4. radial-corrected sharp and lightly denoised full frames;
5. lightly denoised half-scale frame;
6. sharp and lightly denoised overlapping tiles;
7. one inverted full-frame pass.

Each candidate is retried with quirc's mirror flip. No extra contrast or gamma
pass is applied because quirc's thresholding already handles those cases.
The expensive nine-tile sweeps stay late, while sharp half-scale remains ahead
of lens correction because it is a measured rescue path for small optical
captures.

## Bounded-frame execution

`qr_decode` uses this cascade for each full-frame/tile/scale attempt:

1. threshold the grayscale image once;
2. find a connected continuous outer frame and validate its quadrilateral;
3. when found, map the expected 21×21 QR directly and decode it immediately;
4. when direct decoding fails, derive a tight ROI from that frame and run
   finder-pattern refinement only inside the ROI;
5. when no frame is found, move directly to the next bounded image pass without
   running finder discovery.

Unbounded finder-pattern discovery is never run by the production decoder.
Clean framed inputs stay on the cheapest direct path; curved or distorted
framed inputs pay for local refinement only after the required transport
marker has been accepted. If the normal bounded chain fails, one nearest-
neighbour radial remap with `k1=-0.30` retries corrected full-frame sharp and
light-denoised images. The corrected images still use the mandatory-frame gate;
there is no global finder fallback.

## Decode diagnostics

`--stats` keeps stdout payload-only and writes structured diagnostics to
stderr. It reports every applied image pass and region, time spent preparing,
identifying the outer frame, and decoding, the number of frame candidates, and
whether processing stopped at frame discovery, QR error correction, envelope
validation, or success. The final summary includes total/load/transform times,
region, frame, refinement, finder, lens-correction, and QR candidate counts,
mirror retries, decoded QRs, and rejected envelopes. Times are monotonic
microseconds.

```bash
qr_decode --stats capture.pgm
qr_watch.sh -c -v
```

## Build and verify

```bash
make qr-decode SOC_BUILD=star6e
make qr-decode SOC_BUILD=maruko
make qr-test-host
make qr-test-cli
make qr-test-extended
```

The host corpus renders an actual Version-1/Q alphanumeric matrix into front,
rotated, mirrored, small, and projectively compressed quadrilaterals. It
reports stock and marker-assisted results and fails unless every framed case
decodes to the exact payload.

The extended series renders 768 additional 1280×720 symbols: four captured
marker widths (180–500 px), compressed-edge ratios from 1.00 down to 0.35,
three rotations, four perspective directions, and four deterministic defocus
levels. The defocus filter is a repeated separable 5-tap approximation, so the
series remains dependency-free and reproducible.

Current code-level result:

```text
outer frame identified: 768/768 (100.0%)
framed payload decoded: 709/768 (92.3%)
stock payload decoded:  506/768 (65.9%)
```

Framed decode remained 100% at edge ratios 1.00 and 0.70. It reached 77.1% at
ratio 0.35 across all sizes and blur levels, and 81.2% at the strongest
defocus level across all sizes and perspective ratios. The remaining misses
occur after successful frame identification and are concentrated where blur
and projection compress QR modules below the recoverable pixel budget.

The same 768-image series was run natively on the SSC338Q Star6E bench and
produced identical counts. Live optical validation with the phone fixture also
decoded `P23456789ABCDEFG` natively on Star6E through both
`sharp/tile/refine` and `sharp/half/refine`. The first five-frame bounded-
refinement burst decoded 2/5 captures; the remaining misses show that strong
fisheye curvature still limits frame acceptance and QR sampling at large
on-screen sizes. With the radial fallback enabled, a subsequent live
five-frame burst decoded 4/5: one through normal `blur/full/refine`, two
through `lens/full/refine`, and one through `lens-blur/full/refine`.

## Size and Star6E performance

The final standalone target is size-optimized with `-Os`, per-function/data
sections, and linker garbage collection. It retains the target's existing
Star6E NEON/VFPv4 flags and quirc's single-precision perspective math. The
stripped binaries are 30,288 bytes on Star6E and 30,136 bytes on Maruko,
instead of 63,052 bytes for the earlier Star6E `-O3`/loop-unrolled build.

The algorithmic reductions remain in place: outer-frame geometry is scored
once per symmetric candidate, expensive tiles stay late, radial correction
precomputes invariant column terms, repeated cell coordinates are cached, and
quirc retains high-water image/flood-fill allocations across scan sizes.
Changing compiler optimization does not alter samples, thresholds, candidates,
scan coverage, or the 768-case recognition counts.

There is a measured speed/flash tradeoff. On SSC338Q Star6E, the final `-Os`
binary decoded the saved hard 1280×720 fisheye capture through
`lens-blur/full/refine` in a 425 ms mean over 20 runs (404 ms minimum, 472 ms
maximum). The previous 63 KB speed-focused build averaged 257 ms on the same
capture. The smaller build still fits the watcher's 0.5-second minimum start
cadence for successful hard captures; a complete no-code cascade can take
roughly 0.55 seconds and therefore starts the next capture immediately on
completion.

## Standalone use

```bash
curl -s http://127.0.0.1/api/v1/snapshot.jpg | qr_decode
qr_decode --raw capture.jpg
qr_decode --stats capture.pgm   # PGM input still supported (bench corpora)
```

For polling without action dispatch:

```bash
qr_watch.sh
qr_watch.sh -c
qr_watch.sh -c -v
```

`snapshot.enabled` must be true. `qr_watch.sh` polls
`GET /api/v1/snapshot.jpg`; use `-e` to point it elsewhere.

The decode timings in the section above were measured on a 1280×720 capture;
cost scales with the pixel count the endpoint returns.

`qr_watch.sh` reports HTTP `503` when snapshots are disabled. It exits after
the first decode by default; `-c` streams decoded envelopes continuously.
The default stress cadence starts the next capture as soon as the preceding
capture and decode return, while keeping capture starts at least 0.5 seconds
apart. `-i SECONDS` can request a slower minimum cadence; values below 0.5 are
clamped.

Deployment, init scripts, packaging, and any consumer/action hook are deferred.
