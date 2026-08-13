/* test_qr_phone.js — validate the JS marker encoder embedded in
 * tools/qr/test-images/phone.html.
 *
 * phone.html has to be a single self-contained file: the whole point is that a
 * phone can open it off a memory card with no network.  So the encoder lives
 * inline, and this harness extracts it from between the
 * WAYBEAM-QR-ENCODER-BEGIN/END sentinels rather than keeping a second copy that
 * could drift.
 *
 * Three checks per payload, because a QR encoder that is subtly wrong still
 * produces a plausible-looking square:
 *
 *   1. the rendered marker decodes back to its payload through the SAME cascade
 *      the craft runs (tests/qr_decode_host).  Authoritative.
 *   2. the Waybeam wrapper — everything outside the 21x21 symbol — is
 *      byte-identical to tools/qr/generate_qr.py, the generator that produced
 *      the checked-in fixtures.  This is the part unique to us.
 *   3. when this encoder and the generator happen to pick the same data mask,
 *      the full matrix must be byte-identical.
 *
 * Note on 3: the mask is NOT required to match.  All eight are valid and
 * decodable, the spec only asks for the lowest penalty score, and python-qrcode
 * scores masks slightly differently from ISO 18004 — so the two disagree on
 * roughly two thirds of payloads while both being correct.  Measured: when the
 * masks do agree the matrices are identical to the module, and when they differ
 * only format-info and data modules differ, never the function patterns or the
 * frame.  Demanding bit-identity here would mean deliberately reproducing
 * another library's deviation from the standard.
 *
 * Usage:  node tests/test_qr_phone.js
 * Needs:  python3 with the generator deps, and `make tests/qr_decode_host`.
 */
"use strict";

const fs = require("fs");
const path = require("path");
const os = require("os");
const { execFileSync } = require("child_process");

const ROOT = path.resolve(__dirname, "..");
/* The generator needs the `qrcode` package, which distro pythons increasingly
 * refuse to install into (PEP 668).  PYTHON=... points at a venv. */
const PYTHON = process.env.PYTHON || "python3";
const PHONE = path.join(ROOT, "tools/qr/test-images/phone.html");
const DECODER = path.join(ROOT, "tests/qr_decode_host");

/* ---- pull the encoder out of phone.html -------------------------------- */
function loadEncoder() {
  const html = fs.readFileSync(PHONE, "utf8");
  const begin = html.indexOf("WAYBEAM-QR-ENCODER-BEGIN");
  const end = html.indexOf("WAYBEAM-QR-ENCODER-END");
  if (begin < 0 || end < 0) throw new Error("encoder sentinels missing from phone.html");
  const block = html.slice(begin, end);
  const open = block.indexOf("<script>");
  const close = block.lastIndexOf("</script>");
  if (open < 0 || close < 0) throw new Error("no <script> between the sentinels");
  const src = block.slice(open + "<script>".length, close);
  const module = { exports: {} };
  new Function("module", "crypto", src)(module, undefined);
  if (!module.exports.markerMatrix) throw new Error("encoder did not export markerMatrix");
  return module.exports;
}

/* ---- reference matrix from the Python generator ------------------------ */
function pythonMatrix(payload) {
  const script = `
import sys
sys.path.insert(0, ${JSON.stringify(path.join(ROOT, "tools/qr"))})
from generate_qr import bounded_matrix
m = bounded_matrix(${JSON.stringify(payload)})
print("\\n".join("".join("1" if v else "0" for v in row) for row in m))
`;
  return execFileSync(PYTHON, ["-c", script], { encoding: "utf8" })
    .trim().split("\n");
}

function jsMatrix(enc, payload) {
  return enc.markerMatrix(payload)
    .map((row) => row.map((v) => (v ? "1" : "0")).join(""));
}

/* Recover the data mask from a marker's format-info block, so a matrix
 * mismatch can be attributed to mask choice rather than to a broken encoder.
 * Format copy 1 sits around the top-left finder; the 15 bits are XOR-masked
 * with 0x5412 and the top five are (ECC level << 3) | mask. */
const QR_OFFSET = 6, QR_SIZE = 21;
function maskOf(rows) {
  const at = (y, x) => (rows[QR_OFFSET + y][QR_OFFSET + x] === "1" ? 1 : 0);
  let bits = 0;
  for (let i = 0; i <= 5; i++) bits |= at(i, 8) << i;
  bits |= at(7, 8) << 6;
  bits |= at(8, 8) << 7;
  bits |= at(8, 7) << 8;
  for (let i = 9; i < 15; i++) bits |= at(8, 14 - i) << i;
  return ((bits ^ 0x5412) >> 10) & 7;
}

/* Everything except the 21x21 symbol: the 2-module solid frame and the
 * 4-module quiet zone that make this a Waybeam marker rather than a bare QR. */
function wrapperEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let y = 0; y < a.length; y++)
    for (let x = 0; x < a.length; x++) {
      const inSymbol = x >= QR_OFFSET && x < QR_OFFSET + QR_SIZE &&
                       y >= QR_OFFSET && y < QR_OFFSET + QR_SIZE;
      if (!inSymbol && a[y][x] !== b[y][x]) return false;
    }
  return true;
}

/* ---- render a marker to P5 PGM, exactly as the page draws it ----------- */
function writePgm(enc, payload, file, scale) {
  const m = enc.markerMatrix(payload);
  const margin = enc.MARGIN;
  const side = (m.length + 2 * margin) * scale;
  const px = Buffer.alloc(side * side, 255);
  for (let y = 0; y < m.length; y++)
    for (let x = 0; x < m.length; x++)
      if (m[y][x])
        for (let dy = 0; dy < scale; dy++)
          px.fill(0,
            ((y + margin) * scale + dy) * side + (x + margin) * scale,
            ((y + margin) * scale + dy) * side + (x + margin) * scale + scale);
  fs.writeFileSync(file, Buffer.concat([Buffer.from(`P5\n${side} ${side}\n255\n`), px]));
}

/* ---- run ---------------------------------------------------------------- */
let pass = 0, fail = 0;
const ck = (name, ok, detail) => {
  if (ok) { pass++; console.log(`  PASS  ${name}`); }
  else { fail++; console.log(`  FAIL  ${name}${detail ? "  " + detail : ""}`); }
};

console.log("\n=== qr phone.html encoder ===");
const enc = loadEncoder();
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "qrphone-"));

/* Payloads chosen to move every part of the encoder: the shipped fixture, both
 * legal prefixes, the alphabet extremes, the punctuation subset that is easiest
 * to get wrong, and repetition that pushes the mask-penalty scoring around. */
const PAYLOADS = [
  "P23456789ABCDEFG",
  "C23456789ABCDEFG",
  "P000000000000000",
  "PZZZZZZZZZZZZZZZ",
  "P $%*+-./:0123AZ",
  "C:./-+*%$ ZA9876",
  "PAAAAAAAAAAAAAAA",
  "P0Z1Y2X3W4V5U6T7",
  "CQRSTUVWXYZ01234",
  "P987654321ZYXWVU",
];

let haveDecoder = fs.existsSync(DECODER);
if (!haveDecoder) console.log("  NOTE  tests/qr_decode_host missing — run `make qr-test-cli` first");

let sameMask = 0;
for (const payload of PAYLOADS) {
  const js = jsMatrix(enc, payload);
  const py = pythonMatrix(payload);
  const mj = maskOf(js), mp = maskOf(py);

  ck(`marker_size ${payload}`, js.length === 33 && py.length === 33,
     `${js.length} vs ${py.length}`);
  ck(`wrapper_matches_generator ${payload}`, wrapperEqual(js, py));
  if (mj === mp) {
    sameMask++;
    ck(`matrix_identical_when_mask_agrees ${payload} (mask ${mj})`,
       js.every((r, i) => r === py[i]));
  }

  if (haveDecoder) {
    /* scale 6 matches the checked-in .pgm fixture; scale 2 is a hard case that
     * proves the marker survives at a size a camera might actually see. */
    for (const scale of [6, 2]) {
      const f = path.join(tmp, `m${scale}.pgm`);
      writePgm(enc, payload, f, scale);
      let out = "";
      try { out = execFileSync(DECODER, [f], { encoding: "utf8" }).trim(); }
      catch (e) { out = `<rc=${e.status}>`; }
      ck(`decodes_at_scale${scale} ${payload}`, out === payload, `got "${out}"`);
    }
  }
}

console.log(`  INFO  mask agreed with the generator on ${sameMask}/${PAYLOADS.length} payloads` +
            ` (agreement is not required — see the header)`);

/* payload validation must match tools/qr/generate_qr.py's PAYLOAD_RE */
ck("rejects_short",        !enc.validate("P2345"));
ck("rejects_long",         !enc.validate("P23456789ABCDEFGH"));
ck("rejects_bad_prefix",   !enc.validate("X23456789ABCDEFG"));
ck("rejects_lowercase",    !enc.validate("Pabcdefghijklmno"));
ck("rejects_out_of_set",   !enc.validate("P2345678!ABCDEFG"));
ck("accepts_fixture",       enc.validate("P23456789ABCDEFG"));
ck("accepts_punctuation",   enc.validate("P $%*+-./:0123AZ"));

/* the Random button must always produce something the craft will accept */
let randomOk = true;
for (let i = 0; i < 200; i++) if (!enc.validate(enc.randomPayload())) randomOk = false;
ck("random_always_valid", randomOk);

fs.rmSync(tmp, { recursive: true, force: true });
console.log(`\n=== Results: ${pass} passed, ${fail} failed ===`);
process.exit(fail === 0 ? 0 : 1);
