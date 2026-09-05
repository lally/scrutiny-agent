"""`watch.head`: HEAD-file change notifications.

The documented semantics are precise and easy to get subtly wrong: the
watcher observes the clone's *resolved `.git/HEAD` file*, so a checkout
or rebase fires but a plain commit (which only moves the branch ref)
does not. Clients use this to know when to re-read the review state.
"""
import os
import time

import pytest

from scrutiny import NOT_FOUND, INVALID_REQUEST
from scrutiny.fixtures import git


def start_watch(agent, repo):
    result = agent.call("watch.head", {"path": repo.path})
    watch_id = result["watchId"]
    assert isinstance(watch_id, str) and watch_id
    return watch_id


def test_watch_returns_an_id(agent, repo):
    assert start_watch(agent, repo)


def test_watch_ids_are_unique(fresh_agent, repo_factory):
    ids = set()
    for _ in range(3):
        repo = repo_factory()
        repo.build_default()
        ids.add(fresh_agent.call("watch.head", {"path": repo.path})["watchId"])
    assert len(ids) == 3, "watch ids collided: %r" % ids


def test_branch_switch_fires(fresh_agent, repo):
    watch_id = start_watch(fresh_agent, repo)
    mark = fresh_agent.notification_mark()
    repo.checkout("switched", create=True)
    note = fresh_agent.wait_notification(
        "watch.headChanged", lambda p: p.get("watchId") == watch_id,
        timeout=20, since=mark)
    assert note is not None, "branch switch did not fire watch.headChanged"


def test_checkout_back_fires_again(fresh_agent, repo):
    """The watch re-arms: a second HEAD change fires a second time.

    The two checkouts are separated by more than the debounce window;
    coalescing rapid changes is the documented behavior and is covered
    by test_notifications_are_debounced.
    """
    watch_id = start_watch(fresh_agent, repo)
    time.sleep(0.5)
    repo.checkout("other", create=True)
    assert fresh_agent.wait_notification(
        "watch.headChanged", lambda p: p.get("watchId") == watch_id,
        timeout=20) is not None
    time.sleep(1.5)
    mark = fresh_agent.notification_mark()
    repo.checkout("work")
    note = fresh_agent.wait_notification(
        "watch.headChanged", lambda p: p.get("watchId") == watch_id,
        timeout=20, since=mark)
    assert note is not None, "switching back did not fire a second time"


def test_detached_checkout_fires(fresh_agent, repo):
    watch_id = start_watch(fresh_agent, repo)
    mark = fresh_agent.notification_mark()
    git(repo.path, "checkout", "-q", "--detach", repo.parent_sha)
    note = fresh_agent.wait_notification(
        "watch.headChanged", lambda p: p.get("watchId") == watch_id,
        timeout=20, since=mark)
    assert note is not None, "detaching HEAD did not fire"


def test_plain_commit_does_not_fire(fresh_agent, repo):
    """Documented: a commit moves the branch ref, not the HEAD file."""
    watch_id = start_watch(fresh_agent, repo)
    time.sleep(0.5)
    mark = fresh_agent.notification_mark()
    repo.commit("just a commit", {"file.txt": "committed\n"})
    note = fresh_agent.wait_notification(
        "watch.headChanged", lambda p: p.get("watchId") == watch_id,
        timeout=4, since=mark)
    assert note is None, \
        "a plain commit fired watch.headChanged; only HEAD-file changes should"


def test_watch_is_scoped_to_its_repo(fresh_agent, repo_factory):
    """A change in one repo must not notify another repo's watch."""
    first = repo_factory()
    first.build_default()
    second = repo_factory()
    second.build_default()
    first_id = fresh_agent.call("watch.head", {"path": first.path})["watchId"]
    second_id = fresh_agent.call("watch.head",
                                 {"path": second.path})["watchId"]
    time.sleep(0.5)
    mark = fresh_agent.notification_mark()
    second.checkout("moved", create=True)
    assert fresh_agent.wait_notification(
        "watch.headChanged", lambda p: p.get("watchId") == second_id,
        timeout=20, since=mark) is not None
    stray = fresh_agent.wait_notification(
        "watch.headChanged", lambda p: p.get("watchId") == first_id,
        timeout=2, since=mark)
    assert stray is None, "a watch fired for a change in a different repo"


