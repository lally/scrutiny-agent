"""The binary's command-line surface and startup behavior.

This is the contract the Scrutiny bootstrap and the Emacs client rely
on before a single frame is exchanged: `--version` output shape, exit
codes, and the fact that the process is a well-behaved stdio filter.
"""
import os
import subprocess

import pytest

from scrutiny.client import agent_version_of


def run(binary, *args, timeout=30, stdin=b""):
    return subprocess.run([binary, *args], input=stdin,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=timeout)


def test_version_flag_shape(agent_binary):
    """`--version` prints `<agentVersion> proto <protocolVersion>`."""
    proc = run(agent_binary, "--version")
    assert proc.returncode == 0, proc.stderr.decode()
    text = proc.stdout.decode().strip()
    parts = text.split()
    assert len(parts) == 3 and parts[1] == "proto", \
        "expected '<version> proto <n>', got %r" % text
    assert parts[2] == "1", "protocol version should be 1, got %r" % parts[2]


def test_version_matches_build_version(agent_version, project_version):
    """The reported version tracks the version the tree declares.

    Release builds override this with the git tag; a dev build that
    drifts from CMakeLists.txt means the bootstrap would install a
    binary under a name that does not match what it reports.
    """
    if project_version is None:
        pytest.skip("could not parse the project version from CMakeLists.txt")
    assert agent_version == project_version


def test_version_is_stable_across_invocations(agent_binary):
    first = agent_version_of(agent_binary)
    second = agent_version_of(agent_binary)
    assert first == second


def test_stdout_stays_clean_before_any_request(agent_binary):
    """Startup diagnostics must never land on stdout.

    stdout carries protocol frames only; anything else desynchronizes
    every client's framer. Closing stdin immediately should exit
    cleanly with no stdout output at all.
    """
    proc = run(agent_binary, "--rpc-stdio", stdin=b"", timeout=30)
    assert proc.stdout == b"", \
        "agent wrote non-frame bytes to stdout: %r" % proc.stdout[:400]


def test_exits_when_stdin_closes(agent_binary):
    """EOF on stdin ends the process -- the transport's hangup path."""
    proc = subprocess.Popen([agent_binary, "--rpc-stdio"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    proc.stdin.close()
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        pytest.fail("agent did not exit after stdin EOF")


def test_log_file_is_created_and_written(agent_factory, tmp_path):
    """`--log` writes to the given path, and only there."""
    log_path = tmp_path / "explicit.log"
    instance = agent_factory(["--log", str(log_path), "--log-level", "info"])
    instance.call("meta.stat")
    assert log_path.exists(), "--log did not create the log file"
    assert log_path.stat().st_size > 0, "--log file is empty after a request"


def test_unknown_flag_does_not_crash_the_protocol(agent_binary,
                                                  agent_factory):
    """An unrecognized flag must not corrupt stdout framing.

    Either the agent rejects the flag and exits, or it ignores it and
    serves normally -- but it must never emit a diagnostic on stdout
    and then keep speaking the protocol.
    """
    proc = run(agent_binary, "--rpc-stdio", "--not-a-real-flag", stdin=b"",
               timeout=30)
    assert proc.stdout == b"", \
        "unknown flag produced stdout noise: %r" % proc.stdout[:400]


def test_allow_root_accepts_repeats(agent_factory, tmp_path):
    """`--allow-root` is repeatable; each root is honored."""
    first = tmp_path / "one"
    second = tmp_path / "two"
    first.mkdir()
    second.mkdir()
    (first / "a.txt").write_text("a\n")
    (second / "b.txt").write_text("b\n")
    instance = agent_factory(["--allow-root", str(first),
                              "--allow-root", str(second)])
    assert instance.call("fs.readFile",
                         {"path": str(first / "a.txt")})["content"] == "a\n"
    assert instance.call("fs.readFile",
                         {"path": str(second / "b.txt")})["content"] == "b\n"
