"""`meta.hello`: version negotiation and frame-cap clamping.

The handshake decides whether the client can talk to this binary at
all, and fixes the frame cap for the connection. Getting the clamp
wrong silently breaks chunking for every large response afterwards.
"""
import pytest

from scrutiny import VERSION_MISMATCH

FLOOR = 64 * 1024
CEILING = 256 * 1024


def test_hello_reports_version_and_protocol(agent_factory, agent_version):
    instance = agent_factory(auto_hello=False)
    hello = instance.handshake()
    assert hello["protocolVersion"] == 1
    assert hello["agentVersion"] == agent_version


def test_hello_advertises_capabilities(agent):
    capabilities = agent.capabilities
    assert isinstance(capabilities, list) and capabilities
    assert len(set(capabilities)) == len(capabilities), \
        "capabilities list contains duplicates"
    for name in capabilities:
        assert isinstance(name, str) and "." in name, \
            "capability %r is not a <namespace>.<op> name" % name


def test_capabilities_cover_the_documented_namespaces(agent):
    """Every namespace docs/protocol.md defines is represented."""
    namespaces = {name.split(".", 1)[0] for name in agent.capabilities}
    for expected in ("meta", "git", "fs", "lsp", "index", "watch", "cred",
                     "diffcache", "logs"):
        assert expected in namespaces, \
            "no %s.* capability advertised" % expected


def test_unsupported_protocol_version_is_rejected(agent_factory):
    instance = agent_factory(auto_hello=False)
    error = instance.call_expect_error("meta.hello",
                                       {"clientVersion": "pytest",
                                        "supportedProtocolVersions": [99]})
    assert error.code == VERSION_MISMATCH


def test_empty_supported_versions_is_rejected(agent_factory):
    instance = agent_factory(auto_hello=False)
    error = instance.call_expect_error("meta.hello",
                                       {"clientVersion": "pytest",
                                        "supportedProtocolVersions": []})
    assert error.code is not None, \
        "an empty version list must not be treated as acceptable"


def test_client_offering_several_versions_negotiates_one(agent_factory):
    """A forward-looking client offering [1, 2] still connects at 1."""
    instance = agent_factory(auto_hello=False)
    hello = instance.handshake(versions=(1, 2))
    assert hello["protocolVersion"] == 1


@pytest.mark.parametrize("proposal,expected", [
    (131072, 131072),          # inside the range: honored
    (FLOOR, FLOOR),            # exactly the floor
    (CEILING, CEILING),        # exactly the ceiling
    (1024, FLOOR),             # below the floor: clamped up
    (0, FLOOR),
    (10 ** 9, CEILING),        # above the ceiling: clamped down
    (-5, FLOOR),               # nonsense: clamped, not crashed
])
def test_frame_cap_clamping(agent_factory, proposal, expected):
    instance = agent_factory(auto_hello=False)
    hello = instance.handshake(frame_cap=proposal)
    assert hello["frameCap"] == expected, \
        "frameCap %r should clamp to %d" % (proposal, expected)


def test_frame_cap_default_when_unproposed(agent_factory):
    """Omitting frameCap yields the documented 128 KiB default."""
    instance = agent_factory(auto_hello=False)
    hello = instance.handshake(frame_cap=None)
    assert hello["frameCap"] == 128 * 1024


def test_hello_without_client_version(agent_factory):
    """clientVersion is informational; its absence must not fail."""
    instance = agent_factory(auto_hello=False)
    hello = instance.call("meta.hello", {"supportedProtocolVersions": [1]})
    assert hello["protocolVersion"] == 1


def test_second_hello_is_accepted(agent_factory):
    """Re-handshaking on a live connection is not an error.

    A client that reconnects its own state (or an Emacs client
    re-running setup) must not have to tear down the transport.
    """
    instance = agent_factory(auto_hello=False)
    first = instance.handshake(frame_cap=131072)
    second = instance.handshake(frame_cap=131072)
    assert second["protocolVersion"] == first["protocolVersion"]
    assert second["agentVersion"] == first["agentVersion"]


def test_requests_before_hello(agent_factory):
    """Calling a method before the handshake is answered, not ignored.

    Whether the agent serves it or rejects it, the client must get a
    response -- a silently dropped request would hang every client.
    """
    instance = agent_factory(auto_hello=False)
    try:
        result = instance.call("meta.stat", timeout=10)
        assert isinstance(result, dict)
    except Exception as exc:                            # noqa: BLE001
        assert "timeout" not in str(exc).lower(), \
            "pre-handshake request was dropped instead of answered"
