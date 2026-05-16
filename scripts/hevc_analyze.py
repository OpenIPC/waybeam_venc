#!/usr/bin/env python3
"""HEVC ES bitstream analyzer for refPred verification.

Two subcommands:

  walk  <file>         Histogram NAL types and temporal_id distribution.
  drop  <file> <rate>  Stream NALs to stdout, dropping <rate> fraction of
                       non-IDR / non-parameter-set NALs.  Deterministic via
                       --seed (default 1).

HEVC NAL header (2 bytes after start code):
  byte0: forbidden_zero_bit(1) | nal_unit_type(6) | nuh_layer_id_msb(1)
  byte1: nuh_layer_id_lsb(5)   | nuh_temporal_id_plus1(3)
"""

from __future__ import annotations

import argparse
import os
import random
import sys
from collections import Counter
from typing import Iterator, Tuple


# HEVC NAL types (subset that matters here)
NAL_TRAIL_N    = 0
NAL_TRAIL_R    = 1
NAL_TSA_N      = 2
NAL_TSA_R      = 3
NAL_RASL_R     = 9
NAL_BLA_W_LP   = 16
NAL_IDR_W_RADL = 19
NAL_IDR_N_LP   = 20
NAL_CRA_NUT    = 21
NAL_VPS        = 32
NAL_SPS        = 33
NAL_PPS        = 34
NAL_AUD        = 35
NAL_PREFIX_SEI = 39
NAL_SUFFIX_SEI = 40

NAL_NAMES = {
    0: "TRAIL_N", 1: "TRAIL_R", 2: "TSA_N", 3: "TSA_R",
    4: "STSA_N", 5: "STSA_R", 6: "RADL_N", 7: "RADL_R",
    8: "RASL_N", 9: "RASL_R",
    16: "BLA_W_LP", 17: "BLA_W_RADL", 18: "BLA_N_LP",
    19: "IDR_W_RADL", 20: "IDR_N_LP", 21: "CRA",
    32: "VPS", 33: "SPS", 34: "PPS", 35: "AUD",
    36: "EOS", 37: "EOB", 38: "FD",
    39: "PREFIX_SEI", 40: "SUFFIX_SEI",
}

PARAM_OR_IDR = {NAL_VPS, NAL_SPS, NAL_PPS,
                NAL_IDR_W_RADL, NAL_IDR_N_LP, NAL_CRA_NUT,
                NAL_BLA_W_LP, NAL_AUD}


def iter_nals(data: bytes) -> Iterator[Tuple[int, int, bytes]]:
    """Yield (start_offset, end_offset, nal_bytes) for each NAL.

    Splits on 00 00 00 01 / 00 00 01 start codes.  nal_bytes excludes the
    start code prefix.
    """
    n = len(data)
    i = 0
    starts = []
    while i < n - 3:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                starts.append((i, i + 3))
                i += 3
                continue
            if data[i + 2] == 0 and i + 3 < n and data[i + 3] == 1:
                starts.append((i, i + 4))
                i += 4
                continue
        i += 1
    for idx, (sc_begin, payload_begin) in enumerate(starts):
        if idx + 1 < len(starts):
            payload_end = starts[idx + 1][0]
        else:
            payload_end = n
        yield sc_begin, payload_end, data[payload_begin:payload_end]


def parse_header(nal: bytes) -> Tuple[int, int, int]:
    """Return (nal_type, layer_id, temporal_id)."""
    if len(nal) < 2:
        return -1, -1, -1
    b0, b1 = nal[0], nal[1]
    nal_type = (b0 >> 1) & 0x3f
    layer_id = ((b0 & 0x01) << 5) | ((b1 >> 3) & 0x1f)
    tid_p1 = b1 & 0x07
    return nal_type, layer_id, tid_p1 - 1


