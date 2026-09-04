"""Structured diffs: `git.diffForCommit`, `workingTreeDiff`, `stagedDiff`.

These produce the `FileDiff` structure the review UI renders line by
line, so the tests check the shape *and* the semantics: hunk line
numbers, origin characters, and which of the three surfaces a given
change should appear in.
"""
import pytest

from scrutiny import INVALID_REQUEST
from scrutiny.fixtures import git


def assert_file_diff_shape(diff, label):
    assert isinstance(diff["status"], int), "%s: status not an int" % label
    assert isinstance(diff["oldPath"], str), "%s: oldPath" % label
    assert isinstance(diff["newPath"], str), "%s: newPath" % label
    assert isinstance(diff["patch"], str), "%s: patch" % label
    hunks = diff["hunks"]
    assert isinstance(hunks, list) and hunks, "%s: no hunks" % label
    for hunk in hunks:
        for key in ("oldStart", "oldLines", "newStart", "newLines"):
            assert isinstance(hunk[key], int), "%s: hunk.%s" % (label, key)
        assert isinstance(hunk["header"], str)
        assert hunk["lines"], "%s: hunk has no lines" % label
        for line in hunk["lines"]:
            assert isinstance(line["origin"], str) and len(line["origin"]) == 1
            assert "content" in line
            assert "oldLineNo" in line and "newLineNo" in line


# ---------------------------------------------------------------------
# diffForCommit
# ---------------------------------------------------------------------
def test_diff_for_commit(agent, repo):
    diffs = agent.call("git.diffForCommit",
                       {"path": repo.path, "sha": repo.head_sha})["diffs"]
    assert len(diffs) == 1
    assert diffs[0]["newPath"] == "file.txt"
    assert_file_diff_shape(diffs[0], "diffForCommit")
    assert "CHANGED" in diffs[0]["patch"]


def test_diff_for_commit_line_origins(agent, repo):
    """Added / removed / context lines are distinguishable."""
    lines = agent.call("git.diffForCommit",
                       {"path": repo.path,
                        "sha": repo.head_sha})["diffs"][0]["hunks"][0]["lines"]
    origins = {line["origin"] for line in lines}
    assert "+" in origins, "no added lines marked"
    assert "-" in origins, "no removed lines marked"
    added = [ln["content"] for ln in lines if ln["origin"] == "+"]
    assert any("CHANGED" in text for text in added)


def test_diff_for_commit_line_numbers(agent, repo):
    """Added lines carry a new line number; removed lines carry an old one."""
    lines = agent.call("git.diffForCommit",
                       {"path": repo.path,
                        "sha": repo.head_sha})["diffs"][0]["hunks"][0]["lines"]
    for line in lines:
        if line["origin"] == "+":
            assert line["newLineNo"] > 0, \
                "added line has no new line number: %r" % line
        elif line["origin"] == "-":
            assert line["oldLineNo"] > 0, \
                "removed line has no old line number: %r" % line


def test_diff_for_root_commit(agent, repo):
    """The first commit diffs against the empty tree, not nothing."""
    diffs = agent.call("git.diffForCommit",
                       {"path": repo.path, "sha": repo.parent_sha})["diffs"]
    assert diffs, "root commit produced no diff"
    assert diffs[0]["newPath"] == "file.txt"


def test_diff_for_commit_multiple_files(agent, repo):
    repo.commit("touch several",
                {"a.txt": "a\n", "b.txt": "b\n", "c.txt": "c\n"})
    diffs = agent.call("git.diffForCommit",
                       {"path": repo.path, "sha": repo.head()})["diffs"]
    assert {d["newPath"] for d in diffs} == {"a.txt", "b.txt", "c.txt"}


def test_diff_for_commit_file_deletion(agent, repo):
    repo.commit("add doomed", {"doomed.txt": "bye\n"})
    git(repo.path, "rm", "-q", "doomed.txt")
    git(repo.path, "commit", "-q", "-m", "delete it")
    diffs = agent.call("git.diffForCommit",
                       {"path": repo.path, "sha": repo.head()})["diffs"]
    doomed = [d for d in diffs
              if "doomed.txt" in (d["oldPath"], d["newPath"])]
    assert doomed, "deletion not reported: %r" % diffs
    assert doomed[0]["oldPath"] == "doomed.txt"


def test_diff_for_commit_rename(agent, repo):
    git(repo.path, "mv", "file.txt", "renamed.txt")
    git(repo.path, "commit", "-q", "-m", "rename")
    diffs = agent.call("git.diffForCommit",
                       {"path": repo.path, "sha": repo.head()})["diffs"]
    paths = {(d["oldPath"], d["newPath"]) for d in diffs}
    assert any("renamed.txt" in pair for pair in paths), \
        "rename not represented: %r" % paths


def test_diff_for_unknown_sha_is_empty(agent, repo):
    """Documented parity: an unknown sha yields no diffs, not an error."""
    assert agent.call("git.diffForCommit",
                      {"path": repo.path, "sha": "0" * 40})["diffs"] == []


