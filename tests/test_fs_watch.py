"""`fs.listDirectory {watch}` / `fs.changed`: a listing that stays true.

A client that lists a directory holds a copy of it. These tests are
about what the agent promises regarding that copy: every change to a
watched directory is pushed as the *new state* of the names involved,
so the client can patch what it holds and never has to ask again.
The properties that matter are the ones a cache depends on -- nothing
missed, deletions reported as deletions, and an honest `watching:
false` wherever the promise cannot be kept.
"""
import os
import sys
import time

import pytest

pytestmark = pytest.mark.skipif(not sys.platform.startswith("linux"),
                                reason="directory watches are inotify-only")


@pytest.fixture
def w_agent(agent_factory, tmp_path):
    return agent_factory(["--allow-root", str(tmp_path), "--allow-write"])


def listing(agent, path, **extra):
    params = {"path": str(path), "attributes": True, "watch": True}
    params.update(extra)
    return agent.call("fs.listDirectory", params)


def changes_for(agent, directory, since, predicate, timeout=10):
    return agent.wait_notification(
        "fs.changed",
        lambda p: p.get("path") == str(directory) and predicate(p),
        timeout=timeout, since=since)


def names(note):
    return {e["name"]: e for e in note.get("entries", [])}


def collect(agent, directory, since, done, timeout=10):
    """Every fs.changed for DIRECTORY after SINCE, until DONE(merged)."""
    deadline = time.monotonic() + timeout
    while True:
        notes = [p for m, p in list(agent.notifications[since:])
                 if m == "fs.changed" and p.get("path") == str(directory)]
        merged = {}
        for note in notes:
            merged.update(names(note))
        if done(merged) or time.monotonic() > deadline:
            return notes, merged
        time.sleep(0.05)


def test_feature_is_advertised(w_agent):
    assert "fs.watch" in w_agent.hello["features"]


def test_listing_reports_whether_it_is_watched(w_agent, tmp_path):
    assert listing(w_agent, tmp_path)["watching"] is True
    plain = w_agent.call("fs.listDirectory",
                         {"path": str(tmp_path), "attributes": True})
    assert "watching" not in plain, \
        "a client that did not ask must not be told it has a watch"


def test_listing_carries_the_directory_itself(w_agent, tmp_path):
    me = listing(w_agent, tmp_path)["self"]
    assert me["exists"] and me["isDir"]
    assert me["mtimeNs"] == os.stat(tmp_path).st_mtime_ns


def test_created_file_is_pushed_with_its_attributes(w_agent, tmp_path):
    listing(w_agent, tmp_path)
    mark = w_agent.notification_mark()
    (tmp_path / "new.txt").write_text("hello")
    note = changes_for(w_agent, tmp_path, mark, lambda p: "new.txt" in names(p))
    assert note is not None, "creating a file in a watched directory was silent"
    entry = names(note)["new.txt"]
    assert entry["size"] == 5 and entry["isRegular"] and not entry["isDir"]
    assert entry["mtimeNs"] == os.stat(tmp_path / "new.txt").st_mtime_ns
    assert note["self"]["isDir"], "the directory's own attributes ride along"


def test_modified_file_is_pushed(w_agent, tmp_path):
    target = tmp_path / "a.txt"
    target.write_text("one")
    listing(w_agent, tmp_path)
    mark = w_agent.notification_mark()
    time.sleep(0.05)
    target.write_text("three")
    note = changes_for(w_agent, tmp_path, mark,
                       lambda p: names(p).get("a.txt", {}).get("size") == 5)
    assert note is not None


def test_deletion_is_pushed_as_a_deletion(w_agent, tmp_path):
    target = tmp_path / "gone.txt"
    target.write_text("x")
    listing(w_agent, tmp_path)
    mark = w_agent.notification_mark()
    target.unlink()
    note = changes_for(w_agent, tmp_path, mark, lambda p: "gone.txt" in names(p))
    assert note is not None
    assert names(note)["gone.txt"] == {"name": "gone.txt", "deleted": True}


