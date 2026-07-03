# scrutiny-agent for Emacs

Run eglot's language server on the remote box, tunneled through one
persistent scrutiny-agent channel — instead of a fresh TRAMP
subprocess per server. On links where session establishment is
expensive (Teleport `tsh`, jump hosts, MFA'd SSH), everything rides a
single multiplexed connection with priority lanes and cancellation:
you pay the connection cost once, at connect.

You keep editing via TRAMP exactly as before; only LSP moves onto the
agent channel. rust-analyzer / gopls / clangd / pylsp run next to the
code, with the remote box's cores and RAM.

## Setup

Requires Emacs 29+ (28.1 for `scrutiny-agent.el` alone). The files are
not on (M)ELPA yet; load them from a checkout:

```elisp
(add-to-list 'load-path "/path/to/scrutiny-agent/emacs")
(require 'scrutiny-agent-eglot)

(setq scrutiny-agent-hosts
      '(("devbox" :transport "tsh ssh devbox"
                  :agent-args ("--allow-root" "src"))))

(scrutiny-agent-eglot-setup)
```

Then open a file over TRAMP (e.g. `/ssh:devbox:~/src/proj/main.rs`)
and `M-x eglot`. The first connect probes the remote's OS/arch,
downloads the matching release binary (cached under
`~/.emacs.d/scrutiny-agent/`), streams it over the transport as a
sha256-gated base64 heredoc — no scp, works through anything that
gives you a shell — and execs it. Subsequent connects hash-check and
skip straight to the handshake.

- If the TRAMP host name differs from the configured name, map it via
  `scrutiny-agent-eglot-host-alist`.
- Non-remote buffers (and remote hosts you haven't configured) fall
  through to your normal `eglot-server-programs` entries.
- `:transport` is any shell command whose stdin/stdout reach a POSIX
  shell on the target: `ssh host`, `ssh -J jump host`,
  `tsh ssh host`, `kubectl exec -i pod -- sh`, or plain `sh` for the
  local machine.

## Commands

- `M-x scrutiny-agent-connect` — connect (idempotent; eglot connects
  on demand, so this is mostly for warming up or testing a host).
- `M-x scrutiny-agent-status` — agent load/uptime/tunnel counts.
- `M-x scrutiny-agent-show-logs` — pull the agent-side log tail
  (enable with `:agent-args ("--log" "agent.log" "--log-level" "info")`).
- `M-x scrutiny-agent-disconnect`.

Per-host client-side logs live in `*scrutiny-agent-log[HOST]*`, and
the transport's stderr (ssh/tsh noise, agent startup lines) in
`*scrutiny-agent-stderr[HOST]*`. Errors are never swallowed — a
failed bootstrap shows the full pre-exec transcript.

## Languages

`scrutiny-agent-eglot-languages` maps major modes to the protocol's
language table; the agent resolves each to a server binary on the
remote (see docs/protocol.md): rust-analyzer, pylsp,
typescript-language-server, gopls, clangd, sourcekit-lsp. The server
must be installed on the remote host.

## Development

Point a host at a locally built agent instead of a release:

```elisp
(setq scrutiny-agent-hosts
      '(("local" :transport "sh"
                 :local-binary "~/src/scrutiny-agent/build/agent/scrutiny-agent")))
```

Run the tests (the integration test needs a built agent):

```sh
emacs -Q --batch -L emacs -f batch-byte-compile emacs/*.el
SCRUTINY_AGENT_BIN=$PWD/build/agent/scrutiny-agent \
  emacs -Q --batch -L emacs -l emacs/scrutiny-agent-tests.el \
  -f ert-run-tests-batch-and-exit
```

## Current limits

- LSP, git, and file reads go through the agent; **saving files still
  goes through TRAMP** (the agent's fs surface is read-only today).
- The agent's lifetime is the connection's: a disconnect tears down
  tunneled servers (eglot restarts them on reconnect).
- Language servers must already be installed on the remote host.
