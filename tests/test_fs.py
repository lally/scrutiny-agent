"""`fs.readFile` and `fs.listDirectory`.

The remote directory browser and every "show me that file" path in a
client run through these two methods, so the contract is about exact
content, stable ordering, and correct symlink handling. Sandbox
*denial* lives in test_sandbox.py; this module covers the allowed path.
"""
import os

import pytest

from scrutiny import INVALID_REQUEST, NOT_FOUND


# ---------------------------------------------------------------------
# readFile
# ---------------------------------------------------------------------
def test_read_file_exact(agent, workspace):
    body = "alpha\nbeta\ngamma\n"
    path = workspace.write("plain.txt", body)
    assert agent.call("fs.readFile", {"path": path})["content"] == body


def test_read_empty_file(agent, workspace):
    path = workspace.write("empty.txt", "")
    assert agent.call("fs.readFile", {"path": path})["content"] == ""


def test_read_file_without_trailing_newline(agent, workspace):
    path = workspace.write("bare.txt", "no newline at end")
    assert agent.call("fs.readFile",
                      {"path": path})["content"] == "no newline at end"


def test_read_file_with_crlf(agent, workspace):
    """Line endings are preserved verbatim -- no normalization."""
    body = "one\r\ntwo\r\n"
    path = workspace.write("crlf.txt", body)
    assert agent.call("fs.readFile", {"path": path})["content"] == body


def test_read_file_unicode(agent, workspace):
    body = "héllo wörld \U0001F600 你好\n"
    path = workspace.write("unicode.txt", body)
    assert agent.call("fs.readFile", {"path": path})["content"] == body


def test_read_file_with_tabs_and_control_chars(agent, workspace):
    body = "col1\tcol2\tcol3\n\x0bvertical tab\n"
    path = workspace.write("control.txt", body)
    assert agent.call("fs.readFile", {"path": path})["content"] == body


def test_read_file_is_deterministic(agent, workspace):
    path = workspace.write("stable.txt", "content\n")
    first = agent.call("fs.readFile", {"path": path})["content"]
    second = agent.call("fs.readFile", {"path": path})["content"]
    assert first == second


def test_read_file_sees_updates(agent, workspace):
    """No stale caching on a mutable path."""
    path = workspace.write("mutable.txt", "before\n")
    assert agent.call("fs.readFile", {"path": path})["content"] == "before\n"
    workspace.write("mutable.txt", "after\n")
    assert agent.call("fs.readFile", {"path": path})["content"] == "after\n"


def test_read_file_through_symlink(agent, workspace):
    """A symlink inside the roots resolves to its target's content."""
    target = workspace.write("real.txt", "target content\n")
    link = workspace.join("link.txt")
    os.symlink(target, link)
    assert agent.call("fs.readFile",
                      {"path": link})["content"] == "target content\n"


def test_read_file_missing(agent, workspace):
    assert agent.call_expect_error(
        "fs.readFile", {"path": workspace.join("nope.txt")}).code == NOT_FOUND


def test_read_file_broken_symlink(agent, workspace):
    os.symlink(workspace.join("does-not-exist"), workspace.join("dangling"))
    assert agent.call_expect_error(
        "fs.readFile", {"path": workspace.join("dangling")}).code == NOT_FOUND


def test_read_file_requires_path(agent):
    assert agent.call_expect_error("fs.readFile", {}).code == INVALID_REQUEST


def test_read_relative_path_anchors_under_home(agent):
    """Relative paths resolve under $HOME, per the documented convention."""
    home = os.path.realpath(os.path.expanduser("~"))
    marker = os.path.join(home, ".scrutiny-test-marker")
    with open(marker, "w") as handle:
        handle.write("marker\n")
    try:
        result = agent.call("fs.readFile",
                            {"path": ".scrutiny-test-marker"})
        assert result["content"] == "marker\n"
    finally:
        os.unlink(marker)


def test_read_file_with_spaces_in_name(agent, workspace):
    path = workspace.write("a file with spaces.txt", "spaced\n")
    assert agent.call("fs.readFile", {"path": path})["content"] == "spaced\n"


