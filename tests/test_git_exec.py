"""`git.exec`: the allowlisted general git surface.

This is the widest method in the protocol -- it exists so a real git UI
(magit) can drive the remote over the one multiplexed connection -- so
most of these tests are about what it must *refuse*. Whitelisting
subcommands alone is not security: git has several options that turn
any subcommand into an arbitrary command runner, and those are the
cases that matter.
"""
import os

import pytest

from scrutiny import INVALID_REQUEST, PERMISSION_DENIED
from scrutiny.fixtures import git


@pytest.fixture
def readonly_agent(agent_factory, tmp_path):
    return agent_factory(["--allow-root", str(tmp_path), "--allow-root", "/tmp",
                          "--git-exec-preset", "read-only"])


@pytest.fixture
def magit_agent(agent_factory, tmp_path):
    return agent_factory(["--allow-root", str(tmp_path), "--allow-root", "/tmp",
                          "--git-exec-preset", "magit"])


# ---------------------------------------------------------------------
# off by default
# ---------------------------------------------------------------------
def test_disabled_by_default(agent, repo):
    """An agent started without the flag must not run anything."""
    error = agent.call_expect_error("git.exec",
                                    {"repoPath": repo.path,
                                     "args": ["status"]})
    assert error.code == PERMISSION_DENIED
    assert "disabled" in str(error), \
        "the refusal should say how to enable it: %s" % error


def test_not_advertised_when_disabled(agent):
    """`capabilities` is the exhaustive list of what is actually served."""
    assert "git.exec" not in agent.capabilities


def test_advertised_when_enabled(readonly_agent):
    assert "git.exec" in readonly_agent.capabilities


# ---------------------------------------------------------------------
# the happy path
# ---------------------------------------------------------------------
def test_status(readonly_agent, repo):
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path,
                                  "args": ["status", "--porcelain", "-b"]})
    assert result["exitCode"] == 0
    assert result["stdout"].startswith("## work")
    assert result["truncated"] is False


def test_log_matches_the_cli(readonly_agent, repo):
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path,
                                  "args": ["log", "--oneline"]})
    expected = git(repo.path, "log", "--oneline", capture=True)
    assert result["stdout"].strip() == expected


def test_stdout_and_stderr_are_separate(readonly_agent, repo):
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path,
                                  "args": ["rev-parse", "no-such-ref"]})
    assert result["exitCode"] != 0
    assert result["stdout"].strip() in ("", "no-such-ref")
    assert result["stderr"], "git's diagnostic should reach stderr"


def test_nonzero_exit_is_data_not_an_error(readonly_agent, repo):
    """git answers questions with exit codes; the caller needs the code.

    `diff --quiet` exits 1 for "there are differences". Turning that
    into an RPC error would make the answer unreachable.
    """
    repo.dirty()
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path,
                                  "args": ["diff", "--quiet"]})
    assert result["exitCode"] == 1


def test_pathspec_arguments(readonly_agent, repo):
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path,
                                  "args": ["log", "--oneline", "--",
                                           "file.txt"]})
    assert result["exitCode"] == 0
    assert result["stdout"].strip()


def test_arguments_are_not_shell_interpreted(readonly_agent, repo):
    """Arguments go to execve, not a shell: no word splitting or globbing."""
    marker = str(repo.join("pwned"))
    result = readonly_agent.call(
        "git.exec",
        {"repoPath": repo.path,
         "args": ["log", "-1", "--format=%%s $(touch %s) `touch %s`"
                  % (marker, marker)]})
    assert result["exitCode"] == 0
    # git expands its own %-placeholders and leaves the rest literal --
    # which is exactly the evidence that nothing else interpreted it.
    assert "$(touch" in result["stdout"]
    assert not os.path.exists(marker), \
        "an argument reached a shell -- git.exec must exec directly"


def test_unicode_output(readonly_agent, repo):
    git(repo.path, "commit", "-q", "--allow-empty", "-m", "café \U0001F600")
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path,
                                  "args": ["log", "-1", "--format=%s"]})
    assert "café" in result["stdout"]


def test_large_output_streams(readonly_agent, repo):
    """Output over the frame cap comes back through rpc.chunk intact."""
    body = "".join("line %05d\n" % n for n in range(30000))
    repo.commit("big", {"big.txt": body})
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path,
                                  "args": ["show", "HEAD:big.txt"]},
                                 timeout=120)
    assert result["exitCode"] == 0
    assert result["stdout"] == body


def test_no_pager_hang(readonly_agent, repo):
    """git must never page: a pager on a pipe would hang the request."""
    for _ in range(60):
        repo.commit("filler", {"file.txt": "x%d\n" % _})
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path,
                                  "args": ["log"]}, timeout=60)
    assert result["exitCode"] == 0


