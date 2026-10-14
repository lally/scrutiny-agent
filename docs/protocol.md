# scrutiny-agent wire protocol (v1)

This is the normative description of the JSON-RPC API `scrutiny-agent`
serves over stdio. The conformance suite
(`tests/conformance/conformance.py`) verifies a built binary against
this document: every capability the agent advertises in `meta.hello`
must have a passing conformance check, and the suite fails if the
advertised set and the verified set drift apart in either direction.

The agent is spawned over a user-supplied stdio transport (ssh /
`ssh -J` / tsh / `kubectl exec` / a local shell / ...) as:

```
scrutiny-agent --rpc-stdio [--config <path>] [--log <path>] [--log-level <lvl>]
                           [--allow-root <path>]... [--allow-write]
                           [--git-exec <sub>]... [--git-exec-preset <name>]
                           [--git-exec-config <key>]...
```

`--config <path>` reads settings from a file: one `key = value` per
line, `#` to end of line for comments, a bare `key` meaning true.
Every flag above has a config key with the same name minus the
leading dashes (`allow-root`, `allow-write`, `git-exec-preset`, …),
and repeatable flags accumulate. Flags and file feed the same table,
so neither can gain an option the other lacks; both apply, in the
order they are read. Relative config paths anchor under `$HOME`. An
unreadable file or an unknown key warns on stderr rather than
aborting — one stale line in a shared config must not stop an agent
from starting.

The point of a file is that policy lives on the host: it is readable
and editable where the agent runs, it survives reconnects, and every
client bootstrapping into the same install directory agrees about
what the agent may touch.

`--version` prints `<agentVersion> proto <protocolVersion>` and exits.

## Bootstrap

Before any frame is exchanged, a client gets the right binary onto the
host and proves it runs. The sequence is fixed, and every step is a
marker-terminated line with its own deadline, so a failure names
itself instead of surfacing as a timeout at the handshake.

| # | Step | Sent | Reply |
| --- | --- | --- | --- |
| 1 | **Probe the platform** | `echo __SCRA_PROBE__ $(uname -s) $(uname -m)` | `__SCRA_PROBE__ Linux x86_64` |
| 2 | **Check what is installed** | sha256 the installed binary | `__SCRA_ST__ OK` \| `__SCRA_ST__ <hash>` \| `__SCRA_ST__ MISSING` |
| 3 | **Install** (only if 2 was not `OK`) | base64 heredoc, then hash-gate and atomic rename | `__SCRA_INST__ OK` \| `__SCRA_INST__ <hash>` |
| 4 | **Verify it runs** | `"$B" --version` | `__SCRA_VERIFY__ <status> <output>` |
| 5 | **Install the config** (hash-gated like 2/3) | base64 heredoc | `__SCRA_CFG__ …`, `__SCRA_CFGDONE__ OK` |
| 6 | **Hand over** | `echo __SCRA_EXEC__; exec "$B" --rpc-stdio …` | `__SCRA_EXEC__` |

**Step 1** is also the terminal check. The transport must put the
remote shell on pipes; a PTY echoes every line back, prompt first and
CR LF terminated, so no marker ever starts a line. Teleport allocates
one for a plain `tsh ssh HOST` session whatever the client asks
(`tsh ssh HOST sh` is an exec request and gets pipes). A client that
sees its own probe command echoed back fails immediately, naming the
transport to use, rather than waiting out the deadline.

**Step 1** uses only `uname -s` and `uname -m`: both are POSIX, and
both exist everywhere this could run. `uname -o`/`-i`/`-p` are not
portable. `uname -m` spellings are normalized (`amd64` → `x86_64`,
`arm64` → `aarch64`) before selecting an asset. A platform with no
published binary is an error naming what was found — distinct from a
binary that is present but broken.

**Step 4** is what makes a bad binary diagnosable. `--version` is the
no-op probe because one line answers three questions at once: the file
execs (a wrong architecture fails here with `Exec format error`),
dynamic linking resolved (a missing `libc`/`libsqlite3` fails here),
and the binary is the version that was intended. The exit status is
returned with the output flattened to one line, so the client reports
*why* rather than timing out at `meta.hello` twenty seconds later.

**Step 6**'s marker closes a real race. The shell reads its script
from the same pipe the protocol will use; anything written before it
has consumed the exec line can be swallowed by its read-ahead and
never reach the agent, which then waits on stdin while the client
times out on a handshake it did send. Echoing on the *same line* as
the exec means that when the marker arrives the shell has read exactly
up to that newline and no further, leaving everything after it
untouched in the pipe for the agent to inherit.

## Framing

LSP-style length-prefixed frames over stdio:

```
Content-Length: <N>\r\n
\r\n
<N bytes of UTF-8 JSON>
```

