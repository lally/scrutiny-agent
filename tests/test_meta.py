"""`meta.*` and `logs.tail` -- the agent's self-description.

These are what an operator (and the Emacs client's status command)
reads to know what the running binary is, what it may touch, and what
it is doing right now.
"""
import os

import pytest


def test_stat_shape(agent, agent_version):
    stat = agent.call("meta.stat")
    for key in ("lanes", "lspSessions", "lspTunnels", "uptimeMs",
                "gitParallel", "logLevel", "agentVersion"):
        assert key in stat, "meta.stat missing %r" % key
    assert stat["agentVersion"] == agent_version


def test_stat_lanes(agent):
    """Each lane reports its live `active` / `queued` occupancy."""
    lanes = agent.call("meta.stat")["lanes"]
    assert set(lanes) >= {"interactive", "normal", "bulk"}
    for name, lane in lanes.items():
        assert set(lane) >= {"active", "queued"}, \
            "lane %s has an unexpected shape: %r" % (name, lane)
        for key, value in lane.items():
            assert isinstance(value, int) and value >= 0, \
                "lane %s.%s is nonsensical: %r" % (name, key, value)


def test_stat_counts_the_in_flight_request(agent):
    """meta.stat itself occupies the interactive lane while it runs."""
    lanes = agent.call("meta.stat")["lanes"]
    assert lanes["interactive"]["active"] >= 1, \
        "the in-flight meta.stat is not reflected in its own lane"


def test_uptime_advances(agent):
    first = agent.call("meta.stat")["uptimeMs"]
    agent.call("meta.debug", {"sleepMs": 250})
    second = agent.call("meta.stat")["uptimeMs"]
    assert second > first, "uptimeMs did not advance across 250ms"


def test_stat_counts_open_tunnels(agent, language_server, workspace):
    language, _path = language_server()
    before = agent.call("meta.stat")["lspTunnels"]
    tunnel = agent.tunnel_open(workspace.path, language)
    try:
        assert agent.call("meta.stat")["lspTunnels"] == before + 1
    finally:
        tunnel.close()
        tunnel.wait_closed(timeout=30)
    assert agent.call("meta.stat")["lspTunnels"] == before


def test_capabilities_posture(agent, agent_version):
    caps = agent.call("meta.capabilities")
    assert caps["agentVersion"] == agent_version
    assert caps["protocolVersion"] == 1
    assert isinstance(caps["outboundIO"], list) and caps["outboundIO"]
    assert isinstance(caps["fileSystemAccess"], dict)
    assert "description" in caps["fileSystemAccess"]
    assert caps["selftestMethod"] == "fs.selftest"
    assert isinstance(caps["languageServers"], dict)


def test_capabilities_report_the_configured_roots(agent_factory, tmp_path):
    """The posture report reflects this process's actual --allow-root set."""
    root = tmp_path / "only-here"
    root.mkdir()
    instance = agent_factory(["--allow-root", str(root)])
    posture = instance.call("meta.capabilities")["fileSystemAccess"]
    assert posture["allowedRoots"] == [str(root)], \
        "fileSystemAccess should report the running process's roots, got %r" \
        % posture.get("allowedRoots")


def test_capabilities_roots_default_to_home(agent_factory):
    """With no --allow-root, the reported root is $HOME."""
    instance = agent_factory([])
    roots = instance.call("meta.capabilities")["fileSystemAccess"]["allowedRoots"]
    assert roots == [os.path.realpath(os.path.expanduser("~"))]


def test_capabilities_is_side_effect_free(agent):
    first = agent.call("meta.capabilities")
    second = agent.call("meta.capabilities")
    assert first == second


def test_debug_echoes_request_id(agent):
    request_id, wait = agent.call_async("meta.debug", {"padBytes": 8})
    result = wait()
    assert result["echoId"] == request_id
    assert result["pad"] == "x" * 8


def test_debug_zero_pad(agent):
    assert agent.call("meta.debug", {"padBytes": 0})["pad"] == ""


def test_debug_no_params(agent):
    result = agent.call("meta.debug")
    assert "echoId" in result and "pad" in result


def test_debug_sleep_is_observable(agent):
    import time
    start = time.monotonic()
    agent.call("meta.debug", {"sleepMs": 600})
    assert time.monotonic() - start >= 0.5, "sleepMs was not honored"


def test_debug_negative_pad_is_safe(agent):
    """A nonsense padBytes must not allocate wildly or crash."""
    try:
        result = agent.call("meta.debug", {"padBytes": -1})
        assert result["pad"] == ""
    except Exception as exc:                            # noqa: BLE001
        assert "timeout" not in str(exc).lower()
    assert agent.alive()


def test_logs_tail_reports_the_path(agent, session_log_path):
    tail = agent.call("logs.tail")
    assert tail["enabled"] is True
    assert tail["path"] == session_log_path
    assert isinstance(tail["text"], str)
    assert tail["bytes"] > 0


def test_logs_tail_respects_max_bytes(agent):
    agent.call("meta.stat")           # guarantee there is something to tail
    small = agent.call("logs.tail", {"maxBytes": 64})
    assert small["bytes"] <= 64
    assert len(small["text"].encode()) <= 64


def test_logs_tail_caps_at_one_mib(agent):
    """`maxBytes` above the documented 1 MiB ceiling is clamped."""
    tail = agent.call("logs.tail", {"maxBytes": 100 * 1024 * 1024})
    assert tail["bytes"] <= 1024 * 1024


def test_logs_tail_when_logging_is_off(agent_factory):
    instance = agent_factory([])       # no --log
    tail = instance.call("logs.tail")
    assert tail["enabled"] is False


def test_logs_tail_reflects_new_activity(agent):
    before = agent.call("logs.tail", {"maxBytes": 1024 * 1024})["bytes"]
    for _ in range(20):
        agent.call("meta.stat")
    after = agent.call("logs.tail", {"maxBytes": 1024 * 1024})["bytes"]
    assert after >= before, "log tail shrank while the agent was working"


def test_fs_selftest_shape(agent):
    result = agent.call("fs.selftest")
    assert result["probe"] == "/etc/passwd"
    assert isinstance(result["succeeded"], bool)
    assert "errorCode" in result
    assert "firstBytesReadable" in result
    assert "sampleSnippet" in result


def test_fs_selftest_reflects_the_sandbox(agent_factory, tmp_path):
    """With /etc outside the roots, the probe must report failure."""
    root = tmp_path / "narrow"
    root.mkdir()
    instance = agent_factory(["--allow-root", str(root)])
    result = instance.call("fs.selftest")
    assert result["succeeded"] is False, \
        "selftest claims /etc/passwd is readable from a sandbox that " \
        "excludes /etc"
