#!/usr/bin/env python3
"""Receive RTP H.265 (RFC 7798), depayload to H.265 ES on stdout.

Usage:
  rtp_h265_capture.py --port 5611 --duration 30 --out /tmp/x.h265

Designed for the refPred harness — no external deps.  Handles single-NAL,
aggregation (AP, type 48), and fragmentation (FU, type 49) payloads.
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import time
from typing import Optional


NAL_AP = 48
NAL_FU = 49


def write_nal(out, nal_bytes: bytes) -> None:
    out.write(b"\x00\x00\x00\x01")
    out.write(nal_bytes)


def handle_payload(payload: bytes, fu_state: list, out) -> None:
    """Depayload one RTP payload."""
    if len(payload) < 2:
        return
    b0, b1 = payload[0], payload[1]
    nal_type = (b0 >> 1) & 0x3f

    if nal_type < NAL_AP:
        # Single NAL unit.
        write_nal(out, payload)
        fu_state.clear()
        return

    if nal_type == NAL_AP:
        # Aggregation packet: [DONL?] [size NAL] [size NAL] ...
        # We assume DONL absent (typical for non-interleaved sprop=0).
        i = 2
        n = len(payload)
        while i + 2 <= n:
            size = struct.unpack(">H", payload[i:i + 2])[0]
            i += 2
            if i + size > n:
                break
            write_nal(out, payload[i:i + size])
            i += size
        fu_state.clear()
        return

    if nal_type == NAL_FU:
        if len(payload) < 3:
            return
        # FU header in payload[2]: S(1) E(1) R(1) FuType(5)
        fu_header = payload[2]
        start = (fu_header & 0x80) != 0
        end = (fu_header & 0x40) != 0
        fu_type = fu_header & 0x3f
        if start:
            # Rebuild NAL header from payload header bits + fu_type.
            new_b0 = (b0 & 0x81) | (fu_type << 1)
            new_b1 = b1
            fu_state.clear()
            fu_state.append(bytes([new_b0, new_b1]))
            fu_state.append(payload[3:])
        else:
            if not fu_state:
                # FU continuation without start — drop.
                return
            fu_state.append(payload[3:])
        if end and fu_state:
            write_nal(out, b"".join(fu_state))
            fu_state.clear()
        return
    # Unknown — drop.


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--duration", type=float, required=True,
        help="seconds to capture")
    ap.add_argument("--out", required=True, help="output .h265 ES")
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    except (AttributeError, OSError):
        pass
    sock.bind((args.bind, args.port))
    sock.settimeout(1.0)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)

    deadline = time.monotonic() + args.duration
    fu_state: list = []
    packets = 0
    bytes_recv = 0

    with open(args.out, "wb") as out:
        while time.monotonic() < deadline:
            try:
                data, _ = sock.recvfrom(65536)
            except socket.timeout:
                continue
            packets += 1
            bytes_recv += len(data)
            if len(data) < 12:
                continue
            # RTP fixed header: 12 bytes.  Skip CSRC if any.
            v = (data[0] >> 6) & 0x3
            if v != 2:
                continue
            cc = data[0] & 0x0f
            offset = 12 + 4 * cc
            ext = (data[0] >> 4) & 0x01
            if ext:
                if offset + 4 > len(data):
                    continue
                ext_len = struct.unpack(">H", data[offset + 2:offset + 4])[0]
                offset += 4 + 4 * ext_len
            if offset >= len(data):
                continue
            payload = data[offset:]
            handle_payload(payload, fu_state, out)

    sys.stderr.write(
        f"rtp_h265_capture: port={args.port} duration={args.duration} "
        f"packets={packets} bytes={bytes_recv} out={args.out}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
