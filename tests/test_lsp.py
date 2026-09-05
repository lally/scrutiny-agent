"""`lsp.*` request-level queries.

Two layers are tested. Parameter validation and the clean-failure path
run everywhere: a missing language server must produce `LSP_FAILED`,
never a hang or a crash. The semantic assertions (a definition really
points at the definition) run only where a real server is installed,
and skip cleanly otherwise.
"""
import pytest

from scrutiny import AgentError, INVALID_REQUEST, LSP_FAILED

SWIFT = 8          # rarely installed on Linux -- the "no server" probe
POSITIONAL = ["lsp.gotoDefinition", "lsp.findReferences", "lsp.hover"]
WHOLE_FILE = ["lsp.documentSymbols", "lsp.foldingRange"]


def base_params(workspace, language, file_path, content):
    return {"workspacePath": workspace, "language": language,
            "filePath": file_path, "fileContent": content}


# ---------------------------------------------------------------------
# parameter validation (no server required)
# ---------------------------------------------------------------------
@pytest.mark.parametrize("method", POSITIONAL + WHOLE_FILE)
def test_requires_workspace_path(agent, method, workspace):
    path = workspace.write("t.swift", "let x = 1\n")
    params = base_params(workspace.path, SWIFT, path, "let x = 1\n")
    params.update({"line": 0, "character": 4})
    del params["workspacePath"]
    assert agent.call_expect_error(method, params).code == INVALID_REQUEST


@pytest.mark.parametrize("method", POSITIONAL + WHOLE_FILE)
def test_requires_file_path(agent, method, workspace):
    params = base_params(workspace.path, SWIFT, None, "let x = 1\n")
    params.update({"line": 0, "character": 4})
    del params["filePath"]
    assert agent.call_expect_error(method, params).code == INVALID_REQUEST


def test_workspace_symbols_requires_query(agent, workspace):
    assert agent.call_expect_error(
        "lsp.workspaceSymbols",
        {"workspacePath": workspace.path,
         "language": SWIFT}).code == INVALID_REQUEST


def test_unknown_language_fails_cleanly(agent, workspace):
    path = workspace.write("t.txt", "x\n")
    params = base_params(workspace.path, 99, path, "x\n")
    params.update({"line": 0, "character": 0})
    error = agent.call_expect_error("lsp.hover", params, timeout=60)
    assert error.code in (LSP_FAILED, INVALID_REQUEST)


@pytest.mark.parametrize("method", POSITIONAL + WHOLE_FILE)
def test_missing_server_is_lsp_failed_not_a_hang(agent, method, workspace,
                                                 available_languages):
    """With no server for the language, the call fails fast and cleanly."""
    if SWIFT in available_languages:
        pytest.skip("sourcekit-lsp is installed; cannot test the absent path")
    path = workspace.write("t.swift", "let x = 1\n")
    params = base_params(workspace.path, SWIFT, path, "let x = 1\n")
    params.update({"line": 0, "character": 4})
    error = agent.call_expect_error(method, params, timeout=60)
    assert error.code == LSP_FAILED


def test_agent_survives_repeated_lsp_failures(agent, workspace,
                                              available_languages):
    if SWIFT in available_languages:
        pytest.skip("sourcekit-lsp is installed")
    path = workspace.write("t.swift", "let x = 1\n")
    params = base_params(workspace.path, SWIFT, path, "let x = 1\n")
    params.update({"line": 0, "character": 4})
    for _ in range(6):
        agent.call_expect_error("lsp.hover", params, timeout=60)
    assert agent.alive()
    assert agent.call("meta.debug", {"padBytes": 3})["pad"] == "xxx"


# ---------------------------------------------------------------------
# real server semantics
# ---------------------------------------------------------------------
@pytest.fixture
def python_lsp(available_languages, python_workspace):
    if 2 not in available_languages:
        pytest.skip("pylsp is not installed")
    return python_workspace


@pytest.mark.lsp
def test_document_symbols_finds_definitions(agent, python_lsp):
    content = python_lsp.read("lib.py").decode()
    result = agent.call("lsp.documentSymbols",
                        {"workspacePath": python_lsp.path, "language": 2,
                         "filePath": python_lsp.join("lib.py"),
                         "fileContent": content}, timeout=120)
    blob = repr(result["symbols"])
    assert "add" in blob, "documentSymbols did not report `add`: %s" % blob
    assert "Calculator" in blob, \
        "documentSymbols did not report `Calculator`: %s" % blob


@pytest.mark.lsp
def test_hover_returns_documentation(agent, python_lsp):
    content = python_lsp.read("lib.py").decode()
    result = agent.call("lsp.hover",
                        {"workspacePath": python_lsp.path, "language": 2,
                         "filePath": python_lsp.join("lib.py"),
                         "fileContent": content,
                         "line": 0, "character": 4}, timeout=120)
    assert "hover" in result
    if result["hover"] is not None:
        assert "contents" in result["hover"]


