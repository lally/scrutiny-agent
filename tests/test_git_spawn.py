"""`git.spawn`: a git command as a live process.

`git.exec` answers "what did it print"; this is for the commands a
person watches or takes part in -- a patch fed in on standard input, a
commit that stops to have its message written. It hands back a tunnel,
so the byte-pipe contract is the one `lsp.tunnel*` already has; what
is tested here is what is new: end-of-input, the exit status, the
policy being git.exec's, and signalling the editor a commit is waiting
on -- and nothing else.
"""
import base64
import os
import signal
import subprocess
import sys
import time
import zlib

import pytest

from scrutiny import INVALID_REQUEST, PERMISSION_DENIED
from scrutiny.fixtures import git


@pytest.fixture
def g_agent(agent_factory, tmp_path):
    return agent_factory(["--allow-root", str(tmp_path), "--allow-root", "/tmp",
                          "--allow-write", "--git-exec-preset", "magit"])


def spawn(agent, repo_path, args, env=None):
    params = {"repoPath": repo_path, "args": args}
    if env is not None:
        params["env"] = env
    mark = agent.notification_mark()
    result = agent.call("git.spawn", params)
    assert isinstance(result["tunnelId"], str) and result["pid"] > 1
    return result["tunnelId"], result["pid"], mark


def send(agent, tunnel_id, data=None, eof=False):
    params = {"tunnelId": tunnel_id}
    if data is not None:
        params["data"] = base64.b64encode(data).decode()
    if eof:
        params["eof"] = True
    agent.notify("lsp.tunnelSend", params)


def output(agent, tunnel_id, since):
    out = b""
    for method, params in list(agent.notifications[since:]):
        if method == "lsp.tunnelRecv" and params.get("tunnelId") == tunnel_id:
            data = base64.b64decode(params["data"])
            out += zlib.decompress(data) if params.get("z") else data
    return out


def finish(agent, tunnel_id, since, timeout=30):
    closed = agent.wait_notification(
        "lsp.tunnelClosed", lambda p: p.get("tunnelId") == tunnel_id,
        timeout=timeout, since=since)
    assert closed is not None, "the command never reported that it ended"
    return closed, output(agent, tunnel_id, since)


def test_advertised_with_git_exec(g_agent, agent):
    assert {"git.spawn", "git.signal"} <= set(g_agent.capabilities)
    assert "git.spawn" not in agent.capabilities


def test_output_and_exit_status(g_agent, repo):
    tid, _pid, mark = spawn(g_agent, repo.path, ["rev-parse", "HEAD"])
    closed, out = finish(g_agent, tid, mark)
    assert closed["exitCode"] == 0 and closed["reason"] == "exit"
    assert out.decode().strip() == repo.head_sha


def test_failure_is_reported_with_what_git_said(g_agent, repo):
    """stderr arrives on the same stream, as a terminal would show it."""
    tid, _pid, mark = spawn(g_agent, repo.path, ["rev-parse", "no-such-ref"])
    closed, out = finish(g_agent, tid, mark)
    assert closed["exitCode"] != 0
    assert b"no-such-ref" in out


def test_stdin_then_end_of_input(g_agent, repo):
    """Staging a hunk: a patch written to `git apply --cached -`."""
    with open(os.path.join(repo.path, "hunk.txt"), "w") as handle:
        handle.write("staged through a process\n")
    git(repo.path, "add", "-N", "hunk.txt")
    patch = git(repo.path, "diff", "--", "hunk.txt", capture=True) + "\n"
    tid, _pid, mark = spawn(g_agent, repo.path, ["apply", "--cached", "-"])
    # In pieces, the way a process filter delivers them.
    encoded = patch.encode()
    send(g_agent, tid, encoded[:10])
    send(g_agent, tid, encoded[10:], eof=True)
    closed, out = finish(g_agent, tid, mark)
    assert closed["exitCode"] == 0, out
    assert "hunk.txt" in git(repo.path, "diff", "--cached", "--name-only",
                             capture=True)


def test_end_of_input_alone(g_agent, repo):
    tid, _pid, mark = spawn(g_agent, repo.path, ["hash-object", "--stdin"])
    send(g_agent, tid, eof=True)
    closed, out = finish(g_agent, tid, mark)
    assert closed["exitCode"] == 0
    assert out.decode().strip() == "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391"


