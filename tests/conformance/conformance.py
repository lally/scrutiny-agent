#!/usr/bin/env python3
"""scrutiny-agent wire-protocol conformance suite.

Verifies that a built agent binary implements the wire API it
advertises: every capability listed in the meta.hello response must
have a passing conformance check here, and the suite fails if the
agent advertises a capability this file does not know how to verify
(or vice versa). Protocol-level behavior -- framing, the handshake,
frame-cap negotiation, chunked streaming, wire-level cancellation,
the error model, the fs sandbox, and the credential-broker
round-trip -- is exercised alongside the per-method checks.

Everything runs against hermetic, throwaway fixtures (git init repos,
temp trees, file:// clones). NO network, NO GitHub. Python 3 stdlib
only. Every wait is bounded so a hung agent fails fast.

The normative protocol description lives in docs/protocol.md; the
method-level assertions here mirror it.

This suite answers one question: does the binary implement the wire
API it advertises? For behavior *depth* -- framing edge cases,
chunking, lane scheduling, sandbox escapes, the credential broker,
per-method semantics against real fixtures -- see the pytest suite
next to it (`tests/run-tests.sh`), which shares no code with this
file on purpose: an agreement between two independent clients is
worth more than one client agreeing with itself.

Usage: conformance.py /path/to/scrutiny-agent [--agent-version X.Y.Z]

Exit 0 = fully conformant; 1 = at least one failed check; 2 = usage.
"""
import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid

# Stable error codes (docs/protocol.md "Error model").
INTERNAL = 1000
NOT_FOUND = 1001
INVALID_REQUEST = 1002
GIT_FAILED = 1003
LSP_FAILED = 1004
PERMISSION_DENIED = 1005
VERSION_MISMATCH = 1006
CANCELLED = 1007


class AgentError(Exception):
    def __init__(self, message, code=None):
        super().__init__(message)
        self.code = code


class Agent:
    """One agent process + a reader thread that reassembles chunked
    responses, records notifications, and auto-answers cred.request."""

    def __init__(self, path, extra_args=None):
        argv = [path, "--rpc-stdio"]
        argv.extend(extra_args or [])
        self.proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        self._wlock = threading.Lock()
        self._id = 0
        self._pending = {}            # id(str) -> [Event, response|None]
        self._chunks = {}             # id(str) -> {seq: data}
        self._secrets = {}            # authOpId -> secret to provide
        self._dead = None
        self.notifications = []       # [(method, params)] except rpc.chunk
        self._note_cv = threading.Condition()
        self.null_id_errors = []      # error envelopes with id null
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    # ---- framing ----------------------------------------------------
    def send_raw(self, payload_bytes):
        """Send one frame with an arbitrary (possibly invalid) body."""
        with self._wlock:
            try:
                self.proc.stdin.write(
                    b"Content-Length: %d\r\n\r\n" % len(payload_bytes)
                    + payload_bytes)
                self.proc.stdin.flush()
            except (BrokenPipeError, ValueError) as e:
                raise AgentError("agent stdin closed: %s" % e)

    def _send(self, obj):
        self.send_raw(json.dumps(obj).encode())

    def _read_frame(self):
        out = self.proc.stdout
        hdr = b""
        while b"\r\n\r\n" not in hdr:
            ch = out.read(1)
            if not ch:
                return None
            hdr += ch
        n = 0
        for line in hdr.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                n = int(line.split(b":", 1)[1].strip())
        body = b""
        while len(body) < n:
            part = out.read(n - len(body))
            if not part:
                return None
            body += part
        return json.loads(body)

    def _read_loop(self):
        try:
            while True:
                msg = self._read_frame()
                if msg is None:
                    break
                self._dispatch(msg)
        except Exception as e:           # noqa: BLE001 - surfaced to callers
            self._dead = e
        finally:
            self._dead = self._dead or AgentError("agent exited")
            for ev, _ in self._pending.values():
                ev.set()

    def _dispatch(self, msg):
        method = msg.get("method")
        if method == "rpc.chunk":
            p = msg["params"]
            key = str(p["id"])
            self._chunks.setdefault(key, {})[p["seq"]] = p["data"]
            return
        if method == "cred.request":
            with self._note_cv:
                self.notifications.append((method, msg.get("params", {})))
                self._note_cv.notify_all()
            self._on_cred_request(msg["params"])
            return
        if method is not None:
            with self._note_cv:
                self.notifications.append((method, msg.get("params", {})))
                self._note_cv.notify_all()
            return
        if msg.get("id") is None and "error" in msg:
            self.null_id_errors.append(msg["error"])
            with self._note_cv:
                self._note_cv.notify_all()
            return
        key = str(msg.get("id"))
        result = msg.get("result")
        if isinstance(result, dict) and result.get("streamed") is True:
            parts = self._chunks.pop(key, {})
            b64 = "".join(parts[s] for s in sorted(parts))
            msg = json.loads(base64.b64decode(b64))
            key = str(msg.get("id"))
        slot = self._pending.get(key)
        if slot:
            slot[1] = msg
            slot[0].set()

    # ---- credential round-trip -------------------------------------
    def _on_cred_request(self, params):
        secret = self._secrets.get(params.get("authOpId"), "")
        with self._wlock:
            self._id += 1
            rid = self._id
        self._send({"jsonrpc": "2.0", "id": rid, "method": "cred.provide",
                    "params": {"credId": params["credId"], "value": secret}})

    def expect_secret(self, auth_op_id, secret):
        self._secrets[auth_op_id] = secret

    # ---- request/response ------------------------------------------
    def call_async(self, method, params=None):
        """Send a request; return (id, wait_fn)."""
        if self._dead:
            raise AgentError("agent not running: %s" % self._dead)
        with self._wlock:
            self._id += 1
            rid = self._id
        ev = threading.Event()
        slot = [ev, None]
        self._pending[str(rid)] = slot

        def wait(timeout=20):
            if not ev.wait(timeout):
                raise AgentError("timeout (%ss) waiting for %s"
                                 % (timeout, method))
            self._pending.pop(str(rid), None)
            msg = slot[1]
            if msg is None:
                raise AgentError("agent died during %s: %s"
                                 % (method, self._dead))
            if "error" in msg:
                e = msg["error"]
                raise AgentError("%s -> rpc error %s: %s"
                                 % (method, e.get("code"), e.get("message")),
                                 code=e.get("code"))
            return msg.get("result")

        self._send({"jsonrpc": "2.0", "id": rid, "method": method,
                    "params": params or {}})
        return rid, wait

    def call(self, method, params=None, timeout=20):
        _, wait = self.call_async(method, params)
        return wait(timeout)

    def call_error(self, method, params=None, timeout=20):
        """Expect an error; return the AgentError (with .code)."""
        try:
            self.call(method, params, timeout)
        except AgentError as e:
            return e
        raise AssertionError("%s should have failed" % method)

    def notify(self, method, params=None):
        self._send({"jsonrpc": "2.0", "method": method,
                    "params": params or {}})

    def cancel(self, request_id):
        self.notify("$/cancelRequest", {"id": request_id})

    def wait_notification(self, method, pred=None, timeout=10):
        """Block until a notification `method` matching pred arrives."""
        deadline = time.time() + timeout
        with self._note_cv:
            while True:
                for m, p in self.notifications:
                    if m == method and (pred is None or pred(p)):
                        return p
                remain = deadline - time.time()
                if remain <= 0:
                    return None
                self._note_cv.wait(remain)

    def alive(self):
        return self.proc.poll() is None and self._dead is None

    def close(self):
        try:
            self.proc.terminate()
        except Exception:               # noqa: BLE001
            pass