Frames are capped at a negotiated maximum (default 128 KiB, clamped to
the 64–256 KiB range). Responses whose serialized body would exceed
the cap are chunked (below). stdout carries only protocol frames; all
agent diagnostics go to stderr or the `--log` file.

## Messages

JSON-RPC 2.0, three kinds:

- **Request** (client → agent):
  `{"jsonrpc":"2.0","id":<n>,"method":"<ns>.<op>","params":{...}}`.
  An optional top-level `"lane"` (`"interactive"` / `"normal"` /
  `"bulk"`) overrides the per-method default priority lane.
- **Response** (agent → client):
  `{"jsonrpc":"2.0","id":<n>,"result":{...}}` or
  `{"jsonrpc":"2.0","id":<n>,"error":{"code":<c>,"message":"..."}}`.
- **Notification** (either direction, no `id`).

A frame that is not valid JSON is answered with a null-id
`INVALID_REQUEST` error envelope; the connection stays up. A request
with an `id` but no string `method` is answered `INVALID_REQUEST`. An
unknown method is answered `NOT_FOUND` with message
`unknown method: <name>`.

The `Content-Length` header is matched case-insensitively. A header
block that carries no `Content-Length` at all is answered with a
null-id `INVALID_REQUEST` and skipped -- it does **not** end the
session. This matters because the transport is a shell pipe: a late
login banner or a stray `motd` line arriving on stdin must not be
indistinguishable from the transport closing. Only EOF ends a
session. A bare `\r\n\r\n` between frames is padding and is
ignored silently.

### Streaming large bodies (`rpc.chunk`)

A response whose body would exceed the frame cap is split into
`rpc.chunk` notifications keyed by the request id, followed by a small
envelope response:

```
{"jsonrpc":"2.0","method":"rpc.chunk","params":{"id":42,"seq":0,"last":false,"data":"<base64>"}}
...
{"jsonrpc":"2.0","method":"rpc.chunk","params":{"id":42,"seq":N,"last":true,"data":"<base64>"}}
{"jsonrpc":"2.0","id":42,"result":{"streamed":true,"bytes":<total>}}
```

The client concatenates `data` in `seq` order, base64-decodes, and
parses the reconstructed JSON-RPC response. Chunks from different
requests may interleave freely.

### Cancellation

`{"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":<n>}}`
(notification). Queued work is dropped before it starts; long-running
work polls the flag at safe points. A cancelled request answers error
`CANCELLED` (1007).

### Priority lanes

Three lanes — `interactive`, `normal`, `bulk` — govern both worker
scheduling (per-lane slot caps: normal 8, bulk 2, pool 16) and writer
scheduling (higher lanes preempt at frame boundaries). Per-method
defaults: `index.run`, `git.clone`, `git.fetch`,
`git.ensureRepository` are bulk; `meta.debug` and `cred.selftest`
are normal; everything else is interactive.

## Handshake

The first request must be:

```
meta.hello { clientVersion, supportedProtocolVersions: [1], frameCap? }
  -> { agentVersion, protocolVersion, frameCap, capabilities: [...] }
```

If the client cannot speak the agent's protocol version, the agent
answers `VERSION_MISMATCH` (1006) and the client re-bootstraps a
matching binary. `frameCap` is the client's proposal, clamped into
[65536, 262144]; the response value is authoritative for the
connection. `capabilities` is the exhaustive list of methods the
agent serves (the conformance contract).

The hello result also carries what a client would otherwise spend its
first round trips discovering:

```
  features: [...],                       // see below
  fileSystemAccess: { allowedRoots, allowedRootsAsGiven, writable, home },
  config: { loaded, unreadable },        // was a --config file read?
  encoding?: "deflate"                   // only when negotiated
```

`features` names behaviours *within* a method, which `capabilities`
(a list of methods) cannot express. A client must test for a feature
before relying on it; an agent without `features` has none of them.

| Feature | Meaning |
| --- | --- |
| `hello.facts` | the hello result carries `fileSystemAccess` and `config` |
| `fs.watch` | `fs.listDirectory {watch}` and `fs.changed` (Linux/inotify; not claimed elsewhere, where every watch is declined) |
| `fs.changed.ordered` | the causal-ordering guarantee under *Directory watches* (Linux only, like `fs.watch`) |
| `fs.listDirectory.self` | attributed listings carry `self`, the directory's own entry |
| `fs.listDirectory.maxEntries`, `fs.readFile.maxBytes` | the `tooLarge` refusals |
| `fs.mtimeNs` | every entry / stat / write result carries `mtimeNs` |
| `fs.writeFile.ifMtimeNs` | conditional write |
| `git.exec.stdin`, `git.exec.env` | `stdinBase64` and `env` on `git.exec` |
| `rpc.deflate` | the agent can compress (see *Compression*) |
| `lsp.tunnelSend.eof` | `eof: true` half-closes a tunnel's stdin |

