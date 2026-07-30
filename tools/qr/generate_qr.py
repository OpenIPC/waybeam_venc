#!/usr/bin/env python3
"""Generate a bounded Waybeam Version-1/Q QR marker."""

from __future__ import annotations

import argparse
import html
import re
from pathlib import Path


PAYLOAD_RE = re.compile(r"[PC][0-9A-Z $%*+\-./:]{15}\Z")
QR_MODULES = 21
MARKER_MODULES = 33
QR_OFFSET = 6
OUTER_MARGIN = 4


def qr_matrix(payload: str) -> list[list[bool]]:
    try:
        import qrcode
        from qrcode.constants import ERROR_CORRECT_Q
        from qrcode.util import MODE_ALPHA_NUM, QRData
    except ImportError:
        raise SystemExit(
            "generate_qr.py: missing dependency; run "
            "'python3 -m pip install -r tools/qr/requirements-generator.txt'"
        ) from None

    qr = qrcode.QRCode(
        version=1,
        error_correction=ERROR_CORRECT_Q,
        box_size=1,
        border=0,
    )
    qr.add_data(QRData(payload, mode=MODE_ALPHA_NUM, check_data=True))
    qr.make(fit=False)
    matrix = qr.get_matrix()
    if len(matrix) != QR_MODULES or any(len(row) != QR_MODULES for row in matrix):
        raise RuntimeError("encoder did not produce a Version-1 matrix")
    return matrix


def bounded_matrix(payload: str) -> list[list[bool]]:
    qr = qr_matrix(payload)
    marker = [[False] * MARKER_MODULES for _ in range(MARKER_MODULES)]

    for y in range(MARKER_MODULES):
        for x in range(MARKER_MODULES):
            if (
                x < 2
                or y < 2
                or x >= MARKER_MODULES - 2
                or y >= MARKER_MODULES - 2
            ):
                marker[y][x] = True
    for y, row in enumerate(qr):
        for x, black in enumerate(row):
            marker[y + QR_OFFSET][x + QR_OFFSET] = black
    return marker


def canvas_matrix(marker: list[list[bool]]) -> list[list[bool]]:
    side = MARKER_MODULES + 2 * OUTER_MARGIN
    canvas = [[False] * side for _ in range(side)]
    for y, row in enumerate(marker):
        canvas[y + OUTER_MARGIN][
            OUTER_MARGIN : OUTER_MARGIN + MARKER_MODULES
        ] = row
    return canvas


def write_svg(
    path: Path, marker: list[list[bool]], payload: str, scale: int
) -> None:
    side = MARKER_MODULES + 2 * OUTER_MARGIN
    cells = []
    for y, row in enumerate(marker):
        for x, black in enumerate(row):
            if black:
                cells.append(f"M{x} {y}h1v1h-1z")
    title = html.escape(f"Waybeam bounded QR — {payload}")
    svg = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<svg xmlns="http://www.w3.org/2000/svg"\n'
        f'     viewBox="{-OUTER_MARGIN} {-OUTER_MARGIN} {side} {side}"\n'
        f'     width="{side * scale}" height="{side * scale}"\n'
        '     shape-rendering="crispEdges">\n'
        f"  <title>{title}</title>\n"
        f'  <rect x="{-OUTER_MARGIN}" y="{-OUTER_MARGIN}" '
        f'width="{side}" height="{side}" fill="#fff"/>\n'
        f'  <path fill="#000" d="{"".join(cells)}"/>\n'
        "</svg>\n"
    )
    path.write_text(svg, encoding="utf-8")


def raster_bytes(marker: list[list[bool]], scale: int) -> tuple[int, bytes]:
    canvas = canvas_matrix(marker)
    side = len(canvas) * scale
    row_bytes = []
    for row in canvas:
        expanded = b"".join(
            (b"\x00" if black else b"\xff") * scale for black in row
        )
        row_bytes.extend([expanded] * scale)
    return side, b"".join(row_bytes)


def write_pgm(path: Path, marker: list[list[bool]], scale: int) -> None:
    side, pixels = raster_bytes(marker, scale)
    path.write_bytes(f"P5\n{side} {side}\n255\n".encode("ascii") + pixels)


def write_png(path: Path, marker: list[list[bool]], scale: int) -> None:
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit(
            "generate_qr.py: PNG output needs Pillow; install the pinned "
            "requirements-generator.txt dependencies"
        ) from None
    side, pixels = raster_bytes(marker, scale)
    image = Image.frombytes("L", (side, side), pixels)
    image.save(path, optimize=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Generate a Version-1/Q, alphanumeric Waybeam QR inside the "
            "required continuous outer frame."
        )
    )
    parser.add_argument(
        "payload", help="16 characters: P or C plus 15 QR-alphanumeric"
    )
    parser.add_argument("output", type=Path, help="output .svg, .png, or .pgm")
    parser.add_argument(
        "--scale",
        type=int,
        default=24,
        help="integer pixels per marker unit for raster output (default: 24)",
    )
    args = parser.parse_args()
    if not PAYLOAD_RE.fullmatch(args.payload):
        parser.error(
            "payload must be exactly 16 QR-alphanumeric characters and start with P or C"
        )
    if args.output.suffix.lower() not in {".svg", ".png", ".pgm"}:
        parser.error("output extension must be .svg, .png, or .pgm")
    if not 1 <= args.scale <= 64:
        parser.error("--scale must be in the range 1..64")
    return args


def main() -> int:
    args = parse_args()
    marker = bounded_matrix(args.payload)
    suffix = args.output.suffix.lower()
    if suffix == ".svg":
        write_svg(args.output, marker, args.payload, args.scale)
    elif suffix == ".png":
        write_png(args.output, marker, args.scale)
    else:
        write_pgm(args.output, marker, args.scale)
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