def test_runs_in_the_named_repository(readonly_agent, repo_factory):
    """repoPath selects the repo -- output must come from that one."""
    first = repo_factory()
    first_head, _ = first.build_default()
    second = repo_factory()
    second_head, _ = second.build_default()
    for target, expected in ((first, first_head), (second, second_head)):
        result = readonly_agent.call("git.exec",
                                     {"repoPath": target.path,
                                      "args": ["rev-parse", "HEAD"]})
        assert result["stdout"].strip() == expected


# ---------------------------------------------------------------------
# the allowlist
# ---------------------------------------------------------------------
def test_subcommand_outside_the_preset_is_refused(readonly_agent, repo):
    error = readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path, "args": ["commit", "-m", "x"]})
    assert error.code == PERMISSION_DENIED
    assert "allowlist" in str(error)


def test_explicit_single_subcommand(agent_factory, tmp_path, repo):
    """--git-exec grants exactly one subcommand and nothing else."""
    instance = agent_factory(["--allow-root", str(tmp_path),
                              "--allow-root", "/tmp",
                              "--git-exec", "status"])
    assert instance.call("git.exec",
                         {"repoPath": repo.path,
                          "args": ["status", "--porcelain"]})["exitCode"] == 0
    assert instance.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["log"]}).code == PERMISSION_DENIED


def test_repeated_flags_accumulate(agent_factory, tmp_path, repo):
    instance = agent_factory(["--allow-root", str(tmp_path),
                              "--allow-root", "/tmp",
                              "--git-exec", "status",
                              "--git-exec", "log"])
    for subcommand in ("status", "log"):
        assert instance.call("git.exec",
                             {"repoPath": repo.path,
                              "args": [subcommand]})["exitCode"] == 0
    assert instance.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["show"]}).code == PERMISSION_DENIED


def test_magit_preset_allows_mutation(magit_agent, repo):
    repo.write("added.txt", "new\n")
    assert magit_agent.call("git.exec",
                            {"repoPath": repo.path,
                             "args": ["add", "added.txt"]})["exitCode"] == 0
    staged = magit_agent.call("git.exec",
                              {"repoPath": repo.path,
                               "args": ["diff", "--cached", "--name-only"]})
    assert "added.txt" in staged["stdout"]


def test_magit_preset_includes_read_only(magit_agent, repo):
    assert magit_agent.call("git.exec",
                            {"repoPath": repo.path,
                             "args": ["log", "--oneline"]})["exitCode"] == 0


def test_unknown_preset_leaves_it_disabled(agent_factory, tmp_path, repo):
    instance = agent_factory(["--allow-root", str(tmp_path),
                              "--git-exec-preset", "nonsense"])
    assert "git.exec" not in instance.capabilities


# ---------------------------------------------------------------------
# argument injection -- the cases that actually matter
# ---------------------------------------------------------------------
@pytest.mark.parametrize("args,what", [
    (["-c", "core.pager=sh -c id", "status"], "-c core.pager"),
    (["-c", "alias.x=!id", "status"], "-c alias"),
    (["-c", "credential.helper=!id", "status"], "-c credential.helper"),
    (["-c", "core.sshCommand=id", "status"], "-c core.sshCommand"),
    (["--config-env=core.pager=EVIL", "status"], "--config-env"),
    (["--exec-path=/tmp/evil", "status"], "--exec-path"),
    (["--exec-path", "/tmp/evil", "status"], "--exec-path (separate)"),
])
def test_config_and_exec_path_injection_is_refused(readonly_agent, repo,
                                                   args, what):
    """These turn any allowed subcommand into arbitrary code execution.

    A subcommand-only allowlist would let every one of them through,
    which is why the argument scan exists.
    """
    error = readonly_agent.call_expect_error("git.exec",
                                             {"repoPath": repo.path,
                                              "args": args})
    assert error.code == PERMISSION_DENIED, "%s was not refused" % what


@pytest.mark.parametrize("args,what", [
    (["fetch", "--upload-pack=id"], "--upload-pack"),
    (["fetch", "--receive-pack=id"], "--receive-pack"),
    (["ls-remote", "--upload-pack", "id", "origin"], "--upload-pack separate"),
])
def test_remote_command_injection_is_refused(magit_agent, repo, args, what):
    """`--upload-pack`/`--receive-pack` name a program git will run."""
    error = magit_agent.call_expect_error("git.exec",
                                          {"repoPath": repo.path,
                                           "args": args})
    assert error.code == PERMISSION_DENIED, "%s was not refused" % what


@pytest.mark.parametrize("args,what", [
    (["--git-dir=/etc", "status"], "--git-dir"),
    (["--git-dir", "/etc", "status"], "--git-dir separate"),
    (["--work-tree=/", "status"], "--work-tree"),
    (["-C", "/etc", "status"], "-C"),
    (["--namespace=x", "status"], "--namespace"),
])
def test_repository_relocation_is_refused(readonly_agent, repo, args, what):
    """repoPath must decide which repository runs, not a hidden option."""
    error = readonly_agent.call_expect_error("git.exec",
                                             {"repoPath": repo.path,
                                              "args": args})
    assert error.code == PERMISSION_DENIED, "%s was not refused" % what


