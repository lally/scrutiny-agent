"""Mutating and network `git.*` operations.

`git.clone` / `git.fetch` / `git.ensureRepository` run on the bulk lane
behind the credential broker. Everything here uses filesystem
"remotes", so the tests exercise the real code path (subprocess git,
askpass wiring, install-dir math) without touching a network.
"""
import os
import time

import pytest

from scrutiny import GIT_FAILED, INTERNAL, INVALID_REQUEST
from scrutiny.fixtures import GitRepo, git


# ---------------------------------------------------------------------
# checkoutBranch
# ---------------------------------------------------------------------
def test_checkout_branch(agent, repo):
    repo.branch("feature")
    assert agent.call("git.checkoutBranch",
                      {"path": repo.path, "branch": "feature"}) == {}
    assert agent.call("git.repoMetadata",
                      {"path": repo.path})["currentBranch"] == "feature"


def test_checkout_updates_head_sha(agent, repo):
    repo.checkout("side", create=True)
    side_head = repo.commit("side work", {"side.txt": "s\n"})
    repo.checkout("work")
    agent.call("git.checkoutBranch", {"path": repo.path, "branch": "side"})
    assert agent.call("git.headSha", {"path": repo.path})["headSha"] == \
        side_head


def test_checkout_unknown_branch(agent, repo):
    error = agent.call_expect_error("git.checkoutBranch",
                                    {"path": repo.path,
                                     "branch": "no-such-branch"})
    assert error.code == INTERNAL
    assert str(error).strip(), "refusal should carry the libgit2 reason"


def test_checkout_is_idempotent(agent, repo):
    agent.call("git.checkoutBranch", {"path": repo.path, "branch": "work"})
    assert agent.call("git.repoMetadata",
                      {"path": repo.path})["currentBranch"] == "work"


def test_checkout_refuses_to_clobber_local_changes(agent, repo):
    """SAFE checkout: uncommitted work is not silently destroyed."""
    repo.checkout("side", create=True)
    repo.commit("side changes file", {"file.txt": "side version\n"})
    repo.checkout("work")
    repo.dirty(content="precious local edit\n")
    try:
        agent.call("git.checkoutBranch", {"path": repo.path,
                                          "branch": "side"})
    except Exception as exc:                            # noqa: BLE001
        assert getattr(exc, "code", None) == INTERNAL
        return
    content = repo.read("file.txt").decode()
    assert "precious local edit" in content, \
        "checkout discarded an uncommitted change without refusing"


def test_checkout_requires_branch(agent, repo):
    assert agent.call_expect_error(
        "git.checkoutBranch", {"path": repo.path}).code == INVALID_REQUEST


# ---------------------------------------------------------------------
# clone
# ---------------------------------------------------------------------
def test_clone(agent, repo, tmp_path):
    install = tmp_path / "install"
    install.mkdir()
    result = agent.call("git.clone",
                        {"fullName": "acme/widgets", "cloneURL": repo.path,
                         "installDir": str(install), "authOpId": "op-1"},
                        timeout=120)
    assert result["localPath"] == str(install / "acme_widgets"), \
        "clone should land at <installDir>/<owner>_<repo>"
    assert os.path.isdir(os.path.join(result["localPath"], ".git"))
    assert isinstance(result["lastFetched"], int)
    assert result["cloneURL"] == repo.path


def test_cloned_repo_matches_upstream(agent, repo, tmp_path):
    install = tmp_path / "install"
    install.mkdir()
    result = agent.call("git.clone",
                        {"fullName": "acme/widgets", "cloneURL": repo.path,
                         "installDir": str(install), "authOpId": "op-2"},
                        timeout=120)
    assert agent.call("git.headSha",
                      {"path": result["localPath"]})["headSha"] == \
        repo.head_sha


