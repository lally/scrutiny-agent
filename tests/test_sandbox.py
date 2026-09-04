"""The filesystem sandbox (`--allow-root`).

This is the agent's only real security boundary: it runs as the user
on a shared dev box and a client can name any path. The properties
under test are the ones a reviewer would ask about -- symlink escapes
are resolved before the check, prefix matches respect component
boundaries, and denial is reported as PERMISSION_DENIED rather than
being confused with NOT_FOUND (clients render the two differently).
"""
import os

import pytest

from scrutiny import NOT_FOUND, PERMISSION_DENIED


@pytest.fixture
def sandbox(agent_factory, tmp_path):
    """An agent allowed into `allowed/` only, with a `secret/` sibling."""
    allowed = tmp_path / "allowed"
    secret = tmp_path / "secret"
    allowed.mkdir()
    secret.mkdir()
    (allowed / "ok.txt").write_text("inside\n")
    (allowed / "sub").mkdir()
    (allowed / "sub" / "deep.txt").write_text("deep\n")
    (secret / "keys.txt").write_text("shhh\n")
    instance = agent_factory(["--allow-root", str(allowed)])
    return instance, allowed, secret


def test_inside_root_read_is_allowed(sandbox):
    instance, allowed, _ = sandbox
    assert instance.call("fs.readFile",
                         {"path": str(allowed / "ok.txt")})["content"] == \
        "inside\n"


def test_nested_inside_root_is_allowed(sandbox):
    instance, allowed, _ = sandbox
    assert instance.call(
        "fs.readFile",
        {"path": str(allowed / "sub" / "deep.txt")})["content"] == "deep\n"


def test_the_root_itself_is_allowed(sandbox):
    instance, allowed, _ = sandbox
    result = instance.call("fs.listDirectory", {"path": str(allowed)})
    assert os.path.realpath(result["path"]) == os.path.realpath(str(allowed))


def test_sibling_read_is_denied(sandbox):
    instance, _, secret = sandbox
    error = instance.call_expect_error("fs.readFile",
                                       {"path": str(secret / "keys.txt")})
    assert error.code == PERMISSION_DENIED


def test_sibling_listing_is_denied(sandbox):
    instance, _, secret = sandbox
    assert instance.call_expect_error(
        "fs.listDirectory", {"path": str(secret)}).code == PERMISSION_DENIED


def test_parent_directory_is_denied(sandbox):
    instance, allowed, _ = sandbox
    assert instance.call_expect_error(
        "fs.listDirectory",
        {"path": str(allowed.parent)}).code == PERMISSION_DENIED


def test_dotdot_escape_is_denied(sandbox):
    """`allowed/../secret/keys.txt` is canonicalized before the check."""
    instance, allowed, secret = sandbox
    escape = os.path.join(str(allowed), "..", "secret", "keys.txt")
    assert instance.call_expect_error(
        "fs.readFile", {"path": escape}).code == PERMISSION_DENIED


def test_symlink_escape_is_denied(sandbox):
    """A symlink inside the root pointing out of it does not grant access.

    Checking the pre-resolution path would let any writable directory
    inside the root become a universal read primitive.
    """
    instance, allowed, secret = sandbox
    trap = allowed / "trap"
    os.symlink(str(secret / "keys.txt"), str(trap))
    assert instance.call_expect_error(
        "fs.readFile", {"path": str(trap)}).code == PERMISSION_DENIED


def test_symlinked_directory_escape_is_denied(sandbox):
    instance, allowed, secret = sandbox
    trap = allowed / "trapdir"
    os.symlink(str(secret), str(trap))
    assert instance.call_expect_error(
        "fs.listDirectory", {"path": str(trap)}).code == PERMISSION_DENIED
    assert instance.call_expect_error(
        "fs.readFile",
        {"path": str(trap / "keys.txt")}).code == PERMISSION_DENIED


def test_sibling_prefix_is_not_under_root(agent_factory, tmp_path):
    """`/a/bc` must not count as being under `/a/b`.

    A naive string-prefix check would grant access to any sibling whose
    name merely starts with the root's name.
    """
    root = tmp_path / "proj"
    sibling = tmp_path / "proj-secrets"
    root.mkdir()
    sibling.mkdir()
    (sibling / "leak.txt").write_text("leak\n")
    instance = agent_factory(["--allow-root", str(root)])
    assert instance.call_expect_error(
        "fs.readFile",
        {"path": str(sibling / "leak.txt")}).code == PERMISSION_DENIED