`config.loaded: false` with a client that believes it sent a config is
the single most consequential misconfiguration there is: the agent is
then read-only, has no `git.exec`, and is rooted at `$HOME`, and
every client quietly falls back to something slower. Clients should
say so loudly.

### Compression

A client that lists `encodings: ["deflate"]` in `meta.hello` gets
`encoding: "deflate"` back from an agent that supports it, and from
then on, on that connection:

- a response whose body is at least 1024 bytes is sent as `rpc.chunk`
  notifications carrying the base64 of the **zlib-deflated** JSON
  envelope, and its final frame is
  `{ result: { streamed: true, bytes, encoding: "deflate" } }`
  (`bytes` is the inflated size). Without `encoding` the chunks are
  the plain envelope, as before.
- an `lsp.tunnelRecv` whose payload is at least 512 bytes carries
  `z: true`, and `data` is the base64 of the deflated bytes.

Requests are never compressed. Source, diffs and listings deflate
3-5x, which is what matters on a link where the bytes are the cost;
base64 alone had been *adding* 34-78 % to every large reply.

## Error model

```
1000 INTERNAL            generic agent-side failure (message carries the reason)
1001 NOT_FOUND           repo/file/path/id missing; also unknown method
1002 INVALID_REQUEST     malformed JSON or missing/mistyped params
1003 GIT_FAILED          git subprocess non-zero exit (message carries stderr)
1004 LSP_FAILED          language server unavailable/crashed
1005 PERMISSION_DENIED   path outside the fs sandbox allowed roots
1006 VERSION_MISMATCH    protocol version negotiation failed
1007 CANCELLED           request cancelled by the client
```

## fs sandbox

Every path given to `fs.readFile` / `fs.listDirectory` is anchored
under `$HOME` if relative, symlink-resolved with `realpath()`, then
checked against the allowed roots (`--allow-root`, repeatable;
default `$HOME`). The kernel only ever sees the canonical path
(closes the symlink-swap TOCTOU window). Outside every root →
`PERMISSION_DENIED` (1005), distinct from `NOT_FOUND` (1001) — the
distinction is load-bearing for client UI.

## Credential broker

Git on the agent never receives a token in URL/config/env. For
`git.clone` / `git.fetch` / `git.ensureRepository` the agent re-execs
itself as `$GIT_ASKPASS` connected to a private per-op unix socket.
When git prompts, the agent emits a `cred.request` notification
`{ authOpId, credId, prompt }`; the client answers with a
`cred.provide` request `{ credId, value }`. The secret exists on the
agent only transiently, in the broker→askpass pipe.

Two consequences the implementation must preserve: the broker's unix
socket lives in `$TMPDIR` (not in the repository -- `sun_path` is 108
bytes, and anchoring in the repo broke every brokered op for
checkouts deeper than ~85 characters, besides littering the work
tree), and `value` is redacted from the agent's log at **every** log
level, since `logs.tail` streams that log back to the client.

## Methods

Reference of request params → result shape. All `path` params are
absolute; relative cache/install paths anchor under `$HOME`.

### meta.*

| Method | Params | Result |
| --- | --- | --- |
| `meta.hello` | `clientVersion`, `supportedProtocolVersions[]`, `frameCap?` | `{ agentVersion, protocolVersion, frameCap, capabilities[] }` |
| `meta.debug` | `sleepMs?`, `padBytes?` | `{ echoId, pad }` — diagnostics: sleeps (cancellable), pads the result to exercise chunking |
| `meta.stat` | — | `{ lanes: {interactive,normal,bulk}, lspSessions, lspTunnels, uptimeMs, gitParallel, logLevel, agentVersion }` |
| `meta.capabilities` | — | `{ agentVersion, protocolVersion, outboundIO[], fileSystemAccess{}, languageServers{}, network, selftestMethod }` — the agent's security/IO posture. `fileSystemAccess` carries `allowedRoots` (the roots **this process** is running with, canonical, so a client reports fact rather than default), `allowedRootsAsGiven` (the spellings those roots were configured with, listed only where `realpath` changed them -- `/home/dev/src` when `/home` is a symlink; the sandbox enforces the canonical path, but a client can then tell that a name written the configured way is inside it without asking), `writable` (whether the write surface is served), and `home` (the remote `$HOME` — a client needs it to expand `~`, and cannot discover it by listing when `$HOME` is not itself inside the roots). |
| `logs.tail` | `maxBytes?` (≤ 1 MiB) | `{ enabled, path?, level?, bytes?, text? }` — tail of the `--log` file; `{ enabled: false }` when logging is off |

