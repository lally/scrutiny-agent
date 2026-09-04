"""The fs metadata and write surface.

`fs.stat` / `fs.statBatch` / `fs.listDirectory attributes` exist so a
client can answer a directory's worth of questions in one round trip;
the write methods exist so saving a file does not have to go through a
remote shell. Writes are opt-in (`--allow-write`) and stay inside the
same `--allow-root` sandbox, so a good half of this file is about what
must still be refused.
"""
import base64
import os
import stat as stat_module

import pytest

from scrutiny import INVALID_REQUEST, NOT_FOUND, PERMISSION_DENIED


@pytest.fixture
def rw_agent(agent_factory, tmp_path):
    """An agent that may write, rooted at the pytest tmp directory."""
    return agent_factory(["--allow-root", str(tmp_path), "--allow-write"])


@pytest.fixture
def ro_agent(agent_factory, tmp_path):
    return agent_factory(["--allow-root", str(tmp_path)])


# ---------------------------------------------------------------------
# opt-in
# ---------------------------------------------------------------------
def test_writes_are_off_by_default(ro_agent, tmp_path):
    error = ro_agent.call_expect_error(
        "fs.writeFile", {"path": str(tmp_path / "x.txt"), "content": "x"})
    assert error.code == PERMISSION_DENIED
    assert "read-only" in str(error)


@pytest.mark.parametrize("method,params", [
    ("fs.writeFile", {"content": "x"}),
    ("fs.mkdir", {}),
    ("fs.delete", {}),
    ("fs.rename", {"to": "/tmp/y"}),
    ("fs.copy", {"to": "/tmp/y"}),
    ("fs.chmod", {"mode": 420}),
])
def test_every_write_method_is_gated(ro_agent, tmp_path, method, params):
    payload = dict(params)
    key = "from" if method in ("fs.rename", "fs.copy") else "path"
    payload[key] = str(tmp_path / "x.txt")
    assert ro_agent.call_expect_error(method, payload).code == \
        PERMISSION_DENIED


def test_write_methods_not_advertised_when_off(ro_agent):
    for method in ("fs.writeFile", "fs.mkdir", "fs.delete", "fs.rename",
                   "fs.copy", "fs.chmod"):
        assert method not in ro_agent.capabilities


def test_write_methods_advertised_when_on(rw_agent):
    for method in ("fs.writeFile", "fs.mkdir", "fs.delete", "fs.rename",
                   "fs.copy", "fs.chmod"):
        assert method in rw_agent.capabilities


def test_read_metadata_is_always_available(ro_agent):
    assert "fs.stat" in ro_agent.capabilities
    assert "fs.statBatch" in ro_agent.capabilities


def test_capabilities_report_writability(ro_agent, rw_agent):
    assert ro_agent.call(
        "meta.capabilities")["fileSystemAccess"]["writable"] is False
    assert rw_agent.call(
        "meta.capabilities")["fileSystemAccess"]["writable"] is True


# ---------------------------------------------------------------------
# stat
# ---------------------------------------------------------------------
def test_stat_regular_file(ro_agent, tmp_path):
    target = tmp_path / "a.txt"
    target.write_text("0123456789")
    result = ro_agent.call("fs.stat", {"path": str(target)})
    assert result["exists"] is True
    assert result["isRegular"] is True
    assert result["isDir"] is False
    assert result["size"] == 10
    assert result["readable"] is True
    assert result["mtime"] == int(target.stat().st_mtime)
    assert result["mode"] == stat_module.S_IMODE(target.stat().st_mode)


def test_stat_directory(ro_agent, tmp_path):
    result = ro_agent.call("fs.stat", {"path": str(tmp_path)})
    assert result["exists"] is True and result["isDir"] is True


def test_stat_missing_is_not_an_error(ro_agent, tmp_path):
    """"Does this exist?" is the question; absence is the answer."""
    result = ro_agent.call("fs.stat", {"path": str(tmp_path / "nope")})
    assert result["exists"] is False


