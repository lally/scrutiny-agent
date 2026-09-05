;;; scrutiny-agent-ops.el --- Typed wrappers for every agent method  -*- lexical-binding: t; -*-

;; Author: Lally Singh
;; URL: https://github.com/lally/scrutiny-agent
;; Version: 0.2.0
;; (The package's requirements are declared in scrutiny-agent.el;
;; a Package-Requires header outside the main file has no effect.)
;; Keywords: tools, vc, processes

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; One Lisp function per wire method in docs/protocol.md, so Elisp
;; callers never hand-assemble JSON-RPC params.  Everything here is a
;; thin, side-effect-free wrapper over `scrutiny-agent-request': it
;; converts arguments to the documented param names, and normalizes
;; results into shapes Elisp likes (lists rather than vectors, `nil'
;; rather than `:null').
;;
;; This is the layer that makes the agent useful as a general remote
;; back end -- `scrutiny-agent-ui.el' builds the interactive commands
;; on it, and `scrutiny-agent-verify.el' exercises it end to end.
;;
;; Errors surface as the `scrutiny-agent-rpc-error' signal carrying
;; (CODE MESSAGE); see `scrutiny-agent-ops-error-name'.

;;; Code:

(require 'cl-lib)
(require 'subr-x)
(require 'scrutiny-agent)

;; ---------------------------------------------------------------------
;; Error codes (docs/protocol.md "Error model")
;; ---------------------------------------------------------------------

(defconst scrutiny-agent-ops-error-names
  '((1000 . "INTERNAL")
    (1001 . "NOT_FOUND")
    (1002 . "INVALID_REQUEST")
    (1003 . "GIT_FAILED")
    (1004 . "LSP_FAILED")
    (1005 . "PERMISSION_DENIED")
    (1006 . "VERSION_MISMATCH")
    (1007 . "CANCELLED"))
  "Stable wire error codes mapped to their documented names.")

(defun scrutiny-agent-ops-error-name (code)
  "Human-readable name for wire error CODE."
  (or (cdr (assq code scrutiny-agent-ops-error-names))
      (format "ERROR(%s)" code)))

;; ---------------------------------------------------------------------
;; Result normalization
;; ---------------------------------------------------------------------

(defun scrutiny-agent-ops--list (value)
  "VALUE as a list.  JSON arrays parse to vectors; callers want lists."
  (cond ((vectorp value) (cl-coerce value 'list))
        ((listp value) value)
        (t (list value))))

(defun scrutiny-agent-ops--string (value)
  "VALUE as a string, or nil for a JSON null / missing field."
  (and (stringp value) value))

(defun scrutiny-agent-ops-bool (value)
  "Non-nil iff VALUE is JSON true.
`json-parse-string' is called with :false-object :json-false, so a
plain `nil' test would treat false as \"absent\"."
  (eq value t))

(defconst scrutiny-agent-ops-false :false
  "The value that serializes to JSON `false' in an outgoing request.
Note the asymmetry with `scrutiny-agent-ops-bool': the client parses
responses with :false-object :json-false, but `json-serialize'
accepts only its own :false sentinel, so the two directions do not
share a symbol.")

(defun scrutiny-agent-ops-json-bool (value)
  "VALUE as something `json-serialize' will render as a JSON boolean."
  (if value t scrutiny-agent-ops-false))

(defun scrutiny-agent-ops--language (language)
  "Coerce LANGUAGE (an int or a symbol/string name) to the wire int."
  (if (integerp language)
      language
    (let ((name (downcase (format "%s" language))))
      (or (cdr (assoc name '(("rust" . 1) ("python" . 2) ("javascript" . 3)
                             ("typescript" . 4) ("go" . 5) ("cpp" . 6)
                             ("c++" . 6) ("c" . 7) ("swift" . 8))))
          (error "Scrutiny-agent: unknown language %S" language)))))

