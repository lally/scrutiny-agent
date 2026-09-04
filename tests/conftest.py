"""Shared pytest fixtures for the scrutiny-agent test suite.

The agent binary under test comes from (highest precedence first):

  * ``--agent-binary=PATH``
  * ``$SCRUTINY_AGENT_BIN``
  * ``<repo>/build/agent/scrutiny-agent``

Most tests share one session-scoped agent (`agent`) because the agent
is designed to be a long-lived multiplexed server and reusing it is
itself part of the contract under test. Tests that need particular
argv (sandbox roots, logging) spawn their own via the `agent_factory`
fixture, which guarantees cleanup.
"""
import os
import shutil
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from scrutiny import Agent, GitRepo, Workspace              # noqa: E402
from scrutiny.client import agent_version_of                # noqa: E402

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Protocol language enum (docs/protocol.md) -> the server binary the
# agent resolves it to. Used to skip language-specific tests cleanly on
# hosts that lack a given server, and to report what was covered.
LANGUAGE_SERVERS = {
    1: ("rust", "rust-analyzer"),
    2: ("python", "pylsp"),
    3: ("javascript", "typescript-language-server"),
    4: ("typescript", "typescript-language-server"),
    5: ("go", "gopls"),
    6: ("cpp", "clangd"),
    7: ("c", "clangd"),
    8: ("swift", "sourcekit-lsp"),
}


def pytest_addoption(parser):
    group = parser.getgroup("scrutiny-agent")
    group.addoption("--agent-binary", action="store", default=None,
                    help="path to the scrutiny-agent binary under test")
    group.addoption("--run-slow", action="store_true", default=False,
                    help="run tests marked slow (soak / large payloads)")


def pytest_configure(config):
    config.addinivalue_line("markers", "slow: long-running; needs --run-slow")
    config.addinivalue_line("markers", "lsp: needs a real language server")
    # A language server pip-installed alongside pytest (the documented
    # way to get pylsp for this suite) lives in the interpreter's bin
    # directory, which is only on PATH if the venv was activated. The
    # agent resolves servers by searching PATH in its own process, so
    # put it there for every agent this run spawns -- otherwise the
    # whole LSP surface silently skips.
    bindir = os.path.dirname(sys.executable)
    path = os.environ.get("PATH", "")
    if bindir and bindir not in path.split(os.pathsep):
        os.environ["PATH"] = bindir + os.pathsep + path


def pytest_collection_modifyitems(config, items):
    if config.getoption("--run-slow"):
        return
    skip = pytest.mark.skip(reason="needs --run-slow")
    for item in items:
        if "slow" in item.keywords:
            item.add_marker(skip)


# ---------------------------------------------------------------------
# The binary under test
# ---------------------------------------------------------------------
@pytest.fixture(scope="session")
def agent_binary(pytestconfig):
    candidate = (pytestconfig.getoption("--agent-binary")
                 or os.environ.get("SCRUTINY_AGENT_BIN")
                 or os.path.join(REPO_ROOT, "build", "agent", "scrutiny-agent"))
    candidate = os.path.abspath(os.path.expanduser(candidate))
    if not os.path.isfile(candidate) or not os.access(candidate, os.X_OK):
        pytest.exit(
            "scrutiny-agent binary not found or not executable: %s\n"
            "Build it with scripts/build-host.sh, or pass --agent-binary."
            % candidate, returncode=2)
    return candidate


@pytest.fixture(scope="session")
def agent_version(agent_binary):
    version, _proto = agent_version_of(agent_binary)
    return version


@pytest.fixture(scope="session")
def protocol_version(agent_binary):
    _version, proto = agent_version_of(agent_binary)
    return proto


@pytest.fixture(scope="session")
def project_version():
    """The version declared in CMakeLists.txt, for drift checks."""
    with open(os.path.join(REPO_ROOT, "CMakeLists.txt")) as handle:
        for line in handle:
            if line.startswith("project(scrutiny-agent VERSION "):
                return line.split("VERSION", 1)[1].split()[0]
    return None


# ---------------------------------------------------------------------
# Agents
# ---------------------------------------------------------------------
@pytest.fixture(scope="session")
def session_log_path(tmp_path_factory):
    return str(tmp_path_factory.mktemp("agent-log") / "agent.log")


@pytest.fixture(scope="session")
def agent(agent_binary, session_log_path, tmp_path_factory):
    """A shared, handshaked agent with logging and broad sandbox roots.

    Roots cover $HOME and the pytest tmp tree so fixtures anywhere on
    this machine are reachable; sandbox *denial* is tested separately
    against purpose-built agents with narrow roots.
    """
    roots = [os.path.expanduser("~"), str(tmp_path_factory.getbasetemp()),
             "/tmp"]
    args = ["--log", session_log_path, "--log-level", "debug"]
    for root in roots:
        args += ["--allow-root", root]
    instance = Agent(agent_binary, args=args)
    yield instance
    instance.close()


@pytest.fixture
def agent_factory(agent_binary):
    """Spawn purpose-built agents; all are terminated at test teardown."""
    spawned = []

    def make(args=(), **kwargs):
        instance = Agent(agent_binary, args=list(args), **kwargs)
        spawned.append(instance)
        return instance

    yield make
    for instance in spawned:
        instance.close()


