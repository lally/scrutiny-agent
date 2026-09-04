"""The error model (docs/protocol.md).

Codes are a stable contract: the Scrutiny UI distinguishes
`PERMISSION_DENIED` from `NOT_FOUND` to decide what to tell the user,
and the Emacs client keys retry behavior off `LSP_FAILED`. These tests
pin each code to the condition that must produce it.
"""
import pytest

from scrutiny import (GIT_FAILED, INVALID_REQUEST, NOT_FOUND,
                      PERMISSION_DENIED)


def test_unknown_method(agent):
    error = agent.call_expect_error("no.such.method")
    assert error.code == NOT_FOUND
    assert "unknown method" in str(error), \
        "message should name the failure: %s" % error


def test_unknown_method_names_the_method(agent):
    error = agent.call_expect_error("git.notARealOperation")
    assert "git.notARealOperation" in str(error)


def test_unknown_method_in_known_namespace(agent):
    """A typo'd op in a real namespace is still NOT_FOUND, not INTERNAL."""
    assert agent.call_expect_error("fs.readFyle",
                                   {"path": "/tmp"}).code == NOT_FOUND


def test_empty_method_name(agent):
    error = agent.call_expect_error("")
    assert error.code in (NOT_FOUND, INVALID_REQUEST)


@pytest.mark.parametrize("method,params", [
    ("git.headSha", {}),
    ("git.repoMetadata", {}),
    ("git.branches", {}),
    ("git.commits", {}),
    ("git.showFile", {}),
    ("git.diff", {}),
    ("git.isAncestor", {}),
    ("fs.readFile", {}),
    ("fs.listDirectory", {}),
    ("index.create", {}),
    ("index.run", {}),
    ("index.destroy", {}),
    ("watch.head", {}),
    ("diffcache.get", {}),
    ("diffcache.put", {}),
    ("lsp.tunnelOpen", {}),
    ("lsp.tunnelClose", {}),
])
def test_missing_required_params(agent, method, params):
    """Every method validates its required params before doing work."""
    assert agent.call_expect_error(method, params).code == INVALID_REQUEST


@pytest.mark.parametrize("bad_path", [123, True, None, [], {}])
def test_mistyped_path_param(agent, bad_path):
    """A path of the wrong JSON type is INVALID_REQUEST, not a crash."""
    error = agent.call_expect_error("git.headSha", {"path": bad_path})
    assert error.code == INVALID_REQUEST, \
        "path=%r should be INVALID_REQUEST, got %s" % (bad_path, error)


def test_null_params_object(agent):
    """`params: null` is handled like an empty object, not dereferenced."""
    agent.send_frame(b'{"jsonrpc":"2.0","id":123456,"method":"meta.stat",'
                     b'"params":null}')
    assert agent.call("meta.debug", {"padBytes": 2})["pad"] == "xx"


def test_missing_params_key(agent):
    """A request with no `params` at all does not crash the dispatcher."""
    agent.send_frame(b'{"jsonrpc":"2.0","id":123457,"method":"meta.stat"}')
    assert agent.call("meta.debug", {"padBytes": 2})["pad"] == "xx"


def test_not_found_for_missing_repo(agent):
    assert agent.call_expect_error(
        "git.headSha", {"path": "/definitely/not/a/repo"}).code == NOT_FOUND


def test_not_found_for_missing_file(agent):
    assert agent.call_expect_error(
        "fs.readFile", {"path": "/definitely/not/a/file"}).code == NOT_FOUND


def test_not_found_for_missing_directory(agent):
    assert agent.call_expect_error(
        "fs.listDirectory", {"path": "/definitely/not/a/dir"}).code == NOT_FOUND


def test_directory_passed_to_read_file(agent, tree):
    """Reading a directory fails cleanly rather than returning garbage."""
    error = agent.call_expect_error("fs.readFile", {"path": tree.path})
    assert error.code in (NOT_FOUND, INVALID_REQUEST, PERMISSION_DENIED, 1000)


def test_file_passed_to_list_directory(agent, tree):
    error = agent.call_expect_error("fs.listDirectory",
                                    {"path": tree.join("gamma.txt")})
    assert error.code in (NOT_FOUND, INVALID_REQUEST, 1000)


def test_git_failed_for_network_operation(agent, tmp_path):
    """A failing git subprocess surfaces as GIT_FAILED with its stderr."""
    install = tmp_path / "install"
    install.mkdir()
    error = agent.call_expect_error(
        "git.clone", {"fullName": "test/nope",
                      "cloneURL": "/no/such/upstream/repo",
                      "installDir": str(install), "authOpId": "op-x"},
        timeout=90)
    assert error.code == GIT_FAILED
    assert str(error).strip(), "GIT_FAILED should carry git's reason"


def test_error_response_shape(agent):
    """Errors are JSON-RPC error objects with an int code and a message."""
    error = agent.call_expect_error("fs.readFile", {})
    assert isinstance(error.code, int)
    assert isinstance(str(error), str) and str(error)


def test_agent_survives_a_burst_of_errors(agent):
    """Many failing calls must not degrade the connection."""
    for _ in range(50):
        agent.call_expect_error("no.such.method")
    assert agent.call("meta.debug", {"padBytes": 3})["pad"] == "xxx"
    assert agent.alive()