def test_unknown_global_option_is_refused(readonly_agent, repo):
    """Unrecognized leading options are refused, not passed through.

    An allowlist in this position is what stops a client reaching a
    dangerous option under a spelling the deny list did not predict.
    """
    assert readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["--some-new-global", "status"]}).code == \
        PERMISSION_DENIED


def test_safe_global_options_are_allowed(readonly_agent, repo):
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path,
                                  "args": ["--no-optional-locks", "status",
                                           "--porcelain"]})
    assert result["exitCode"] == 0


def test_config_cannot_be_injected_after_the_subcommand(readonly_agent, repo,
                                                        tmp_path):
    """`-c` sets configuration only BEFORE the subcommand. After it, it
    is the subcommand's own option (`log -c`: combined diffs), so the
    would-be setting is just an argument git does not understand --
    and the program it names is never run."""
    marker = tmp_path / "ran"
    result = readonly_agent.call(
        "git.exec", {"repoPath": repo.path,
                     "args": ["--paginate", "log", "-c",
                              "core.pager=touch %s" % marker]})
    assert result["exitCode"] != 0
    assert not marker.exists()


@pytest.mark.parametrize("args", [
    ["log", "-c", "-1"],                 # combined diff format
    ["diff", "-C", "HEAD"],              # copy detection
    ["grep", "-c", "e"],                 # count matches
    ["blame", "-C", "file.txt"],
])
def test_subcommand_options_spelled_like_globals_are_served(readonly_agent,
                                                            repo, args):
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path, "args": args})
    assert result["exitCode"] in (0, 1), result["stderr"]


def test_gpg_sign_program_is_refused(magit_agent, repo):
    assert magit_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["commit", "--gpg-sign=evil", "-m", "x"]}).code \
        == PERMISSION_DENIED


def test_output_redirection_is_refused(magit_agent, repo):
    """`--output=` writes wherever the client points, outside the sandbox."""
    assert magit_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["format-patch", "--output=/tmp/x", "-1"]}).code \
        == PERMISSION_DENIED


# ---------------------------------------------------------------------
# parameter validation
# ---------------------------------------------------------------------
def test_requires_repo_path(readonly_agent):
    assert readonly_agent.call_expect_error(
        "git.exec", {"args": ["status"]}).code == INVALID_REQUEST


def test_requires_args(readonly_agent, repo):
    assert readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path}).code == INVALID_REQUEST


def test_args_must_be_strings(readonly_agent, repo):
    assert readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["status", 42]}).code == INVALID_REQUEST


def test_empty_args_is_refused(readonly_agent, repo):
    error = readonly_agent.call_expect_error("git.exec",
                                             {"repoPath": repo.path,
                                              "args": []})
    assert error.code == PERMISSION_DENIED
    assert "subcommand" in str(error)


def test_only_options_no_subcommand(readonly_agent, repo):
    assert readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["--no-pager"]}).code == PERMISSION_DENIED


def test_missing_repository(readonly_agent, tmp_path):
    """A path that is not a repository fails through git, cleanly."""
    plain = tmp_path / "not-a-repo"
    plain.mkdir()
    result = readonly_agent.call("git.exec",
                                 {"repoPath": str(plain),
                                  "args": ["status"]})
    assert result["exitCode"] != 0
    assert result["stderr"]


# ---------------------------------------------------------------------
# concurrency and health
# ---------------------------------------------------------------------
def test_concurrent_execs(readonly_agent, repo):
    import threading
    results = {}
    errors = []

    def run(index):
        try:
            results[index] = readonly_agent.call(
                "git.exec", {"repoPath": repo.path,
                             "args": ["rev-parse", "HEAD"]}, timeout=60)
        except Exception as exc:                        # noqa: BLE001
            errors.append(exc)

    threads = [threading.Thread(target=run, args=(n,)) for n in range(8)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(90)
    assert not errors, "concurrent git.exec failed: %r" % errors
    assert {r["stdout"].strip() for r in results.values()} == {repo.head_sha}


def test_agent_survives_many_execs(readonly_agent, repo):
    for _ in range(40):
        readonly_agent.call("git.exec", {"repoPath": repo.path,
                                         "args": ["rev-parse", "HEAD"]})
    assert readonly_agent.alive()


def test_refusals_do_not_run_anything(readonly_agent, repo):
    """A refused request must not have started a process at all."""
    marker = str(repo.join("should-not-exist"))
    readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["-c", "core.pager=touch %s" % marker, "log"]})
    assert not os.path.exists(marker)


