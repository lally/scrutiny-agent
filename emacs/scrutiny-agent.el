;;; scrutiny-agent.el --- Client for the scrutiny-agent remote backend  -*- lexical-binding: t; -*-

;; Author: Lally Singh
;; URL: https://github.com/lally/scrutiny-agent
;; Version: 0.2.0
;; Package-Requires: ((emacs "28.1"))
;; Keywords: tools, processes

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; Connects Emacs to a scrutiny-agent running on a remote host over a
;; user-supplied stdio transport (ssh / ssh -J / tsh / kubectl exec /
;; a local shell).  One persistent, multiplexed JSON-RPC channel per
;; host carries everything -- which is the whole point on links where
;; connection establishment is expensive (Teleport, jump hosts).
;;
;; The wire protocol is docs/protocol.md in the scrutiny-agent repo:
;; JSON-RPC 2.0 over LSP-style Content-Length frames, plus three
;; agent-specific mechanisms this client implements: `rpc.chunk'
;; reassembly for over-cap responses, the cred.request/cred.provide
;; credential broker, and the lsp.tunnel* raw-LSP envelope (see
;; scrutiny-agent-eglot.el for the eglot integration built on it).
;;
;; Connecting bootstraps the agent binary if needed: the remote's
;; OS/arch is probed over the pipe, the matching release binary is
;; downloaded locally (or taken from :local-binary), streamed across
;; as base64 inside a quoted heredoc, hash-gated with sha256, and
;; exec'd.  No scp, no second channel.
;;
;; Minimal setup:
;;
;;   (setq scrutiny-agent-hosts
;;         '(("devbox" :transport "tsh ssh devbox")))
;;   M-x scrutiny-agent-connect
;;
;; See emacs/README.md for eglot wiring.

;;; Code:

(require 'cl-lib)
(require 'subr-x)

(defgroup scrutiny-agent nil
  "Client for the scrutiny-agent remote backend."
  :group 'tools
  :prefix "scrutiny-agent-")

(defcustom scrutiny-agent-hosts nil
  "Alist of remote hosts: (NAME . PLIST).
NAME is a string (matched against TRAMP host names by the eglot
integration).  PLIST keys:

  :transport     (required) shell command whose stdin/stdout reach a
                 POSIX shell on the remote, e.g. \"ssh devbox\",
                 \"tsh ssh devbox\", \"kubectl exec -i pod -- sh\", or
                 just \"sh\" for a local agent.
  :install-dir   remote directory for the agent binary, a shell
                 expression (default \"$HOME/.scrutiny-agent\").
  :agent-args    list of extra agent argv strings, e.g.
                 (\"--allow-root\" \"src\" \"--log\" \"agent.log\").
  :local-binary  path to a locally built agent binary to stream
                 instead of downloading a release (development)."
  :type '(alist :key-type string :value-type plist))

(defcustom scrutiny-agent-binary-version "0.2.0"
  "Release version of the agent binary to bootstrap onto remotes."
  :type 'string)

(defcustom scrutiny-agent-release-url-format
  "https://github.com/lally/scrutiny-agent/releases/download/v%s/scrutiny-agent-%s-%s"
  "Format for release-asset URLs: version, version, arch."
  :type 'string)

(defcustom scrutiny-agent-cache-directory
  (expand-file-name "scrutiny-agent" user-emacs-directory)
  "Directory caching downloaded agent binaries."
  :type 'directory)

(defcustom scrutiny-agent-credential-function
  (lambda (prompt) (read-passwd (format "scrutiny-agent %s " prompt)))
  "Function answering an agent `cred.request' PROMPT with a secret.
