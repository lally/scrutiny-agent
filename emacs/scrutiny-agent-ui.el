;;; scrutiny-agent-ui.el --- Interactive commands for scrutiny-agent  -*- lexical-binding: t; -*-

;; Author: Lally Singh
;; URL: https://github.com/lally/scrutiny-agent
;; Version: 0.2.0
;; (The package's requirements are declared in scrutiny-agent.el;
;; a Package-Requires header outside the main file has no effect.)
;; Keywords: tools, vc, processes

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; Drives every operation the agent serves from Emacs, so the agent is
;; useful on its own -- not only as eglot plumbing.  Over one already
;; established connection you get a remote file browser, git history /
;; branches / diffs, LSP queries at point, the symbol indexer, HEAD
;; watches, and the diagnostic surfaces (status, capabilities, log
;; tail, sandbox and credential self-tests).
;;
;; The point is that all of it rides the single multiplexed channel
;; you already paid to establish.  On a link where opening a session
;; is expensive -- Teleport, a jump host, MFA'd SSH -- that is the
;; difference between "browse the remote tree" being instant and being
;; a fresh handshake per file.
;;
;; Entry point:
;;
;;   M-x scrutiny-agent-menu      ; everything, in one transient
;;
;; or bind the prefix keymap:
;;
;;   (global-set-key (kbd "C-c s") scrutiny-agent-command-map)
;;
;; Every command reads the host from `scrutiny-agent-ui-host' when it
;; is set, else the single live connection, else prompts.

;;; Code:

(require 'cl-lib)
(require 'subr-x)
(require 'diff-mode)
(require 'scrutiny-agent)
(require 'scrutiny-agent-ops)

(defgroup scrutiny-agent-ui nil
  "Interactive commands for the scrutiny-agent remote backend."
  :group 'scrutiny-agent
  :prefix "scrutiny-agent-ui-")

(defcustom scrutiny-agent-ui-host nil
  "Host name used by the interactive commands, or nil to infer it.
When nil, a command uses the only live connection if there is
exactly one, otherwise it prompts."
  :type '(choice (const :tag "Infer" nil) string))

(defcustom scrutiny-agent-ui-commit-limit 100
  "Number of commits `scrutiny-agent-log' fetches."
  :type 'integer)