# ---------------------------------------------------------------------
# `-c` config settings: allowlisted by key
# ---------------------------------------------------------------------
# Exactly what magit puts before every subcommand it runs. If this set
# is ever refused, the magit integration stops working entirely.
MAGIT_GLOBALS = ["--no-pager", "--literal-pathspecs",
                 "-c", "core.preloadIndex=true",
                 "-c", "log.showSignature=false",
                 "-c", "color.ui=false",
                 "-c", "color.diff=false",
                 "-c", "diff.noPrefix=false"]


def test_magit_global_arguments_are_accepted(magit_agent, repo):
    result = magit_agent.call("git.exec",
                              {"repoPath": repo.path,
                               "args": MAGIT_GLOBALS + ["status",
                                                        "--porcelain", "-b"]})
    assert result["exitCode"] == 0
    assert result["stdout"].startswith("## work")


def test_config_keys_are_case_insensitive(readonly_agent, repo):
    """git config keys are case-insensitive; the check must be too."""
    for spelling in ("core.preloadIndex=true", "CORE.PRELOADINDEX=true",
                     "core.preloadindex=true"):
        result = readonly_agent.call("git.exec",
                                     {"repoPath": repo.path,
                                      "args": ["-c", spelling, "status",
                                               "--porcelain"]})
        assert result["exitCode"] == 0, "%s was refused" % spelling


def test_whole_section_allowlist(readonly_agent, repo):
    """A bare section in the allowlist covers its keys (color.*)."""
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path,
                                  "args": ["-c", "color.status=false",
                                           "status", "--porcelain"]})
    assert result["exitCode"] == 0


@pytest.mark.parametrize("setting", [
    "core.pager=sh -c id",
    "core.editor=id",
    "core.sshCommand=id",
    "core.gitProxy=id",
    "alias.x=!id",
    "credential.helper=!id",
    "diff.external=id",
    "diff.mine.textconv=id",
    "filter.f.clean=id",
    "http.proxy=http://evil",
    "uploadpack.packObjectsHook=id",
    "protocol.ext.allow=always",
    "sequence.editor=id",
])
def test_execution_capable_config_keys_are_refused(readonly_agent, repo,
                                                   setting):
    """Only formatting/local-behavior keys are permitted.

    Every setting here can make git run a program of the caller's
    choosing, which is exactly what the subcommand allowlist alone
    would not prevent.
    """
    error = readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["-c", setting, "status"]})
    assert error.code == PERMISSION_DENIED, "%s was allowed" % setting


def test_config_after_the_subcommand_is_not_configuration(readonly_agent, repo):
    """`-c` is meaningful only before the subcommand. After it, git reads
    the would-be setting as an ordinary argument and says so; nothing
    was configured."""
    result = readonly_agent.call(
        "git.exec", {"repoPath": repo.path,
                     "args": ["log", "-c", "color.ui=false"]})
    assert result["exitCode"] != 0
    assert "color.ui=false" in result["stderr"]


def test_dangling_config_flag(readonly_agent, repo):
    error = readonly_agent.call_expect_error("git.exec",
                                             {"repoPath": repo.path,
                                              "args": ["-c"]})
    assert error.code == PERMISSION_DENIED


def test_config_without_a_key(readonly_agent, repo):
    assert readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["-c", "=value", "status"]}).code == \
        PERMISSION_DENIED


def test_operator_can_extend_the_config_allowlist(agent_factory, tmp_path,
                                                  repo):
    """--git-exec-config adds a key the built-in set does not carry."""
    # The root is the repository itself, not /tmp: pytest's tmp tree is
    # not under /tmp everywhere (macOS: /private/var/folders/...), and
    # a repository outside the roots is refused for that reason alone,
    # which would make the baseline assertion below pass vacuously.
    baseline = agent_factory(["--allow-root", repo.path,
                              "--git-exec-preset", "read-only"])
    assert baseline.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["-c", "push.default=simple", "status"]}).code \
        == PERMISSION_DENIED

    extended = agent_factory(["--allow-root", repo.path,
                              "--git-exec-preset", "read-only",
                              "--git-exec-config", "push.default"])
    assert extended.call("git.exec",
                         {"repoPath": repo.path,
                          "args": ["-c", "push.default=simple", "status",
                                   "--porcelain"]})["exitCode"] == 0


def test_extending_the_allowlist_does_not_widen_the_rest(agent_factory, repo):
    extended = agent_factory(["--allow-root", repo.path,
                              "--git-exec-preset", "read-only",
                              "--git-exec-config", "push.default"])
    assert extended.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["-c", "core.pager=id", "status"]}).code == \
        PERMISSION_DENIED


# ---------------------------------------------------------------------
# what a git UI asks on every refresh
# ---------------------------------------------------------------------
# Each of these used to be refused, which did not stop magit -- the
# client fell back to its slow path -- it only made every refresh pay
# for a refusal plus a fallback, several times over.
def test_rev_parse_git_dir_is_a_query(readonly_agent, repo):
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path,
                                  "args": ["rev-parse", "--git-dir"]})
    assert result["exitCode"] == 0
    assert result["stdout"].strip() == ".git"