### git.* (read surfaces)

| Method | Params | Result |
| --- | --- | --- |
| `git.headSha` | `path` | `{ headSha }` |
| `git.repoMetadata` | `path` | `{ path, gitDir, isBare, headSha, currentBranch, hasUncommittedChanges }` — `path` is the **work tree** (what every other `git.*` `path` param means, and what a UI should display); `gitDir` is libgit2's repository path (`<repo>/.git/`). For a bare repo both are the gitdir. |
| `git.remotes` | `path` | `{ remotes: [{ name, url, pushUrl }] }` (pushUrl falls back to url) |
| `git.branches` | `path`, `local?`, `remote?` | `{ branches: [{ name, refname, targetOid, isRemote, isHead, upstream\|null, remoteName\|null }] }` |
| `git.commits` | `path`, `branch?`, `limit?` (default 100) | `{ commits: [{ oid, shortOid, message, summary, authorName, authorEmail, authorTime, parentOids[] }] }` newest first |
| `git.aheadBehind` | `path`, `branch` | `{ ahead, behind }` vs upstream |
| `git.diffForCommit` | `path`, `sha` | `{ diffs: [FileDiff] }` (unknown sha → `[]`) |
| `git.workingTreeDiff` | `path` | `{ diffs: [FileDiff] }` index → workdir, incl. untracked |
| `git.stagedDiff` | `path` | `{ diffs: [FileDiff] }` HEAD → index |
| `git.showFile` | `path`, `sha`, `file` | `{ content: string\|null }` (missing path in commit → null) |
| `git.diff` | `path`, `from`, `to`, `file` | `{ diff: string\|null }` (`git diff` subprocess; immutable by key) |
| `git.isAncestor` | `path`, `ancestor`, `descendant` | `{ isAncestor }` (`git merge-base --is-ancestor`) |

`git.exec` (below) covers anything else, when the operator enables it.

`FileDiff` = `{ status (libgit2 int), oldPath, newPath, patch,
hunks: [{ oldStart, oldLines, newStart, newLines, header,
lines: [{ origin (1-char), content, oldLineNo, newLineNo }] }] }`.

### git.* (mutating / network; bulk lane, credential-brokered)

| Method | Params | Result |
| --- | --- | --- |
| `git.checkoutBranch` | `path`, `branch` | `{}` on success; `INTERNAL` with the libgit2 reason on refusal (SAFE checkout) |
| `git.clone` | `fullName`, `cloneURL`, `installDir`, `authOpId` | `{ localPath, cloneURL, lastFetched }` — clones to `<installDir>/<fullName with / -> _>` |
| `git.fetch` | `repoPath`, `authOpId` | `{ ok, lastFetched }` (`git fetch --all --prune`) |
| `git.ensureRepository` | `fullName`, `cloneURL`, `installDir`, `authOpId` | `{ localPath, cloneURL, lastFetched }` — fetch if present else clone |

### git.exec (allowlisted general git; opt-in)

`git.exec` runs a git subcommand the agent has no typed method for, so
a real git UI can drive the remote over the one multiplexed
connection instead of a subprocess per invocation. It is the widest
method in the protocol and is therefore **off unless the operator
enables it**, and is advertised in `meta.hello` only when enabled.

```
scrutiny-agent --rpc-stdio \
    [--git-exec <subcommand>]...      allow one subcommand
    [--git-exec-preset read-only|magit]
    [--git-exec-config <key>]...      extend the -c allowlist
```

| Method | Params | Result |
| --- | --- | --- |
| `git.exec` | `repoPath`, `args[]`, `authOpId?`, `stdinBase64?`, `env?` | `{ exitCode, stdout, stderr, truncated }` |
| `git.spawn` | `repoPath`, `args[]`, `env?` | `{ tunnelId, pid }` — the same policy as `git.exec`, but the command runs as a tunnel (below) |
| `git.signal` | `pid`, `signal` | `{ ok }` — `pid` must be in the process group of a live `git.spawn` command |

A non-zero `exitCode` is **data, not an error**: git answers questions
with exit codes (`diff --quiet`, `merge-base --is-ancestor`). Output
is capped at 16 MiB per stream, with `truncated: true` when the cap
bites. Subcommands that can reach the network (`fetch`, `pull`,
`push`, `ls-remote`, `clone`, `submodule`) go through the credential
broker when `authOpId` is given. Runs on the `normal` lane by default;
override with the top-level `lane` field.

