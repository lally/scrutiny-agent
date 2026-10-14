"""What `hello.features` claims must be true on the platform the agent
runs on.  A client that is told `fs.watch` keeps a watched listing
forever; told it where watches are silently declined, it would never
see a change made behind its back."""

import os
import sys


def test_watch_features_are_claimed_only_where_watches_work(agent):
    features = set(agent.hello["features"])
    on_linux = sys.platform.startswith("linux")
    assert ("fs.watch" in features) == on_linux
    assert ("fs.changed.ordered" in features) == on_linux
    # The rest of the feature set does not depend on the platform.
    assert {"hello.facts", "fs.mtimeNs", "fs.writeFile.ifMtimeNs",
            "git.exec.stdin", "git.exec.env", "rpc.deflate"} <= features


def test_roots_are_reported_as_given_when_a_symlink_renames_them(
        agent_factory, tmp_path):
    """A root configured through a symlink is enforced by its canonical
    path, and reported under both spellings so a client can tell a
    name written the configured way is inside the sandbox."""
    real = tmp_path / "real"
    real.mkdir()
    link = tmp_path / "link"
    link.symlink_to(real)
    agent = agent_factory(["--allow-root", str(link)])
    fsa = agent.hello["fileSystemAccess"]
    assert fsa["allowedRoots"] == [os.path.realpath(str(real))]
    assert fsa["allowedRootsAsGiven"] == [str(link)]
    # A root whose spelling realpath() leaves alone has no alias.
    plain = agent_factory(["--allow-root", os.path.realpath(str(real))])
    assert plain.hello["fileSystemAccess"]["allowedRootsAsGiven"] == []
    # The sandbox is unchanged: the alias is served, and so is the
    # canonical name.
    (real / "f.txt").write_text("x\n")
    assert agent.call("fs.readFile", {"path": str(link / "f.txt")})
    assert agent.call("fs.readFile", {"path": str(real / "f.txt")})
