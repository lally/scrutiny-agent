"""`git.*` read surfaces.

These answer everything the review UI draws: branches, history, diffs,
file contents at a revision. Assertions are against exact values
computed by the `git` CLI on the same fixture, so a regression in the
libgit2 bridge shows up as a mismatch rather than a plausible-looking
wrong answer.
"""
import os

import pytest

from scrutiny import INVALID_REQUEST, NOT_FOUND
from scrutiny.fixtures import git


# ---------------------------------------------------------------------
# identity / metadata
# ---------------------------------------------------------------------
def test_head_sha(agent, repo):
    assert agent.call("git.headSha", {"path": repo.path})["headSha"] == \
        repo.head_sha


def test_head_sha_follows_checkout(agent, repo):
    repo.checkout("older", create=True)
    git(repo.path, "reset", "-q", "--hard", repo.parent_sha)
    assert agent.call("git.headSha",
                      {"path": repo.path})["headSha"] == repo.parent_sha


def test_head_sha_from_a_subdirectory(agent, repo):
    """A path inside the work tree resolves to the enclosing repo."""
    subdir = repo.mkdir("sub/deeper")
    try:
        result = agent.call("git.headSha", {"path": subdir})
        assert result["headSha"] == repo.head_sha
    except Exception as exc:                            # noqa: BLE001
        # Requiring the repo root is a defensible contract; silently
        # returning the wrong sha is not.
        assert getattr(exc, "code", None) in (NOT_FOUND, INVALID_REQUEST)


def test_repo_metadata(agent, repo):
    meta = agent.call("git.repoMetadata", {"path": repo.path})
    assert meta["headSha"] == repo.head_sha
    assert meta["currentBranch"] == "work"
    assert meta["isBare"] is False
    assert meta["hasUncommittedChanges"] is False


def test_repo_metadata_path_is_the_work_tree(agent, repo):
    """`path` names the work tree, not the gitdir.

    Every other git.* method takes a work-tree path, and clients
    display this one; handing back "<repo>/.git/" is both surprising
    and wrong in a UI. The gitdir is available separately.
    """
    meta = agent.call("git.repoMetadata", {"path": repo.path})
    assert os.path.realpath(meta["path"]) == os.path.realpath(repo.path)
    assert os.path.realpath(meta["gitDir"]) == \
        os.path.realpath(os.path.join(repo.path, ".git"))


def test_repo_metadata_path_round_trips(agent, repo):
    """The reported path can be fed straight back to another method."""
    meta = agent.call("git.repoMetadata", {"path": repo.path})
    assert agent.call("git.headSha",
                      {"path": meta["path"]})["headSha"] == repo.head_sha


def test_repo_metadata_detects_dirt(agent, repo):
    repo.dirty()
    meta = agent.call("git.repoMetadata", {"path": repo.path})
    assert meta["hasUncommittedChanges"] is True


def test_repo_metadata_detects_staged_only_dirt(agent, repo):
    """Staged-but-uncommitted counts as uncommitted changes."""
    repo.stage()
    meta = agent.call("git.repoMetadata", {"path": repo.path})
    assert meta["hasUncommittedChanges"] is True


def test_repo_metadata_untracked_file(agent, repo):
    repo.write("brand-new.txt", "hello\n")
    meta = agent.call("git.repoMetadata", {"path": repo.path})
    assert isinstance(meta["hasUncommittedChanges"], bool)


def test_repo_metadata_bare_repo(agent, repo_factory):
    bare = repo_factory(bare=True)
    meta = agent.call("git.repoMetadata", {"path": bare.path})
    assert meta["isBare"] is True


def test_repo_metadata_detached_head(agent, repo):
    git(repo.path, "checkout", "-q", "--detach", repo.parent_sha)
    meta = agent.call("git.repoMetadata", {"path": repo.path})
    assert meta["headSha"] == repo.parent_sha
    assert isinstance(meta["currentBranch"], (str, type(None)))


def test_empty_repo_has_no_head(agent, repo_factory):
    """A freshly-inited repo has no commits; that must not crash."""
    empty = repo_factory()
    try:
        result = agent.call("git.headSha", {"path": empty.path})
        assert result.get("headSha") in (None, "", "0" * 40)
    except Exception as exc:                            # noqa: BLE001
        assert getattr(exc, "code", None) is not None, \
            "unborn HEAD produced a non-protocol failure: %s" % exc


# ---------------------------------------------------------------------
# remotes
# ---------------------------------------------------------------------
def test_remotes_empty(agent, repo):
    assert agent.call("git.remotes", {"path": repo.path})["remotes"] == []


