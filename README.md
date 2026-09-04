# scrutiny-agent

The remote backend process for [Scrutiny](https://github.com/lally/git-review),
a native macOS GitHub-PR review app. Scrutiny spawns this agent on a
remote box over a user-supplied stdio transport (ssh / `ssh -J` / tsh /
`kubectl exec` / a local shell / ...) and speaks JSON-RPC 2.0 with
LSP-style `Content-Length` framing to it. The agent answers git,
filesystem, LSP, indexing, and cache operations against the working
trees it runs next to. No Swift, no network listener — stdio only.

It also backs a **remote-development setup for Emacs**
([emacs/](emacs/README.md)): one connection carries find-file, save,
dired, completion, magit, eglot and code navigation, which is what
makes editing on a remote box usable over a link where establishing a
session is expensive (Teleport, jump hosts, MFA'd SSH).

The wire protocol is documented in [docs/protocol.md](docs/protocol.md)
and enforced by the conformance suite
([tests/conformance/conformance.py](tests/conformance/conformance.py)):
every capability the agent advertises in `meta.hello` must have a
passing conformance check, and the suite fails if the advertised set
and the verified set drift apart in either direction.

Besides the request-level API, the agent offers an opt-in
**allowlisted general git surface** (`git.exec`) so a real git UI can
drive the remote over the one connection — magit's synchronous git
runs over it (see [emacs/](emacs/README.md)). It is off unless the
operator enables it, and refuses the git options that would turn any
permitted subcommand into arbitrary code execution.

Besides the request-level API, the agent offers a **raw LSP tunnel**
(`lsp.tunnel*`): it spawns the language server next to the code and
pipes LSP bytes verbatim, so any real LSP client can speak its native
protocol through the multiplexed channel. An Emacs client built on it
lives in [emacs/](emacs/README.md) — eglot with the server running
remotely, over a single Teleport/ssh session.

Security posture, briefly (details in the protocol doc):

- **Credentials never land on the agent.** git auth is brokered from
  the client per prompt (`cred.request`/`cred.provide`, the agent
  re-exec'd as `$GIT_ASKPASS`); tokens exist only transiently in the
  broker pipe.
- **The fs write surface is opt-in.** The agent is read-only unless
  started with `--allow-write`; the write methods are advertised only
  when it is. Writes are atomic (temp + fsync + rename) and confined
  to the same allowed roots — `--allow-write` widens what may be done
  to a path, never which paths are reachable.
- **Filesystem sandbox.** `fs.*` paths are symlink-resolved and must
  fall inside configured allowed roots (`--allow-root`, repeatable;
  default `$HOME`). `meta.capabilities` / `fs.selftest` report the
  posture of the running binary — and `fs.selftest` probes through the
  same code path a client request takes, so it measures this agent
  rather than libc.
- **`git.exec` is opt-in and allowlisted.** Off unless the operator
  passes `--git-exec` / `--git-exec-preset`; not advertised in
  `meta.hello` when off. Refuses `-c` for any config key that could
  make git run a program, and every option that relocates the
  repository or names an external command.

## Layout

```
agent/      the scrutiny-agent executable (JSON-RPC dispatcher, lanes,
            chunking, cancellation, cred broker, fs sandbox)
core/       GitReviewCore: shared C++ core (git bridge, LSP client,
            indexer, caches, fs, HEAD watcher) with a C API — also
            linked in-process by the Scrutiny Mac app
gitmanip/   modern C++23 wrapper over libgit2
docs/       protocol.md — the normative wire API
tests/      conformance/ — wire-protocol conformance suite
            scrutiny/ + test_*.py — pytest behavioral suite (framing,
            handshake, chunking, cancellation, lanes, sandbox, cred
            broker, git/lsp/index/watch/diffcache)
emacs/      remote development over one connection: -fs (file-name
            handler for find-file/save/dired), -magit (magit's git over
            git.exec), -eglot (LSP tunnel), -xref (xref/eldoc/imenu),
            -remote (one-call setup), -ops (typed wrappers), -ui
            (commands), -verify (drives every capability)
docker/     portable-binary build image (debian:11, glibc 2.31 floor)
scripts/    bootstrap-linux.sh — toolchain check + Conan + build
            build-host.sh — the same build, assuming Conan is set up
            build-agents.sh — both Linux arches, installed for Emacs
            package-emacs.sh — the Emacs package (Lisp + both binaries)
```

## Build

Linux, one command, no root and no `-dev` packages — every library
comes from Conan, and Conan itself is fetched into a private venv if
it is not already installed:

```sh
scripts/bootstrap-linux.sh            # -> build/agent/scrutiny-agent
```

It checks the toolchain first (CMake ≥ 3.25, a compiler that really
compiles C++23) and fails with an actionable message rather than
thousands of lines of template errors. On Ubuntu 24.04 the only
prerequisites are `cmake g++ python3 python3-venv git`.

`scripts/build-host.sh` is the same build without the toolchain checks
or the Conan bootstrap, for when Conan is already set up.

Tests:

```sh
ctest --test-dir build --output-on-failure   # C++ unit tests (gitmanip + core)
tests/run-tests.sh                           # Python behavioral suite
python3 tests/conformance/conformance.py \
    build/agent/scrutiny-agent               # wire conformance
```

`tests/run-tests.sh` provisions its own venv (pytest plus pylsp, so the
LSP, tunnel and indexer tests assert against a real language server
instead of skipping) and takes pytest arguments: `-k sandbox` for one
area, `--run-slow` to include the soak tests.

Smoke test:

```sh
printf 'Content-Length: %d\r\n\r\n%s' 95 \
  '{"jsonrpc":"2.0","id":1,"method":"meta.hello","params":{"supportedProtocolVersions":[1]}}' \
  | ./build/agent/scrutiny-agent --rpc-stdio
```

## Configuration

The agent takes its settings from flags and/or a config file
(`--config <path>`), which use the same names:

```
allow-root = src          # filesystem sandbox; relative anchors under $HOME
allow-write               # enable the fs write surface (off by default)
git-exec-preset = magit   # allowlist for git.exec (off by default)
log-level = info
```

Clients stream this file to the host during bootstrap, hash-gated like
the binary, so the policy lives on the server rather than only on a
client's exec line.

## Portable Linux binaries

`docker/build-agent.sh` produces the shippable, statically-linked
(except glibc + libsqlite3) `linux/amd64` + `linux/arm64` binaries
plus `.sha256` sidecars, named exactly as the Scrutiny bootstrap
expects (`scrutiny-agent-<version>-<arch>`):

`scripts/build-agents.sh` is the convenient front end: it checks
Docker and the emulator registration first, builds both architectures,
and installs them where the Emacs client looks
(`~/.emacs.d/scrutiny-agent/`), so a laptop can drive both x86 and ARM
servers with no further configuration.

```sh
scripts/build-agents.sh                # both arches, installed for Emacs
scripts/build-agents.sh --arch arm64   # one
scripts/build-agents.sh --no-install   # leave them in docker/dist
```

The lower-level script it wraps:

```sh
AGENT_VERSION=0.1.0 docker/build-agent.sh
# -> docker/dist/scrutiny-agent-0.1.0-{x86_64,aarch64}[.sha256]
# single arch: PLATFORMS=linux/arm64 AGENT_VERSION=0.1.0 docker/build-agent.sh
```

The binaries are built inside a debian:11 image with clang-17/libc++
(static C++ runtime), so they run on any distro with glibc ≥ 2.31.

## CI and releases

- **ci.yml** — every push/PR builds natively on `ubuntu-24.04`
  (x86_64) and `ubuntu-24.04-arm` (aarch64), runs the unit tests and
  the wire conformance suite.
- **release.yml** — pushing a tag `v<version>` builds the portable
  binaries per architecture, re-runs the conformance suite against the
  exact release artifacts (including the `.sha256` hash-gate and the
  version check against the tag), and publishes a GitHub Release with
  the binaries attached.

```sh
git tag v0.2.0 && git push origin v0.2.0   # cut a release
```

A release carries both of the ways this gets used:

| Asset | For |
| --- | --- |
| `scrutiny-agent-<version>.tar` | **Emacs.** `M-x package-install-file`; both Linux binaries are bundled inside, so the first connect needs no further download. |
| `scrutiny-agent-<version>-{x86_64,aarch64}` + `.sha256` | **Scrutiny**, and any client that bootstraps by name. |

The workflow installs the package into a throwaway Emacs and checks it
reports the right version and ships both architectures *before*
publishing — a release that installs but cannot connect is worse than
no release.

Build the same artifacts locally:

```sh
scripts/build-agents.sh --no-install   # both Linux binaries
scripts/package-emacs.sh               # -> dist/scrutiny-agent-<version>.tar
```

## Versioning

The version the agent reports in `meta.hello` (and `--version`) is
injected at build time via `-DSCRUTINY_AGENT_VERSION`; release builds
carry the git tag. Dev builds default to the CMake project version.
The protocol version (currently 1) is negotiated separately in the
handshake and only changes on breaking wire changes.