def test_stat_describes_the_symlink_not_its_target(ro_agent, tmp_path):
    """A stat that follows the final link can never report a symlink.

    Canonicalizing the whole path before stat'ing resolves the link
    away, so `isSymlink` would be false for every symlink in existence
    and dired would render them as ordinary files.
    """
    (tmp_path / "target.txt").write_text("t")
    os.symlink("target.txt", str(tmp_path / "link.txt"))
    result = ro_agent.call("fs.stat", {"path": str(tmp_path / "link.txt")})
    assert result["isSymlink"] is True
    assert result["symlinkTarget"] == "target.txt"
    # ...while `path` still names the resolved target, for file-truename.
    assert result["path"].endswith("target.txt")


def test_stat_dangling_symlink(ro_agent, tmp_path):
    os.symlink("does-not-exist", str(tmp_path / "dangling"))
    result = ro_agent.call("fs.stat", {"path": str(tmp_path / "dangling")})
    assert result["exists"] is True
    assert result["isSymlink"] is True


def test_stat_outside_the_roots_is_denied(ro_agent):
    assert ro_agent.call_expect_error(
        "fs.stat", {"path": "/etc/passwd"}).code == PERMISSION_DENIED


def test_stat_requires_path(ro_agent):
    assert ro_agent.call_expect_error("fs.stat", {}).code == INVALID_REQUEST


def test_stat_batch(ro_agent, tmp_path):
    (tmp_path / "one.txt").write_text("1")
    (tmp_path / "two.txt").write_text("22")
    result = ro_agent.call("fs.statBatch",
                           {"paths": [str(tmp_path / "one.txt"),
                                      str(tmp_path / "two.txt"),
                                      str(tmp_path / "missing.txt")]})
    stats = result["stats"]
    assert len(stats) == 3, "the batch must answer in the order asked"
    assert stats[0]["size"] == 1
    assert stats[1]["size"] == 2
    assert stats[2]["exists"] is False


def test_stat_batch_isolates_denied_paths(ro_agent, tmp_path):
    """One bad path must not fail the whole batch."""
    (tmp_path / "ok.txt").write_text("x")
    stats = ro_agent.call("fs.statBatch",
                          {"paths": [str(tmp_path / "ok.txt"),
                                     "/etc/passwd"]})["stats"]
    assert stats[0]["exists"] is True
    assert stats[1]["exists"] is False and stats[1].get("denied") is True


def test_stat_batch_empty(ro_agent):
    assert ro_agent.call("fs.statBatch", {"paths": []})["stats"] == []


def test_stat_batch_requires_array(ro_agent):
    assert ro_agent.call_expect_error("fs.statBatch",
                                      {}).code == INVALID_REQUEST


def test_stat_batch_matches_individual_stats(ro_agent, tmp_path):
    names = ["f%02d.txt" % n for n in range(20)]
    for index, name in enumerate(names):
        (tmp_path / name).write_text("x" * index)
    paths = [str(tmp_path / name) for name in names]
    batch = ro_agent.call("fs.statBatch", {"paths": paths})["stats"]
    for path, batched in zip(paths, batch):
        single = ro_agent.call("fs.stat", {"path": path})
        assert batched["size"] == single["size"]
        assert batched["mode"] == single["mode"]


# ---------------------------------------------------------------------
# listing with attributes
# ---------------------------------------------------------------------
def test_listing_attributes(ro_agent, tmp_path):
    (tmp_path / "a.txt").write_text("12345")
    (tmp_path / "sub").mkdir()
    entries = ro_agent.call("fs.listDirectory",
                            {"path": str(tmp_path),
                             "attributes": True})["entries"]
    by_name = {e["name"]: e for e in entries}
    assert by_name["a.txt"]["size"] == 5
    assert by_name["a.txt"]["isRegular"] is True
    assert by_name["sub"]["isDir"] is True
    assert "mtime" in by_name["a.txt"] and "mode" in by_name["a.txt"]


