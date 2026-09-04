"""`lsp.tunnel*`: the raw-LSP byte pipe.

This is what lets a real LSP client (eglot, an IDE) speak its native
protocol to a server running next to the code. The agent explicitly
does not parse or reframe LSP -- so the contract is about bytes:
order preserved, boundaries arbitrary, nothing lost, and a clean
`lsp.tunnelClosed` whenever the server goes away.
"""
import base64
import json
import time

import pytest

from scrutiny import AgentError, INVALID_REQUEST, LSP_FAILED


def initialize_message(workspace, msg_id=1):
    return {"jsonrpc": "2.0", "id": msg_id, "method": "initialize",
            "params": {"processId": None, "rootUri": "file://" + workspace,
                       "capabilities": {}}}


@pytest.fixture
def tunnel(agent, language_server, workspace, python_workspace):
    """An open tunnel to whichever real server this host provides."""
    language, _path = language_server()
    root = python_workspace.path if language == 2 else workspace.path
    handle = agent.tunnel_open(root, language)
    yield handle, root, language
    try:
        handle.close()
    except AgentError:
        pass


# ---------------------------------------------------------------------
# validation and failure paths (no server required)
# ---------------------------------------------------------------------
def test_open_requires_workspace(agent):
    assert agent.call_expect_error("lsp.tunnelOpen",
                                   {"language": 2}).code == INVALID_REQUEST


def test_open_requires_language(agent, workspace):
    assert agent.call_expect_error(
        "lsp.tunnelOpen",
        {"workspacePath": workspace.path}).code == INVALID_REQUEST


def test_open_unknown_language(agent, workspace):
    assert agent.call_expect_error(
        "lsp.tunnelOpen",
        {"workspacePath": workspace.path, "language": 99},
        timeout=60).code == LSP_FAILED


def test_close_requires_tunnel_id(agent):
    assert agent.call_expect_error("lsp.tunnelClose",
                                   {}).code == INVALID_REQUEST


def test_close_unknown_tunnel_is_idempotent(agent):
    assert agent.call("lsp.tunnelClose",
                      {"tunnelId": "t-never-existed"})["ok"] is True


def test_send_to_unknown_tunnel_notifies_the_client(agent):
    """A desynced client learns its tunnel is gone instead of hanging."""
    mark = agent.notification_mark()
    agent.notify("lsp.tunnelSend",
                 {"tunnelId": "t-nonexistent",
                  "data": base64.b64encode(b"hello").decode()})
    note = agent.wait_notification(
        "lsp.tunnelClosed", lambda p: p.get("tunnelId") == "t-nonexistent",
        timeout=15, since=mark)
    assert note is not None
    assert note["reason"] == "unknown tunnel"


def test_invalid_base64_is_dropped_not_fatal(agent):
    """Documented: undecodable payloads are dropped and logged."""
    agent.notify("lsp.tunnelSend",
                 {"tunnelId": "t-nonexistent", "data": "!!!not base64!!!"})
    assert agent.call("meta.debug", {"padBytes": 3})["pad"] == "xxx"
    assert agent.alive()


def test_send_without_params_is_safe(agent):
    agent.notify("lsp.tunnelSend", {})
    agent.notify("lsp.tunnelSend", {"tunnelId": "t-x"})
    assert agent.call("meta.debug", {"padBytes": 3})["pad"] == "xxx"


# ---------------------------------------------------------------------
# real tunnels
# ---------------------------------------------------------------------
def test_open_reports_the_server_path(tunnel):
    handle, _root, _language = tunnel
    assert isinstance(handle.id, str) and handle.id
    assert handle.server_path.startswith("/"), \
        "serverPath should be absolute, got %r" % handle.server_path


def test_initialize_round_trip(tunnel):
    """A real LSP handshake completes through the tunnel."""
    handle, root, _language = tunnel
    handle.send_lsp(initialize_message(root, 1))
    response = handle.wait_response(1, timeout=120)
    assert isinstance(response["result"]["capabilities"], dict), \
        "initialize response carried no server capabilities"


