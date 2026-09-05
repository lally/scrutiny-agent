#!/usr/bin/env bash
# Host build: Conan deps + CMake configure + build, into ./build.
# Works on Linux and macOS with CMake >= 3.25, a C++23 compiler, and
# Conan 2. Usage: scripts/build-host.sh [Debug|Release]
set -euo pipefail

BUILD_TYPE="${1:-Release}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

conan install "$ROOT" --output-folder="$ROOT/build" --build=missing \
    -s build_type="$BUILD_TYPE" -s compiler.cppstd=23

cmake -S "$ROOT" -B "$ROOT/build" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/build/conan_toolchain.cmake"

if command -v nproc >/dev/null 2>&1; then J="$(nproc)"; else J="$(sysctl -n hw.ncpu)"; fi
cmake --build "$ROOT/build" -j"$J"

echo ""
echo "agent:  $ROOT/build/agent/scrutiny-agent"
echo "tests:  ctest --test-dir $ROOT/build --output-on-failure"
echo "wire:   python3 $ROOT/tests/conformance/conformance.py $ROOT/build/agent/scrutiny-agent"
