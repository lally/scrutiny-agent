# Handoff: making Emacs fast over scrutiny-agent

## What the owner wants

Emacs (file open/edit/save, magit, eglot) must be fast against hosts
reached through a heavily loaded Teleport cluster, where one round trip
costs hundreds of milliseconds. The agent already keeps one persistent
connection; the Emacs integration was the problem. Requirements:
simple configuration, minimal intrusion into other Emacs packages, and
very high performance. Owner's direction mid-task: "if you see
something suspicious, assume it's wrong and let's fix it."

Everything so far was developed and measured against a **local
delay-injecting relay**, never against Teleport. The owner has a local
Docker Teleport setup. **Your first job is to validate on it**
(section 5), then work down section 6.

## 1. What is in this tree

Base commit: `bbcc66d` ("Fixing URLs."), the tip of
`five-rivers/scrutiny-agent` when this was written. The tarball
overlaid 23 changed/new files on top of it (uncommitted), plus this
`handoff/` directory and a root `CLAUDE.md`.

**Round two (2026-10-13) note.** The working tree's HEAD is `a011687`
on `origin/main` (lally), which shares no history with `bbcc66d`
(frd/main) and carries later LSP work (`lsp.workspaceInfo`, install
hints, text-search fallback). The overlay had silently reverted that
work in `agent/main.cpp`, `docs/protocol.md` and
`tests/conformance/conformance.py`. The three patches apply cleanly
with `git am -3` onto `a011687`; that was done in a scratch worktree
and the three merged files copied back, so the tree is now
`a011687` + the three patches + the round-two fixes below, still
uncommitted. `git diff a011687` is the whole change.