`repoPath` is authorized against the fs roots like any other path.
The child's stdin is `/dev/null`, or exactly the bytes of
`stdinBase64` (`git apply --cached -`, `hash-object --stdin`,
`update-index --index-info`); it is never the agent's own stdin,
which is the RPC channel.

`env` is an object, `{ "NAME": "value" }` to set and `{ "NAME": null }`
to unset. Only an allowlist of names may be set — the ones that change *what
git computes* without changing *what git runs*: `GIT_INDEX_FILE`
(which must itself lie inside the roots), the author/committer
identity and dates, `GIT_OPTIONAL_LOCKS`, `GIT_LITERAL_PATHSPECS`,
`GIT_REFLOG_ACTION`, `GIT_MERGE_AUTOEDIT`, `LC_ALL`/`LANG`/`TZ` and a
few like them. Unsetting any `GIT_*` name is always allowed, except
that the agent's own `GIT_PAGER=cat` and `GIT_TERMINAL_PROMPT=0` are
applied last. `env` and `stdinBase64` cannot be combined with
`authOpId` (use `git.spawn`). Dropping
`env` on the floor is not an option: a client that builds a tree
against a temporary index (`GIT_INDEX_FILE=… git read-tree`, as magit
does for stashes and WIP commits) would be run against the **real
index** instead.

`git.spawn` is `git.exec` for commands that are long-running, read
stdin incrementally, prompt, or wait on an editor (`commit`, `rebase
-i`, `push`, `pull`, `apply` fed a hunk). The result's `tunnelId` is
an ordinary tunnel: output (stdout and stderr merged) arrives as
`lsp.tunnelRecv`, input goes by `lsp.tunnelSend` (`eof: true` closes
stdin), exit is `lsp.tunnelClosed`. The command gets its own process
group and the credential broker. Exit is detected by `waitpid`, not
by EOF on the pipe, so a detached grandchild holding the pipe open
does not hide it.

When the operator allows editors (the `magit` preset, or
`--git-exec-editor`), `env` may also set `GIT_EDITOR`,
`GIT_SEQUENCE_EDITOR`, `EDITOR` and `VISUAL`. That is deliberately a
grant of command execution to the client — it is what lets a client
install a "sleeping editor" that prints the file to edit and waits
for `git.signal { signal: "USR1" }` (finish) or `"USR2"` (abort)
while the user writes the commit message over `fs.*`.

Two checks, and the second is the one that matters:

1. The subcommand must be on the allowlist.
2. No argument may be one of git's "run this command for me" options.
   **A subcommand allowlist alone is not security**: `-c
   core.pager=…`, `-c credential.helper=…`, `-c alias.x=!…`,
   `--upload-pack=`, `--receive-pack=`, `--exec-path=` and friends
   turn *any* permitted subcommand into arbitrary code execution.
   Also refused: `--git-dir`/`--work-tree`/`-C`/`--namespace` (which
   would move the repository out from under `repoPath`),
   `--gpg-sign=`, `--output=`, and `--textconv`. Refused per
   subcommand: `--no-index` (`diff`, `grep` — it reads any file on
   the host), `grep -O`/`--open-files-in-pager` and `--exec` on
   `ls-remote`/`fetch`/`pull`/`push`/`clone` (command execution),
   `blame --contents`/`--ignore-revs-file`/`-S`, `grep -f`/`--file`,
   `ls-files -X`/`--exclude-from`, `--pathspec-from-file`, `-O<file>`
   (arbitrary file read), and, for `diff`, any path argument that is
   absolute or contains a `..` component (`git diff` compares
   arbitrary files, unasked, when given paths outside the work tree).

   Every long option above is refused in **every spelling git
   accepts**: git's parser takes any unambiguous abbreviation
   (`--upload-pa=…`, `--outp …`, `--cont …`) and a value either
   attached with `=` or as the next argument, so the check is "is this
   argument's name a prefix of a refused option", not string equality.
   Single-letter options are looked for inside bundles (`-lO<cmd>`).
   The price is the occasional false refusal (a `grep` pattern given
   as `-eOops`), which costs that request the fast path and nothing
   else. Before the
   subcommand, only a fixed set of global options is accepted at all,
   so an unanticipated spelling cannot slip through.

`-c <key>=<value>` is permitted before the subcommand for an
allowlisted set of *keys* — formatting and local-behavior settings
only (`color.*`, `advice.*`, `core.preloadIndex`, `log.showSignature`,
`diff.noPrefix`, …). Keys are allowlisted rather than deny-listed
because the execution-capable ones are too many and too easily
extended for a deny list to be trustworthy. This set exists because
magit sends `-c core.preloadIndex=true -c log.showSignature=false -c
color.ui=false -c color.diff=false -c diff.noPrefix=false` on every
invocation; refusing `-c` outright would mean refusing magit.