def cmd_walk(path: str) -> int:
    with open(path, "rb") as f:
        data = f.read()
    by_type: Counter[int] = Counter()
    by_tid: Counter[int] = Counter()
    pair: Counter[Tuple[int, int]] = Counter()
    total = 0
    for _, _, nal in iter_nals(data):
        nal_type, _, tid = parse_header(nal)
        if nal_type < 0:
            continue
        total += 1
        by_type[nal_type] += 1
        by_tid[tid] += 1
        pair[(nal_type, tid)] += 1

    print(f"file: {path}  size: {len(data)} B  nals: {total}")
    print()
    print(f"{'nal_type':>3}  {'name':<14}  {'count':>8}")
    for t, c in sorted(by_type.items()):
        print(f"{t:>3}  {NAL_NAMES.get(t, '?'):<14}  {c:>8}")
    print()
    print(f"{'temporal_id':>11}  {'count':>8}")
    for tid in sorted(by_tid):
        print(f"{tid:>11}  {by_tid[tid]:>8}")
    print()
    print(f"{'nal_type':>3}  {'name':<14}  {'tid':>3}  {'count':>8}")
    for (t, tid), c in sorted(pair.items()):
        print(f"{t:>3}  {NAL_NAMES.get(t, '?'):<14}  {tid:>3}  {c:>8}")
    return 0


def cmd_drop_at(path: str, drop_idx: int, start_from_vps: bool) -> int:
    """Drop a single non-param NAL by 1-based index.

    --start_from_vps: skip everything before the first VPS so the decoder
    has params at byte 0 (necessary when the source capture started
    mid-stream).
    """
    with open(path, "rb") as f:
        data = f.read()
    out = sys.stdout.buffer
    seen_vps = False
    droppable = 0
    dropped_at = None
    for sc_begin, end, nal in iter_nals(data):
        nal_type, _, _ = parse_header(nal)
        unit = data[sc_begin:end]
        if start_from_vps and not seen_vps:
            if nal_type == NAL_VPS:
                seen_vps = True
            else:
                continue
        is_param_or_idr = nal_type in PARAM_OR_IDR
        if is_param_or_idr:
            out.write(unit)
            continue
        droppable += 1
        if droppable == drop_idx:
            dropped_at = sc_begin
            continue
        out.write(unit)
    sys.stderr.write(
        f"hevc_analyze drop_at: idx={drop_idx} dropped_offset={dropped_at} "
        f"non_param_seen={droppable}\n")
    return 0


def cmd_drop(path: str, rate: float, seed: int, warmup: int,
             start_from_vps: bool, only_type: int | None = None) -> int:
    """Randomly drop non-parameter NALs at the given rate.

    The drop tool no longer requires a captured IDR — many encoders (ours
    included, when IntraRefresh is on) never emit IDR_W_RADL.  Instead we
    optionally trim the prefix up to the first VPS (so the decoder boots
    with parameters in hand), then warm up for `warmup` NALs before
    enabling drops.
    """
    rng = random.Random(seed)
    with open(path, "rb") as f:
        data = f.read()
    out = sys.stdout.buffer
    seen_vps = False
    droppable = 0
    dropped = 0
    by_type_seen = {0: 0, 1: 0}
    by_type_dropped = {0: 0, 1: 0}
    for sc_begin, end, nal in iter_nals(data):
        nal_type, _, _ = parse_header(nal)
        unit = data[sc_begin:end]
        if start_from_vps and not seen_vps:
            if nal_type == NAL_VPS:
                seen_vps = True
            else:
                continue
        is_param_or_idr = nal_type in PARAM_OR_IDR
        if is_param_or_idr:
            out.write(unit)
            continue
        if droppable < warmup:
            droppable += 1
            out.write(unit)
            continue
        droppable += 1
        if nal_type in by_type_seen:
            by_type_seen[nal_type] += 1
        eligible = only_type is None or nal_type == only_type
        if eligible and rng.random() < rate:
            dropped += 1
            if nal_type in by_type_dropped:
                by_type_dropped[nal_type] += 1
            continue
        out.write(unit)

    sys.stderr.write(
        f"hevc_analyze drop: rate={rate} seed={seed} warmup={warmup} "
        f"droppable={droppable} dropped={dropped} "
        f"TRAIL_N={by_type_dropped[0]}/{by_type_seen[0]} "
        f"TRAIL_R={by_type_dropped[1]}/{by_type_seen[1]}\n")
    return 0