def test_a_finished_command_is_forgotten(g_agent, repo):
    tid, _pid, mark = spawn(g_agent, repo.path, ["rev-parse", "HEAD"])
    finish(g_agent, tid, mark)
    again = g_agent.notification_mark()
    send(g_agent, tid, b"late")
    note = g_agent.wait_notification(
        "lsp.tunnelClosed", lambda p: p.get("tunnelId") == tid,
        timeout=10, since=again)
    assert note is not None and note["reason"] == "unknown tunnel"


def test_changes_are_announced_before_the_exit(g_agent, repo):
    """The client refreshes when the command ends; what the command
    changed must already be known by then."""
    if "fs.watch" not in g_agent.hello["features"]:
        pytest.skip("no fs.watch on this platform (needs inotify)")
    g_agent.call("fs.listDirectory", {"path": os.path.join(repo.path, ".git"),
                                      "attributes": True, "watch": True})
    with open(os.path.join(repo.path, "c.txt"), "w") as handle:
        handle.write("x\n")
    tid, _pid, mark = spawn(g_agent, repo.path, ["add", "c.txt"])
    finish(g_agent, tid, mark)
    seen = [m for m, p in g_agent.notifications[mark:]
            if (m == "fs.changed" and any(e["name"] == "index"
                                          for e in p.get("entries", [])))
            or (m == "lsp.tunnelClosed" and p.get("tunnelId") == tid)]
    assert seen and seen[0] == "fs.changed", seen


# ---------------------------------------------------------------------
# policy: git.exec's, exactly
# ---------------------------------------------------------------------
def test_policy_is_shared_with_git_exec(agent_factory, tmp_path, repo):
    agent = agent_factory(["--allow-root", str(tmp_path),
                           "--git-exec-preset", "read-only"])
    for args in (["commit", "-m", "x"], ["-c", "core.pager=id", "log"],
                 ["diff", "--no-index", "/dev/null", "/etc/hostname"],
                 ["branch", "sneaky"]):
        assert agent.call_expect_error(
            "git.spawn", {"repoPath": repo.path,
                          "args": args}).code == PERMISSION_DENIED, args
    assert agent.call_expect_error(
        "git.spawn", {"repoPath": "/etc",
                      "args": ["status"]}).code == PERMISSION_DENIED


def test_editor_needs_the_magit_preset(agent_factory, tmp_path, repo):
    agent = agent_factory(["--allow-root", str(tmp_path),
                           "--git-exec-preset", "read-only"])
    assert agent.call_expect_error(
        "git.spawn", {"repoPath": repo.path, "args": ["log", "-1"],
                      "env": {"GIT_EDITOR": "true"}}).code == PERMISSION_DENIED


def test_requires_arguments(g_agent, repo):
    assert g_agent.call_expect_error(
        "git.spawn", {"repoPath": repo.path}).code == INVALID_REQUEST
    assert g_agent.call_expect_error(
        "git.spawn", {"args": ["status"]}).code == INVALID_REQUEST


# ---------------------------------------------------------------------
# a commit that waits for its message
# ---------------------------------------------------------------------
# What with-editor gives git as its editor, in miniature: announce the
# file and the process to signal, then wait. USR1 means "saved, go on".
SLEEPING_EDITOR = ("sh -c 'printf \"EDIT %s %s\\n\" $$ \"$0\"; "
                   "sleep 60 & sleep=$!; "
                   "trap \"kill $sleep; exit 0\" USR1; "
                   "trap \"kill $sleep; exit 1\" USR2; "
                   "wait $sleep' ")


