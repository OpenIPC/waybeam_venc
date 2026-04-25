#!/bin/bash
#
# Build patched wfb_tx (SHM ring input) + SHM diagnostic tools.
#
# Cross-compiles everything with the star6e toolchain for SigmaStar
# Infinity6E (armv7l). Clones wfb-ng, cross-compiles libsodium,
# applies the SHM input patch, and builds all binaries.
#
# Usage:
#   ./build_wfb_tx.sh            # build all
#   ./build_wfb_tx.sh --clean    # remove build directory
#   ./build_wfb_tx.sh --deploy   # build + scp to device
#
# Output (all in build/):
#   wfb_tx             - patched wfb_tx with -H (SHM), -b, -r, -x flags (cross, static)
#   wfb_tx_cmd         - runtime control client (set_fec, set_radio, set_mbit, get_*) (cross, static)
#   wfb_keygen         - key generator (cross, static)
#   shm_ring_stats     - ring status checker (cross, dynamic)
#   shm_consumer_test  - ring throughput tester (cross, dynamic)
#   wfb_rx_native      - x86_64 native build for the ground-station laptop;
#                        the cross pcap stub only covers wfb_tx's inject path
#
# Requirements:
#   - star6e toolchain at ../toolchain/toolchain.sigmastar-infinity6e/
#   - Host x86_64: g++, gcc, libsodium-dev, libpcap-dev (for wfb_rx_native)
#   - git, curl, autotools (first run only, for libsodium cross-build)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENC_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$SCRIPT_DIR/build"
LIBSODIUM_VER="1.0.20"
DEPLOY_HOST="${DEPLOY_HOST:-192.168.1.10}"
DEPLOY_DIR="${DEPLOY_DIR:-/usr/bin}"

# Toolchain (at repo root level)
TOOLCHAIN_DIR="$VENC_ROOT/toolchain/toolchain.sigmastar-infinity6e"
CROSS_PREFIX="arm-openipc-linux-gnueabihf"
CROSS_CC="$TOOLCHAIN_DIR/bin/${CROSS_PREFIX}-gcc"
CROSS_CXX="$TOOLCHAIN_DIR/bin/${CROSS_PREFIX}-g++"
CROSS_AR="$TOOLCHAIN_DIR/bin/${CROSS_PREFIX}-ar"
CROSS_RANLIB="$TOOLCHAIN_DIR/bin/${CROSS_PREFIX}-ranlib"
CROSS_STRIP="$TOOLCHAIN_DIR/bin/${CROSS_PREFIX}-strip"

# Verify toolchain exists
if [ ! -x "$CROSS_CC" ]; then
    echo "ERROR: Toolchain not found at $TOOLCHAIN_DIR"
    echo "Expected: $CROSS_CC"
    exit 1
fi