(defconst scrutiny-agent-ops-languages
  '((1 . "rust") (2 . "python") (3 . "javascript") (4 . "typescript")
    (5 . "go") (6 . "cpp") (7 . "c") (8 . "swift"))
  "Protocol language ints mapped to their names (docs/protocol.md).")

;; ---------------------------------------------------------------------
;; meta.*
;; ---------------------------------------------------------------------

(defun scrutiny-agent-ops-stat (conn)
  "Agent load and uptime: plist with :lanes :lspSessions :uptimeMs ..."
  (scrutiny-agent-request conn "meta.stat" nil))

(defun scrutiny-agent-ops-capabilities (conn)
  "The agent's security/IO posture (`meta.capabilities')."
  (scrutiny-agent-request conn "meta.capabilities" nil))

(defun scrutiny-agent-ops-debug (conn &optional pad-bytes sleep-ms)
  "Diagnostic echo: pad the result to PAD-BYTES after sleeping SLEEP-MS."
  (scrutiny-agent-request
   conn "meta.debug"
   (nconc (and pad-bytes (list :padBytes pad-bytes))
          (and sleep-ms (list :sleepMs sleep-ms)))
   (max 30 (/ (or sleep-ms 0) 500))))

(defun scrutiny-agent-ops-logs-tail (conn &optional max-bytes)
  "Tail of the agent's `--log' file (at most MAX-BYTES, <= 1 MiB)."
  (scrutiny-agent-request conn "logs.tail"
                          (list :maxBytes (or max-bytes 65536))))

(defun scrutiny-agent-ops-fs-selftest (conn)
  "Runtime sandbox probe: does this agent's fs surface reach /etc/passwd?"
  (scrutiny-agent-request conn "fs.selftest" nil))

(defun scrutiny-agent-ops-allowed-roots (conn)
  "The filesystem roots this agent is running with, as a list of strings."
  (scrutiny-agent-ops--list
   (plist-get (plist-get (scrutiny-agent-ops-capabilities conn)
                         :fileSystemAccess)
              :allowedRoots)))

;; ---------------------------------------------------------------------
;; fs.*
;; ---------------------------------------------------------------------

(defun scrutiny-agent-ops-read-file (conn path)
  "Contents of remote PATH as a string (subject to the agent sandbox)."
  (plist-get (scrutiny-agent-request conn "fs.readFile" (list :path path)
                                     60)
             :content))

(defun scrutiny-agent-ops-list-directory (conn path)
  "List remote PATH.
Returns (CANONICAL-PATH . ENTRIES) where each entry is a plist with
:name and :isDir."
  (let ((r (scrutiny-agent-request conn "fs.listDirectory"
                                   (list :path path) 60)))
    (cons (plist-get r :path)
          (mapcar (lambda (e)
                    (list :name (plist-get e :name)
                          :isDir (scrutiny-agent-ops-bool (plist-get e :isDir))))
                  (scrutiny-agent-ops--list (plist-get r :entries))))))

;; ---------------------------------------------------------------------
;; git.* (read)
;; ---------------------------------------------------------------------

(defun scrutiny-agent-ops-head-sha (conn path)
  "HEAD sha of the repository at remote PATH."
  (plist-get (scrutiny-agent-request conn "git.headSha" (list :path path))
             :headSha))

(defun scrutiny-agent-ops-repo-metadata (conn path)
  "Repository metadata: :path :gitDir :isBare :headSha :currentBranch ..."
  (scrutiny-agent-request conn "git.repoMetadata" (list :path path)))

(defun scrutiny-agent-ops-remotes (conn path)
  "Remotes of the repository at PATH, as a list of plists."
  (scrutiny-agent-ops--list
   (plist-get (scrutiny-agent-request conn "git.remotes" (list :path path))
              :remotes)))