class BitReader:
    """Minimal big-endian bit reader over a bytes-like RBSP payload.

    Handles emulation prevention byte removal (00 00 03 -> 00 00) per
    T-REC-H.265 7.4.1.1 — required before reading slice header syntax.
    """

    def __init__(self, payload: bytes):
        # Strip emulation prevention bytes.
        rbsp = bytearray()
        i = 0
        n = len(payload)
        while i < n:
            if i + 2 < n and payload[i] == 0 and payload[i + 1] == 0 \
                    and payload[i + 2] == 3:
                rbsp.append(0)
                rbsp.append(0)
                i += 3
                continue
            rbsp.append(payload[i])
            i += 1
        self.buf = bytes(rbsp)
        self.bitpos = 0  # absolute bit index

    def u(self, n: int) -> int:
        v = 0
        for _ in range(n):
            byte = self.buf[self.bitpos >> 3]
            bit = (byte >> (7 - (self.bitpos & 7))) & 1
            v = (v << 1) | bit
            self.bitpos += 1
        return v

    def ue(self) -> int:
        # Exp-Golomb unsigned.
        zeros = 0
        while self.u(1) == 0 and self.bitpos < len(self.buf) * 8:
            zeros += 1
            if zeros > 32:
                return 0
        if zeros == 0:
            return 0
        return (1 << zeros) - 1 + self.u(zeros)


# Cached SPS log2 max POC bits — needed to read slice_pic_order_cnt_lsb.
# HEVC spec: log2_max_pic_order_cnt_lsb_minus4 ∈ [0..12], so 4..16 bits.
def parse_sps_for_poc_lsb_bits(sps_rbsp: bytes) -> int:
    """Return log2_max_pic_order_cnt_lsb (i.e. minus4 + 4)."""
    # Skip the 2-byte NAL header included in sps_rbsp[0:2] before reading.
    br = BitReader(sps_rbsp[2:])
    # sps_video_parameter_set_id u(4), sps_max_sub_layers_minus1 u(3),
    # sps_temporal_id_nesting_flag u(1)
    br.u(4); max_sub = br.u(3); br.u(1)
    # profile_tier_level(1, sps_max_sub_layers_minus1) — fixed 12 bytes for
    # the general layer, then per sub-layer flags + optional info.  We need
    # to parse it carefully but a common simplification: skip 12 bytes for
    # general profile/level, then 2 bytes of sub_layer_profile/level flags,
    # then per-sub-layer 12-byte blocks for any sub-layer with flags set.
    # For our streams (max_sub_layers=0), it's just the 12 bytes.
    for _ in range(12):
        br.u(8)
    if max_sub > 0:
        sub_layer_profile_present = []
        sub_layer_level_present = []
        for _ in range(max_sub):
            sub_layer_profile_present.append(br.u(1))
            sub_layer_level_present.append(br.u(1))
        # reserved_zero_2bits each up to 8 sub-layers (max_sub_layers <= 7)
        for _ in range(max_sub, 8):
            br.u(2)
        for i in range(max_sub):
            if sub_layer_profile_present[i]:
                for _ in range(11):
                    br.u(8)
            if sub_layer_level_present[i]:
                br.u(8)
    # sps_seq_parameter_set_id ue(v)
    br.ue()
    # chroma_format_idc ue(v)
    cfi = br.ue()
    if cfi == 3:
        br.u(1)  # separate_colour_plane_flag
    br.ue()  # pic_width_in_luma_samples
    br.ue()  # pic_height_in_luma_samples
    if br.u(1):  # conformance_window_flag
        br.ue(); br.ue(); br.ue(); br.ue()
    br.ue()  # bit_depth_luma_minus8
    br.ue()  # bit_depth_chroma_minus8
    log2_minus4 = br.ue()
    return log2_minus4 + 4


