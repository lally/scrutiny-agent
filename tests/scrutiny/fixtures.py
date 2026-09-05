"""Hermetic fixtures: git repositories and file trees on local disk.

Nothing here touches the network or a real forge. Repositories are
built with the `git` CLI under a pinned identity and with the user's
global/system git config neutralized, so results do not depend on the
machine running the tests.
"""
import os
import shutil
import subprocess
import tempfile

# Neutralize ambient git configuration: no user identity leakage, no
# host-specific defaults (hooks, templates, signing, default branch).
GIT_ENV = dict(
    os.environ,
    GIT_AUTHOR_NAME="Scrutiny Test",
    GIT_AUTHOR_EMAIL="test@scrutiny.invalid",
    GIT_COMMITTER_NAME="Scrutiny Test",
    GIT_COMMITTER_EMAIL="test@scrutiny.invalid",
    GIT_AUTHOR_DATE="2024-01-01T00:00:00+0000",
    GIT_COMMITTER_DATE="2024-01-01T00:00:00+0000",
    GIT_CONFIG_GLOBAL="/dev/null",
    GIT_CONFIG_SYSTEM="/dev/null",
    GIT_TERMINAL_PROMPT="0",
    GIT_ASKPASS="",
)


def git(cwd, *args, capture=False, check=True):
    """Run a git command in `cwd`; return stripped stdout when captured."""
    proc = subprocess.run(
        ["git", "-C", str(cwd), *args], env=GIT_ENV, check=False,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
    if check and proc.returncode != 0:
        raise AssertionError(
            "git %s failed (%d) in %s:\n%s"
            % (" ".join(args), proc.returncode, cwd,
               proc.stderr.decode("utf-8", "replace")))
    return proc.stdout.decode("utf-8", "replace").strip() if capture else None


class Workspace:
    """A throwaway directory tree, cleaned up on `destroy()`."""

    def __init__(self, prefix="scrutiny-test-", parent=None):
        self.path = tempfile.mkdtemp(prefix=prefix, dir=parent)

    def __str__(self):
        return self.path

    def join(self, *parts):
        return os.path.join(self.path, *parts)

    def write(self, relpath, content, mode=None):
        """Create/overwrite a file (parents created); return its path."""
        target = self.join(relpath)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        data = content.encode() if isinstance(content, str) else content
        with open(target, "wb") as handle:
            handle.write(data)
        if mode is not None:
            os.chmod(target, mode)
        return target

    def read(self, relpath):
        with open(self.join(relpath), "rb") as handle:
            return handle.read()

    def mkdir(self, relpath):
        target = self.join(relpath)
        os.makedirs(target, exist_ok=True)
        return target

    def symlink(self, target, relpath):
        link = self.join(relpath)
        os.makedirs(os.path.dirname(link), exist_ok=True)
        os.symlink(target, link)
        return link

    def destroy(self):
        shutil.rmtree(self.path, ignore_errors=True)


class GitRepo(Workspace):
    """A git repository built commit by commit.

    The default layout (`build_default`) is two commits on branch
    `work`, editing one file, so diff/commit/showFile assertions have
    a known-exact expected value.
    """

    FIRST = "line1\nline2\n"
    SECOND = "line1\nCHANGED\nline3\n"

    def __init__(self, prefix="scrutiny-repo-", parent=None, bare=False,
                 initial_branch="work"):
        super().__init__(prefix=prefix, parent=parent)
        self.initial_branch = initial_branch
        if bare:
            git(self.path, "init", "-q", "--bare",
                "--initial-branch=" + initial_branch)
        else:
            git(self.path, "init", "-q", "--initial-branch=" + initial_branch)
        self.shas = []

    # -- building --------------------------------------------------------
    def commit(self, message, files=None, allow_empty=False):
        """Write `files` ({relpath: content}) and commit; return the sha."""
        for relpath, content in (files or {}).items():
            self.write(relpath, content)
        git(self.path, "add", "-A")
        args = ["commit", "-q", "-m", message]
        if allow_empty:
            args.append("--allow-empty")
        git(self.path, *args)
        sha = self.head()
        self.shas.append(sha)
        return sha

    def build_default(self):
        """Two commits on `work`; returns (head_sha, parent_sha)."""
        parent = self.commit("first", {"file.txt": self.FIRST})
        head = self.commit("second", {"file.txt": self.SECOND})
        return head, parent

    def branch(self, name, start_point=None):
        git(self.path, "branch", name, *( [start_point] if start_point else []))
        return name

    def checkout(self, name, create=False):
        git(self.path, "checkout", "-q", *(["-b"] if create else []), name)
        return name

    def add_remote(self, name, url):
        git(self.path, "remote", "add", name, url)

    def dirty(self, relpath="file.txt", content="dirty\n"):
        """Make an unstaged working-tree modification."""
        with open(self.join(relpath), "a") as handle:
            handle.write(content)

    def stage(self, relpath="file.txt", content="staged\n"):
        """Make a staged (index-only) modification."""
        with open(self.join(relpath), "a") as handle:
            handle.write(content)
        git(self.path, "add", relpath)

    # -- querying --------------------------------------------------------
    def head(self):
        return git(self.path, "rev-parse", "HEAD", capture=True)

    def rev(self, spec):
        return git(self.path, "rev-parse", spec, capture=True)

    def current_branch(self):
        return git(self.path, "rev-parse", "--abbrev-ref", "HEAD",
                   capture=True)

    def clone_to(self, prefix="scrutiny-clone-", parent=None):
        """Clone this repo over the filesystem; returns a GitRepo view."""
        dest = tempfile.mkdtemp(prefix=prefix, dir=parent)
        shutil.rmtree(dest)
        proc = subprocess.run(["git", "clone", "-q", self.path, dest],
                              env=GIT_ENV, check=False,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=120)
        if proc.returncode != 0:
            raise AssertionError("git clone failed: %s"
                                 % proc.stderr.decode("utf-8", "replace"))
        view = GitRepo.__new__(GitRepo)
        view.path = dest
        view.initial_branch = self.initial_branch
        view.shas = list(self.shas)
        return view