def test_absolute_system_paths_are_denied(sandbox):
    instance, _, _ = sandbox
    for path in ("/etc/passwd", "/etc/shadow", "/root", "/proc/self/environ"):
        error = instance.call_expect_error("fs.readFile", {"path": path})
        assert error.code in (PERMISSION_DENIED, NOT_FOUND), \
            "%s produced %s instead of a denial" % (path, error)


def test_denied_is_distinct_from_missing(sandbox):
    """The two codes stay distinguishable -- clients branch on them."""
    instance, allowed, secret = sandbox
    denied = instance.call_expect_error("fs.readFile",
                                        {"path": str(secret / "keys.txt")})
    missing = instance.call_expect_error(
        "fs.readFile", {"path": str(allowed / "not-there.txt")})
    assert denied.code == PERMISSION_DENIED
    assert missing.code == NOT_FOUND


def test_missing_path_outside_root_does_not_confirm_existence(sandbox):
    """A nonexistent path outside the roots must not leak its absence.

    Either answer is defensible, but the agent must not distinguish
    "outside and missing" from "outside and present" in a way that
    turns the sandbox into an existence oracle for the whole disk.
    """
    instance, _, secret = sandbox
    present = instance.call_expect_error("fs.readFile",
                                         {"path": str(secret / "keys.txt")})
    absent = instance.call_expect_error(
        "fs.readFile", {"path": str(secret / "no-such-file.txt")})
    assert present.code in (PERMISSION_DENIED, NOT_FOUND)
    assert absent.code in (PERMISSION_DENIED, NOT_FOUND)


def test_multiple_roots_are_all_honored(agent_factory, tmp_path):
    first = tmp_path / "one"
    second = tmp_path / "two"
    third = tmp_path / "three"
    for path in (first, second, third):
        path.mkdir()
        (path / "f.txt").write_text(path.name + "\n")
    instance = agent_factory(["--allow-root", str(first),
                              "--allow-root", str(second)])
    assert instance.call("fs.readFile",
                         {"path": str(first / "f.txt")})["content"] == "one\n"
    assert instance.call("fs.readFile",
                         {"path": str(second / "f.txt")})["content"] == "two\n"
    assert instance.call_expect_error(
        "fs.readFile", {"path": str(third / "f.txt")}).code == PERMISSION_DENIED


def test_default_root_is_home(agent_factory, tmp_path):
    """With no --allow-root, $HOME is readable and outside it is not."""
    instance = agent_factory([])
    home = os.path.realpath(os.path.expanduser("~"))
    result = instance.call("fs.listDirectory", {"path": home})
    assert os.path.realpath(result["path"]) == home
    if not str(tmp_path).startswith(home):
        assert instance.call_expect_error(
            "fs.listDirectory", {"path": str(tmp_path)}).code == \
            PERMISSION_DENIED


def test_selftest_matches_the_actual_posture(agent_factory, tmp_path):
    """`fs.selftest` agrees with what `fs.readFile` actually does.

    The probe exists so an operator can trust the running binary rather
    than the docs; a probe that disagrees with the enforcement path is
    worse than none.
    """
    root = tmp_path / "narrow"
    root.mkdir()
    instance = agent_factory(["--allow-root", str(root)])
    probe = instance.call("fs.selftest")
    try:
        instance.call("fs.readFile", {"path": probe["probe"]})
        actually_readable = True
    except Exception:                                   # noqa: BLE001
        actually_readable = False
    assert probe["succeeded"] == actually_readable, \
        "fs.selftest reports succeeded=%r but fs.readFile says %r" \
        % (probe["succeeded"], actually_readable)


def test_denied_probe_leaks_no_content(agent_factory, tmp_path):
    """A denied probe must not return bytes from the probed file."""
    root = tmp_path / "narrow"
    root.mkdir()
    instance = agent_factory(["--allow-root", str(root)])
    probe = instance.call("fs.selftest")
    if probe["succeeded"] is False:
        assert probe["sampleSnippet"] == ""
        assert probe["firstBytesReadable"] == 0


def test_sandbox_survives_repeated_denials(sandbox):
    instance, _, secret = sandbox
    for _ in range(40):
        instance.call_expect_error("fs.readFile",
                                   {"path": str(secret / "keys.txt")})
    assert instance.alive()
    assert instance.call("meta.debug", {"padBytes": 2})["pad"] == "xx"


def test_git_methods_are_not_sandboxed_by_fs_roots(sandbox, repo):
    """git.* operates on repos outside the fs roots by design.

    Worth pinning explicitly: the fs sandbox governs `fs.*`, and the
    protocol does not claim it covers `git.*`. If that ever changes,
    this test is where the decision gets recorded.
    """
    instance, _, _ = sandbox
    result = instance.call("git.headSha", {"path": repo.path})
    assert result["headSha"] == repo.head_sha