def test_stop_silences_the_watch(fresh_agent, repo):
    watch_id = start_watch(fresh_agent, repo)
    time.sleep(0.5)
    fresh_agent.notify("watch.stop", {"watchId": watch_id})
    time.sleep(0.5)
    mark = fresh_agent.notification_mark()
    repo.checkout("after-stop", create=True)
    note = fresh_agent.wait_notification(
        "watch.headChanged", lambda p: p.get("watchId") == watch_id,
        timeout=4, since=mark)
    assert note is None, "watch.stop did not stop the notifications"


def test_stop_unknown_id_is_harmless(agent):
    agent.notify("watch.stop", {"watchId": "w-does-not-exist"})
    assert agent.call("meta.debug", {"padBytes": 3})["pad"] == "xxx"
    assert agent.alive()


def test_stop_twice_is_harmless(fresh_agent, repo):
    watch_id = start_watch(fresh_agent, repo)
    fresh_agent.notify("watch.stop", {"watchId": watch_id})
    fresh_agent.notify("watch.stop", {"watchId": watch_id})
    assert fresh_agent.call("meta.debug", {"padBytes": 3})["pad"] == "xxx"
    assert fresh_agent.alive()


def test_watch_requires_a_repository(agent, workspace):
    assert agent.call_expect_error(
        "watch.head", {"path": workspace.path}).code == NOT_FOUND


def test_watch_missing_path(agent):
    assert agent.call_expect_error("watch.head",
                                   {"path": "/no/such/repo"}).code == NOT_FOUND


def test_watch_requires_param(agent):
    assert agent.call_expect_error("watch.head", {}).code == INVALID_REQUEST


def test_two_watches_on_one_repo_both_fire(fresh_agent, repo):
    first = fresh_agent.call("watch.head", {"path": repo.path})["watchId"]
    second = fresh_agent.call("watch.head", {"path": repo.path})["watchId"]
    assert first != second
    time.sleep(0.5)
    mark = fresh_agent.notification_mark()
    repo.checkout("both", create=True)
    for watch_id in (first, second):
        assert fresh_agent.wait_notification(
            "watch.headChanged", lambda p, w=watch_id: p.get("watchId") == w,
            timeout=20, since=mark) is not None, \
            "watch %s did not fire" % watch_id


def test_notifications_are_debounced(fresh_agent, repo):
    """Rapid HEAD churn coalesces instead of flooding the transport."""
    watch_id = start_watch(fresh_agent, repo)
    time.sleep(0.5)
    mark = fresh_agent.notification_mark()
    for n in range(10):
        repo.checkout("churn-%d" % n, create=True)
    time.sleep(3)
    fired = sum(1 for method, params in fresh_agent.notifications[mark:]
                if method == "watch.headChanged"
                and params.get("watchId") == watch_id)
    assert fired >= 1, "rapid checkouts fired nothing at all"
    assert fired < 10, \
        "10 checkouts produced %d notifications; debouncing is not working" \
        % fired


def test_watch_survives_repo_deletion(fresh_agent, repo_factory):
    """Removing the watched repo must not take the agent down."""
    import shutil
    repo = repo_factory()
    repo.build_default()
    watch_id = fresh_agent.call("watch.head", {"path": repo.path})["watchId"]
    time.sleep(0.5)
    shutil.rmtree(repo.path, ignore_errors=True)
    time.sleep(1.5)
    assert fresh_agent.alive(), "agent died after its watched repo vanished"
    assert fresh_agent.call("meta.debug", {"padBytes": 3})["pad"] == "xxx"
    fresh_agent.notify("watch.stop", {"watchId": watch_id})


def test_agent_shuts_down_cleanly_with_live_watches(agent_factory, repo):
    """A watch thread must not block or crash shutdown."""
    instance = agent_factory(["--allow-root", "/tmp"])
    instance.call("watch.head", {"path": repo.path})
    instance.close(timeout=15)
    assert instance.proc.poll() is not None, \
        "agent did not exit with a live HEAD watch"