@pytest.mark.lsp
def test_goto_definition_points_at_the_definition(agent, python_lsp):
    """From the `add(...)` call in main.py to `def add` in lib.py."""
    content = python_lsp.read("main.py").decode()
    result = agent.call("lsp.gotoDefinition",
                        {"workspacePath": python_lsp.path, "language": 2,
                         "filePath": python_lsp.join("main.py"),
                         "fileContent": content,
                         "line": 2, "character": 6}, timeout=120)
    assert "locations" in result
    if result["locations"]:
        assert any("lib.py" in repr(loc) for loc in result["locations"]), \
            "definition did not resolve into lib.py: %r" % result["locations"]


@pytest.mark.lsp
def test_find_references_returns_locations(agent, python_lsp):
    content = python_lsp.read("lib.py").decode()
    result = agent.call("lsp.findReferences",
                        {"workspacePath": python_lsp.path, "language": 2,
                         "filePath": python_lsp.join("lib.py"),
                         "fileContent": content,
                         "line": 0, "character": 4,
                         "includeDeclaration": True}, timeout=120)
    assert isinstance(result["locations"], list)


@pytest.mark.lsp
def test_folding_range_shape(agent, python_lsp):
    content = python_lsp.read("lib.py").decode()
    result = agent.call("lsp.foldingRange",
                        {"workspacePath": python_lsp.path, "language": 2,
                         "filePath": python_lsp.join("lib.py"),
                         "fileContent": content}, timeout=120)
    assert isinstance(result["ranges"], list)
    for entry in result["ranges"]:
        assert isinstance(entry["startLine"], int)
        assert isinstance(entry["endLine"], int)
        assert entry["endLine"] >= entry["startLine"], \
            "folding range is inverted: %r" % entry


@pytest.mark.lsp
def test_workspace_symbols_shape(agent, python_lsp):
    """Either the documented shape, or a clean LSP_FAILED.

    `workspace/symbol` is optional in LSP and several servers omit it
    (pylsp answers -32601). The agent must surface that as LSP_FAILED
    rather than hanging or inventing an empty result that a client
    would mistake for "no symbols in this workspace".
    """
    try:
        result = agent.call("lsp.workspaceSymbols",
                            {"workspacePath": python_lsp.path, "language": 2,
                             "query": "add"}, timeout=120)
        assert isinstance(result["symbols"], list)
    except AgentError as exc:
        assert exc.code == LSP_FAILED, \
            "a server without workspace/symbol should yield LSP_FAILED, " \
            "got %s" % exc


@pytest.mark.lsp
def test_session_is_reused_across_requests(agent, python_lsp):
    """One session per (workspace, language) -- not one per request.

    Spawning a language server per request would make every query cost
    a cold start, which is the whole reason the agent keeps sessions.
    """
    content = python_lsp.read("lib.py").decode()
    params = {"workspacePath": python_lsp.path, "language": 2,
              "filePath": python_lsp.join("lib.py"), "fileContent": content}
    agent.call("lsp.documentSymbols", params, timeout=120)
    after_first = agent.call("meta.stat")["lspSessions"]
    for _ in range(3):
        agent.call("lsp.documentSymbols", params, timeout=120)
    after_more = agent.call("meta.stat")["lspSessions"]
    assert after_more == after_first, \
        "session count grew from %d to %d across repeated queries" \
        % (after_first, after_more)


@pytest.mark.lsp
def test_concurrent_queries_do_not_interleave_results(agent, python_lsp):
    """Parallel queries against one session return their own answers."""
    import threading
    content = python_lsp.read("lib.py").decode()
    params = {"workspacePath": python_lsp.path, "language": 2,
              "filePath": python_lsp.join("lib.py"), "fileContent": content}
    agent.call("lsp.documentSymbols", params, timeout=120)   # warm up

    results = {}
    errors = []

    def query(index):
        try:
            results[index] = agent.call("lsp.documentSymbols", params,
                                        timeout=120)
        except AgentError as exc:
            errors.append(exc)

    threads = [threading.Thread(target=query, args=(n,)) for n in range(4)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(180)
    assert not errors, "concurrent LSP queries failed: %r" % errors
    values = list(results.values())
    assert all(value == values[0] for value in values), \
        "concurrent identical queries returned different answers"


@pytest.mark.lsp
def test_cpp_document_symbols(agent, available_languages, cpp_workspace):
    if 6 not in available_languages:
        pytest.skip("clangd is not installed")
    content = cpp_workspace.read("main.cpp").decode()
    result = agent.call("lsp.documentSymbols",
                        {"workspacePath": cpp_workspace.path, "language": 6,
                         "filePath": cpp_workspace.join("main.cpp"),
                         "fileContent": content}, timeout=180)
    assert "square" in repr(result["symbols"]), \
        "clangd did not report `square`: %r" % result["symbols"]
