#!/usr/bin/env python3
"""
Regenerate embedded gzip blobs inside src/*.c from their web/*.html sources.

Each entry in BLOBS maps an HTML file to a target C file and the array
name that holds the gzip bytes inside it.  The C file must contain a
block of the form:

    static const unsigned char <array_name>[] = {
        0x..., 0x..., ...
    };

The splicer rewrites everything between `<array_name>[] = {\\n` and
`\\n};` with deterministic output (mtime=0) so repeated runs produce
byte-identical files.

Usage:
    python3 tools/build_webui.py          # regenerate in place
    python3 tools/build_webui.py --check  # fail if any src is out of date
"""
import argparse
import gzip
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

BLOBS = [
    # (html source, C target, array name)
    (ROOT / "web" / "dashboard.html",
     ROOT / "src" / "venc_webui.c", "dashboard_gz"),
]


def build_gzip(html_bytes: bytes) -> bytes:
    # mtime=0 so repeated builds produce byte-identical output.
    return gzip.compress(html_bytes, compresslevel=9, mtime=0)


def format_c_array(data: bytes, indent: str = "\t") -> str:
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append(indent + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return "\n".join(lines)


def splice_into_c(src_text: str, array_name: str, array_body: str) -> str:
    pat = re.compile(
        r"(" + re.escape(array_name) + r"\[\] = \{\n)(.*?)(\n\};)", re.S)
    m = pat.search(src_text)
    if not m:
        raise SystemExit(
            f"could not find {array_name}[] block in source")
    return src_text[:m.start(2)] + array_body + src_text[m.end(2):]


def regenerate_one(html_path, c_path, array_name, check):
    html_bytes = html_path.read_bytes()
    gz = build_gzip(html_bytes)
    array_body = format_c_array(gz)
    current = c_path.read_text()
    expected = splice_into_c(current, array_name, array_body)
    rel_c = c_path.relative_to(ROOT)
    if check:
        if current != expected:
            print(
                f"{rel_c} is out of date with "
                f"{html_path.relative_to(ROOT)}.",
                file=sys.stderr)
            print("Run: python3 tools/build_webui.py", file=sys.stderr)
            return False
        return True
    if current == expected:
        print(f"{rel_c} already up to date.")
        return True
    c_path.write_text(expected)
    print(
        f"Regenerated {rel_c} "
        f"({len(gz)} gz bytes from {len(html_bytes)} html bytes).")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if any src is stale")
    args = ap.parse_args()

    ok = True
    for html_path, c_path, name in BLOBS:
        if not regenerate_one(html_path, c_path, name, args.check):
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