def test_git_dir_as_a_global_option_is_still_refused(readonly_agent, repo):
    for args in (["--git-dir", "/tmp", "status"],
                 ["--git-dir=/tmp", "status"],
                 ["rev-parse", "--git-dir=/tmp"],
                 ["status", "--git-dir"]):
        assert readonly_agent.call_expect_error(
            "git.exec", {"repoPath": repo.path, "args": args}).code == \
            PERMISSION_DENIED, args


@pytest.mark.parametrize("args", [
    ["config", "--list", "-z"],
    ["config", "--local", "-z", "--get-all", "--include",
     "status.showUntrackedFiles"],
    ["config", "--get", "user.name"],
    ["config", "--get-regexp", "^remote\\."],
])
def test_read_only_preset_answers_config_queries(readonly_agent, repo, args):
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path, "args": args})
    # 1 is git's "no such key", which is an answer.
    assert result["exitCode"] in (0, 1)


@pytest.mark.parametrize("args", [
    ["config", "user.name", "mallory"],            # bare set
    ["config", "user.name"],                       # ambiguous by arity
    ["config", "--unset", "user.name"],
    ["config", "--add", "alias.x", "!id"],
    ["config", "--get", "--file", "/etc/passwd", "a.b"],
    ["config", "--list", "--blob", "HEAD:x"],
    ["config", "--edit"],
    ["config", "--replace-all", "core.pager", "id"],
])
def test_read_only_preset_refuses_config_writes(readonly_agent, repo, args):
    assert readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path, "args": args}).code == \
        PERMISSION_DENIED
    listed = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path,
                                  "args": ["config", "--list"]})["stdout"]
    assert "mallory" not in listed and "alias.x" not in listed


def test_magit_preset_can_write_config(magit_agent, repo):
    assert magit_agent.call(
        "git.exec", {"repoPath": repo.path,
                     "args": ["config", "--local", "branch.work.description",
                              "x"]})["exitCode"] == 0
    assert magit_agent.call(
        "git.exec", {"repoPath": repo.path,
                     "args": ["config", "--get",
                              "branch.work.description"]})["stdout"].strip() \
        == "x"


def test_config_is_not_served_without_a_preset(agent_factory, tmp_path, repo):
    """An explicit allowlist means exactly what it lists."""
    agent = agent_factory(["--allow-root", str(tmp_path), "--allow-root",
                           "/tmp", "--git-exec", "status"])
    assert agent.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["config", "--list"]}).code == PERMISSION_DENIED


# ---------------------------------------------------------------------
# stdin
# ---------------------------------------------------------------------
import base64


@pytest.fixture
def stdin_agent(agent_factory, tmp_path):
    return agent_factory(["--allow-root", str(tmp_path), "--allow-root", "/tmp",
                          "--git-exec-preset", "magit",
                          "--git-exec", "hash-object"])


def test_a_command_that_reads_stdin_does_not_eat_the_channel(stdin_agent, repo):
    """The agent's own stdin IS the protocol. A child that inherited it
    and read from it would swallow the client's next request; here the
    child must see end-of-file at once, and the session must go on."""
    result = stdin_agent.call("git.exec",
                              {"repoPath": repo.path,
                               "args": ["hash-object", "--stdin"]},
                              timeout=10)
    assert result["exitCode"] == 0
    # The hash of the empty blob: it read nothing, not our frames.
    assert result["stdout"].strip() == \
        "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391"
    assert stdin_agent.call("git.exec",
                            {"repoPath": repo.path,
                             "args": ["rev-parse", "HEAD"]})["exitCode"] == 0


def test_stdin_is_delivered(stdin_agent, repo):
    data = b"hello\n"
    result = stdin_agent.call(
        "git.exec", {"repoPath": repo.path,
                     "args": ["hash-object", "--stdin"],
                     "stdinBase64": base64.b64encode(data).decode()})
    assert result["stdout"].strip() == \
        "ce013625030ba8dba906f756967f9e9ca394464a"


def test_large_stdin_does_not_deadlock(stdin_agent, repo):
    """More than a pipe holds, to a command that also writes."""
    data = os.urandom(3 * 1024 * 1024)
    result = stdin_agent.call(
        "git.exec", {"repoPath": repo.path,
                     "args": ["hash-object", "--stdin"],
                     "stdinBase64": base64.b64encode(data).decode()},
        timeout=60)
    assert result["exitCode"] == 0 and len(result["stdout"].strip()) == 40