def test_clone_name_mangling(agent, repo, tmp_path):
    """Slashes in fullName become underscores in the directory name."""
    install = tmp_path / "install"
    install.mkdir()
    result = agent.call("git.clone",
                        {"fullName": "deep/nested/name",
                         "cloneURL": repo.path,
                         "installDir": str(install), "authOpId": "op-3"},
                        timeout=120)
    assert "/" not in os.path.basename(result["localPath"])
    assert os.path.basename(result["localPath"]) == "deep_nested_name"


def test_clone_failure(agent, tmp_path):
    install = tmp_path / "install"
    install.mkdir()
    error = agent.call_expect_error(
        "git.clone", {"fullName": "acme/nope",
                      "cloneURL": "/no/such/upstream",
                      "installDir": str(install), "authOpId": "op-4"},
        timeout=120)
    assert error.code == GIT_FAILED


def test_clone_requires_params(agent):
    assert agent.call_expect_error("git.clone", {}).code == INVALID_REQUEST


def test_clone_runs_on_the_bulk_lane(agent, repo, tmp_path):
    """A clone in flight must not block interactive requests."""
    install = tmp_path / "install"
    install.mkdir()
    _clone_id, wait = agent.call_async(
        "git.clone", {"fullName": "acme/lane", "cloneURL": repo.path,
                      "installDir": str(install), "authOpId": "op-5"})
    start = time.monotonic()
    assert agent.call("meta.debug", {"padBytes": 4}, timeout=30)["pad"] == \
        "xxxx"
    interactive_latency = time.monotonic() - start
    wait(timeout=120)
    assert interactive_latency < 10, \
        "interactive request waited %.1fs behind a bulk clone" \
        % interactive_latency


# ---------------------------------------------------------------------
# fetch
# ---------------------------------------------------------------------
def test_fetch(agent, clone_with_upstream):
    clone, _ = clone_with_upstream
    result = agent.call("git.fetch",
                        {"repoPath": clone.path, "authOpId": "op-f1"},
                        timeout=120)
    assert result["ok"] is True
    assert isinstance(result["lastFetched"], int)


def test_fetch_picks_up_upstream_commits(agent, clone_with_upstream):
    clone, upstream = clone_with_upstream
    new_sha = upstream.commit("upstream work", {"file.txt": "upstream\n"})
    agent.call("git.fetch", {"repoPath": clone.path, "authOpId": "op-f2"},
               timeout=120)
    remote = git(clone.path, "rev-parse", "origin/work", capture=True)
    assert remote == new_sha, "fetch did not advance origin/work"


def test_fetch_prunes_deleted_branches(agent, clone_with_upstream):
    """Documented as `git fetch --all --prune`."""
    clone, upstream = clone_with_upstream
    upstream.branch("temporary")
    agent.call("git.fetch", {"repoPath": clone.path, "authOpId": "op-f3"},
               timeout=120)
    refs = git(clone.path, "branch", "-r", capture=True)
    assert "origin/temporary" in refs
    git(upstream.path, "branch", "-D", "temporary")
    agent.call("git.fetch", {"repoPath": clone.path, "authOpId": "op-f4"},
               timeout=120)
    refs = git(clone.path, "branch", "-r", capture=True)
    assert "origin/temporary" not in refs, "--prune did not remove the ref"


def test_fetch_outside_a_repo(agent, tmp_path):
    plain = tmp_path / "not-a-repo"
    plain.mkdir()
    error = agent.call_expect_error(
        "git.fetch", {"repoPath": str(plain), "authOpId": "op-f5"},
        timeout=120)
    assert error.code == GIT_FAILED


def test_fetch_requires_params(agent):
    assert agent.call_expect_error("git.fetch", {}).code == INVALID_REQUEST


# ---------------------------------------------------------------------
# ensureRepository
# ---------------------------------------------------------------------
def test_ensure_repository_clones_when_absent(agent, repo, tmp_path):
    install = tmp_path / "install"
    install.mkdir()
    result = agent.call("git.ensureRepository",
                        {"fullName": "acme/ensure", "cloneURL": repo.path,
                         "installDir": str(install), "authOpId": "op-e1"},
                        timeout=120)
    assert os.path.isdir(os.path.join(result["localPath"], ".git"))


