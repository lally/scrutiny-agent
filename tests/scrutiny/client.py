"""A complete client for one `scrutiny-agent --rpc-stdio` process.

Implements everything docs/protocol.md asks of a client:

  * LSP-style `Content-Length` framing on stdio,
  * JSON-RPC 2.0 requests / responses / notifications,
  * `rpc.chunk` reassembly for over-cap response bodies,
  * `$/cancelRequest`,
  * the `cred.request` -> `cred.provide` credential broker round trip,
  * the `lsp.tunnel*` raw-LSP envelope, with LSP re-framing on top so
    tests can talk to a real language server through the tunnel.

Everything is bounded: every wait takes a deadline, so a hung agent
fails a test instead of hanging the run. stderr is drained by a thread
(an undrained stderr pipe deadlocks a chatty agent once the pipe
buffer fills) and kept for assertions and failure output.
"""
import base64
import json
import os
import subprocess
import threading
import time

from .errors import AgentError

DEFAULT_TIMEOUT = 30.0


class Tunnel:
    """One `lsp.tunnel*` channel, with LSP framing layered on top.

    The agent pipes raw bytes both ways and explicitly does not align
    `lsp.tunnelRecv` chunk boundaries with LSP message boundaries, so
    this reassembles the stream itself.
    """

    def __init__(self, agent, tunnel_id, server_path):
        self.agent = agent
        self.id = tunnel_id
        self.server_path = server_path
        self.raw = b""                 # every byte the server has sent
        self.messages = []             # parsed LSP messages, in order
        self.closed = None             # the lsp.tunnelClosed params
        self._parsed_upto = 0
        self._lock = threading.Lock()
        self._cv = threading.Condition(self._lock)

    # -- server -> client ------------------------------------------------
    def _feed(self, data):
        with self._cv:
            self.raw += data
            self._parse_locked()
            self._cv.notify_all()

    def _parse_locked(self):
        buf = self.raw[self._parsed_upto:]
        consumed = 0
        while True:
            hdr_end = buf.find(b"\r\n\r\n", consumed)
            if hdr_end < 0:
                break
            length = None
            for line in buf[consumed:hdr_end].split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":", 1)[1].strip())
            if length is None:
                # Not an LSP header block; skip past it rather than spin.
                consumed = hdr_end + 4
                continue
            body_end = hdr_end + 4 + length
            if len(buf) < body_end:
                break
            try:
                self.messages.append(json.loads(buf[hdr_end + 4:body_end]))
            except ValueError:
                pass
            consumed = body_end
        self._parsed_upto += consumed

    def _on_closed(self, params):
        with self._cv:
            self.closed = params
            self._cv.notify_all()

    # -- client -> server ------------------------------------------------
    def send_bytes(self, data, chunk=32 * 1024):
        """Write raw bytes to the server's stdin, split into chunks."""
        for i in range(0, len(data), chunk):
            part = data[i:i + chunk]
            self.agent.notify("lsp.tunnelSend",
                              {"tunnelId": self.id,
                               "data": base64.b64encode(part).decode()})

    def send_lsp(self, payload, split_at=None):
        """Frame and send one LSP message.

        `split_at` sends the frame as two `lsp.tunnelSend` notifications
        cut at that byte offset, exercising the documented guarantee
        that chunk boundaries need not be message boundaries.
        """
        body = json.dumps(payload).encode()
        frame = b"Content-Length: %d\r\n\r\n" % len(body) + body
        if split_at is None:
            self.send_bytes(frame)
        else:
            self.send_bytes(frame[:split_at])
            self.send_bytes(frame[split_at:])

    # -- waiting ---------------------------------------------------------
    def wait_response(self, msg_id, timeout=DEFAULT_TIMEOUT):
        """Wait for the LSP response with id `msg_id`."""
        return self._wait(
            lambda: next((m for m in self.messages
                          if m.get("id") == msg_id and "method" not in m),
                         None),
            timeout, "LSP response id=%s" % msg_id)

    def wait_message(self, predicate, timeout=DEFAULT_TIMEOUT, what="message"):
        return self._wait(
            lambda: next((m for m in self.messages if predicate(m)), None),
            timeout, what)

    def wait_closed(self, timeout=DEFAULT_TIMEOUT):
        return self._wait(lambda: self.closed, timeout,
                          "lsp.tunnelClosed for %s" % self.id)

    def _wait(self, probe, timeout, what):
        deadline = time.monotonic() + timeout
        with self._cv:
            while True:
                got = probe()
                if got is not None:
                    return got
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AgentError(
                        "timeout (%.1fs) waiting for %s on tunnel %s "
                        "(%d bytes, %d messages, closed=%r)"
                        % (timeout, what, self.id, len(self.raw),
                           len(self.messages), self.closed))
                self._cv.wait(min(remaining, 0.25))

    def close(self, timeout=DEFAULT_TIMEOUT):
        return self.agent.call("lsp.tunnelClose", {"tunnelId": self.id},
                               timeout=timeout)