def test_staging_a_hunk_through_stdin(stdin_agent, repo):
    """What the parameter is for: `git apply --cached` reading a patch."""
    target = os.path.join(repo.path, "staged-by-patch.txt")
    with open(target, "w") as handle:
        handle.write("new file\n")
    git(repo.path, "add", "-N", "staged-by-patch.txt")
    patch = stdin_agent.call("git.exec",
                             {"repoPath": repo.path,
                              "args": ["diff", "--", "staged-by-patch.txt"]}
                             )["stdout"]
    assert "+new file" in patch
    applied = stdin_agent.call(
        "git.exec", {"repoPath": repo.path,
                     "args": ["apply", "--cached", "-"],
                     "stdinBase64": base64.b64encode(patch.encode()).decode()})
    assert applied["exitCode"] == 0, applied["stderr"]
    staged = stdin_agent.call("git.exec",
                              {"repoPath": repo.path,
                               "args": ["diff", "--cached", "--name-only"]}
                              )["stdout"]
    assert "staged-by-patch.txt" in staged


def test_invalid_stdin_is_refused(stdin_agent, repo):
    assert stdin_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path, "args": ["hash-object", "--stdin"],
                     "stdinBase64": "!!! not base64 !!!"}).code == \
        INVALID_REQUEST


# ---------------------------------------------------------------------
# environment
# ---------------------------------------------------------------------
def test_index_file_redirects_the_index(stdin_agent, repo):
    """The case that matters: a UI staging into a SCRATCH index must not
    find its commands applied to the real one."""
    with open(os.path.join(repo.path, "scratch.txt"), "w") as handle:
        handle.write("x\n")
    scratch = os.path.join(repo.path, ".git", "index.scratch")
    for args in (["read-tree", "HEAD"],
                 ["update-index", "--add", "--", "scratch.txt"]):
        result = stdin_agent.call("git.exec",
                                  {"repoPath": repo.path, "args": args,
                                   "env": {"GIT_INDEX_FILE": scratch}})
        assert result["exitCode"] == 0, result["stderr"]
    in_scratch = stdin_agent.call(
        "git.exec", {"repoPath": repo.path, "args": ["ls-files"],
                     "env": {"GIT_INDEX_FILE": scratch}})["stdout"]
    in_real = stdin_agent.call(
        "git.exec", {"repoPath": repo.path, "args": ["ls-files"]})["stdout"]
    assert "scratch.txt" in in_scratch
    assert "scratch.txt" not in in_real, \
        "a command aimed at a scratch index modified the real one"


def test_index_file_must_stay_in_the_sandbox(stdin_agent, repo):
    assert stdin_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path, "args": ["ls-files"],
                     "env": {"GIT_INDEX_FILE": "/etc/scrutiny-index"}}).code == \
        PERMISSION_DENIED


def test_editor_is_for_the_magit_preset_only(readonly_agent, stdin_agent,
                                             repo, tmp_path):
    """Naming git's editor is naming a program to run. The magit preset
    already serves `rebase --exec`, so it loses nothing by allowing it;
    the read-only preset would lose its meaning."""
    marker = tmp_path / "ran"
    env = {"GIT_EDITOR": "touch %s" % marker}
    assert readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path, "args": ["log", "-1"],
                     "env": env}).code == PERMISSION_DENIED
    assert not marker.exists()
    assert stdin_agent.call("git.exec",
                            {"repoPath": repo.path, "args": ["log", "-1"],
                             "env": env})["exitCode"] == 0


@pytest.mark.parametrize("key", ["GIT_SSH_COMMAND", "GIT_DIR",
                                 "GIT_WORK_TREE", "GIT_EXTERNAL_DIFF",
                                 "GIT_ASKPASS", "GIT_PAGER", "GIT_EXEC_PATH",
                                 "GIT_CONFIG_GLOBAL", "PATH", "LD_PRELOAD"])
def test_program_running_environment_is_refused(stdin_agent, repo, tmp_path,
                                                key):
    marker = tmp_path / "ran"
    assert stdin_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path, "args": ["log", "-1"],
                     "env": {key: "touch %s" % marker}}).code == \
        PERMISSION_DENIED
    assert not marker.exists()


def test_author_date_is_honored(stdin_agent, repo):
    with open(os.path.join(repo.path, "dated.txt"), "w") as handle:
        handle.write("x\n")
    stdin_agent.call("git.exec", {"repoPath": repo.path,
                                  "args": ["add", "dated.txt"]})
    done = stdin_agent.call(
        "git.exec", {"repoPath": repo.path,
                     "args": ["commit", "-q", "-m", "dated"],
                     "env": {"GIT_AUTHOR_DATE": "2001-02-03T04:05:06Z",
                             "GIT_AUTHOR_NAME": "A", "GIT_AUTHOR_EMAIL": "a@x",
                             "GIT_COMMITTER_NAME": "A",
                             "GIT_COMMITTER_EMAIL": "a@x"}})
    assert done["exitCode"] == 0, done["stderr"]
    shown = stdin_agent.call(
        "git.exec", {"repoPath": repo.path,
                     "args": ["log", "-1", "--format=%aI"]})["stdout"]
    assert shown.startswith("2001-02-03")


