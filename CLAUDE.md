# scrutiny-agent

An in-progress performance and security rework of the agent and its
Emacs integration is sitting in this working tree, uncommitted.
**Read `handoff/HANDOFF.md` before doing anything**: it says what the
tree contains, how to build and test it, what was measured, and what
is left to do (starting with validation against a real Teleport link).

Standing constraints from the owner:

- Emacs side: simple configuration, no advice on or redefinition of
  other packages' functions (magit, eglot, TRAMP), very high performance.
  Performance means round trips: count them, do not guess.
- If something looks suspicious, assume it is wrong, prove it with a
  test, and fix it. Never let an operation fall back to TRAMP silently.
- Do not introduce Rust. The agent is C++; tooling is Python/shell/elisp.