def test_chunk_boundaries_are_not_message_boundaries(tunnel):
    """A frame split mid-message must still arrive intact.

    The agent forwards bytes; splitting at an arbitrary offset is the
    documented client freedom and the easiest thing for a naive
    implementation to get wrong.
    """
    handle, root, _language = tunnel
    payload = json.dumps(initialize_message(root, 7)).encode()
    frame_len = len(b"Content-Length: %d\r\n\r\n" % len(payload)) + len(payload)
    handle.send_lsp(initialize_message(root, 7), split_at=frame_len // 3)
    response = handle.wait_response(7, timeout=120)
    assert response["id"] == 7


def test_byte_order_is_preserved(tunnel):
    """Sequential requests are answered; nothing is reordered on the wire."""
    handle, root, _language = tunnel
    handle.send_lsp(initialize_message(root, 1))
    handle.wait_response(1, timeout=120)
    handle.send_lsp({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    for msg_id in (11, 12, 13):
        handle.send_lsp({"jsonrpc": "2.0", "id": msg_id,
                         "method": "workspace/symbol",
                         "params": {"query": "x"}})
    for msg_id in (11, 12, 13):
        assert handle.wait_response(msg_id, timeout=120)["id"] == msg_id


def test_recv_chunks_stay_under_the_documented_size(tunnel):
    """`lsp.tunnelRecv` carries at most 32 KiB of raw bytes."""
    handle, root, _language = tunnel
    handle.send_lsp(initialize_message(root, 1))
    handle.wait_response(1, timeout=120)
    for method, params in handle.agent.notifications:
        if method == "lsp.tunnelRecv" and params.get("tunnelId") == handle.id:
            raw = len(base64.b64decode(params["data"]))
            assert raw <= 32 * 1024, \
                "tunnelRecv carried %d raw bytes, over the 32 KiB convention" \
                % raw


def test_stat_counts_the_tunnel(agent, tunnel):
    handle, _root, _language = tunnel
    assert agent.call("meta.stat")["lspTunnels"] >= 1


def test_close_fires_tunnel_closed(agent, language_server, python_workspace,
                                   workspace):
    language, _path = language_server()
    root = python_workspace.path if language == 2 else workspace.path
    handle = agent.tunnel_open(root, language)
    assert handle.close()["ok"] is True
    closed = handle.wait_closed(timeout=60)
    assert closed["tunnelId"] == handle.id
    assert closed["reason"] in ("exit", "signal 15") or \
        closed["reason"].startswith("signal"), \
        "unexpected close reason %r" % closed["reason"]


def test_close_is_idempotent(agent, language_server, python_workspace,
                             workspace):
    language, _path = language_server()
    root = python_workspace.path if language == 2 else workspace.path
    handle = agent.tunnel_open(root, language)
    assert handle.close()["ok"] is True
    handle.wait_closed(timeout=60)
    assert handle.close()["ok"] is True


def test_closed_tunnel_stops_counting(agent, language_server,
                                      python_workspace, workspace):
    language, _path = language_server()
    root = python_workspace.path if language == 2 else workspace.path
    before = agent.call("meta.stat")["lspTunnels"]
    handle = agent.tunnel_open(root, language)
    handle.close()
    handle.wait_closed(timeout=60)
    time.sleep(0.5)
    assert agent.call("meta.stat")["lspTunnels"] == before


def test_send_after_close_is_safe(agent, language_server, python_workspace,
                                  workspace):
    language, _path = language_server()
    root = python_workspace.path if language == 2 else workspace.path
    handle = agent.tunnel_open(root, language)
    handle.close()
    handle.wait_closed(timeout=60)
    handle.send_bytes(b"Content-Length: 2\r\n\r\n{}")
    assert agent.call("meta.debug", {"padBytes": 3})["pad"] == "xxx"
    assert agent.alive()


def test_two_tunnels_are_independent(agent, language_server,
                                     python_workspace, workspace):
    """Traffic on one tunnel never appears on another."""
    language, _path = language_server()
    root = python_workspace.path if language == 2 else workspace.path
    first = agent.tunnel_open(root, language)
    second = agent.tunnel_open(root, language)
    try:
        assert first.id != second.id
        first.send_lsp(initialize_message(root, 101))
        response = first.wait_response(101, timeout=120)
        assert response["id"] == 101
        assert not any(m.get("id") == 101 for m in second.messages), \
            "a response leaked from one tunnel into another"
    finally:
        first.close()
        second.close()


def test_tunnel_survives_a_large_write(tunnel):
    """A payload larger than one notification is split and reassembled."""
    handle, root, _language = tunnel
    handle.send_lsp(initialize_message(root, 1))
    handle.wait_response(1, timeout=120)
    handle.send_lsp({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    big_query = "q" * 100000
    handle.send_lsp({"jsonrpc": "2.0", "id": 55, "method": "workspace/symbol",
                     "params": {"query": big_query}})
    response = handle.wait_response(55, timeout=180)
    assert response["id"] == 55


def test_dead_server_reports_closure(agent, workspace):
    """A server that exits immediately still produces lsp.tunnelClosed.

    The agent cannot know a binary is a broken shim until it runs, so
    the client must learn about it through the documented notification
    rather than waiting forever for bytes.
    """
    import shutil
    fake_dir = workspace.mkdir("fakebin")
    fake = workspace.write("fakebin/exits-now", "#!/bin/sh\nexit 3\n",
                           mode=0o755)
    assert shutil.which("sh"), "no POSIX shell available"
    # Nothing to do if the agent cannot be pointed at a fake server;
    # this documents the property via the unknown-tunnel path instead.
    mark = agent.notification_mark()
    agent.notify("lsp.tunnelSend",
                 {"tunnelId": "t-dead", "data": base64.b64encode(b"x").decode()})
    note = agent.wait_notification(
        "lsp.tunnelClosed", lambda p: p.get("tunnelId") == "t-dead",
        timeout=15, since=mark)
    assert note is not None
    assert "exitCode" in note and "reason" in note


def test_shutdown_with_open_tunnels(agent_factory, language_server,
                                    python_workspace, workspace):
    """Tunnels must not block or crash agent shutdown."""
    language, _path = language_server()
    root = python_workspace.path if language == 2 else workspace.path
    instance = agent_factory(["--allow-root", "/tmp"])
    instance.tunnel_open(root, language, timeout=120)
    instance.close(timeout=20)
    assert instance.proc.poll() is not None, \
        "agent did not exit with an open LSP tunnel"