def test_listing_without_attributes_is_unchanged(ro_agent, tmp_path):
    """The added parameter must not change the default shape."""
    (tmp_path / "a.txt").write_text("x")
    entries = ro_agent.call("fs.listDirectory",
                            {"path": str(tmp_path)})["entries"]
    assert set(entries[0]) == {"name", "isDir"}


def test_listing_attributes_flag_symlinks(ro_agent, tmp_path):
    (tmp_path / "target.txt").write_text("t")
    os.symlink("target.txt", str(tmp_path / "link.txt"))
    entries = ro_agent.call("fs.listDirectory",
                            {"path": str(tmp_path),
                             "attributes": True})["entries"]
    link = next(e for e in entries if e["name"] == "link.txt")
    assert link["isSymlink"] is True


def test_listing_attributes_match_stat(ro_agent, tmp_path):
    for n in range(10):
        (tmp_path / ("f%d.txt" % n)).write_text("x" * n)
    entries = ro_agent.call("fs.listDirectory",
                            {"path": str(tmp_path),
                             "attributes": True})["entries"]
    for entry in entries:
        single = ro_agent.call("fs.stat",
                               {"path": str(tmp_path / entry["name"])})
        assert entry["size"] == single["size"]


# ---------------------------------------------------------------------
# readFile extensions
# ---------------------------------------------------------------------
def test_read_file_base64_is_byte_exact(ro_agent, tmp_path):
    """A JSON string cannot carry arbitrary bytes; base64 can."""
    blob = bytes(range(256)) * 4
    target = tmp_path / "blob.bin"
    target.write_bytes(blob)
    result = ro_agent.call("fs.readFile", {"path": str(target),
                                           "base64": True})
    assert base64.b64decode(result["contentBase64"]) == blob


def test_read_file_with_stat(ro_agent, tmp_path):
    target = tmp_path / "a.txt"
    target.write_text("hello")
    result = ro_agent.call("fs.readFile", {"path": str(target), "stat": True})
    assert result["content"] == "hello"
    assert result["size"] == 5
    assert result["mtime"] == int(target.stat().st_mtime)


def test_read_file_default_shape_unchanged(ro_agent, tmp_path):
    target = tmp_path / "a.txt"
    target.write_text("hello")
    assert set(ro_agent.call("fs.readFile", {"path": str(target)})) == \
        {"content"}


# ---------------------------------------------------------------------
# writing
# ---------------------------------------------------------------------
def test_write_creates_a_file(rw_agent, tmp_path):
    target = tmp_path / "new.txt"
    result = rw_agent.call("fs.writeFile", {"path": str(target),
                                            "content": "written\n"})
    assert result["ok"] is True
    assert result["size"] == 8
    assert target.read_text() == "written\n"


def test_write_reports_the_new_mtime(rw_agent, tmp_path):
    """The client needs the mtime to track the buffer without a re-stat."""
    target = tmp_path / "a.txt"
    result = rw_agent.call("fs.writeFile", {"path": str(target),
                                            "content": "x"})
    assert result["mtime"] == int(target.stat().st_mtime)


def test_write_overwrites(rw_agent, tmp_path):
    target = tmp_path / "a.txt"
    target.write_text("before")
    rw_agent.call("fs.writeFile", {"path": str(target), "content": "after"})
    assert target.read_text() == "after"


def test_write_binary(rw_agent, tmp_path):
    blob = bytes(range(256))
    target = tmp_path / "b.bin"
    rw_agent.call("fs.writeFile",
                  {"path": str(target),
                   "contentBase64": base64.b64encode(blob).decode()})
    assert target.read_bytes() == blob


def test_write_empty_file(rw_agent, tmp_path):
    target = tmp_path / "empty.txt"
    rw_agent.call("fs.writeFile", {"path": str(target), "content": ""})
    assert target.exists() and target.read_bytes() == b""


def test_write_unicode(rw_agent, tmp_path):
    target = tmp_path / "u.txt"
    body = "héllo wörld \U0001F600\n"
    rw_agent.call("fs.writeFile", {"path": str(target), "content": body})
    assert target.read_text() == body