def test_ensure_repository_fetches_when_present(agent, repo, tmp_path):
    install = tmp_path / "install"
    install.mkdir()
    params = {"fullName": "acme/ensure2", "cloneURL": repo.path,
              "installDir": str(install), "authOpId": "op-e2"}
    first = agent.call("git.ensureRepository", params, timeout=120)
    new_sha = repo.commit("after clone", {"file.txt": "later\n"})
    second = agent.call("git.ensureRepository", params, timeout=120)
    assert second["localPath"] == first["localPath"], \
        "second call relocated the repo instead of fetching in place"
    remote = git(second["localPath"], "rev-parse", "origin/work", capture=True)
    assert remote == new_sha, "ensureRepository did not fetch new commits"


def test_ensure_repository_is_repeatable(agent, repo, tmp_path):
    install = tmp_path / "install"
    install.mkdir()
    params = {"fullName": "acme/ensure3", "cloneURL": repo.path,
              "installDir": str(install), "authOpId": "op-e3"}
    paths = {agent.call("git.ensureRepository", params,
                        timeout=120)["localPath"] for _ in range(3)}
    assert len(paths) == 1


def test_ensure_repository_requires_params(agent):
    assert agent.call_expect_error("git.ensureRepository",
                                   {}).code == INVALID_REQUEST


def test_relative_install_dir_anchors_under_home(agent, repo):
    """Documented: relative install paths anchor under $HOME."""
    relative = "scrutiny-test-install"
    target = os.path.join(os.path.expanduser("~"), relative)
    try:
        result = agent.call("git.clone",
                            {"fullName": "acme/rel", "cloneURL": repo.path,
                             "installDir": relative, "authOpId": "op-rel"},
                            timeout=120)
        assert result["localPath"].startswith(
            os.path.realpath(os.path.expanduser("~")))
    finally:
        import shutil
        shutil.rmtree(target, ignore_errors=True)


# ---------------------------------------------------------------------
# credential broker plumbing
# ---------------------------------------------------------------------
def test_brokered_ops_work_in_deeply_nested_repos(agent, tmp_path):
    """A long repository path must not break the credential broker.

    The broker's unix socket has a ~108-byte `sun_path` limit. Anchoring
    it in the repository made every clone/fetch fail for checkouts
    deeper than ~85 characters -- an ordinary path on a dev box -- with
    an opaque "cred broker setup failed".
    """
    deep = tmp_path
    for part in ("organization-name", "team-platform-services",
                 "repositories-checked-out-here", "api-gateway-service"):
        deep = deep / part
    deep.mkdir(parents=True)

    upstream = GitRepo(prefix="upstream-", parent=str(deep))
    upstream.build_default()
    clone = upstream.clone_to(prefix="clone-of-the-service-", parent=str(deep))
    try:
        assert len(clone.path) > 90, \
            "fixture path is too short to exercise the limit (%d)" \
            % len(clone.path)
        result = agent.call("git.fetch",
                            {"repoPath": clone.path, "authOpId": "op-deep"},
                            timeout=120)
        assert result["ok"] is True
    finally:
        clone.destroy()
        upstream.destroy()


def test_broker_leaves_no_socket_in_the_work_tree(agent, clone_with_upstream):
    """The broker must not litter the repository being reviewed."""
    clone, _ = clone_with_upstream
    agent.call("git.fetch", {"repoPath": clone.path, "authOpId": "op-clean"},
               timeout=120)
    leftovers = [name for name in os.listdir(clone.path)
                 if "scrutiny-cred" in name]
    assert leftovers == [], \
        "credential broker left socket files in the work tree: %r" % leftovers
    status = git(clone.path, "status", "--porcelain", capture=True)
    assert status == "", "brokered fetch dirtied the work tree:\n%s" % status
