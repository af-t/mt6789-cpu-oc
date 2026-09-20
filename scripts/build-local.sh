#!/bin/sh
# Lightweight cpu_oc_mt6789.ko build (same pattern as gpu build-local.sh).
# Vanilla 6.12.38 + Wild Bypass + insmod -f on device if vermagic mismatches.
set -eu
VER=6.12.38
WORK=${WORK:-$HOME/cpu-oc-build}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$WORK"; cd "$WORK"

if [ ! -d "linux-$VER" ]; then
  echo "[1/4] download linux-$VER.tar.xz ..."
  curl -L -o "linux-$VER.tar.xz" "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$VER.tar.xz"
  echo "[2/4] extract ..."
  tar xf "linux-$VER.tar.xz"
fi
cd "linux-$VER"

if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  export CROSS_COMPILE=aarch64-linux-gnu-
  export LLVM=0
  echo "[*] using gcc cross aarch64"
else
  export LLVM=1
  echo "[*] using clang (LLVM=1)"
fi

if [ ! -f .config ]; then
  echo "[3/4] defconfig + modules_prepare ..."
  make ARCH=arm64 ${LLVM:+LLVM=1} defconfig
  make ARCH=arm64 ${LLVM:+LLVM=1} -j"$(nproc)" modules_prepare
fi

echo "[4/4] building module ..."
make -C "$WORK/linux-$VER" ARCH=arm64 ${LLVM:+LLVM=1} \
  ${CROSS_COMPILE:+CROSS_COMPILE=$CROSS_COMPILE} M="$ROOT" modules

echo "== result =="
file "$ROOT/src/cpu_oc_mt6789.ko"
modinfo "$ROOT/src/cpu_oc_mt6789.ko" | grep -E "vermagic|description" || true