def test_write_large_file(rw_agent, tmp_path):
    body = "".join("line %06d\n" % n for n in range(50000))
    target = tmp_path / "big.txt"
    rw_agent.call("fs.writeFile",
                  {"path": str(target),
                   "contentBase64": base64.b64encode(body.encode()).decode()},
                  timeout=120)
    assert target.read_text() == body


def test_write_preserves_existing_mode(rw_agent, tmp_path):
    """Saving an executable script must not silently drop its +x bit."""
    target = tmp_path / "s.sh"
    target.write_text("#!/bin/sh\n")
    target.chmod(0o755)
    rw_agent.call("fs.writeFile", {"path": str(target),
                                   "content": "#!/bin/sh\necho hi\n"})
    assert stat_module.S_IMODE(target.stat().st_mode) == 0o755


def test_write_honors_explicit_mode(rw_agent, tmp_path):
    target = tmp_path / "m.txt"
    rw_agent.call("fs.writeFile", {"path": str(target), "content": "x",
                                   "mode": 0o600})
    assert stat_module.S_IMODE(target.stat().st_mode) == 0o600


def test_write_leaves_no_temp_file(rw_agent, tmp_path):
    """The atomic write's temp file must not survive."""
    rw_agent.call("fs.writeFile", {"path": str(tmp_path / "a.txt"),
                                   "content": "x"})
    leftovers = [n for n in os.listdir(tmp_path) if "scrutiny-write" in n]
    assert leftovers == []


def test_write_is_atomic_on_failure(rw_agent, tmp_path):
    """A refused write leaves the previous contents untouched."""
    target = tmp_path / "keep.txt"
    target.write_text("original")
    target.chmod(0o444)
    parent_mode = stat_module.S_IMODE(tmp_path.stat().st_mode)
    tmp_path.chmod(0o555)              # cannot create the temp file
    try:
        try:
            rw_agent.call("fs.writeFile", {"path": str(target),
                                           "content": "replacement"})
        except Exception:                               # noqa: BLE001
            pass
        assert target.read_text() == "original"
    finally:
        tmp_path.chmod(parent_mode)
        target.chmod(0o644)


def test_write_requires_content(rw_agent, tmp_path):
    assert rw_agent.call_expect_error(
        "fs.writeFile", {"path": str(tmp_path / "x")}).code == INVALID_REQUEST


def test_write_rejects_bad_base64(rw_agent, tmp_path):
    assert rw_agent.call_expect_error(
        "fs.writeFile", {"path": str(tmp_path / "x"),
                         "contentBase64": "!!!not base64!!!"}).code == \
        INVALID_REQUEST


def test_write_create_dirs(rw_agent, tmp_path):
    target = tmp_path / "a" / "b" / "c.txt"
    rw_agent.call("fs.writeFile", {"path": str(target), "content": "deep",
                                   "createDirs": True})
    assert target.read_text() == "deep"


# ---------------------------------------------------------------------
# the sandbox still governs writes
# ---------------------------------------------------------------------
def test_write_outside_the_roots_is_denied(rw_agent, tmp_path):
    for target in ("/tmp/scrutiny-escape.txt", "/etc/scrutiny-escape.txt"):
        error = rw_agent.call_expect_error("fs.writeFile",
                                           {"path": target, "content": "x"})
        assert error.code in (PERMISSION_DENIED, INVALID_REQUEST), \
            "%s was not refused" % target
        assert not os.path.exists(target)


def test_write_through_dotdot_is_denied(rw_agent, tmp_path):
    escape = str(tmp_path / ".." / "scrutiny-escape.txt")
    error = rw_agent.call_expect_error("fs.writeFile",
                                       {"path": escape, "content": "x"})
    assert error.code in (PERMISSION_DENIED, INVALID_REQUEST)
    assert not os.path.exists(os.path.normpath(escape))