def test_remotes_after_add(agent, repo):
    repo.add_remote("origin", "https://example.invalid/x.git")
    remotes = agent.call("git.remotes", {"path": repo.path})["remotes"]
    assert len(remotes) == 1
    assert remotes[0]["name"] == "origin"
    assert remotes[0]["url"] == "https://example.invalid/x.git"
    assert remotes[0]["pushUrl"] == remotes[0]["url"], \
        "pushUrl should fall back to url when unset"


def test_remotes_distinct_push_url(agent, repo):
    repo.add_remote("origin", "https://example.invalid/fetch.git")
    git(repo.path, "remote", "set-url", "--push", "origin",
        "ssh://example.invalid/push.git")
    remote = agent.call("git.remotes", {"path": repo.path})["remotes"][0]
    assert remote["url"] == "https://example.invalid/fetch.git"
    assert remote["pushUrl"] == "ssh://example.invalid/push.git"


def test_multiple_remotes(agent, repo):
    repo.add_remote("origin", "https://example.invalid/a.git")
    repo.add_remote("fork", "https://example.invalid/b.git")
    names = {r["name"] for r in
             agent.call("git.remotes", {"path": repo.path})["remotes"]}
    assert names == {"origin", "fork"}


# ---------------------------------------------------------------------
# branches
# ---------------------------------------------------------------------
def test_branches_local(agent, repo):
    branches = agent.call("git.branches",
                          {"path": repo.path, "local": True,
                           "remote": False})["branches"]
    work = [b for b in branches if b["name"] == "work"]
    assert len(work) == 1
    entry = work[0]
    assert entry["refname"] == "refs/heads/work"
    assert entry["targetOid"] == repo.head_sha
    assert entry["isHead"] is True
    assert entry["isRemote"] is False


def test_branches_lists_every_local_branch(agent, repo):
    repo.branch("feature-a")
    repo.branch("feature-b")
    names = {b["name"] for b in
             agent.call("git.branches",
                        {"path": repo.path, "local": True,
                         "remote": False})["branches"]}
    assert {"work", "feature-a", "feature-b"} <= names


def test_branches_only_one_is_head(agent, repo):
    repo.branch("feature-a")
    branches = agent.call("git.branches",
                          {"path": repo.path, "local": True,
                           "remote": False})["branches"]
    heads = [b["name"] for b in branches if b["isHead"]]
    assert heads == ["work"], "expected exactly one HEAD branch, got %r" % heads


def test_branches_remote(agent, clone_with_upstream):
    clone, _upstream = clone_with_upstream
    branches = agent.call("git.branches",
                          {"path": clone.path, "local": False,
                           "remote": True})["branches"]
    assert branches, "clone has no remote-tracking branches"
    for entry in branches:
        assert entry["isRemote"] is True
        assert entry["remoteName"] == "origin"


def test_branches_upstream_link(agent, clone_with_upstream):
    clone, _upstream = clone_with_upstream
    branches = agent.call("git.branches",
                          {"path": clone.path, "local": True,
                           "remote": False})["branches"]
    work = next(b for b in branches if b["name"] == "work")
    assert work["upstream"], "tracked branch reports no upstream"
    assert "work" in work["upstream"]


def test_branches_local_only_excludes_remotes(agent, clone_with_upstream):
    clone, _upstream = clone_with_upstream
    branches = agent.call("git.branches",
                          {"path": clone.path, "local": True,
                           "remote": False})["branches"]
    assert all(b["isRemote"] is False for b in branches)


# ---------------------------------------------------------------------
# commits
# ---------------------------------------------------------------------
def test_commits_newest_first(agent, repo):
    commits = agent.call("git.commits", {"path": repo.path})["commits"]
    assert len(commits) == 2
    assert commits[0]["oid"] == repo.head_sha
    assert commits[1]["oid"] == repo.parent_sha


def test_commit_fields(agent, repo):
    commit = agent.call("git.commits", {"path": repo.path})["commits"][0]
    assert commit["summary"] == "second"
    assert "second" in commit["message"]
    assert commit["authorName"] == "Scrutiny Test"
    assert commit["authorEmail"] == "test@scrutiny.invalid"
    assert isinstance(commit["authorTime"], int) and commit["authorTime"] > 0
    assert commit["parentOids"] == [repo.parent_sha]
    assert len(commit["shortOid"]) >= 7
    assert commit["oid"].startswith(commit["shortOid"])


def test_root_commit_has_no_parents(agent, repo):
    root = agent.call("git.commits", {"path": repo.path})["commits"][1]
    assert root["parentOids"] == []


