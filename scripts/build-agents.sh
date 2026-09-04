#!/usr/bin/env bash
# Build the Linux agent binaries for BOTH architectures and install them
# where the Emacs client looks for them.
#
# Run this on your laptop -- macOS or Linux. The binaries are built
# inside a debian:11 image (glibc 2.31 floor, static C++ runtime), so
# they run on essentially any Linux server you connect to, and the
# client picks the right one per host from the arch it probes.
#
#   scripts/build-agents.sh                 # both arches, install for Emacs
#   scripts/build-agents.sh --arch arm64    # just one
#   scripts/build-agents.sh --no-install    # leave them in docker/dist only
#   AGENT_VERSION=0.3.0 scripts/build-agents.sh
#
# Cross-architecture builds need qemu registered with Docker; the script
# checks and tells you how rather than failing halfway through a
# twenty-minute compile.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_DIR="${SCRUTINY_AGENT_BIN_DIR:-$HOME/.emacs.d/scrutiny-agent}"
ARCHES="amd64,arm64"
DO_INSTALL=1

# Default to the version the tree declares, so the built file names match
# what the Emacs client asks for out of the box.
if [ -z "${AGENT_VERSION:-}" ]; then
    AGENT_VERSION="$(sed -n 's/^project(scrutiny-agent VERSION \([0-9.]*\).*/\1/p' \
        "${ROOT}/CMakeLists.txt")"
fi

while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCHES="$2"; shift 2 ;;
        --no-install) DO_INSTALL=0; shift ;;
        --install-dir) INSTALL_DIR="$2"; shift 2 ;;
        -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }

echo "==> Checking Docker"
command -v docker >/dev/null 2>&1 || die \
    "docker not found. Install Docker Desktop (macOS) or docker.io (Linux).
   The agent is a C++ binary for Linux; building it for a server from a
   laptop needs the toolchain a container provides."
docker buildx version >/dev/null 2>&1 || die \
    "docker buildx not available. It ships with Docker Desktop; on Linux
   install docker-buildx-plugin."
docker info >/dev/null 2>&1 || die "the Docker daemon is not running."

# Which platforms are we actually building?
PLATFORMS=""
for arch in ${ARCHES//,/ }; do
    case "${arch}" in
        amd64|x86_64) PLATFORMS="${PLATFORMS}${PLATFORMS:+,}linux/amd64" ;;
        arm64|aarch64) PLATFORMS="${PLATFORMS}${PLATFORMS:+,}linux/arm64" ;;
        *) die "unknown arch '${arch}' (use amd64 and/or arm64)" ;;
    esac
done

# A non-native platform is emulated, and the emulator has to be
# registered before buildx will accept the platform at all.
NATIVE="linux/amd64"
case "$(uname -m)" in
    arm64|aarch64) NATIVE="linux/arm64" ;;
esac
case ",${PLATFORMS}," in
    *","${NATIVE}","*|*",${NATIVE},"*) ;;
esac
for platform in ${PLATFORMS//,/ }; do
    if [ "${platform}" != "${NATIVE}" ]; then
        if ! docker buildx inspect --bootstrap 2>/dev/null \
             | grep -qi "${platform#linux/}"; then
            echo "    ${platform} is not native here and no emulator is registered."
            echo "    Register one with:"
            echo "        docker run --privileged --rm tonistiigi/binfmt --install all"
            echo "    ...or build only the native arch:"
            echo "        scripts/build-agents.sh --arch ${NATIVE#linux/}"
            die "cannot build ${platform}"
        fi
    fi
done
echo "    building ${PLATFORMS} (native: ${NATIVE})"

echo "==> Building scrutiny-agent ${AGENT_VERSION}"
echo "    first run compiles every dependency from source; expect a while"
AGENT_VERSION="${AGENT_VERSION}" PLATFORMS="${PLATFORMS}" \
    "${ROOT}/docker/build-agent.sh"

DIST="${ROOT}/docker/dist"
echo ""
echo "==> Built"
for f in "${DIST}"/scrutiny-agent-"${AGENT_VERSION}"-*; do
    case "${f}" in *.sha256) continue ;; esac
    [ -f "${f}" ] || continue
    printf '    %-52s %s\n' "$(basename "${f}")" \
        "$(du -h "${f}" | cut -f1)"
done

if [ "${DO_INSTALL}" -eq 1 ]; then
    echo ""
    echo "==> Installing into ${INSTALL_DIR}"
    mkdir -p "${INSTALL_DIR}"
    for f in "${DIST}"/scrutiny-agent-"${AGENT_VERSION}"-*; do
        [ -f "${f}" ] || continue
        cp "${f}" "${INSTALL_DIR}/"
    done
    chmod +x "${INSTALL_DIR}"/scrutiny-agent-"${AGENT_VERSION}"-* 2>/dev/null || true
    ls -1 "${INSTALL_DIR}" | sed 's/^/    /'
    cat <<EOF

The Emacs client finds these by name and picks the one matching each
host's architecture. Nothing further to configure, as long as

    (setq scrutiny-agent-binary-version "${AGENT_VERSION}")

matches (it is the default when the tree's version is ${AGENT_VERSION}).
EOF
fi