def wait_for_editor(agent, tid, mark, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for line in output(agent, tid, mark).decode("utf-8", "replace") \
                                            .splitlines():
            if line.startswith("EDIT "):
                _tag, pid, path = line.split(" ", 2)
                return int(pid), path
        time.sleep(0.05)
    raise AssertionError("the editor never announced itself: %r"
                         % output(agent, tid, mark))


def test_commit_through_a_waiting_editor(g_agent, repo):
    with open(os.path.join(repo.path, "msg.txt"), "w") as handle:
        handle.write("x\n")
    git(repo.path, "add", "msg.txt")
    tid, _pid, mark = spawn(g_agent, repo.path, ["commit"],
                            env={"GIT_EDITOR": SLEEPING_EDITOR,
                                 "GIT_AUTHOR_NAME": "T",
                                 "GIT_AUTHOR_EMAIL": "t@x",
                                 "GIT_COMMITTER_NAME": "T",
                                 "GIT_COMMITTER_EMAIL": "t@x"})
    editor_pid, message_file = wait_for_editor(g_agent, tid, mark)
    # The client writes the message through the file surface...
    g_agent.call("fs.writeFile", {"path": message_file,
                                  "content": "written from the client\n"})
    # ...and tells the editor it is done.
    assert g_agent.call("git.signal", {"pid": editor_pid,
                                       "signal": "USR1"})["ok"] is True
    closed, out = finish(g_agent, tid, mark)
    assert closed["exitCode"] == 0, out
    assert git(repo.path, "log", "-1", "--format=%s", capture=True) == \
        "written from the client"


def test_exit_is_reported_even_if_something_keeps_the_pipe(g_agent, repo):
    """A hook or an editor that leaves a process running in the
    background holds the output pipe open. The commit is finished all
    the same, and the client must hear so now, not when that process
    eventually exits."""
    with open(os.path.join(repo.path, "bg.txt"), "w") as handle:
        handle.write("x\n")
    git(repo.path, "add", "bg.txt")
    started = time.monotonic()
    tid, _pid, mark = spawn(
        g_agent, repo.path, ["commit"],
        env={"GIT_EDITOR": "sh -c 'sleep 30 & echo from-editor > \"$0\"' ",
             "GIT_AUTHOR_NAME": "T", "GIT_AUTHOR_EMAIL": "t@x",
             "GIT_COMMITTER_NAME": "T", "GIT_COMMITTER_EMAIL": "t@x"})
    closed, out = finish(g_agent, tid, mark, timeout=20)
    assert closed["exitCode"] == 0, out
    assert time.monotonic() - started < 10
    assert git(repo.path, "log", "-1", "--format=%s", capture=True) == \
        "from-editor"


def test_end_of_input_ends_the_agent_even_with_a_waiting_editor(
        agent_factory, repo):
    """The client's link drops while a commit sits in its editor.  The
    agent must still exit on stdin EOF -- and take the editor with it.
    Shutdown used to join the tunnel's threads while holding the tunnel
    mutex that the output reader takes on its way out, so the agent
    hung forever, one per dropped session, on the host."""
    with open(os.path.join(repo.path, "eof.txt"), "w") as handle:
        handle.write("x\n")
    git(repo.path, "add", "eof.txt")
    agent = agent_factory(["--allow-root", repo.path, "--allow-write",
                           "--git-exec-preset", "magit"])
    tid, _pid, mark = spawn(agent, repo.path, ["commit"],
                            env={"GIT_EDITOR": SLEEPING_EDITOR,
                                 "GIT_AUTHOR_NAME": "T",
                                 "GIT_AUTHOR_EMAIL": "t@x",
                                 "GIT_COMMITTER_NAME": "T",
                                 "GIT_COMMITTER_EMAIL": "t@x"})
    editor_pid, _path = wait_for_editor(agent, tid, mark)
    agent.proc.stdin.close()
    assert agent.proc.wait(timeout=10) == 0
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            os.kill(editor_pid, 0)
        except ProcessLookupError:
            break
        time.sleep(0.05)
    else:
        raise AssertionError("the editor outlived the agent")


def test_commit_can_be_abandoned(g_agent, repo):
    with open(os.path.join(repo.path, "abandon.txt"), "w") as handle:
        handle.write("x\n")
    git(repo.path, "add", "abandon.txt")
    before = git(repo.path, "rev-parse", "HEAD", capture=True)
    tid, _pid, mark = spawn(g_agent, repo.path, ["commit"],
                            env={"GIT_EDITOR": SLEEPING_EDITOR})
    editor_pid, _message_file = wait_for_editor(g_agent, tid, mark)
    g_agent.call("git.signal", {"pid": editor_pid, "signal": "USR2"})
    closed, _out = finish(g_agent, tid, mark)
    assert closed["exitCode"] != 0
    assert git(repo.path, "rev-parse", "HEAD", capture=True) == before


def test_closing_the_tunnel_takes_the_editor_with_it(g_agent, repo):
    with open(os.path.join(repo.path, "kill.txt"), "w") as handle:
        handle.write("x\n")
    git(repo.path, "add", "kill.txt")
    tid, _pid, mark = spawn(g_agent, repo.path, ["commit"],
                            env={"GIT_EDITOR": SLEEPING_EDITOR})
    editor_pid, _ = wait_for_editor(g_agent, tid, mark)
    g_agent.call("lsp.tunnelClose", {"tunnelId": tid})
    closed = g_agent.wait_notification(
        "lsp.tunnelClosed", lambda p: p.get("tunnelId") == tid,
        timeout=20, since=mark)
    assert closed is not None
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            os.kill(editor_pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.05)
    pytest.fail("the editor outlived the command that started it")


def test_signal_only_reaches_our_own_processes(g_agent, repo):
    """Not a way to signal anything else the user is running."""
    bystander = subprocess.Popen([sys.executable, "-c",
                                  "import time; time.sleep(60)"])
    try:
        assert g_agent.call_expect_error(
            "git.signal", {"pid": bystander.pid,
                           "signal": "TERM"}).code == PERMISSION_DENIED
        assert g_agent.call_expect_error(
            "git.signal", {"pid": 1, "signal": "TERM"}).code == \
            PERMISSION_DENIED
        assert g_agent.call_expect_error(
            "git.signal", {"pid": os.getpid(),
                           "signal": "USR1"}).code == PERMISSION_DENIED
        time.sleep(0.2)
        assert bystander.poll() is None, "a bystander was signalled"
    finally:
        bystander.kill()
        bystander.wait()


def test_signal_validates_its_arguments(g_agent):
    assert g_agent.call_expect_error(
        "git.signal", {"signal": "USR1"}).code == INVALID_REQUEST
    assert g_agent.call_expect_error(
        "git.signal", {"pid": 4242, "signal": "STOP"}).code == INVALID_REQUEST


def _threads(agent):
    return len(os.listdir("/proc/%d/task" % agent.proc.pid))


@pytest.mark.skipif(not os.path.isdir("/proc/self/task"), reason="needs /proc")
def test_finished_commands_are_freed(g_agent, repo):
    """A command that ends by itself -- which is all of them -- must not
    leave its threads behind waiting for input that will never come."""
    tid, _pid, mark = spawn(g_agent, repo.path, ["rev-parse", "HEAD"])
    finish(g_agent, tid, mark)
    base = _threads(g_agent)
    for _ in range(25):
        tid, _pid, mark = spawn(g_agent, repo.path, ["rev-parse", "HEAD"])
        finish(g_agent, tid, mark)          # note: no end-of-input sent
    time.sleep(0.3)
    tid, _pid, mark = spawn(g_agent, repo.path, ["rev-parse", "HEAD"])
    finish(g_agent, tid, mark)              # reaping happens on the next one
    assert _threads(g_agent) <= base + 6


def test_input_nobody_reads_does_not_wedge_the_connection(g_agent, repo):
    """More input than the queue holds, to a command that never reads
    it: the connection's reader blocks for room. When the command ends
    it must be let go, or the agent never reads another request."""
    with open(os.path.join(repo.path, "f.txt"), "w") as f:
        f.write("x\n")
    git(repo.path, "add", "f.txt")
    tid, _pid, mark = spawn(g_agent, repo.path, ["commit"],
                            env={"GIT_EDITOR": "sleep 2; false"})
    block = b"\0" * (32 * 1024)
    for _ in range(400):                    # 12.8 MiB against an 8 MiB queue
        send(g_agent, tid, block)
    closed, _out = finish(g_agent, tid, mark)
    assert closed["exitCode"] != 0
    result = g_agent.call("git.exec", {"repoPath": repo.path,
                                       "args": ["rev-parse", "HEAD"]})
    assert result["exitCode"] == 0
