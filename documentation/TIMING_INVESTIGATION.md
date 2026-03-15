Investigate and implement a sender/decoder timestamp propagation
  path in venc by reusing the existing RTP timestamp field,
  while preserving normal RTP sequence-number behavior. The goal is
  to support low-latency encoder->decoder timing diagnostics and
  decoder-buffer analysis in a controlled sender/receiver pair that
  I own. Do not introduce a new RTP extension field or custom
  payload header unless investigation proves the existing RTP
  timestamp field is unusable for this purpose.

  First, verify current behavior end to end. Confirm how the
  encoder currently populates RTP sequence and timestamp in both
  Star6E and Maruko backends, and confirm what the controlled
  decoder currently reads, ignores, or depends on. Distinguish
  clearly between the current simple in-tree/custom decoder
  behavior and generic RTP receiver behavior. Specifically
  determine whether the controlled decoder uses RTP sequence for
  depacketization, loss detection, or frame assembly; whether it
  uses RTP timestamp at all; whether any jitterbuffer-like logic
  exists; and whether changing RTP timestamp semantics would break
  current decode behavior. Sequence number must remain valid and
  standard-like unless investigation proves otherwise. Assume
  timestamp is the candidate field to repurpose, not sequence.

  Then design the smallest viable change for a controlled private
  pipeline: keep RTP sequence number as normal per-packet
  continuity metadata, keep SSRC/payload type behavior unchanged,
  and repurpose RTP timestamp as sender timing metadata usable by
  the controlled decoder. Use sender monotonic microseconds in the
  32-bit RTP timestamp field. The value should be derived from a
  monotonic clock on the sender and stored as a 32-bit microsecond
  counter. Wraparound is acceptable and should be handled with
  normal modulo arithmetic because the target latency-analysis
  window is only about 100-500 ms.

  On the decoder side, update the controlled decoder to explicitly
  parse and retain RTP timestamp and sequence. Preserve packet
  continuity logic via sequence numbers. Add instrumentation so the
  decoder can log, per completed frame/access unit, the received
  sender timestamp in microsecond, local receive/assemble/decode/
  display times, packet gaps, and any inferred buffering growth. If
  sender and receiver clocks are not synchronized, still make the
  decoder useful: support relative timing analysis, per-frame
  spacing, jitter, queue-growth analysis, and optional offset
  estimation if practical, prefer a simple NTP-style offset sync. Absolute glass-to-glass latency is
  the target; decoder compatibility and diagnostic usefulness are
  important.

  The investigation must answer these specific questions before
  finalizing implementation: Are current custom decoders ignoring
  RTP timestamp completely? Are they using only payload/FU start-
  end bits? Does any current decoder path or tool I care about
  depend on RTP timestamp semantics for playout? If a jitterbuffer
  is used, what exact RTP fields does it inspect? Will reusing
  timestamp in this private pipeline affect custom depacketization,
  GStreamer compatibility, or only standards-based receivers? Make
  the answer concrete, code-referenced, and implementation-driven.

  After investigation, implement the safest path for the controlled
  decoder environment, update both sender and decoder as needed,
  and provide: modified files, confirmed current behavior, exact
  new RTP timestamp semantics as sender monotonic microsecond
  modulo 2^32, any compatibility caveats, and how to interpret the
  resulting logs to determine whether decoder buffering is the main
  latency source.
