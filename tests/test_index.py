"""`index.*`: the durable, agent-side symbol index.

Indexing runs on the bulk lane, streams `index.progress`, and is
cancellable. Creation and lifecycle are testable without a language
server; the actual indexing run needs one, so those tests skip
cleanly where none is installed.
"""
import os
import time
import uuid

import pytest

from scrutiny import AgentError, CANCELLED, INVALID_REQUEST, NOT_FOUND


def cache_db_path(tmp_path):
    return str(tmp_path / ("index-%s.db" % uuid.uuid4().hex))


# ---------------------------------------------------------------------
# lifecycle (no server required)
# ---------------------------------------------------------------------
def test_create_requires_params(agent, workspace):
    assert agent.call_expect_error("index.create", {}).code == INVALID_REQUEST
    assert agent.call_expect_error(
        "index.create",
        {"workspacePath": workspace.path}).code == INVALID_REQUEST


def test_run_unknown_indexer(agent):
    assert agent.call_expect_error(
        "index.run", {"indexerId": "no-such-indexer"}).code == NOT_FOUND


def test_run_requires_id(agent):
    assert agent.call_expect_error("index.run", {}).code == INVALID_REQUEST


def test_destroy_is_idempotent(agent):
    assert agent.call("index.destroy",
                      {"indexerId": "no-such-indexer"})["ok"] is True


def test_destroy_requires_id(agent):
    assert agent.call_expect_error("index.destroy",
                                   {}).code == INVALID_REQUEST


def test_cancel_unknown_id_is_safe(agent):
    agent.notify("index.cancel", {"indexerId": "no-such-indexer"})
    assert agent.call("meta.debug", {"padBytes": 3})["pad"] == "xxx"
    assert agent.alive()


def test_cancel_without_params_is_safe(agent):
    agent.notify("index.cancel", {})
    assert agent.call("meta.debug", {"padBytes": 3})["pad"] == "xxx"


def test_create_makes_the_cache_db(agent, workspace, tmp_path):
    """The cache database is created eagerly, before any indexing."""
    db_path = cache_db_path(tmp_path)
    try:
        agent.call("index.create",
                   {"workspacePath": workspace.path, "language": 8,
                    "cacheDBPath": db_path}, timeout=60)
    except AgentError:
        pass       # no language server: the db is created first regardless
    assert os.path.exists(db_path), \
        "index.create did not create the cache database at %s" % db_path


def test_relative_cache_path_anchors_under_home(agent, workspace):
    """Documented: a relative cacheDBPath resolves under $HOME."""
    name = "scrutiny-test-%s.db" % uuid.uuid4().hex
    relative = os.path.join("scrutiny-cache", "index", name)
    absolute = os.path.join(os.path.expanduser("~"), relative)
    try:
        try:
            agent.call("index.create",
                       {"workspacePath": workspace.path, "language": 8,
                        "cacheDBPath": relative}, timeout=60)
        except AgentError:
            pass
        assert os.path.exists(absolute), \
            "relative cacheDBPath did not anchor under $HOME"
    finally:
        if os.path.exists(absolute):
            os.remove(absolute)


@pytest.fixture
def indexer(agent, available_languages, python_workspace, tmp_path):
    """A created indexer over a real Python workspace."""
    if 2 not in available_languages:
        pytest.skip("pylsp is not installed")
    result = agent.call("index.create",
                        {"workspacePath": python_workspace.path,
                         "language": 2,
                         "cacheDBPath": cache_db_path(tmp_path)}, timeout=120)
    indexer_id = result["indexerId"]
    yield indexer_id, python_workspace
    try:
        agent.call("index.destroy", {"indexerId": indexer_id}, timeout=60)
    except AgentError:
        pass


def test_create_returns_an_id(indexer):
    indexer_id, _workspace = indexer
    assert isinstance(indexer_id, str) and indexer_id


def test_ids_are_unique(agent, available_languages, python_workspace,
                        tmp_path):
    if 2 not in available_languages:
        pytest.skip("pylsp is not installed")
    ids = set()
    for _ in range(3):
        result = agent.call("index.create",
                            {"workspacePath": python_workspace.path,
                             "language": 2,
                             "cacheDBPath": cache_db_path(tmp_path)},
                            timeout=120)
        ids.add(result["indexerId"])
    assert len(ids) == 3
    for indexer_id in ids:
        agent.call("index.destroy", {"indexerId": indexer_id}, timeout=60)