class Agent:
    """A live `scrutiny-agent --rpc-stdio` process plus reader threads."""

    def __init__(self, binary, args=(), env=None, cwd=None,
                 auto_hello=True, client_version="pytest",
                 frame_cap=131072):
        self.binary = binary
        self.argv = [binary, "--rpc-stdio", *args]
        self.proc = subprocess.Popen(
            self.argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, cwd=cwd,
            env=dict(os.environ, **(env or {})))

        self._write_lock = threading.Lock()
        self._state_lock = threading.Lock()
        self._cv = threading.Condition(self._state_lock)
        self._next_id = 0
        self._pending = {}          # id -> {"event", "message"}
        self._chunks = {}           # id -> {seq: b64 data}
        self._tunnels = {}          # tunnelId -> Tunnel
        self._secrets = {}          # authOpId -> secret
        self.cred_requests = []     # every cred.request seen
        self.notifications = []     # (method, params), except rpc.chunk
        self.null_id_errors = []    # error envelopes carrying id: null
        self.stderr = bytearray()
        self.transport_error = None

        self._reader = threading.Thread(target=self._read_loop, daemon=True,
                                        name="agent-stdout")
        self._reader.start()
        self._errreader = threading.Thread(target=self._stderr_loop,
                                           daemon=True, name="agent-stderr")
        self._errreader.start()

        self.hello = None
        if auto_hello:
            self.hello = self.handshake(client_version=client_version,
                                        frame_cap=frame_cap)

    # -- lifecycle -------------------------------------------------------
    def handshake(self, client_version="pytest", versions=(1,),
                  frame_cap=None, timeout=DEFAULT_TIMEOUT):
        params = {"clientVersion": client_version,
                  "supportedProtocolVersions": list(versions)}
        if frame_cap is not None:
            params["frameCap"] = frame_cap
        self.hello = self.call("meta.hello", params, timeout=timeout)
        return self.hello

    @property
    def capabilities(self):
        return list((self.hello or {}).get("capabilities", []))

    @property
    def agent_version(self):
        return (self.hello or {}).get("agentVersion")

    @property
    def frame_cap(self):
        return (self.hello or {}).get("frameCap")

    def alive(self):
        return self.proc.poll() is None and self.transport_error is None

    def close(self, timeout=5.0):
        """Terminate the agent and reap it, without leaking the process."""
        if self.proc.poll() is None:
            try:
                self.proc.terminate()
            except OSError:
                pass
            try:
                self.proc.wait(timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout)
        for stream in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
            try:
                if stream:
                    stream.close()
            except OSError:
                pass
        with self._cv:
            self._cv.notify_all()

    def stderr_text(self):
        return bytes(self.stderr).decode("utf-8", "replace")

    # -- framing ---------------------------------------------------------
    def send_frame(self, body):
        """Send one frame with an arbitrary (possibly invalid) body."""
        if isinstance(body, str):
            body = body.encode()
        with self._write_lock:
            try:
                self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body))
                self.proc.stdin.write(body)
                self.proc.stdin.flush()
            except (BrokenPipeError, ValueError, OSError) as exc:
                raise AgentError("agent stdin unusable: %s (stderr: %s)"
                                 % (exc, self.stderr_text()[-2000:]))

    def send_raw(self, data):
        """Send bytes verbatim -- no framing added (header fuzzing)."""
        if isinstance(data, str):
            data = data.encode()
        with self._write_lock:
            try:
                self.proc.stdin.write(data)
                self.proc.stdin.flush()
            except (BrokenPipeError, ValueError, OSError) as exc:
                raise AgentError("agent stdin unusable: %s" % exc)

    def _send_json(self, obj):
        self.send_frame(json.dumps(obj).encode())

    # -- reader ----------------------------------------------------------
    def _read_exact(self, n):
        out = b""
        while len(out) < n:
            part = self.proc.stdout.read(n - len(out))
            if not part:
                return None
            out += part
        return out

    def _read_frame(self):
        header = b""
        while b"\r\n\r\n" not in header:
            byte = self.proc.stdout.read(1)
            if not byte:
                return None
            header += byte
            if len(header) > 8192:
                raise AgentError("frame header exceeded 8 KiB: %r" % header[:200])
        length = None
        for line in header.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":", 1)[1].strip())
        if length is None:
            raise AgentError("frame header without Content-Length: %r" % header)
        body = self._read_exact(length)
        if body is None:
            return None
        return json.loads(body)

    def _read_loop(self):
        try:
            while True:
                message = self._read_frame()
                if message is None:
                    break
                self._dispatch(message)
        except Exception as exc:                      # noqa: BLE001
            self.transport_error = exc
        finally:
            if self.transport_error is None:
                self.transport_error = AgentError("agent stdout closed")
            with self._cv:
                for slot in self._pending.values():
                    slot["event"].set()
                self._cv.notify_all()

    def _stderr_loop(self):
        try:
            while True:
                chunk = self.proc.stderr.read(4096)
                if not chunk:
                    break
                self.stderr.extend(chunk)
        except (OSError, ValueError):
            pass

    def _dispatch(self, message):
        method = message.get("method")
        if method == "rpc.chunk":
            params = message["params"]
            with self._cv:
                self._chunks.setdefault(params["id"], {})[params["seq"]] = \
                    params["data"]
                # Also logged as a notification so tests can assert on
                # the streaming behavior itself (seq ordering, per-chunk
                # size), not only on the reassembled result.
                self.notifications.append((method, params))
                self._cv.notify_all()
            return
        if method is not None:
            self._dispatch_notification(method, message.get("params") or {})
            return
        if message.get("id") is None and "error" in message:
            with self._cv:
                self.null_id_errors.append(message["error"])
                self._cv.notify_all()
            return
        self._complete(message)

    def _dispatch_notification(self, method, params):
        with self._cv:
            self.notifications.append((method, params))
            self._cv.notify_all()
        if method == "cred.request":
            self.cred_requests.append(params)
            self._answer_cred(params)
        elif method == "lsp.tunnelRecv":
            tunnel = self._tunnels.get(params.get("tunnelId"))
            if tunnel is not None:
                tunnel._feed(base64.b64decode(params["data"]))
        elif method == "lsp.tunnelClosed":
            tunnel = self._tunnels.get(params.get("tunnelId"))
            if tunnel is not None:
                tunnel._on_closed(params)

    def _complete(self, message):
        msg_id = message.get("id")
        result = message.get("result")
        if isinstance(result, dict) and result.get("streamed") is True:
            with self._cv:
                parts = self._chunks.pop(msg_id, {})
            blob = "".join(parts[seq] for seq in sorted(parts))
            try:
                message = json.loads(base64.b64decode(blob))
            except (ValueError, TypeError) as exc:
                message = {"id": msg_id,
                           "error": {"code": -1,
                                     "message": "chunk reassembly failed: %s"
                                                % exc}}
            msg_id = message.get("id")
        with self._cv:
            slot = self._pending.get(msg_id)
            if slot is not None:
                slot["message"] = message
                slot["event"].set()
            self._cv.notify_all()

    # -- credentials -----------------------------------------------------
    def expect_secret(self, auth_op_id, secret):
        """Answer the next cred.request for `auth_op_id` with `secret`."""
        self._secrets[auth_op_id] = secret

    def _answer_cred(self, params):
        secret = self._secrets.get(params.get("authOpId"), "")
        self.call_async("cred.provide",
                        {"credId": params.get("credId"), "value": secret})

    # -- requests --------------------------------------------------------
    def call_async(self, method, params=None, lane=None):
        """Send a request; return (id, wait_callable)."""
        if not self.alive():
            raise AgentError("agent is not running (%s); stderr: %s"
                             % (self.transport_error,
                                self.stderr_text()[-2000:]))
        with self._cv:
            self._next_id += 1
            msg_id = self._next_id
            slot = {"event": threading.Event(), "message": None}
            self._pending[msg_id] = slot
        payload = {"jsonrpc": "2.0", "id": msg_id, "method": method,
                   "params": params if params is not None else {}}
        if lane is not None:
            payload["lane"] = lane
        self._send_json(payload)

        def wait(timeout=DEFAULT_TIMEOUT):
            if not slot["event"].wait(timeout):
                raise AgentError("timeout (%.1fs) waiting for %s"
                                 % (timeout, method), method=method)
            with self._cv:
                self._pending.pop(msg_id, None)
            message = slot["message"]
            if message is None:
                raise AgentError(
                    "agent died during %s: %s (stderr: %s)"
                    % (method, self.transport_error,
                       self.stderr_text()[-2000:]), method=method)
            if "error" in message:
                err = message["error"]
                raise AgentError("%s: %s" % (method, err.get("message")),
                                 code=err.get("code"), method=method)
            return message.get("result")

        return msg_id, wait

    def call(self, method, params=None, timeout=DEFAULT_TIMEOUT, lane=None):
        _, wait = self.call_async(method, params, lane=lane)
        return wait(timeout)

    def call_expect_error(self, method, params=None, timeout=DEFAULT_TIMEOUT):
        """Call `method` expecting failure; return the AgentError."""
        try:
            result = self.call(method, params, timeout=timeout)
        except AgentError as exc:
            if exc.code is None:
                raise
            return exc
        raise AssertionError("%s unexpectedly succeeded: %r" % (method, result))

    def notify(self, method, params=None):
        self._send_json({"jsonrpc": "2.0", "method": method,
                         "params": params if params is not None else {}})

    def cancel(self, request_id):
        self.notify("$/cancelRequest", {"id": request_id})

    # -- notifications ---------------------------------------------------
    def wait_notification(self, method, predicate=None,
                          timeout=DEFAULT_TIMEOUT, since=0):
        """Wait for a notification `method` matching `predicate`.

        `since` skips notifications recorded before that index, so a
        test can ignore traffic from an earlier phase.
        """
        deadline = time.monotonic() + timeout
        with self._cv:
            while True:
                for m, p in self.notifications[since:]:
                    if m == method and (predicate is None or predicate(p)):
                        return p
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self._cv.wait(min(remaining, 0.25))

    def notification_count(self, method=None):
        with self._cv:
            if method is None:
                return len(self.notifications)
            return sum(1 for m, _ in self.notifications if m == method)

    def notification_mark(self):
        """Current notification-log length, for use as `since`."""
        with self._cv:
            return len(self.notifications)

    def wait_null_id_error(self, timeout=DEFAULT_TIMEOUT, since=0):
        deadline = time.monotonic() + timeout
        with self._cv:
            while True:
                if len(self.null_id_errors) > since:
                    return self.null_id_errors[since]
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self._cv.wait(min(remaining, 0.25))

    # -- tunnels ---------------------------------------------------------
    def tunnel_open(self, workspace, language, timeout=60.0):
        """Open an LSP tunnel and register it for stream reassembly."""
        result = self.call("lsp.tunnelOpen",
                           {"workspacePath": workspace, "language": language},
                           timeout=timeout)
        tunnel = Tunnel(self, result["tunnelId"], result.get("serverPath"))
        self._tunnels[tunnel.id] = tunnel
        # Bytes may already have arrived before registration.
        for method, params in list(self.notifications):
            if method == "lsp.tunnelRecv" and params.get("tunnelId") == tunnel.id:
                tunnel._feed(base64.b64decode(params["data"]))
        return tunnel

    def tunnel(self, tunnel_id):
        return self._tunnels.get(tunnel_id)


def agent_version_of(binary):
    """`<version> proto <n>` from `--version`, as a (version, proto) pair."""
    out = subprocess.run([binary, "--version"], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, check=True,
                         timeout=30).stdout.decode().strip()
    parts = out.split()
    if len(parts) != 3 or parts[1] != "proto":
        raise AgentError("unexpected --version output: %r" % out)
    return parts[0], int(parts[2])
