;;; scrutiny-agent-ui-tests.el --- ERT tests for the ops/UI layers  -*- lexical-binding: t; -*-

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; Covers `scrutiny-agent-ops.el' (typed wrappers over every wire
;; method), `scrutiny-agent-ui.el' (the interactive command surface)
;; and `scrutiny-agent-verify.el' (the self-check harness).
;;
;; Pure-function tests run anywhere.  The rest are gated on
;; SCRUTINY_AGENT_BIN and drive a REAL agent over a local `sh'
;; transport against real git fixtures -- the same path a user gets
;; over ssh, minus the network.
;;
;; Run:  SCRUTINY_AGENT_BIN=build/agent/scrutiny-agent \
;;         emacs -Q --batch -L emacs -l emacs/scrutiny-agent-ui-tests.el \
;;         -f ert-run-tests-batch-and-exit

;;; Code:

(require 'ert)
(require 'cl-lib)
(require 'scrutiny-agent)
(require 'scrutiny-agent-ops)
(require 'scrutiny-agent-ui)
(require 'scrutiny-agent-verify)

;; ---------------------------------------------------------------------
;; Pure functions
;; ---------------------------------------------------------------------

(ert-deftest scrutiny-agent-ops-language-coercion ()
  (should (equal (scrutiny-agent-ops--language 2) 2))
  (should (equal (scrutiny-agent-ops--language "python") 2))
  (should (equal (scrutiny-agent-ops--language 'rust) 1))
  (should (equal (scrutiny-agent-ops--language "C++") 6))
  (should-error (scrutiny-agent-ops--language "cobol")))

(ert-deftest scrutiny-agent-ops-list-normalization ()
  ;; JSON arrays parse to vectors; every wrapper hands back lists.
  (should (equal (scrutiny-agent-ops--list [1 2 3]) '(1 2 3)))
  (should (equal (scrutiny-agent-ops--list nil) nil))
  (should (equal (scrutiny-agent-ops--list '(1 2)) '(1 2))))

(ert-deftest scrutiny-agent-ops-bool-distinguishes-false-from-absent ()
  ;; :json-false must not read as nil-the-absent-value, or every
  ;; boolean the agent returns false for would look like a missing key.
  (should (scrutiny-agent-ops-bool t))
  (should-not (scrutiny-agent-ops-bool :json-false))
  (should-not (scrutiny-agent-ops-bool nil)))

(ert-deftest scrutiny-agent-ops-error-names ()
  (should (equal (scrutiny-agent-ops-error-name 1005) "PERMISSION_DENIED"))
  (should (equal (scrutiny-agent-ops-error-name 1004) "LSP_FAILED"))
  (should (string-match-p "9999" (scrutiny-agent-ops-error-name 9999))))

(ert-deftest scrutiny-agent-verify-source-detection ()
  (should (scrutiny-agent-verify--source-p "main.py" 2))
  (should (scrutiny-agent-verify--source-p "lib.rs" 1))
  (should (scrutiny-agent-verify--source-p "a.cpp" 6))
  (should-not (scrutiny-agent-verify--source-p "README.md" 2))
  (should-not (scrutiny-agent-verify--source-p "noextension" 2)))

(ert-deftest scrutiny-agent-verify-check-records-each-outcome ()
  ;; A failing check must be recorded, not propagated: one broken
  ;; operation cannot be allowed to abandon the rest of the run.
  (let ((scrutiny-agent-verify--results nil)
        (scrutiny-agent-verify--covered nil))
    (scrutiny-agent-verify--check '("a.one") "fine")
    (scrutiny-agent-verify--check '("a.two") (error "boom"))
    (scrutiny-agent-verify--check '("a.three")
      (scrutiny-agent-verify--skip "not here"))
    (let ((results (nreverse scrutiny-agent-verify--results)))
      (should (equal (mapcar (lambda (r) (plist-get r :status)) results)
                     '("PASS" "FAIL" "SKIP")))
      (should (equal (sort (copy-sequence scrutiny-agent-verify--covered)
                           #'string<)
                     '("a.one" "a.three" "a.two"))))))

(ert-deftest scrutiny-agent-verify-check-reports-rpc-errors-by-name ()
  (let ((scrutiny-agent-verify--results nil)
        (scrutiny-agent-verify--covered nil))
    (scrutiny-agent-verify--check '("fs.readFile")
      (signal 'scrutiny-agent-rpc-error (list 1005 "outside the roots")))
    (let ((result (car scrutiny-agent-verify--results)))
      (should (equal (plist-get result :status) "FAIL"))
      (should (string-match-p "PERMISSION_DENIED" (plist-get result :detail))))))

(ert-deftest scrutiny-agent-ui-host-prefers-buffer-then-custom ()
  (with-temp-buffer
    (let ((scrutiny-agent-ui-host "configured"))
      (should (equal (scrutiny-agent-ui-host) "configured"))
      (setq scrutiny-agent-ui--host "buffer-local")
      (should (equal (scrutiny-agent-ui-host) "buffer-local")))))

(ert-deftest scrutiny-agent-ui-notification-handler-is-total ()
  ;; The handler runs on the reader path; an unknown method must be a
  ;; no-op rather than an error.
  (should-not (scrutiny-agent-ui--on-notification nil "unknown.method" nil))
  (scrutiny-agent-ui--on-notification nil "index.progress"
                                      '(:current 1 :total 2 :filePath "x")))

;; ---------------------------------------------------------------------
;; Integration fixtures
;; ---------------------------------------------------------------------

(defvar scrutiny-agent-ui-tests--install nil)

(defun scrutiny-agent-ui-tests--git (dir &rest args)
  (let ((process-environment
         (append '("GIT_AUTHOR_NAME=Scrutiny Test"
                   "GIT_AUTHOR_EMAIL=test@scrutiny.invalid"
                   "GIT_COMMITTER_NAME=Scrutiny Test"
                   "GIT_COMMITTER_EMAIL=test@scrutiny.invalid"
                   "GIT_CONFIG_GLOBAL=/dev/null"
                   "GIT_CONFIG_SYSTEM=/dev/null")
                 process-environment))
        (default-directory (file-name-as-directory dir)))
    (with-temp-buffer
      (unless (zerop (apply #'call-process "git" nil t nil args))
        (error "git %s failed in %s: %s" (string-join args " ") dir
               (buffer-string))))))

(defun scrutiny-agent-ui-tests--repo ()
  "Build a two-commit Python repo; return its directory."
  (let ((dir (make-temp-file "scra-ui-repo" t)))
    (scrutiny-agent-ui-tests--git dir "init" "-q" "--initial-branch=work")
    (with-temp-file (expand-file-name "lib.py" dir)
      (insert "def add(a, b):\n    return a + b\n"))
    (scrutiny-agent-ui-tests--git dir "add" "-A")
    (scrutiny-agent-ui-tests--git dir "commit" "-q" "-m" "first")
    (with-temp-file (expand-file-name "lib.py" dir)
      (insert "def add(a, b):\n    \"\"\"Add.\"\"\"\n    return a + b\n"))
    (scrutiny-agent-ui-tests--git dir "add" "-A")
    (scrutiny-agent-ui-tests--git dir "commit" "-q" "-m" "second")
    (scrutiny-agent-ui-tests--git dir "branch" "sidebranch")
    dir))

(defmacro scrutiny-agent-ui-tests--with-agent (varlist &rest body)
  "Connect a real agent over `sh' and bind (CONN REPO) from VARLIST."
  (declare (indent 1) (debug t))
  (let ((conn (nth 0 varlist)) (repo (nth 1 varlist)))
    `(let* ((bin (getenv "SCRUTINY_AGENT_BIN"))
            (install (make-temp-file "scra-ui-install" t))
            (,repo (scrutiny-agent-ui-tests--repo))
            (tmproot (file-name-as-directory temporary-file-directory))
            (scrutiny-agent-hosts
             `(("uitest" :transport "sh"
                :install-dir ,install
                :local-binary ,bin
                :agent-args ("--allow-root" ,tmproot
                             "--allow-root" ,(expand-file-name "~")))))
            (scrutiny-agent-ui-host "uitest")
            (,conn (scrutiny-agent-connect "uitest")))
       (unwind-protect (progn ,@body)
         (scrutiny-agent-disconnect "uitest")
         (delete-directory install t)
         (delete-directory ,repo t)))))

(defmacro scrutiny-agent-ui-tests--deftest (name &rest body)
  "Define an integration test gated on SCRUTINY_AGENT_BIN."
  (declare (indent 1) (debug t))
  `(ert-deftest ,name ()
     :tags '(integration)
     (skip-unless (getenv "SCRUTINY_AGENT_BIN"))
     ,@body))

;; ---------------------------------------------------------------------
;; ops: git reads
;; ---------------------------------------------------------------------

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ops-repo-metadata-integration
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (let ((meta (scrutiny-agent-ops-repo-metadata conn repo)))
      (should (equal (plist-get meta :currentBranch) "work"))
      (should-not (scrutiny-agent-ops-bool (plist-get meta :isBare)))
      ;; `path' is the work tree; the gitdir is reported separately.
      ;; libgit2 reports a work tree with a trailing slash; what
      ;; matters is that it names the work tree and not the gitdir.
      (should (equal (file-truename (plist-get meta :path))
                     (file-name-as-directory (file-truename repo))))
      (should (string-suffix-p ".git/" (plist-get meta :gitDir))))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ops-head-and-commits-agree
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (let* ((head (scrutiny-agent-ops-head-sha conn repo))
           (commits (scrutiny-agent-ops-commits conn repo nil 10)))
      (should (equal (length commits) 2))
      (should (equal (plist-get (car commits) :oid) head))
      (should (equal (plist-get (car commits) :summary) "second"))
      (should (equal (plist-get (cadr commits) :summary) "first")))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ops-branches-integration
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (let* ((branches (scrutiny-agent-ops-branches conn repo))
           (names (mapcar (lambda (b) (plist-get b :name)) branches)))
      (should (member "work" names))
      (should (member "sidebranch" names))
      (should (equal 1 (cl-count-if
                        (lambda (b) (scrutiny-agent-ops-bool
                                     (plist-get b :isHead)))
                        branches))))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ops-show-file-and-diff
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (let* ((head (scrutiny-agent-ops-head-sha conn repo))
           (commits (scrutiny-agent-ops-commits conn repo nil 10))
           (parent (plist-get (cadr commits) :oid))
           (at-head (scrutiny-agent-ops-show-file conn repo head "lib.py"))
           (at-parent (scrutiny-agent-ops-show-file conn repo parent "lib.py")))
      (should (string-match-p "Add\\." at-head))
      (should-not (string-match-p "Add\\." at-parent))
      (should (null (scrutiny-agent-ops-show-file conn repo head "absent.py")))
      (should (string-match-p
               "Add\\." (scrutiny-agent-ops-diff conn repo parent head
                                                 "lib.py")))
      (should (scrutiny-agent-ops-ancestor-p conn repo parent head))
      (should-not (scrutiny-agent-ops-ancestor-p conn repo head parent)))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ops-structured-diffs
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (let* ((head (scrutiny-agent-ops-head-sha conn repo))
           (diffs (scrutiny-agent-ops-diff-for-commit conn repo head)))
      (should (equal (length diffs) 1))
      (should (equal (plist-get (car diffs) :newPath) "lib.py"))
      (should (string-match-p "Add\\." (plist-get (car diffs) :patch))))
    ;; Clean tree: both mutable surfaces are empty.
    (should (null (scrutiny-agent-ops-working-tree-diff conn repo)))
    (should (null (scrutiny-agent-ops-staged-diff conn repo)))
    ;; ...and an unstaged edit shows up in exactly one of them.
    (with-temp-file (expand-file-name "lib.py" repo)
      (insert "def add(a, b):\n    return a + b  # edited\n"))
    (should (scrutiny-agent-ops-working-tree-diff conn repo))
    (should (null (scrutiny-agent-ops-staged-diff conn repo)))))

;; ---------------------------------------------------------------------
;; ops: fs, cache, credentials, watches
;; ---------------------------------------------------------------------

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ops-fs-integration
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (let* ((listing (scrutiny-agent-ops-list-directory conn repo))
           (names (mapcar (lambda (e) (plist-get e :name)) (cdr listing))))
      (should (member "lib.py" names))
      (should (member ".git" names))
      (should (scrutiny-agent-ops-bool
               (plist-get (cl-find ".git" (cdr listing)
                                   :key (lambda (e) (plist-get e :name))
                                   :test #'equal)
                          :isDir))))
    (should (string-match-p
             "def add" (scrutiny-agent-ops-read-file
                        conn (expand-file-name "lib.py" repo))))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ops-fs-sandbox-is-enforced
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (ignore repo)
    ;; The agent runs with --allow-root on tmp and $HOME only.
    (let ((err (should-error (scrutiny-agent-ops-read-file conn "/etc/passwd")
                             :type 'scrutiny-agent-rpc-error)))
      (should (equal (nth 1 err) 1005)))
    ;; ...and fs.selftest must agree with that, not contradict it.
    (should-not (scrutiny-agent-ops-bool
                 (plist-get (scrutiny-agent-ops-fs-selftest conn)
                            :succeeded)))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ops-diffcache-integration
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (ignore repo)
    (let ((cache (make-temp-file "scra-ui-cache" t))
          (value (list :fileExistsInTo t
                       :isForcePush scrutiny-agent-ops-false
                       :hunksJSON "[]")))
      (unwind-protect
          (progn
            (should (null (scrutiny-agent-ops-diffcache-get
                           conn cache "aaa" "bbb" "f.py")))
            (scrutiny-agent-ops-diffcache-put conn cache "aaa" "bbb" "f.py"
                                              value)
            (let ((got (scrutiny-agent-ops-diffcache-get
                        conn cache "aaa" "bbb" "f.py")))
              (should got)
              (should (equal (plist-get got :hunksJSON) "[]")))
            (should (>= (scrutiny-agent-ops-diffcache-prune conn cache -1) 1))
            (should (null (scrutiny-agent-ops-diffcache-get
                           conn cache "aaa" "bbb" "f.py"))))
        (delete-directory cache t)))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ops-credential-broker
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (ignore repo)
    (let* ((secret "ui-test-secret-value")
           (scrutiny-agent-credential-function (lambda (_p) secret))
           (result (scrutiny-agent-ops-cred-selftest
                    conn "ui-op" "Password for 'https://x':")))
      (should (equal (plist-get result :got) secret))
      (should (equal (plist-get result :askpassExit) 0)))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ops-watch-integration
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (let* ((fired nil)
           (watch-id (scrutiny-agent-ops-watch-head conn repo))
           (scrutiny-agent-notification-functions
            (list (lambda (_c method params)
                    (when (and (equal method "watch.headChanged")
                               (equal (plist-get params :watchId) watch-id))
                      (setq fired t))))))
      (should (stringp watch-id))
      ;; Let the watcher settle before moving HEAD, so the change is
      ;; observed rather than folded into startup.
      (let ((deadline (+ (float-time) 1)))
        (while (< (float-time) deadline) (accept-process-output nil 0.05)))
      (scrutiny-agent-ui-tests--git repo "checkout" "-q" "sidebranch")
      (let ((deadline (+ (float-time) 20)))
        (while (and (not fired) (< (float-time) deadline))
          (accept-process-output nil 0.05)))
      (should fired)
      (scrutiny-agent-ops-watch-stop conn watch-id)
      ;; The connection stays healthy after a stop notification.
      (should (scrutiny-agent-ops-debug conn 4)))))

;; ---------------------------------------------------------------------
;; ops: LSP (skips cleanly with no server installed)
;; ---------------------------------------------------------------------

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ops-lsp-integration
  (skip-unless (executable-find "pylsp"))
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (let* ((file (expand-file-name "lib.py" repo))
           (content (scrutiny-agent-ops-read-file conn file))
           (symbols (scrutiny-agent-ops-document-symbols
                     conn repo 2 file content)))
      (should symbols)
      (should (cl-find "add" symbols
                       :key (lambda (s) (plist-get s :name)) :test #'equal))
      ;; Positional queries answer with the documented shape.
      (should (listp (scrutiny-agent-ops-goto-definition
                      conn repo 2 file content 0 4)))
      (should (listp (scrutiny-agent-ops-folding-ranges
                      conn repo 2 file content))))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ops-lsp-tunnel-integration
  (skip-unless (executable-find "pylsp"))
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    ;; The tunnel check inside the verify harness is the real exercise:
    ;; open, split an initialize frame mid-message, await the response,
    ;; close.
    (should (string-match-p
             "initialize" (scrutiny-agent-verify--tunnel conn repo 2)))))

;; ---------------------------------------------------------------------
;; UI commands
;; ---------------------------------------------------------------------

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ui-browse-and-view
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (ignore conn)
    (let ((buffer (scrutiny-agent-browse repo)))
      (unwind-protect
          (with-current-buffer buffer
            (should (eq major-mode 'scrutiny-agent-browse-mode))
            (should (string-match-p "lib.py" (buffer-string)))
            ;; Every listed line carries the entry data RET acts on.
            (goto-char (point-min))
            (forward-line 2)
            (should (get-text-property (line-beginning-position)
                                       'scrutiny-entry)))
        (kill-buffer buffer)))
    (let ((buffer (scrutiny-agent-view-file (expand-file-name "lib.py" repo))))
      (unwind-protect
          (with-current-buffer buffer
            (should (string-match-p "def add" (buffer-string)))
            ;; Read-only: the agent's fs surface cannot write back.
            (should buffer-read-only))
        (kill-buffer buffer)))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ui-status-and-log
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (ignore conn)
    (let ((buffer (scrutiny-agent-status repo)))
      (unwind-protect
          (with-current-buffer buffer
            (should (string-match-p "branch      work" (buffer-string)))
            (should (string-match-p "uncommitted no" (buffer-string))))
        (kill-buffer buffer)))
    (let ((buffer (scrutiny-agent-log repo)))
      (unwind-protect
          (with-current-buffer buffer
            (should (eq major-mode 'scrutiny-agent-log-mode))
            (should (string-match-p "second" (buffer-string)))
            (should (string-match-p "first" (buffer-string)))
            (goto-char (point-min))
            (forward-line 2)
            (should (get-text-property (line-beginning-position)
                                       'scrutiny-commit)))
        (kill-buffer buffer)))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ui-branches-and-checkout
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (let ((buffer (scrutiny-agent-branches repo)))
      (unwind-protect
          (with-current-buffer buffer
            (should (string-match-p "sidebranch" (buffer-string)))
            ;; Move to the sidebranch line and check it out for real.
            (goto-char (point-min))
            (should (search-forward "sidebranch" nil t))
            (beginning-of-line)
            (scrutiny-agent-branches-checkout)
            (should (equal (plist-get (scrutiny-agent-ops-repo-metadata
                                       conn repo)
                                      :currentBranch)
                           "sidebranch")))
        (kill-buffer (get-buffer (format "*scrutiny-agent-branches[%s]*"
                                         "uitest")))))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ui-commit-diff-buffer
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (let* ((head (scrutiny-agent-ops-head-sha conn repo))
           (buffer (scrutiny-agent-show-commit head repo)))
      (unwind-protect
          (with-current-buffer buffer
            (should (eq major-mode 'diff-mode))
            (should (string-match-p "lib.py" (buffer-string)))
            (should (string-match-p "^\\+.*Add\\." (buffer-string))))
        (kill-buffer buffer)))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-ui-info-buffer
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (ignore conn repo)
    (let ((buffer (scrutiny-agent-info)))
      (unwind-protect
          (with-current-buffer buffer
            (should (string-match-p "agent version" (buffer-string)))
            (should (string-match-p "allowed root" (buffer-string)))
            ;; With a sandbox in force the probe must say so.
            (should (string-match-p "/etc/passwd    denied" (buffer-string)))
            (should (string-match-p "meta.hello\\|git.headSha"
                                    (buffer-string))))
        (kill-buffer buffer)))))

;; ---------------------------------------------------------------------
;; The verify harness itself
;; ---------------------------------------------------------------------

(scrutiny-agent-ui-tests--deftest scrutiny-agent-verify-covers-every-capability
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    ;; Drive the harness directly (the command prompts) and assert both
    ;; that nothing failed and that every advertised capability was
    ;; exercised -- the same contract the Python conformance suite
    ;; enforces on the other side.
    (let ((scrutiny-agent-verify--results nil)
          (scrutiny-agent-verify--covered nil)
          (scrutiny-agent-credential-function (lambda (_p) "x"))
          (language (if (executable-find "pylsp") 2 nil)))
      (scrutiny-agent-verify--meta conn)
      (scrutiny-agent-verify--fs conn repo)
      (scrutiny-agent-verify--fs-meta conn repo)
      (scrutiny-agent-verify--fs-write conn repo nil)
      (scrutiny-agent-verify--git-read conn repo)
      (scrutiny-agent-verify--git-exec conn repo)
      (scrutiny-agent-verify--diffcache conn "scrutiny-cache/verify-test")
      (scrutiny-agent-verify--cred conn)
      (scrutiny-agent-verify--lsp conn repo (or language 2))
      (scrutiny-agent-verify--watch conn repo)
      (scrutiny-agent-verify--index conn repo language nil)
      (scrutiny-agent-verify--git-write conn repo nil)

      (let* ((results (nreverse scrutiny-agent-verify--results))
             (failures (cl-remove-if-not
                        (lambda (r) (equal (plist-get r :status) "FAIL"))
                        results))
             (uncovered (cl-remove-if
                         (lambda (c) (member c scrutiny-agent-verify--covered))
                         (scrutiny-agent--conn-capabilities conn))))
        (should (equal failures nil))
        ;; A capability the agent advertises but the harness never
        ;; drives is a hole in exactly the coverage this file promises.
        (should (equal uncovered nil))
        (should (> (length results) 20))))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-verify-report-buffer
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (ignore repo)
    (let* ((scrutiny-agent-verify--results nil)
           (scrutiny-agent-verify--covered nil)
           (_ (scrutiny-agent-verify--meta conn))
           (buffer (scrutiny-agent-verify--report
                    "uitest" conn (nreverse scrutiny-agent-verify--results)
                    '("git.clone"))))
      (unwind-protect
          (with-current-buffer buffer
            (should (string-match-p "PASS" (buffer-string)))
            (should (string-match-p "meta.stat" (buffer-string)))
            (should (string-match-p "Advertised but not exercised"
                                    (buffer-string)))
            (should (string-match-p "git.clone" (buffer-string))))
        (kill-buffer buffer)))))

(provide 'scrutiny-agent-ui-tests)
;;; scrutiny-agent-ui-tests.el ends here

;; ---------------------------------------------------------------------
;; xref / eldoc / imenu backends
;; ---------------------------------------------------------------------

(require 'scrutiny-agent-xref)

(ert-deftest scrutiny-agent-xref-uri-decoding ()
  (should (equal (scrutiny-agent-xref--uri-path "file:///src/lib.py")
                 "/src/lib.py"))
  ;; Servers percent-encode; a path with a space must survive.
  (should (equal (scrutiny-agent-xref--uri-path "file:///src/my%20file.py")
                 "/src/my file.py"))
  (should (equal (scrutiny-agent-xref--uri-path nil) "")))

(ert-deftest scrutiny-agent-xref-declines-without-context ()
  ;; The backend must return nil rather than signal, so other xref
  ;; backends still get their turn in an unrelated buffer.
  (with-temp-buffer
    (fundamental-mode)
    (should-not (scrutiny-agent-xref-backend))))

(ert-deftest scrutiny-agent-xref-position-is-zero-based ()
  (with-temp-buffer
    (insert "line one\nline two\n")
    (goto-char (point-min))
    (forward-line 1)
    (forward-char 3)
    ;; The protocol counts from zero; Emacs counts lines from one.
    (should (equal (scrutiny-agent-xref--position) (cons 1 3)))))

(ert-deftest scrutiny-agent-xref-symbol-flattening ()
  ;; documentSymbol answers come flat or nested depending on the server;
  ;; imenu needs every entry either way.
  (let ((nested '((:name "Outer" :children [(:name "inner1")
                                            (:name "inner2")]))))
    (should (equal (mapcar (lambda (s) (plist-get s :name))
                           (scrutiny-agent-xref--flatten-symbols nested))
                   '("Outer" "inner1" "inner2")))))

(ert-deftest scrutiny-agent-xref-location-accessors ()
  (let ((location (scrutiny-agent-xref-location-make
                   :host "h" :path "/src/a.py" :line 4 :character 2)))
    (should (equal (xref-location-group location) "/src/a.py"))
    ;; xref lines are 1-based; the protocol's are 0-based.
    (should (equal (xref-location-line location) 5))))

(ert-deftest scrutiny-agent-code-mode-installs-and-removes-hooks ()
  (with-temp-buffer
    (let ((original imenu-create-index-function))
      (scrutiny-agent-code-mode 1)
      (should (memq #'scrutiny-agent-xref-backend xref-backend-functions))
      (should (memq #'scrutiny-agent-eldoc-function
                    eldoc-documentation-functions))
      (should (eq imenu-create-index-function #'scrutiny-agent-imenu-index))
      (scrutiny-agent-code-mode -1)
      (should-not (memq #'scrutiny-agent-xref-backend xref-backend-functions))
      (should-not (memq #'scrutiny-agent-eldoc-function
                        eldoc-documentation-functions))
      (should (eq imenu-create-index-function original)))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-xref-definitions-integration
  (skip-unless (executable-find "pylsp"))
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (ignore conn)
    (with-temp-buffer
      (insert (with-temp-buffer
                (insert-file-contents (expand-file-name "lib.py" repo))
                (buffer-string)))
      (python-mode)
      (setq scrutiny-agent-ui--host "uitest"
            scrutiny-agent-ui--path (expand-file-name "lib.py" repo))
      (should (eq (scrutiny-agent-xref-backend) 'scrutiny-agent))
      ;; Point at `add' in "def add(a, b):".
      (goto-char (point-min))
      (should (search-forward "add" nil t))
      (backward-char 1)
      (let ((found (xref-backend-definitions 'scrutiny-agent "add")))
        (should (listp found))
        (dolist (item found)
          (should (scrutiny-agent-xref-location-p
                   (xref-item-location item))))))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-imenu-integration
  (skip-unless (executable-find "pylsp"))
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (ignore conn)
    (with-temp-buffer
      (insert-file-contents (expand-file-name "lib.py" repo))
      (python-mode)
      (setq scrutiny-agent-ui--host "uitest"
            scrutiny-agent-ui--path (expand-file-name "lib.py" repo))
      (let* ((index (scrutiny-agent-imenu-index))
             (names (apply #'append
                           (mapcar (lambda (group) (mapcar #'car (cdr group)))
                                   index))))
        (should index)
        (should (member "add" names))
        ;; Entries are grouped by symbol kind and point somewhere real.
        (dolist (group index)
          (should (stringp (car group)))
          (dolist (entry (cdr group))
            (should (integerp (cdr entry)))
            (should (<= (point-min) (cdr entry) (point-max)))))))))

(scrutiny-agent-ui-tests--deftest scrutiny-agent-eldoc-integration
  (skip-unless (executable-find "pylsp"))
  (scrutiny-agent-ui-tests--with-agent (conn repo)
    (ignore conn)
    (with-temp-buffer
      (insert-file-contents (expand-file-name "lib.py" repo))
      (python-mode)
      (setq scrutiny-agent-ui--host "uitest"
            scrutiny-agent-ui--path (expand-file-name "lib.py" repo))
      (goto-char (point-min))
      (should (search-forward "add" nil t))
      (backward-char 1)
      (let ((answer nil))
        ;; eldoc answers asynchronously: the function reports that a
        ;; reply is coming, and the callback delivers it.
        (should (scrutiny-agent-eldoc-function
                 (lambda (text &rest _) (setq answer text))))
        (let ((deadline (+ (float-time) 30)))
          (while (and (not answer) (< (float-time) deadline))
            (accept-process-output nil 0.05)))
        (when answer (should (stringp answer)))))))