(defcustom scrutiny-agent-ui-index-cache "scrutiny-cache/index"
  "Remote directory (relative paths anchor under the remote $HOME)
holding indexer databases created by `scrutiny-agent-index'."
  :type 'string)

;; Per-buffer context for the special-mode buffers below.
(defvar-local scrutiny-agent-ui--host nil)
(defvar-local scrutiny-agent-ui--path nil)
(defvar-local scrutiny-agent-ui--extra nil)

;; ---------------------------------------------------------------------
;; Host and connection plumbing
;; ---------------------------------------------------------------------

(defun scrutiny-agent-ui-host ()
  "Host name for the current command.
Prefers the buffer's own host, then `scrutiny-agent-ui-host', then
the sole live connection, then the sole configured host, else asks."
  (or scrutiny-agent-ui--host
      scrutiny-agent-ui-host
      (let ((live (hash-table-keys scrutiny-agent--connections)))
        (when (= 1 (length live)) (car live)))
      (when (= 1 (length scrutiny-agent-hosts))
        (car (car scrutiny-agent-hosts)))
      (completing-read "scrutiny-agent host: "
                       (or (mapcar #'car scrutiny-agent-hosts)
                           (hash-table-keys scrutiny-agent--connections))
                       nil t)))

(defun scrutiny-agent-ui-connection (&optional host)
  "Live connection for HOST, connecting first if necessary."
  (let ((name (or host (scrutiny-agent-ui-host))))
    (or (scrutiny-agent-connection name)
        (scrutiny-agent-connect name))))

(defmacro scrutiny-agent-ui--with-error (what &rest body)
  "Run BODY, turning an RPC error into a readable `user-error'.
WHAT names the operation for the message."
  (declare (indent 1) (debug t))
  `(condition-case err (progn ,@body)
     (scrutiny-agent-rpc-error
      (user-error "%s failed: %s (%s)" ,what (nth 2 err)
                  (scrutiny-agent-ops-error-name (nth 1 err))))))

(defun scrutiny-agent-ui--read-remote-path (prompt &optional default)
  "Read a remote path, defaulting to DEFAULT or this buffer's path."
  (let ((initial (or default scrutiny-agent-ui--path
                     (and buffer-file-name
                          (file-local-name
                           (file-name-directory buffer-file-name)))
                     (and (file-remote-p default-directory)
                          (file-local-name default-directory)))))
    (read-string (format "%s: " prompt) initial)))

(defun scrutiny-agent-ui--display (name mode setup &optional path)
  "Show a buffer called NAME in MODE, filled by calling SETUP.
SETUP runs with the buffer current and writable.  PATH becomes the
buffer's remote context, so follow-up commands need not re-ask.
Returns the buffer."
  (let ((buffer (get-buffer-create name))
        (host (scrutiny-agent-ui-host)))
    (with-current-buffer buffer
      (let ((inhibit-read-only t))
        (erase-buffer)
        (funcall setup))
      ;; The mode call resets buffer-local state, so the context is set
      ;; after it, never before.
      (when mode (funcall mode))
      (setq scrutiny-agent-ui--host host)
      (when path (setq scrutiny-agent-ui--path path))
      (goto-char (point-min)))
    (display-buffer buffer)
    buffer))

;; ---------------------------------------------------------------------
;; Connection, status, diagnostics
;; ---------------------------------------------------------------------

;;;###autoload
(defun scrutiny-agent-info (&optional host)
  "Show agent version, capabilities, load and security posture for HOST.
This is the \"what am I actually talking to\" view: it reports the
running binary's own answers, not what the documentation promises."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (stat (scrutiny-agent-ui--with-error "meta.stat"
                 (scrutiny-agent-ops-stat conn)))
         (caps (scrutiny-agent-ui--with-error "meta.capabilities"
                 (scrutiny-agent-ops-capabilities conn)))
         (probe (scrutiny-agent-ui--with-error "fs.selftest"
                  (scrutiny-agent-ops-fs-selftest conn)))
         (fsa (plist-get caps :fileSystemAccess)))
    (scrutiny-agent-ui--display
     (format "*scrutiny-agent-info[%s]*" name) #'special-mode
     (lambda ()
       (insert (format "scrutiny-agent on %s\n" name)
               (format "  agent version    %s (protocol %s)\n"
                       (plist-get caps :agentVersion)
                       (plist-get caps :protocolVersion))
               (format "  uptime           %.1fs\n"
                       (/ (or (plist-get stat :uptimeMs) 0) 1000.0))
               (format "  frame cap        %s bytes\n"
                       (scrutiny-agent--conn-frame-cap conn))
               (format "  log level        %s\n" (plist-get stat :logLevel))
               (format "  LSP sessions     %s   tunnels %s\n"
                       (plist-get stat :lspSessions)
                       (plist-get stat :lspTunnels)))
       (insert "\nLanes (active/queued)\n")
       (let ((lanes (plist-get stat :lanes)))
         (dolist (lane '(:interactive :normal :bulk))
           (let ((l (plist-get lanes lane)))
             (insert (format "  %-14s %s/%s\n"
                             (substring (symbol-name lane) 1)
                             (plist-get l :active)
                             (plist-get l :queued))))))
       (insert "\nFilesystem sandbox\n")
       (dolist (root (scrutiny-agent-ops--list (plist-get fsa :allowedRoots)))
         (insert (format "  allowed root   %s\n" root)))
       (insert (format "  /etc/passwd    %s\n"
                       (if (scrutiny-agent-ops-bool (plist-get probe :succeeded))
                           "READABLE -- no effective sandbox"
                         (format "denied (%s)" (plist-get probe :errorCode)))))
       (insert "\nLanguage servers found on the remote\n")
       (let ((found (scrutiny-agent-ops--list
                     (plist-get (plist-get caps :languageServers) :binaries))))
         (if found
             (dolist (bin found) (insert (format "  %s\n" bin)))
           (insert "  (none)\n")))
       (insert (format "\nCapabilities (%d)\n"
                       (length (scrutiny-agent--conn-capabilities conn))))
       (dolist (cap (sort (copy-sequence
                           (scrutiny-agent--conn-capabilities conn))
                          #'string<))
         (insert (format "  %s\n" cap)))))))

;;;###autoload
(defun scrutiny-agent-ping (&optional host)
  "Time a round trip to HOST, and confirm chunked streaming works.
Sends a small request and one larger than the negotiated frame cap,
so both the plain and the `rpc.chunk' paths are measured."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (cap (or (scrutiny-agent--conn-frame-cap conn) 131072))
         (t0 (float-time))
         (_ (scrutiny-agent-ops-debug conn 8))
         (small (- (float-time) t0))
         (t1 (float-time))
         (big (scrutiny-agent-ops-debug conn (* cap 2)))
         (large (- (float-time) t1)))
    (unless (= (length (plist-get big :pad)) (* cap 2))
      (user-error "Scrutiny-agent[%s]: streamed response was truncated" name))
    (message "scrutiny-agent[%s]: round trip %.0f ms; %d KiB streamed in %.0f ms"
             name (* 1000 small) (/ (* cap 2) 1024) (* 1000 large))))

