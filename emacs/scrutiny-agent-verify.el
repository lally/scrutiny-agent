;;; scrutiny-agent-verify.el --- Exercise every agent operation  -*- lexical-binding: t; -*-

;; Author: Lally Singh
;; URL: https://github.com/lally/scrutiny-agent
;; Version: 0.2.0
;; (The package's requirements are declared in scrutiny-agent.el;
;; a Package-Requires header outside the main file has no effect.)
;; Keywords: tools, processes

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; `M-x scrutiny-agent-verify' drives every operation the connected
;; agent advertises and reports what actually happened, in a buffer.
;; It answers the question you have after setting up a new host: is
;; this thing working, and which parts?
;;
;; It mirrors the property the Python conformance suite enforces
;; (tests/conformance/conformance.py): every capability in the
;; `meta.hello' response must be exercised, and the run flags any
;; advertised capability this file does not know how to drive -- so a
;; protocol addition cannot quietly go unverified from the Emacs side.
;;
;; Read-only by default.  Operations that write to the remote
;; (checkout, clone, fetch, indexing) run only with a prefix argument,
;; and even then only against paths you supply.
;;
;; Statuses:
;;   PASS  the operation ran and its result had the documented shape
;;   FAIL  it errored unexpectedly, or answered something malformed
;;   SKIP  not applicable here (no language server, no upstream, ...)

;;; Code:

(require 'cl-lib)
(require 'subr-x)
(require 'scrutiny-agent)
(require 'scrutiny-agent-ops)
(require 'scrutiny-agent-ui)

(defvar scrutiny-agent-verify--results nil
  "Accumulated results for the run in progress: list of plists.")

(defvar scrutiny-agent-verify--covered nil
  "Capabilities exercised by the run in progress.")

(define-error 'scrutiny-agent-verify-skip "check not applicable")

(defun scrutiny-agent-verify--skip (format &rest args)
  "Abort the current check as SKIP, with a reason."
  (signal 'scrutiny-agent-verify-skip (list (apply #'format format args))))

(defun scrutiny-agent-verify--record (capabilities status detail)
  (push (list :capabilities capabilities :status status :detail detail)
        scrutiny-agent-verify--results)
  (dolist (capability capabilities)
    (cl-pushnew capability scrutiny-agent-verify--covered :test #'equal)))

(defmacro scrutiny-agent-verify--check (capabilities &rest body)
  "Run BODY as one check covering CAPABILITIES (a list of method names).
BODY returns a detail string on success, calls
`scrutiny-agent-verify--skip' to skip, and may signal anything to
fail.  Its result is recorded either way -- one bad operation never
ends the run."
  (declare (indent 1) (debug t))
  `(let ((caps ,capabilities))
     (condition-case err
         (let ((detail (progn ,@body)))
           (scrutiny-agent-verify--record caps "PASS" (or detail "")))
       (scrutiny-agent-verify-skip
        (scrutiny-agent-verify--record caps "SKIP" (car (cdr err))))
       (scrutiny-agent-rpc-error
        (scrutiny-agent-verify--record
         caps "FAIL" (format "%s: %s"
                             (scrutiny-agent-ops-error-name (nth 1 err))
                             (nth 2 err))))
       (error
        (scrutiny-agent-verify--record caps "FAIL" (format "%S" err))))))

(defun scrutiny-agent-verify--expect (condition format &rest args)
  "Fail the current check unless CONDITION holds."
  (unless condition (apply #'error format args))
  t)

;; ---------------------------------------------------------------------
;; Checks
;; ---------------------------------------------------------------------

(defun scrutiny-agent-verify--meta (conn)
  (scrutiny-agent-verify--check '("meta.stat")
    (let ((stat (scrutiny-agent-ops-stat conn)))
      (scrutiny-agent-verify--expect
       (plist-get stat :lanes) "meta.stat returned no lane information")
      (format "v%s, up %.0fs, %s LSP sessions, %s tunnels"
              (plist-get stat :agentVersion)
              (/ (or (plist-get stat :uptimeMs) 0) 1000.0)
              (plist-get stat :lspSessions) (plist-get stat :lspTunnels))))

  (scrutiny-agent-verify--check '("meta.capabilities")
    (let* ((caps (scrutiny-agent-ops-capabilities conn))
           (roots (scrutiny-agent-ops--list
                   (plist-get (plist-get caps :fileSystemAccess)
                              :allowedRoots))))
      (scrutiny-agent-verify--expect
       (equal (plist-get caps :protocolVersion) 1)
       "protocolVersion is %S, expected 1" (plist-get caps :protocolVersion))
      (format "protocol 1, roots: %s"
              (if roots (string-join roots ", ") "(not reported)"))))

  (scrutiny-agent-verify--check '("meta.debug")
    (let* ((cap (or (scrutiny-agent--conn-frame-cap conn) 131072))
           (small (scrutiny-agent-ops-debug conn 32))
           (big (scrutiny-agent-ops-debug conn (* cap 2))))
      (scrutiny-agent-verify--expect
       (= (length (plist-get small :pad)) 32)
       "meta.debug padded %d bytes, asked for 32"
       (length (plist-get small :pad)))
      ;; The large one must come back through rpc.chunk intact -- this
      ;; is the streaming path every big diff and file read uses.
      (scrutiny-agent-verify--expect
       (= (length (plist-get big :pad)) (* cap 2))
       "streamed response was %d bytes, expected %d"
       (length (plist-get big :pad)) (* cap 2))
      (format "echo ok; %d KiB reassembled from rpc.chunk"
              (/ (* cap 2) 1024))))

  (scrutiny-agent-verify--check '("logs.tail")
    (let ((tail (scrutiny-agent-ops-logs-tail conn 4096)))
      (if (scrutiny-agent-ops-bool (plist-get tail :enabled))
          (format "%s (%s bytes available)" (plist-get tail :path)
                  (plist-get tail :bytes))
        (scrutiny-agent-verify--skip
         "agent logging is off; add --log to :agent-args"))))

  (scrutiny-agent-verify--check '("fs.selftest")
    (let ((probe (scrutiny-agent-ops-fs-selftest conn)))
      (if (scrutiny-agent-ops-bool (plist-get probe :succeeded))
          (format "%s is READABLE -- this agent has no effective sandbox"
                  (plist-get probe :probe))
        (format "%s denied (%s) -- sandbox enforcing"
                (plist-get probe :probe) (plist-get probe :errorCode))))))

(defun scrutiny-agent-verify--fs (conn directory)
  (scrutiny-agent-verify--check '("fs.listDirectory")
    (let* ((result (scrutiny-agent-ops-list-directory conn directory))
           (entries (cdr result)))
      (scrutiny-agent-verify--expect
       (stringp (car result)) "fs.listDirectory returned no canonical path")
      (format "%s: %d entries" (car result) (length entries))))

  (scrutiny-agent-verify--check '("fs.readFile")
    (let* ((entries (cdr (scrutiny-agent-ops-list-directory conn directory)))
           (file (cl-find-if-not (lambda (e) (plist-get e :isDir)) entries)))
      (unless file
        (scrutiny-agent-verify--skip "no regular file in %s" directory))
      (let* ((path (concat (file-name-as-directory directory)
                           (plist-get file :name)))
             (content (scrutiny-agent-ops-read-file conn path)))
        (scrutiny-agent-verify--expect
         (stringp content) "fs.readFile returned %S" content)
        (format "%s: %d bytes" (plist-get file :name)
                (string-bytes content))))))

(defun scrutiny-agent-verify--fs-meta (conn directory)
  "Check the metadata surface the Emacs file handler is built on."
  (scrutiny-agent-verify--check '("fs.stat" "fs.statBatch")
    (let* ((entries (cdr (scrutiny-agent-ops-list-directory conn directory)))
           (file (cl-find-if-not (lambda (e) (plist-get e :isDir)) entries)))
      (unless file
        (scrutiny-agent-verify--skip "no regular file in %s" directory))
      (let* ((path (concat (file-name-as-directory directory)
                           (plist-get file :name)))
             (stat (scrutiny-agent-request conn "fs.stat" (list :path path)
                                           30))
             (batch (plist-get
                     (scrutiny-agent-request
                      conn "fs.statBatch"
                      (list :paths (vector path (concat path ".missing")))
                      30)
                     :stats)))
        (scrutiny-agent-verify--expect
         (scrutiny-agent-ops-bool (plist-get stat :exists))
         "fs.stat says %s does not exist" path)
        (scrutiny-agent-verify--expect
         (equal (length (scrutiny-agent-ops--list batch)) 2)
         "fs.statBatch answered %s entries for 2 paths"
         (length (scrutiny-agent-ops--list batch)))
        (format "%s: %s bytes, mode %o; batch answers in order"
                (plist-get file :name) (plist-get stat :size)
                (or (plist-get stat :mode) 0))))))

(defun scrutiny-agent-verify--fs-write (conn directory write)
  "Check the write surface, when enabled and when writes are allowed."
  (scrutiny-agent-verify--check
      '("fs.writeFile" "fs.mkdir" "fs.delete" "fs.rename" "fs.copy"
        "fs.chmod")
    (unless (member "fs.writeFile" (scrutiny-agent--conn-capabilities conn))
      (scrutiny-agent-verify--skip
       "agent is read-only (start it with --allow-write)"))
    (unless write
      (scrutiny-agent-verify--skip
       "writes disabled; re-run with a prefix argument"))
    (unless directory (scrutiny-agent-verify--skip "no directory given"))
    ;; Everything happens inside a scratch directory we create and
    ;; remove, so nothing of the user's is touched.
    (let* ((scratch (format "%sscrutiny-verify-%s"
                            (file-name-as-directory directory)
                            (random (expt 2 30))))
           (file (concat scratch "/probe.txt")))
      (unwind-protect
          (progn
            (scrutiny-agent-request conn "fs.mkdir"
                                    (list :path scratch :parents t)
                                    60)
            (let ((result (scrutiny-agent-request
                           conn "fs.writeFile"
                           (list :path file :content "verify\n")
                           60)))
              (scrutiny-agent-verify--expect
               (scrutiny-agent-ops-bool (plist-get result :ok))
               "fs.writeFile did not report success"))
            (scrutiny-agent-verify--expect
             (equal (scrutiny-agent-ops-read-file conn file) "verify\n")
             "the bytes written did not come back")
            (scrutiny-agent-request conn "fs.rename"
                                    (list :from file
                                          :to (concat scratch "/moved.txt"))
                                    60)
            (scrutiny-agent-request conn "fs.copy"
                                    (list :from (concat scratch "/moved.txt")
                                          :to (concat scratch "/copy.txt"))
                                    60)
            (scrutiny-agent-request conn "fs.chmod"
                                    (list :path (concat scratch "/copy.txt")
                                          :mode #o640)
                                    60)
            "write / read / rename / copy / chmod all round-tripped")
        (ignore-errors
          (scrutiny-agent-request conn "fs.delete"
                                  (list :path scratch :recursive t)
                                  60))))))

(defun scrutiny-agent-verify--git-read (conn repo)
  (unless repo (scrutiny-agent-verify--skip "no repository given"))
  (let (head parent branch)

    (scrutiny-agent-verify--check '("git.repoMetadata")
      (let ((meta (scrutiny-agent-ops-repo-metadata conn repo)))
        (setq head (plist-get meta :headSha)
              branch (plist-get meta :currentBranch))
        (scrutiny-agent-verify--expect
         (stringp head) "git.repoMetadata returned no headSha")
        (format "%s on %s%s" (substring head 0 8) (or branch "(detached)")
                (if (scrutiny-agent-ops-bool
                     (plist-get meta :hasUncommittedChanges))
                    ", dirty" ""))))

    (scrutiny-agent-verify--check '("git.headSha")
      (let ((sha (scrutiny-agent-ops-head-sha conn repo)))
        (scrutiny-agent-verify--expect
         (equal sha head)
         "git.headSha (%s) disagrees with git.repoMetadata (%s)" sha head)
        (format "%s" (substring sha 0 8))))

    (scrutiny-agent-verify--check '("git.remotes")
      (let ((remotes (scrutiny-agent-ops-remotes conn repo)))
        (if remotes
            (string-join (mapcar (lambda (r) (plist-get r :name)) remotes) ", ")
          "no remotes configured")))

    (scrutiny-agent-verify--check '("git.branches")
      (let* ((branches (scrutiny-agent-ops-branches conn repo))
             (heads (cl-remove-if-not
                     (lambda (b) (scrutiny-agent-ops-bool
                                  (plist-get b :isHead)))
                     branches)))
        (scrutiny-agent-verify--expect
         branches "git.branches returned nothing")
        (scrutiny-agent-verify--expect
         (<= (length heads) 1) "%d branches claim to be HEAD" (length heads))
        (format "%d branches, HEAD is %s" (length branches)
                (or (plist-get (car heads) :name) "(detached)"))))

    (scrutiny-agent-verify--check '("git.commits")
      (let ((commits (scrutiny-agent-ops-commits conn repo nil 20)))
        (scrutiny-agent-verify--expect
         commits "git.commits returned nothing")
        (setq parent (car (scrutiny-agent-ops--list
                           (plist-get (car commits) :parentOids))))
        (scrutiny-agent-verify--expect
         (equal (plist-get (car commits) :oid) head)
         "newest commit %s is not HEAD %s"
         (plist-get (car commits) :oid) head)
        (format "%d commits, newest %S" (length commits)
                (plist-get (car commits) :summary))))

    (scrutiny-agent-verify--check '("git.aheadBehind")
      (unless branch (scrutiny-agent-verify--skip "HEAD is detached"))
      (let ((r (scrutiny-agent-ops-ahead-behind conn repo branch)))
        (format "%s: %s ahead, %s behind upstream" branch
                (plist-get r :ahead) (plist-get r :behind))))

    (scrutiny-agent-verify--check '("git.diffForCommit")
      (let ((diffs (scrutiny-agent-ops-diff-for-commit conn repo head)))
        (format "HEAD touches %d file(s)" (length diffs))))

    (scrutiny-agent-verify--check '("git.workingTreeDiff")
      (format "%d file(s) with unstaged changes"
              (length (scrutiny-agent-ops-working-tree-diff conn repo))))

    (scrutiny-agent-verify--check '("git.stagedDiff")
      (format "%d file(s) staged"
              (length (scrutiny-agent-ops-staged-diff conn repo))))

    (scrutiny-agent-verify--check '("git.showFile")
      (let* ((diffs (scrutiny-agent-ops-diff-for-commit conn repo head))
             (file (plist-get (car diffs) :newPath)))
        (unless (and file (not (string-empty-p file)))
          (scrutiny-agent-verify--skip "HEAD changed no readable file"))
        (let ((content (scrutiny-agent-ops-show-file conn repo head file)))
          (if content
              (format "%s at HEAD: %d bytes" file (string-bytes content))
            (format "%s absent at HEAD (deleted there)" file)))))

    (scrutiny-agent-verify--check '("git.diff")
      (unless parent
        (scrutiny-agent-verify--skip "HEAD is a root commit"))
      (let* ((diffs (scrutiny-agent-ops-diff-for-commit conn repo head))
             (file (plist-get (car diffs) :newPath)))
        (unless (and file (not (string-empty-p file)))
          (scrutiny-agent-verify--skip "HEAD changed no readable file"))
        (let ((text (scrutiny-agent-ops-diff conn repo parent head file)))
          (format "%s: %d bytes of patch" file
                  (string-bytes (or text ""))))))

    (scrutiny-agent-verify--check '("git.isAncestor")
      (unless parent
        (scrutiny-agent-verify--skip "HEAD is a root commit"))
      (scrutiny-agent-verify--expect
       (scrutiny-agent-ops-ancestor-p conn repo parent head)
       "HEAD's parent is not reported as its ancestor")
      (scrutiny-agent-verify--expect
       (not (scrutiny-agent-ops-ancestor-p conn repo head parent))
       "HEAD is reported as an ancestor of its own parent")
      "parent->HEAD true, HEAD->parent false")))

(defun scrutiny-agent-verify--git-exec (conn repo)
  "Check the allowlisted general git surface, when the agent serves it."
  (scrutiny-agent-verify--check '("git.exec")
    (unless (member "git.exec" (scrutiny-agent--conn-capabilities conn))
      (scrutiny-agent-verify--skip
       "agent does not serve git.exec (start it with --git-exec-preset)"))
    (unless repo (scrutiny-agent-verify--skip "no repository given"))
    (let ((result (scrutiny-agent-request
                   conn "git.exec"
                   (list :repoPath repo :args (vector "rev-parse" "HEAD"))
                   120))
          (head (scrutiny-agent-ops-head-sha conn repo)))
      (scrutiny-agent-verify--expect
       (equal (string-trim (or (plist-get result :stdout) "")) head)
       "git.exec rev-parse HEAD returned %S, not %s"
       (plist-get result :stdout) head)
      ;; The allowlist must actually refuse something, or it is not one.
      (let ((refused
             (condition-case err
                 (progn (scrutiny-agent-request
                         conn "git.exec"
                         (list :repoPath repo
                               :args (vector "-c" "core.pager=id" "log"))
                         60)
                        nil)
               (scrutiny-agent-rpc-error (equal (nth 1 err) 1005)))))
        (scrutiny-agent-verify--expect
         refused
         "-c core.pager was NOT refused -- this agent would let a client "
         "run arbitrary commands through git")
        (format "rev-parse matches; injection refused (%d subcommands allowed)"
                (length (scrutiny-agent--conn-capabilities conn)))))))

(defun scrutiny-agent-verify--diffcache (conn cache-dir)
  (scrutiny-agent-verify--check
      '("diffcache.put" "diffcache.get" "diffcache.prune")
    (let* ((token (format "verify-%s" (random (expt 2 30))))
           (from (concat "f" token))
           (to (concat "t" token))
           (value (list :fileExistsInTo t
                        :isForcePush scrutiny-agent-ops-false
                        :hunksJSON "[]")))
      (scrutiny-agent-verify--expect
       (null (scrutiny-agent-ops-diffcache-get conn cache-dir from to "x"))
       "a never-written cache key reported a hit")
      (scrutiny-agent-ops-diffcache-put conn cache-dir from to "x" value)
      (let ((got (scrutiny-agent-ops-diffcache-get conn cache-dir from to "x")))
        (scrutiny-agent-verify--expect got "the value just written was not found")
        (scrutiny-agent-verify--expect
         (equal (plist-get got :hunksJSON) "[]")
         "the cached value came back changed: %S" got))
      ;; Clean up after ourselves: prune only reaches entries older than
      ;; the cutoff, and -1 day covers everything including this one.
      (let ((removed (scrutiny-agent-ops-diffcache-prune conn cache-dir -1)))
        (format "put/get round trip ok; prune removed %s entr%s"
                removed (if (equal removed 1) "y" "ies"))))))

(defun scrutiny-agent-verify--cred (conn)
  (scrutiny-agent-verify--check '("cred.provide")
    (let* ((op (format "verify-%s" (random (expt 2 30))))
           (secret (format "verify-secret-%s" (random (expt 2 30))))
           ;; Answer our own prompt so the check needs no interaction
           ;; and no real credential.
           (scrutiny-agent-credential-function (lambda (_prompt) secret))
           (result (scrutiny-agent-ops-cred-selftest
                    conn op "Password for 'https://verify.invalid':")))
      (scrutiny-agent-verify--expect
       (equal (plist-get result :got) secret)
       "the broker delivered %S, not the secret this client provided"
       (plist-get result :got))
      (scrutiny-agent-verify--expect
       (equal (plist-get result :askpassExit) 0)
       "the askpass child exited %s" (plist-get result :askpassExit))
      "broker round trip delivered the secret; askpass exited 0")))

(defun scrutiny-agent-verify--lsp (conn workspace language)
  (unless (and workspace language)
    (scrutiny-agent-verify--skip "no workspace/language given"))
  (let* ((entries (ignore-errors
                    (cdr (scrutiny-agent-ops-list-directory conn workspace))))
         (source (cl-find-if
                  (lambda (e)
                    (and (not (plist-get e :isDir))
                         (scrutiny-agent-verify--source-p
                          (plist-get e :name) language)))
                  entries))
         (file (and source (concat (file-name-as-directory workspace)
                                   (plist-get source :name))))
         (content (and file (ignore-errors
                              (scrutiny-agent-ops-read-file conn file)))))

    (scrutiny-agent-verify--check '("lsp.documentSymbols")
      (unless content
        (scrutiny-agent-verify--skip "no source file for language %s in %s"
                                     language workspace))
      (let ((symbols (scrutiny-agent-ops-document-symbols
                      conn workspace language file content)))
        (format "%s: %d symbols" (plist-get source :name) (length symbols))))

    (scrutiny-agent-verify--check '("lsp.hover")
      (unless content (scrutiny-agent-verify--skip "no source file"))
      (let ((hover (scrutiny-agent-ops-hover conn workspace language file
                                             content 0 0)))
        (if hover "hover answered" "no hover at 0:0 (server answered null)")))

    (scrutiny-agent-verify--check '("lsp.gotoDefinition")
      (unless content (scrutiny-agent-verify--skip "no source file"))
      (format "%d location(s) at 0:0"
              (length (scrutiny-agent-ops-goto-definition
                       conn workspace language file content 0 0))))

    (scrutiny-agent-verify--check '("lsp.findReferences")
      (unless content (scrutiny-agent-verify--skip "no source file"))
      (format "%d location(s) at 0:0"
              (length (scrutiny-agent-ops-find-references
                       conn workspace language file content 0 0))))

    (scrutiny-agent-verify--check '("lsp.foldingRange")
      (unless content (scrutiny-agent-verify--skip "no source file"))
      (format "%d range(s)"
              (length (scrutiny-agent-ops-folding-ranges
                       conn workspace language file content))))

    (scrutiny-agent-verify--check '("lsp.workspaceSymbols")
      ;; Optional in LSP; pylsp and others answer -32601. A clean
      ;; LSP_FAILED is the documented outcome, not a defect.
      (condition-case err
          (format "%d symbol(s)"
                  (length (scrutiny-agent-ops-workspace-symbols
                           conn workspace language "a")))
        (scrutiny-agent-rpc-error
         (if (equal (nth 1 err) 1004)
             (scrutiny-agent-verify--skip
              "this server does not implement workspace/symbol")
           (signal (car err) (cdr err))))))

    (scrutiny-agent-verify--check
        '("lsp.tunnelOpen" "lsp.tunnelSend" "lsp.tunnelClose")
      (scrutiny-agent-verify--tunnel conn workspace language))))

(defun scrutiny-agent-verify--source-p (name language)
  "Non-nil if NAME looks like a source file for protocol LANGUAGE."
  (let ((extension (file-name-extension name)))
    (and extension
         (member (downcase extension)
                 (pcase language
                   (1 '("rs"))
                   (2 '("py"))
                   (3 '("js" "jsx" "mjs"))
                   (4 '("ts" "tsx"))
                   (5 '("go"))
                   (6 '("cpp" "cc" "cxx" "hpp" "hh"))
                   (7 '("c" "h"))
                   (8 '("swift"))
                   (_ nil))))))

(defun scrutiny-agent-verify--tunnel (conn workspace language)
  "Open a raw LSP tunnel, complete an `initialize', and close it."
  (let ((stream "")
        (closed nil)
        (tunnel nil))
    (unwind-protect
        (progn
          (setq tunnel (scrutiny-agent-tunnel-open
                        conn workspace language
                        (lambda (bytes)
                                    (setq stream (concat stream bytes)))
                        (lambda (reason) (setq closed reason))))
          (let* ((body (json-serialize
                        `(:jsonrpc "2.0" :id 1 :method "initialize"
                          :params (:processId :null
                                   :rootUri ,(concat "file://" workspace)
                                   :capabilities ,(make-hash-table)))))
                 (frame (concat (format "Content-Length: %d\r\n\r\n"
                                        (string-bytes body))
                                body))
                 (cut (/ (length frame) 2)))
            ;; Split mid-frame on purpose: the agent forwards bytes and
            ;; makes no promise that a chunk is a message.
            (scrutiny-agent-tunnel-send tunnel (substring frame 0 cut))
            (scrutiny-agent-tunnel-send tunnel (substring frame cut))
            (let ((deadline (+ (float-time) 60))
                  (response nil))
              (while (and (not response) (< (float-time) deadline))
                (accept-process-output nil 0.05)
                (setq response
                      (cl-find-if
                       (lambda (m) (and (equal (plist-get m :id) 1)
                                        (plist-get m :result)))
                       (car (ignore-errors
                              (scrutiny-agent--parse-frames stream))))))
              (scrutiny-agent-verify--expect
               response
               "no initialize response after 60s (%d bytes received%s)"
               (length stream) (if closed (format ", closed: %s" closed) ""))
              (format "%s answered initialize (%d bytes)"
                      (file-name-nondirectory
                       (scrutiny-agent-tunnel-server-path tunnel))
                      (length stream)))))
      (when tunnel
        (ignore-errors (scrutiny-agent-tunnel-close tunnel))))))

(defun scrutiny-agent-verify--watch (conn repo)
  (scrutiny-agent-verify--check '("watch.head" "watch.stop")
    (unless repo (scrutiny-agent-verify--skip "no repository given"))
    (let ((watch-id (scrutiny-agent-ops-watch-head conn repo)))
      (scrutiny-agent-verify--expect
       (stringp watch-id) "watch.head returned %S" watch-id)
      (scrutiny-agent-ops-watch-stop conn watch-id)
      ;; Confirm the connection is still healthy after the stop
      ;; notification, which is the only observable the client gets.
      (scrutiny-agent-ops-debug conn 4)
      (format "watch %s started and stopped" watch-id))))

(defun scrutiny-agent-verify--index (conn workspace language write)
  (scrutiny-agent-verify--check
      '("index.create" "index.run" "index.cancel" "index.destroy")
    (unless write
      (scrutiny-agent-verify--skip
       "writes disabled; re-run with a prefix argument"))
    (unless (and workspace language)
      (scrutiny-agent-verify--skip "no workspace/language given"))
    (let* ((db (format "scrutiny-cache/index/verify-%s.db"
                       (random (expt 2 30))))
           (indexer (scrutiny-agent-ops-index-create
                     conn workspace language db)))
      (unwind-protect
          (let ((result (scrutiny-agent-ops-index-run conn indexer 600)))
            (format "%s files, %s definitions -> %s"
                    (plist-get result :filesIndexed)
                    (plist-get result :definitionsFound) db))
        (ignore-errors (scrutiny-agent-ops-index-destroy conn indexer))))))

(defun scrutiny-agent-verify--git-write (conn repo write)
  (scrutiny-agent-verify--check '("git.fetch")
    (unless write
      (scrutiny-agent-verify--skip
       "writes disabled; re-run with a prefix argument"))
    (unless repo (scrutiny-agent-verify--skip "no repository given"))
    (unless (scrutiny-agent-ops-remotes conn repo)
      (scrutiny-agent-verify--skip "repository has no remotes"))
    (let ((r (scrutiny-agent-ops-fetch conn repo)))
      (scrutiny-agent-verify--expect
       (scrutiny-agent-ops-bool (plist-get r :ok)) "fetch reported not-ok")
      "git fetch --all --prune succeeded"))

  (scrutiny-agent-verify--check '("git.checkoutBranch")
    (unless write
      (scrutiny-agent-verify--skip
       "writes disabled; re-run with a prefix argument"))
    (unless repo (scrutiny-agent-verify--skip "no repository given"))
    (let ((branch (plist-get (scrutiny-agent-ops-repo-metadata conn repo)
                             :currentBranch)))
      (unless branch (scrutiny-agent-verify--skip "HEAD is detached"))
      ;; Check out the branch that is already current: exercises the
      ;; real code path without moving the user's working tree.
      (scrutiny-agent-ops-checkout-branch conn repo branch)
      (format "re-checked out %s (no-op, tree untouched)" branch)))

  (scrutiny-agent-verify--check '("git.clone" "git.ensureRepository")
    (unless write
      (scrutiny-agent-verify--skip
       "writes disabled; re-run with a prefix argument"))
    (unless repo (scrutiny-agent-verify--skip "no repository given"))
    ;; Clone the given repository over the remote's own filesystem into
    ;; a scratch directory: the real clone path, no network, and
    ;; nothing of the user's is modified.
    (let* ((token (format "verify-%s" (random (expt 2 30))))
           (install (format "scrutiny-cache/verify/%s" token))
           (r (scrutiny-agent-ops-ensure-repository
               conn (format "verify/%s" token) repo install)))
      (scrutiny-agent-verify--expect
       (stringp (plist-get r :localPath))
       "ensureRepository returned no localPath")
      ;; A second call must fetch in place rather than relocate.
      (let ((again (scrutiny-agent-ops-ensure-repository
                    conn (format "verify/%s" token) repo install)))
        (scrutiny-agent-verify--expect
         (equal (plist-get again :localPath) (plist-get r :localPath))
         "the second ensureRepository moved the clone"))
      (format "cloned then re-fetched at %s" (plist-get r :localPath)))))

;; ---------------------------------------------------------------------
;; Report
;; ---------------------------------------------------------------------

(defun scrutiny-agent-verify--face (status)
  (pcase status
    ("PASS" 'success)
    ("FAIL" 'error)
    (_ 'shadow)))

(defun scrutiny-agent-verify--report (host conn results uncovered)
  (let ((buffer (get-buffer-create (format "*scrutiny-agent-verify[%s]*" host)))
        (passed (cl-count "PASS" results :key (lambda (r) (plist-get r :status))
                          :test #'equal))
        (failed (cl-count "FAIL" results :key (lambda (r) (plist-get r :status))
                          :test #'equal))
        (skipped (cl-count "SKIP" results :key (lambda (r) (plist-get r :status))
                           :test #'equal)))
    (with-current-buffer buffer
      (let ((inhibit-read-only t))
        (erase-buffer)
        (insert (propertize
                 (format "scrutiny-agent verification -- %s (agent %s)\n\n"
                         host (scrutiny-agent--conn-agent-version conn))
                 'face 'bold))
        (dolist (result results)
          (insert (propertize (format "  %-4s " (plist-get result :status))
                              'face (scrutiny-agent-verify--face
                                     (plist-get result :status)))
                  (format "%-46s %s\n"
                          (string-join (plist-get result :capabilities) ", ")
                          (plist-get result :detail))))
        (insert (format "\n%d passed, %d failed, %d skipped\n"
                        passed failed skipped))
        (if uncovered
            (insert (propertize
                     (format "\nAdvertised but not exercised (%d): %s\n"
                             (length uncovered)
                             (string-join (sort uncovered #'string<) ", "))
                     'face 'warning))
          (insert "\nEvery advertised capability was exercised.\n")))
      (special-mode)
      (goto-char (point-min)))
    (display-buffer buffer)
    (message "scrutiny-agent[%s]: %d passed, %d failed, %d skipped%s"
             host passed failed skipped
             (if uncovered (format "; %d unexercised" (length uncovered)) ""))
    buffer))

;;;###autoload
(defun scrutiny-agent-verify (&optional write)
  "Exercise every operation the connected agent advertises, and report.

Prompts for a remote repository and a workspace to work against;
either may be left empty to skip the checks that need it.  Read-only
unless WRITE (a prefix argument) is given, in which case fetch,
checkout, clone and indexing are exercised too -- against scratch
paths, except the checkout, which re-checks-out the branch already
current so the working tree does not move.

The report ends with any capability the agent advertises that this
file does not know how to drive, so a protocol addition cannot go
silently unverified."
  (interactive "P")
  (let* ((host (scrutiny-agent-ui-host))
         (conn (scrutiny-agent-ui-connection host))
         (repo (let ((answer (read-string
                              "Remote repository (empty to skip git): "
                              (or scrutiny-agent-ui--path ""))))
                 (unless (string-empty-p answer) answer)))
         (workspace (let ((answer (read-string
                                   "Remote code workspace (empty to skip LSP): "
                                   (or repo ""))))
                      (unless (string-empty-p answer) answer)))
         (language (when workspace
                     (scrutiny-agent-ops--language
                      (completing-read
                       "Workspace language: "
                       (mapcar #'cdr scrutiny-agent-ops-languages) nil t
                       (cdr (assq (or (scrutiny-agent-ui--buffer-language) 2)
                                  scrutiny-agent-ops-languages))))))
         (scrutiny-agent-verify--results nil)
         (scrutiny-agent-verify--covered nil))

    (message "scrutiny-agent[%s]: verifying..." host)
    (scrutiny-agent-verify--meta conn)
    (scrutiny-agent-verify--fs conn (or repo workspace "."))
    (scrutiny-agent-verify--fs-meta conn (or repo workspace "."))
    (scrutiny-agent-verify--fs-write conn (or repo workspace ".") write)
    (if repo
        (scrutiny-agent-verify--git-read conn repo)
      (scrutiny-agent-verify--check
          '("git.repoMetadata" "git.headSha" "git.remotes" "git.branches"
            "git.commits" "git.aheadBehind" "git.diffForCommit"
            "git.workingTreeDiff" "git.stagedDiff" "git.showFile"
            "git.diff" "git.isAncestor")
        (scrutiny-agent-verify--skip "no repository given")))
    (scrutiny-agent-verify--git-exec conn repo)
    (scrutiny-agent-verify--diffcache conn "scrutiny-cache/diffcache")
    (scrutiny-agent-verify--cred conn)
    (if workspace
        (scrutiny-agent-verify--lsp conn workspace language)
      (scrutiny-agent-verify--check
          '("lsp.documentSymbols" "lsp.hover" "lsp.gotoDefinition"
            "lsp.findReferences" "lsp.foldingRange" "lsp.workspaceSymbols"
            "lsp.tunnelOpen" "lsp.tunnelSend" "lsp.tunnelClose")
        (scrutiny-agent-verify--skip "no workspace given")))
    (scrutiny-agent-verify--watch conn repo)
    (scrutiny-agent-verify--index conn workspace language write)
    (scrutiny-agent-verify--git-write conn repo write)

    (let ((uncovered
           (cl-remove-if
            (lambda (capability)
              (member capability scrutiny-agent-verify--covered))
            (scrutiny-agent--conn-capabilities conn))))
      (scrutiny-agent-verify--report
       host conn (nreverse scrutiny-agent-verify--results) uncovered))))

(provide 'scrutiny-agent-verify)
;;; scrutiny-agent-verify.el ends here