Some subcommands are both a query and a mutation (`config`, `branch`,
`tag`, `symbolic-ref`). The `read-only` preset admits only their read
forms (`config --get…`/`--list`, `branch --list`/`--show-current`/…,
`tag -l`, `symbolic-ref <name>`); the `magit` preset admits them
whole.

Failures of the policy answer `PERMISSION_DENIED` (1005) and start no
process at all. Repository-local config and hooks are out of scope
here: they are the user's own, already reachable through `git.fetch`,
and equally in play for every other `git.*` method. What the policy
prevents is the *client* injecting them.

### fs.*

| Method | Params | Result |
| --- | --- | --- |
| `fs.readFile` | `path`, `base64?`, `stat?`, `maxBytes?` | `{ content }`, or `{ contentBase64 }` with `base64: true`; `stat: true` adds `size`, `mtime`, `mode`. Base64 exists because a JSON string cannot carry arbitrary bytes — a client that must reproduce a file exactly asks for it. A regular file larger than `maxBytes` answers `{ tooLarge: true, size, mtime, mtimeNs, mode }` and no content, so a client can prefetch speculatively without ever paying for a huge file it did not mean to open. |
| `fs.listDirectory` | `path`, `attributes?`, `watch?`, `maxEntries?` | `{ path (canonical absolute), entries: [{ name, isDir }] }` sorted by name; symlinks followed for `isDir`. With `attributes: true` each entry also carries `size`, `mtime`, `mode`, `isRegular`, `isSymlink`, `symlinkTarget`, `uid`, `gid`, `readable`, `writable`, and `path` (the resolved name, so a client can answer `file-truename` from the listing) — one request answers everything a client is about to ask about that directory. Attributed listings also carry `self`, the directory's own entry. A directory with more than `maxEntries` children answers `{ path, tooLarge: true, count, self? }` instead. `watch: true` adds `watching: true|false`; see *Directory watches*. |
| `fs.stat` | `path` | `{ exists, isDir, isRegular, isSymlink, symlinkTarget, size, mtime, mode, uid, gid, readable, writable, path }`, or `{ exists: false }`. **Does not follow the final component**: the parent is canonicalized and authorized, then the leaf is `lstat`ed, so a symlink is described rather than resolved away (`path` still names the resolved target). A path inside the roots that is simply absent answers `exists: false`, not an error — that is the question being asked. |
| `fs.statBatch` | `paths[]` | `{ stats: [...] }` in the order asked. One bad path yields a per-entry `{ exists: false }` (plus `denied: true` when outside the roots) rather than failing the batch. |
| `fs.selftest` | — | `{ probe, succeeded, errorCode, reason?, firstBytesReadable, sampleSnippet }` — runtime sandbox probe of /etc/passwd. The probe takes the same route a client request takes (resolve, authorize, read the canonical path), so it reports **this agent's** posture; a denied probe returns `succeeded: false`, `errorCode: "PERMISSION_DENIED"`, and no content. |

Every entry, stat and write result carries `mtimeNs` (nanoseconds
since the epoch) beside the whole-second `mtime`; it is the version
stamp used by conditional writes.

#### Directory watches

A complete attributed listing is a proof: a name that is not in it
does not exist. That makes one listing answer every later `stat`,
`file-exists-p` and completion in that directory — for as long as the
listing is still true. `watch: true` is how the agent keeps it true:

```
fs.listDirectory { path, attributes: true, watch: true }
  -> { ..., watching: true }
fs.changed { path, entries: [ entry | { name, deleted: true } ], self }
fs.changed { path, reset: true [, unwatched: true] }
fs.changed { all: true, reset: true }
```

`entries` are full attributed entries for names created or modified
and tombstones for names removed; `self` is the directory's new
entry. `reset` means "discard what you hold for this directory"
(events were lost, or it was removed); `unwatched` adds "and I have
stopped watching it". `all` is a queue overflow: discard everything.
A client holding a watched listing therefore needs **no TTL** — its
cache is exact until told otherwise.

`watching: false` means the agent declined and the client must fall
back to expiring the listing by time. It declines on network
filesystems (inotify sees only local changes there). It later
withdraws a watch, with `unwatched`, from a directory that changes
too often to be worth describing (more than 40 notifications in 10 s
— a build directory) and from the least recently listed directory
once more than 2048 are watched.
Notifications are coalesced (25 ms quiet, 150 ms at most).

**Ordering guarantee** (`fs.changed.ordered`): the reply to any
request that changes the filesystem through the agent — an `fs.*`
write, `git.exec`, the close of a `git.spawn` — is sent *after* the
`fs.changed` notifications that request caused. A client that has
processed a reply has already seen its consequences; it never needs
to invalidate defensively after its own writes.