def test_diff_for_commit_requires_sha(agent, repo):
    assert agent.call_expect_error(
        "git.diffForCommit", {"path": repo.path}).code == INVALID_REQUEST


def test_diff_for_merge_commit(agent, repo):
    repo.checkout("side", create=True)
    repo.commit("side change", {"side.txt": "s\n"})
    repo.checkout("work")
    git(repo.path, "merge", "-q", "--no-ff", "-m", "merge side", "side")
    result = agent.call("git.diffForCommit",
                        {"path": repo.path, "sha": repo.head()})
    assert isinstance(result["diffs"], list)


# ---------------------------------------------------------------------
# workingTreeDiff
# ---------------------------------------------------------------------
def test_working_tree_diff_clean(agent, repo):
    assert agent.call("git.workingTreeDiff",
                      {"path": repo.path})["diffs"] == []


def test_working_tree_diff_unstaged_edit(agent, repo):
    repo.dirty(content="workdir edit\n")
    diffs = agent.call("git.workingTreeDiff", {"path": repo.path})["diffs"]
    assert len(diffs) == 1
    assert diffs[0]["newPath"] == "file.txt"
    assert_file_diff_shape(diffs[0], "workingTreeDiff")
    assert "workdir edit" in diffs[0]["patch"]


def test_working_tree_diff_includes_untracked(agent, repo):
    """Documented: index -> workdir, including untracked files."""
    repo.write("untracked.txt", "brand new\n")
    diffs = agent.call("git.workingTreeDiff", {"path": repo.path})["diffs"]
    paths = {d["newPath"] for d in diffs}
    assert "untracked.txt" in paths, \
        "untracked file missing from workingTreeDiff: %r" % paths


def test_working_tree_diff_excludes_staged(agent, repo):
    """Once staged, an edit leaves the working-tree diff."""
    repo.stage(content="staged edit\n")
    assert agent.call("git.workingTreeDiff",
                      {"path": repo.path})["diffs"] == []


def test_working_tree_diff_deleted_file(agent, repo):
    import os
    os.unlink(repo.join("file.txt"))
    diffs = agent.call("git.workingTreeDiff", {"path": repo.path})["diffs"]
    assert diffs, "deleting a tracked file produced no working-tree diff"


# ---------------------------------------------------------------------
# stagedDiff
# ---------------------------------------------------------------------
def test_staged_diff_clean(agent, repo):
    assert agent.call("git.stagedDiff", {"path": repo.path})["diffs"] == []


def test_staged_diff_after_add(agent, repo):
    repo.stage(content="staged edit\n")
    diffs = agent.call("git.stagedDiff", {"path": repo.path})["diffs"]
    assert len(diffs) == 1
    assert diffs[0]["newPath"] == "file.txt"
    assert_file_diff_shape(diffs[0], "stagedDiff")
    assert "staged edit" in diffs[0]["patch"]


def test_staged_diff_excludes_unstaged(agent, repo):
    repo.dirty(content="only in workdir\n")
    assert agent.call("git.stagedDiff", {"path": repo.path})["diffs"] == []


def test_staged_and_unstaged_are_separated(agent, repo):
    """A file edited, staged, then edited again appears in both, differently."""
    repo.stage(content="staged part\n")
    repo.dirty(content="unstaged part\n")
    staged = agent.call("git.stagedDiff", {"path": repo.path})["diffs"]
    working = agent.call("git.workingTreeDiff", {"path": repo.path})["diffs"]
    assert staged and working
    assert "staged part" in staged[0]["patch"]
    assert "unstaged part" in working[0]["patch"]
    assert "unstaged part" not in staged[0]["patch"], \
        "stagedDiff leaked an unstaged change"


def test_staged_diff_new_file(agent, repo):
    repo.write("added.txt", "new file\n")
    git(repo.path, "add", "added.txt")
    diffs = agent.call("git.stagedDiff", {"path": repo.path})["diffs"]
    assert any(d["newPath"] == "added.txt" for d in diffs)


@pytest.mark.parametrize("method", ["git.workingTreeDiff", "git.stagedDiff"])
def test_diff_surfaces_require_path(agent, method):
    assert agent.call_expect_error(method, {}).code == INVALID_REQUEST


def test_large_diff_streams(agent, repo):
    """A diff big enough to exceed the frame cap reassembles intact."""
    body = "".join("line %05d\n" % n for n in range(20000))
    repo.commit("huge", {"huge.txt": body})
    diffs = agent.call("git.diffForCommit",
                       {"path": repo.path, "sha": repo.head()},
                       timeout=120)["diffs"]
    huge = next(d for d in diffs if d["newPath"] == "huge.txt")
    added = sum(1 for hunk in huge["hunks"]
                for line in hunk["lines"] if line["origin"] == "+")
    assert added == 20000, "streamed diff lost lines (%d of 20000)" % added
