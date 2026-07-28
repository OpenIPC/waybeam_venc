#!/bin/sh

set -eu

decoder=${1:?usage: test_qr_cli.sh /path/to/qr_decode}
repo_dir=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
fixture="$repo_dir/tools/qr/test-images/bounded-P23456789ABCDEFG.pgm"
expected=P23456789ABCDEFG
test_dir=$(mktemp -d /tmp/waybeam-qr-cli.XXXXXX)

cleanup() {
	rm -rf "$test_dir"
}
trap cleanup 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

expect_rc() {
	expected_rc=$1
	shift
	set +e
	"$@" >"$test_dir/stdout" 2>"$test_dir/stderr"
	actual_rc=$?
	set -e
	if [ "$actual_rc" -ne "$expected_rc" ]; then
		echo "FAIL: expected rc=$expected_rc, got rc=$actual_rc: $*" >&2
		sed -n '1,8p' "$test_dir/stderr" >&2
		exit 1
	fi
}

[ -x "$decoder" ] || { echo "FAIL: decoder not executable: $decoder" >&2; exit 1; }
[ -f "$fixture" ] || { echo "FAIL: fixture missing: $fixture" >&2; exit 1; }

payload=$("$decoder" "$fixture")
[ "$payload" = "$expected" ] ||
	{ echo "FAIL: fixture decoded as '$payload'" >&2; exit 1; }

payload=$("$decoder" "$fixture" --stats 2>"$test_dir/stats")
[ "$payload" = "$expected" ] ||
	{ echo "FAIL: file-first --stats decoded as '$payload'" >&2; exit 1; }
grep -q 'summary result=decoded' "$test_dir/stats" ||
	{ echo "FAIL: --stats summary missing" >&2; exit 1; }

cp "$fixture" "$test_dir/-capture.pgm"
payload=$(cd "$test_dir" && "$decoder" -- -capture.pgm)
[ "$payload" = "$expected" ] ||
	{ echo "FAIL: -- filename decoded as '$payload'" >&2; exit 1; }

expect_rc 2 "$decoder" - "$fixture"

printf 'P5\n42949672960 1\n255\n' >"$test_dir/integer-overflow.pgm"
expect_rc 2 "$decoder" "$test_dir/integer-overflow.pgm"

printf 'P5\n4097 1\n255\n' >"$test_dir/oversize.pgm"
expect_rc 2 "$decoder" "$test_dir/oversize.pgm"

printf 'P5\n1 1\n1\n\000' >"$test_dir/wrong-maxval.pgm"
expect_rc 2 "$decoder" "$test_dir/wrong-maxval.pgm"

printf 'P5\n1 1\n255X\000' >"$test_dir/missing-separator.pgm"
expect_rc 2 "$decoder" "$test_dir/missing-separator.pgm"

echo "qr CLI tests: PASS"