;;;###autoload
(defun scrutiny-agent-remote-logs (&optional host)
  "Fetch and show the agent-side log tail for HOST."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (r (scrutiny-agent-ui--with-error "logs.tail"
              (scrutiny-agent-ops-logs-tail conn (* 256 1024)))))
    (if (not (scrutiny-agent-ops-bool (plist-get r :enabled)))
        (message "scrutiny-agent[%s]: agent logging is off (add --log via :agent-args)"
                 name)
      (scrutiny-agent-ui--display
       (format "*scrutiny-agent-remote-log[%s]*" name) #'special-mode
       (lambda ()
         (insert (format ";; %s (%s bytes)\n\n" (plist-get r :path)
                         (plist-get r :bytes))
                 (or (plist-get r :text) "")))))))

;;;###autoload
(defun scrutiny-agent-credential-selftest (&optional host)
  "Exercise the credential broker on HOST end to end.
Prompts through `scrutiny-agent-credential-function' exactly as a
real `git fetch' would, and reports whether the secret arrived."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (op (format "selftest-%s" (random (expt 2 24))))
         (r (scrutiny-agent-ui--with-error "cred.selftest"
              (scrutiny-agent-ops-cred-selftest
               conn op "Password for 'https://example.invalid':"))))
    (message "scrutiny-agent[%s]: broker delivered %d bytes, askpass exited %s"
             name (length (or (plist-get r :got) ""))
             (plist-get r :askpassExit))))

;; ---------------------------------------------------------------------
;; Remote file browser
;; ---------------------------------------------------------------------