if [ "${1:-}" = "--clean" ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    echo "Done."
    exit 0
fi

DO_DEPLOY=0
if [ "${1:-}" = "--deploy" ]; then
    DO_DEPLOY=1
fi

mkdir -p "$BUILD_DIR"

# ── Step 1: Download and cross-compile libsodium ─────────────────────

SODIUM_DIR="$BUILD_DIR/libsodium-${LIBSODIUM_VER}"
SODIUM_PREFIX="$BUILD_DIR/sodium-install"

if [ ! -f "$SODIUM_PREFIX/lib/libsodium.a" ]; then
    echo "=== Building libsodium ${LIBSODIUM_VER} ==="

    if [ ! -d "$SODIUM_DIR" ]; then
        cd "$BUILD_DIR"
        TARBALL="libsodium-${LIBSODIUM_VER}.tar.gz"
        if [ ! -f "$TARBALL" ]; then
            echo "Downloading libsodium..."
            curl -L -o "$TARBALL" \
                "https://download.libsodium.org/libsodium/releases/${TARBALL}"
        fi
        tar xzf "$TARBALL"
    fi

    cd "$SODIUM_DIR"
    ./configure \
        --host=arm-linux-gnueabihf \
        --prefix="$SODIUM_PREFIX" \
        --disable-shared \
        --enable-static \
        --disable-asm \
        CC="$CROSS_CC" \
        AR="$CROSS_AR" \
        RANLIB="$CROSS_RANLIB" \
        CFLAGS="-Os -fPIC"

    make -j"$(nproc)"
    make install
    echo "libsodium installed to $SODIUM_PREFIX"
else
    echo "=== libsodium already built ==="
fi

# ── Step 2: Clone wfb-ng ─────────────────────────────────────────────

WFB_DIR="$BUILD_DIR/wfb-ng"

if [ ! -d "$WFB_DIR" ]; then
    echo "=== Cloning wfb-ng ==="
    git clone --depth 1 https://github.com/svpcom/wfb-ng.git "$WFB_DIR"
else
    echo "=== wfb-ng already cloned ==="
fi

# ── Step 3: Copy venc_ring files + create pcap stub ──────────────────

echo "=== Copying venc_ring files ==="
cp "$VENC_ROOT/include/venc_ring.h" "$WFB_DIR/src/venc_ring.h"
cp "$VENC_ROOT/src/venc_ring.c" "$WFB_DIR/src/venc_ring.c"

# Stub pcap.h — wifibroadcast.hpp includes it but TX doesn't use pcap
mkdir -p "$WFB_DIR/src/stub"
cat > "$WFB_DIR/src/stub/pcap.h" << 'STUB'
/* Stub: pcap.h not needed for wfb_tx (only wfb_rx uses libpcap) */
#ifndef PCAP_H_STUB
#define PCAP_H_STUB
#endif
STUB

# ── Step 4: Apply SHM input patch ────────────────────────────────────

cd "$WFB_DIR"

if git apply --reverse --check "$SCRIPT_DIR/shm-input.patch" 2>/dev/null; then
    echo "=== Patch already applied ==="
elif git apply --check "$SCRIPT_DIR/shm-input.patch" 2>/dev/null; then
    echo "=== Applying SHM input patch ==="
    git apply "$SCRIPT_DIR/shm-input.patch"
    echo "Patch applied successfully."
else
    echo "ERROR: Existing wfb-ng tree is stale or dirty and does not match shm-input.patch."
    echo "Run ./build_wfb_tx.sh --clean and rebuild so src/tx.cpp is regenerated."
    exit 1
fi

# ── Step 5: Build wfb_tx + wfb_keygen ───────────────────────────────

echo "=== Building wfb_tx ==="

WFB_CFLAGS="-Wall -O2 -fno-strict-aliasing -I$SODIUM_PREFIX/include -I$VENC_ROOT/include -I$WFB_DIR/src/stub"
WFB_CFLAGS="$WFB_CFLAGS -DZFEX_UNROLL_ADDMUL_SIMD=8 -DZFEX_INLINE_ADDMUL -DZFEX_INLINE_ADDMUL_SIMD"
WFB_CFLAGS="$WFB_CFLAGS -DWFB_VERSION='\"shm-patched\"'"
WFB_LDFLAGS="-L$SODIUM_PREFIX/lib -lrt -lsodium -static"

cd "$WFB_DIR"

echo "  Compiling tx.cpp..."
$CROSS_CXX $WFB_CFLAGS -std=gnu++11 -c -o src/tx.o src/tx.cpp

echo "  Compiling zfex.c..."
$CROSS_CC $WFB_CFLAGS -std=gnu99 -c -o src/zfex.o src/zfex.c

echo "  Compiling wifibroadcast.cpp..."
$CROSS_CXX $WFB_CFLAGS -std=gnu++11 -c -o src/wifibroadcast.o src/wifibroadcast.cpp

echo "  Compiling venc_ring.c..."
$CROSS_CC $WFB_CFLAGS -std=gnu99 -c -o src/venc_ring.o src/venc_ring.c

echo "  Linking wfb_tx..."
$CROSS_CXX -o wfb_tx src/tx.o src/zfex.o src/wifibroadcast.o src/venc_ring.o $WFB_LDFLAGS

echo "  Stripping..."
$CROSS_STRIP wfb_tx
cp wfb_tx "$BUILD_DIR/wfb_tx"

echo "  Building wfb_keygen..."
$CROSS_CC $WFB_CFLAGS -std=gnu99 -c -o src/keygen.o src/keygen.c
$CROSS_CC -o wfb_keygen src/keygen.o $WFB_LDFLAGS
$CROSS_STRIP wfb_keygen
cp wfb_keygen "$BUILD_DIR/wfb_keygen"

echo "  Building wfb_tx_cmd..."
$CROSS_CC $WFB_CFLAGS -std=gnu99 -c -o src/tx_cmd.o src/tx_cmd.c
$CROSS_CC -o wfb_tx_cmd src/tx_cmd.o $WFB_LDFLAGS
$CROSS_STRIP wfb_tx_cmd
cp wfb_tx_cmd "$BUILD_DIR/wfb_tx_cmd"

# ── Native wfb_rx (x86_64) for the ground station laptop ────────────
# wfb_rx needs real libpcap (not the stub used by the cross build of
# wfb_tx) so we compile it natively with the host toolchain and system
# libpcap/libsodium.  Drop it onto the ground-station laptop via:
#   cp build/wfb_rx_native /home/snokvist/dev/wfb-ng/wfb_rx
# (or wherever you run wfb_rx from).
echo "  Building wfb_rx (native x86_64)..."
NATIVE_BUILD="$BUILD_DIR/native"
mkdir -p "$NATIVE_BUILD"
NATIVE_CFLAGS="-Wall -O2 -fno-strict-aliasing -I$VENC_ROOT/include"
NATIVE_CFLAGS="$NATIVE_CFLAGS -DZFEX_UNROLL_ADDMUL_SIMD=8 -DZFEX_INLINE_ADDMUL -DZFEX_INLINE_ADDMUL_SIMD"
NATIVE_CFLAGS="$NATIVE_CFLAGS -DWFB_VERSION='\"shm-patched-native\"'"
g++ $NATIVE_CFLAGS -std=gnu++11 -c -o "$NATIVE_BUILD/rx_native.o" src/rx.cpp
gcc $NATIVE_CFLAGS -std=gnu99 -c -o "$NATIVE_BUILD/zfex_native.o" src/zfex.c
g++ $NATIVE_CFLAGS -std=gnu++11 -c -o "$NATIVE_BUILD/wfb_native.o" src/wifibroadcast.cpp
gcc $NATIVE_CFLAGS -std=gnu99 -c -o "$NATIVE_BUILD/radiotap_native.o" src/radiotap.c
g++ -o "$BUILD_DIR/wfb_rx_native" \
    "$NATIVE_BUILD/rx_native.o" \
    "$NATIVE_BUILD/zfex_native.o" \
    "$NATIVE_BUILD/wfb_native.o" \
    "$NATIVE_BUILD/radiotap_native.o" \
    -lrt -lsodium -lpcap
strip "$BUILD_DIR/wfb_rx_native"

# ── Step 6: Build SHM diagnostic tools ───────────────────────────────

echo "=== Building SHM tools ==="

TOOL_CFLAGS="-Os -Wall -I$VENC_ROOT/include"
TOOL_LDFLAGS="-lrt -lpthread"

echo "  Building shm_ring_stats..."
$CROSS_CC $TOOL_CFLAGS -o "$BUILD_DIR/shm_ring_stats" \
    "$VENC_ROOT/tools/shm_ring_stats.c" \
    "$VENC_ROOT/src/venc_ring.c" \
    $TOOL_LDFLAGS
$CROSS_STRIP "$BUILD_DIR/shm_ring_stats"

echo "  Building shm_consumer_test..."
$CROSS_CC $TOOL_CFLAGS -o "$BUILD_DIR/shm_consumer_test" \
    "$VENC_ROOT/tools/shm_consumer_test.c" \
    "$VENC_ROOT/src/venc_ring.c" \
    $TOOL_LDFLAGS
$CROSS_STRIP "$BUILD_DIR/shm_consumer_test"

# ── Summary ──────────────────────────────────────────────────────────

echo ""
echo "=== Build complete ==="
echo ""
ls -lh "$BUILD_DIR/wfb_tx" "$BUILD_DIR/wfb_keygen" "$BUILD_DIR/wfb_tx_cmd" \
       "$BUILD_DIR/wfb_rx_native" \
       "$BUILD_DIR/shm_ring_stats" "$BUILD_DIR/shm_consumer_test"
echo ""

# ── Deploy ───────────────────────────────────────────────────────────

if [ "$DO_DEPLOY" = "1" ]; then
    echo "=== Deploying to root@${DEPLOY_HOST}:${DEPLOY_DIR} ==="
    scp -O \
        "$BUILD_DIR/wfb_tx" \
        "$BUILD_DIR/wfb_keygen" \
        "$BUILD_DIR/wfb_tx_cmd" \
        "$BUILD_DIR/shm_ring_stats" \
        "$BUILD_DIR/shm_consumer_test" \
        "root@${DEPLOY_HOST}:${DEPLOY_DIR}/"
    echo "Deployed successfully."
    echo ""
    echo "On device:"
    echo "  # Quick test — verify binaries run:"
    echo "  wfb_tx --help"
    echo "  shm_ring_stats --help"
    echo ""
    echo "  # Full test — start venc with SHM, then consumer:"
    echo "  venc &                      # always reads /etc/venc.json (set server = shm://venc_wfb)"
    echo "  sleep 1"
    echo "  shm_ring_stats venc_wfb     # check ring created"
    echo "  shm_consumer_test venc_wfb 5 # read 5 seconds of packets"
    echo "  wfb_tx -H venc_wfb -K /etc/drone.key -k 8 -n 12 -p 0 wlan0"
else
    echo "To deploy:  ./build_wfb_tx.sh --deploy"
    echo "  or:       DEPLOY_HOST=192.168.1.10 ./build_wfb_tx.sh --deploy"
    echo ""
    echo "Manual deploy (vehicle):"
    echo "  scp -O $BUILD_DIR/{wfb_tx,wfb_tx_cmd,wfb_keygen,shm_ring_stats,shm_consumer_test} root@$DEPLOY_HOST:$DEPLOY_DIR/"
    echo ""
    echo "Manual deploy (ground-station x86_64 native wfb_rx):"
    echo "  cp $BUILD_DIR/wfb_rx_native /wherever/you/run/wfb_rx"
fi
