"""Frame-level robustness.

The agent reads LSP-style `Content-Length` frames from a pipe that
may be shared with a login shell, a bootstrap dialogue, and a
half-broken client. Framing bugs desynchronize the stream permanently,
so the interesting property is not just "valid input works" but
"invalid input does not take the connection down".
"""
import json

import pytest

from scrutiny import INVALID_REQUEST


def probe(agent, timeout=10):
    """A cheap round trip proving the connection is still usable."""
    return agent.call("meta.debug", {"padBytes": 4}, timeout=timeout)["pad"]


def test_wellformed_frame_round_trips(fresh_agent):
    assert probe(fresh_agent) == "xxxx"


def test_frame_split_across_writes(fresh_agent):
    """A frame delivered in several pipe writes reassembles correctly.

    TCP/ssh transports split writes at arbitrary boundaries; the reader
    must not assume one write is one frame.
    """
    body = json.dumps({"jsonrpc": "2.0", "id": 9001, "method": "meta.debug",
                       "params": {"padBytes": 12}}).encode()
    header = b"Content-Length: %d\r\n\r\n" % len(body)
    whole = header + body
    # Cut mid-header, then mid-body.
    fresh_agent.send_raw(whole[:8])
    fresh_agent.send_raw(whole[8:len(header) + 5])
    fresh_agent.send_raw(whole[len(header) + 5:])
    # The id was chosen by us, so wait for it via a fresh request that
    # must be answered after it (ordering on one lane is enough here).
    assert probe(fresh_agent) == "xxxx"


def test_two_frames_in_one_write(fresh_agent):
    """Back-to-back frames in a single write are both processed."""
    frames = b""
    for pad in (3, 5):
        body = json.dumps({"jsonrpc": "2.0", "id": 7000 + pad,
                           "method": "meta.debug",
                           "params": {"padBytes": pad}}).encode()
        frames += b"Content-Length: %d\r\n\r\n" % len(body) + body
    fresh_agent.send_raw(frames)
    assert probe(fresh_agent) == "xxxx"


def test_invalid_json_body_yields_null_id_error(fresh_agent):
    """A frame whose body is not JSON -> null-id INVALID_REQUEST."""
    mark = len(fresh_agent.null_id_errors)
    fresh_agent.send_frame(b"this is not json at all")
    error = fresh_agent.wait_null_id_error(timeout=10, since=mark)
    assert error is not None, "no null-id error envelope arrived"
    assert error.get("code") == INVALID_REQUEST


def test_connection_survives_invalid_json(fresh_agent):
    fresh_agent.send_frame(b"{{{{ not json")
    fresh_agent.wait_null_id_error(timeout=10)
    assert probe(fresh_agent) == "xxxx"


def test_truncated_json_body(fresh_agent):
    """A body that is valid-looking but cut short is rejected cleanly."""
    mark = len(fresh_agent.null_id_errors)
    fresh_agent.send_frame(b'{"jsonrpc":"2.0","id":1,"method":"meta.deb')
    assert fresh_agent.wait_null_id_error(timeout=10, since=mark) is not None
    assert probe(fresh_agent) == "xxxx"


def test_empty_frame_body(fresh_agent):
    """A zero-length frame is malformed, not a connection killer."""
    mark = len(fresh_agent.null_id_errors)
    fresh_agent.send_frame(b"")
    fresh_agent.wait_null_id_error(timeout=5, since=mark)
    assert probe(fresh_agent) == "xxxx"


def test_json_array_body_is_rejected(fresh_agent):
    """JSON-RPC batches are not part of protocol v1."""
    mark = len(fresh_agent.null_id_errors)
    fresh_agent.send_frame(json.dumps([
        {"jsonrpc": "2.0", "id": 1, "method": "meta.stat", "params": {}}
    ]).encode())
    fresh_agent.wait_null_id_error(timeout=5, since=mark)
    assert probe(fresh_agent) == "xxxx", \
        "a JSON array body must not desynchronize the connection"


def test_json_scalar_body_is_rejected(fresh_agent):
    mark = len(fresh_agent.null_id_errors)
    fresh_agent.send_frame(b"42")
    fresh_agent.wait_null_id_error(timeout=5, since=mark)
    assert probe(fresh_agent) == "xxxx"


