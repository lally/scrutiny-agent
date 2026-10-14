# scrutiny-agent for Emacs

Remote development over **one** connection. You pay the cost of
establishing a session once — at connect — and then LSP, git, and file
reads all ride the same multiplexed channel with priority lanes and
cancellation. On links where session setup is expensive (Teleport
`tsh`, jump hosts, MFA'd SSH), that is the whole difference.

Nothing here asks you to learn a new interface. eglot, magit, `M-.`,
eldoc, and imenu keep working exactly as they do — they just stop
opening a connection per operation.

## Setup

Requires Emacs 29+ (28.1 for `scrutiny-agent.el` alone).

The easiest path is the release package, which carries both Linux
agent binaries inside it — so the first connect to a server needs no
further download:

```
M-x package-install-file RET scrutiny-agent-<version>.tar RET
```

Then the four lines below. Or load from a checkout:

```elisp
(add-to-list 'load-path "/path/to/scrutiny-agent/emacs")
(require 'scrutiny-agent-remote)

(scrutiny-agent-remote-setup
 '(("devbox" :transport "tsh ssh devbox sh"
             :roots ("src" "work"))))

(global-set-key (kbd "C-c s") scrutiny-agent-command-map)
```

That is the whole configuration. It registers the host, derives the
agent's argv from `:roots`, and turns on the file accelerator, magit
routing, eglot tunneling, and the xref/eldoc/imenu backends.

Then use Emacs exactly as you always have — `C-x C-f
/ssh:devbox:~/src/proj/main.py`, `C-x C-s`, `M-x magit-status`, `M-.`,
`M-x dired` — and all of it rides one connection.

`M-x scrutiny-agent-remote-connect` warms the connection at the start
of a session (the expensive part of a Teleport link is establishing
it). `M-x scrutiny-agent-remote-status` shows what is actually active,
and `M-x scrutiny-agent-fs-benchmark` measures it against TRAMP.

If you would rather wire the pieces up yourself:

```elisp
(setq scrutiny-agent-hosts
      '(("devbox" :transport "tsh ssh devbox sh"
                  :agent-args ("--allow-root" "src" "--allow-write"
                               "--git-exec-preset" "magit"
                               "--log" "agent.log" "--log-level" "info"))))

(require 'scrutiny-agent-fs)     (scrutiny-agent-fs-mode 1)     ; files
(require 'scrutiny-agent-eglot)  (scrutiny-agent-eglot-setup)   ; LSP
(require 'scrutiny-agent-magit)  (scrutiny-agent-magit-mode 1)  ; git
(require 'scrutiny-agent-ui)                                    ; commands
```

`:transport` is any shell command whose stdin/stdout reach a POSIX
shell on the target: `ssh host`, `ssh -J jump host`, `tsh ssh host sh`,
`kubectl exec -i pod -- sh`, or plain `sh` for the local machine.

The shell must be on plain pipes, not a terminal. Teleport nodes run a
plain `tsh ssh host` session in a PTY whatever the client asked for
(`--no-tty` included), and so does OpenSSH through `tsh proxy ssh`;
an explicit remote command (`tsh ssh host sh`) is an exec request and
gets pipes. The client notices a terminal from the echo of its first
probe and fails at once, naming the transport that works.

### Getting the agent onto the remote

The first connect runs a fixed six-step dialogue over the transport —
no scp, no second channel, works through anything that gives you a
shell:

1. **Probe** with `uname -s` and `uname -m` (POSIX flags only).
   Spellings are normalized, so `amd64` and `x86_64` are the same
   thing, as are `arm64` and `aarch64`.
2. **Check** the sha256 of whatever binary is already installed.
3. **Install** — only on a mismatch — by streaming the right binary as
   a base64 heredoc, hash-gated, then renamed into place atomically.
4. **Verify** by running `--version` on the far end. This is the step
   that turns "it hangs" into a sentence: a wrong architecture, a
   failed dynamic link, or a truncated transfer is reported
   immediately, with the shell's own words, instead of a timeout at
   the handshake.
5. **Install the config**, hash-gated the same way.
6. **Hand over** to the agent.

Later connects hash-check, verify, and go straight to the handshake.
The whole pre-exec transcript is kept in `*scrutiny-agent-log[HOST]*`,
so when something does go wrong you can read exactly how far it got.

It streams the binary **from the client**, so a Mac driving a Linux
server needs a *Linux* binary locally. Two ways:

- **Install the release package**, which bundles both. This is the
  default path and needs nothing else; the binaries live in `bin/`
  inside the installed package and are found before anything is
  downloaded.

- **Build both locally** (no network, no release needed):

  ```sh
  scripts/build-agents.sh
  ```

  Builds `linux/amd64` and `linux/arm64` in a container and installs
  them into `~/.emacs.d/scrutiny-agent/`. The client picks the one
  matching each host's probed architecture, so one Emacs drives both
  x86 and ARM servers with nothing further to configure. Add
  `--arch amd64` for just one; it checks Docker and the qemu
  registration first rather than failing partway through a long build.

- **Cut a release**. `git tag v0.2.0 && git push origin v0.2.0` builds
  and publishes both; the client downloads the right one on first
  connect and caches it in the same directory.
  `scrutiny-agent-binary-version` selects which release to fetch, and
  `scrutiny-agent-download-releases` set to nil keeps connects offline.

- **Point at specific files**: `:local-binary "/path/to/agent"` for one
  host, or an alist to pick by architecture:

  ```elisp
  :local-binary '(("x86_64"  . "~/agents/scrutiny-agent-0.2.0-x86_64")
                  ("aarch64" . "~/agents/scrutiny-agent-0.2.0-aarch64"))
  ```

`M-x eval-expression (scrutiny-agent-installed-binaries)` lists what
you have locally.

### The config sent to the host

During bootstrap the client also streams an agent config file to
`<install-dir>/scrutiny-agent.conf` and passes it as `--config`. It is
hash-gated like the binary, so reconnecting costs an echo rather than
a transfer, and it is only rewritten when the client's own default
changes — edit it on the server and your edits stay.

`scrutiny-agent-remote-setup` generates it from `:roots` and the
options below. Otherwise `scrutiny-agent-default-config` is the
content, a host's `:config` overrides that for one host, and `:config
nil` sends none. `:agent-args` still applies, on top of whichever file
is used.

One thing to know: the default config applies to **every** host you
have not overridden. If you want a host with a narrower git allowlist
than your default, set its `:config` explicitly rather than relying on
`:agent-args` to tighten it — they compose, they do not replace.

## What you get

### Files — find-file, save, dired, completion

`scrutiny-agent-fs-mode` installs a `file-name-handler-alist` entry
**ahead of TRAMP's** that matches only your configured hosts. It
serves `find-file`, `save-buffer`, `dired`, `directory-files`,
minibuffer completion and the rest over the agent; anything it does
not implement is handed straight back to TRAMP, so an unimplemented
corner behaves exactly as it does today.

What things cost, in agent round trips — which, on a slow link, is
the only cost there is:

| operation | round trips |
| --- | --- |
| `C-x C-f` (file, its directory and its ancestors are asked for at once) | 1 |
| `C-x C-f`, another file in a directory already seen | 1 |
| first keystroke in a buffer (lock check) | 0 |
| `C-x C-s` | 1 |
| `C-x C-s`, first save of a session with backups on | 2 |
| `dired` of a directory already seen | 0 |
| minibuffer completion in a directory already seen | 0 |
| `file-exists-p` / `file-attributes` in a directory already seen | 0 |

How it gets there:

- **Requests are pipelined.** Everything a command is known to need
  is sent before the first reply is awaited, so N questions cost one
  round trip, not N. Opening a file sends the read, the listing of
  its directory, and the listings of the directories above it
  together.
- **A listing is a proof.** One attributed `fs.listDirectory` answers
  every later question about names in that directory — including "it
  does not exist", which is most of what Emacs asks (`.dir-locals.el`,
  `.git`, `.editorconfig`, backup and lock names, at every level).
- **The agent keeps the listing true.** With
  `scrutiny-agent-fs-watch` (default on) the agent watches each
  directory it has listed and pushes changes, so a cached listing
  never expires and is never wrong: a file someone else creates shows
  up without a round trip being spent to look. Where the agent cannot
  watch (network filesystems, macOS hosts, very busy directories)
  listings fall back to `scrutiny-agent-fs-cache-ttl`.
- **Saving is one conditional write.** "Has this file changed on disk
  since I read it?" travels *with* the write (`ifMtimeNs`) rather
  than as a stat before it, and the reply carries the file's new
  attributes. If the file did change, nothing is written and you get
  the usual "changed on disk; really edit?" question.
- **Replies are compressed** (`scrutiny-agent-compress`).
- **`vc-registered` answers `nil` without asking**, and nothing is
  asked about paths outside the agent's roots (below).
- **Nothing falls back silently.** An operation the agent cannot
  serve goes to TRAMP and is counted;
  `M-x scrutiny-agent-fs-status` shows the counts. An ordinary editing
  session should show none that open a TRAMP connection.

`scrutiny-agent-fs-roots-are-the-world` (on by default) says the
agent's roots are everything visible on that host: directories on the
way down to a root exist, and nothing else does. It makes the walk
above free. The tradeoff is that a project root *above* your
configured roots becomes invisible rather than merely slow — the fix
is to add it to `:roots`, or set the option to nil to have such paths
answered by TRAMP, truthfully and slowly.

Saving needs the agent started with `--allow-write` (the generated
config has it). Without it writes fall back to TRAMP, and
`scrutiny-agent` warns at connect time if the agent came up without
the configuration this client sent it — that used to happen silently
and made everything slow.

Worth checking once: `M-x scrutiny-agent-remote-status` will tell you
if the handler ended up *behind* TRAMP's, which leaves everything
working and nothing accelerated.

### LSP — eglot, unchanged

Open a file over TRAMP (`/ssh:devbox:~/src/proj/main.rs`) and `M-x
eglot`. rust-analyzer / gopls / clangd / pylsp run next to the code,
with the remote box's cores and RAM, tunneled through the agent
channel instead of a fresh TRAMP subprocess per server.

Map TRAMP host names that differ from the configured name via
`scrutiny-agent-eglot-host-alist`. Non-remote buffers and
unconfigured hosts fall through to your normal `eglot-server-programs`.

### git — magit, unchanged

`scrutiny-agent-magit-mode` runs magit's git on the agent. It does not
advise or redefine anything in magit: git reaches the agent through
the same `process-file` / `start-file-process` file-name-handler
operations that TRAMP itself implements, so it applies equally to
`vc`, `project.el`, `diff-hl` and anything else that runs git in a
remote directory, and a magit upgrade has nothing to break.

- **Synchronous git** (everything a status refresh runs) goes through
  `git.exec`, with stdin and the environment (`GIT_INDEX_FILE` …)
  carried over.
- **A refresh is one round trip, not fifty.** magit asks its
  questions one at a time and waits for each answer. The read-only
  ones are almost always the same ones as last time, so when a
  command starts, the batch it ran before is sent again at once, and
  magit's own sequential calls are then answered from replies already
  on their way. Nothing is ever *shown* from the previous run — only
  the questions are predicted, never the answers — and a mutating
  command is never speculated. The first refresh of a repository in a
  brand-new Emacs has nothing to go on; the batch is remembered in
  `scrutiny-agent-magit-history-file` for the next session.
  `scrutiny-agent-magit-speculate` turns this off.
- **Asynchronous git** — staging a hunk, commit, rebase, push, pull,
  fetch — runs on the agent through `git.spawn`. Emacs gets a real
  process object, so magit's process buffer, sentinels and
  `with-editor` work unchanged: `c c` opens `COMMIT_EDITMSG` over the
  agent and `C-c C-c` finishes the commit, with no TRAMP connection
  involved. `scrutiny-agent-magit-async` turns this off (async git
  then goes through TRAMP as before).

Requires the remote agent to be started with an allowlist
(`--git-exec-preset magit`, or `--git-exec <subcommand>` repeated);
`scrutiny-agent-remote-setup` does that. `M-x
scrutiny-agent-magit-check` reports what a given host will and will
not run.

- A command the allowlist refuses **falls back to TRAMP**, so a narrow
  allowlist costs speed, never function. Set
  `scrutiny-agent-magit-fallback` to nil while tuning one, to see what
  is missing instead of silently working around it.
- `scrutiny-agent-magit-log-commands` records every routed command in
  `*scrutiny-agent-log[HOST]*` — the practical way to build a minimal
  allowlist.

### Code navigation — xref, eldoc, imenu

`M-x scrutiny-agent-code-mode` registers the agent as an xref backend,
an eldoc function, and an imenu index, so `M-.`, `M-?`, `C-M-.`,
eldoc, and `imenu` answer from the remote language server:

```elisp
(add-hook 'python-ts-mode-hook #'scrutiny-agent-code-mode)
```

This is the lightweight path, for reading code you have not opened a
project for — one question, one answer, no session that outlives it.
For actually editing remote code use eglot, which gets completion,
diagnostics, and formatting as well.

`scrutiny-agent-xref-open-function` decides how a cross-reference
opens its target: through TRAMP (default, editable) or over the agent
(read-only, no TRAMP setup).

### Everything else — the command map

`M-x scrutiny-agent-menu` (a transient), or `C-c s` with the binding
above:

| Key | Command | |
| --- | --- | --- |
| `c` `q` | connect / disconnect | |
| `i` | `scrutiny-agent-info` | version, lanes, sandbox roots, capabilities |
| `p` | `scrutiny-agent-ping` | round-trip time, and a chunked-stream check |
| `L` | `scrutiny-agent-remote-logs` | the agent's own log tail |
| `b` `f` | browse / view file | remote tree over the agent channel |
| `s` `r` `l` | status / branches / log | agent-native views (magit is better; these need no TRAMP) |
| `d` `D` | unstaged / staged diff | |
| `F` `C` | fetch / clone | credentials brokered per prompt, never stored remotely |
| `w` `W` | watch / unwatch HEAD | fires on checkout, branch switch, rebase |
| `x` | `scrutiny-agent-index` | build a symbol index on the bulk lane |
| `e` | `scrutiny-agent-code-mode` | xref/eldoc/imenu here |
| `v` | `scrutiny-agent-verify` | **exercise every operation and report** |

### Verifying a host

`M-x scrutiny-agent-verify` drives every capability the connected
agent advertises and reports PASS / FAIL / SKIP per operation, ending
with any capability it did not know how to exercise. It is read-only
unless given a prefix argument, in which case fetch, checkout, clone,
and indexing run too — against scratch paths, except the checkout,
which re-checks-out the branch already current so your working tree
does not move.

This is the answer to "I just set up a new host; does it work?"

## Diagnostics

- `*scrutiny-agent-log[HOST]*` — client-side log, including the full
  pre-exec bootstrap transcript. Errors are never swallowed.
- `*scrutiny-agent-stderr[HOST]*` — the transport's stderr (ssh/tsh
  noise, agent startup lines).
- `M-x scrutiny-agent-remote-logs` — the agent's own log (needs
  `--log` in `:agent-args`).
- `M-x scrutiny-agent-credential-selftest` — drives the whole
  credential broker without needing a remote that demands auth.

## Files

| File | |
| --- | --- |
| `scrutiny-agent.el` | the wire client: framing, `rpc.chunk`, cancellation, credential broker, LSP tunnels, bootstrap |
| `scrutiny-agent-ops.el` | one typed wrapper per protocol method |
| `scrutiny-agent-fs.el` | the file-name handler: find-file, save, dired, completion |
| `scrutiny-agent-remote.el` | one-call setup, status and benchmarking |
| `scrutiny-agent-eglot.el` | eglot over the raw-LSP tunnel |
| `scrutiny-agent-magit.el` | git (magit, vc, project.el) over `git.exec` / `git.spawn` |
| `scrutiny-agent-xref.el` | xref / eldoc / imenu backends |
| `scrutiny-agent-ui.el` | interactive commands |
| `scrutiny-agent-menu.el` | the transient menu, loaded on demand |
| `scrutiny-agent-verify.el` | drives every advertised capability |

## Languages

`scrutiny-agent-eglot-languages` maps major modes to the protocol's
language table; the agent resolves each to a server binary on the
remote (see `docs/protocol.md`): rust-analyzer, pylsp,
typescript-language-server, gopls, clangd, sourcekit-lsp. The server
must already be installed on the remote host.

## Development

Point a host at a locally built agent instead of a release:

```elisp
(setq scrutiny-agent-hosts
      '(("local" :transport "sh"
                 :local-binary "~/src/scrutiny-agent/build/agent/scrutiny-agent"
                 :agent-args ("--git-exec-preset" "magit"))))
