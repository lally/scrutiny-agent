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
scrutiny-agent --rpc-stdio [--log <path>] [--log-level <lvl>] [--allow-root <path>]...
```

`--version` prints `<agentVersion> proto <protocolVersion>` and exits.

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

## Methods

Reference of request params → result shape. All `path` params are
absolute; relative cache/install paths anchor under `$HOME`.

### meta.*

| Method | Params | Result |
| --- | --- | --- |
| `meta.hello` | `clientVersion`, `supportedProtocolVersions[]`, `frameCap?` | `{ agentVersion, protocolVersion, frameCap, capabilities[] }` |
| `meta.debug` | `sleepMs?`, `padBytes?` | `{ echoId, pad }` — diagnostics: sleeps (cancellable), pads the result to exercise chunking |
| `meta.stat` | — | `{ lanes: {interactive,normal,bulk}, lspSessions, lspTunnels, uptimeMs, gitParallel, logLevel, agentVersion }` |
| `meta.capabilities` | — | `{ agentVersion, protocolVersion, outboundIO[], fileSystemAccess{}, languageServers{}, network, selftestMethod }` — the agent's security/IO posture |
| `logs.tail` | `maxBytes?` (≤ 1 MiB) | `{ enabled, path?, level?, bytes?, text? }` — tail of the `--log` file; `{ enabled: false }` when logging is off |

### git.* (read surfaces)

| Method | Params | Result |
| --- | --- | --- |
| `git.headSha` | `path` | `{ headSha }` |
| `git.repoMetadata` | `path` | `{ path, isBare, headSha, currentBranch, hasUncommittedChanges }` |
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

### fs.*

| Method | Params | Result |
| --- | --- | --- |
| `fs.readFile` | `path` | `{ content }` (sandboxed; binary-safe emit) |
| `fs.listDirectory` | `path` | `{ path (canonical absolute), entries: [{ name, isDir }] }` sorted by name; symlinks followed for `isDir` |
| `fs.selftest` | — | `{ probe, succeeded, errorCode, firstBytesReadable, sampleSnippet }` — runtime sandbox probe of /etc/passwd |

### lsp.* (request-level queries)

Common params: `workspacePath`, `language` (int, see the language
table below), `filePath`, `fileContent`; positional methods add
`line`, `character`. One language-server session per (workspace,
language), spawned lazily; if no server binary is installed the call
fails `LSP_FAILED`.

Language enum (`GRCLanguage`), used by both the query methods and the
tunnel: `1` rust (rust-analyzer), `2` python (pylsp), `3` javascript /
`4` typescript (typescript-language-server --stdio), `5` go (gopls),
`6` cpp / `7` c (clangd), `8` swift (sourcekit-lsp).

| Method | Extra params | Result |
| --- | --- | --- |
| `lsp.gotoDefinition` | position | `{ locations: [...] }` |
| `lsp.findReferences` | position, `includeDeclaration?` | `{ locations: [...] }` |
| `lsp.hover` | position | `{ hover: { contents, hasRange, range }\|null }` |
| `lsp.documentSymbols` | — | `{ symbols: [...] }` |
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
| `lsp.tunnelSend` | notification | `tunnelId`, `data` (base64) | client → server bytes, written to the server's stdin in notification-arrival order. Invalid base64 is dropped (logged). Unknown/closed tunnelId elicits a `lsp.tunnelClosed { reason: "unknown tunnel" }` notification. |
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
| `lsp.tunnelRecv` | `{ tunnelId, data }` | server → client bytes on an open tunnel |
| `lsp.tunnelClosed` | `{ tunnelId, exitCode, reason }` | tunnel's server exited (or a send targeted an unknown tunnel) |