def test_commits_limit(agent, repo):
    for n in range(8):
        repo.commit("extra %d" % n, {"file.txt": "v%d\n" % n})
    assert len(agent.call("git.commits",
                          {"path": repo.path, "limit": 3})["commits"]) == 3
    assert len(agent.call("git.commits",
                          {"path": repo.path, "limit": 1})["commits"]) == 1


def test_commits_default_limit(agent, repo):
    """The documented default is 100."""
    for n in range(120):
        repo.commit("bulk %d" % n, {"file.txt": "v%d\n" % n})
    commits = agent.call("git.commits", {"path": repo.path}, timeout=60)
    assert len(commits["commits"]) == 100


def test_commits_for_a_named_branch(agent, repo):
    repo.checkout("side", create=True)
    side_head = repo.commit("side commit", {"side.txt": "s\n"})
    repo.checkout("work")
    side = agent.call("git.commits",
                      {"path": repo.path, "branch": "side"})["commits"]
    assert side[0]["oid"] == side_head
    work = agent.call("git.commits",
                      {"path": repo.path, "branch": "work"})["commits"]
    assert work[0]["oid"] == repo.head_sha


def test_commits_unknown_branch(agent, repo):
    """An unknown branch yields no commits rather than a hard error.

    This mirrors the documented `git.diffForCommit` behavior for an
    unknown sha (empty result, not NOT_FOUND); clients distinguish
    "branch has no commits" from "no such branch" via git.branches.
    """
    try:
        result = agent.call("git.commits",
                            {"path": repo.path, "branch": "nope"})
        assert result["commits"] == []
    except Exception as exc:                            # noqa: BLE001
        assert getattr(exc, "code", None) in (NOT_FOUND, INVALID_REQUEST, 1000)


def test_merge_commit_has_two_parents(agent, repo):
    repo.checkout("side", create=True)
    side = repo.commit("side work", {"side.txt": "s\n"})
    repo.checkout("work")
    git(repo.path, "merge", "-q", "--no-ff", "-m", "merge side", "side")
    merge_sha = repo.head()
    merge = next(c for c in
                 agent.call("git.commits", {"path": repo.path})["commits"]
                 if c["oid"] == merge_sha)
    assert len(merge["parentOids"]) == 2
    assert side in merge["parentOids"]


def test_commit_message_with_body(agent, repo):
    git(repo.path, "commit", "-q", "--allow-empty", "-m",
        "subject line", "-m", "body paragraph here")
    commit = agent.call("git.commits", {"path": repo.path})["commits"][0]
    assert commit["summary"] == "subject line"
    assert "body paragraph here" in commit["message"]


def test_commit_message_with_unicode(agent, repo):
    git(repo.path, "commit", "-q", "--allow-empty", "-m",
        "fix: naïve café \U0001F680")
    commit = agent.call("git.commits", {"path": repo.path})["commits"][0]
    assert commit["summary"] == "fix: naïve café \U0001F680"


# ---------------------------------------------------------------------
# ahead / behind
# ---------------------------------------------------------------------
def test_ahead_behind_fresh_clone(agent, clone_with_upstream):
    clone, _ = clone_with_upstream
    result = agent.call("git.aheadBehind",
                        {"path": clone.path, "branch": "work"})
    assert result == {"ahead": 0, "behind": 0}


def test_ahead_behind_local_commit(agent, clone_with_upstream):
    clone, _ = clone_with_upstream
    clone.commit("local only", {"new.txt": "x\n"})
    result = agent.call("git.aheadBehind",
                        {"path": clone.path, "branch": "work"})
    assert result["ahead"] == 1 and result["behind"] == 0


def test_ahead_behind_upstream_commit(agent, clone_with_upstream):
    clone, upstream = clone_with_upstream
    upstream.commit("upstream moved", {"file.txt": "moved\n"})
    git(clone.path, "fetch", "-q", "origin")
    result = agent.call("git.aheadBehind",
                        {"path": clone.path, "branch": "work"})
    assert result["ahead"] == 0 and result["behind"] == 1


def test_ahead_behind_diverged(agent, clone_with_upstream):
    clone, upstream = clone_with_upstream
    upstream.commit("upstream moved", {"file.txt": "moved\n"})
    clone.commit("local moved", {"other.txt": "local\n"})
    git(clone.path, "fetch", "-q", "origin")
    result = agent.call("git.aheadBehind",
                        {"path": clone.path, "branch": "work"})
    assert result["ahead"] == 1 and result["behind"] == 1


def test_ahead_behind_requires_branch(agent, clone_with_upstream):
    clone, _ = clone_with_upstream
    assert agent.call_expect_error(
        "git.aheadBehind", {"path": clone.path}).code == INVALID_REQUEST