### fs.* (writes; opt-in)

The agent is read-only unless started with `--allow-write`; the write
methods are advertised in `meta.hello` only when it is, and
`meta.capabilities.fileSystemAccess.writable` reports the posture.
Writes are confined to the same `--allow-root` sandbox — `--allow-write`
widens *what* may be done to a path, never *which* paths are reachable.

| Method | Params | Result |
| --- | --- | --- |
| `fs.writeFile` | `path`, `content` \| `contentBase64`, `mode?`, `createDirs?`, `ifMtimeNs?` | `{ ok, size, mtime, mtimeNs, mode, entry }`, or `{ ok: false, conflict: true, exists, size, mtime, mtimeNs }` |
| `fs.mkdir` | `path`, `parents?` | `{ ok }` |
| `fs.delete` | `path`, `recursive?` | `{ ok, removed }` |
| `fs.rename` | `from`, `to` | `{ ok }` |
| `fs.copy` | `from`, `to`, `overwrite?` | `{ ok }` |
| `fs.chmod` | `path`, `mode` | `{ ok }` |

`fs.writeFile` is **atomic**: the bytes go to a temp file beside the
target, are `fsync`ed, then `rename`d over it. A crash or a dropped
transport leaves either the old file or the new one, never a truncated
buffer. An existing file's mode is preserved unless `mode` is given,
so saving an executable script does not silently drop its `+x` bit.
The returned `mtime` lets a client track the file without a second
round trip.

`ifMtimeNs` makes the write **conditional**: it happens only if the
file's `mtimeNs` is still that value (`0`: only if the file does not
exist). A mismatch changes nothing and answers `conflict: true` with
the file's current stamp. This is compare-and-swap for "has this
file changed on disk since I read it?": the check an editor makes
before every save costs no round trip of its own, and unlike a
separate stat it cannot race. `entry` is the file's full attributed
entry after the write, so the client's cache needs no follow-up
`fs.stat`.

Authorizing a path that does not exist yet resolves the deepest
component that does and appends the rest, so every existing directory
along the way is symlink-resolved and checked. A symlink inside the
roots therefore cannot become a door out of them, and `fs.rename` /
`fs.copy` authorize **both** ends.

### lsp.* (request-level queries)

Common params: `workspacePath`, `language` (int, see the language
table below), `filePath` (absolute on the host; a relative path is
joined onto `workspacePath`), `fileContent`; positional methods add
`line`, `character`. One language-server session per (workspace,
language), spawned lazily; if no server binary is installed the call
fails `LSP_FAILED`.

Language enum (`GRCLanguage`), used by both the query methods and the
tunnel: `1` rust (rust-analyzer), `2` python (pylsp), `3` javascript /
`4` typescript (typescript-language-server --stdio), `5` go (gopls),
`6` cpp / `7` c (clangd), `8` swift (sourcekit-lsp).

| Method | Extra params | Result |
| --- | --- | --- |
| `lsp.gotoDefinition` | position | `{ locations: [...], source: "server"\|"text", note? }` — `source: "text"` means no server could be started (or it answered nothing, or was still preparing its index past the wait limit) and the locations are ranked declaration-shaped text matches for the identifier under the cursor; `note` says why. Never an error just because a server is missing. |
| `lsp.findReferences` | position, `includeDeclaration?` | `{ locations: [...], source, note? }` — same fallback: whole-word occurrences across the workspace's files of that language. |
| `lsp.hover` | position | `{ hover: { contents, hasRange, range }\|null }` |
| `lsp.documentSymbols` | — | `{ symbols: [...] }` |
| `lsp.workspaceInfo` | `workspacePath`, `language` (no file) | `{ language, workspacePath, serverArguments[], compileCommandsDir\|null, indexStorePath\|null, source: "found"\|"generated"\|"package"\|"server"\|"none", crossFileCapable, notes[] }` — what the agent found or generated so the server can answer cross-file questions (clangd: a `compile_commands.json`, generated via cmake when a configured build dir or CMakeLists.txt exists; sourcekit-lsp: an Xcode DerivedData index store). `notes` say what is missing and how to fix it; a client shows them when results are empty. Starts the server for the workspace if needed. |
| `lsp.workspaceSymbols` | `query` (no filePath/fileContent) | `{ symbols: [...] }` |
| `lsp.foldingRange` | — | `{ ranges: [{ startLine, endLine, hasKind, kind }] }` |

### lsp.tunnel* (raw LSP envelope)

The tunnel is a raw byte pipe to a language server the agent spawns
next to the code — the agent does **not** parse or reframe LSP
messages; LSP framing (`Content-Length` etc.) is the contract between
the two endpoints. This is what lets a real LSP client (eglot, an
IDE) speak its native protocol through the multiplexed channel while
the server runs on the remote box. It coexists with the request-level
`lsp.*` queries above.