(defvar-keymap scrutiny-agent-browse-mode-map
  :doc "Keymap for `scrutiny-agent-browse-mode'."
  "RET" #'scrutiny-agent-browse-visit
  "f"   #'scrutiny-agent-browse-visit
  "^"   #'scrutiny-agent-browse-up
  "g"   #'scrutiny-agent-browse-refresh
  "G"   #'scrutiny-agent-status-at-point
  "l"   #'scrutiny-agent-log
  "n"   #'next-line
  "p"   #'previous-line)

(define-derived-mode scrutiny-agent-browse-mode special-mode
  "Scrutiny-Browse"
  "Browse a remote directory over the scrutiny-agent channel."
  (setq-local truncate-lines t))

;;;###autoload
(defun scrutiny-agent-browse (&optional path host)
  "Browse remote directory PATH on HOST.
With no PATH, starts at the remote $HOME (the agent resolves \".\")."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (target (or path (scrutiny-agent-ui--read-remote-path
                           "Remote directory" ".")))
         (result (scrutiny-agent-ui--with-error "fs.listDirectory"
                   (scrutiny-agent-ops-list-directory conn target)))
         (canonical (car result))
         (entries (cdr result))
         (buffer (get-buffer-create
                  (format "*scrutiny-agent-browse[%s]*" name))))
    (with-current-buffer buffer
      (let ((inhibit-read-only t))
        (erase-buffer)
        (insert (propertize (format "%s:%s\n\n" name canonical)
                            'face 'bold))
        (dolist (entry entries)
          (let ((entry-name (plist-get entry :name))
                (dir (plist-get entry :isDir)))
            (insert (propertize (format "  %s%s\n" entry-name (if dir "/" ""))
                                'scrutiny-entry
                                (list :name entry-name :isDir dir
                                      :path (concat
                                             (file-name-as-directory canonical)
                                             entry-name))
                                'face (if dir 'dired-directory 'default))))))
      (scrutiny-agent-browse-mode)
      (setq scrutiny-agent-ui--host name
            scrutiny-agent-ui--path canonical)
      (goto-char (point-min))
      (forward-line 2))
    (display-buffer buffer)
    buffer))

(defun scrutiny-agent-browse--entry ()
  (or (get-text-property (line-beginning-position) 'scrutiny-entry)
      (user-error "No entry on this line")))

(defun scrutiny-agent-browse-visit ()
  "Open the entry at point: descend into a directory, or view a file."
  (interactive)
  (let ((entry (scrutiny-agent-browse--entry)))
    (if (plist-get entry :isDir)
        (scrutiny-agent-browse (plist-get entry :path)
                               scrutiny-agent-ui--host)
      (scrutiny-agent-view-file (plist-get entry :path)
                                scrutiny-agent-ui--host))))

(defun scrutiny-agent-browse-up ()
  "Go to the parent directory."
  (interactive)
  (scrutiny-agent-browse
   (directory-file-name (file-name-directory
                         (directory-file-name scrutiny-agent-ui--path)))
   scrutiny-agent-ui--host))

(defun scrutiny-agent-browse-refresh ()
  "Re-read the current directory."
  (interactive)
  (scrutiny-agent-browse scrutiny-agent-ui--path scrutiny-agent-ui--host))

;;;###autoload
(defun scrutiny-agent-view-file (&optional path host)
  "View remote file PATH on HOST, read-only, with the matching major mode.
Reads through the agent's sandboxed `fs.readFile', so the file
arrives over the same channel as everything else -- no second
connection, no TRAMP round trip."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (target (or path (scrutiny-agent-ui--read-remote-path "Remote file")))
         (content (scrutiny-agent-ui--with-error "fs.readFile"
                    (scrutiny-agent-ops-read-file conn target)))
         (buffer (get-buffer-create
                  (format "%s:%s" name (file-name-nondirectory target)))))
    (with-current-buffer buffer
      (let ((inhibit-read-only t))
        (erase-buffer)
        (insert content))
      (goto-char (point-min))
      ;; Pick the major mode from the name, but keep the buffer
      ;; read-only: the agent's fs surface has no write side, so
      ;; pretending this is editable would invite lost work.
      (let ((buffer-file-name target))
        (ignore-errors (set-auto-mode)))
      (setq scrutiny-agent-ui--host name
            scrutiny-agent-ui--path target)
      (set-buffer-modified-p nil)
      (read-only-mode 1))
    (display-buffer buffer)
    buffer))

;; ---------------------------------------------------------------------
;; Git: status, branches, log, diffs
;; ---------------------------------------------------------------------

;;;###autoload
(defun scrutiny-agent-status (&optional path host)
  "Summarize the remote repository at PATH: HEAD, branch, remotes, dirt."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (repo (or path (scrutiny-agent-ui--read-remote-path "Remote repo")))
         (meta (scrutiny-agent-ui--with-error "git.repoMetadata"
                 (scrutiny-agent-ops-repo-metadata conn repo)))
         (remotes (scrutiny-agent-ops-remotes conn repo))
         (branch (plist-get meta :currentBranch))
         (tracking (and branch
                        (ignore-errors
                          (scrutiny-agent-ops-ahead-behind conn repo branch)))))
    (scrutiny-agent-ui--display
     (format "*scrutiny-agent-status[%s]*" name) #'special-mode
     (lambda ()
       (insert (format "%s:%s\n\n" name (plist-get meta :path))
               (format "  HEAD        %s\n" (plist-get meta :headSha))
               (format "  branch      %s\n" (or branch "(detached)"))
               (format "  git dir     %s\n" (plist-get meta :gitDir))
               (format "  bare        %s\n"
                       (if (scrutiny-agent-ops-bool (plist-get meta :isBare))
                           "yes" "no"))
               (format "  uncommitted %s\n"
                       (if (scrutiny-agent-ops-bool
                            (plist-get meta :hasUncommittedChanges))
                           "yes" "no")))
       (when tracking
         (insert (format "  upstream    %s ahead, %s behind\n"
                         (plist-get tracking :ahead)
                         (plist-get tracking :behind))))
       (insert "\nRemotes\n")
       (if remotes
           (dolist (remote remotes)
             (insert (format "  %-10s %s\n" (plist-get remote :name)
                             (plist-get remote :url)))
             (unless (equal (plist-get remote :url)
                            (plist-get remote :pushUrl))
               (insert (format "  %-10s %s (push)\n" ""
                               (plist-get remote :pushUrl)))))
         (insert "  (none)\n")))
     repo)))

(defun scrutiny-agent-status-at-point ()
  "Run `scrutiny-agent-status' on the directory at point."
  (interactive)
  (let ((entry (ignore-errors (scrutiny-agent-browse--entry))))
    (scrutiny-agent-status (if entry (plist-get entry :path)
                             scrutiny-agent-ui--path)
                           scrutiny-agent-ui--host)))

(defvar-keymap scrutiny-agent-branches-mode-map
  :doc "Keymap for `scrutiny-agent-branches-mode'."
  "RET" #'scrutiny-agent-branches-checkout
  "c"   #'scrutiny-agent-branches-checkout
  "l"   #'scrutiny-agent-branches-log
  "g"   #'scrutiny-agent-branches-refresh)

(define-derived-mode scrutiny-agent-branches-mode special-mode
  "Scrutiny-Branches"
  "List remote branches; RET checks one out."
  (setq-local truncate-lines t))

;;;###autoload
(defun scrutiny-agent-branches (&optional path host)
  "List branches of the remote repository at PATH."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (repo (or path (scrutiny-agent-ui--read-remote-path "Remote repo")))
         (branches (scrutiny-agent-ui--with-error "git.branches"
                     (scrutiny-agent-ops-branches conn repo)))
         (buffer (get-buffer-create
                  (format "*scrutiny-agent-branches[%s]*" name))))
    (with-current-buffer buffer
      (let ((inhibit-read-only t))
        (erase-buffer)
        (insert (propertize (format "%s:%s\n\n" name repo) 'face 'bold))
        (dolist (branch branches)
          (let ((bname (plist-get branch :name))
                (head (scrutiny-agent-ops-bool (plist-get branch :isHead)))
                (remote (scrutiny-agent-ops-bool (plist-get branch :isRemote))))
            (insert (propertize
                     (format " %s %-40s %s%s\n"
                             (if head "*" " ") bname
                             (substring (or (plist-get branch :targetOid) "") 0
                                        (min 8 (length (or (plist-get branch :targetOid) ""))))
                             (if (plist-get branch :upstream)
                                 (format "  -> %s" (plist-get branch :upstream))
                               ""))
                     'scrutiny-branch (list :name bname :remote remote)
                     'face (cond (head 'bold)
                                 (remote 'shadow)
                                 (t 'default)))))))
      (scrutiny-agent-branches-mode)
      (setq scrutiny-agent-ui--host name
            scrutiny-agent-ui--path repo)
      (goto-char (point-min))
      (forward-line 2))
    (display-buffer buffer)
    buffer))

(defun scrutiny-agent-branches--at-point ()
  (or (get-text-property (line-beginning-position) 'scrutiny-branch)
      (user-error "No branch on this line")))

(defun scrutiny-agent-branches-checkout ()
  "Check out the branch at point on the remote."
  (interactive)
  (let* ((branch (scrutiny-agent-branches--at-point))
         (bname (plist-get branch :name))
         (conn (scrutiny-agent-ui-connection scrutiny-agent-ui--host)))
    (when (plist-get branch :remote)
      (user-error "%s is a remote-tracking branch; check out a local one"
                  bname))
    (scrutiny-agent-ui--with-error "git.checkoutBranch"
      (scrutiny-agent-ops-checkout-branch conn scrutiny-agent-ui--path bname))
    (message "scrutiny-agent: checked out %s" bname)
    (scrutiny-agent-branches-refresh)))

(defun scrutiny-agent-branches-log ()
  "Show the commit log for the branch at point."
  (interactive)
  (scrutiny-agent-log scrutiny-agent-ui--path scrutiny-agent-ui--host
                      (plist-get (scrutiny-agent-branches--at-point) :name)))

(defun scrutiny-agent-branches-refresh ()
  "Re-read the branch list."
  (interactive)
  (scrutiny-agent-branches scrutiny-agent-ui--path scrutiny-agent-ui--host))

(defvar-keymap scrutiny-agent-log-mode-map
  :doc "Keymap for `scrutiny-agent-log-mode'."
  "RET" #'scrutiny-agent-log-show
  "d"   #'scrutiny-agent-log-show
  "g"   #'scrutiny-agent-log-refresh)

(define-derived-mode scrutiny-agent-log-mode special-mode "Scrutiny-Log"
  "Commit log of a remote repository; RET shows a commit's diff."
  (setq-local truncate-lines t))

;;;###autoload
(defun scrutiny-agent-log (&optional path host branch)
  "Show the commit log of the remote repository at PATH, optionally BRANCH."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (repo (or path (scrutiny-agent-ui--read-remote-path "Remote repo")))
         (commits (scrutiny-agent-ui--with-error "git.commits"
                    (scrutiny-agent-ops-commits
                     conn repo branch scrutiny-agent-ui-commit-limit)))
         (buffer (get-buffer-create
                  (format "*scrutiny-agent-log[%s]*" name))))
    (with-current-buffer buffer
      (let ((inhibit-read-only t))
        (erase-buffer)
        (insert (propertize (format "%s:%s%s  (%d commits)\n\n"
                                    name repo
                                    (if branch (format " [%s]" branch) "")
                                    (length commits))
                            'face 'bold))
        (dolist (commit commits)
          (insert (propertize
                   (format "%s  %-20s  %s\n"
                           (plist-get commit :shortOid)
                           (truncate-string-to-width
                            (or (plist-get commit :authorName) "") 20)
                           (or (plist-get commit :summary) ""))
                   'scrutiny-commit commit))))
      (scrutiny-agent-log-mode)
      (setq scrutiny-agent-ui--host name
            scrutiny-agent-ui--path repo
            scrutiny-agent-ui--extra branch)
      (goto-char (point-min))
      (forward-line 2))
    (display-buffer buffer)
    buffer))

(defun scrutiny-agent-log--at-point ()
  (or (get-text-property (line-beginning-position) 'scrutiny-commit)
      (user-error "No commit on this line")))

(defun scrutiny-agent-log-show ()
  "Show the diff of the commit at point."
  (interactive)
  (let ((commit (scrutiny-agent-log--at-point)))
    (scrutiny-agent-show-commit (plist-get commit :oid)
                                scrutiny-agent-ui--path
                                scrutiny-agent-ui--host)))

(defun scrutiny-agent-log-refresh ()
  "Re-read the commit log."
  (interactive)
  (scrutiny-agent-log scrutiny-agent-ui--path scrutiny-agent-ui--host
                      scrutiny-agent-ui--extra))

(defun scrutiny-agent-ui--insert-diffs (diffs)
  "Insert the `patch' text of DIFFS, or a note when there is none."
  (if (null diffs)
      (insert "(no changes)\n")
    (dolist (diff diffs)
      (let ((patch (plist-get diff :patch)))
        (if (and (stringp patch) (not (string-empty-p patch)))
            (insert patch)
          ;; A binary or mode-only change carries no textual patch.
          (insert (format "diff --git a/%s b/%s\n(no textual patch)\n"
                          (plist-get diff :oldPath)
                          (plist-get diff :newPath))))))))

;;;###autoload
(defun scrutiny-agent-show-commit (&optional sha path host)
  "Show the diff introduced by SHA in the remote repository at PATH."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (repo (or path (scrutiny-agent-ui--read-remote-path "Remote repo")))
         (rev (or sha (read-string "Commit: ")))
         (diffs (scrutiny-agent-ui--with-error "git.diffForCommit"
                  (scrutiny-agent-ops-diff-for-commit conn repo rev))))
    (scrutiny-agent-ui--display
     (format "*scrutiny-agent-commit[%s]*" name) #'diff-mode
     (lambda ()
       (insert (format "# %s:%s %s\n\n" name repo rev))
       (scrutiny-agent-ui--insert-diffs diffs))
     repo)))

;;;###autoload
(defun scrutiny-agent-diff-working-tree (&optional path host)
  "Show unstaged changes (index to working tree) in the remote repo PATH."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (repo (or path (scrutiny-agent-ui--read-remote-path "Remote repo")))
         (diffs (scrutiny-agent-ui--with-error "git.workingTreeDiff"
                  (scrutiny-agent-ops-working-tree-diff conn repo))))
    (scrutiny-agent-ui--display
     (format "*scrutiny-agent-worktree-diff[%s]*" name) #'diff-mode
     (lambda ()
       (insert (format "# %s:%s unstaged\n\n" name repo))
       (scrutiny-agent-ui--insert-diffs diffs))
     repo)))

;;;###autoload
(defun scrutiny-agent-diff-staged (&optional path host)
  "Show staged changes (HEAD to index) in the remote repository at PATH."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (repo (or path (scrutiny-agent-ui--read-remote-path "Remote repo")))
         (diffs (scrutiny-agent-ui--with-error "git.stagedDiff"
                  (scrutiny-agent-ops-staged-diff conn repo))))
    (scrutiny-agent-ui--display
     (format "*scrutiny-agent-staged-diff[%s]*" name) #'diff-mode
     (lambda ()
       (insert (format "# %s:%s staged\n\n" name repo))
       (scrutiny-agent-ui--insert-diffs diffs))
     repo)))

;;;###autoload
(defun scrutiny-agent-show-file-at-revision (&optional path sha file host)
  "View FILE as of revision SHA in the remote repository at PATH."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (repo (or path (scrutiny-agent-ui--read-remote-path "Remote repo")))
         (rev (or sha (read-string "Revision: " "HEAD")))
         (relative (or file (read-string "File (repo-relative): ")))
         (content (scrutiny-agent-ui--with-error "git.showFile"
                    (scrutiny-agent-ops-show-file conn repo rev relative))))
    (unless content
      (user-error "%s does not exist at %s" relative rev))
    (let ((buffer (get-buffer-create
                   (format "%s:%s@%s" name (file-name-nondirectory relative)
                           (substring rev 0 (min 8 (length rev)))))))
      (with-current-buffer buffer
        (let ((inhibit-read-only t))
          (erase-buffer)
          (insert content))
        (goto-char (point-min))
        (let ((buffer-file-name relative))
          (ignore-errors (set-auto-mode)))
        (setq scrutiny-agent-ui--host name
              scrutiny-agent-ui--path repo)
        (set-buffer-modified-p nil)
        (read-only-mode 1))
      (display-buffer buffer)
      buffer)))

;;;###autoload
(defun scrutiny-agent-fetch (&optional path host)
  "Run `git fetch --all --prune' in the remote repository at PATH.
Credentials, if git asks for any, are brokered to this Emacs and
never stored on the remote."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (repo (or path (scrutiny-agent-ui--read-remote-path "Remote repo"))))
    (message "scrutiny-agent[%s]: fetching %s..." name repo)
    (let ((r (scrutiny-agent-ui--with-error "git.fetch"
               (scrutiny-agent-ops-fetch conn repo))))
      (message "scrutiny-agent[%s]: fetch %s" name
               (if (scrutiny-agent-ops-bool (plist-get r :ok)) "ok" "failed")))))

;;;###autoload
(defun scrutiny-agent-clone (full-name clone-url install-dir &optional host)
  "Clone CLONE-URL as FULL-NAME under INSTALL-DIR on the remote."
  (interactive
   (list (read-string "Full name (owner/repo): ")
         (read-string "Clone URL: ")
         (read-string "Remote install dir: " "src")))
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name)))
    (message "scrutiny-agent[%s]: cloning %s..." name full-name)
    (let ((r (scrutiny-agent-ui--with-error "git.ensureRepository"
               (scrutiny-agent-ops-ensure-repository
                conn full-name clone-url install-dir))))
      (message "scrutiny-agent[%s]: repository at %s" name
               (plist-get r :localPath))
      (scrutiny-agent-status (plist-get r :localPath) name))))

