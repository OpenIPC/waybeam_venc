# Phone camera fixtures

`bounded-P23456789ABCDEFG.svg` is the vector master and
`bounded-P23456789ABCDEFG.png` is a binary-clean 1230×1230 phone-ready
rendering. `bounded-P23456789ABCDEFG.pgm` is a compact 246×246 decoder
regression fixture. They use the exact Version-1/Q matrix wrapped in the
required 33×33 Waybeam outer-frame profile. Their decoded payload is:

```text
P23456789ABCDEFG
```

Open `phone.html` on the phone for large, medium, small, and 35-degree display
presets. The QR remains vector-sharp at every display resolution.

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

Regenerate all three with the checked-in generator:

```bash
python3 -m pip install -r tools/qr/requirements-generator.txt
python3 tools/qr/generate_qr.py P23456789ABCDEFG tools/qr/test-images/bounded-P23456789ABCDEFG.svg --scale 30
python3 tools/qr/generate_qr.py P23456789ABCDEFG tools/qr/test-images/bounded-P23456789ABCDEFG.png --scale 30
python3 tools/qr/generate_qr.py P23456789ABCDEFG tools/qr/test-images/bounded-P23456789ABCDEFG.pgm --scale 6
```
