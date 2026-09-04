"""`diffcache.*`: the agent-side SQLite cache of computed diffs.

Entries are keyed on `(fromSha, toSha, file)` and are immutable by
construction, so there is no invalidation -- only mtime pruning. The
tests cover key isolation (the failure that would serve one file's
diff for another), value fidelity, and prune semantics.
"""
import uuid

import pytest

from scrutiny import INVALID_REQUEST


def make_key():
    return ("f" + uuid.uuid4().hex, "t" + uuid.uuid4().hex)


SAMPLE = {"fileExistsInTo": True, "isForcePush": False, "hunksJSON": "[]"}


@pytest.fixture
def cache(tmp_path):
    path = tmp_path / "diffcache"
    path.mkdir()
    return str(path)


def test_cold_lookup_is_a_miss(agent, cache):
    from_sha, to_sha = make_key()
    result = agent.call("diffcache.get", {"cacheDir": cache,
                                          "fromSha": from_sha,
                                          "toSha": to_sha, "file": "x.txt"})
    assert result["hit"] is False


def test_put_then_get(agent, cache):
    from_sha, to_sha = make_key()
    key = {"cacheDir": cache, "fromSha": from_sha, "toSha": to_sha,
           "file": "x.txt"}
    assert agent.call("diffcache.put", dict(key, value=SAMPLE))["ok"] is True
    result = agent.call("diffcache.get", key)
    assert result["hit"] is True
    assert result["value"] == SAMPLE


def test_value_round_trips_exactly(agent, cache):
    """Nested structure and types survive the SQLite round trip."""
    from_sha, to_sha = make_key()
    value = {"fileExistsInTo": False, "isForcePush": True,
             "hunksJSON": '[{"a":1,"b":[2,3],"c":null}]',
             "count": 42, "ratio": 0.5}
    key = {"cacheDir": cache, "fromSha": from_sha, "toSha": to_sha,
           "file": "nested.txt"}
    agent.call("diffcache.put", dict(key, value=value))
    assert agent.call("diffcache.get", key)["value"] == value


def test_keys_are_isolated_by_file(agent, cache):
    """Two files under the same sha pair must not collide."""
    from_sha, to_sha = make_key()
    base = {"cacheDir": cache, "fromSha": from_sha, "toSha": to_sha}
    agent.call("diffcache.put",
               dict(base, file="a.txt", value={"which": "a"}))
    agent.call("diffcache.put",
               dict(base, file="b.txt", value={"which": "b"}))
    assert agent.call("diffcache.get",
                      dict(base, file="a.txt"))["value"] == {"which": "a"}
    assert agent.call("diffcache.get",
                      dict(base, file="b.txt"))["value"] == {"which": "b"}


def test_keys_are_isolated_by_sha(agent, cache):
    from_sha, to_sha = make_key()
    other_from, other_to = make_key()
    agent.call("diffcache.put",
               {"cacheDir": cache, "fromSha": from_sha, "toSha": to_sha,
                "file": "same.txt", "value": {"which": "first"}})
    miss = agent.call("diffcache.get",
                      {"cacheDir": cache, "fromSha": other_from,
                       "toSha": other_to, "file": "same.txt"})
    assert miss["hit"] is False, "a different sha pair returned a hit"


def test_direction_matters(agent, cache):
    """(from, to) is ordered: the reverse pair is a different entry."""
    from_sha, to_sha = make_key()
    agent.call("diffcache.put",
               {"cacheDir": cache, "fromSha": from_sha, "toSha": to_sha,
                "file": "f.txt", "value": {"dir": "forward"}})
    reverse = agent.call("diffcache.get",
                         {"cacheDir": cache, "fromSha": to_sha,
                          "toSha": from_sha, "file": "f.txt"})
    assert reverse["hit"] is False


def test_overwrite_replaces_the_value(agent, cache):
    from_sha, to_sha = make_key()
    key = {"cacheDir": cache, "fromSha": from_sha, "toSha": to_sha,
           "file": "x.txt"}
    agent.call("diffcache.put", dict(key, value={"v": 1}))
    agent.call("diffcache.put", dict(key, value={"v": 2}))
    assert agent.call("diffcache.get", key)["value"] == {"v": 2}


def test_caches_are_isolated_by_directory(agent, tmp_path):
    first = tmp_path / "cache-a"
    second = tmp_path / "cache-b"
    first.mkdir()
    second.mkdir()
    from_sha, to_sha = make_key()
    agent.call("diffcache.put",
               {"cacheDir": str(first), "fromSha": from_sha, "toSha": to_sha,
                "file": "f.txt", "value": SAMPLE})
    other = agent.call("diffcache.get",
                       {"cacheDir": str(second), "fromSha": from_sha,
                        "toSha": to_sha, "file": "f.txt"})
    assert other["hit"] is False, "cache directories are not isolated"