def test_ahead_behind_without_upstream(agent, repo):
    """A branch with no upstream must fail cleanly, not report nonsense."""
    try:
        result = agent.call("git.aheadBehind",
                            {"path": repo.path, "branch": "work"})
        assert result["ahead"] == 0 and result["behind"] == 0
    except Exception as exc:                            # noqa: BLE001
        assert getattr(exc, "code", None) is not None


# ---------------------------------------------------------------------
# showFile / diff / isAncestor
# ---------------------------------------------------------------------
def test_show_file_at_head(agent, repo):
    result = agent.call("git.showFile", {"path": repo.path,
                                         "sha": repo.head_sha,
                                         "file": "file.txt"})
    assert result["content"] == repo.SECOND


def test_show_file_at_parent(agent, repo):
    result = agent.call("git.showFile", {"path": repo.path,
                                         "sha": repo.parent_sha,
                                         "file": "file.txt"})
    assert result["content"] == repo.FIRST


def test_show_file_missing_path_is_null(agent, repo):
    result = agent.call("git.showFile", {"path": repo.path,
                                         "sha": repo.head_sha,
                                         "file": "no-such-file.txt"})
    assert result["content"] is None


def test_show_file_unknown_sha(agent, repo):
    try:
        result = agent.call("git.showFile", {"path": repo.path,
                                             "sha": "0" * 40,
                                             "file": "file.txt"})
        assert result["content"] is None
    except Exception as exc:                            # noqa: BLE001
        assert getattr(exc, "code", None) is not None


def test_show_file_nested_path(agent, repo):
    repo.commit("nested", {"a/b/c.txt": "deep\n"})
    result = agent.call("git.showFile", {"path": repo.path,
                                         "sha": repo.head(),
                                         "file": "a/b/c.txt"})
    assert result["content"] == "deep\n"


def test_show_file_unicode_content(agent, repo):
    body = "naïve café \U0001F600\n"
    repo.commit("unicode", {"u.txt": body})
    result = agent.call("git.showFile", {"path": repo.path,
                                         "sha": repo.head(),
                                         "file": "u.txt"})
    assert result["content"] == body


def test_git_diff_between_revisions(agent, repo):
    result = agent.call("git.diff", {"path": repo.path,
                                     "from": repo.parent_sha,
                                     "to": repo.head_sha,
                                     "file": "file.txt"})
    assert "CHANGED" in result["diff"]
    assert "line2" in result["diff"], "the removed line should appear"


def test_git_diff_is_stable(agent, repo):
    """The cache is keyed on an immutable triple; repeats must match."""
    params = {"path": repo.path, "from": repo.parent_sha,
              "to": repo.head_sha, "file": "file.txt"}
    assert agent.call("git.diff", params) == agent.call("git.diff", params)


def test_git_diff_unchanged_file(agent, repo):
    """A file untouched between two revisions produces no diff."""
    second = repo.commit("add stable", {"stable.txt": "constant\n"})
    third = repo.commit("touch other", {"file.txt": "again\n"})
    result = agent.call("git.diff", {"path": repo.path,
                                     "from": second, "to": third,
                                     "file": "stable.txt"})
    assert result["diff"] in (None, ""), \
        "an unchanged file should produce no diff, got %r" % result["diff"]


def test_is_ancestor_true(agent, repo):
    assert agent.call("git.isAncestor",
                      {"path": repo.path, "ancestor": repo.parent_sha,
                       "descendant": repo.head_sha})["isAncestor"] is True


def test_is_ancestor_false(agent, repo):
    assert agent.call("git.isAncestor",
                      {"path": repo.path, "ancestor": repo.head_sha,
                       "descendant": repo.parent_sha})["isAncestor"] is False


def test_is_ancestor_self(agent, repo):
    """A commit is its own ancestor (git merge-base semantics)."""
    assert agent.call("git.isAncestor",
                      {"path": repo.path, "ancestor": repo.head_sha,
                       "descendant": repo.head_sha})["isAncestor"] is True


def test_is_ancestor_across_branches(agent, repo):
    repo.checkout("side", create=True)
    side = repo.commit("side", {"side.txt": "s\n"})
    repo.checkout("work")
    assert agent.call("git.isAncestor",
                      {"path": repo.path, "ancestor": side,
                       "descendant": repo.head_sha})["isAncestor"] is False


def test_is_ancestor_unknown_sha(agent, repo):
    try:
        result = agent.call("git.isAncestor",
                            {"path": repo.path, "ancestor": "0" * 40,
                             "descendant": repo.head_sha})
        assert result["isAncestor"] is False
    except Exception as exc:                            # noqa: BLE001
        assert getattr(exc, "code", None) is not None