Called with the git prompt string (e.g. \"Password for ...\"); its
return value is sent back via `cred.provide'.  Returning an empty
string fails the underlying git operation."
  :type 'function)

(defconst scrutiny-agent-protocol-version 1)
(defconst scrutiny-agent--tunnel-chunk (* 32 1024)
  "Max raw bytes per lsp.tunnelSend notification (protocol convention).")

;; ---------------------------------------------------------------------
;; Connection object
;; ---------------------------------------------------------------------

(cl-defstruct (scrutiny-agent--conn (:constructor scrutiny-agent--conn-make))
  name process
  (buffer "")                ; unibyte read accumulator
  (next-id 0)
  (pending (make-hash-table :test #'eql))   ; id -> callback (RESULT ERR)
  (chunks (make-hash-table :test #'equal))  ; id-key -> reversed data list
  (tunnels (make-hash-table :test #'equal)) ; tunnelId -> tunnel struct
  capabilities agent-version frame-cap)

(defvar scrutiny-agent--connections (make-hash-table :test #'equal)
  "Live connections, keyed by host NAME.")

(defun scrutiny-agent--log (conn format &rest args)
  "Append a line to the host's log buffer (never hidden; always kept)."
  (let ((buf (get-buffer-create
              (format "*scrutiny-agent-log[%s]*"
                      (if conn (scrutiny-agent--conn-name conn) "?")))))
    (with-current-buffer buf
      (goto-char (point-max))
      (insert (format-time-string "%H:%M:%S ")
              (apply #'format format args) "\n"))))

;; ---------------------------------------------------------------------
;; Framing (pure; unit-tested)
;; ---------------------------------------------------------------------

(defun scrutiny-agent--frame (payload)
  "Wire bytes (unibyte) for one JSON-RPC PAYLOAD (a plist)."
  (let ((body (encode-coding-string (json-serialize payload) 'utf-8)))
    (concat (format "Content-Length: %d\r\n\r\n" (length body)) body)))

(defun scrutiny-agent--parse-frames (buffer)
  "Extract complete frames from unibyte BUFFER string.
Return (MESSAGES . REMAINING) where MESSAGES is a list of parsed
JSON plists in arrival order."
  (let (messages)
    ;; Tolerate stray newline bytes between the bootstrap dialogue and
    ;; the first frame (a trailing echo terminator can race the filter
    ;; swap). Anything else before a header is a hard error below.
    (setq buffer (string-trim-left buffer "[\r\n]+"))
    (catch 'done
      (while t
        (let ((hdr-end (string-search "\r\n\r\n" buffer)))
          (unless hdr-end (throw 'done nil))
          (let ((len nil))
            (dolist (line (split-string (substring buffer 0 hdr-end) "\r\n"))
              (when (string-match "^[Cc]ontent-[Ll]ength: *\\([0-9]+\\)" line)
                (setq len (string-to-number (match-string 1 line)))))
            (unless len
              ;; A header block without Content-Length is unrecoverable.
              (error "scrutiny-agent: malformed frame header %S"
                     (substring buffer 0 hdr-end)))
            (when (< (length buffer) (+ hdr-end 4 len))
              (throw 'done nil))
            (let ((body (substring buffer (+ hdr-end 4) (+ hdr-end 4 len))))
              (setq buffer (substring buffer (+ hdr-end 4 len)))
              (push (json-parse-string (decode-coding-string body 'utf-8)
                                       :object-type 'plist
                                       :null-object nil
                                       :false-object :json-false)
                    messages))))))
    (cons (nreverse messages) buffer)))

(defun scrutiny-agent--split-bytes (bytes n)
  "Split unibyte string BYTES into a list of chunks of at most N bytes."
  (let (out (off 0) (len (length bytes)))
    (while (< off len)
      (push (substring bytes off (min len (+ off n))) out)
      (setq off (+ off n)))
    (nreverse out)))

;; ---------------------------------------------------------------------
;; rpc.chunk reassembly (pure; unit-tested)
;; ---------------------------------------------------------------------

(defun scrutiny-agent--chunk-key (id) (format "%s" id))

(defun scrutiny-agent--assemble-chunks (parts)
  "PARTS is a list of (SEQ . DATA) in any order; return the decoded
JSON-RPC envelope they carry, parsed as a plist."
  (let ((sorted (sort (copy-sequence parts) (lambda (a b) (< (car a) (car b))))))
    (json-parse-string
     (decode-coding-string
      (base64-decode-string (mapconcat #'cdr sorted ""))
      'utf-8)
     :object-type 'plist :null-object nil :false-object :json-false)))

;; ---------------------------------------------------------------------
;; Dispatch
;; ---------------------------------------------------------------------

(defun scrutiny-agent--filter (conn _proc output)
  (setf (scrutiny-agent--conn-buffer conn)
        (concat (scrutiny-agent--conn-buffer conn) output))
  (let* ((parsed (scrutiny-agent--parse-frames
                  (scrutiny-agent--conn-buffer conn))))
    (setf (scrutiny-agent--conn-buffer conn) (cdr parsed))
    (dolist (msg (car parsed))
      (scrutiny-agent--dispatch conn msg))))

(defun scrutiny-agent--dispatch (conn msg)
  (let ((method (plist-get msg :method))
        (id (plist-get msg :id)))
    (cond
     ;; Notifications and agent-side requests we answer.
     (method
      (pcase method
        ("rpc.chunk"
         (let* ((p (plist-get msg :params))
                (key (scrutiny-agent--chunk-key (plist-get p :id))))
           (push (cons (plist-get p :seq) (plist-get p :data))
                 (gethash key (scrutiny-agent--conn-chunks conn)))))
        ("cred.request"
         (scrutiny-agent--on-cred-request conn (plist-get msg :params)))
        ("lsp.tunnelRecv"
         (scrutiny-agent--on-tunnel-recv conn (plist-get msg :params)))
        ("lsp.tunnelClosed"
         (scrutiny-agent--on-tunnel-closed conn (plist-get msg :params)))
        (_ (scrutiny-agent--log conn "notification %s (ignored)" method))))
     ;; Response.
     (id
      (let* ((result (plist-get msg :result))
             (err (plist-get msg :error))
             (cb (gethash id (scrutiny-agent--conn-pending conn))))
        (when (and (listp result) (eq (plist-get result :streamed) t))
          ;; Over-cap response: the real envelope arrived as chunks.
          (let* ((key (scrutiny-agent--chunk-key id))
                 (parts (gethash key (scrutiny-agent--conn-chunks conn)))
                 (envelope (scrutiny-agent--assemble-chunks parts)))
            (remhash key (scrutiny-agent--conn-chunks conn))
            (setq result (plist-get envelope :result)
                  err (plist-get envelope :error))))
        (when cb
          (remhash id (scrutiny-agent--conn-pending conn))
          (funcall cb result err))))
     ;; Null-id error (agent rejected a malformed frame).
     (t (scrutiny-agent--log conn "null-id error: %S"
                             (plist-get msg :error))))))

(defun scrutiny-agent--send (conn payload)
  (process-send-string (scrutiny-agent--conn-process conn)
                       (scrutiny-agent--frame payload)))

(defun scrutiny-agent-notify (conn method params)
  "Send a notification to CONN."
  (scrutiny-agent--send conn (list :jsonrpc "2.0" :method method
                                   :params params)))

(cl-defun scrutiny-agent-async-request (conn method params &key callback)
  "Send request METHOD; CALLBACK gets (RESULT ERR).  Returns the id."
  (let ((id (cl-incf (scrutiny-agent--conn-next-id conn))))
    (puthash id (or callback #'ignore)
             (scrutiny-agent--conn-pending conn))
    (scrutiny-agent--send conn (list :jsonrpc "2.0" :id id :method method
                                     :params params))
    id))

(cl-defun scrutiny-agent-request (conn method params &key (timeout 30))
  "Send request METHOD with PARAMS and block for the result.
Signals `scrutiny-agent-rpc-error' with (CODE MESSAGE) on an error
response, or a plain error on timeout/disconnect."
  (let* ((done nil) (res nil) (err nil)
         (proc (scrutiny-agent--conn-process conn)))
    (scrutiny-agent-async-request
     conn method params
     :callback (lambda (r e) (setq res r err e done t)))
    (let ((deadline (+ (float-time) timeout)))
      (while (and (not done) (process-live-p proc))
        (when (> (float-time) deadline)
          (error "scrutiny-agent: timeout (%ss) waiting for %s"
                 timeout method))
        (accept-process-output proc 0.05)))
    (unless done
      (error "scrutiny-agent: connection died during %s" method))
    (when err
      (signal 'scrutiny-agent-rpc-error
              (list (plist-get err :code) (plist-get err :message))))
    res))