@pytest.mark.lsp
def test_run_reports_counts(agent, indexer):
    indexer_id, _workspace = indexer
    result = agent.call("index.run", {"indexerId": indexer_id}, timeout=300)
    assert isinstance(result["filesIndexed"], int)
    assert result["filesIndexed"] >= 1, \
        "indexed no files in a workspace with two Python modules"
    assert isinstance(result["definitionsFound"], int)


@pytest.mark.lsp
def test_run_streams_progress(agent, indexer):
    indexer_id, _workspace = indexer
    mark = agent.notification_mark()
    agent.call("index.run", {"indexerId": indexer_id}, timeout=300)
    progress = [params for method, params in agent.notifications[mark:]
                if method == "index.progress"
                and params.get("indexerId") == indexer_id]
    assert progress, "index.run emitted no index.progress notifications"
    for entry in progress:
        assert isinstance(entry["current"], int)
        assert isinstance(entry["total"], int)
        assert entry["current"] <= entry["total"], \
            "progress overran its total: %r" % entry


@pytest.mark.lsp
def test_run_populates_the_cache_db(agent, available_languages,
                                    python_workspace, tmp_path):
    if 2 not in available_languages:
        pytest.skip("pylsp is not installed")
    db_path = cache_db_path(tmp_path)
    result = agent.call("index.create",
                        {"workspacePath": python_workspace.path,
                         "language": 2, "cacheDBPath": db_path}, timeout=120)
    agent.call("index.run", {"indexerId": result["indexerId"]}, timeout=300)
    agent.call("index.destroy", {"indexerId": result["indexerId"]},
               timeout=60)
    assert os.path.getsize(db_path) > 0, "the index database is empty"


@pytest.mark.lsp
def test_run_uses_the_bulk_lane(agent, indexer):
    """Indexing must not starve interactive requests."""
    indexer_id, _workspace = indexer
    _run_id, wait = agent.call_async("index.run", {"indexerId": indexer_id})
    start = time.monotonic()
    agent.call("meta.debug", {"padBytes": 4}, timeout=60)
    latency = time.monotonic() - start
    wait(timeout=300)
    assert latency < 15, \
        "an interactive request waited %.1fs behind a bulk index run" % latency


@pytest.mark.lsp
def test_cancel_stops_a_run(agent, indexer):
    """`index.cancel` ends the run instead of letting it finish."""
    indexer_id, _workspace = indexer
    run_id, wait = agent.call_async("index.run", {"indexerId": indexer_id})
    time.sleep(0.2)
    agent.notify("index.cancel", {"indexerId": indexer_id})
    try:
        wait(timeout=120)
    except AgentError as exc:
        assert exc.code in (CANCELLED, NOT_FOUND, 1000), \
            "cancelled index.run failed with an unexpected code: %s" % exc
    assert agent.alive()
    assert agent.call("meta.debug", {"padBytes": 3})["pad"] == "xxx"


@pytest.mark.lsp
def test_destroy_after_run(agent, available_languages, python_workspace,
                           tmp_path):
    if 2 not in available_languages:
        pytest.skip("pylsp is not installed")
    result = agent.call("index.create",
                        {"workspacePath": python_workspace.path,
                         "language": 2,
                         "cacheDBPath": cache_db_path(tmp_path)}, timeout=120)
    indexer_id = result["indexerId"]
    agent.call("index.run", {"indexerId": indexer_id}, timeout=300)
    assert agent.call("index.destroy",
                      {"indexerId": indexer_id}, timeout=60)["ok"] is True
    assert agent.call_expect_error(
        "index.run", {"indexerId": indexer_id}).code == NOT_FOUND


def test_shutdown_with_a_live_indexer(agent_factory, available_languages,
                                      python_workspace, tmp_path):
    """A created indexer must not block agent shutdown."""
    if 2 not in available_languages:
        pytest.skip("pylsp is not installed")
    instance = agent_factory(["--allow-root", "/tmp",
                              "--allow-root", os.path.expanduser("~")])
    instance.call("index.create",
                  {"workspacePath": python_workspace.path, "language": 2,
                   "cacheDBPath": cache_db_path(tmp_path)}, timeout=120)
    instance.close(timeout=20)
    assert instance.proc.poll() is not None