# ---- assertions / runner -------------------------------------------
_FAILS = []


def check(cond, what):
    status = "PASS" if cond else "FAIL"
    print("  [%s] %s" % (status, what))
    if not cond:
        _FAILS.append(what)


def section(name):
    print("== %s ==" % name)


# ---- fixtures -------------------------------------------------------
GIT_ENV = dict(os.environ,
               GIT_AUTHOR_NAME="conf", GIT_AUTHOR_EMAIL="conf@example.com",
               GIT_COMMITTER_NAME="conf",
               GIT_COMMITTER_EMAIL="conf@example.com",
               GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null")


def git(cwd, *args, out=False):
    r = subprocess.run(["git", "-C", cwd, *args], check=True, env=GIT_ENV,
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return r.stdout.decode().strip() if out else None


class Fixtures:
    """Temp dirs, cleaned up at the end of the run."""

    def __init__(self):
        self.dirs = []

    def tempdir(self, prefix):
        d = tempfile.mkdtemp(prefix=prefix)
        self.dirs.append(d)
        return d

    def repo(self):
        """Hermetic repo: two commits on `work`, no remote.
        Returns (path, headSha, parentSha)."""
        d = self.tempdir("conf-repo-")
        git(d, "init", "-q")
        git(d, "checkout", "-q", "-b", "work")
        with open(os.path.join(d, "file.txt"), "w") as f:
            f.write("line1\nline2\n")
        git(d, "add", "file.txt")
        git(d, "commit", "-q", "-m", "first")
        with open(os.path.join(d, "file.txt"), "w") as f:
            f.write("line1\nCHANGED\nline3\n")
        git(d, "commit", "-q", "-am", "second")
        head = git(d, "rev-parse", "HEAD", out=True)
        parent = git(d, "rev-parse", "HEAD~1", out=True)
        return d, head, parent

    def repo_with_upstream(self):
        """(clone, upstream): clone's `work` tracks origin/work."""
        up, _, _ = self.repo()
        d = self.tempdir("conf-clone-")
        subprocess.run(["git", "clone", "-q", up, d], check=True, env=GIT_ENV,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return d, up

    def tree(self):
        """base/{alpha/ beta/ delta.txt gamma.txt link-to-alpha->alpha}."""
        base = self.tempdir("conf-tree-")
        os.makedirs(os.path.join(base, "alpha"))
        os.makedirs(os.path.join(base, "beta"))
        open(os.path.join(base, "gamma.txt"), "w").write("g")
        open(os.path.join(base, "delta.txt"), "w").write("d")
        os.symlink(os.path.join(base, "alpha"),
                   os.path.join(base, "link-to-alpha"))
        return base

    def cleanup(self):
        for d in self.dirs:
            shutil.rmtree(d, ignore_errors=True)


class Ctx:
    def __init__(self, agent, fixtures, agent_path, agent_version, log_path):
        self.a = agent
        self.fx = fixtures
        self.agent_path = agent_path
        self.agent_version = agent_version
        self.log_path = log_path


# ---- capability registry ---------------------------------------------
# Every capability the agent advertises in meta.hello MUST have an
# entry here; the run fails otherwise (and fails if an entry names a
# capability the agent stopped advertising). This is the "implements
# the wire API it says it does" gate.
CONFORMANCE = {}


def conformance(*caps):
    def deco(fn):
        for c in caps:
            CONFORMANCE.setdefault(c, []).append(fn)
        return fn
    return deco


HEX40 = set("0123456789abcdef")


def is_sha(s):
    return isinstance(s, str) and len(s) == 40 and set(s) <= HEX40


# ---- git.* -----------------------------------------------------------
@conformance("git.headSha")
def t_head_sha(ctx):
    repo, head, _ = ctx.fx.repo()
    r = ctx.a.call("git.headSha", {"path": repo})
    check(r.get("headSha") == head, "git.headSha matches rev-parse HEAD")
    check(ctx.a.call_error("git.headSha", {}).code == INVALID_REQUEST,
          "git.headSha missing 'path' -> INVALID_REQUEST 1002")
    check(ctx.a.call_error(
        "git.headSha", {"path": "/no/such/conf/repo"}).code == NOT_FOUND,
        "git.headSha bad repo -> NOT_FOUND 1001")


@conformance("git.repoMetadata")
def t_repo_metadata(ctx):
    repo, head, _ = ctx.fx.repo()
    m = ctx.a.call("git.repoMetadata", {"path": repo})
    check(m.get("headSha") == head, "repoMetadata.headSha == HEAD")
    check(m.get("currentBranch") == "work", "repoMetadata.currentBranch")
    check(m.get("isBare") is False, "repoMetadata.isBare false")
    check(m.get("hasUncommittedChanges") is False,
          "clean tree -> hasUncommittedChanges false")
    with open(os.path.join(repo, "file.txt"), "a") as f:
        f.write("dirty\n")
    m2 = ctx.a.call("git.repoMetadata", {"path": repo})
    check(m2.get("hasUncommittedChanges") is True,
          "dirty tree -> hasUncommittedChanges true")


@conformance("git.remotes")
def t_remotes(ctx):
    repo, _, _ = ctx.fx.repo()
    check(ctx.a.call("git.remotes", {"path": repo}).get("remotes") == [],
          "fresh init -> no remotes")
    git(repo, "remote", "add", "origin", "https://example.com/x.git")
    rl = ctx.a.call("git.remotes", {"path": repo})["remotes"]
    check(len(rl) == 1 and rl[0]["name"] == "origin"
          and rl[0]["url"] == "https://example.com/x.git"
          and rl[0]["pushUrl"] == rl[0]["url"],
          "remote add -> { name, url, pushUrl (falls back to url) }")


@conformance("git.branches")
def t_branches(ctx):
    repo, head, _ = ctx.fx.repo()
    br = ctx.a.call("git.branches",
                    {"path": repo, "local": True, "remote": False})
    work = [b for b in br.get("branches", []) if b.get("name") == "work"]
    check(len(work) == 1, "branches includes 'work' exactly once")
    if work:
        b = work[0]
        check(b.get("refname") == "refs/heads/work", "branch refname")
        check(b.get("targetOid") == head, "branch targetOid == HEAD")
        check(b.get("isHead") is True, "checked-out branch isHead")
        check(b.get("isRemote") is False, "local branch isRemote false")


@conformance("git.commits")
def t_commits(ctx):
    repo, head, parent = ctx.fx.repo()
    cs = ctx.a.call("git.commits", {"path": repo})["commits"]
    check(len(cs) == 2, "two commits returned")
    check(cs[0]["oid"] == head and cs[1]["oid"] == parent,
          "newest-first order")
    c = cs[0]
    check(c["summary"] == "second" and "second" in c["message"],
          "commit summary/message")
    check(c["authorName"] == "conf" and "@" in c["authorEmail"],
          "commit author fields")
    check(isinstance(c["authorTime"], int) and c["authorTime"] > 0,
          "authorTime epoch seconds")
    check(c["parentOids"] == [parent], "parentOids link to parent")
    check(c["shortOid"] == head[:len(c["shortOid"])] and
          len(c["shortOid"]) >= 7, "shortOid prefixes oid")
    one = ctx.a.call("git.commits", {"path": repo, "limit": 1})["commits"]
    check(len(one) == 1, "limit=1 respected")


@conformance("git.aheadBehind")
def t_ahead_behind(ctx):
    clone, _ = ctx.fx.repo_with_upstream()
    r = ctx.a.call("git.aheadBehind", {"path": clone, "branch": "work"})
    check(r.get("ahead") == 0 and r.get("behind") == 0,
          "fresh clone: ahead 0 / behind 0")
    with open(os.path.join(clone, "new.txt"), "w") as f:
        f.write("x\n")
    git(clone, "add", "new.txt")
    git(clone, "commit", "-q", "-m", "local-only")
    r2 = ctx.a.call("git.aheadBehind", {"path": clone, "branch": "work"})
    check(r2.get("ahead") == 1 and r2.get("behind") == 0,
          "local commit: ahead 1 / behind 0")
    check(ctx.a.call_error("git.aheadBehind",
                           {"path": clone}).code == INVALID_REQUEST,
          "missing 'branch' -> INVALID_REQUEST 1002")


@conformance("git.checkoutBranch")
def t_checkout_branch(ctx):
    repo, _, _ = ctx.fx.repo()
    git(repo, "branch", "feature")
    r = ctx.a.call("git.checkoutBranch", {"path": repo, "branch": "feature"})
    check(r == {}, "checkout success -> empty result object")
    m = ctx.a.call("git.repoMetadata", {"path": repo})
    check(m.get("currentBranch") == "feature", "HEAD moved to 'feature'")
    e = ctx.a.call_error("git.checkoutBranch",
                         {"path": repo, "branch": "no-such-branch"})
    check(e.code == INTERNAL and str(e),
          "unknown branch -> INTERNAL 1000 with libgit2 reason")


def _check_file_diff_shape(d, label):
    check(isinstance(d.get("status"), int), "%s: status is int" % label)
    check(isinstance(d.get("oldPath"), str) and
          isinstance(d.get("newPath"), str), "%s: old/newPath" % label)
    check(isinstance(d.get("patch"), str), "%s: patch string" % label)
    hunks = d.get("hunks")
    check(isinstance(hunks, list) and hunks, "%s: hunks present" % label)
    if hunks:
        h = hunks[0]
        for k in ("oldStart", "oldLines", "newStart", "newLines"):
            check(isinstance(h.get(k), int), "%s: hunk.%s int" % (label, k))
        lines = h.get("lines")
        check(isinstance(lines, list) and lines, "%s: hunk lines" % label)
        if lines:
            ln = lines[0]
            check(isinstance(ln.get("origin"), str) and
                  len(ln["origin"]) == 1, "%s: line origin 1-char" % label)
            check("content" in ln and "oldLineNo" in ln and
                  "newLineNo" in ln, "%s: line fields" % label)


@conformance("git.diffForCommit")
def t_diff_for_commit(ctx):
    repo, head, _ = ctx.fx.repo()
    diffs = ctx.a.call("git.diffForCommit",
                       {"path": repo, "sha": head})["diffs"]
    check(len(diffs) == 1 and diffs[0]["newPath"] == "file.txt",
          "HEAD diff covers file.txt")
    if diffs:
        _check_file_diff_shape(diffs[0], "diffForCommit")
        check("CHANGED" in diffs[0]["patch"], "patch carries the change")
    check(ctx.a.call_error("git.diffForCommit",
                           {"path": repo}).code == INVALID_REQUEST,
          "missing 'sha' -> INVALID_REQUEST 1002")
    empty = ctx.a.call("git.diffForCommit",
                       {"path": repo, "sha": "0" * 40})["diffs"]
    check(empty == [], "unknown sha -> empty diffs (documented parity)")


@conformance("git.workingTreeDiff")
def t_working_tree_diff(ctx):
    repo, _, _ = ctx.fx.repo()
    check(ctx.a.call("git.workingTreeDiff", {"path": repo})["diffs"] == [],
          "clean tree -> empty workingTreeDiff")
    with open(os.path.join(repo, "file.txt"), "a") as f:
        f.write("workdir edit\n")
    diffs = ctx.a.call("git.workingTreeDiff", {"path": repo})["diffs"]
    check(len(diffs) == 1 and diffs[0]["newPath"] == "file.txt",
          "unstaged edit appears in workingTreeDiff")
    if diffs:
        _check_file_diff_shape(diffs[0], "workingTreeDiff")


@conformance("git.stagedDiff")
def t_staged_diff(ctx):
    repo, _, _ = ctx.fx.repo()
    check(ctx.a.call("git.stagedDiff", {"path": repo})["diffs"] == [],
          "clean tree -> empty stagedDiff")
    with open(os.path.join(repo, "file.txt"), "a") as f:
        f.write("staged edit\n")
    git(repo, "add", "file.txt")
    staged = ctx.a.call("git.stagedDiff", {"path": repo})["diffs"]
    check(len(staged) == 1 and staged[0]["newPath"] == "file.txt",
          "staged edit appears in stagedDiff")
    if staged:
        _check_file_diff_shape(staged[0], "stagedDiff")
    check(ctx.a.call("git.workingTreeDiff", {"path": repo})["diffs"] == [],
          "fully staged edit leaves workingTreeDiff empty")


@conformance("git.showFile")
def t_show_file(ctx):
    repo, head, parent = ctx.fx.repo()
    sf = ctx.a.call("git.showFile",
                    {"path": repo, "sha": head, "file": "file.txt"})
    check(sf.get("content") == "line1\nCHANGED\nline3\n",
          "showFile HEAD:file.txt byte-exact")
    old = ctx.a.call("git.showFile",
                     {"path": repo, "sha": parent, "file": "file.txt"})
    check(old.get("content") == "line1\nline2\n",
          "showFile parent:file.txt is the first version")
    miss = ctx.a.call("git.showFile",
                      {"path": repo, "sha": head, "file": "nope.txt"})
    check(miss.get("content") is None, "missing path -> null content")
    check(ctx.a.call_error("git.showFile",
                           {"path": repo}).code == INVALID_REQUEST,
          "missing params -> INVALID_REQUEST 1002")


@conformance("git.diff")
def t_git_diff(ctx):
    repo, head, parent = ctx.fx.repo()
    gd = ctx.a.call("git.diff", {"path": repo, "from": parent,
                                 "to": head, "file": "file.txt"})
    check(isinstance(gd.get("diff"), str) and "CHANGED" in gd["diff"],
          "git.diff parent..head shows the change")
    check(ctx.a.call_error("git.diff",
                           {"path": repo}).code == INVALID_REQUEST,
          "missing params -> INVALID_REQUEST 1002")


@conformance("git.isAncestor")
def t_is_ancestor(ctx):
    repo, head, parent = ctx.fx.repo()
    check(ctx.a.call("git.isAncestor",
                     {"path": repo, "ancestor": parent,
                      "descendant": head}).get("isAncestor") is True,
          "parent -> head is ancestor")
    check(ctx.a.call("git.isAncestor",
                     {"path": repo, "ancestor": head,
                      "descendant": parent}).get("isAncestor") is False,
          "head -> parent is not")


@conformance("git.clone")
def t_git_clone(ctx):
    upstream, head, _ = ctx.fx.repo()
    install = ctx.fx.tempdir("conf-install-")
    r = ctx.a.call("git.clone",
                   {"fullName": "conf/c1", "cloneURL": upstream,
                    "installDir": install, "authOpId": "op-clone"},
                   timeout=60)
    check(r.get("localPath") == os.path.join(install, "conf_c1"),
          "clone lands at <installDir>/<owner>_<repo>")
    check(isinstance(r.get("lastFetched"), int), "clone reports lastFetched")
    m = ctx.a.call("git.repoMetadata", {"path": r["localPath"]})
    check(m.get("headSha") == head, "cloned repo HEAD matches upstream")
    e = ctx.a.call_error("git.clone",
                         {"fullName": "conf/bad", "cloneURL": "/no/such/up",
                          "installDir": install, "authOpId": "op"},
                         timeout=60)
    check(e.code == GIT_FAILED, "failed clone -> GIT_FAILED 1003")
    check(ctx.a.call_error("git.clone", {}).code == INVALID_REQUEST,
          "missing params -> INVALID_REQUEST 1002")


@conformance("git.fetch")
def t_git_fetch(ctx):
    clone, _ = ctx.fx.repo_with_upstream()
    r = ctx.a.call("git.fetch", {"repoPath": clone, "authOpId": "op-f"},
                   timeout=60)
    check(r.get("ok") is True and isinstance(r.get("lastFetched"), int),
          "fetch on a healthy clone -> ok + lastFetched")
    e = ctx.a.call_error("git.fetch",
                         {"repoPath": ctx.fx.tempdir("conf-notrepo-"),
                          "authOpId": "op"}, timeout=60)
    check(e.code == GIT_FAILED, "fetch outside a repo -> GIT_FAILED 1003")


@conformance("git.ensureRepository")
def t_ensure_repository(ctx):
    upstream, head, _ = ctx.fx.repo()
    install = ctx.fx.tempdir("conf-ensure-")
    r1 = ctx.a.call("git.ensureRepository",
                    {"fullName": "conf/e1", "cloneURL": upstream,
                     "installDir": install, "authOpId": "op-e"},
                    timeout=60)
    check(os.path.isdir(os.path.join(r1["localPath"], ".git")),
          "absent repo -> cloned")
    r2 = ctx.a.call("git.ensureRepository",
                    {"fullName": "conf/e1", "cloneURL": upstream,
                     "installDir": install, "authOpId": "op-e"},
                    timeout=60)
    check(r2.get("localPath") == r1.get("localPath"),
          "present repo -> fetched in place (same localPath)")


@conformance("git.exec")
def t_git_exec(ctx):
    repo, head, _ = ctx.fx.repo()
    r = ctx.a.call("git.exec",
                   {"repoPath": repo, "args": ["rev-parse", "HEAD"]})
    check(r.get("stdout", "").strip() == head,
          "git.exec rev-parse HEAD matches the repository")
    check(r.get("exitCode") == 0 and r.get("truncated") is False,
          "git.exec result shape { exitCode, stdout, stderr, truncated }")
    bad = ctx.a.call("git.exec",
                     {"repoPath": repo, "args": ["rev-parse", "no-such-ref"]})
    check(bad.get("exitCode") != 0 and bad.get("stderr"),
          "non-zero exit is reported as data, with stderr")
    # The allowlist, and the argument scan that makes it meaningful.
    check(ctx.a.call_error(
        "git.exec", {"repoPath": repo,
                     "args": ["commit", "-m", "x"]}).code == PERMISSION_DENIED,
        "subcommand outside the allowlist -> PERMISSION_DENIED 1005")
    check(ctx.a.call_error(
        "git.exec", {"repoPath": repo,
                     "args": ["-c", "core.pager=id", "log"]}).code ==
        PERMISSION_DENIED,
        "-c core.pager (arbitrary execution) -> PERMISSION_DENIED 1005")
    check(ctx.a.call_error(
        "git.exec", {"repoPath": repo,
                     "args": ["--git-dir=/etc", "status"]}).code ==
        PERMISSION_DENIED,
        "--git-dir relocation -> PERMISSION_DENIED 1005")
    check(ctx.a.call("git.exec",
                     {"repoPath": repo,
                      "args": ["-c", "color.ui=false", "status",
                               "--porcelain"]}).get("exitCode") == 0,
          "-c with an allowlisted key is accepted")
    check(ctx.a.call_error("git.exec", {"repoPath": repo}).code ==
          INVALID_REQUEST,
          "git.exec missing 'args' -> INVALID_REQUEST 1002")


# ---- fs.* ------------------------------------------------------------
@conformance("fs.readFile")
def t_fs_read_file(ctx):
    fd, fp = tempfile.mkstemp(suffix=".txt")
    body = "alpha\nUTF-8 éñ \U0001F600\n\ttab\t\n"
    os.write(fd, body.encode())
    os.close(fd)
    try:
        check(ctx.a.call("fs.readFile", {"path": fp}).get("content") == body,
              "fs.readFile round-trips byte-exact")
    finally:
        os.unlink(fp)
    check(ctx.a.call_error("fs.readFile",
                           {"path": "/no/such/conf/zzz"}).code == NOT_FOUND,
          "missing file -> NOT_FOUND 1001")
    check(ctx.a.call_error("fs.readFile", {}).code == INVALID_REQUEST,
          "missing 'path' -> INVALID_REQUEST 1002")


@conformance("fs.listDirectory")
def t_fs_list_directory(ctx):
    tree = ctx.fx.tree()
    r = ctx.a.call("fs.listDirectory", {"path": tree})
    names = [e["name"] for e in r["entries"]]
    check(names == ["alpha", "beta", "delta.txt", "gamma.txt",
                    "link-to-alpha"],
          "entries sorted, ./.. excluded (got %r)" % names)
    flags = {e["name"]: e["isDir"] for e in r["entries"]}
    check(flags.get("alpha") is True and flags.get("gamma.txt") is False,
          "isDir flags")
    check(flags.get("link-to-alpha") is True,
          "symlink-to-dir followed -> isDir true")
    check(r["path"].startswith("/"), "resolved path is absolute")
    home = ctx.a.call("fs.listDirectory", {"path": "."})["path"]
    check(home == os.path.realpath(os.path.expanduser("~")),
          "'.' canonicalizes to $HOME")
    check(ctx.a.call_error(
        "fs.listDirectory", {"path": "/no/such/conf/dir"}).code == NOT_FOUND,
        "missing dir -> NOT_FOUND 1001")


@conformance("fs.stat", "fs.statBatch")
def t_fs_stat(ctx):
    tree = ctx.fx.tree()
    st = ctx.a.call("fs.stat", {"path": os.path.join(tree, "gamma.txt")})
    check(st.get("exists") is True and st.get("isRegular") is True,
          "fs.stat reports an existing regular file")
    check(st.get("size") == 1 and isinstance(st.get("mtime"), int),
          "fs.stat carries size and mtime")
    check(ctx.a.call("fs.stat",
                     {"path": os.path.join(tree, "nope")}).get("exists")
          is False,
          "fs.stat on a missing path -> { exists: false }, not an error")
    link = ctx.a.call("fs.stat", {"path": os.path.join(tree, "link-to-alpha")})
    check(link.get("isSymlink") is True,
          "fs.stat describes a symlink rather than following it")
    batch = ctx.a.call("fs.statBatch",
                       {"paths": [os.path.join(tree, "gamma.txt"),
                                  os.path.join(tree, "nope"),
                                  "/etc/passwd"]})["stats"]
    check(len(batch) == 3, "fs.statBatch answers in the order asked")
    check(batch[0].get("exists") is True and batch[1].get("exists") is False,
          "fs.statBatch reports per-path results")
    check(batch[2].get("exists") is False,
          "fs.statBatch does not leak an out-of-roots path")
    entries = ctx.a.call("fs.listDirectory",
                         {"path": tree, "attributes": True})["entries"]
    check(all("size" in e and "mode" in e for e in entries),
          "fs.listDirectory attributes:true carries per-entry metadata")
    check(ctx.a.call_error("fs.stat", {}).code == INVALID_REQUEST,
          "fs.stat missing 'path' -> INVALID_REQUEST 1002")


@conformance("fs.writeFile", "fs.mkdir", "fs.delete", "fs.rename",
             "fs.copy", "fs.chmod")
def t_fs_write(ctx):
    base = ctx.fx.tempdir("conf-write-")
    target = os.path.join(base, "written.txt")
    r = ctx.a.call("fs.writeFile", {"path": target, "content": "hello\n"})
    check(r.get("ok") is True and r.get("size") == 6,
          "fs.writeFile reports ok and the byte count")
    check(open(target).read() == "hello\n", "the bytes reached the file")
    check([n for n in os.listdir(base) if "scrutiny-write" in n] == [],
          "the atomic write leaves no temp file behind")
    blob = bytes(range(256))
    bpath = os.path.join(base, "blob.bin")
    ctx.a.call("fs.writeFile",
               {"path": bpath,
                "contentBase64": base64.b64encode(blob).decode()})
    check(open(bpath, "rb").read() == blob,
          "contentBase64 round-trips arbitrary bytes")
    back = ctx.a.call("fs.readFile", {"path": bpath, "base64": True})
    check(base64.b64decode(back["contentBase64"]) == blob,
          "fs.readFile base64:true returns the same bytes")
    ctx.a.call("fs.mkdir", {"path": os.path.join(base, "a/b"),
                            "parents": True})
    check(os.path.isdir(os.path.join(base, "a/b")), "fs.mkdir parents:true")
    ctx.a.call("fs.rename", {"from": target,
                             "to": os.path.join(base, "moved.txt")})
    check(os.path.exists(os.path.join(base, "moved.txt")) and
          not os.path.exists(target), "fs.rename moves the file")
    ctx.a.call("fs.copy", {"from": os.path.join(base, "moved.txt"),
                           "to": os.path.join(base, "copy.txt")})
    check(os.path.exists(os.path.join(base, "copy.txt")), "fs.copy")
    ctx.a.call("fs.chmod", {"path": os.path.join(base, "copy.txt"),
                            "mode": 0o640})
    check((os.stat(os.path.join(base, "copy.txt")).st_mode & 0o777) == 0o640,
          "fs.chmod sets the mode")
    ctx.a.call("fs.delete", {"path": os.path.join(base, "a"),
                             "recursive": True})
    check(not os.path.exists(os.path.join(base, "a")),
          "fs.delete recursive:true")
    # The sandbox still governs every one of these.
    check(ctx.a.call_error("fs.writeFile",
                           {"path": "/etc/scrutiny-conformance",
                            "content": "x"}).code == PERMISSION_DENIED,
          "write outside the allowed roots -> PERMISSION_DENIED 1005")
    check(ctx.a.call_error("fs.writeFile", {"path": target}).code ==
          INVALID_REQUEST,
          "fs.writeFile without content -> INVALID_REQUEST 1002")


@conformance("fs.selftest")
def t_fs_selftest(ctx):
    r = ctx.a.call("fs.selftest", {})
    check(r.get("probe") == "/etc/passwd", "selftest probes /etc/passwd")
    check(isinstance(r.get("succeeded"), bool), "selftest reports a fact")
    check("errorCode" in r and "firstBytesReadable" in r,
          "selftest result shape")


# ---- lsp.* -----------------------------------------------------------
# A language server may or may not exist on the box running this suite.
# Conformance therefore asserts (a) parameter validation is exact and
# (b) a well-formed call answers with either the documented result
# shape or a clean LSP_FAILED error -- never a hang or a crash.
def _lsp_params(ctx, with_pos=True):
    ws = ctx.fx.tempdir("conf-lspws-")
    fp = os.path.join(ws, "t.swift")
    content = "let x = 1\n"
    open(fp, "w").write(content)
    p = {"workspacePath": ws, "language": 8, "filePath": fp,
         "fileContent": content}
    if with_pos:
        p.update({"line": 0, "character": 4})
    return p


def _lsp_method(ctx, method, result_key, with_pos=True):
    p = _lsp_params(ctx, with_pos)
    bad = dict(p)
    bad.pop("workspacePath")
    check(ctx.a.call_error(method, bad).code == INVALID_REQUEST,
          "%s missing workspacePath -> INVALID_REQUEST 1002" % method)
    try:
        r = ctx.a.call(method, p, timeout=45)
        check(result_key in r, "%s result carries '%s'" % (method, result_key))
    except AgentError as e:
        check(e.code == LSP_FAILED,
              "%s without a language server -> clean LSP_FAILED 1004 "
              "(got %s)" % (method, e.code))


@conformance("lsp.gotoDefinition")
def t_lsp_goto_definition(ctx):
    _lsp_method(ctx, "lsp.gotoDefinition", "locations")


@conformance("lsp.findReferences")
def t_lsp_find_references(ctx):
    _lsp_method(ctx, "lsp.findReferences", "locations")


@conformance("lsp.hover")
def t_lsp_hover(ctx):
    _lsp_method(ctx, "lsp.hover", "hover")


@conformance("lsp.documentSymbols")
def t_lsp_document_symbols(ctx):
    _lsp_method(ctx, "lsp.documentSymbols", "symbols", with_pos=False)


@conformance("lsp.workspaceSymbols")
def t_lsp_workspace_symbols(ctx):
    ws = ctx.fx.tempdir("conf-lspws-")
    check(ctx.a.call_error("lsp.workspaceSymbols",
                           {"workspacePath": ws,
                            "language": 8}).code == INVALID_REQUEST,
          "lsp.workspaceSymbols missing query -> INVALID_REQUEST 1002")
    try:
        r = ctx.a.call("lsp.workspaceSymbols",
                       {"workspacePath": ws, "query": "x", "language": 8},
                       timeout=45)
        check("symbols" in r, "lsp.workspaceSymbols result carries 'symbols'")
    except AgentError as e:
        check(e.code == LSP_FAILED,
              "lsp.workspaceSymbols without a server -> LSP_FAILED 1004")


@conformance("lsp.foldingRange")
def t_lsp_folding_range(ctx):
    _lsp_method(ctx, "lsp.foldingRange", "ranges", with_pos=False)


# ---- lsp.tunnel* (raw LSP envelope) -----------------------------------
def _lsp_frame(payload):
    body = json.dumps(payload).encode()
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


def _tunnel_stream(ctx, tid):
    """Decoded server->client byte stream for a tunnel, arrival order."""
    out = b""
    for m, p in ctx.a.notifications:
        if m == "lsp.tunnelRecv" and p.get("tunnelId") == tid:
            out += base64.b64decode(p["data"])
    return out


def _tunnel_wait_response(ctx, tid, want_id, timeout=45):
    """Parse LSP frames out of the tunnel stream until a response with
    id `want_id` appears (or the deadline passes -> None)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        buf = _tunnel_stream(ctx, tid)
        while True:
            hdr_end = buf.find(b"\r\n\r\n")
            if hdr_end < 0:
                break
            n = 0
            for line in buf[:hdr_end].split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    n = int(line.split(b":", 1)[1].strip())
            if len(buf) < hdr_end + 4 + n:
                break
            body = buf[hdr_end + 4:hdr_end + 4 + n]
            buf = buf[hdr_end + 4 + n:]
            try:
                msg = json.loads(body)
            except ValueError:
                continue
            if msg.get("id") == want_id and "method" not in msg:
                return msg
        time.sleep(0.1)
    return None


@conformance("lsp.tunnelOpen", "lsp.tunnelSend", "lsp.tunnelClose")
def t_lsp_tunnel(ctx):
    ws = ctx.fx.tempdir("conf-tunws-")
    check(ctx.a.call_error("lsp.tunnelOpen",
                           {"language": 2}).code == INVALID_REQUEST,
          "tunnelOpen missing workspacePath -> INVALID_REQUEST 1002")
    check(ctx.a.call_error("lsp.tunnelOpen",
                           {"workspacePath": ws}).code == INVALID_REQUEST,
          "tunnelOpen missing language -> INVALID_REQUEST 1002")
    check(ctx.a.call_error("lsp.tunnelOpen",
                           {"workspacePath": ws,
                            "language": 99}).code == LSP_FAILED,
          "tunnelOpen unknown language -> LSP_FAILED 1004")
    check(ctx.a.call_error("lsp.tunnelClose", {}).code == INVALID_REQUEST,
          "tunnelClose missing tunnelId -> INVALID_REQUEST 1002")

    # A send to a tunnel that never existed must be safe and tell the
    # client, so a desynced client can notice.
    ctx.a.notify("lsp.tunnelSend",
                 {"tunnelId": "t-never", "data": base64.b64encode(
                     b"x").decode()})
    note = ctx.a.wait_notification(
        "lsp.tunnelClosed",
        lambda p: p.get("tunnelId") == "t-never", timeout=10)
    check(note is not None and note.get("reason") == "unknown tunnel",
          "send to unknown tunnel -> lsp.tunnelClosed 'unknown tunnel'")

    # Full raw-LSP round trip against whichever real server this host
    # has (python/swift/rust/cpp); with none installed, every open must
    # fail with a clean LSP_FAILED -- never a hang or a crash.
    opened = None
    for lang in (2, 8, 1, 6):
        try:
            r = ctx.a.call("lsp.tunnelOpen",
                           {"workspacePath": ws, "language": lang},
                           timeout=30)
            opened = (lang, r)
            break
        except AgentError as e:
            check(e.code == LSP_FAILED,
                  "tunnelOpen without server (lang %d) -> LSP_FAILED "
                  "(got %s)" % (lang, e.code))
    if opened is None:
        print("  [PASS] no language server on this host; "
              "clean-failure path verified")
        return

    lang, r = opened
    tid = r.get("tunnelId")
    check(isinstance(tid, str) and tid, "tunnelOpen returns a tunnelId")
    check(isinstance(r.get("serverPath"), str) and
          r["serverPath"].startswith("/"),
          "tunnelOpen reports the spawned server's absolute path")

    init = _lsp_frame({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                       "params": {"processId": None,
                                  "rootUri": "file://" + ws,
                                  "capabilities": {}}})
    # Split the frame at an arbitrary byte boundary: chunk boundaries
    # are explicitly NOT message boundaries, and order must hold.
    cut = len(init) // 2
    for part in (init[:cut], init[cut:]):
        ctx.a.notify("lsp.tunnelSend",
                     {"tunnelId": tid,
                      "data": base64.b64encode(part).decode()})
    rsp = _tunnel_wait_response(ctx, tid, 1, timeout=45)
    check(rsp is not None,
          "raw LSP initialize round-trips through the tunnel (lang %d)"
          % lang)
    if rsp is not None:
        check(isinstance(rsp.get("result", {}).get("capabilities"), dict),
              "initialize response carries server capabilities")

    st = ctx.a.call("meta.stat", {})
    check(st.get("lspTunnels", 0) >= 1, "meta.stat counts the open tunnel")

    check(ctx.a.call("lsp.tunnelClose",
                     {"tunnelId": tid}).get("ok") is True,
          "tunnelClose -> ok")
    closed = ctx.a.wait_notification(
        "lsp.tunnelClosed", lambda p: p.get("tunnelId") == tid, timeout=15)
    check(closed is not None,
          "server exit after close -> lsp.tunnelClosed")
    check(ctx.a.call("lsp.tunnelClose",
                     {"tunnelId": tid}).get("ok") is True,
          "tunnelClose is idempotent")


# ---- index.* ---------------------------------------------------------
@conformance("index.create")
def t_index_create(ctx):
    ws = ctx.fx.tempdir("conf-idxws-")
    name = "conf-%s.db" % uuid.uuid4().hex
    rel = "scrutiny-cache/index/%s" % name
    try:
        ctx.a.call("index.create",
                   {"workspacePath": ws, "language": 8, "cacheDBPath": rel},
                   timeout=30)
    except AgentError:
        pass  # indexer/LSP may be unavailable; the db is created first
    names = [e["name"] for e in ctx.a.call(
        "fs.listDirectory", {"path": "scrutiny-cache/index"})["entries"]]
    check(name in names,
          "relative cacheDBPath resolves under the agent cache root")
    try:
        os.remove(os.path.join(os.path.expanduser("~"),
                               "scrutiny-cache/index", name))
    except OSError:
        pass
    check(ctx.a.call_error("index.create",
                           {"workspacePath": ws}).code == INVALID_REQUEST,
          "index.create missing params -> INVALID_REQUEST 1002")


@conformance("index.run")
def t_index_run(ctx):
    e = ctx.a.call_error("index.run", {"indexerId": "no-such-indexer"})
    check(e.code == NOT_FOUND, "index.run unknown indexerId -> NOT_FOUND 1001")
    check(ctx.a.call_error("index.run", {}).code == INVALID_REQUEST,
          "index.run missing indexerId -> INVALID_REQUEST 1002")


@conformance("index.destroy")
def t_index_destroy(ctx):
    r = ctx.a.call("index.destroy", {"indexerId": "no-such-indexer"})
    check(r.get("ok") is True, "index.destroy is idempotent (unknown -> ok)")
    check(ctx.a.call_error("index.destroy", {}).code == INVALID_REQUEST,
          "index.destroy missing indexerId -> INVALID_REQUEST 1002")


@conformance("index.cancel")
def t_index_cancel(ctx):
    ctx.a.notify("index.cancel", {"indexerId": "no-such-indexer"})
    r = ctx.a.call("meta.debug", {"padBytes": 8})
    check(len(r.get("pad", "")) == 8,
          "index.cancel notification is safe on an unknown id")


# ---- watch.* ---------------------------------------------------------
@conformance("watch.head", "watch.stop")
def t_watch_head(ctx):
    repo, _, _ = ctx.fx.repo()
    r = ctx.a.call("watch.head", {"path": repo})
    wid = r.get("watchId")
    check(isinstance(wid, str) and wid, "watch.head returns a watchId")
    # The watcher observes the resolved .git/HEAD file: it fires on
    # checkout / branch switch / rebase (HEAD rewritten atomically),
    # not on a plain commit (which only moves the branch ref).
    git(repo, "checkout", "-q", "-b", "switched")
    note = ctx.a.wait_notification(
        "watch.headChanged", lambda p: p.get("watchId") == wid, timeout=15)
    check(note is not None,
          "branch switch fires watch.headChanged for the right watchId")
    ctx.a.notify("watch.stop", {"watchId": wid})
    check(ctx.a.call("meta.debug", {"padBytes": 4}).get("pad") == "xxxx",
          "agent healthy after watch.stop")
    check(ctx.a.call_error("watch.head",
                           {"path": "/no/such/conf/repo"}).code == NOT_FOUND,
          "watch.head on a non-repo -> NOT_FOUND 1001")


# ---- cred.* ----------------------------------------------------------
@conformance("cred.provide")
def t_cred_roundtrip(ctx):
    op = "op-" + uuid.uuid4().hex
    secret = "s3cr3t-" + uuid.uuid4().hex
    ctx.a.expect_secret(op, secret)
    cs = ctx.a.call("cred.selftest",
                    {"authOpId": op, "prompt": "Password for 'https://x':"},
                    timeout=30)
    check(cs.get("got") == secret,
          "cred.request/cred.provide broker round-trip delivers the secret")
    check(cs.get("askpassExit") == 0, "askpass child exited 0")
    r = ctx.a.call("cred.provide", {"credId": "c-unknown", "value": "x"})
    check(r.get("ok") is False, "cred.provide unknown credId -> ok false")


# ---- diffcache.* -----------------------------------------------------
@conformance("diffcache.get", "diffcache.put", "diffcache.prune")
def t_diffcache(ctx):
    cache = ctx.fx.tempdir("conf-cache-")
    fr, to = "f" + uuid.uuid4().hex, "t" + uuid.uuid4().hex
    val = {"fileExistsInTo": True, "isForcePush": False, "hunksJSON": "[]"}
    check(ctx.a.call("diffcache.get",
                     {"cacheDir": cache, "fromSha": fr, "toSha": to,
                      "file": "x"}).get("hit") is False,
          "cold lookup is a miss")
    ctx.a.call("diffcache.put", {"cacheDir": cache, "fromSha": fr,
                                 "toSha": to, "file": "x", "value": val})
    hit = ctx.a.call("diffcache.get", {"cacheDir": cache, "fromSha": fr,
                                       "toSha": to, "file": "x"})
    check(hit.get("hit") is True and hit.get("value") == val,
          "get returns the stored value")
    pr = ctx.a.call("diffcache.prune", {"cacheDir": cache, "days": -1})
    check(isinstance(pr.get("removed"), int) and pr["removed"] >= 1,
          "prune reports removed count")
    check(ctx.a.call("diffcache.get",
                     {"cacheDir": cache, "fromSha": fr, "toSha": to,
                      "file": "x"}).get("hit") is False,
          "prune(-1) evicts -> miss")


# ---- meta.* / logs ----------------------------------------------------
@conformance("meta.debug")
def t_meta_debug(ctx):
    rid, wait = ctx.a.call_async("meta.debug", {"padBytes": 16})
    r = wait()
    check(r.get("echoId") == rid, "meta.debug echoes the request id")
    check(r.get("pad") == "x" * 16, "meta.debug pads exactly padBytes")

    pad = 300000  # > 128 KiB negotiated cap -> forced through rpc.chunk
    dbg = ctx.a.call("meta.debug", {"padBytes": pad, "sleepMs": 0})
    check(len(dbg.get("pad", "")) == pad,
          "over-cap response reassembles exactly via rpc.chunk (%dB)" % pad)

    rid, wait = ctx.a.call_async("meta.debug", {"sleepMs": 8000})
    time.sleep(0.2)
    ctx.a.cancel(rid)
    t0 = time.time()
    try:
        wait(timeout=6)
        check(False, "$/cancelRequest interrupts in-flight work")
    except AgentError as e:
        check(e.code == CANCELLED, "cancelled request -> CANCELLED 1007")
        check(time.time() - t0 < 5,
              "cancellation lands promptly (not after the full sleep)")


@conformance("meta.stat")
def t_meta_stat(ctx):
    r = ctx.a.call("meta.stat", {})
    lanes = r.get("lanes", {})
    check(all(k in lanes for k in ("interactive", "normal", "bulk")),
          "meta.stat lanes cover interactive/normal/bulk")
    check(isinstance(r.get("uptimeMs"), int) and r["uptimeMs"] >= 0,
          "meta.stat uptimeMs")
    check(isinstance(r.get("lspSessions"), int), "meta.stat lspSessions")
    check(r.get("agentVersion") == ctx.agent_version,
          "meta.stat agentVersion matches")
    check("gitParallel" in r and "logLevel" in r, "meta.stat shape")


@conformance("meta.capabilities")
def t_meta_capabilities(ctx):
    r = ctx.a.call("meta.capabilities", {})
    check(r.get("agentVersion") == ctx.agent_version,
          "meta.capabilities agentVersion matches")
    check(r.get("protocolVersion") == 1, "meta.capabilities protocolVersion")
    check(isinstance(r.get("outboundIO"), list) and r["outboundIO"],
          "outboundIO enumerated")
    fsa = r.get("fileSystemAccess", {})
    check(isinstance(fsa, dict) and "description" in fsa,
          "fileSystemAccess posture described")
    check(r.get("selftestMethod") == "fs.selftest",
          "selftestMethod names fs.selftest")


@conformance("logs.tail")
def t_logs_tail(ctx):
    r = ctx.a.call("logs.tail", {})
    check(r.get("enabled") is True,
          "agent launched with --log reports logging enabled")
    check(r.get("path") == ctx.log_path, "logs.tail reports the log path")
    check(isinstance(r.get("text"), str) and r.get("bytes", 0) > 0,
          "logs.tail returns tail text")
    small = ctx.a.call("logs.tail", {"maxBytes": 10})
    check(small.get("bytes", 99999) <= 10, "maxBytes cap honored")


# ---- protocol-level checks (not capability-keyed) ---------------------
def protocol_version_flag(agent_path, agent_version):
    section("--version flag")
    out = subprocess.run([agent_path, "--version"], stdout=subprocess.PIPE,
                         check=True).stdout.decode().strip()
    check(out == "%s proto 1" % agent_version,
          "--version prints '<version> proto 1' (got %r)" % out)


def protocol_hash_gate(agent_path):
    side = agent_path + ".sha256"
    if not os.path.isfile(side):
        return
    section("bootstrap hash-gate sidecar")
    h = hashlib.sha256(open(agent_path, "rb").read()).hexdigest()
    want = open(side).read().strip()
    check(h == want, "sha256(binary) matches the .sha256 sidecar")


def protocol_handshake(agent_path, agent_version):
    section("handshake")
    a = Agent(agent_path)
    try:
        r = a.call("meta.hello",
                   {"clientVersion": "conformance",
                    "supportedProtocolVersions": [1],
                    "frameCap": 131072})
        check(r.get("protocolVersion") == 1, "protocolVersion == 1")
        check(r.get("agentVersion") == agent_version,
              "agentVersion == %s (got %r)" % (agent_version,
                                               r.get("agentVersion")))
        check(r.get("frameCap") == 131072, "proposed frameCap accepted")
        check(isinstance(r.get("capabilities"), list)
              and r["capabilities"], "capabilities advertised")
    finally:
        a.close()

    a = Agent(agent_path)
    try:
        e = a.call_error("meta.hello",
                         {"clientVersion": "conformance",
                          "supportedProtocolVersions": [99]})
        check(e.code == VERSION_MISMATCH,
              "unsupported protocol -> VERSION_MISMATCH 1006")
    finally:
        a.close()

    a = Agent(agent_path)
    try:
        low = a.call("meta.hello", {"supportedProtocolVersions": [1],
                                    "frameCap": 1024})
        check(low.get("frameCap") == 64 * 1024,
              "frameCap proposal below floor clamps to 64 KiB")
    finally:
        a.close()
    a = Agent(agent_path)
    try:
        high = a.call("meta.hello", {"supportedProtocolVersions": [1],
                                     "frameCap": 10 ** 9})
        check(high.get("frameCap") == 256 * 1024,
              "frameCap proposal above ceiling clamps to 256 KiB")
    finally:
        a.close()


def protocol_errors(ctx):
    section("error model / malformed input")
    e = ctx.a.call_error("no.such.method", {})
    check(e.code == NOT_FOUND and "unknown method" in str(e),
          "unknown method -> NOT_FOUND 1001 'unknown method'")

    before = len(ctx.a.null_id_errors)
    ctx.a.send_raw(b"this is not json")
    deadline = time.time() + 5
    while len(ctx.a.null_id_errors) <= before and time.time() < deadline:
        time.sleep(0.05)
    check(len(ctx.a.null_id_errors) > before and
          ctx.a.null_id_errors[-1].get("code") == INVALID_REQUEST,
          "invalid JSON frame -> null-id INVALID_REQUEST 1002")
    check(ctx.a.call("meta.debug", {"padBytes": 4}).get("pad") == "xxxx",
          "agent keeps serving after a malformed frame")

    rid, wait = ctx.a.call_async("meta.debug", {"padBytes": 1})
    wait()
    ctx.a.send_raw(json.dumps({"jsonrpc": "2.0", "id": rid + 1000,
                               "params": {}}).encode())
    # A request with an id but no method must answer INVALID_REQUEST.
    slot_ev = threading.Event()
    ctx.a._pending[str(rid + 1000)] = [slot_ev, None]
    check(slot_ev.wait(5) and
          "error" in (ctx.a._pending.pop(str(rid + 1000))[1] or {}),
          "request without 'method' -> INVALID_REQUEST envelope")


def protocol_concurrency(ctx):
    section("concurrent requests / single-writer integrity")
    waits = []
    for i in range(8):
        pad = 1000 * (i + 1)
        rid, wait = ctx.a.call_async("meta.debug",
                                     {"sleepMs": 100, "padBytes": pad})
        waits.append((rid, pad, wait))
    ok = True
    for rid, pad, wait in waits:
        r = wait(timeout=30)
        ok = ok and r.get("echoId") == rid and len(r.get("pad", "")) == pad
    check(ok, "8 concurrent meta.debug responses all intact and matched")


def protocol_fs_sandbox(agent_path):
    section("fs sandbox (--allow-root)")
    sb_allowed = tempfile.mkdtemp(prefix="conf-sb-allow-")
    sb_secret = tempfile.mkdtemp(prefix="conf-sb-secret-")
    ok_file = os.path.join(sb_allowed, "ok.txt")
    secret_file = os.path.join(sb_secret, "keys.txt")
    open(ok_file, "w").write("inside\n")
    open(secret_file, "w").write("shhh\n")
    trap = os.path.join(sb_allowed, "trap")
    os.symlink(secret_file, trap)
    scoped = Agent(agent_path, extra_args=["--allow-root", sb_allowed])
    try:
        check(scoped.call("fs.readFile",
                          {"path": ok_file}).get("content") == "inside\n",
              "inside-root read works")
        check(scoped.call_error(
            "fs.readFile", {"path": secret_file}).code == PERMISSION_DENIED,
            "sibling-tree read -> PERMISSION_DENIED 1005")
        check(scoped.call_error(
            "fs.readFile", {"path": trap}).code == PERMISSION_DENIED,
            "escaping symlink -> PERMISSION_DENIED 1005 (canonicalized)")
        check(scoped.call_error(
            "fs.listDirectory", {"path": sb_secret}).code == PERMISSION_DENIED,
            "sibling-tree listDirectory -> PERMISSION_DENIED 1005")
    finally:
        scoped.close()
        shutil.rmtree(sb_allowed, ignore_errors=True)
        shutil.rmtree(sb_secret, ignore_errors=True)


# ---- runner -----------------------------------------------------------
def run(agent_path, agent_version):
    protocol_version_flag(agent_path, agent_version)
    protocol_hash_gate(agent_path)
    protocol_handshake(agent_path, agent_version)

    fx = Fixtures()
    log_dir = fx.tempdir("conf-log-")
    log_path = os.path.join(log_dir, "agent.log")
    extra = ["--allow-root", tempfile.gettempdir(),
             "--log", log_path, "--log-level", "info",
             # git.exec is off unless allowlisted, so enable the
             # read-only preset here -- otherwise the capability is not
             # advertised and its conformance check never runs.
             "--git-exec-preset", "read-only",
             # Likewise for the fs write surface: unadvertised unless
             # enabled, so the coverage gate needs it on to check it.
             "--allow-write"]
    home_root = os.environ.get("HOME") or os.path.expanduser("~")
    if home_root and home_root != "~":
        extra += ["--allow-root", home_root]
    a = Agent(agent_path, extra_args=extra)
    try:
        section("capability coverage")
        hello = a.call("meta.hello",
                       {"clientVersion": "conformance",
                        "supportedProtocolVersions": [1],
                        "frameCap": 131072})
        advertised = sorted(hello.get("capabilities", []))
        covered = sorted(CONFORMANCE.keys())
        missing = [c for c in advertised if c not in CONFORMANCE]
        stale = [c for c in covered if c not in advertised]
        check(not missing,
              "every advertised capability has a conformance test "
              "(missing: %r)" % missing)
        check(not stale,
              "no conformance test targets an unadvertised capability "
              "(stale: %r)" % stale)

        ctx = Ctx(a, fx, agent_path, agent_version, log_path)
        ran = set()
        for cap in advertised:
            for fn in CONFORMANCE.get(cap, []):
                if fn in ran:
                    continue
                ran.add(fn)
                section(cap if len(CONFORMANCE.get(cap, [])) == 1
                        else "%s (%s)" % (cap, fn.__name__))
                try:
                    fn(ctx)
                except (AgentError, AssertionError, OSError) as e:
                    check(False, "%s raised: %s" % (fn.__name__, e))

        protocol_errors(ctx)
        protocol_concurrency(ctx)
        check(a.alive(), "agent survived the full suite")
    finally:
        a.close()
        fx.cleanup()

    protocol_fs_sandbox(agent_path)

    print()
    if _FAILS:
        print("FAILED (%d): %s" % (len(_FAILS), "; ".join(_FAILS)))
        return 1
    print("ALL PASS (conformant)")
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    agent = argv[1]
    version = "0.1.0"
    if "--agent-version" in argv:
        version = argv[argv.index("--agent-version") + 1]
    if not (os.path.isfile(agent) and os.access(agent, os.X_OK)):
        print("error: not an executable: %s" % agent)
        return 2
    return run(agent, version)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
