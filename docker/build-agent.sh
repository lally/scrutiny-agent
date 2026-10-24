#!/usr/bin/env bash
# Build the shippable, portable Linux scrutiny-agent binaries.
#
# Produces, per platform in ${PLATFORMS} (default both):
#   dist/scrutiny-agent-<version>-<arch>         arch = uname -m name
#   dist/scrutiny-agent-<version>-<arch>.sha256  (x86_64 / aarch64)
#
# The .sha256 is exactly what the Scrutiny bootstrap hash-gate compares
# against, and the binary name is exactly what the bootstrap expects at
# <installDir>/scrutiny-agent-<version>-<arch>.
#
# Requires Docker with buildx. Building the non-native platform needs
# binfmt/qemu; CI avoids that by running this once per native runner
# with PLATFORMS=linux/amd64 or PLATFORMS=linux/arm64.
set -euo pipefail

AGENT_VERSION="${AGENT_VERSION:-0.1.0}"
PLATFORMS="${PLATFORMS:-linux/amd64,linux/arm64}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CTX="${CTX:-$(cd "${SCRIPT_DIR}/.." && pwd)}"
OUT="${OUT:-${SCRIPT_DIR}/out}"
DIST="${DIST:-${SCRIPT_DIR}/dist}"

rm -rf "${OUT}"
mkdir -p "${DIST}"

# One buildx invocation PER platform, each writing into its own
# ${OUT}/linux_<arch>/. A single multi-platform invocation is refused by
# Docker Desktop's default `docker` driver ("Multi-platform build is
# not supported for the docker driver. Switch to a different driver, or
# turn on the containerd image store") and would otherwise require
# every developer to reconfigure their engine; single-platform builds
# work on every driver, and the layer cache is shared between the two
# runs. --target export is the scratch stage holding just the binary.
# NB: no bash-4 `declare -A` -- macOS's stock /bin/bash is 3.2.
for pair in linux/amd64:linux_amd64:x86_64 linux/arm64:linux_arm64:aarch64; do
    plat="${pair%%:*}"
    rest="${pair#*:}"
    subdir="${rest%%:*}"
    case ",${PLATFORMS}," in
        *",${plat},"*) ;;
        *) continue ;;
    esac
    echo "building scrutiny-agent ${AGENT_VERSION} for ${plat}"
    docker buildx build \
        --platform "${plat}" \
        --build-arg "AGENT_VERSION=${AGENT_VERSION}" \
        -f "${SCRIPT_DIR}/Dockerfile" \
        --target export \
        --output "type=local,dest=${OUT}/${subdir}" \
        "${CTX}"
done

# Rename to the bootstrap convention and emit sha256 sidecars.
for pair in linux/amd64:linux_amd64:x86_64 linux/arm64:linux_arm64:aarch64; do
    plat="${pair%%:*}"
    rest="${pair#*:}"
    subdir="${rest%%:*}"
    arch="${rest##*:}"
    case ",${PLATFORMS}," in
        *",${plat},"*) ;;
        *) continue ;;
    esac
    src="${OUT}/${subdir}/scrutiny-agent"
    [ -f "${src}" ] || { echo "error: missing build output for ${plat}" >&2; exit 1; }
    dst="${DIST}/scrutiny-agent-${AGENT_VERSION}-${arch}"
    cp "${src}" "${dst}"
    chmod +x "${dst}"
    if command -v sha256sum >/dev/null 2>&1; then
        ( cd "${DIST}" && sha256sum "$(basename "${dst}")" | awk '{print $1}' > "${dst}.sha256" )
    else
        ( cd "${DIST}" && shasum -a 256 "$(basename "${dst}")" | awk '{print $1}' > "${dst}.sha256" )
    fi
    echo "built ${dst}  sha256=$(cat "${dst}.sha256")"
done

echo "done -> ${DIST}"