def test_read_file_with_unicode_name(agent, workspace):
    path = workspace.write("café-∂ata.txt", "unicode name\n")
    assert agent.call("fs.readFile",
                      {"path": path})["content"] == "unicode name\n"


# ---------------------------------------------------------------------
# listDirectory
# ---------------------------------------------------------------------
def test_list_directory_entries_and_order(agent, tree):
    result = agent.call("fs.listDirectory", {"path": tree.path})
    names = [entry["name"] for entry in result["entries"]]
    assert names == sorted(names), "entries are not sorted by name"
    assert names == ["alpha", "beta", "delta.txt", "gamma.txt",
                     "link-to-alpha"]


def test_list_directory_excludes_dot_entries(agent, tree):
    names = [e["name"] for e in
             agent.call("fs.listDirectory", {"path": tree.path})["entries"]]
    assert "." not in names and ".." not in names


def test_list_directory_is_dir_flags(agent, tree):
    flags = {e["name"]: e["isDir"] for e in
             agent.call("fs.listDirectory", {"path": tree.path})["entries"]}
    assert flags["alpha"] is True
    assert flags["beta"] is True
    assert flags["gamma.txt"] is False
    assert flags["delta.txt"] is False


def test_list_directory_follows_symlink_for_is_dir(agent, tree):
    """A symlink pointing at a directory reports isDir true."""
    flags = {e["name"]: e["isDir"] for e in
             agent.call("fs.listDirectory", {"path": tree.path})["entries"]}
    assert flags["link-to-alpha"] is True


def test_list_directory_returns_canonical_path(agent, tree):
    result = agent.call("fs.listDirectory", {"path": tree.path})
    assert result["path"].startswith("/")
    assert ".." not in result["path"]


def test_list_directory_resolves_dot_to_home(agent):
    result = agent.call("fs.listDirectory", {"path": "."})
    assert os.path.realpath(result["path"]) == \
        os.path.realpath(os.path.expanduser("~"))


def test_list_directory_resolves_dotdot(agent, tree):
    """`..` is canonicalized rather than passed through."""
    result = agent.call("fs.listDirectory",
                        {"path": os.path.join(tree.path, "alpha", "..")})
    assert os.path.realpath(result["path"]) == os.path.realpath(tree.path)


def test_list_empty_directory(agent, workspace):
    empty = workspace.mkdir("nothing-here")
    assert agent.call("fs.listDirectory", {"path": empty})["entries"] == []


def test_list_directory_with_hidden_files(agent, workspace):
    workspace.write(".hidden", "h\n")
    workspace.write("visible", "v\n")
    names = [e["name"] for e in
             agent.call("fs.listDirectory",
                        {"path": workspace.path})["entries"]]
    assert ".hidden" in names, "dotfiles should be listed"
    assert "visible" in names


def test_list_directory_missing(agent, workspace):
    assert agent.call_expect_error(
        "fs.listDirectory",
        {"path": workspace.join("nope")}).code == NOT_FOUND


def test_list_directory_requires_path(agent):
    assert agent.call_expect_error("fs.listDirectory",
                                   {}).code == INVALID_REQUEST


def test_list_directory_ordering_with_mixed_case(agent, workspace):
    """Ordering is byte-stable, whatever collation the host prefers."""
    for name in ["Zebra", "apple", "Banana", "cherry"]:
        workspace.write(name, "x")
    names = [e["name"] for e in
             agent.call("fs.listDirectory",
                        {"path": workspace.path})["entries"]]
    assert names == sorted(names), \
        "listing order %r is not a stable sort" % names


def test_list_directory_sees_new_entries(agent, workspace):
    before = agent.call("fs.listDirectory",
                        {"path": workspace.path})["entries"]
    workspace.write("added-later.txt", "x")
    after = agent.call("fs.listDirectory",
                       {"path": workspace.path})["entries"]
    assert len(after) == len(before) + 1


@pytest.mark.parametrize("count", [1, 50, 500])
def test_list_directory_sizes(agent, workspace, count):
    target = workspace.mkdir("sized-%d" % count)
    for n in range(count):
        with open(os.path.join(target, "f%04d" % n), "w") as handle:
            handle.write("x")
    entries = agent.call("fs.listDirectory", {"path": target},
                         timeout=60)["entries"]
    assert len(entries) == count
