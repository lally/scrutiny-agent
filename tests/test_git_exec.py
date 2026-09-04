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


def test_denied_option_after_the_subcommand_is_still_refused(readonly_agent,
                                                             repo):
    """The scan covers every argument, not just the leading ones."""
    assert readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["log", "-c", "core.pager=id"]}).code == \
        PERMISSION_DENIED


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


def test_config_after_the_subcommand_is_refused(readonly_agent, repo):
    """`-c` is meaningful only before the subcommand, so only allowed there."""
    assert readonly_agent.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["log", "-c", "color.ui=false"]}).code == \
        PERMISSION_DENIED


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
    baseline = agent_factory(["--allow-root", "/tmp",
                              "--git-exec-preset", "read-only"])
    assert baseline.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["-c", "push.default=simple", "status"]}).code \
        == PERMISSION_DENIED

    extended = agent_factory(["--allow-root", "/tmp",
                              "--git-exec-preset", "read-only",
                              "--git-exec-config", "push.default"])
    assert extended.call("git.exec",
                         {"repoPath": repo.path,
                          "args": ["-c", "push.default=simple", "status",
                                   "--porcelain"]})["exitCode"] == 0


def test_extending_the_allowlist_does_not_widen_the_rest(agent_factory, repo):
    extended = agent_factory(["--allow-root", "/tmp",
                              "--git-exec-preset", "read-only",
                              "--git-exec-config", "push.default"])
    assert extended.call_expect_error(
        "git.exec", {"repoPath": repo.path,
                     "args": ["-c", "core.pager=id", "status"]}).code == \
        PERMISSION_DENIED