def test_write_through_symlinked_parent_is_denied(rw_agent, tmp_path):
    """A symlink inside the roots must not become a write primitive.

    Authorizing a not-yet-existing path means resolving its parent; if
    that resolution were skipped, any writable directory in the sandbox
    could be turned into a door out of it.
    """
    outside = tmp_path.parent / "scrutiny-outside"
    outside.mkdir(exist_ok=True)
    os.symlink(str(outside), str(tmp_path / "door"))
    error = rw_agent.call_expect_error(
        "fs.writeFile", {"path": str(tmp_path / "door" / "pwn.txt"),
                         "content": "x"})
    assert error.code == PERMISSION_DENIED
    assert not (outside / "pwn.txt").exists()


def test_mkdir_outside_the_roots_is_denied(rw_agent):
    assert rw_agent.call_expect_error(
        "fs.mkdir", {"path": "/tmp/scrutiny-escape-dir"}).code in \
        (PERMISSION_DENIED, INVALID_REQUEST)


def test_delete_outside_the_roots_is_denied(rw_agent):
    assert rw_agent.call_expect_error(
        "fs.delete", {"path": "/etc/passwd"}).code == PERMISSION_DENIED


def test_rename_authorizes_both_ends(rw_agent, tmp_path):
    source = tmp_path / "src.txt"
    source.write_text("x")
    error = rw_agent.call_expect_error(
        "fs.rename", {"from": str(source), "to": "/tmp/scrutiny-escape.txt"})
    assert error.code in (PERMISSION_DENIED, INVALID_REQUEST)
    assert source.exists(), "the source was moved despite the refusal"


def test_copy_authorizes_both_ends(rw_agent, tmp_path):
    source = tmp_path / "src.txt"
    source.write_text("x")
    assert rw_agent.call_expect_error(
        "fs.copy", {"from": source.as_posix(),
                    "to": "/tmp/scrutiny-escape.txt"}).code in \
        (PERMISSION_DENIED, INVALID_REQUEST)
    assert not os.path.exists("/tmp/scrutiny-escape.txt")


# ---------------------------------------------------------------------
# the rest of the write surface
# ---------------------------------------------------------------------
def test_mkdir(rw_agent, tmp_path):
    rw_agent.call("fs.mkdir", {"path": str(tmp_path / "one")})
    assert (tmp_path / "one").is_dir()


def test_mkdir_parents(rw_agent, tmp_path):
    rw_agent.call("fs.mkdir", {"path": str(tmp_path / "a" / "b" / "c"),
                               "parents": True})
    assert (tmp_path / "a" / "b" / "c").is_dir()


def test_mkdir_without_parents_fails_on_missing_levels(rw_agent, tmp_path):
    assert rw_agent.call_expect_error(
        "fs.mkdir", {"path": str(tmp_path / "x" / "y")}).code is not None


def test_mkdir_existing_is_ok(rw_agent, tmp_path):
    (tmp_path / "there").mkdir()
    assert rw_agent.call("fs.mkdir",
                         {"path": str(tmp_path / "there")})["ok"] is True


def test_delete_file(rw_agent, tmp_path):
    target = tmp_path / "gone.txt"
    target.write_text("x")
    rw_agent.call("fs.delete", {"path": str(target)})
    assert not target.exists()


def test_delete_missing(rw_agent, tmp_path):
    assert rw_agent.call_expect_error(
        "fs.delete", {"path": str(tmp_path / "never")}).code == NOT_FOUND


def test_delete_nonempty_directory_needs_recursive(rw_agent, tmp_path):
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "f.txt").write_text("x")
    assert rw_agent.call_expect_error(
        "fs.delete", {"path": str(tree)}).code is not None
    assert tree.exists()
    rw_agent.call("fs.delete", {"path": str(tree), "recursive": True})
    assert not tree.exists()


def test_rename(rw_agent, tmp_path):
    source = tmp_path / "from.txt"
    source.write_text("payload")
    rw_agent.call("fs.rename", {"from": str(source),
                                "to": str(tmp_path / "to.txt")})
    assert not source.exists()
    assert (tmp_path / "to.txt").read_text() == "payload"