def test_rename_reports_both_names(w_agent, tmp_path):
    (tmp_path / "old").write_text("x")
    listing(w_agent, tmp_path)
    mark = w_agent.notification_mark()
    os.rename(tmp_path / "old", tmp_path / "new")
    _, seen = collect(w_agent, tmp_path, mark,
                      lambda m: {"old", "new"} <= set(m))
    assert seen.get("old", {}).get("deleted") is True
    assert seen.get("new", {}).get("isRegular") is True


def test_chmod_is_pushed(w_agent, tmp_path):
    target = tmp_path / "script.sh"
    target.write_text("#!/bin/sh\n")
    os.chmod(target, 0o644)
    listing(w_agent, tmp_path)
    mark = w_agent.notification_mark()
    os.chmod(target, 0o755)
    note = changes_for(w_agent, tmp_path, mark,
                       lambda p: names(p).get("script.sh", {}).get("mode") == 0o755)
    assert note is not None


def test_a_burst_is_coalesced(w_agent, tmp_path):
    """Fifty files created together arrive as a few notes, not fifty."""
    listing(w_agent, tmp_path)
    mark = w_agent.notification_mark()
    for i in range(50):
        (tmp_path / ("f%02d" % i)).write_text("x")
    notes, seen = collect(w_agent, tmp_path, mark, lambda m: len(m) >= 50)
    notes = len(notes)
    assert len(seen) == 50, "lost %d of 50 creations" % (50 - len(seen))
    assert notes <= 10, "50 creations produced %d notifications" % notes


def test_the_agents_own_writes_are_reported_too(w_agent, tmp_path):
    """A client patches its copy from fs.changed; its own save must not
    be the one change it never hears about. The temp file the atomic
    write goes through is an implementation detail and is not."""
    listing(w_agent, tmp_path)
    mark = w_agent.notification_mark()
    w_agent.call("fs.writeFile", {"path": str(tmp_path / "saved.txt"),
                                  "content": "data"})
    note = changes_for(w_agent, tmp_path, mark,
                       lambda p: "saved.txt" in names(p))
    assert note is not None
    time.sleep(0.5)
    for method, params in w_agent.notifications[mark:]:
        if method == "fs.changed":
            assert not any(n.startswith(".scrutiny-write-")
                           for n in names(params)), params


def test_unwatched_directory_is_silent(w_agent, tmp_path):
    quiet = tmp_path / "quiet"
    quiet.mkdir()
    w_agent.call("fs.listDirectory", {"path": str(quiet), "attributes": True})
    mark = w_agent.notification_mark()
    (quiet / "x").write_text("x")
    assert changes_for(w_agent, quiet, mark, lambda p: True, timeout=1) is None


def test_removed_directory_ends_its_watch(w_agent, tmp_path):
    doomed = tmp_path / "doomed"
    doomed.mkdir()
    listing(w_agent, doomed)
    mark = w_agent.notification_mark()
    doomed.rmdir()
    note = changes_for(w_agent, doomed, mark, lambda p: p.get("unwatched"))
    assert note is not None and note["reset"] is True


def test_a_constantly_changing_directory_loses_its_watch(w_agent, tmp_path):
    """Narrating a build tree is worse than not caching it: after a
    sustained stream the agent says so and stops."""
    busy = tmp_path / "busy"
    busy.mkdir()
    listing(w_agent, busy)
    mark = w_agent.notification_mark()
    note = None
    deadline = time.monotonic() + 30
    i = 0
    while time.monotonic() < deadline and note is None:
        (busy / "log").write_text(str(i))
        i += 1
        time.sleep(0.06)
        note = w_agent.wait_notification(
            "fs.changed",
            lambda p: p.get("path") == str(busy) and p.get("unwatched"),
            timeout=0.01, since=mark)
    assert note is not None, "the watch was never dropped"
    # ...and asking again re-arms it.
    assert listing(w_agent, busy)["watching"] is True


def test_watch_does_not_widen_the_sandbox(w_agent):
    from scrutiny import PERMISSION_DENIED
    assert w_agent.call_expect_error(
        "fs.listDirectory", {"path": "/etc", "watch": True}).code == \
        PERMISSION_DENIED