@pytest.fixture
def fresh_agent(agent_factory, tmp_path):
    """A private agent with the tmp tree allowed -- for stateful tests."""
    return agent_factory(["--allow-root", str(tmp_path),
                          "--allow-root", os.path.expanduser("~")])


# ---------------------------------------------------------------------
# Repositories and trees
# ---------------------------------------------------------------------
@pytest.fixture
def repo(tmp_path):
    """Two commits on `work`; exposes .head_sha / .parent_sha."""
    instance = GitRepo(parent=str(tmp_path))
    head, parent = instance.build_default()
    instance.head_sha = head
    instance.parent_sha = parent
    yield instance
    instance.destroy()


@pytest.fixture
def repo_factory(tmp_path):
    made = []

    def make(**kwargs):
        instance = GitRepo(parent=str(tmp_path), **kwargs)
        made.append(instance)
        return instance

    yield make
    for instance in made:
        instance.destroy()


@pytest.fixture
def clone_with_upstream(tmp_path):
    """(clone, upstream) where clone's `work` tracks origin/work."""
    upstream = GitRepo(prefix="scrutiny-upstream-", parent=str(tmp_path))
    head, parent = upstream.build_default()
    upstream.head_sha, upstream.parent_sha = head, parent
    clone = upstream.clone_to(parent=str(tmp_path))
    yield clone, upstream
    clone.destroy()
    upstream.destroy()


@pytest.fixture
def tree(tmp_path):
    """A directory tree with subdirs, files and a symlink to a dir."""
    workspace = Workspace(prefix="scrutiny-tree-", parent=str(tmp_path))
    workspace.mkdir("alpha")
    workspace.mkdir("beta")
    workspace.write("gamma.txt", "gamma\n")
    workspace.write("delta.txt", "delta\n")
    workspace.write("alpha/nested.txt", "nested\n")
    workspace.symlink(workspace.join("alpha"), "link-to-alpha")
    yield workspace
    workspace.destroy()


@pytest.fixture
def workspace(tmp_path):
    instance = Workspace(prefix="scrutiny-ws-", parent=str(tmp_path))
    yield instance
    instance.destroy()


# ---------------------------------------------------------------------
# Language servers
# ---------------------------------------------------------------------
def _which(binary):
    """Find `binary` on PATH, or next to the interpreter running pytest.

    A language server installed into the same virtualenv as pytest
    (`pip install python-lsp-server`) is not on PATH unless that venv
    was activated, and silently skipping every LSP test because of it
    would hide exactly the coverage those tests exist for.
    """
    found = shutil.which(binary)
    if found:
        return found
    sibling = os.path.join(os.path.dirname(sys.executable), binary)
    if os.path.isfile(sibling) and os.access(sibling, os.X_OK):
        return sibling
    return None


@pytest.fixture(scope="session")
def available_languages():
    """{language_int: server_path} for servers installed on this host."""
    found = {}
    for lang, (_name, binary) in LANGUAGE_SERVERS.items():
        path = _which(binary)
        if path and _server_runs(path, binary):
            found[lang] = path
    return found


def _server_runs(path, binary):
    """Reject shims that resolve but cannot start (e.g. a rustup stub
    for an uninstalled component), so tests skip instead of failing on
    something that is not the agent's fault."""
    if binary != "rust-analyzer":
        return True
    try:
        proc = subprocess.run([path, "--version"], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=30)
        return proc.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


@pytest.fixture
def language_server(available_languages):
    """Pick an installed language server, preferring fast, cheap ones."""
    def pick(*preferred):
        for lang in (preferred or (2, 6, 7, 1, 5, 4, 3, 8)):
            if lang in available_languages:
                return lang, available_languages[lang]
        pytest.skip("no language server installed for %r"
                    % (preferred or "any",))
    return pick


@pytest.fixture
def python_workspace(workspace):
    """A small Python project that pylsp can answer questions about."""
    workspace.write("lib.py",
                    "def add(a, b):\n"
                    "    \"\"\"Add two numbers.\"\"\"\n"
                    "    return a + b\n"
                    "\n"
                    "\n"
                    "class Calculator:\n"
                    "    def total(self, values):\n"
                    "        result = 0\n"
                    "        for value in values:\n"
                    "            result = add(result, value)\n"
                    "        return result\n")
    workspace.write("main.py",
                    "from lib import add\n"
                    "\n"
                    "print(add(1, 2))\n")
    return workspace


@pytest.fixture
def cpp_workspace(workspace):
    """A small C++ project with a compile_commands.json for clangd."""
    source = workspace.write(
        "main.cpp",
        "int square(int value) { return value * value; }\n"
        "\n"
        "int main() {\n"
        "    int total = square(7);\n"
        "    return total;\n"
        "}\n")
    workspace.write(
        "compile_commands.json",
        '[{"directory": "%s", "command": "c++ -std=c++17 -c main.cpp", '
        '"file": "%s"}]' % (workspace.path, source))
    return workspace