def test_persists_across_agent_restarts(agent_binary, agent_factory, tmp_path):
    """The cache is on disk, so a new agent process still hits it."""
    cache = tmp_path / "durable"
    cache.mkdir()
    from_sha, to_sha = make_key()
    key = {"cacheDir": str(cache), "fromSha": from_sha, "toSha": to_sha,
           "file": "f.txt"}
    writer = agent_factory(["--allow-root", str(tmp_path)])
    writer.call("diffcache.put", dict(key, value=SAMPLE))
    writer.close()

    reader = agent_factory(["--allow-root", str(tmp_path)])
    result = reader.call("diffcache.get", key)
    assert result["hit"] is True and result["value"] == SAMPLE


def test_prune_removes_entries(agent, cache):
    from_sha, to_sha = make_key()
    key = {"cacheDir": cache, "fromSha": from_sha, "toSha": to_sha,
           "file": "f.txt"}
    agent.call("diffcache.put", dict(key, value=SAMPLE))
    pruned = agent.call("diffcache.prune", {"cacheDir": cache, "days": -1})
    assert pruned["removed"] >= 1
    assert agent.call("diffcache.get", key)["hit"] is False


def test_prune_keeps_fresh_entries(agent, cache):
    """A generous retention window must not evict what was just written."""
    from_sha, to_sha = make_key()
    key = {"cacheDir": cache, "fromSha": from_sha, "toSha": to_sha,
           "file": "f.txt"}
    agent.call("diffcache.put", dict(key, value=SAMPLE))
    agent.call("diffcache.prune", {"cacheDir": cache, "days": 3650})
    assert agent.call("diffcache.get", key)["hit"] is True, \
        "prune(3650 days) evicted an entry written seconds ago"


def test_prune_on_empty_cache(agent, tmp_path):
    empty = tmp_path / "empty-cache"
    empty.mkdir()
    result = agent.call("diffcache.prune", {"cacheDir": str(empty),
                                            "days": -1})
    assert result["removed"] == 0


def test_prune_reports_a_count(agent, cache):
    from_sha, to_sha = make_key()
    for n in range(5):
        agent.call("diffcache.put",
                   {"cacheDir": cache, "fromSha": from_sha, "toSha": to_sha,
                    "file": "f%d.txt" % n, "value": SAMPLE})
    result = agent.call("diffcache.prune", {"cacheDir": cache, "days": -1})
    assert result["removed"] >= 5


@pytest.mark.parametrize("method", ["diffcache.get", "diffcache.put",
                                    "diffcache.prune"])
def test_missing_params(agent, method):
    assert agent.call_expect_error(method, {}).code == INVALID_REQUEST


def test_large_value(agent, cache):
    """A cached value big enough to stream survives the round trip."""
    from_sha, to_sha = make_key()
    key = {"cacheDir": cache, "fromSha": from_sha, "toSha": to_sha,
           "file": "big.txt"}
    value = {"hunksJSON": "x" * 400000}
    agent.call("diffcache.put", dict(key, value=value), timeout=90)
    assert agent.call("diffcache.get", key, timeout=90)["value"] == value


def test_unicode_file_key(agent, cache):
    from_sha, to_sha = make_key()
    key = {"cacheDir": cache, "fromSha": from_sha, "toSha": to_sha,
           "file": "café/naïve—file.txt"}
    agent.call("diffcache.put", dict(key, value=SAMPLE))
    assert agent.call("diffcache.get", key)["hit"] is True


def test_concurrent_writes_do_not_corrupt(agent, cache):
    """Parallel puts into one SQLite file all land."""
    import threading
    from_sha, to_sha = make_key()
    errors = []

    def write(index):
        try:
            agent.call("diffcache.put",
                       {"cacheDir": cache, "fromSha": from_sha,
                        "toSha": to_sha, "file": "f%d.txt" % index,
                        "value": {"index": index}}, timeout=60)
        except Exception as exc:                        # noqa: BLE001
            errors.append(exc)

    threads = [threading.Thread(target=write, args=(n,)) for n in range(12)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(90)
    assert not errors, "concurrent diffcache writes failed: %r" % errors
    for n in range(12):
        result = agent.call("diffcache.get",
                            {"cacheDir": cache, "fromSha": from_sha,
                             "toSha": to_sha, "file": "f%d.txt" % n})
        assert result["hit"] is True and result["value"] == {"index": n}