def test_copy_file(rw_agent, tmp_path):
    source = tmp_path / "from.txt"
    source.write_text("payload")
    rw_agent.call("fs.copy", {"from": str(source),
                              "to": str(tmp_path / "copy.txt")})
    assert source.exists()
    assert (tmp_path / "copy.txt").read_text() == "payload"


def test_copy_directory(rw_agent, tmp_path):
    tree = tmp_path / "tree"
    (tree / "inner").mkdir(parents=True)
    (tree / "inner" / "f.txt").write_text("x")
    rw_agent.call("fs.copy", {"from": str(tree), "to": str(tmp_path / "clone")})
    assert (tmp_path / "clone" / "inner" / "f.txt").read_text() == "x"


def test_chmod(rw_agent, tmp_path):
    target = tmp_path / "m.txt"
    target.write_text("x")
    rw_agent.call("fs.chmod", {"path": str(target), "mode": 0o640})
    assert stat_module.S_IMODE(target.stat().st_mode) == 0o640


def test_chmod_requires_mode(rw_agent, tmp_path):
    target = tmp_path / "m.txt"
    target.write_text("x")
    assert rw_agent.call_expect_error(
        "fs.chmod", {"path": str(target)}).code == INVALID_REQUEST


# ---------------------------------------------------------------------
# concurrency and durability
# ---------------------------------------------------------------------
def test_concurrent_writes_to_distinct_files(rw_agent, tmp_path):
    import threading
    errors = []

    def write(index):
        try:
            rw_agent.call("fs.writeFile",
                          {"path": str(tmp_path / ("c%d.txt" % index)),
                           "content": "content %d" % index}, timeout=60)
        except Exception as exc:                        # noqa: BLE001
            errors.append(exc)

    threads = [threading.Thread(target=write, args=(n,)) for n in range(12)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(90)
    assert not errors, "concurrent writes failed: %r" % errors
    for n in range(12):
        assert (tmp_path / ("c%d.txt" % n)).read_text() == "content %d" % n
    assert [n for n in os.listdir(tmp_path) if "scrutiny-write" in n] == []


def test_write_read_round_trip_many_times(rw_agent, tmp_path):
    target = tmp_path / "cycle.txt"
    for n in range(30):
        body = "revision %d\n" % n
        rw_agent.call("fs.writeFile", {"path": str(target), "content": body})
        assert rw_agent.call("fs.readFile",
                             {"path": str(target)})["content"] == body
    assert rw_agent.alive()


def test_listing_entries_carry_the_resolved_path(ro_agent, tmp_path):
    """Each entry names its resolved path, so a client can answer
    file-truename from the listing rather than stat'ing every file."""
    (tmp_path / "target.txt").write_text("t")
    os.symlink("target.txt", str(tmp_path / "link.txt"))
    entries = ro_agent.call("fs.listDirectory",
                            {"path": str(tmp_path),
                             "attributes": True})["entries"]
    by_name = {e["name"]: e for e in entries}
    assert by_name["target.txt"]["path"] == str(tmp_path / "target.txt")
    # A symlink is flagged as one, and `path` names what it resolves to.
    assert by_name["link.txt"]["isSymlink"] is True
    assert by_name["link.txt"]["path"] == str(tmp_path / "target.txt")


def test_capabilities_report_the_remote_home(ro_agent):
    """A client needs $HOME to expand `~`, and cannot get it by listing
    when $HOME is not itself inside the allowed roots."""
    posture = ro_agent.call("meta.capabilities")["fileSystemAccess"]
    assert posture["home"] == os.path.expanduser("~")


def test_home_is_reported_even_when_unreachable(agent_factory, tmp_path):
    narrow = tmp_path / "narrow"
    narrow.mkdir()
    instance = agent_factory(["--allow-root", str(narrow)])
    posture = instance.call("meta.capabilities")["fileSystemAccess"]
    assert posture["home"] == os.path.expanduser("~")
    # ...while still being unreadable, which is the whole point.
    assert instance.call_expect_error(
        "fs.listDirectory", {"path": "."}).code == PERMISSION_DENIED