(defun scrutiny-agent-ops-branches (conn path &optional scope)
  "Branches of the repository at PATH.
SCOPE limits the result to `local' or `remote'; nil returns both."
  (scrutiny-agent-ops--list
   (plist-get (scrutiny-agent-request
               conn "git.branches"
               (list :path path
                     :local (scrutiny-agent-ops-json-bool
                             (memq scope '(nil local)))
                     :remote (scrutiny-agent-ops-json-bool
                              (memq scope '(nil remote)))))
              :branches)))

(defun scrutiny-agent-ops-commits (conn path &optional branch limit)
  "Commits of PATH (newest first), optionally for BRANCH, up to LIMIT.
LIMIT defaults to 100."
  (scrutiny-agent-ops--list
   (plist-get (scrutiny-agent-request
               conn "git.commits"
               (nconc (list :path path :limit (or limit 100))
                      (and branch (list :branch branch)))
               60)
              :commits)))

(defun scrutiny-agent-ops-ahead-behind (conn path branch)
  "Ahead/behind counts for BRANCH in PATH versus its upstream."
  (scrutiny-agent-request conn "git.aheadBehind"
                          (list :path path :branch branch)))

(defun scrutiny-agent-ops-diff-for-commit (conn path sha)
  "Structured per-file diffs introduced by SHA in PATH."
  (scrutiny-agent-ops--list
   (plist-get (scrutiny-agent-request conn "git.diffForCommit"
                                      (list :path path :sha sha)
                                      120)
              :diffs)))

(defun scrutiny-agent-ops-working-tree-diff (conn path)
  "Structured diffs from the index to the working tree in PATH."
  (scrutiny-agent-ops--list
   (plist-get (scrutiny-agent-request conn "git.workingTreeDiff"
                                      (list :path path) 120)
              :diffs)))

(defun scrutiny-agent-ops-staged-diff (conn path)
  "Structured diffs from HEAD to the index in PATH."
  (scrutiny-agent-ops--list
   (plist-get (scrutiny-agent-request conn "git.stagedDiff"
                                      (list :path path) 120)
              :diffs)))

(defun scrutiny-agent-ops-show-file (conn path sha file)
  "Contents of FILE at revision SHA in PATH, or nil if absent there."
  (scrutiny-agent-ops--string
   (plist-get (scrutiny-agent-request conn "git.showFile"
                                      (list :path path :sha sha :file file)
                                      60)
              :content)))

(defun scrutiny-agent-ops-diff (conn path from to file)
  "Textual `git diff' of FILE between revisions FROM and TO in PATH."
  (scrutiny-agent-ops--string
   (plist-get (scrutiny-agent-request conn "git.diff"
                                      (list :path path :from from :to to
                                            :file file)
                                      60)
              :diff)))

(defun scrutiny-agent-ops-ancestor-p (conn path ancestor descendant)
  "Non-nil iff ANCESTOR is an ancestor of DESCENDANT in PATH."
  (scrutiny-agent-ops-bool
   (plist-get (scrutiny-agent-request conn "git.isAncestor"
                                      (list :path path :ancestor ancestor
                                            :descendant descendant))
              :isAncestor)))

;; ---------------------------------------------------------------------
;; git.* (mutating / network)
;; ---------------------------------------------------------------------

(defun scrutiny-agent-ops-checkout-branch (conn path branch)
  "Check out BRANCH in PATH (SAFE checkout; signals on refusal)."
  (scrutiny-agent-request conn "git.checkoutBranch"
                          (list :path path :branch branch) 120))

(defun scrutiny-agent-ops-clone (conn full-name clone-url install-dir
                                      &optional auth-op-id)
  "Clone CLONE-URL as FULL-NAME under INSTALL-DIR on the remote.
Credentials are brokered back to this client per prompt."
  (scrutiny-agent-request
   conn "git.clone"
   (list :fullName full-name :cloneURL clone-url :installDir install-dir
         :authOpId (or auth-op-id (format "op-%s" (random (expt 2 24)))))
   900))

(defun scrutiny-agent-ops-fetch (conn repo-path &optional auth-op-id)
  "Run `git fetch --all --prune' in REPO-PATH on the remote."
  (scrutiny-agent-request
   conn "git.fetch"
   (list :repoPath repo-path
         :authOpId (or auth-op-id (format "op-%s" (random (expt 2 24)))))
   900))

(defun scrutiny-agent-ops-ensure-repository (conn full-name clone-url
                                                  install-dir
                                                  &optional auth-op-id)
  "Clone FULL-NAME if absent under INSTALL-DIR, else fetch it in place."
  (scrutiny-agent-request
   conn "git.ensureRepository"
   (list :fullName full-name :cloneURL clone-url :installDir install-dir
         :authOpId (or auth-op-id (format "op-%s" (random (expt 2 24)))))
   900))

;; ---------------------------------------------------------------------
;; lsp.* (request-level queries)
;; ---------------------------------------------------------------------

(defun scrutiny-agent-ops--lsp-params (workspace language file content
                                                 &optional line character)
  (nconc (list :workspacePath workspace
               :language (scrutiny-agent-ops--language language))
         (and file (list :filePath file))
         (and content (list :fileContent content))
         (and line (list :line line))
         (and character (list :character character))))

(defun scrutiny-agent-ops-goto-definition (conn workspace language file
                                                content line character)
  "Definition locations for the symbol at LINE/CHARACTER (0-based)."
  (scrutiny-agent-ops--list
   (plist-get (scrutiny-agent-request
               conn "lsp.gotoDefinition"
               (scrutiny-agent-ops--lsp-params workspace language file
                                               content line character)
               120)
              :locations)))

(defun scrutiny-agent-ops-find-references (conn workspace language file
                                                content line character
                                                &optional exclude-declaration)
  "Reference locations for the symbol at LINE/CHARACTER (0-based).
The declaration itself is included unless EXCLUDE-DECLARATION."
  (scrutiny-agent-ops--list
   (plist-get (scrutiny-agent-request
               conn "lsp.findReferences"
               (nconc (scrutiny-agent-ops--lsp-params
                       workspace language file content line character)
                      (list :includeDeclaration
                            (scrutiny-agent-ops-json-bool
                             (not exclude-declaration))))
               120)
              :locations)))

(defun scrutiny-agent-ops-hover (conn workspace language file content
                                      line character)
  "Hover information at LINE/CHARACTER (0-based), or nil."
  (plist-get (scrutiny-agent-request
              conn "lsp.hover"
              (scrutiny-agent-ops--lsp-params workspace language file
                                              content line character)
              120)
             :hover))

(defun scrutiny-agent-ops-document-symbols (conn workspace language file
                                                 content)
  "Symbols defined in FILE."
  (scrutiny-agent-ops--list
   (plist-get (scrutiny-agent-request
               conn "lsp.documentSymbols"
               (scrutiny-agent-ops--lsp-params workspace language file content)
               120)
              :symbols)))

(defun scrutiny-agent-ops-workspace-symbols (conn workspace language query)
  "Symbols across WORKSPACE matching QUERY.
Optional in LSP: servers that do not implement `workspace/symbol'
answer LSP_FAILED (1004)."
  (scrutiny-agent-ops--list
   (plist-get (scrutiny-agent-request
               conn "lsp.workspaceSymbols"
               (list :workspacePath workspace
                     :language (scrutiny-agent-ops--language language)
                     :query query)
               120)
              :symbols)))

(defun scrutiny-agent-ops-folding-ranges (conn workspace language file content)
  "Folding ranges for FILE."
  (scrutiny-agent-ops--list
   (plist-get (scrutiny-agent-request
               conn "lsp.foldingRange"
               (scrutiny-agent-ops--lsp-params workspace language file content)
               120)
              :ranges)))

;; ---------------------------------------------------------------------
;; index.*
;; ---------------------------------------------------------------------

(defun scrutiny-agent-ops-index-create (conn workspace language cache-db)
  "Create an indexer over WORKSPACE for LANGUAGE; return the indexerId.
CACHE-DB is a remote path; a relative one anchors under the remote
$HOME."
  (plist-get (scrutiny-agent-request
              conn "index.create"
              (list :workspacePath workspace
                    :language (scrutiny-agent-ops--language language)
                    :cacheDBPath cache-db)
              120)
             :indexerId))

(defun scrutiny-agent-ops-index-run (conn indexer-id &optional timeout)
  "Run indexer INDEXER-ID to completion (bulk lane; streams progress).
TIMEOUT is in seconds, defaulting to 1800."
  (scrutiny-agent-request conn "index.run" (list :indexerId indexer-id)
                          (or timeout 1800)))

(defun scrutiny-agent-ops-index-cancel (conn indexer-id)
  "Ask indexer INDEXER-ID to stop (notification; no reply)."
  (scrutiny-agent-notify conn "index.cancel" (list :indexerId indexer-id)))

(defun scrutiny-agent-ops-index-destroy (conn indexer-id)
  "Destroy indexer INDEXER-ID (idempotent; refused while running)."
  (scrutiny-agent-request conn "index.destroy"
                          (list :indexerId indexer-id) 60))

;; ---------------------------------------------------------------------
;; watch.*
;; ---------------------------------------------------------------------

(defun scrutiny-agent-ops-watch-head (conn path)
  "Watch the resolved .git/HEAD of PATH; return the watchId.
Fires `watch.headChanged' on checkout / branch switch / rebase -- not
on a plain commit, which moves the branch ref rather than HEAD."
  (plist-get (scrutiny-agent-request conn "watch.head" (list :path path))
             :watchId))

(defun scrutiny-agent-ops-watch-stop (conn watch-id)
  "Stop watch WATCH-ID (notification; no reply)."
  (scrutiny-agent-notify conn "watch.stop" (list :watchId watch-id)))

;; ---------------------------------------------------------------------
;; diffcache.*
;; ---------------------------------------------------------------------

(defun scrutiny-agent-ops-diffcache-get (conn cache-dir from to file)
  "Look up the cached diff for (FROM TO FILE) in CACHE-DIR.
Returns the cached value, or nil on a miss."
  (let ((r (scrutiny-agent-request conn "diffcache.get"
                                   (list :cacheDir cache-dir :fromSha from
                                         :toSha to :file file)
                                   60)))
    (and (scrutiny-agent-ops-bool (plist-get r :hit)) (plist-get r :value))))

(defun scrutiny-agent-ops-diffcache-put (conn cache-dir from to file value)
  "Store VALUE for (FROM TO FILE) in CACHE-DIR."
  (scrutiny-agent-request conn "diffcache.put"
                          (list :cacheDir cache-dir :fromSha from :toSha to
                                :file file :value value)
                          60))

(defun scrutiny-agent-ops-diffcache-prune (conn cache-dir days)
  "Prune entries older than DAYS from CACHE-DIR; return the count removed."
  (plist-get (scrutiny-agent-request conn "diffcache.prune"
                                     (list :cacheDir cache-dir :days days)
                                     120)
             :removed))

;; ---------------------------------------------------------------------
;; cred.*
;; ---------------------------------------------------------------------

(defun scrutiny-agent-ops-cred-selftest (conn auth-op-id prompt)
  "Drive the full credential-broker round trip with PROMPT.
Diagnostic: exercises the unix socket, the agent's self-exec as
$GIT_ASKPASS, and the cred.request/cred.provide exchange with this
client, without needing a remote that demands authentication."
  (scrutiny-agent-request conn "cred.selftest"
                          (list :authOpId auth-op-id :prompt prompt)
                          120))

(provide 'scrutiny-agent-ops)
;;; scrutiny-agent-ops.el ends here
