# scrutiny-agent

The remote backend process for [Scrutiny](https://github.com/lally/git-review),
a native macOS GitHub-PR review app. Scrutiny spawns this agent on a
remote box over a user-supplied stdio transport (ssh / `ssh -J` / tsh /
`kubectl exec` / a local shell / ...) and speaks JSON-RPC 2.0 with
LSP-style `Content-Length` framing to it. The agent answers git,
filesystem, LSP, indexing, and cache operations against the working
trees it runs next to. No Swift, no network listener — stdio only.

The wire protocol is documented in [docs/protocol.md](docs/protocol.md)
and enforced by the conformance suite
([tests/conformance/conformance.py](tests/conformance/conformance.py)):
every capability the agent advertises in `meta.hello` must have a
passing conformance check, and the suite fails if the advertised set
and the verified set drift apart in either direction.

Security posture, briefly (details in the protocol doc):

- **Credentials never land on the agent.** git auth is brokered from
  the client per prompt (`cred.request`/`cred.provide`, the agent
  re-exec'd as `$GIT_ASKPASS`); tokens exist only transiently in the
  broker pipe.
- **Filesystem sandbox.** `fs.*` paths are symlink-resolved and must
  fall inside configured allowed roots (`--allow-root`, repeatable;
  default `$HOME`). `meta.capabilities` / `fs.selftest` report the
  posture of the running binary.

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
docker/     portable-binary build image (debian:11, glibc 2.31 floor)
scripts/    build-host.sh — plain Conan+CMake host build
```

## Build

Prerequisites: CMake ≥ 3.25, a C++23 compiler, Conan 2, Python 3.

```sh
scripts/build-host.sh                 # -> build/agent/scrutiny-agent
ctest --test-dir build --output-on-failure          # unit tests
python3 tests/conformance/conformance.py \
    build/agent/scrutiny-agent                      # wire conformance
```

Smoke test:

```sh
printf 'Content-Length: %d\r\n\r\n%s' 95 \
  '{"jsonrpc":"2.0","id":1,"method":"meta.hello","params":{"supportedProtocolVersions":[1]}}' \
  | ./build/agent/scrutiny-agent --rpc-stdio
```

## Portable Linux binaries

`docker/build-agent.sh` produces the shippable, statically-linked
(except glibc + libsqlite3) `linux/amd64` + `linux/arm64` binaries
plus `.sha256` sidecars, named exactly as the Scrutiny bootstrap
expects (`scrutiny-agent-<version>-<arch>`):

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
git tag v0.1.0 && git push origin v0.1.0   # cut a release
```

## Versioning

The version the agent reports in `meta.hello` (and `--version`) is
injected at build time via `-DSCRUTINY_AGENT_VERSION`; release builds
carry the git tag. Dev builds default to the CMake project version.
The protocol version (currently 1) is negotiated separately in the
handshake and only changes on breaking wire changes.
