"""Test-side client library for the scrutiny-agent wire protocol.

`client` speaks the protocol documented in docs/protocol.md; `fixtures`
builds hermetic git repositories and file trees to point it at. Both
are stdlib-only so the conformance suite and the pytest suite can
share them.
"""
from .errors import (AgentError, CANCELLED, GIT_FAILED, INTERNAL,
                     INVALID_REQUEST, LSP_FAILED, NOT_FOUND,
                     PERMISSION_DENIED, VERSION_MISMATCH, code_name)
from .client import Agent, Tunnel
from .fixtures import GitRepo, Workspace, git

__all__ = [
    "Agent", "Tunnel", "AgentError", "GitRepo", "Workspace", "git",
    "code_name", "INTERNAL", "NOT_FOUND", "INVALID_REQUEST", "GIT_FAILED",
    "LSP_FAILED", "PERMISSION_DENIED", "VERSION_MISMATCH", "CANCELLED",
]
