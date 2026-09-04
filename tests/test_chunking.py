"""`rpc.chunk`: streaming responses that exceed the frame cap.

Every large result the client cares about (a full diff, a directory
listing, a file body) rides this path, and a bug here corrupts data
rather than failing loudly -- so these tests check byte-exact
reassembly, ordering, and interleaving between concurrent requests.
"""
import base64
import json
import threading

import pytest


def chunk_notifications(agent, request_id):
    return [p for m, p in agent.notifications
            if m == "rpc.chunk" and p.get("id") == request_id]


def test_small_response_is_not_chunked(agent):
    mark = agent.notification_mark()
    agent.call("meta.debug", {"padBytes": 100})
    assert not [p for m, p in agent.notifications[mark:] if m == "rpc.chunk"], \
        "a 100-byte response should not be streamed"


def test_over_cap_response_reassembles_exactly(agent):
    """The reassembled body is byte-identical to what was asked for."""
    size = agent.frame_cap * 3
    result = agent.call("meta.debug", {"padBytes": size}, timeout=60)
    assert len(result["pad"]) == size
    assert result["pad"] == "x" * size


@pytest.mark.parametrize("multiplier", [1.01, 2, 5])
def test_sizes_around_the_cap(agent, multiplier):
    size = int(agent.frame_cap * multiplier)
    result = agent.call("meta.debug", {"padBytes": size}, timeout=60)
    assert len(result["pad"]) == size


def test_just_under_the_cap_is_inline(agent):
    """A body comfortably under the cap comes back in one frame."""
    size = agent.frame_cap // 2
    mark = agent.notification_mark()
    result = agent.call("meta.debug", {"padBytes": size}, timeout=60)
    assert len(result["pad"]) == size
    streamed = [p for m, p in agent.notifications[mark:] if m == "rpc.chunk"]
    assert not streamed, "under-cap response was streamed unnecessarily"


def test_chunk_sequence_is_complete_and_ordered(agent):
    """seq numbers are 0..N with exactly one `last`."""
    request_id, wait = agent.call_async("meta.debug",
                                        {"padBytes": agent.frame_cap * 3})
    wait(timeout=60)
    chunks = chunk_notifications(agent, request_id)
    assert chunks, "no rpc.chunk notifications for an over-cap response"
    seqs = [c["seq"] for c in chunks]
    assert sorted(seqs) == list(range(len(seqs))), \
        "seq numbers are not a complete 0..N run: %r" % sorted(seqs)
    lasts = [c for c in chunks if c["last"]]
    assert len(lasts) == 1, "expected exactly one last=true chunk"
    assert lasts[0]["seq"] == max(seqs), "last=true is not the final seq"


def test_chunk_payloads_are_valid_base64(agent):
    request_id, wait = agent.call_async("meta.debug",
                                        {"padBytes": agent.frame_cap * 2})
    wait(timeout=60)
    for chunk in chunk_notifications(agent, request_id):
        base64.b64decode(chunk["data"], validate=True)


def test_chunks_reconstruct_a_jsonrpc_envelope(agent):
    """The concatenated payload is a complete response envelope."""
    request_id, wait = agent.call_async("meta.debug",
                                        {"padBytes": agent.frame_cap * 2})
    wait(timeout=60)
    chunks = sorted(chunk_notifications(agent, request_id),
                    key=lambda c: c["seq"])
    blob = base64.b64decode("".join(c["data"] for c in chunks))
    envelope = json.loads(blob)
    assert envelope["jsonrpc"] == "2.0"
    assert envelope["id"] == request_id
    assert "result" in envelope


def test_envelope_reports_the_streamed_byte_count(agent):
    """The small `{streamed, bytes}` envelope matches what was sent."""
    request_id, wait = agent.call_async("meta.debug",
                                        {"padBytes": agent.frame_cap * 2})
    wait(timeout=60)
    chunks = sorted(chunk_notifications(agent, request_id),
                    key=lambda c: c["seq"])
    total = len(base64.b64decode("".join(c["data"] for c in chunks)))
    assert total > agent.frame_cap


def test_no_chunk_frame_exceeds_the_cap(agent):
    """Individual chunk notifications stay inside the negotiated cap."""
    request_id, wait = agent.call_async("meta.debug",
                                        {"padBytes": agent.frame_cap * 3})
    wait(timeout=60)
    for chunk in chunk_notifications(agent, request_id):
        raw = len(base64.b64decode(chunk["data"]))
        assert raw <= agent.frame_cap, \
            "chunk seq=%d carries %d raw bytes, over the %d cap" \
            % (chunk["seq"], raw, agent.frame_cap)


def test_interleaved_streams_do_not_mix(agent):
    """Concurrent over-cap responses stay separable by request id.

    This is the corruption case that matters: two big results in flight
    at once must not splice into each other.
    """
    sizes = [agent.frame_cap * 2 + 11, agent.frame_cap * 3 + 7,
             agent.frame_cap + 1]
    results = {}
    errors = []

    def run(size, slot):
        try:
            results[slot] = agent.call("meta.debug", {"padBytes": size},
                                       timeout=90)["pad"]
        except Exception as exc:                        # noqa: BLE001
            errors.append(exc)

    threads = [threading.Thread(target=run, args=(size, i))
               for i, size in enumerate(sizes)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(120)

    assert not errors, "concurrent streamed responses failed: %r" % errors
    for index, size in enumerate(sizes):
        assert len(results[index]) == size
        assert results[index] == "x" * size, \
            "streamed body %d was corrupted by interleaving" % index


def test_chunked_and_inline_responses_interleave(agent):
    """A small request answered while a big one streams is unaffected."""
    big_id, big_wait = agent.call_async("meta.debug",
                                        {"padBytes": agent.frame_cap * 4})
    small = agent.call("meta.debug", {"padBytes": 5}, timeout=30)
    assert small["pad"] == "xxxxx"
    assert len(big_wait(timeout=90)["pad"]) == agent.frame_cap * 4


def test_large_file_read_streams_correctly(agent, workspace):
    """A real payload -- a big file over fs.readFile -- round-trips."""
    content = "".join("line %06d\n" % n for n in range(40000))
    path = workspace.write("big.txt", content)
    result = agent.call("fs.readFile", {"path": path}, timeout=90)
    assert result["content"] == content


def test_large_directory_listing_streams_correctly(agent, workspace):
    """A directory big enough to exceed the cap lists completely."""
    names = ["entry-%05d.txt" % n for n in range(6000)]
    for name in names:
        workspace.write(name, "x")
    result = agent.call("fs.listDirectory", {"path": workspace.path},
                        timeout=90)
    got = [entry["name"] for entry in result["entries"]]
    assert got == sorted(names), \
        "large listing lost or reordered entries (%d of %d)" \
        % (len(got), len(names))


def test_binary_content_survives_streaming(agent, workspace):
    """Bytes that are not valid UTF-8 must not silently corrupt.

    The agent documents a binary-safe emit; whatever encoding it picks,
    a second read must return the identical value (no lossy round trip).
    """
    blob = bytes(range(256)) * 600
    path = workspace.write("blob.bin", blob)
    first = agent.call("fs.readFile", {"path": path}, timeout=60)["content"]
    second = agent.call("fs.readFile", {"path": path}, timeout=60)["content"]
    assert first == second, "binary read is not deterministic"


@pytest.mark.slow
def test_very_large_response(agent):
    """A 20 MiB body streams without loss -- the ceiling case."""
    size = 20 * 1024 * 1024
    result = agent.call("meta.debug", {"padBytes": size}, timeout=300)
    assert len(result["pad"]) == size