def parse_slice_poc_lsb(nal_payload: bytes, poc_lsb_bits: int,
                        first_slice_only: bool = True) -> int | None:
    """Return slice_pic_order_cnt_lsb for this slice, or None if can't parse."""
    # nal_payload includes 2-byte NAL header.
    nal_type = (nal_payload[0] >> 1) & 0x3f
    # Slice types: TRAIL_N=0, TRAIL_R=1, TSA_*, STSA_*, RADL_*, RASL_*,
    # IDR_*, CRA, BLA_* — covering 0..21 except some reserved.
    if nal_type > 23:
        return None
    br = BitReader(nal_payload[2:])
    first_slice = br.u(1)
    if not first_slice and first_slice_only:
        return None
    # If RAP NAL: no_output_of_prior_pics_flag u(1)
    if 16 <= nal_type <= 23:
        br.u(1)
    br.ue()  # slice_pic_parameter_set_id
    # Skip dependent_slice_segment_flag handling for non-first slices.
    # slice_segment_header() body starts here for first slice:
    # slice_reserved_flag[i] — depends on PPS (num_extra_slice_header_bits).
    # We don't have PPS parsing, so assume 0.
    # slice_type ue(v)
    br.ue()
    # output_flag_present_flag (PPS) — assume false; pic_output_flag skipped
    # separate_colour_plane_flag — assume not chroma 4:4:4
    if not (16 <= nal_type <= 21):
        # Non-IDR slices have slice_pic_order_cnt_lsb here
        return br.u(poc_lsb_bits)
    return 0  # IDR -> POC LSB is 0


def cmd_frames(path: str, limit: int) -> int:
    with open(path, "rb") as f:
        data = f.read()
    poc_lsb_bits = None
    sps_rbsp = None
    rows = []
    last_poc_lsb = None
    for idx, (sc_begin, end, nal) in enumerate(iter_nals(data)):
        nal_type, _, tid = parse_header(nal)
        size = end - sc_begin
        if nal_type == NAL_SPS:
            sps_rbsp = nal
            try:
                poc_lsb_bits = parse_sps_for_poc_lsb_bits(sps_rbsp)
            except Exception:
                poc_lsb_bits = None
            rows.append((idx, nal_type, NAL_NAMES.get(nal_type, "?"),
                tid, size, None))
            continue
        if nal_type in (NAL_VPS, NAL_PPS, NAL_AUD,
                        NAL_PREFIX_SEI, NAL_SUFFIX_SEI):
            rows.append((idx, nal_type, NAL_NAMES.get(nal_type, "?"),
                tid, size, None))
            continue
        # Slice
        poc_lsb = None
        if poc_lsb_bits is not None:
            try:
                poc_lsb = parse_slice_poc_lsb(nal, poc_lsb_bits)
            except Exception:
                poc_lsb = None
        rows.append((idx, nal_type, NAL_NAMES.get(nal_type, "?"),
            tid, size, poc_lsb))

    header = f"file: {path}  size: {len(data)} B  poc_lsb_bits: {poc_lsb_bits}"
    print(header)
    print(f"{'idx':>5} {'type':>4} {'name':<12} {'tid':>3} {'size':>8} "
        f"{'poc_lsb':>8} {'Δposc':>6}")
    last = None
    n_shown = 0
    for idx, t, name, tid, size, poc in rows:
        if n_shown >= limit:
            break
        delta = ""
        if poc is not None and last is not None:
            delta = str(poc - last)
        if poc is not None:
            last = poc
        poc_s = "-" if poc is None else str(poc)
        print(f"{idx:>5} {t:>4} {name:<12} {tid:>3} {size:>8} "
            f"{poc_s:>8} {delta:>6}")
        n_shown += 1
    return 0