| Method | Kind | Params | Result / behavior |
| --- | --- | --- | --- |
| `lsp.tunnelOpen` | request | `workspacePath`, `language` | `{ tunnelId, serverPath }` — spawns the server with cwd = workspacePath, stderr inherited. `LSP_FAILED` if no server binary is installed for the language. |
| `lsp.tunnelSend` | notification | `tunnelId`, `data` (base64), `eof?` | client → server bytes, written to the server's stdin in notification-arrival order. Invalid base64 is dropped (logged). Unknown/closed tunnelId elicits a `lsp.tunnelClosed { reason: "unknown tunnel" }` notification. `eof: true` (with or without `data`) closes the server's stdin once everything queued has been written. |
| `lsp.tunnelClose` | request | `tunnelId` | `{ ok }` (idempotent). Closes the server's stdin and sends SIGTERM; the server's exit then produces `lsp.tunnelClosed`. |

Agent → client:

- `lsp.tunnelRecv { tunnelId, data }` — server → client bytes, base64,
  at most 32 KiB of raw bytes per notification, emitted in stream
  order. The client concatenates decoded chunks and parses LSP framing
  itself; chunk boundaries are arbitrary (they need not align with LSP
  messages).
- `lsp.tunnelClosed { tunnelId, exitCode, reason }` — the server
  exited (`reason` `"exit"` or `"signal <n>"`; `exitCode` is the wait
  status, 128+signal for signals, `null`/`"unknown tunnel"` for sends
  to a nonexistent tunnel).

Byte order is guaranteed in both directions. Back-pressure is bounded
end-to-end: the agent stops reading a chatty server while the
transport is behind (which backs up into the server's stdout pipe),
and the client→server queue is capped at 8 MiB. Clients should split
large writes into chunks well under the negotiated frame cap (32 KiB
is the convention).

### index.*

| Method | Params | Result |
| --- | --- | --- |
| `index.create` | `workspacePath`, `language`, `cacheDBPath` (relative → `$HOME/...`) | `{ indexerId }` |
| `index.run` | `indexerId` | `{ filesIndexed, definitionsFound, ... }`; streams `index.progress` notifications; bulk lane |
| `index.cancel` | notification: `indexerId` | — |
| `index.destroy` | `indexerId` | `{ ok }` (idempotent; refused while running) |

### watch.*

| Method | Params | Result |
| --- | --- | --- |
| `watch.head` | `path` | `{ watchId }` |
| `watch.stop` | notification: `watchId` | — |

The watcher observes the clone's **resolved `.git/HEAD` file**
(worktree `gitdir:` pointers followed): it fires a debounced
`watch.headChanged { watchId }` notification on checkout / branch
switch / rebase / clone-level HEAD rewrites. A plain commit moves the
branch ref, not the HEAD file, and does not fire.

### cred.*

| Method | Params | Result |
| --- | --- | --- |
| `cred.provide` | `credId`, `value` | `{ ok }` (false for an unknown/expired credId) |
| `cred.selftest` | `authOpId`, `prompt` | `{ got, askpassExit }` — full broker + unix-socket + self-exec round trip (not advertised; diagnostic) |

### diffcache.*

Agent-side SQLite cache keyed `(fromSha, toSha, file)`; entries are
immutable so there is no invalidation, only mtime-based pruning.

| Method | Params | Result |
| --- | --- | --- |
| `diffcache.get` | `cacheDir`, `fromSha`, `toSha`, `file` | `{ hit, value? }` |
| `diffcache.put` | `cacheDir`, `fromSha`, `toSha`, `file`, `value` | `{ ok }` |
| `diffcache.prune` | `cacheDir`, `days` | `{ removed }` |

## Notifications emitted by the agent

| Method | Params | When |
| --- | --- | --- |
| `rpc.chunk` | `{ id, seq, last, data }` | response body exceeds the frame cap |
| `index.progress` | `{ indexerId, filePath, current, total }` | during `index.run` (coalesced ≈ per percent) |
| `watch.headChanged` | `{ watchId }` | watched HEAD file changed |
| `cred.request` | `{ authOpId, credId, prompt }` | git prompts during a brokered op |
| `lsp.tunnelRecv` | `{ tunnelId, data, z? }` | server → client bytes on an open tunnel (LSP server or `git.spawn` command) |
| `fs.changed` | see *Directory watches* | a watched directory changed |
| `lsp.tunnelClosed` | `{ tunnelId, exitCode, reason }` | tunnel's server exited (or a send targeted an unknown tunnel) |
