"""Wire error codes (docs/protocol.md, "Error model").

These are the agent's stable contract with every client, so the tests
name them symbolically and assert on the exact integer.
"""

INTERNAL = 1000
NOT_FOUND = 1001
INVALID_REQUEST = 1002
GIT_FAILED = 1003
LSP_FAILED = 1004
PERMISSION_DENIED = 1005
VERSION_MISMATCH = 1006
CANCELLED = 1007

_NAMES = {
    INTERNAL: "INTERNAL",
    NOT_FOUND: "NOT_FOUND",
    INVALID_REQUEST: "INVALID_REQUEST",
    GIT_FAILED: "GIT_FAILED",
    LSP_FAILED: "LSP_FAILED",
    PERMISSION_DENIED: "PERMISSION_DENIED",
    VERSION_MISMATCH: "VERSION_MISMATCH",
    CANCELLED: "CANCELLED",
}


def code_name(code):
    """Human-readable name for an error code, for assertion messages."""
    return "%s(%s)" % (_NAMES.get(code, "UNKNOWN"), code)


class AgentError(Exception):
    """An error response from the agent (or a transport-level failure)."""

    def __init__(self, message, code=None, method=None):
        super().__init__(message)
        self.code = code
        self.method = method

    def __str__(self):
        base = super().__str__()
        if self.code is None:
            return base
        return "%s [%s]" % (base, code_name(self.code))
