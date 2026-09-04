"""Multiplexing: lanes, the single writer, and sustained load.

The agent's reason for existing is that one expensive connection
carries everything. That only works if concurrent requests stay
correctly matched to their responses, if a bulk operation cannot
starve an interactive one, and if nothing leaks across thousands of
round trips.
"""
import threading
import time

import pytest

from scrutiny import AgentError


def test_responses_match_their_requests(agent):
    """The classic multiplexing bug: right answer, wrong request id."""
    results = {}
    errors = []

    def call(index):
        try:
            pad = 100 + index
            request_id, wait = agent.call_async(
                "meta.debug", {"sleepMs": 50 * (index % 5), "padBytes": pad})
            result = wait(timeout=60)
            results[index] = (request_id, result, pad)
        except AgentError as exc:
            errors.append(exc)

    threads = [threading.Thread(target=call, args=(n,)) for n in range(24)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(90)

    assert not errors, "concurrent requests failed: %r" % errors
    assert len(results) == 24
    for index, (request_id, result, pad) in results.items():
        assert result["echoId"] == request_id, \
            "request %d got the response for id %s" % (request_id,
                                                       result["echoId"])
        assert len(result["pad"]) == pad, \
            "request %d got a body sized for a different request" % index


def test_interactive_preempts_bulk(agent):
    """An interactive request is not stuck behind bulk-lane work."""
    bulk_waits = []
    for n in range(4):
        _id, wait = agent.call_async("meta.debug",
                                     {"sleepMs": 3000, "padBytes": 2000},
                                     lane="bulk")
        bulk_waits.append(wait)
    time.sleep(0.3)
    start = time.monotonic()
    agent.call("meta.debug", {"padBytes": 4}, timeout=30, lane="interactive")
    latency = time.monotonic() - start
    for wait in bulk_waits:
        try:
            wait(timeout=60)
        except AgentError:
            pass
    assert latency < 2.5, \
        "interactive request took %.2fs behind 4 bulk requests" % latency


def test_explicit_lane_override_is_accepted(agent):
    """The top-level `lane` field overrides the per-method default."""
    for lane in ("interactive", "normal", "bulk"):
        result = agent.call("meta.debug", {"padBytes": 6}, lane=lane)
        assert result["pad"] == "xxxxxx", \
            "lane=%s changed the result" % lane


def test_unknown_lane_is_tolerated(agent):
    """A bogus lane name falls back rather than failing the request."""
    try:
        result = agent.call("meta.debug", {"padBytes": 5},
                            lane="no-such-lane")
        assert result["pad"] == "xxxxx"
    except AgentError as exc:
        assert exc.code is not None, "unknown lane produced a transport error"


def test_bulk_lane_has_a_slot_cap(agent):
    """Bulk work is capped so it cannot consume the whole pool."""
    waits = []
    for _ in range(6):
        _id, wait = agent.call_async("meta.debug", {"sleepMs": 1500},
                                     lane="bulk")
        waits.append(wait)
    time.sleep(0.4)
    lanes = agent.call("meta.stat")["lanes"]
    assert lanes["bulk"]["active"] <= 2, \
        "bulk lane ran %d jobs at once; the documented cap is 2" \
        % lanes["bulk"]["active"]
    for wait in waits:
        wait(timeout=60)


def test_mixed_method_concurrency(agent, repo, workspace, tmp_path):
    """Different subsystems in flight at once do not interfere."""
    workspace.write("concurrent.txt", "payload\n")
    results = {}
    errors = []

    def run(name, method, params):
        try:
            results[name] = agent.call(method, params, timeout=90)
        except AgentError as exc:
            errors.append((name, exc))

    work = [
        ("head", "git.headSha", {"path": repo.path}),
        ("commits", "git.commits", {"path": repo.path}),
        ("branches", "git.branches", {"path": repo.path, "local": True}),
        ("diff", "git.diffForCommit",
         {"path": repo.path, "sha": repo.head_sha}),
        ("read", "fs.readFile", {"path": workspace.join("concurrent.txt")}),
        ("list", "fs.listDirectory", {"path": workspace.path}),
        ("stat", "meta.stat", {}),
        ("caps", "meta.capabilities", {}),
        ("debug", "meta.debug", {"padBytes": 64}),
    ]
    threads = [threading.Thread(target=run, args=item) for item in work]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(120)

    assert not errors, "mixed concurrent requests failed: %r" % errors
    assert results["head"]["headSha"] == repo.head_sha
    assert results["read"]["content"] == "payload\n"
    assert len(results["commits"]["commits"]) == 2
    assert len(results["debug"]["pad"]) == 64


def test_sustained_request_volume(agent):
    """A long session of small requests stays correct and responsive."""
    for n in range(400):
        pad = n % 32
        result = agent.call("meta.debug", {"padBytes": pad}, timeout=30)
        assert len(result["pad"]) == pad
    assert agent.alive()


def test_no_leak_across_many_requests(agent):
    """Lanes drain fully -- nothing accumulates over a long session."""
    for _ in range(200):
        agent.call("meta.stat")
    time.sleep(0.3)
    lanes = agent.call("meta.stat")["lanes"]
    assert all(lane["queued"] == 0 for lane in lanes.values()), \
        "queued work remained after a quiet period: %r" % lanes


def test_concurrent_git_reads_on_one_repo(agent, repo):
    """libgit2 access from several workers at once stays correct."""
    errors = []
    answers = []

    def read():
        try:
            answers.append(agent.call("git.headSha",
                                      {"path": repo.path},
                                      timeout=60)["headSha"])
        except AgentError as exc:
            errors.append(exc)

    threads = [threading.Thread(target=read) for _ in range(16)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(90)
    assert not errors, "concurrent git reads failed: %r" % errors
    assert set(answers) == {repo.head_sha}, \
        "concurrent git.headSha disagreed: %r" % set(answers)


def test_concurrent_reads_of_distinct_repos(agent, repo_factory):
    """Per-request repo handles keep parallel reads independent."""
    repos = []
    for _ in range(6):
        instance = repo_factory()
        head, _parent = instance.build_default()
        instance.head_sha = head
        repos.append(instance)

    errors = []
    answers = {}

    def read(index, instance):
        try:
            answers[index] = agent.call("git.headSha",
                                        {"path": instance.path},
                                        timeout=60)["headSha"]
        except AgentError as exc:
            errors.append(exc)

    threads = [threading.Thread(target=read, args=(i, r))
               for i, r in enumerate(repos)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(90)
    assert not errors, "concurrent multi-repo reads failed: %r" % errors
    for index, instance in enumerate(repos):
        assert answers[index] == instance.head_sha, \
            "repo %d got another repo's HEAD" % index


def test_writer_integrity_under_mixed_sizes(agent):
    """Large and small responses interleaving must not corrupt frames."""
    sizes = [4, 200000, 8, 300000, 16, 150000, 32]
    results = {}
    errors = []

    def call(index, size):
        try:
            results[index] = agent.call("meta.debug", {"padBytes": size},
                                        timeout=120)["pad"]
        except AgentError as exc:
            errors.append(exc)

    threads = [threading.Thread(target=call, args=(i, s))
               for i, s in enumerate(sizes)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(180)
    assert not errors, "mixed-size concurrency failed: %r" % errors
    for index, size in enumerate(sizes):
        assert len(results[index]) == size, \
            "response %d had %d bytes, expected %d" \
            % (index, len(results[index]), size)


def test_notifications_do_not_disturb_responses(agent, repo):
    """A stream of notifications alongside requests keeps both intact."""
    stop = threading.Event()

    def spam():
        while not stop.is_set():
            agent.notify("index.cancel", {"indexerId": "nonexistent"})
            time.sleep(0.005)

    noise = threading.Thread(target=spam, daemon=True)
    noise.start()
    try:
        for _ in range(60):
            assert agent.call("git.headSha",
                              {"path": repo.path})["headSha"] == repo.head_sha
    finally:
        stop.set()
        noise.join(5)
    assert agent.alive()


@pytest.mark.slow
def test_soak(agent, repo, workspace):
    """A few thousand mixed operations, checking for drift or leaks."""
    workspace.write("soak.txt", "soak\n")
    for n in range(2000):
        if n % 4 == 0:
            assert agent.call("git.headSha",
                              {"path": repo.path})["headSha"] == repo.head_sha
        elif n % 4 == 1:
            assert agent.call(
                "fs.readFile",
                {"path": workspace.join("soak.txt")})["content"] == "soak\n"
        elif n % 4 == 2:
            agent.call("meta.stat")
        else:
            assert len(agent.call("meta.debug",
                                  {"padBytes": n % 100})["pad"]) == n % 100
    assert agent.alive()
    lanes = agent.call("meta.stat")["lanes"]
    assert all(lane["queued"] == 0 for lane in lanes.values())
