#!/usr/bin/env python3
"""
Regenerate the embedded dashboard gzip inside src/venc_webui.c from
web/dashboard.html.

Writes a deterministic gzip (mtime=0, fixed OS byte from Python's stdlib
default) so diffs on src/venc_webui.c reflect only actual HTML changes.

Usage:
    python3 tools/build_webui.py          # regenerate in place
    python3 tools/build_webui.py --check  # fail if src is out of date
"""
import argparse
import gzip
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC_HTML = ROOT / "web" / "dashboard.html"
SRC_C    = ROOT / "src" / "venc_webui.c"


def build_gzip(html_bytes: bytes) -> bytes:
    # mtime=0 so repeated builds produce byte-identical output.
    return gzip.compress(html_bytes, compresslevel=9, mtime=0)


def format_c_array(data: bytes, indent: str = "\t") -> str:
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append(indent + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return "\n".join(lines)


def splice_into_c(src_text: str, array_body: str) -> str:
    pat = re.compile(r"(dashboard_gz\[\] = \{\n)(.*?)(\n\};)", re.S)
    m = pat.search(src_text)
    if not m:
        raise SystemExit("could not find dashboard_gz[] block in venc_webui.c")
    return src_text[:m.start(2)] + array_body + src_text[m.end(2):]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if src is stale")
    args = ap.parse_args()

    html_bytes = SRC_HTML.read_bytes()
    gz = build_gzip(html_bytes)
    array_body = format_c_array(gz)

    current = SRC_C.read_text()
    expected = splice_into_c(current, array_body)

    if args.check:
        if current != expected:
            print("venc_webui.c is out of date with web/dashboard.html.",
                  file=sys.stderr)
            print("Run: python3 tools/build_webui.py", file=sys.stderr)
            return 1
        return 0

    if current == expected:
        print("venc_webui.c already up to date.")
        return 0
    SRC_C.write_text(expected)
    print(f"Regenerated {SRC_C.relative_to(ROOT)} "
          f"({len(gz)} gz bytes from {len(html_bytes)} html bytes).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
