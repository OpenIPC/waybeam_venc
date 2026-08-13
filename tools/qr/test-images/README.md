# Phone camera fixtures

`bounded-P23456789ABCDEFG.svg` is the vector master and
`bounded-P23456789ABCDEFG.png` is a binary-clean 1230×1230 phone-ready
rendering. `bounded-P23456789ABCDEFG.pgm` is a compact 246×246 decoder
regression fixture. They use the exact Version-1/Q matrix wrapped in the
required 33×33 Waybeam outer-frame profile. Their decoded payload is:

```text
P23456789ABCDEFG
```

## phone.html

Open `phone.html` on the phone. It is a self-contained page — no network, no
assets beside it — that **generates markers itself**: type any valid payload and
it renders live, or hit **Random** for a fresh one. Display presets cover large,
medium, small, tiny and 35-degree, plus an **Invert** toggle for the
light-on-dark case the decoder's inversion pass handles.

The encoder is a Version-1/Q alphanumeric QR wrapped in the 33x33 outer frame,
written out inline. It is not decoration: `make qr-test-phone` extracts it from
between the `WAYBEAM-QR-ENCODER-BEGIN/END` sentinels and checks every generated
marker both against `generate_qr.py` and by decoding it back through the same
cascade the craft runs, so the page cannot drift from the real decoder.

The marker stays crisp at every display size — it is drawn to a canvas at
integer module scale with smoothing off.

Suggested optical test order:

1. Large, phone square to the camera.
2. Medium, square to the camera.
3. Large, physically tilt the phone left/right and up/down. Physical tilt is
   preferred to a pre-warped image because it exercises the real camera
   projective transform.
4. Rotated 35 degrees, then repeat the physical tilt.
5. Small, followed by gradual defocus or increased distance.

Use `qr_watch.sh -v -i 2` during the test. Payloads remain on stdout; frame
discovery, decode outcome, applied pass, and timings appear on stderr.

Regenerate the three checked-in fixtures with the Python generator (the same
matrix `phone.html` produces, modulo data-mask choice — see below):

```bash
python3 -m pip install -r tools/qr/requirements-generator.txt
python3 tools/qr/generate_qr.py P23456789ABCDEFG tools/qr/test-images/bounded-P23456789ABCDEFG.svg --scale 30
python3 tools/qr/generate_qr.py P23456789ABCDEFG tools/qr/test-images/bounded-P23456789ABCDEFG.png --scale 30
python3 tools/qr/generate_qr.py P23456789ABCDEFG tools/qr/test-images/bounded-P23456789ABCDEFG.pgm --scale 6
```

`phone.html` and `generate_qr.py` agree on the frame wrapper and the symbol
data, but may pick a different **data mask** — python-qrcode scores the eight
masks slightly differently from ISO 18004. All eight are valid and decodable;
`make qr-test-phone` asserts byte-identity only when the two happen to agree,
and always asserts the marker decodes.