(define-error 'scrutiny-agent-rpc-error "scrutiny-agent RPC error")

(defun scrutiny-agent-cancel (conn id)
  "Cancel in-flight request ID on CONN (wire-level $/cancelRequest)."
  (scrutiny-agent-notify conn "$/cancelRequest" (list :id id)))

(defun scrutiny-agent--on-cred-request (conn params)
  (let* ((prompt (or (plist-get params :prompt) ""))
         (value (condition-case e
                    (funcall scrutiny-agent-credential-function prompt)
                  (quit "")
                  (error (scrutiny-agent--log conn "cred function: %S" e)
                         ""))))
    (scrutiny-agent-async-request
     conn "cred.provide"
     (list :credId (plist-get params :credId) :value (or value "")))))

;; ---------------------------------------------------------------------
;; Bootstrap
;; ---------------------------------------------------------------------
;; Mirrors the documented bootstrap (docs/protocol.md, README): probe
;; OS/arch, hash-check the installed binary, stream the local copy as
;; base64 in a quoted heredoc on mismatch, verify sha256 remotely,
;; atomic-install, exec.  Every phase has a hard deadline and the whole
;; pre-exec transcript is kept in the host's log buffer.

(defun scrutiny-agent--arch-asset (uname-s uname-m)
  "Release-asset arch for a remote's (UNAME-S . UNAME-M), or nil."
  (when (string= uname-s "Linux")
    (pcase uname-m
      ("x86_64" "x86_64")
      ((or "aarch64" "arm64") "aarch64"))))

(defun scrutiny-agent--bootstrap-script (install-dir binary-name expected-hash)
  "Shell command that reports the install state of BINARY-NAME.
Prints `__SCRA_ST__ OK' when the installed binary matches
EXPECTED-HASH, else `__SCRA_ST__ <got-or-MISSING>'."
  (format (concat "D=\"%s\"; B=\"$D/%s\"; "
                  "if [ -x \"$B\" ]; then "
                  "H=$( (sha256sum \"$B\" 2>/dev/null || shasum -a 256 \"$B\") "
                  "| awk '{print $1}'); else H=MISSING; fi; "
                  "if [ \"$H\" = \"%s\" ]; then echo \"__SCRA_ST__ OK\"; "
                  "else echo \"__SCRA_ST__ $H\"; fi")
          install-dir binary-name expected-hash))

(defun scrutiny-agent--install-script (install-dir binary-name)
  "Shell prologue for streaming BINARY-NAME: consumes a base64 heredoc
into a tmp file (the epilogue then hash-gates and installs it)."
  (format (concat "D=\"%s\"; B=\"$D/%s\"; mkdir -p \"$D\" && "
                  "base64 -d > \"$B.tmp\" <<'__SCRB64__'")
          install-dir binary-name))

(defun scrutiny-agent--install-epilogue (install-dir binary-name expected-hash)
  (format (concat "D=\"%s\"; B=\"$D/%s\"; "
                  "H=$( (sha256sum \"$B.tmp\" 2>/dev/null || "
                  "shasum -a 256 \"$B.tmp\") | awk '{print $1}'); "
                  "if [ \"$H\" = \"%s\" ]; then chmod +x \"$B.tmp\" && "
                  "mv \"$B.tmp\" \"$B\" && echo \"__SCRA_INST__ OK\"; "
                  "else rm -f \"$B.tmp\"; echo \"__SCRA_INST__ $H\"; fi")
          install-dir binary-name expected-hash))

(defun scrutiny-agent--file-sha256 (file)
  (with-temp-buffer
    (set-buffer-multibyte nil)
    (insert-file-contents-literally file)
    (secure-hash 'sha256 (current-buffer))))

(defun scrutiny-agent--local-binary (host-plist arch)
  "Path to the agent binary to stream: :local-binary or a cached
release download for ARCH.  Errors with instructions when neither
is available."
  (or (when-let ((local (plist-get host-plist :local-binary)))
        (expand-file-name local))
      (let* ((ver scrutiny-agent-binary-version)
             (name (format "scrutiny-agent-%s-%s" ver arch))
             (cached (expand-file-name name scrutiny-agent-cache-directory))
             (url (format scrutiny-agent-release-url-format ver ver arch)))
        (unless (file-exists-p cached)
          (make-directory scrutiny-agent-cache-directory t)
          (message "scrutiny-agent: downloading %s..." url)
          (url-copy-file url cached))
        cached)))

(defun scrutiny-agent--wait-line (proc state marker timeout)
  "Wait until STATE's transcript contains a line starting with MARKER;
return the rest of that line.  STATE is a cons cell whose car
accumulates raw pre-exec output."
  (let ((deadline (+ (float-time) timeout)))
    (catch 'got
      (while t
        (dolist (line (split-string (car state) "\n"))
          (when (string-prefix-p marker line)
            (throw 'got (string-trim (substring line (length marker))))))
        (when (> (float-time) deadline)
          (error "scrutiny-agent bootstrap: timed out waiting for %s; transcript:\n%s"
                 marker (car state)))
        (unless (process-live-p proc)
          (error "scrutiny-agent bootstrap: transport died; transcript:\n%s"
                 (car state)))
        (accept-process-output proc 0.1)))))

(defun scrutiny-agent--bootstrap (proc host-plist name)
  "Run the pre-exec bootstrap dialogue on PROC.  Returns when the
remote shell has exec'd the agent (the pipe then speaks JSON-RPC)."
  (let* ((state (cons "" nil))
         (install-dir (or (plist-get host-plist :install-dir)
                          "$HOME/.scrutiny-agent")))
    (set-process-filter proc (lambda (_p out)
                               (setcar state (concat (car state) out))))
    ;; 1. Probe OS/arch (marker survives login banners/motd noise).
    (process-send-string proc "echo __SCRA_PROBE__ $(uname -s) $(uname -m)\n")
    (let* ((probe (scrutiny-agent--wait-line proc state "__SCRA_PROBE__" 30))
           (parts (split-string probe))
           (uname-s (nth 0 parts)) (uname-m (nth 1 parts))
           (arch (or (scrutiny-agent--arch-asset uname-s uname-m)
                     (if (and (string= uname-s "Darwin")
                              (plist-get host-plist :local-binary))
                         uname-m
                       (error "scrutiny-agent: no release binary for %s/%s (set :local-binary?)"
                              uname-s uname-m))))
           (binary (scrutiny-agent--local-binary host-plist arch))
           (hash (scrutiny-agent--file-sha256 binary))
           (bname (format "scrutiny-agent-%s-%s"
                          scrutiny-agent-binary-version arch)))
      ;; 2. Hash-check what is installed.
      (process-send-string
       proc (concat (scrutiny-agent--bootstrap-script install-dir bname hash)
                    "\n"))
      (let ((st (scrutiny-agent--wait-line proc state "__SCRA_ST__" 30)))
        (unless (string= st "OK")
          ;; 3. Stream the binary (base64 heredoc, hash-gated install).
          (message "scrutiny-agent[%s]: installing agent (%s)..." name bname)
          (process-send-string
           proc (concat (scrutiny-agent--install-script
                         install-dir bname) "\n"))
          ;; 4 KiB base64 lines: comfortably inside every shell's
          ;; heredoc line handling, still few enough sends to be fast.
          (let ((b64 (with-temp-buffer
                       (set-buffer-multibyte nil)
                       (insert-file-contents-literally binary)
                       (base64-encode-region (point-min) (point-max) t)
                       (buffer-string))))
            (dolist (chunk (scrutiny-agent--split-bytes b64 4096))
              (process-send-string proc chunk)
              (process-send-string proc "\n")))
          (process-send-string proc "__SCRB64__\n")
          (process-send-string
           proc (concat (scrutiny-agent--install-epilogue
                         install-dir bname hash) "\n"))
          (let ((inst (scrutiny-agent--wait-line proc state "__SCRA_INST__" 120)))
            (unless (string= inst "OK")
              (error "scrutiny-agent: install hash mismatch (got %s)" inst)))))
      ;; 4. Exec. The next bytes on the pipe are JSON-RPC frames.
      (let ((args (mapconcat #'shell-quote-argument
                             (plist-get host-plist :agent-args) " ")))
        (process-send-string
         proc (format "exec \"%s/%s\" --rpc-stdio %s\n"
                      install-dir bname args))))
    (car state)))

;; ---------------------------------------------------------------------
;; Connect / disconnect / commands
;; ---------------------------------------------------------------------

(defun scrutiny-agent-connection (name)
  "The live connection for host NAME, or nil."
  (let ((conn (gethash name scrutiny-agent--connections)))
    (when (and conn (process-live-p (scrutiny-agent--conn-process conn)))
      conn)))

;;;###autoload
(defun scrutiny-agent-connect (name)
  "Connect to configured host NAME (bootstrap + handshake); idempotent."
  (interactive (list (completing-read "scrutiny-agent host: "
                                      (mapcar #'car scrutiny-agent-hosts))))
  (or (scrutiny-agent-connection name)
      (let* ((plist (cdr (or (assoc name scrutiny-agent-hosts)
                             (error "Unknown scrutiny-agent host: %s" name))))
             (transport (or (plist-get plist :transport)
                            (error "Host %s has no :transport" name)))
             (stderr-buf (get-buffer-create
                          (format "*scrutiny-agent-stderr[%s]*" name)))
             (proc (make-process
                    :name (format "scrutiny-agent[%s]" name)
                    :command (list shell-file-name shell-command-switch
                                   transport)
                    :coding 'binary
                    :connection-type 'pipe
                    :noquery t
                    :stderr stderr-buf)))
        (when-let ((sp (get-buffer-process stderr-buf)))
          (set-process-query-on-exit-flag sp nil))
        (let ((transcript (scrutiny-agent--bootstrap proc plist name))
              (conn nil))
          (setq conn (scrutiny-agent--conn-make :name name :process proc))
          (scrutiny-agent--log conn "bootstrap transcript:\n%s" transcript)
          (set-process-filter
           proc (lambda (p out) (scrutiny-agent--filter conn p out)))
          (set-process-sentinel
           proc (lambda (p event)
                  (scrutiny-agent--log conn "transport: %s" (string-trim event))
                  (unless (process-live-p p)
                    (scrutiny-agent--on-disconnect conn))))
          (let ((hello (scrutiny-agent-request
                        conn "meta.hello"
                        (list :clientVersion "scrutiny-agent.el"
                              :supportedProtocolVersions
                              (vector scrutiny-agent-protocol-version)
                              :frameCap 131072))))
            (setf (scrutiny-agent--conn-capabilities conn)
                  (append (plist-get hello :capabilities) nil)
                  (scrutiny-agent--conn-agent-version conn)
                  (plist-get hello :agentVersion)
                  (scrutiny-agent--conn-frame-cap conn)
                  (plist-get hello :frameCap)))
          (puthash name conn scrutiny-agent--connections)
          (message "scrutiny-agent[%s]: connected (agent %s, %d capabilities)"
                   name (scrutiny-agent--conn-agent-version conn)
                   (length (scrutiny-agent--conn-capabilities conn)))
          conn))))

(defun scrutiny-agent--on-disconnect (conn)
  "Fail all pending requests and tunnels; drop the registry entry."
  (maphash (lambda (_id cb)
             (funcall cb nil (list :code -1 :message "disconnected")))
           (scrutiny-agent--conn-pending conn))
  (clrhash (scrutiny-agent--conn-pending conn))
  (maphash (lambda (_tid tunnel)
             (when-let ((cb (scrutiny-agent-tunnel-on-closed tunnel)))
               (funcall cb "disconnected")))
           (scrutiny-agent--conn-tunnels conn))
  (clrhash (scrutiny-agent--conn-tunnels conn))
  (remhash (scrutiny-agent--conn-name conn) scrutiny-agent--connections))

;;;###autoload
(defun scrutiny-agent-disconnect (name)
  "Close the connection to host NAME (kills the transport)."
  (interactive (list (completing-read "Disconnect: "
                                      (hash-table-keys
                                       scrutiny-agent--connections))))
  (when-let ((conn (gethash name scrutiny-agent--connections)))
    (delete-process (scrutiny-agent--conn-process conn))))

;;;###autoload
(defun scrutiny-agent-status (name)
  "Show meta.stat for host NAME."
  (interactive (list (completing-read "Host: "
                                      (hash-table-keys
                                       scrutiny-agent--connections))))
  (let* ((conn (or (scrutiny-agent-connection name)
                   (user-error "Not connected to %s" name)))
         (st (scrutiny-agent-request conn "meta.stat" nil)))
    (message "scrutiny-agent[%s] v%s up %.0fs lanes=%S lsp=%s tunnels=%s"
             name (plist-get st :agentVersion)
             (/ (or (plist-get st :uptimeMs) 0) 1000.0)
             (plist-get st :lanes)
             (plist-get st :lspSessions) (plist-get st :lspTunnels))))

;;;###autoload
(defun scrutiny-agent-show-logs (name)
  "Fetch and display the agent-side log tail for host NAME."
  (interactive (list (completing-read "Host: "
                                      (hash-table-keys
                                       scrutiny-agent--connections))))
  (let* ((conn (or (scrutiny-agent-connection name)
                   (user-error "Not connected to %s" name)))
         (r (scrutiny-agent-request conn "logs.tail" (list :maxBytes 65536)))
         (buf (get-buffer-create (format "*scrutiny-agent-remote-log[%s]*"
                                         name))))
    (if (not (eq (plist-get r :enabled) t))
        (message "scrutiny-agent[%s]: agent logging disabled (pass --log via :agent-args)" name)
      (with-current-buffer buf
        (erase-buffer)
        (insert (or (plist-get r :text) "")))
      (display-buffer buf))))

;; ---------------------------------------------------------------------
;; LSP tunnels (raw byte pipes to a remote language server)
;; ---------------------------------------------------------------------

(cl-defstruct scrutiny-agent-tunnel
  conn id server-path on-bytes on-closed)

(cl-defun scrutiny-agent-tunnel-open (conn workspace language
                                           &key on-bytes on-closed)
  "Open an LSP tunnel on CONN for WORKSPACE (remote path) and LANGUAGE
(protocol language int).  ON-BYTES is called with each unibyte chunk
of server output; ON-CLOSED with a reason string when the server
exits.  Returns a `scrutiny-agent-tunnel'."
  (let* ((r (scrutiny-agent-request
             conn "lsp.tunnelOpen"
             (list :workspacePath workspace :language language)
             :timeout 60))
         (tunnel (make-scrutiny-agent-tunnel
                  :conn conn
                  :id (plist-get r :tunnelId)
                  :server-path (plist-get r :serverPath)
                  :on-bytes on-bytes
                  :on-closed on-closed)))
    (puthash (scrutiny-agent-tunnel-id tunnel) tunnel
             (scrutiny-agent--conn-tunnels conn))
    tunnel))

(defun scrutiny-agent-tunnel-send (tunnel bytes)
  "Send unibyte string BYTES to TUNNEL's language server."
  (dolist (chunk (scrutiny-agent--split-bytes
                  bytes scrutiny-agent--tunnel-chunk))
    (scrutiny-agent-notify (scrutiny-agent-tunnel-conn tunnel)
                           "lsp.tunnelSend"
                           (list :tunnelId (scrutiny-agent-tunnel-id tunnel)
                                 :data (base64-encode-string chunk t)))))

(defun scrutiny-agent-tunnel-close (tunnel)
  "Close TUNNEL (idempotent).
The registration stays until the agent's `lsp.tunnelClosed'
notification confirms the server exited -- that is what fires
ON-CLOSED and unregisters (or `scrutiny-agent--on-disconnect' does,
if the transport dies first)."
  (let ((conn (scrutiny-agent-tunnel-conn tunnel)))
    (when (scrutiny-agent-connection (scrutiny-agent--conn-name conn))
      (scrutiny-agent-async-request
       conn "lsp.tunnelClose"
       (list :tunnelId (scrutiny-agent-tunnel-id tunnel))))))

(defun scrutiny-agent--on-tunnel-recv (conn params)
  (when-let ((tunnel (gethash (plist-get params :tunnelId)
                              (scrutiny-agent--conn-tunnels conn))))
    (when-let ((cb (scrutiny-agent-tunnel-on-bytes tunnel)))
      (funcall cb (base64-decode-string (plist-get params :data))))))

(defun scrutiny-agent--on-tunnel-closed (conn params)
  (let ((tid (plist-get params :tunnelId)))
    (when-let ((tunnel (gethash tid (scrutiny-agent--conn-tunnels conn))))
      (remhash tid (scrutiny-agent--conn-tunnels conn))
      (when-let ((cb (scrutiny-agent-tunnel-on-closed tunnel)))
        (funcall cb (format "%s (exit %s)"
                            (or (plist-get params :reason) "exit")
                            (plist-get params :exitCode)))))))

(provide 'scrutiny-agent)
;;; scrutiny-agent.el ends here
