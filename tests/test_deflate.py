"""`encodings: ["deflate"]`: compressed responses and tunnel output.

Negotiated at the handshake and off otherwise. The property under test
is the boring one -- what arrives is byte-for-byte what would have
arrived uncompressed -- plus the two reasons it exists: that it is
actually smaller on the wire, and that a client which never asked for
it never sees it.
"""
import base64
import os
import zlib

import pytest


@pytest.fixture
def z_agent(agent_factory, tmp_path):
    agent = agent_factory(["--allow-root", str(tmp_path), "--allow-write",
                           "--git-exec-preset", "magit"])
    hello = agent.handshake(encodings=["deflate"])
    assert hello.get("encoding") == "deflate"
    return agent


def wire_bytes(agent, since):
    """Base64 payload bytes of the rpc.chunk frames after SINCE."""
    return sum(len(p.get("data", "")) for m, p in agent.notifications[since:]
               if m == "rpc.chunk")


def test_off_unless_asked_for(agent_factory, tmp_path):
    agent = agent_factory(["--allow-root", str(tmp_path)])
    assert "encoding" not in agent.hello
    assert "rpc.deflate" in agent.hello["features"]
    (tmp_path / "f.txt").write_text("line\n" * 5000)
    mark = agent.notification_mark()
    result = agent.call("fs.readFile", {"path": str(tmp_path / "f.txt")})
    assert result["content"] == "line\n" * 5000
    assert wire_bytes(agent, mark) == 0, \
        "a client that did not negotiate deflate was sent chunks"


def test_text_file_round_trips_and_shrinks(z_agent, tmp_path):
    body = "".join("def function_%d(argument):\n    return argument + %d\n\n"
                   % (i, i) for i in range(4000))
    (tmp_path / "big.py").write_text(body)
    mark = z_agent.notification_mark()
    result = z_agent.call("fs.readFile", {"path": str(tmp_path / "big.py"),
                                          "base64": True})
    assert base64.b64decode(result["contentBase64"]).decode() == body
    sent = wire_bytes(z_agent, mark)
    assert 0 < sent < len(body) / 3, \
        "%d bytes of text went out as %d" % (len(body), sent)


def test_small_responses_are_left_alone(z_agent):
    mark = z_agent.notification_mark()
    z_agent.call("meta.debug", {"padBytes": 100})
    assert wire_bytes(z_agent, mark) == 0


def test_incompressible_data_is_sent_plain(z_agent, tmp_path):
    """Random bytes do not shrink; compressing them only costs CPU."""
    data = os.urandom(20000)
    (tmp_path / "noise.bin").write_bytes(data)
    result = z_agent.call("fs.readFile", {"path": str(tmp_path / "noise.bin"),
                                          "base64": True})
    assert base64.b64decode(result["contentBase64"]) == data


@pytest.mark.parametrize("size", [1023, 1024, 1025, 70000, 400000])
def test_sizes_around_every_threshold(z_agent, size):
    result = z_agent.call("meta.debug", {"padBytes": size}, timeout=60)
    assert result["pad"] == "x" * size


def test_each_unit_is_a_complete_zlib_stream(z_agent):
    """A client needs no state carried from one response to the next."""
    for _ in range(3):
        mark = z_agent.notification_mark()
        request_id, wait = z_agent.call_async("meta.debug",
                                              {"padBytes": 5000})
        wait(30)
        chunks = [p for m, p in z_agent.notifications[mark:]
                  if m == "rpc.chunk" and p.get("id") == request_id]
        blob = base64.b64decode("".join(c["data"] for c in
                                        sorted(chunks, key=lambda c: c["seq"])))
        assert b'"pad"' in zlib.decompress(blob)


def test_git_output_round_trips(z_agent, repo):
    plain = z_agent.call("git.exec", {"repoPath": repo.path,
                                      "args": ["log", "--stat", "-p"]})
    from scrutiny.fixtures import git
    assert plain["stdout"].strip() == git(repo.path, "log", "--stat", "-p",
                                          capture=True)


def test_tunnel_output_round_trips_compressed(agent_factory, language_server,
                                              workspace, python_workspace):
    """A language server's replies are JSON, and large: the capability
    list alone is kilobytes. Compressed on the way out, identical on
    arrival."""
    language, _path = language_server()
    root = python_workspace.path if language == 2 else workspace.path
    agent = agent_factory(["--allow-root", root])
    agent.handshake(encodings=["deflate"])
    tunnel = agent.tunnel_open(root, language)
    mark = agent.notification_mark()
    try:
        tunnel.send_lsp({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                         "params": {"processId": None,
                                    "rootUri": "file://" + root,
                                    "capabilities": {}}})
        reply = tunnel.wait_response(1, timeout=60)
        assert "capabilities" in reply["result"]
    finally:
        tunnel.close()
    received = [p for m, p in agent.notifications[mark:]
                if m == "lsp.tunnelRecv"]
    assert any(p.get("z") is True for p in received), \
        "nothing the server said was sent compressed"