;; ---------------------------------------------------------------------
;; LSP queries at point
;; ---------------------------------------------------------------------

(defun scrutiny-agent-ui--buffer-language ()
  "Protocol language int for this buffer, or nil."
  (require 'scrutiny-agent-eglot nil t)
  (if (fboundp 'scrutiny-agent-eglot--language)
      (scrutiny-agent-eglot--language major-mode)
    (cdr (assq major-mode '((rust-mode . 1) (rust-ts-mode . 1)
                            (python-mode . 2) (python-ts-mode . 2)
                            (js-mode . 3) (typescript-mode . 4)
                            (go-mode . 5) (go-ts-mode . 5)
                            (c++-mode . 6) (c++-ts-mode . 6)
                            (c-mode . 7) (c-ts-mode . 7)
                            (swift-mode . 8))))))

(defun scrutiny-agent-ui--lsp-context ()
  "Gather (CONN WORKSPACE LANGUAGE FILE CONTENT LINE CHARACTER) at point.
Uses the buffer's own text, so unsaved edits are what the remote
server sees -- matching how an LSP client behaves locally."
  (let ((language (or (scrutiny-agent-ui--buffer-language)
                      (user-error "No scrutiny-agent language for %s"
                                  major-mode)))
        (file (or (and buffer-file-name (file-local-name buffer-file-name))
                  scrutiny-agent-ui--path
                  (user-error "This buffer has no remote file path"))))
    (list (scrutiny-agent-ui-connection (scrutiny-agent-ui-host))
          (file-name-directory file)
          language
          file
          (buffer-substring-no-properties (point-min) (point-max))
          (1- (line-number-at-pos))
          (current-column))))

;; The LSP query surface is deliberately NOT a set of bespoke commands.
;; `scrutiny-agent-xref.el' registers the agent as an xref backend, an
;; eldoc function and an imenu index, so `M-.', `M-?', `C-M-.', eldoc
;; and `imenu' work as they always do -- and eglot over the LSP tunnel
;; remains the right tool for actually editing remote code.
;; `scrutiny-agent-ops.el' keeps the typed wrappers those layers (and
;; the verify harness) call.

;; ---------------------------------------------------------------------
;; Indexer and HEAD watches
;; ---------------------------------------------------------------------

(defvar scrutiny-agent-ui--watches nil
  "Alist of (WATCH-ID HOST . PATH) for watches this UI started.")

;;;###autoload
(defun scrutiny-agent-watch (&optional path host)
  "Watch the remote repository at PATH for HEAD changes.
Fires on checkout, branch switch and rebase -- not on a plain
commit, which moves the branch ref rather than the HEAD file."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (repo (or path (scrutiny-agent-ui--read-remote-path "Remote repo")))
         (watch-id (scrutiny-agent-ui--with-error "watch.head"
                     (scrutiny-agent-ops-watch-head conn repo))))
    (push (cons watch-id (cons name repo)) scrutiny-agent-ui--watches)
    (message "scrutiny-agent[%s]: watching %s (%s)" name repo watch-id)
    watch-id))

;;;###autoload
(defun scrutiny-agent-unwatch (watch-id)
  "Stop the HEAD watch WATCH-ID."
  (interactive
   (list (completing-read "Stop watch: "
                          (mapcar #'car scrutiny-agent-ui--watches) nil t)))
  (let* ((entry (assoc watch-id scrutiny-agent-ui--watches))
         (name (cadr entry)))
    (scrutiny-agent-ops-watch-stop (scrutiny-agent-ui-connection name)
                                   watch-id)
    (setq scrutiny-agent-ui--watches
          (assoc-delete-all watch-id scrutiny-agent-ui--watches))
    (message "scrutiny-agent: stopped watch %s" watch-id)))

(defun scrutiny-agent-ui--on-notification (_conn method params)
  "Surface agent-initiated notifications the UI cares about."
  (pcase method
    ("watch.headChanged"
     (let* ((watch-id (plist-get params :watchId))
            (entry (assoc watch-id scrutiny-agent-ui--watches)))
       (message "scrutiny-agent: HEAD changed in %s"
                (if entry (cddr entry) watch-id))))
    ("index.progress"
     (let ((current (plist-get params :current))
           (total (plist-get params :total)))
       (when (and (integerp current) (integerp total) (> total 0))
         (message "scrutiny-agent: indexing %d/%d  %s"
                  current total
                  (or (plist-get params :filePath) "")))))))

(add-hook 'scrutiny-agent-notification-functions
          #'scrutiny-agent-ui--on-notification)

;;;###autoload
(defun scrutiny-agent-index (&optional path language host)
  "Build a symbol index of the remote workspace PATH for LANGUAGE.
Runs on the agent's bulk lane -- interactive work keeps flowing on
the same connection -- and reports progress in the echo area."
  (interactive)
  (let* ((name (or host (scrutiny-agent-ui-host)))
         (conn (scrutiny-agent-ui-connection name))
         (workspace (or path (scrutiny-agent-ui--read-remote-path
                              "Remote workspace")))
         (lang (or language
                   (scrutiny-agent-ui--buffer-language)
                   (scrutiny-agent-ops--language
                    (completing-read "Language: "
                                     (mapcar #'cdr
                                             scrutiny-agent-ops-languages)
                                     nil t))))
         (db (format "%s/%s-%s.db"
                     (string-remove-suffix "/" scrutiny-agent-ui-index-cache)
                     (file-name-nondirectory
                      (directory-file-name workspace))
                     (cdr (assq lang scrutiny-agent-ops-languages))))
         (indexer (scrutiny-agent-ui--with-error "index.create"
                    (scrutiny-agent-ops-index-create conn workspace lang db))))
    (unwind-protect
        (let ((result (scrutiny-agent-ui--with-error "index.run"
                        (scrutiny-agent-ops-index-run conn indexer))))
          (message "scrutiny-agent[%s]: indexed %s files, %s definitions -> %s"
                   name (plist-get result :filesIndexed)
                   (plist-get result :definitionsFound) db))
      (ignore-errors (scrutiny-agent-ops-index-destroy conn indexer)))))

;; ---------------------------------------------------------------------
;; Entry points
;; ---------------------------------------------------------------------

(defvar-keymap scrutiny-agent-command-map
  :doc "Prefix keymap for scrutiny-agent commands."
  "c" #'scrutiny-agent-connect
  "q" #'scrutiny-agent-disconnect
  "i" #'scrutiny-agent-info
  "p" #'scrutiny-agent-ping
  "L" #'scrutiny-agent-remote-logs
  "b" #'scrutiny-agent-browse
  "f" #'scrutiny-agent-view-file
  "s" #'scrutiny-agent-status
  "r" #'scrutiny-agent-branches
  "l" #'scrutiny-agent-log
  "d" #'scrutiny-agent-diff-working-tree
  "D" #'scrutiny-agent-diff-staged
  "F" #'scrutiny-agent-fetch
  "C" #'scrutiny-agent-clone
  "w" #'scrutiny-agent-watch
  "W" #'scrutiny-agent-unwatch
  "x" #'scrutiny-agent-index
  "e" #'scrutiny-agent-code-mode
  "v" #'scrutiny-agent-verify)

;;;###autoload
(defun scrutiny-agent-menu ()
  "Menu of every scrutiny-agent command.
Uses `scrutiny-agent-transient' when transient is available, and
`completing-read' over `scrutiny-agent-command-map' when it is not.
This is the name worth remembering either way."
  (interactive)
  (if (and (require 'transient nil t)
           (require 'scrutiny-agent-menu nil t))
      (call-interactively #'scrutiny-agent-transient)
    (let* ((commands
            (let (out)
              (map-keymap (lambda (_key def)
                            (when (symbolp def) (push def out)))
                          scrutiny-agent-command-map)
              (nreverse out)))
           (choice (completing-read "scrutiny-agent: "
                                    (mapcar #'symbol-name commands) nil t)))
      (call-interactively (intern choice)))))

(declare-function scrutiny-agent-transient "scrutiny-agent-menu")
(declare-function scrutiny-agent-verify "scrutiny-agent-verify")
(autoload 'scrutiny-agent-verify "scrutiny-agent-verify" nil t)
(declare-function scrutiny-agent-code-mode "scrutiny-agent-xref")
(autoload 'scrutiny-agent-code-mode "scrutiny-agent-xref" nil t)

(provide 'scrutiny-agent-ui)
;;; scrutiny-agent-ui.el ends here