`handoff/patches/` holds the same change as three commits with full
messages. The overlay and the patches are the same content; use one:

    # keep the overlay and commit it yourself, or, for the 3-commit history:
    git stash -u            # or: git checkout . && git clean -fd -e handoff -e CLAUDE.md
    git am handoff/patches/*.patch

If upstream has moved past `bbcc66d`, prefer `git am -3` on the patches
over trusting the overlay, which would silently revert upstream edits
to those 23 files.

| Commit | Scope |
| --- | --- |
| 0001 | `agent/main.cpp`, CMake/conan (zlib), `tests/`, `docs/protocol.md` |
| 0002 | `emacs/scrutiny-agent.el`, `emacs/scrutiny-agent-fs.el` and their tests |
| 0003 | `emacs/scrutiny-agent-magit.el`, eglot, remote, verify, tests, `emacs/README.md` |

`docs/protocol.md` and `emacs/README.md` are updated and are the
reference for the new wire features and user-visible behaviour. Read
them rather than relying on the summary below.

## 2. What was wrong, and what changed

All cost was sequential round trips plus silent fallbacks to TRAMP;
client CPU is not a bottleneck.

Bugs found in `bbcc66d` (all fixed, all with tests):

- The client shell-quoted the `--config` path, so `~` never expanded:
  with the default install dir the agent came up **unconfigured**
  (read-only, no `git.exec`, rooted at `$HOME`) and everything went to
  TRAMP. Probably the main cause of the owner's experience. The client
  now warns at connect when `hello.config.loaded` is false.
- `git config` and `rev-parse --git-dir` were refused, so every magit
  refresh fell back; some magit calls bypassed the advice entirely.
- First keystroke in a buffer opened a TRAMP session (`lock-file`).
- A git child inherited the RPC channel as stdin (session hang).
- The environment was dropped: `GIT_INDEX_FILE` commands (magit stash,
  WIP) ran against the **real index**.
- `insert-file-contents` on a missing file signalled the wrong error;
  the handler regexp missed `user@host`; `~` resolved locally.
- **Security, read-only git preset**: command execution
  (`grep -O`, abbreviated `--upload-pack`/`--exec`), file write
  (`--output <path>`), file read outside the roots (`diff` on outside
  paths, `blame --contents`, `grep -f`). git's parser accepts any
  unambiguous abbreviation, so the policy now refuses by prefix
  (`spellsLongOption`). 31 regression tests in `tests/test_git_exec.py`
  fail against the `bbcc66d` binary. The owner should cut a release.
- Tunnels: every finished `git.spawn` leaked two threads; a full stdin
  queue could wedge the reader thread forever; pipes were not
  close-on-exec.

Design, in one paragraph: requests are futures and are pipelined
(`scrutiny-agent-send` / `-await`); a complete attributed directory
listing is a proof of absence and is kept exact by agent-pushed
inotify changes (`fs.changed`, ordered before the reply that caused
them), so listings need no TTL; save is one compare-and-swap write
(`ifMtimeNs`); git reaches the agent through the `process-file` /
`start-file-process` file-name-handler operations, **not** magit
advice; read-only git batches are predicted from the previous run of
the same command and sent at once (only questions are speculated,
never answers); asynchronous git (hunk staging, commit through
`with-editor`, rebase, push) is a real local process object (`sh` +
`cat` + FIFOs) bridged to `git.spawn`, with the sleeping-editor
handshake done via `git.signal`; large replies are deflated.

## 3. Build and test

Build as the repo README says (`scripts/bootstrap-linux.sh` or
`scripts/build-host.sh`; needs Conan 2). **The Conan path with the new
`zlib` requirement in `conanfile.txt` has never been run**: the work
was built against system packages with plain CMake. If it fails, that
is item 6.5. `docker/build-agent.sh` (the static release build) is
likewise untested with zlib.

    export SCRUTINY_AGENT_BIN=$PWD/build/agent/scrutiny-agent

    # wire tests (pytest-xdist optional)
    (cd tests && python3 -m pytest -q -n 2)
    python3 tests/conformance/conformance.py $SCRUTINY_AGENT_BIN --agent-version 0.2.0

    # Emacs tests; magit and its deps are needed for the magit suite
    handoff/bench/fetch-deps.sh
    E=handoff/bench/elpa
    for t in scrutiny-agent-tests scrutiny-agent-fs-tests scrutiny-agent-magit-tests \
             scrutiny-agent-ui-tests scrutiny-agent-eglot-tests; do
      emacs -Q --batch -L emacs -L $E/magit/lisp -L $E/compat -L $E/llama -L $E/cond-let \
            -L $E/transient/lisp -L $E/with-editor/lisp \
            -l emacs/$t.el -f ert-run-tests-batch-and-exit </dev/null
    done

Expected at handoff (Emacs 29.3, git 2.43, Linux): pytest 711 passed,
3 skipped, and one failure, `test_write_is_atomic_on_failure`, which
fails only when run as root (pre-existing). ERT: 43 + 67 + 41 + 37 + 3,
all passing. Conformance: ALL PASS. Byte-compilation is warning-free.

Round two, on macOS (Emacs 30.2, Apple clang 21, git 2.39 on the
remote): the Conan host build with the zlib requirement works
(`scripts/build-host.sh Release`), and so does the static Linux
release build, as `PLATFORMS=linux/amd64 AGENT_VERSION=0.2.0
docker/build-agent.sh` (the default multi-platform invocation needs a
containerd image store or the `scrutiny-builder` buildx builder; the
docker driver refuses it). Conformance: ALL PASS. pytest and ERT
tallies for this round are at the end of section 5b. `fs.watch` is
Linux-only, so the watch tests skip here.

`timeout` does not exist on a stock Mac, and `perl -e 'alarm N; exec'`
does nothing for batch Emacs (it owns SIGALRM). Wrap long runs as
`( cmd & p=$!; (sleep N; kill -9 $p) & wait $p )`.

Practical notes:

- Always give batch Emacs `</dev/null`; a test that hangs otherwise
  hangs the shell. Wrap long runs in `timeout`.
- An ambient `GIT_EDITOR` in the environment confounds the with-editor
  tests; unset it.
- ERT backtraces are enormous; pipe through `cut -c1-200 | head`.

## 4. The benchmark harness

`handoff/bench/bench.el` runs a scenario in batch Emacs and prints,
per step: wall time, agent requests, **round trips** (bursts of
requests sent without waiting in between), TRAMP commands, and every
operation handed to TRAMP. Configuration is by environment variable;
they are documented at the top of the file.

Scenarios: `s-files.el` (open/edit/save/dired/completion/project),
`s-magit.el` (status/refresh/stage/log/diff), `s-async.el` (hunk
staging, commit through with-editor). They assume the remote
repository is a clone of scrutiny-agent itself at `~/src/proj`
(`BENCH_REPO`), and modify/restore `gitmanip/src/status.cpp` in it.

Local mode (how the numbers below were produced; needs a `/su:`
TRAMP method that works without a password, i.e. root in a container):

    git clone . ~/src/proj && git -C ~/src/proj checkout -q bbcc66d
    DELAY=0.1 AGENT_BIN=$PWD/build/agent/scrutiny-agent SCENARIO=s-magit.el \
      emacs -Q --batch -l handoff/bench/bench.el </dev/null

`DELAY` is the one-way delay injected by `relay.py`; 0.1 means a
200 ms round trip. Reference results at 200 ms (before = `bbcc66d`):

| Step | Before | After | Round trips after |
| --- | --- | --- | --- |
| find-file | 0.86 s | 0.28-0.59 s | 1 |
| first keystroke | 0.53 s + a TRAMP session | 0 | 0 |
| save | 1.23 s | 0.21 s | 1 (first save with backup: 2) |
| dired, directory already seen | 0.64 s | 0.02 s | 0 |
| magit refresh | 9.44 s | 0.26-0.49 s | 1-2 |
| stage file + refresh | 10.67 s | 0.7 s | 3 |
| magit-log (repeat) | 3.09 s | 0.22 s | 1 |
| magit-status, first ever in a session with no history | 13.6 s | 6.5 s | 31 |
| stage a hunk | n/a (TRAMP) | 0.9 s | 3 |
| commit: until the message buffer opens | n/a | 4.2 s | 18 |
| commit: C-c C-c to finished | n/a | 0.65 s | 3 |

TRAMP commands: 0 in every step. Delegations seen: `get-file-buffer`
and `find-backup-file-name` only (neither opens a connection).

## 5. Validation against the owner's Teleport Docker setup (done)

The owner's rig is the git-review app's test cluster:
`~/Downloads/claude_projects/git-review/tests/teleport` (its README
is good). `./cluster.sh login --force` re-signs a 24 h identity with
no MFA; then every call is
`TELEPORT_HOME=<rig>/.state/tsh tsh --proxy=scrutiny-tp.localhost:3080 --insecure ...`.
Nodes used: `dev@node-direct` (the proxy dials it) and
`dev@node-tunnel` (reverse tunnel). Both are x86_64 Debian 12,
glibc 2.36, with GitHub egress; a clone of this repository was put at
`~/src/proj` on each (branch `main` at `a011687`, a git identity set).
Delay was added to `node-direct` from a privileged helper sharing its
network namespace (the node images have no `tc`):

    docker run --rm --privileged --net=container:scrutiny-teleport-node-direct-1 \
      alpine:3 sh -c 'apk add -q iproute2 && tc qdisc add dev eth0 root netem delay 200ms 20ms'

Measured through `tsh ... ssh dev@node sh` with a request/reply pair:
0.21 s per round trip with that delay, 3-5 ms without. A fresh
`tsh ssh` session costs 3 s at that delay (0.5 s without): the
one-connection design is the whole difference. Throughput through
tsh is low even on loopback -- 60 KB each way takes 0.17-0.24 s with
no delay, 0.4-1.1 s with it (SSH window growth over two hops) -- so
deflating large replies matters more on Teleport than on ssh.

The harness ran unchanged over Teleport:

    R=~/Downloads/claude_projects/git-review/tests/teleport
    TSH="TELEPORT_HOME=$R/.state/tsh /usr/local/bin/tsh --proxy=scrutiny-tp.localhost:3080 --insecure ssh dev@node-direct"
    DELAY=0.1 BENCH_HOST=node-direct BENCH_PREFIX=/ssh:node-direct: \
      BENCH_TRANSPORT="$TSH sh" BENCH_OOB="$TSH" AGENT_BIN=docker/dist/scrutiny-agent-0.2.0-x86_64 \
      SCENARIO=s-files.el emacs -Q --batch -l handoff/bench/bench.el </dev/null

(`DELAY` only tunes the burst detector; the delay itself is netem.)
`bench.el` now also prints `waits=`: round trips counted as blocking
waits with a send since the previous one, which is right on a fast
link where the gap-based `round-trips=` under-counts.

Results at ~210 ms RTT with 20 ms jitter, binary pushed by the
bootstrap through tsh (install + config + verify all fine; hello says
`config.loaded`, roots `/home/dev/src` writable, `git.exec`,
`git.spawn`, `fs.watch`, `rpc.deflate` present):

| Step | Wall | Round trips | Local mode (section 4) |
| --- | --- | --- | --- |
| find-file | 0.50 s | 1 | 1 |
| first keystroke | 0 | 0 | 0 |
| save (first, with backup / later) | 0.67 / 0.43 s | 2 / 1 | 2 / 1 |
| dired, directory seen | 0.04 s | 0 | 0 |
| magit-status, cold, no history | 5.9 s | 26 | 31 |
| magit refresh (first / repeat) | 0.85 / 0.47 s | 2 / 1 | 1-2 |
| stage file + refresh | 0.99 s | 3 | 3 |
| magit-log (first / repeat) | 1.70 / 0.45 s | 6 / 1 | - / 1 |
| stage a hunk | 1.35 s | 3 | 3 |
| commit: until the message buffer opens | 4.9 s | 17 | 18 |
| commit: C-c C-c to finished | 0.95 s | 3 | 3 |

`tramp-cmds=0` in every step; delegations seen: `get-file-buffer`
and `find-backup-file-name` only, as in local mode. The tunnel node
(no delay) gives the same counts.

Item 6.1 stress: `s-async.el` looped 50 times on `node-tunnel`.
48 runs clean (commit made, exit 0). The other two straddle the Mac's
clamshell sleep (14:12-15:09 by `pmset -g log`): one bootstrap probe
timed out while asleep, the next run's connection died at wake. That
wake-up run, though, exposed a real bug: its agent was still alive
on the node an hour later (6.6 below, fixed in 5b).

The node containers run Teleport as PID 1 with no init, so every
exited agent stays as a zombie in `ps` there. Not a leak.

**The netem delay was removed from `node-direct` at the end of the
round** (`tc qdisc del dev eth0 root` through the same helper).

## 5b. Round two: what was found and fixed

All with tests; the Teleport run and the macOS test run found them.

- **A plain `tsh ssh HOST` cannot carry the agent.** Teleport gives
  a plain session a PTY whatever the client asks (`--no-tty` too),
  which echoes the bootstrap back prompt-first and CRLF-terminated;
  the connect then sat out its first 30 s deadline and reported a
  timeout. The README's documented minimal config was exactly that.
  The bootstrap now recognises the echo of its own probe command and
  fails at once, naming the transport that works (`tsh ssh HOST sh`,
  an exec request, gets pipes). README, protocol.md and every example
  say so. Test: `scrutiny-agent-transport-with-a-terminal-is-diagnosed`
  (a local `python3 -c 'pty.spawn(["sh"])'` transport).
- **Roots reached through a symlink were "outside the roots".** The
  agent reports canonical (realpath) roots; a user writes names the
  way the root was configured (`/home/dev/src` when `/home` is a
  symlink; every macOS temp path). The handler then answered the
  whole sandbox from structure, and magit, finding no repository,
  called `executable-find` with the remote flag, whose `exec-path`
  operation the handler delegated -- a TRAMP connection on the hot
  path, invisible on Linux where TRAMP to localhost works. Agent:
  `fileSystemAccess.allowedRootsAsGiven` beside `allowedRoots`
  (hello and meta.capabilities). Client: both spellings are inside;
  the prefetch on connect uses the canonical ones only; `exec-path`
  is answered empty without TRAMP. Tests:
  `test_roots_are_reported_as_given_when_a_symlink_renames_them`,
  `scrutiny-agent-fs-root-given-through-a-symlink-is-inside`,
  `scrutiny-agent-fs-exec-path-never-reaches-tramp`, and the magit
  hunk-staging test now passes on macOS.
- **Structural listings ignored their arguments.** Outside the
  roots, `directory-files` returned the root's children whatever
  MATCH said, with a trailing slash. Emacs 30's `project-try-vc--search`
  takes any non-nil answer for a marker hit and recursed until
  `max-lisp-eval-depth` on every `find-file` of a file not in a
  repository (eglot's `find-file-hook` runs `project-current`). The
  structural answers now honour FULL/MATCH/NOSORT/COUNT and return
  real names; `file-name-all-completions` honours its prefix. Test:
  `scrutiny-agent-fs-structural-listings-are-shaped-like-listings`.
- **`fs.watch` was advertised on every platform.** Off Linux every
  watch is declined, so a client that believed the feature kept
  listings forever. The two watch features are now claimed only on
  Linux; the docs say so. Test: `tests/test_hello_features.py`.
- **The agent hung at shutdown with a git command running.** After
  stdin EOF, `shutdownTunnels` joined the tunnel threads while holding
  `g_tunMu`, which the output reader takes on its way out: deadlock,
  one agent left behind per dropped link (the Teleport stale-agent
  case above; reproduced locally with a commit waiting on its
  editor). The join now happens with the mutex released, and the
  whole git process group is killed so the editor cannot hold the
  pipe. Test:
  `test_end_of_input_ends_the_agent_even_with_a_waiting_editor`.
- `lsp.workspaceInfo` (restored upstream work) is now covered by the
  verify harness, which the coverage test demands.
- Test fixes for macOS: the eglot fixture binds `eglot-sync-connect`
  to t (with the default 3 s, `eglot--connect` returned with
  `initialize` still in flight when pylsp was slow, and a shutdown
  then failed that continuation from a sentinel, which in batch ends
  Emacs with status 255); two tests assumed Linux or an unabbreviated
  `default-directory`; one wire test rooted the agent at `/tmp`
  where pytest's tree is not; the watch-ordering spawn test skips
  without `fs.watch`. `stopping_` is `[[maybe_unused]]` off Linux.

Tallies for this round (macOS): see the end of this file.

## 6. Remaining work, in priority order

1. **Async-git stall: fixed empirically, not proven.** 48 of 48
   valid runs clean on Teleport this round (section 5); the two
   others were the Mac asleep. Symptom was a
   hang (about 1 run in 4-12 of `s-async.el`) at commit finish.
   Cause: `delete-process` on a helper process runs *all* pending
   sentinels synchronously; magit's sentinel then runs blocking git,
   and the nested `accept-process-output` in `scrutiny-agent-await`
   stalled although the future was already complete. The helpers in
   `scrutiny-agent-magit--bridge` are now killed with `signal-process`
   from a timer and never deleted. 35 consecutive clean runs since.
   Stress it on Teleport (loop `s-async.el` 50+ times under `timeout`).
   A structural fix would make `scrutiny-agent-await` robust to being
   entered from a sentinel. If it recurs, set the default of
   `scrutiny-agent-magit-async` to nil and tell the owner.
2. **Commit start is 18 round trips (4.2 s at 200 ms).** magit's
   commit setup and the commit-diff buffer are sequential and not
   covered by speculation, because the batch is opened by an async
   process rather than a command. Extend the prediction to it.
3. **Cold first status is 31 sequential round trips.** Prediction
   needs a previous run; the history file
   (`scrutiny-agent-magit-history-file`) covers later sessions only.
   Options: ship a default batch for `magit-status`, or key history by
   command rather than by repository.
4. **`GIT_CEILING_DIRECTORIES` (owner decision, do not just do it).**
   git discovers `.git` upward, so a root nested inside a larger
   repository exposes the whole repository to git commands. Setting
   ceilings closes that and breaks magit for anyone whose root is a
   subdirectory of a monorepo.
5. **Builds**: Conan with zlib and the static Docker release build
   are now verified (section 3). macOS has no inotify, so `fs.watch`
   is absent there (now also not claimed) and listings fall back to
   `scrutiny-agent-fs-cache-ttl`; a kqueue/FSEvents watcher is the
   obvious follow-up; `pipe2` is already guarded.
6. **Connection lifetime**: keepalive, and resuming a session after a
   transport drop without restarting language servers. Today the
   agent's lifetime is the connection's -- and the shutdown deadlock
   (5b) meant a dropped link could leave an agent alive forever;
   that is fixed, but a Teleport session that outlives its client
   (proxy keepalives take minutes to notice a vanished laptop) still
   keeps the agent until the node closes the pipe. An idle timeout
   on the agent, or a client ping it expects, would bound that.
7. **Processes other than git** (`compile`, `grep`, `shell-command`,
   eglot started via `make-process`) still go to TRAMP. The
   `git.spawn` tunnel is most of the mechanism; a general `proc.spawn`
   is a real widening of the agent's surface and needs the owner's
   say-so and an allowlist design.
8. Smaller agent items from an independent review, not yet done:
   `runProc` (behind `git.exec`) has no timeout, so a hook that leaves
   a daemon holding stdout blocks a worker forever; `ls-remote`/`fetch`
   accept a local path outside the roots as the remote; the fs barrier
   times out silently after 500 ms; `DirWatcher` calls `emit` with its
   mutex held (safe only while the writer never blocks).
9. The `git.exec` policy is still a deny-list for most subcommands. A
   per-subcommand option allowlist for the read-only preset would be
   the principled version; `isReadOnlyInvocation` already does this
   for `config`/`branch`/`tag`/`symbolic-ref`.

## 7. Things that will bite you

- A refused `git.exec` is not an error to the user: the client falls
  back to TRAMP and gets slow. Every policy tightening needs a check
  that magit's ordinary commands still pass
  (`test_ordinary_options_are_not_caught_with_them`, the magit ERT
  suite, and `scrutiny-agent-fs-delegations` after a bench run).
- The round-trip assertions in the ERT suites count *bursts*, not
  requests. Keep it that way: request counts go up when pipelining
  works.
- The agent binary is pushed to the host hash-gated; a host keeps a
  stale binary only if the client is pointed at one. Use
  `:local-binary` (or `AGENT_BIN` in the harness) while developing so
  the client and agent cannot drift.
- Features are negotiated through `hello.features`
  (`scrutiny-agent-feature-p`). Any new behaviour inside an existing
  method needs a feature name, and the client must still work against
  an agent without it.

## 8. Round-two tallies (macOS, Emacs 30.2, 2026-10-13)

Against `build/agent/scrutiny-agent` from `scripts/build-host.sh`:

- pytest: 686 passed, 32 skipped, 0 failed (`cd tests && python3 -m pytest -q`).
  Skips are the Linux-only watch tests and LSP servers not installed.
- conformance: ALL PASS.
- ERT: tests 44/44, fs 70 (62 passed, 8 skipped: watches), magit 41/41,
  ui 37/37, eglot 3/3 (needs `pylsp`). Byte-compilation of the sources
  is warning-free.
- Teleport, with the static Linux binary from `docker/build-agent.sh`
  (sha256 1be05f10...): bootstrap, hello and all three scenarios as in
  section 5; `lsp.workspaceInfo` present, `allowedRootsAsGiven` reported.

Not done this round: item 6.2 (commit start is 17 round trips), 6.3
(cold status 26), 6.4, 6.7, 6.8, 6.9; the kqueue watcher; a keepalive
or idle timeout for the stale-session case in 6.6.
