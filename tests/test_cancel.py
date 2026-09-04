"""`$/cancelRequest`: dropping queued work and interrupting running work.

Cancellation is what keeps an interactive client responsive when the
user navigates away from a slow operation. The properties that matter:
the cancelled request answers (never hangs), it answers CANCELLED, it
answers promptly, and nothing else in flight is disturbed.
"""
import time

import pytest

from scrutiny import AgentError, CANCELLED


def test_cancel_interrupts_running_work(agent):
    request_id, wait = agent.call_async("meta.debug", {"sleepMs": 10000})
    time.sleep(0.3)
    agent.cancel(request_id)
    start = time.monotonic()
    with pytest.raises(AgentError) as excinfo:
        wait(timeout=8)
    assert excinfo.value.code == CANCELLED
    assert time.monotonic() - start < 6, \
        "cancellation waited for the full sleep instead of interrupting"


def test_cancel_answers_rather_than_dropping(agent):
    """The cancelled request must still get a response envelope."""
    request_id, wait = agent.call_async("meta.debug", {"sleepMs": 5000})
    agent.cancel(request_id)
    with pytest.raises(AgentError) as excinfo:
        wait(timeout=10)
    assert excinfo.value.code == CANCELLED, \
        "expected a CANCELLED response, got %s" % excinfo.value


def test_cancel_before_work_starts(agent):
    """Cancelling immediately drops the request from the queue."""
    request_id, wait = agent.call_async("meta.debug", {"sleepMs": 8000})
    agent.cancel(request_id)
    with pytest.raises(AgentError) as excinfo:
        wait(timeout=10)
    assert excinfo.value.code == CANCELLED


def test_cancel_unknown_id_is_harmless(agent):
    agent.cancel(999999)
    assert agent.call("meta.debug", {"padBytes": 3})["pad"] == "xxx"


def test_cancel_completed_request_is_harmless(agent):
    request_id, wait = agent.call_async("meta.debug", {"padBytes": 4})
    wait()
    agent.cancel(request_id)
    assert agent.call("meta.debug", {"padBytes": 3})["pad"] == "xxx"
    assert agent.alive()


def test_duplicate_cancel_is_harmless(agent):
    request_id, wait = agent.call_async("meta.debug", {"sleepMs": 6000})
    agent.cancel(request_id)
    agent.cancel(request_id)
    agent.cancel(request_id)
    with pytest.raises(AgentError) as excinfo:
        wait(timeout=10)
    assert excinfo.value.code == CANCELLED
    assert agent.alive()


def test_cancel_does_not_disturb_other_requests(agent):
    """Only the named request is cancelled."""
    victim_id, victim_wait = agent.call_async("meta.debug", {"sleepMs": 6000})
    survivor_id, survivor_wait = agent.call_async("meta.debug",
                                                  {"sleepMs": 700,
                                                   "padBytes": 32})
    time.sleep(0.2)
    agent.cancel(victim_id)

    with pytest.raises(AgentError) as excinfo:
        victim_wait(timeout=10)
    assert excinfo.value.code == CANCELLED

    result = survivor_wait(timeout=30)
    assert result["echoId"] == survivor_id
    assert result["pad"] == "x" * 32


def test_many_cancellations_do_not_leak(agent):
    """Repeated cancel cycles leave the lanes drained."""
    for _ in range(15):
        request_id, wait = agent.call_async("meta.debug", {"sleepMs": 4000})
        agent.cancel(request_id)
        with pytest.raises(AgentError):
            wait(timeout=10)
    time.sleep(0.5)
    lanes = agent.call("meta.stat")["lanes"]
    assert all(lane["queued"] == 0 for lane in lanes.values()), \
        "lanes still hold cancelled work: %r" % lanes
    # Only the in-flight meta.stat itself should be active.
    assert sum(lane["active"] for lane in lanes.values()) <= 1, \
        "cancelled work is still running: %r" % lanes
    assert agent.alive()


def test_cancel_a_streamed_response(agent):
    """Cancelling a big response does not corrupt the connection.

    Whether the chunks already left or not, the client must end up in a
    consistent state and the next request must be answered correctly.
    """
    request_id, wait = agent.call_async(
        "meta.debug", {"sleepMs": 1500, "padBytes": agent.frame_cap * 3})
    time.sleep(0.2)
    agent.cancel(request_id)
    try:
        wait(timeout=30)
    except AgentError as exc:
        assert exc.code == CANCELLED
    assert agent.call("meta.debug", {"padBytes": 7})["pad"] == "xxxxxxx"


def test_cancel_notification_shape_is_ignored_when_malformed(agent):
    """A `$/cancelRequest` without an id is dropped, not fatal."""
    agent.notify("$/cancelRequest", {})
    agent.notify("$/cancelRequest", {"id": "not-a-number"})
    assert agent.call("meta.debug", {"padBytes": 3})["pad"] == "xxx"
    assert agent.alive()