def test_null_unsets_a_variable(stdin_agent, repo):
    result = stdin_agent.call(
        "git.exec", {"repoPath": repo.path, "args": ["rev-parse", "HEAD"],
                     "env": {"LC_ALL": None}})
    assert result["exitCode"] == 0


# ---------------------------------------------------------------------
# the read-only preset is read-only, and stays in the sandbox
# ---------------------------------------------------------------------
def test_repo_path_must_be_inside_the_roots(agent_factory, tmp_path, repo):
    agent = agent_factory(["--allow-root", str(tmp_path),
                           "--git-exec-preset", "read-only"])
    assert agent.call_expect_error(
        "git.exec", {"repoPath": "/etc", "args": ["status"]}).code == \
        PERMISSION_DENIED


@pytest.mark.parametrize("args,what", [
    (["diff", "--no-index", "/dev/null", "/etc/hostname"],
     "reads any file"),
    (["grep", "--no-index", "-e", "root", "/etc/passwd"], "reads any file"),
    (["blame", "--contents", "/etc/hostname", "file.txt"], "reads any file"),
    (["blame", "--contents=/etc/hostname", "file.txt"], "reads any file"),
])
def test_reads_outside_the_repository_are_refused(readonly_agent, repo,
                                                  args, what):
    error = readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path, "args": args})
    assert error.code == PERMISSION_DENIED, what


@pytest.mark.parametrize("make_args", [
    lambda m: ["grep", "-Otouch %s;true" % m, "e"],
    lambda m: ["grep", "--open-files-in-pager=touch %s;true" % m, "e"],
    lambda m: ["ls-remote", "--exec=touch %s; git-upload-pack" % m, "."],
    lambda m: ["ls-remote", "--upload-pack=touch %s; git-upload-pack" % m,
               "."],
])
def test_read_only_commands_cannot_run_programs(readonly_agent, repo,
                                                tmp_path, make_args):
    marker = tmp_path / "ran"
    assert readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": make_args(marker)}).code == PERMISSION_DENIED
    assert not marker.exists()


@pytest.mark.parametrize("args", [
    ["branch"], ["branch", "--list"], ["branch", "-a", "-vv"],
    ["branch", "--list", "wo*"], ["branch", "--show-current"],
    ["tag"], ["tag", "--list", "v*"], ["tag", "-l"],
    ["symbolic-ref", "--short", "HEAD"], ["symbolic-ref", "HEAD"],
])
def test_read_only_preset_serves_listing_forms(readonly_agent, repo, args):
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path, "args": args})
    assert result["exitCode"] == 0, result["stderr"]


@pytest.mark.parametrize("args", [
    ["branch", "sneaky"], ["branch", "-D", "work"], ["branch", "-m", "x"],
    ["branch", "-a", "sneaky"], ["branch", "-f", "work", "HEAD~1"],
    ["tag", "sneaky"], ["tag", "-d", "v1"], ["tag", "-f", "v1"],
    ["tag", "-a", "v2", "-m", "x"],
    ["symbolic-ref", "HEAD", "refs/heads/sneaky"],
    ["symbolic-ref", "-d", "HEAD"],
])
def test_read_only_preset_refuses_writing_forms(readonly_agent, repo, args):
    before = git(repo.path, "for-each-ref", capture=True)
    head = git(repo.path, "symbolic-ref", "HEAD", capture=True)
    assert readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path, "args": args}).code == \
        PERMISSION_DENIED
    assert git(repo.path, "for-each-ref", capture=True) == before
    assert git(repo.path, "symbolic-ref", "HEAD", capture=True) == head


def test_magit_preset_still_creates_branches_and_tags(magit_agent, repo):
    for args in (["branch", "made-here"], ["tag", "made-here-tag"],
                 ["update-ref", "refs/wip/x", "HEAD"],
                 ["write-tree"]):
        assert magit_agent.call("git.exec", {"repoPath": repo.path,
                                             "args": args})["exitCode"] == 0


def test_unsetting_a_git_variable_is_always_allowed(stdin_agent, repo):
    """project.el clears GIT_DIR before listing files; that must not be
    what sends the command down the slow path."""
    result = stdin_agent.call(
        "git.exec", {"repoPath": repo.path, "args": ["ls-files"],
                     "env": {"GIT_DIR": None, "GIT_WORK_TREE": None}})
    assert result["exitCode"] == 0


def test_unsetting_other_variables_is_not(stdin_agent, repo):
    assert stdin_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path, "args": ["ls-files"],
                     "env": {"PATH": None}}).code == PERMISSION_DENIED