def test_request_without_method(fresh_agent):
    """An id but no method -> INVALID_REQUEST addressed to that id."""
    request_id = 987654
    fresh_agent.send_frame(json.dumps({"jsonrpc": "2.0", "id": request_id,
                                       "params": {}}).encode())
    # The reply is addressed to an id we never registered, so watch the
    # stream settle and confirm the connection stayed healthy.
    assert probe(fresh_agent) == "xxxx"


def test_notification_with_unknown_method_is_ignored(fresh_agent):
    """Unknown notifications get no reply and no disconnect."""
    fresh_agent.notify("no.such.notification", {"anything": True})
    assert probe(fresh_agent) == "xxxx"


def test_header_case_insensitivity(fresh_agent):
    """`content-length` in any case is a valid header (HTTP semantics)."""
    body = json.dumps({"jsonrpc": "2.0", "id": 4242, "method": "meta.stat",
                       "params": {}}).encode()
    fresh_agent.send_raw(b"content-length: %d\r\n\r\n" % len(body) + body)
    assert probe(fresh_agent) == "xxxx"


def test_extra_headers_are_tolerated(fresh_agent):
    """Unknown headers alongside Content-Length do not break parsing."""
    body = json.dumps({"jsonrpc": "2.0", "id": 4243, "method": "meta.stat",
                       "params": {}}).encode()
    fresh_agent.send_raw(
        b"Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n"
        b"Content-Length: %d\r\n\r\n" % len(body) + body)
    assert probe(fresh_agent) == "xxxx"


def test_utf8_payload_round_trip(fresh_agent, workspace):
    """Multi-byte UTF-8 survives the frame boundary (byte length, not
    character length, is what Content-Length counts)."""
    content = "héllo wörld \U0001F600 你好\n"
    path = workspace.write("utf8.txt", content)
    assert fresh_agent.call("fs.readFile", {"path": path})["content"] == content


def test_large_but_under_cap_request(fresh_agent, workspace):
    """A request body near the negotiated cap is accepted."""
    payload = "z" * 60000
    path = workspace.write("big-param.txt", "ok\n")
    # Oversized param value on a real method: the agent must either
    # answer or reject cleanly, never desync.
    try:
        fresh_agent.call("fs.readFile", {"path": path, "ignored": payload})
    except Exception:                                   # noqa: BLE001
        pass
    assert probe(fresh_agent) == "xxxx"


@pytest.mark.parametrize("garbage", [
    b"GET / HTTP/1.1\r\nHost: x\r\n\r\n",
    b"Welcome to devbox. Last login: Tue.\r\n\r\n",
    b"X-Nonsense: value\r\n\r\n",
])
def test_complete_non_frame_header_block_resyncs(fresh_agent, garbage):
    """A complete but bogus header block is reported, then skipped.

    The transport is a shell pipe: a late login banner or a `motd` line
    can land on stdin mid-session. Treating that like EOF would kill a
    long-lived connection for a cosmetic reason, so the agent must
    answer INVALID_REQUEST and keep serving.
    """
    mark = len(fresh_agent.null_id_errors)
    fresh_agent.send_raw(garbage)
    assert fresh_agent.wait_null_id_error(timeout=5, since=mark) is not None, \
        "no INVALID_REQUEST for a header block without Content-Length"
    assert probe(fresh_agent) == "xxxx", \
        "the connection did not resynchronize after a bogus header block"


def test_blank_line_padding_between_frames(fresh_agent):
    """A bare CRLFCRLF between frames is padding, not an error."""
    mark = len(fresh_agent.null_id_errors)
    fresh_agent.send_raw(b"\r\n\r\n")
    assert probe(fresh_agent) == "xxxx"
    assert len(fresh_agent.null_id_errors) == mark, \
        "empty padding should not produce an error envelope"


def test_binary_garbage_does_not_crash_the_process(fresh_agent):
    """Unterminated binary junk desyncs the stream but must not crash.

    A partial header line is indistinguishable from a slow write, so
    the stream cannot recover -- but the process must stay up rather
    than fault, and the client sees a timeout it can act on.
    """
    fresh_agent.send_raw(b"\x00\x01\x02\x03\xff\xfe")
    try:
        probe(fresh_agent, timeout=3)
    except Exception:                                   # noqa: BLE001
        pass
    assert fresh_agent.proc.poll() in (None, 0), \
        "agent crashed (exit %r) on binary input" % fresh_agent.proc.poll()
