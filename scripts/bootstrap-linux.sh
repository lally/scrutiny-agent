#!/usr/bin/env bash
# One-command Linux build: check the toolchain, get Conan, build.
#
# Everything the build needs beyond a C++23 compiler and CMake comes
# from Conan, so there are NO -dev packages to install and nothing here
# needs root. Conan itself goes into a private venv under ./.build-venv
# if it is not already on PATH.
#
# Usage: scripts/bootstrap-linux.sh [Debug|Release]
set -euo pipefail

BUILD_TYPE="${1:-Release}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="${ROOT}/.build-venv"

die() { echo "error: $*" >&2; exit 1; }

echo "==> Checking the toolchain"

command -v cmake >/dev/null 2>&1 || die \
    "cmake not found. Install CMake >= 3.25 (apt install cmake)."
cmake_version="$(cmake --version | head -1 | awk '{print $3}')"
cmake_major="${cmake_version%%.*}"
cmake_rest="${cmake_version#*.}"
cmake_minor="${cmake_rest%%.*}"
if [ "${cmake_major}" -lt 3 ] || \
   { [ "${cmake_major}" -eq 3 ] && [ "${cmake_minor}" -lt 25 ]; }; then
    die "cmake ${cmake_version} is too old; 3.25+ is required."
fi
echo "    cmake ${cmake_version}"

if [ -n "${CXX:-}" ]; then
    compiler="${CXX}"
elif command -v g++ >/dev/null 2>&1; then
    compiler="g++"
elif command -v clang++ >/dev/null 2>&1; then
    compiler="clang++"
else
    die "no C++ compiler found. Install g++ >= 13 or clang++ >= 16."
fi
echo "    $("${compiler}" --version | head -1)"

# gitmanip is C++23; a compiler that cannot parse the dialect fails
# thousands of lines later with something unhelpful, so check up front.
probe="$(mktemp -d)"
trap 'rm -rf "${probe}"' EXIT
cat > "${probe}/probe.cpp" <<'EOF'
#include <expected>
#include <string>
int main() { std::expected<int, std::string> e{1}; return *e - 1; }
EOF
"${compiler}" -std=c++23 -o "${probe}/probe" "${probe}/probe.cpp" 2>/dev/null \
    || die "${compiler} cannot compile C++23. Install g++ >= 13 or clang++ >= 16."
echo "    C++23 OK"

command -v python3 >/dev/null 2>&1 || die "python3 not found."
command -v git >/dev/null 2>&1 || die "git not found."

echo "==> Locating Conan 2"
if command -v conan >/dev/null 2>&1; then
    CONAN="conan"
elif [ -x "${VENV}/bin/conan" ]; then
    CONAN="${VENV}/bin/conan"
else
    echo "    not found; installing into ${VENV}"
    python3 -m venv "${VENV}" 2>/dev/null || die \
        "could not create a venv. Install python3-venv (apt install python3-venv)."
    "${VENV}/bin/pip" install --quiet --upgrade pip
    "${VENV}/bin/pip" install --quiet "conan>=2,<3"
    CONAN="${VENV}/bin/conan"
fi
echo "    $("${CONAN}" --version)"

if ! "${CONAN}" profile path default >/dev/null 2>&1; then
    echo "==> Creating the default Conan profile"
    "${CONAN}" profile detect --force
fi

echo "==> Building (${BUILD_TYPE}); first run compiles dependencies from source"
"${CONAN}" install "${ROOT}" --output-folder="${ROOT}/build" --build=missing \
    -s build_type="${BUILD_TYPE}" -s compiler.cppstd=23

cmake -S "${ROOT}" -B "${ROOT}/build" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT}/build/conan_toolchain.cmake"

cmake --build "${ROOT}/build" -j"$(nproc)"

cat <<EOF

Built: ${ROOT}/build/agent/scrutiny-agent
       $("${ROOT}/build/agent/scrutiny-agent" --version)

Next:
  ctest --test-dir ${ROOT}/build --output-on-failure     # C++ unit tests
  ${ROOT}/tests/run-tests.sh                             # Python test suite
EOF