# ---------------------------------------------------------------------
# every spelling of a refused option
#
# git's option parser accepts any unambiguous abbreviation of a long
# option, a value either attached with `=` or as the next argument, and
# single-letter options bundled together. A policy that compares whole
# strings refuses `--upload-pack=x` and runs `--upload-pa=x`.
# ---------------------------------------------------------------------
def _refused_and_inert(agent, repo, args, marker):
    error = agent.call_expect_error("git.exec",
                                    {"repoPath": repo.path, "args": args})
    assert error.code == PERMISSION_DENIED, "%r: %s" % (args, error)
    assert not os.path.exists(marker), "%r ran its payload" % (args,)


@pytest.mark.parametrize("make_args", [
    lambda cmd: ["grep", "--open-files-in-pager=" + cmd, "hello"],
    lambda cmd: ["grep", "--open-files-in-pa=" + cmd, "hello"],
    lambda cmd: ["grep", "--open=" + cmd, "hello"],
    lambda cmd: ["grep", "--open", cmd, "hello"],
    lambda cmd: ["grep", "-O" + cmd, "hello"],
    lambda cmd: ["grep", "-lO" + cmd, "hello"],
    lambda cmd: ["ls-remote", "--upload-pack=" + cmd, "."],
    lambda cmd: ["ls-remote", "--upload-pa=" + cmd, "."],
    lambda cmd: ["ls-remote", "--upload-pack", cmd, "."],
    lambda cmd: ["ls-remote", "--exec=" + cmd, "."],
    lambda cmd: ["ls-remote", "--exe=" + cmd, "."],
    lambda cmd: ["ls-remote", "--exec", cmd, "."],
])
def test_command_execution_is_refused_in_every_spelling(readonly_agent, repo,
                                                        tmp_path, make_args):
    marker = str(tmp_path / "pwned")
    script = tmp_path / "payload.sh"
    script.write_text("#!/bin/sh\ntouch %s\n" % marker)
    script.chmod(0o755)
    _refused_and_inert(readonly_agent, repo, make_args(str(script)), marker)


@pytest.mark.parametrize("args", [
    ["log", "--output", "{out}", "-1"],
    ["log", "--output={out}", "-1"],
    ["log", "--outp={out}", "-1"],
    ["diff", "--output", "{out}"],
    ["show", "--out", "{out}"],
])
def test_a_read_only_agent_writes_no_file(readonly_agent, repo, tmp_path, args):
    out = str(tmp_path / "written-by-git")
    _refused_and_inert(readonly_agent, repo,
                       [a.format(out=out) for a in args], out)


@pytest.mark.parametrize("args", [
    ["diff", "/etc/hostname", "/etc/passwd"],
    ["diff", "--", "/etc/hostname", "/etc/passwd"],
    ["diff", "../../../../../../etc/hostname", "/dev/null"],
    ["diff", "--no-index", "a", "b"],
    ["diff", "--no-ind", "a", "b"],
    ["blame", "--contents", "/etc/passwd", "hello.txt"],
    ["blame", "--cont", "/etc/passwd", "hello.txt"],
    ["blame", "--contents=/etc/passwd", "hello.txt"],
    ["blame", "--ignore-revs-file", "/etc/passwd", "hello.txt"],
    ["blame", "-S", "/etc/passwd", "hello.txt"],
    ["grep", "-f", "/etc/passwd"],
    ["grep", "--file=/etc/passwd"],
    ["grep", "--no-index", "root", "/etc/passwd"],
    ["ls-files", "-X", "/etc/passwd"],
    ["ls-files", "--exclude-from=/etc/passwd"],
    ["diff", "-O/etc/passwd"],
    ["status", "--pathspec-from-file=/etc/passwd"],
])
def test_files_outside_the_roots_are_not_read(readonly_agent, repo, args):
    error = readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path, "args": args})
    assert error.code == PERMISSION_DENIED, "%r: %s" % (args, error)


@pytest.mark.parametrize("args", [
    ["diff", "--text", "HEAD"],
    ["diff", "HEAD~0..HEAD", "--", "hello.txt"],
    ["diff", "--stat", "--no-ext-diff", "HEAD"],
    ["log", "--oneline", "-1", "--name-only"],
    ["config", "--get", "--path", "core.hookspath"],
    ["grep", "-n", "-F", "-e", "x"],
    ["blame", "-s", "HEAD", "--", "hello.txt"],
    ["ls-files", "--exclude-standard", "--others"],
])
def test_ordinary_options_are_not_caught_with_them(readonly_agent, repo, args):
    """The refusals above are by prefix; these are the near misses."""
    result = readonly_agent.call("git.exec",
                                 {"repoPath": repo.path, "args": args})
    assert "exitCode" in result


def test_mandatory_environment_cannot_be_unset(readonly_agent, repo):
    """`GIT_PAGER=cat` is what keeps a request from hanging on a pager."""
    result = readonly_agent.call(
        "git.exec", {"repoPath": repo.path, "args": ["var", "GIT_PAGER"],
                     "env": {"GIT_PAGER": None}})
    assert result["stdout"].strip() == "cat"
