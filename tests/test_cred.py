"""The credential broker.

The security claim is specific: a git credential never lands on the
agent's disk, config, environment, or command line -- it exists only
transiently in the broker→askpass pipe, brokered from the client per
prompt. `cred.selftest` drives the whole path (unix socket, the agent
re-exec'd as `$GIT_ASKPASS`, the notification round trip) without
needing a real remote that demands auth.
"""
import os
import re
import uuid

import pytest

from scrutiny import INVALID_REQUEST


def test_selftest_round_trip(agent):
    """The secret the client provides reaches the askpass child."""
    op_id = "op-" + uuid.uuid4().hex
    secret = "s3cr3t-" + uuid.uuid4().hex
    agent.expect_secret(op_id, secret)
    result = agent.call("cred.selftest",
                        {"authOpId": op_id,
                         "prompt": "Password for 'https://example.invalid':"},
                        timeout=60)
    assert result["got"] == secret
    assert result["askpassExit"] == 0


def test_selftest_emits_a_cred_request(agent):
    op_id = "op-" + uuid.uuid4().hex
    secret = "value-" + uuid.uuid4().hex
    prompt = "Username for 'https://example.invalid':"
    agent.expect_secret(op_id, secret)
    mark = agent.notification_mark()
    agent.call("cred.selftest", {"authOpId": op_id, "prompt": prompt},
               timeout=60)
    request = agent.wait_notification(
        "cred.request", lambda p: p.get("authOpId") == op_id,
        timeout=10, since=mark)
    assert request is not None, "no cred.request notification was emitted"
    assert request["prompt"] == prompt
    assert isinstance(request["credId"], str) and request["credId"]


def test_cred_ids_are_unique_per_prompt(agent):
    seen = set()
    for _ in range(4):
        op_id = "op-" + uuid.uuid4().hex
        agent.expect_secret(op_id, "x")
        mark = agent.notification_mark()
        agent.call("cred.selftest", {"authOpId": op_id, "prompt": "p:"},
                   timeout=60)
        request = agent.wait_notification(
            "cred.request", lambda p: p.get("authOpId") == op_id,
            timeout=10, since=mark)
        assert request["credId"] not in seen, \
            "credId %r was reused across prompts" % request["credId"]
        seen.add(request["credId"])


def test_empty_secret_is_delivered(agent):
    """An empty answer is a valid answer (it fails the git op, cleanly)."""
    op_id = "op-" + uuid.uuid4().hex
    agent.expect_secret(op_id, "")
    result = agent.call("cred.selftest",
                        {"authOpId": op_id, "prompt": "Password:"},
                        timeout=60)
    assert result["got"] == ""


def test_secret_with_special_characters(agent):
    """Shell metacharacters must survive the pipe untouched."""
    op_id = "op-" + uuid.uuid4().hex
    secret = "p@ss w0rd!$`\"'\\;|&<>(){}[]#*?~"
    agent.expect_secret(op_id, secret)
    result = agent.call("cred.selftest",
                        {"authOpId": op_id, "prompt": "Password:"},
                        timeout=60)
    assert result["got"] == secret


def test_long_secret(agent):
    """A token-sized secret is not truncated."""
    op_id = "op-" + uuid.uuid4().hex
    secret = "ghp_" + "A" * 500
    agent.expect_secret(op_id, secret)
    result = agent.call("cred.selftest",
                        {"authOpId": op_id, "prompt": "Password:"},
                        timeout=60)
    assert result["got"] == secret


def test_unicode_secret(agent):
    op_id = "op-" + uuid.uuid4().hex
    secret = "pässwörd-\U0001F510"
    agent.expect_secret(op_id, secret)
    result = agent.call("cred.selftest",
                        {"authOpId": op_id, "prompt": "Password:"},
                        timeout=60)
    assert result["got"] == secret


def test_provide_unknown_cred_id(agent):
    result = agent.call("cred.provide",
                        {"credId": "c-does-not-exist", "value": "x"})
    assert result["ok"] is False


def test_provide_twice_is_rejected_the_second_time(agent):
    """A credId is single-use; a replay must not be accepted."""
    op_id = "op-" + uuid.uuid4().hex
    agent.expect_secret(op_id, "once")
    mark = agent.notification_mark()
    agent.call("cred.selftest", {"authOpId": op_id, "prompt": "p:"},
               timeout=60)
    request = agent.wait_notification(
        "cred.request", lambda p: p.get("authOpId") == op_id,
        timeout=10, since=mark)
    replay = agent.call("cred.provide",
                        {"credId": request["credId"], "value": "again"})
    assert replay["ok"] is False, \
        "a consumed credId was accepted a second time"


def test_provide_requires_params(agent):
    assert agent.call_expect_error("cred.provide", {}).code == INVALID_REQUEST


def test_selftest_requires_params(agent):
    assert agent.call_expect_error("cred.selftest",
                                   {}).code == INVALID_REQUEST


def test_secret_does_not_reach_the_log(agent, session_log_path):
    """The whole point: the secret must not be written to disk."""
    op_id = "op-" + uuid.uuid4().hex
    secret = "TOPSECRET" + uuid.uuid4().hex.upper()
    agent.expect_secret(op_id, secret)
    agent.call("cred.selftest", {"authOpId": op_id, "prompt": "Password:"},
               timeout=60)
    with open(session_log_path, "rb") as handle:
        log = handle.read().decode("utf-8", "replace")
    assert secret not in log, \
        "the credential was written to the agent's log file"


def test_secret_does_not_reach_stderr(agent):
    op_id = "op-" + uuid.uuid4().hex
    secret = "STDERRSECRET" + uuid.uuid4().hex.upper()
    agent.expect_secret(op_id, secret)
    agent.call("cred.selftest", {"authOpId": op_id, "prompt": "Password:"},
               timeout=60)
    assert secret not in agent.stderr_text(), \
        "the credential was echoed to stderr"


def test_secret_does_not_persist_in_the_repo_config(agent,
                                                    clone_with_upstream):
    """A brokered fetch must not leave a token in .git/config."""
    clone, _ = clone_with_upstream
    op_id = "op-" + uuid.uuid4().hex
    agent.expect_secret(op_id, "NEVERSTORED")
    agent.call("git.fetch", {"repoPath": clone.path, "authOpId": op_id},
               timeout=120)
    with open(os.path.join(clone.path, ".git", "config")) as handle:
        config = handle.read()
    assert "NEVERSTORED" not in config


def test_broker_socket_is_private(agent, clone_with_upstream):
    """No world-readable socket is left behind after an op."""
    clone, _ = clone_with_upstream
    agent.call("git.fetch", {"repoPath": clone.path, "authOpId": "op-perm"},
               timeout=120)
    stale = [name for name in os.listdir("/tmp")
             if name.startswith(".scrutiny-cred-")]
    for name in stale:
        mode = os.stat(os.path.join("/tmp", name)).st_mode & 0o777
        assert mode == 0o600, \
            "leftover broker socket %s has mode %o" % (name, mode)


def test_many_sequential_brokered_ops(agent, clone_with_upstream):
    """Repeated brokered operations do not exhaust sockets or fds."""
    clone, _ = clone_with_upstream
    for n in range(10):
        result = agent.call("git.fetch",
                            {"repoPath": clone.path, "authOpId": "op-%d" % n},
                            timeout=120)
        assert result["ok"] is True
    assert agent.alive()