# ---------------------------------------------------------------------
# conditional writes
# ---------------------------------------------------------------------
def test_conditional_write_succeeds_when_unchanged(w_agent, tmp_path):
    target = tmp_path / "f.txt"
    target.write_text("v1")
    seen = os.stat(target).st_mtime_ns
    result = w_agent.call("fs.writeFile", {"path": str(target),
                                           "content": "v2",
                                           "ifMtimeNs": seen})
    assert result["ok"] is True
    assert target.read_text() == "v2"
    assert result["mtimeNs"] == os.stat(target).st_mtime_ns


def test_conditional_write_refuses_a_file_that_moved_on(w_agent, tmp_path):
    target = tmp_path / "f.txt"
    target.write_text("v1")
    seen = os.stat(target).st_mtime_ns
    time.sleep(0.02)
    target.write_text("someone else")
    result = w_agent.call("fs.writeFile", {"path": str(target),
                                           "content": "mine",
                                           "ifMtimeNs": seen})
    assert result["ok"] is False and result["conflict"] is True
    assert result["mtimeNs"] == os.stat(target).st_mtime_ns
    assert target.read_text() == "someone else", "a refused write wrote"
    assert not [n for n in os.listdir(tmp_path)
                if n.startswith(".scrutiny-write-")], "temp file left behind"


def test_conditional_write_refuses_a_file_that_vanished(w_agent, tmp_path):
    target = tmp_path / "f.txt"
    target.write_text("v1")
    seen = os.stat(target).st_mtime_ns
    target.unlink()
    result = w_agent.call("fs.writeFile", {"path": str(target),
                                           "content": "mine",
                                           "ifMtimeNs": seen})
    assert result["conflict"] is True and result["exists"] is False
    assert not target.exists()


def test_create_only_if_absent(w_agent, tmp_path):
    target = tmp_path / "new.txt"
    assert w_agent.call("fs.writeFile", {"path": str(target), "content": "a",
                                         "ifMtimeNs": 0})["ok"] is True
    again = w_agent.call("fs.writeFile", {"path": str(target), "content": "b",
                                          "ifMtimeNs": 0})
    assert again["conflict"] is True
    assert target.read_text() == "a"


def test_unconditional_write_is_unchanged(w_agent, tmp_path):
    target = tmp_path / "f.txt"
    target.write_text("v1")
    result = w_agent.call("fs.writeFile", {"path": str(target),
                                           "content": "v2"})
    assert result["ok"] is True and target.read_text() == "v2"


# ---------------------------------------------------------------------
# ordering: a change is announced before the reply to what caused it
# ---------------------------------------------------------------------
def position(agent, predicate, since=0):
    for index, (method, params) in enumerate(agent.notifications[since:]):
        if method == "fs.changed" and predicate(params):
            return since + index
    return None


def test_own_write_is_announced_before_its_reply(w_agent, tmp_path):
    """No sleep, no wait: by the time `call` returns, the notification
    for the change it made has already been read off the wire."""
    listing(w_agent, tmp_path)
    for i in range(20):
        name = "w%02d.txt" % i
        mark = w_agent.notification_mark()
        w_agent.call("fs.writeFile", {"path": str(tmp_path / name),
                                      "content": "x"})
        assert position(w_agent, lambda p: name in names(p), mark) is not None, \
            "reply to write %d overtook its fs.changed" % i


def test_git_changes_are_announced_before_the_reply(agent_factory, tmp_path,
                                                    repo):
    agent = agent_factory(["--allow-root", str(tmp_path), "--allow-write",
                           "--git-exec-preset", "magit"])
    listing(agent, repo.path)
    listing(agent, os.path.join(repo.path, ".git"))
    with open(os.path.join(repo.path, "fresh.txt"), "w") as handle:
        handle.write("x")
    mark = agent.notification_mark()
    assert agent.call("git.exec", {"repoPath": repo.path,
                                   "args": ["add", "fresh.txt"]})["exitCode"] == 0
    assert position(agent,
                    lambda p: p.get("path") == os.path.join(repo.path, ".git")
                    and "index" in names(p), mark) is not None, \
        "the index changed and the reply arrived first"


def test_barrier_costs_nothing_without_watches(w_agent, tmp_path):
    started = time.monotonic()
    for i in range(50):
        w_agent.call("fs.writeFile", {"path": str(tmp_path / "n.txt"),
                                      "content": str(i)})
    assert time.monotonic() - started < 5
