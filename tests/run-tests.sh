#!/usr/bin/env bash
# Run the Python test suite against a built agent.
#
# Creates a private venv under tests/.venv on first run and installs
# the runner plus pylsp (so the LSP, tunnel and indexer tests exercise
# a real language server instead of skipping). Nothing here needs root.
#
# Usage: tests/run-tests.sh [pytest args...]
#   tests/run-tests.sh                       # the whole suite
#   tests/run-tests.sh -k sandbox            # one area
#   tests/run-tests.sh --run-slow            # include the soak tests
#   SCRUTINY_AGENT_BIN=/path/to/agent tests/run-tests.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
VENV="${HERE}/.venv"
AGENT="${SCRUTINY_AGENT_BIN:-${ROOT}/build/agent/scrutiny-agent}"

if [ ! -x "${AGENT}" ]; then
    echo "error: no agent binary at ${AGENT}" >&2
    echo "Build one with scripts/bootstrap-linux.sh, or set SCRUTINY_AGENT_BIN." >&2
    exit 2
fi

if [ ! -x "${VENV}/bin/pytest" ]; then
    echo "==> Creating the test venv at ${VENV}"
    python3 -m venv "${VENV}"
    "${VENV}/bin/pip" install --quiet --upgrade pip
    "${VENV}/bin/pip" install --quiet -r "${HERE}/requirements.txt"
fi

# Default to the whole suite, but step aside when the caller names files
# or directories: passing both would make pytest load conftest.py twice
# under two roots (which it rejects outright).
targets=("${HERE}")
for arg in "$@"; do
    case "${arg}" in
        -*) ;;
        *) if [ -e "${arg}" ]; then targets=(); break; fi ;;
    esac
done

# The suite puts the interpreter's bin directory on PATH for the agents
# it spawns, so a venv-installed pylsp is found without activation.
exec "${VENV}/bin/python" -m pytest "${targets[@]}" \
    --agent-binary="${AGENT}" "$@"