def cmd_refs(path: str, frames: int) -> int:
    """Detect the reference structure by running ffmpeg -v debug and reading
    'Could not find ref with POC' lines.  The decoder emits these for the
    very first frames after stream start (no IDR captured yet), revealing
    the slice's short_term_ref_pic_set delta — the proof of pyramid step.

    We loop the input via concat:N to coax more 'cold start' events out of
    the decoder, then summarise the missing-ref deltas.
    """
    import subprocess

    # Run ffmpeg with stderr captured.  Use stream-copy + null muxer; we
    # only care about the decoder's stderr trace.
    cmd = [
        "ffmpeg", "-v", "debug",
        "-f", "hevc", "-i", path,
        "-frames:v", str(frames),
        "-f", "null", "-",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    log = proc.stderr

    missing = []
    decoded_pocs = []
    for line in log.splitlines():
        if "Could not find ref with POC" in line:
            try:
                poc = int(line.rsplit(" ", 1)[-1].rstrip("."))
                missing.append(poc)
            except ValueError:
                pass
        elif "Decoded frame with POC" in line:
            try:
                poc_part = line.rsplit("POC ", 1)[-1].rstrip(".\n ")
                # Format: "0/150"
                if "/" in poc_part:
                    poc = int(poc_part.split("/")[-1])
                else:
                    poc = int(poc_part)
                decoded_pocs.append(poc)
            except ValueError:
                pass

    print(f"file: {path}")
    print(f"missing_refs: {missing[:10]}{' ...' if len(missing) > 10 else ''}")
    print(f"decoded_pocs (first 20): {decoded_pocs[:20]}")
    # Compute the first delta — that's the proof.  For a flat single-ref
    # stream, first decoded POC = first missing ref + 1 (delta=1).  For a
    # pyramid, the delta is the step size.
    if missing and decoded_pocs:
        first_decoded = decoded_pocs[0]
        first_missing = missing[0]
        delta = first_decoded - first_missing
        print(f"first decoded={first_decoded}, first missing ref={first_missing}, "
            f"delta={delta}")
        if delta == 1:
            print("  ⇒ FLAT single-reference (no pyramid)")
        elif delta > 1:
            print(f"  ⇒ PYRAMID step {delta} "
                f"(refPred active — base period likely {delta})")
    # Check for out-of-POC-order decoding (decoder reorders enhance vs base).
    if len(decoded_pocs) >= 2:
        inversions = sum(1 for i in range(len(decoded_pocs) - 1)
                         if decoded_pocs[i + 1] < decoded_pocs[i])
        print(f"decode-order inversions: {inversions} "
            f"(0 = monotonic, >0 = multi-reference reordering)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    pw = sub.add_parser("walk", help="histogram NAL types/temporal_ids")
    pw.add_argument("file")

    pd = sub.add_parser("drop", help="randomly drop NALs to stdout")
    pd.add_argument("file")
    pd.add_argument("rate", type=float, help="0.0..1.0 drop probability")
    pd.add_argument("--seed", type=int, default=1)
    pd.add_argument("--warmup", type=int, default=60,
        help="keep first N droppable NALs (default 60)")
    pd.add_argument("--start-from-vps", action="store_true",
        help="skip prefix until first VPS so decoder has params")
    pd.add_argument("--only-type", type=int, default=None,
        help="only drop NALs of this nal_unit_type (e.g. 0=TRAIL_N, 1=TRAIL_R)")

    pf = sub.add_parser("frames",
        help="per-NAL size + slice POC LSB (proof of pyramid structure)")
    pf.add_argument("file")
    pf.add_argument("--limit", type=int, default=80,
        help="max NALs to print (default 80)")

    pr = sub.add_parser("refs",
        help="detect ref structure via ffmpeg debug trace (pyramid step)")
    pr.add_argument("file")
    pr.add_argument("--frames", type=int, default=60)

    pda = sub.add_parser("drop_at",
        help="drop ONE specific NAL by 1-based index (reproducible)")
    pda.add_argument("file")
    pda.add_argument("idx", type=int, help="NAL index to drop (1-based)")
    pda.add_argument("--start-from-vps", action="store_true",
        help="skip prefix until first VPS so decoder has params")

    args = ap.parse_args()
    if args.cmd == "walk":
        return cmd_walk(args.file)
    if args.cmd == "drop":
        return cmd_drop(args.file, args.rate, args.seed, args.warmup,
            args.start_from_vps, args.only_type)
    if args.cmd == "frames":
        return cmd_frames(args.file, args.limit)
    if args.cmd == "refs":
        return cmd_refs(args.file, args.frames)
    if args.cmd == "drop_at":
        return cmd_drop_at(args.file, args.idx, args.start_from_vps)
    return 1


if __name__ == "__main__":
    sys.exit(main())