```

Tests (the integration ones need a built agent; they use a local `sh`
transport, so no sshd is involved):

```sh
emacs -Q --batch -L emacs -f batch-byte-compile emacs/*.el && rm -f emacs/*.elc

SCRUTINY_AGENT_BIN=$PWD/build/agent/scrutiny-agent \
  emacs -Q --batch -L emacs -l emacs/scrutiny-agent-tests.el \
  -f ert-run-tests-batch-and-exit          # wire client

SCRUTINY_AGENT_BIN=$PWD/build/agent/scrutiny-agent \
  emacs -Q --batch -L emacs -l emacs/scrutiny-agent-ui-tests.el \
  -f ert-run-tests-batch-and-exit          # ops / ui / xref / verify

SCRUTINY_AGENT_BIN=$PWD/build/agent/scrutiny-agent \
  emacs -Q --batch -L emacs -l emacs/scrutiny-agent-magit-tests.el \
  -f ert-run-tests-batch-and-exit          # magit routing

SCRUTINY_AGENT_BIN=$PWD/build/agent/scrutiny-agent \
  emacs -Q --batch -L emacs -l emacs/scrutiny-agent-fs-tests.el \
  -f ert-run-tests-batch-and-exit          # file handler
```

### Linting

```sh
scripts/lint-elisp.sh          # byte-compile, relint, package-lint, checkdoc
scripts/lint-elisp.sh --elsa   # ...and Elsa, against emacs/.elsa-baseline
```

The first four are fatal on any finding except checkdoc's. Elsa is
gated on a per-file baseline instead: a file whose count goes *up*
fails, and `--elsa-update` rewrites the baseline once a fix brings one
down. The standing count is not zero because some of it is Elsa's own
blind spots — it cannot see `cl-defstruct` accessors, the first
argument of `funcall`, or a value assigned from inside a closure, and
it reads a `defcustom` default as a constant.

Several idioms in this code exist to keep it analysable, and each cut
real findings: plain `defun` with `&optional` rather than `cl-defun`
(Elsa cannot read a `cl-defun` arglist even in the same file); an
explicit lambda at the call site rather than a macro that binds
variables for its body; `let` plus `when` rather than `when-let`, whose
binding list is analysed as if it were a call; top-level helpers rather
than `cl-flet`. Worth knowing before writing something that trips them
again.

The transient menu lives in its own file for the same reason:
`(require 'transient)` alone costs Elsa more than five minutes, so a
file that requires it is never analysed. Keeping it to forty
declarative lines leaves the 1600 lines of `scrutiny-agent-ui.el` and
`scrutiny-agent-verify.el` inside the analyser. The full list, with numbers, is in the note at the bottom of
`scripts/lint-elisp.sh`.

## Current limits

- Shell commands in a remote directory (`M-x shell-command`,
  `compile`, `grep`) still go through TRAMP: the agent has no general
  process surface, only allowlisted git.
- `executable-find` with its REMOTE argument finds nothing on an agent
  host: the `exec-path` it asks for is answered empty (nothing on the
  remote `$PATH` is inside the roots, and TRAMP's answer would open a
  connection). Programs run through `process-file` are unaffected.
- Directory watches are Linux-only (inotify) and are not used on
  network filesystems. There, a change made by someone *else* can be
  up to `scrutiny-agent-fs-cache-ttl` seconds stale; reverting a
  buffer or `M-x scrutiny-agent-fs-flush-cache` clears it.
- A file's *contents* are never cached beyond the prefetch that opens
  it; `auto-revert` sees changes through the watched listing.
- Asynchronous git needs `sh`, `cat` and `mkfifo` on the machine
  running Emacs (any Unix; not native Windows).
- The very first magit refresh of a repository, in an Emacs that has
  never seen it, is still sequential.
- Files larger than `scrutiny-agent-fs-max-inline-bytes` (8 MiB) are
  handed to TRAMP, which can stream them rather than holding the whole
  thing in a base64 payload.
- The agent's lifetime is the connection's: a disconnect tears down
  tunneled servers (eglot restarts them on reconnect).
- Language servers must already be installed on the remote host.
