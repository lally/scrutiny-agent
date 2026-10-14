;;; scrutiny-agent-fs-tests.el --- ERT tests for the fs accelerator  -*- lexical-binding: t; -*-

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; This handler stands between Emacs and the user's files, so the tests
;; are mostly about fidelity: what comes back must be byte-identical to
;; what is on disk, and what Emacs reports about a file must match what
;; the filesystem says.  A subtle bug here loses work.
;;
;; The "remote" is this machine, reached through a `/ssh:HOST:' name
;; that TRAMP never connects to because the handler intercepts first --
;; which also means every assertion can be checked against the real
;; local file the agent just wrote.
;;
;; Run:  SCRUTINY_AGENT_BIN=build/agent/scrutiny-agent \
;;         emacs -Q --batch -L emacs -l emacs/scrutiny-agent-fs-tests.el \
;;         -f ert-run-tests-batch-and-exit

;;; Code:

(require 'ert)
(require 'cl-lib)
(require 'scrutiny-agent)
(require 'scrutiny-agent-ops)
(require 'scrutiny-agent-fs)
(require 'project)

;; ---------------------------------------------------------------------
;; Pure functions
;; ---------------------------------------------------------------------

(ert-deftest scrutiny-agent-fs-mode-string ()
  (should (equal (scrutiny-agent-fs--mode-string #o644 nil nil) "-rw-r--r--"))
  (should (equal (scrutiny-agent-fs--mode-string #o755 t nil) "drwxr-xr-x"))
  (should (equal (scrutiny-agent-fs--mode-string #o777 nil t) "lrwxrwxrwx"))
  (should (equal (scrutiny-agent-fs--mode-string #o600 nil nil) "-rw-------"))
  ;; setuid / setgid / sticky land in the execute positions.
  (should (equal (scrutiny-agent-fs--mode-string #o4755 nil nil) "-rwsr-xr-x"))
  (should (equal (scrutiny-agent-fs--mode-string #o2755 nil nil) "-rwxr-sr-x"))
  (should (equal (scrutiny-agent-fs--mode-string #o1777 t nil) "drwxrwxrwt")))

(ert-deftest scrutiny-agent-fs-attributes-shape ()
  (let* ((stat '(:exists t :isDir :json-false :isRegular t
                 :isSymlink :json-false :symlinkTarget ""
                 :size 42 :mtime 1700000000 :mode 420 :uid 1000 :gid 1000))
         (attrs (scrutiny-agent-fs--attributes stat)))
    (should (null (nth 0 attrs)))               ; regular file
    (should (equal (file-attribute-size attrs) 42))
    (should (equal (file-attribute-modes attrs) "-rw-r--r--"))
    (should (equal (file-attribute-user-id attrs) 1000))
    (should (equal (float-time (file-attribute-modification-time attrs))
                   1700000000.0)))
  ;; A directory reports t, a symlink reports its target.
  (should (eq (nth 0 (scrutiny-agent-fs--attributes
                      '(:exists t :isDir t :mode 493))) t))
  (should (equal (nth 0 (scrutiny-agent-fs--attributes
                         '(:exists t :isSymlink t :symlinkTarget "/tmp/x"
                           :mode 511)))
                 "/tmp/x"))
  ;; A missing file has no attributes at all, not empty ones.
  (should (null (scrutiny-agent-fs--attributes '(:exists :json-false)))))

(ert-deftest scrutiny-agent-fs-parse-only-matches-configured-hosts ()
  (let ((scrutiny-agent-hosts '(("devbox" :transport "ssh devbox")))
        (scrutiny-agent-fs-host-alist '(("devbox.corp" . "devbox"))))
    (should (equal (scrutiny-agent-fs--parse "/ssh:devbox:/src/a.py")
                   '("devbox" "/ssh:devbox:" "/src/a.py")))
    (should (equal (nth 0 (scrutiny-agent-fs--parse "/ssh:devbox.corp:/x"))
                   "devbox"))
    ;; Anything else must be left entirely alone.
    (should-not (scrutiny-agent-fs--parse "/ssh:other:/src/a.py"))
    (should-not (scrutiny-agent-fs--parse "/home/me/a.py"))
    (should-not (scrutiny-agent-fs--parse nil))))

(ert-deftest scrutiny-agent-fs-regexp-is-narrow ()
  ;; The regexp is the outermost safety boundary: a name it does not
  ;; match never reaches this code at all.
  (let* ((scrutiny-agent-hosts '(("devbox" :transport "x")))
         (regexp (scrutiny-agent-fs--file-name-regexp)))
    (should (string-match-p regexp "/ssh:devbox:/src/a.py"))
    (should (string-match-p regexp "/tsh:devbox:/src/a.py"))
    (should-not (string-match-p regexp "/ssh:otherhost:/src/a.py"))
    (should-not (string-match-p regexp "/home/me/a.py"))
    (should-not (string-match-p regexp "/ssh:devbox-staging:/src/a.py")))
  ;; With nothing configured it must match nothing whatsoever.
  (let ((scrutiny-agent-hosts nil))
    (should-not (string-match-p (scrutiny-agent-fs--file-name-regexp)
                                "/ssh:devbox:/x"))))

(ert-deftest scrutiny-agent-fs-mode-installs-and-removes-cleanly ()
  (let ((scrutiny-agent-hosts '(("devbox" :transport "x")))
        (original (copy-sequence file-name-handler-alist)))
    (unwind-protect
        (progn
          (scrutiny-agent-fs-mode 1)
          (should (rassq 'scrutiny-agent-fs-handler file-name-handler-alist))
          ;; Ahead of TRAMP: the first matching entry wins, and a
          ;; handler registered behind TRAMP's is never consulted at
          ;; all -- installed, matching, and completely inert.
          (should (scrutiny-agent-fs-active-p))
          ;; Still ahead after something else lazily loads TRAMP, which
          ;; re-registers by prepending.
          (require 'tramp)
          (should (scrutiny-agent-fs-active-p))
          (scrutiny-agent-fs-mode -1)
          (should-not (rassq 'scrutiny-agent-fs-handler
                             file-name-handler-alist))
          (should (equal file-name-handler-alist original)))
      (scrutiny-agent-fs-mode -1)
      (setq file-name-handler-alist original))))

;; ---------------------------------------------------------------------
;; Integration fixture
;; ---------------------------------------------------------------------

(defmacro scrutiny-agent-fs-tests--with-agent (varlist &rest body)
  "Bind (ROOT REMOTE) with a writable agent rooted at a temp directory.
ROOT is the real local path; REMOTE is the `/ssh:' name for it, which
the handler intercepts before TRAMP ever sees it."
  (declare (indent 1) (debug t))
  (let ((root (nth 0 varlist)) (remote (nth 1 varlist)))
    `(let* ((bin (getenv "SCRUTINY_AGENT_BIN"))
            (install (make-temp-file "scra-fs-install" t))
            (,root (file-truename (make-temp-file "scra-fs-root" t)))
            ;; Policy here comes from the exec line on purpose; the
            ;; client's default config would add its own allow-write
            ;; and roots on top, which is exactly what a test of a
            ;; READ-ONLY agent must not inherit.
            (scrutiny-agent-default-config nil)
            (scrutiny-agent-hosts
             `(("fstest" :transport "sh"
                :install-dir ,install
                :local-binary ,bin
                :agent-args ("--allow-root" ,,root
                             "--allow-root" ,(expand-file-name "~")
                             "--allow-write"))))
            (,remote (format "/ssh:fstest:%s/" ,root))
            (scrutiny-agent-fs-cache-ttl 0))   ; assert on truth, not cache
       (scrutiny-agent-connect "fstest")
       (scrutiny-agent-fs-mode 1)
       (unwind-protect (progn ,@body)
         (scrutiny-agent-fs-mode -1)
         (scrutiny-agent-fs-flush-cache)
         (scrutiny-agent-disconnect "fstest")
         (delete-directory install t)
         (delete-directory ,root t)))))

;; A root configured through a symlink.  The agent enforces the
;; canonical path and reports both spellings; file names written the
;; configured way must be served, not answered from structure as
;; "outside the roots" (which is what every macOS temp path, and any
;; /home that is a symlink, looked like before).
(ert-deftest scrutiny-agent-fs-root-given-through-a-symlink-is-inside ()
  :tags '(integration)
  (skip-unless (getenv "SCRUTINY_AGENT_BIN"))
  (let* ((bin (getenv "SCRUTINY_AGENT_BIN"))
         (install (make-temp-file "scra-fs-install" t))
         (real (file-truename (make-temp-file "scra-fs-real" t)))
         (link (concat (file-truename (make-temp-file "scra-fs-link" t)) "/l"))
         (scrutiny-agent-default-config nil)
         (scrutiny-agent-hosts
          `(("fstest" :transport "sh" :install-dir ,install :local-binary ,bin
             :agent-args ("--allow-root" ,link))))
         (scrutiny-agent-fs-cache-ttl 0)
         (delegated nil))
    (make-symbolic-link real link)
    (with-temp-file (expand-file-name "f.txt" real) (insert "x\n"))
    (scrutiny-agent-connect "fstest")
    (scrutiny-agent-fs-mode 1)
    (unwind-protect
        (cl-letf* ((delegate (symbol-function 'scrutiny-agent-fs--delegate))
                   ((symbol-function 'scrutiny-agent-fs--delegate)
                    (lambda (operation args)
                      (push operation delegated)
                      (funcall delegate operation args))))
          (let ((roots (scrutiny-agent-fs--roots "fstest")))
            (should (member real roots))
            (should (member link roots)))
          ;; Both spellings are inside, and both are answered by the agent.
          (should (scrutiny-agent-fs--in-roots-p "fstest" link))
          (should (scrutiny-agent-fs--in-roots-p "fstest" real))
          (should (file-exists-p (format "/ssh:fstest:%s/f.txt" link)))
          (should (file-exists-p (format "/ssh:fstest:%s/f.txt" real)))
          (should (file-accessible-directory-p (format "/ssh:fstest:%s/" link)))
          (should-not (file-exists-p (format "/ssh:fstest:%s/nope.txt" link)))
          (should-not delegated))
      (scrutiny-agent-fs-mode -1)
      (scrutiny-agent-fs-flush-cache)
      (scrutiny-agent-disconnect "fstest")
      (delete-directory install t)
      (delete-file link)
      (delete-directory (file-name-directory link) t)
      (delete-directory real t))))

;; `executable-find' with REMOTE asks the handler for `exec-path';
;; magit does that on its way to "not inside a repository".  TRAMP's
;; answer opens a connection.  Ours is empty and immediate.
(ert-deftest scrutiny-agent-fs-exec-path-never-reaches-tramp ()
  :tags '(integration)
  (skip-unless (getenv "SCRUTINY_AGENT_BIN"))
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (let ((delegated nil) (tramp-cmds 0))
      (cl-letf* ((delegate (symbol-function 'scrutiny-agent-fs--delegate))
                 ((symbol-function 'scrutiny-agent-fs--delegate)
                  (lambda (operation args)
                    (push operation delegated)
                    (funcall delegate operation args)))
                 ((symbol-function 'tramp-send-command)
                  (lambda (&rest _) (cl-incf tramp-cmds))))
        (let ((default-directory remote))
          (should-not (exec-path))
          (should-not (executable-find "git" t)))
        (should-not (memq 'exec-path delegated))
        (should (= 0 tramp-cmds))))))

(defmacro scrutiny-agent-fs-tests--deftest (name &rest body)
  (declare (indent 1) (debug t))
  `(ert-deftest ,name ()
     :tags '(integration)
     (skip-unless (getenv "SCRUTINY_AGENT_BIN"))
     ,@body))

(defun scrutiny-agent-fs-tests--write-local (path content)
  (let ((coding-system-for-write 'utf-8-unix))
    (with-temp-file path (insert content))))

(defun scrutiny-agent-fs-tests--read-local (path)
  (with-temp-buffer
    (set-buffer-multibyte nil)
    (let ((coding-system-for-read 'binary))
      (insert-file-contents-literally path))
    (buffer-string)))

;; ---------------------------------------------------------------------
;; Predicates and attributes
;; ---------------------------------------------------------------------

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-predicates
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "a.txt" root)
                                          "hello\n")
    (make-directory (expand-file-name "sub" root))
    (should (file-exists-p (concat remote "a.txt")))
    (should-not (file-exists-p (concat remote "nope.txt")))
    (should (file-regular-p (concat remote "a.txt")))
    (should-not (file-directory-p (concat remote "a.txt")))
    (should (file-directory-p (concat remote "sub")))
    (should (file-readable-p (concat remote "a.txt")))
    (should (file-writable-p (concat remote "a.txt")))
    ;; A file that does not exist yet is writable if its directory is.
    (should (file-writable-p (concat remote "brand-new.txt")))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-attributes-match-reality
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "a.txt" root)
                                          "0123456789")
    (set-file-modes (expand-file-name "a.txt" root) #o640)
    (let ((mine (file-attributes (concat remote "a.txt")))
          (real (file-attributes (expand-file-name "a.txt" root))))
      (should (equal (file-attribute-size mine) (file-attribute-size real)))
      (should (equal (file-attribute-modes mine) (file-attribute-modes real)))
      (should (equal (file-attribute-type mine) (file-attribute-type real)))
      (should (equal (file-attribute-user-id mine)
                     (file-attribute-user-id real)))
      ;; Whole seconds: the protocol reports mtime in seconds.
      (should (equal (floor (float-time
                             (file-attribute-modification-time mine)))
                     (floor (float-time
                             (file-attribute-modification-time real))))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-symlinks
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "target.txt" root)
                                          "t\n")
    (make-symbolic-link "target.txt" (expand-file-name "link.txt" root))
    (should (equal (file-symlink-p (concat remote "link.txt")) "target.txt"))
    (should-not (file-symlink-p (concat remote "target.txt")))
    ;; The link resolves for content and for type.
    (should (file-regular-p (concat remote "link.txt")))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-modes
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "a.txt" root) "x")
    (set-file-modes (concat remote "a.txt") #o600)
    (should (equal (file-modes (expand-file-name "a.txt" root)) #o600))
    (should (equal (file-modes (concat remote "a.txt")) #o600))))

;; ---------------------------------------------------------------------
;; Reading -- fidelity is the whole point
;; ---------------------------------------------------------------------

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-read-round-trip
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (let ((content "line one\nline two\nline three\n"))
      (scrutiny-agent-fs-tests--write-local (expand-file-name "a.txt" root)
                                            content)
      (with-temp-buffer
        (insert-file-contents (concat remote "a.txt"))
        (should (equal (buffer-string) content))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-read-unicode
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (let ((content "héllo wörld \U0001F600 你好\n"))
      (scrutiny-agent-fs-tests--write-local (expand-file-name "u.txt" root)
                                            content)
      (with-temp-buffer
        (insert-file-contents (concat remote "u.txt"))
        ;; Decoded as UTF-8, exactly as a local read would.
        (should (equal (buffer-string) content))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-read-is-byte-exact
  (scrutiny-agent-fs-tests--with-agent (root remote)
    ;; Every byte value, including NUL and invalid UTF-8 sequences.
    (let ((blob (apply #'unibyte-string (number-sequence 0 255))))
      (let ((coding-system-for-write 'binary))
        (with-temp-file (expand-file-name "blob.bin" root)
          (set-buffer-multibyte nil)
          (insert blob)))
      (let ((local (scrutiny-agent-fs--handle-file-local-copy
                    (concat remote "blob.bin"))))
        (unwind-protect
            (should (equal (scrutiny-agent-fs-tests--read-local local) blob))
          (delete-file local))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-read-empty-file
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "e.txt" root) "")
    (with-temp-buffer
      (insert-file-contents (concat remote "e.txt"))
      (should (equal (buffer-string) "")))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-read-large-file
  (scrutiny-agent-fs-tests--with-agent (root remote)
    ;; Comfortably past the frame cap, so the reply is streamed.
    (let ((content (mapconcat (lambda (n) (format "line %06d\n" n))
                              (number-sequence 1 40000) "")))
      (scrutiny-agent-fs-tests--write-local (expand-file-name "big.txt" root)
                                            content)
      (with-temp-buffer
        (insert-file-contents (concat remote "big.txt"))
        (should (equal (buffer-size) (length content)))
        (should (equal (buffer-string) content))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-read-with-visit
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "v.txt" root)
                                          "visited\n")
    (with-temp-buffer
      (insert-file-contents (concat remote "v.txt") t)
      ;; The buffer must be visiting the REMOTE name, not the temp file
      ;; the bytes arrived in -- otherwise autosave and the modeline
      ;; would both point at a file that is about to be deleted.
      (should (equal buffer-file-name (concat remote "v.txt")))
      (should-not (buffer-modified-p))
      (should (verify-visited-file-modtime (current-buffer))))))

;; ---------------------------------------------------------------------
;; Writing
;; ---------------------------------------------------------------------

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-write-round-trip
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (with-temp-buffer
      (insert "written through the agent\n")
      (write-region (point-min) (point-max) (concat remote "w.txt")))
    (should (equal (scrutiny-agent-fs-tests--read-local
                    (expand-file-name "w.txt" root))
                   "written through the agent\n"))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-write-unicode-encodes
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (let ((content "héllo \U0001F600\n"))
      (with-temp-buffer
        (set-buffer-file-coding-system 'utf-8-unix)
        (insert content)
        (write-region (point-min) (point-max) (concat remote "u.txt")))
      ;; The bytes on disk are the UTF-8 encoding, not a mangling.
      (should (equal (scrutiny-agent-fs-tests--read-local
                      (expand-file-name "u.txt" root))
                     (encode-coding-string content 'utf-8-unix))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-write-then-read-is-identity
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (ignore root)
    (let ((content "alpha\nbeta\n\tgamma\r\n\ndelta"))
      (with-temp-buffer
        (insert content)
        (write-region (point-min) (point-max) (concat remote "rt.txt")))
      (with-temp-buffer
        (insert-file-contents (concat remote "rt.txt"))
        (should (equal (buffer-string) content))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-write-preserves-modes
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "s.sh" root) "#!\n")
    (set-file-modes (expand-file-name "s.sh" root) #o755)
    (with-temp-buffer
      (insert "#!/bin/sh\necho hi\n")
      (write-region (point-min) (point-max) (concat remote "s.sh")))
    ;; Overwriting an executable must not silently drop its +x bit.
    (should (equal (file-modes (expand-file-name "s.sh" root)) #o755))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-write-is-atomic
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (with-temp-buffer
      (insert "content\n")
      (write-region (point-min) (point-max) (concat remote "atomic.txt")))
    ;; The temp file the write landed in must not survive.
    (should (equal (directory-files root nil "scrutiny-write") nil))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-write-large-file
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (let ((content (mapconcat (lambda (n) (format "row %06d\n" n))
                              (number-sequence 1 30000) "")))
      (with-temp-buffer
        (insert content)
        (write-region (point-min) (point-max) (concat remote "big.txt")))
      (should (equal (scrutiny-agent-fs-tests--read-local
                      (expand-file-name "big.txt" root))
                     content)))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-save-buffer-cycle
  ;; The operation the whole feature exists for: open, edit, save,
  ;; reopen, and see the edit.
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "edit.txt" root)
                                          "original\n")
    (with-temp-buffer
      (insert-file-contents (concat remote "edit.txt") t)
      (setq buffer-file-name (concat remote "edit.txt"))
      (goto-char (point-max))
      (insert "appended by the test\n")
      (let ((create-lockfiles nil))
        (save-buffer))
      (should-not (buffer-modified-p)))
    (should (equal (scrutiny-agent-fs-tests--read-local
                    (expand-file-name "edit.txt" root))
                   "original\nappended by the test\n"))
    (with-temp-buffer
      (insert-file-contents (concat remote "edit.txt"))
      (should (equal (buffer-string) "original\nappended by the test\n")))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-write-falls-back-read-only
  ;; An agent without --allow-write must not lose the save: the write
  ;; has to reach TRAMP instead of failing.
  (let* ((bin (getenv "SCRUTINY_AGENT_BIN"))
         (install (make-temp-file "scra-ro-install" t))
         (root (file-truename (make-temp-file "scra-ro-root" t)))
         (scrutiny-agent-default-config nil)
         (scrutiny-agent-hosts
          `(("rotest" :transport "sh" :install-dir ,install
             :local-binary ,bin :agent-args ("--allow-root" ,root))))
         (conn (scrutiny-agent-connect "rotest")))
    (unwind-protect
        (progn
          (should-not (member "fs.writeFile"
                              (scrutiny-agent--conn-capabilities conn)))
          (should-not (scrutiny-agent-fs--writable-p "rotest"))
          ;; Reads are still accelerated on a read-only agent.
          (scrutiny-agent-fs-mode 1)
          (scrutiny-agent-fs-tests--write-local
           (expand-file-name "r.txt" root) "readable\n")
          (with-temp-buffer
            (insert-file-contents (format "/ssh:rotest:%s/r.txt" root))
            (should (equal (buffer-string) "readable\n"))))
      (scrutiny-agent-fs-mode -1)
      (scrutiny-agent-disconnect "rotest")
      (delete-directory install t)
      (delete-directory root t))))

;; ---------------------------------------------------------------------
;; Directories
;; ---------------------------------------------------------------------

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-directory-files
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (dolist (name '("a.txt" "b.txt" "c.org"))
      (scrutiny-agent-fs-tests--write-local (expand-file-name name root) "x"))
    (make-directory (expand-file-name "sub" root))
    (should (equal (directory-files remote)
                   (directory-files root)))
    (should (equal (directory-files remote nil "\\.txt\\'")
                   '("a.txt" "b.txt")))
    (should (equal (directory-files remote t "\\.org\\'")
                   (list (concat remote "c.org"))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-directory-files-and-attributes
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "a.txt" root)
                                          "12345")
    (make-directory (expand-file-name "sub" root))
    (let ((mine (directory-files-and-attributes remote))
          (real (directory-files-and-attributes root)))
      (should (equal (mapcar #'car mine) (mapcar #'car real)))
      (should (equal (file-attribute-size (cdr (assoc "a.txt" mine))) 5))
      (should (eq (file-attribute-type (cdr (assoc "sub" mine))) t)))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-completion
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (dolist (name '("alpha.txt" "alps.txt" "beta.txt"))
      (scrutiny-agent-fs-tests--write-local (expand-file-name name root) "x"))
    (make-directory (expand-file-name "alphadir" root))
    (let ((completions (file-name-all-completions "alp" remote)))
      (should (equal (sort completions #'string<)
                     '("alpha.txt" "alphadir/" "alps.txt"))))
    ;; Directories carry the trailing slash, which is what lets the
    ;; minibuffer descend without another round trip.
    (should (member "alphadir/" (file-name-all-completions "alpha" remote)))
    (should (equal (file-name-completion "alph" remote) "alpha"))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-empty-directory
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (make-directory (expand-file-name "empty" root))
    (should (equal (directory-files (concat remote "empty/"))
                   '("." "..")))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-insert-directory
  ;; dired's listing, rendered from the agent's attributes.
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "a.txt" root)
                                          "12345")
    (make-directory (expand-file-name "sub" root))
    (with-temp-buffer
      (insert-directory remote "-al" nil t)
      (let ((text (buffer-string)))
        (should (string-match-p "a\\.txt" text))
        (should (string-match-p "sub" text))
        ;; Real ls-style rows, or dired cannot parse them.
        (should (string-match-p "^ *-rw" text))
        (should (string-match-p "^ *drwx" text))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-dired-opens
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "a.txt" root) "x")
    (let ((buffer (dired-noselect remote)))
      (unwind-protect
          (with-current-buffer buffer
            (should (eq major-mode 'dired-mode))
            (should (string-match-p "a\\.txt" (buffer-string))))
        (kill-buffer buffer)))))

;; ---------------------------------------------------------------------
;; Mutations
;; ---------------------------------------------------------------------

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-make-directory
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (make-directory (concat remote "one"))
    (should (file-directory-p (expand-file-name "one" root)))
    (make-directory (concat remote "a/b/c") t)
    (should (file-directory-p (expand-file-name "a/b/c" root)))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-delete
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "gone.txt" root)
                                          "x")
    (delete-file (concat remote "gone.txt"))
    (should-not (file-exists-p (expand-file-name "gone.txt" root)))
    (make-directory (expand-file-name "tree/inner" root) t)
    (scrutiny-agent-fs-tests--write-local
     (expand-file-name "tree/inner/f.txt" root) "x")
    (delete-directory (concat remote "tree") t)
    (should-not (file-exists-p (expand-file-name "tree" root)))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-rename-and-copy
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "from.txt" root)
                                          "payload\n")
    (rename-file (concat remote "from.txt") (concat remote "to.txt"))
    (should-not (file-exists-p (expand-file-name "from.txt" root)))
    (should (equal (scrutiny-agent-fs-tests--read-local
                    (expand-file-name "to.txt" root))
                   "payload\n"))
    (copy-file (concat remote "to.txt") (concat remote "copy.txt"))
    (should (equal (scrutiny-agent-fs-tests--read-local
                    (expand-file-name "copy.txt" root))
                   "payload\n"))
    ;; Refusing to clobber is part of the contract.
    (should-error (rename-file (concat remote "to.txt")
                               (concat remote "copy.txt")))))

;; ---------------------------------------------------------------------
;; Names and caching
;; ---------------------------------------------------------------------

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-expand-file-name
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (ignore root)
    (should (equal (expand-file-name "b.txt" remote) (concat remote "b.txt")))
    (should (equal (expand-file-name "sub/../b.txt" remote)
                   (concat remote "b.txt")))
    ;; `~' resolves to the remote home without a TRAMP round trip.
    (let ((home (scrutiny-agent-fs--home "fstest")))
      (should (equal (expand-file-name "/ssh:fstest:~/x.txt")
                     (format "/ssh:fstest:%s/x.txt" home))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-truename
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "real.txt" root)
                                          "x")
    (make-symbolic-link "real.txt" (expand-file-name "alias.txt" root))
    (should (equal (file-truename (concat remote "alias.txt"))
                   (concat remote "real.txt")))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-cache-follows-our-writes
  ;; A stale cache after our own write would show the user the previous
  ;; contents of a file they just saved.
  (let ((scrutiny-agent-fs-cache-ttl 60.0))
    (scrutiny-agent-fs-tests--with-agent (root remote)
      (let ((scrutiny-agent-fs-cache-ttl 60.0))
        (scrutiny-agent-fs-tests--write-local (expand-file-name "c.txt" root)
                                              "first")
        (should (equal (file-attribute-size
                        (file-attributes (concat remote "c.txt"))) 5))
        (with-temp-buffer
          (insert "considerably longer content")
          (write-region (point-min) (point-max) (concat remote "c.txt")))
        (should (equal (file-attribute-size
                        (file-attributes (concat remote "c.txt")))
                       (length "considerably longer content")))
        ;; A new file must be visible immediately too.
        (with-temp-buffer
          (insert "new")
          (write-region (point-min) (point-max) (concat remote "new.txt")))
        (should (member "new.txt" (directory-files remote)))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-cache-can-be-flushed
  (let ((scrutiny-agent-fs-cache-ttl 60.0))
    (scrutiny-agent-fs-tests--with-agent (root remote)
      (let ((scrutiny-agent-fs-cache-ttl 60.0))
        (scrutiny-agent-fs-tests--write-local (expand-file-name "x.txt" root)
                                              "one")
        (should (file-exists-p (concat remote "x.txt")))
        ;; A change made behind our back is invisible until flushed --
        ;; documented behavior, asserted so it stays deliberate.
        (delete-file (expand-file-name "x.txt" root))
        (should (file-exists-p (concat remote "x.txt")))
        (scrutiny-agent-fs-flush-cache)
        (should-not (file-exists-p (concat remote "x.txt")))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-vc-probe-is-short-circuited
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "a.txt" root) "x")
    ;; vc would otherwise walk the tree looking for version control on
    ;; every find-file -- dozens of round trips, and magit does not use it.
    (should-not (vc-registered (concat remote "a.txt")))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-modtime-detects-change
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "m.txt" root)
                                          "one\n")
    (with-temp-buffer
      (insert-file-contents (concat remote "m.txt") t)
      (should (verify-visited-file-modtime (current-buffer)))
      ;; Touch it behind Emacs's back, far enough for a 1-second mtime.
      (sleep-for 1.1)
      (scrutiny-agent-fs-tests--write-local (expand-file-name "m.txt" root)
                                            "two\n")
      (scrutiny-agent-fs-flush-cache)
      (should-not (verify-visited-file-modtime (current-buffer))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-leaves-other-hosts-alone
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (ignore root remote)
    ;; An unconfigured host is not ours; the handler must decline
    ;; before doing anything that could touch it.
    (should-not (scrutiny-agent-fs--parse "/ssh:someone-elses-box:/etc/passwd"))
    (should-not (string-match-p (scrutiny-agent-fs--file-name-regexp)
                                "/ssh:someone-elses-box:/etc/passwd"))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-local-files-are-untouched
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (ignore remote)
    ;; With the mode on, ordinary local file operations must behave
    ;; exactly as they always did.
    (let ((local (expand-file-name "local.txt" root)))
      (scrutiny-agent-fs-tests--write-local local "local content\n")
      (should (file-exists-p local))
      (with-temp-buffer
        (insert-file-contents local)
        (should (equal (buffer-string) "local content\n"))))))

;; ---------------------------------------------------------------------
;; A listing proves absence
;; ---------------------------------------------------------------------

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-missing-files-cost-nothing
  ;; Most of what Emacs asks about is not there: `.git', `.dir-locals.el',
  ;; backup files, lock files.  Each used to be a request of its own,
  ;; made to learn what the listing had already said.
  (let ((scrutiny-agent-fs-cache-ttl 60.0))
    (scrutiny-agent-fs-tests--with-agent (root remote)
      (let ((scrutiny-agent-fs-cache-ttl 60.0))
        (scrutiny-agent-fs-tests--write-local (expand-file-name "real.txt" root)
                                              "x")
        (should (file-exists-p (concat remote "real.txt")))
        (should (= 0 (scrutiny-agent-fs-tests--count-requests
                      (dolist (name '(".git" ".dir-locals.el" "real.txt~"
                                      "#real.txt#" ".#real.txt" ".editorconfig"
                                      ".projectile" "Makefile"))
                        (should-not (file-exists-p (concat remote name)))
                        (should-not (file-attributes (concat remote name)))
                        (should-not (file-directory-p (concat remote name)))))))
        ;; Nor does anything inside a directory that is not there.
        (should (= 0 (scrutiny-agent-fs-tests--count-requests
                      (should-not (file-exists-p
                                   (concat remote ".git/rebase-apply/onto")))
                      (should-not (file-exists-p
                                   (concat remote "no/such/dir/at/all")))
                      (should-not (file-exists-p
                                   (concat remote "real.txt/child"))))))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-walking-up-is-one-round-trip
  ;; `locate-dominating-file' climbs from a file to the root asking the
  ;; same question at every level.  The first question fetches every
  ;; level at once.
  (let ((scrutiny-agent-fs-cache-ttl 60.0))
    (scrutiny-agent-fs-tests--with-agent (root remote)
      (let ((scrutiny-agent-fs-cache-ttl 60.0)
            (deep (expand-file-name "one/two/three/four" root)))
        (make-directory deep t)
        (make-directory (expand-file-name "one/.git" root))
        (scrutiny-agent-fs-flush-cache)
        (let (found)
          (should (= 1 (scrutiny-agent-fs-tests--count-round-trips
                        (setq found (locate-dominating-file
                                     (concat remote "one/two/three/four/")
                                     ".git")))))
          (should (equal found (concat remote "one/"))))
        ;; A second walk, for something else, asks nothing at all.
        (should (= 0 (scrutiny-agent-fs-tests--count-requests
                      (should-not (locate-dominating-file
                                   (concat remote "one/two/three/four/")
                                   ".dir-locals.el")))))))))

;; ---------------------------------------------------------------------
;; Listings kept current by the agent
;; ---------------------------------------------------------------------

(defmacro scrutiny-agent-fs-tests--watching (varlist &rest body)
  "Like `scrutiny-agent-fs-tests--with-agent', with caching and watches on."
  (declare (indent 1) (debug t))
  `(let ((scrutiny-agent-fs-cache-ttl 0.2))
     (scrutiny-agent-fs-tests--with-agent ,varlist
       ;; A window far shorter than the test: anything still answered
       ;; from the cache afterwards is there because the agent vouches
       ;; for it, not because a timer has not run out.
       (let ((scrutiny-agent-fs-cache-ttl 0.2)
             (scrutiny-agent-fs-watch t))
         (skip-unless (scrutiny-agent-feature-p
                       (scrutiny-agent-connection "fstest") "fs.watch"))
         ,@body))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-watched-listing-does-not-expire
  (scrutiny-agent-fs-tests--watching (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "w.txt" root) "one")
    (should (equal 3 (file-attribute-size
                      (file-attributes (concat remote "w.txt")))))
    (sleep-for 0.5)                     ; well past the cache window
    (should (= 0 (scrutiny-agent-fs-tests--count-requests
                  (should (file-exists-p (concat remote "w.txt")))
                  (should (member "w.txt" (directory-files remote)))
                  (should-not (file-exists-p (concat remote "absent"))))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-outside-changes-are-pushed
  ;; The other half of never expiring: it must never be stale either.
  (scrutiny-agent-fs-tests--watching (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "w.txt" root) "one")
    (should (equal 3 (file-attribute-size
                      (file-attributes (concat remote "w.txt")))))
    ;; Modified, created and deleted behind our back.
    (scrutiny-agent-fs-tests--write-local (expand-file-name "w.txt" root)
                                          "much longer")
    (scrutiny-agent-fs-tests--write-local (expand-file-name "born.txt" root)
                                          "x")
    (scrutiny-agent-fs-tests--settle
     (lambda () (file-exists-p (concat remote "born.txt"))))
    (should (= 0 (scrutiny-agent-fs-tests--count-requests
                  (should (equal 11 (file-attribute-size
                                     (file-attributes
                                      (concat remote "w.txt")))))
                  (should (file-exists-p (concat remote "born.txt")))
                  (should (member "born.txt" (directory-files remote))))))
    (delete-file (expand-file-name "born.txt" root))
    (scrutiny-agent-fs-tests--settle
     (lambda () (not (file-exists-p (concat remote "born.txt")))))
    (should (= 0 (scrutiny-agent-fs-tests--count-requests
                  (should-not (file-exists-p (concat remote "born.txt")))
                  (should-not (member "born.txt"
                                      (directory-files remote))))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-outside-edit-is-noticed-before-typing
  ;; What `lock-file' is for: the buffer's file changed on disk, and the
  ;; first keystroke must say so.  Answered from what the agent pushed.
  (scrutiny-agent-fs-tests--watching (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "e.txt" root)
                                          "one\n")
    (let ((buffer (find-file-noselect (concat remote "e.txt")))
          (asked nil))
      (unwind-protect
          (with-current-buffer buffer
            (should (verify-visited-file-modtime buffer))
            (sleep-for 0.05)
            (scrutiny-agent-fs-tests--write-local
             (expand-file-name "e.txt" root) "someone else\n")
            (scrutiny-agent-fs-tests--settle
             (lambda () (not (verify-visited-file-modtime buffer))))
            (should (= 0 (scrutiny-agent-fs-tests--count-requests
                          (should-not (verify-visited-file-modtime buffer)))))
            (cl-letf (((symbol-function 'ask-user-about-supersession-threat)
                       (lambda (&rest _) (setq asked t))))
              (goto-char (point-max))
              (insert "typed"))
            (should asked))
        (with-current-buffer buffer (set-buffer-modified-p nil))
        (kill-buffer buffer)))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-directory-reached-by-symlink-is-watched
  ;; The agent reports changes by canonical path; the cache is filed
  ;; under the name that was asked for.
  (scrutiny-agent-fs-tests--watching (root remote)
    (make-directory (expand-file-name "real" root))
    (make-symbolic-link (expand-file-name "real" root)
                        (expand-file-name "link" root))
    (should-not (file-exists-p (concat remote "link/late.txt")))
    (scrutiny-agent-fs-tests--write-local
     (expand-file-name "real/late.txt" root) "x")
    (scrutiny-agent-fs-tests--settle
     (lambda () (file-exists-p (concat remote "link/late.txt"))))
    (should (file-exists-p (concat remote "link/late.txt")))
    (should (file-exists-p (concat remote "real/late.txt")))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-replaced-directory-forgets-what-was-in-it
  (scrutiny-agent-fs-tests--watching (root remote)
    (make-directory (expand-file-name "d/sub" root) t)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "d/sub/x" root) "x")
    (should (file-exists-p (concat remote "d/sub/x")))
    (delete-directory (expand-file-name "d" root) t)
    (scrutiny-agent-fs-tests--settle
     (lambda () (not (file-exists-p (concat remote "d")))))
    (should-not (file-exists-p (concat remote "d/sub/x")))
    (should-not (file-exists-p (concat remote "d/sub")))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-disconnect-ends-every-watch
  ;; A listing the agent vouched for is worth nothing once the
  ;; connection that carried the vouching is gone.
  (scrutiny-agent-fs-tests--watching (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "w.txt" root) "x")
    (should (file-exists-p (concat remote "w.txt")))
    (should (> (hash-table-count scrutiny-agent-fs--watched) 0))
    (scrutiny-agent-disconnect "fstest")
    (let ((deadline (+ (float-time) 5)))
      (while (and (scrutiny-agent-connection "fstest")
                  (< (float-time) deadline))
        (accept-process-output nil 0.05)))
    (should (= 0 (hash-table-count scrutiny-agent-fs--watched)))
    (should (= 0 (hash-table-count scrutiny-agent-fs--dir-cache)))
    (scrutiny-agent-connect "fstest")))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-watch-can-be-turned-off
  (let ((scrutiny-agent-fs-cache-ttl 60.0))
    (scrutiny-agent-fs-tests--with-agent (root remote)
      (let ((scrutiny-agent-fs-cache-ttl 60.0)
            (scrutiny-agent-fs-watch nil))
        (scrutiny-agent-fs-tests--write-local (expand-file-name "w.txt" root)
                                              "x")
        (should (file-exists-p (concat remote "w.txt")))
        (should (= 0 (hash-table-count scrutiny-agent-fs--watched)))))))

;; ---------------------------------------------------------------------
;; Saving
;; ---------------------------------------------------------------------

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-save-is-one-request
  ;; Edit and save an open file: the write, and nothing else.  Not a
  ;; lock file before, not a re-read of the directory after.
  (scrutiny-agent-fs-tests--watching (root remote)
    (scrutiny-agent-fs-tests--write-local (expand-file-name "s.txt" root)
                                          "one\n")
    (let ((buffer (find-file-noselect (concat remote "s.txt")))
          (make-backup-files nil))
      (unwind-protect
          (with-current-buffer buffer
            (should (= 1 (scrutiny-agent-fs-tests--count-requests
                          (goto-char (point-max))
                          (insert "two\n")
                          (save-buffer))))
            (should-not (buffer-modified-p))
            (should (verify-visited-file-modtime buffer))
            (should (equal (scrutiny-agent-fs-tests--read-local
                            (expand-file-name "s.txt" root))
                           "one\ntwo\n"))
            ;; What the cache now says is what is on disk, to the
            ;; nanosecond, with nothing asked.
            (should (= 0 (scrutiny-agent-fs-tests--count-requests
                          (should (time-equal-p
                                   (file-attribute-modification-time
                                    (file-attributes (concat remote "s.txt")))
                                   (file-attribute-modification-time
                                    (file-attributes
                                     (expand-file-name "s.txt" root)))))))))
        (kill-buffer buffer)))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-save-does-not-overwrite-an-unseen-change
  ;; The cache says the file is as we left it; the file is not.  (Here
  ;; because watching is off; in life, because the change is a moment
  ;; too recent to have been announced.)  The agent, which can look,
  ;; refuses, and the question Emacs would have asked is asked.
  (let ((scrutiny-agent-fs-cache-ttl 60.0))
    (scrutiny-agent-fs-tests--with-agent (root remote)
      (let* ((scrutiny-agent-fs-cache-ttl 60.0)
             (scrutiny-agent-fs-watch nil)
             (make-backup-files nil)
             (local (expand-file-name "c.txt" root))
             (buffer (progn (scrutiny-agent-fs-tests--write-local local "mine\n")
                            (find-file-noselect (concat remote "c.txt"))))
             (asked 0))
        (unwind-protect
            (with-current-buffer buffer
              (sleep-for 0.05)
              (scrutiny-agent-fs-tests--write-local local "theirs\n")
              (goto-char (point-max))
              (insert "more of mine\n")
              ;; Declined: nothing is written.
              (cl-letf (((symbol-function 'yes-or-no-p)
                         (lambda (&rest _) (setq asked (1+ asked)) nil)))
                (should-error (save-buffer) :type 'user-error))
              (should (= asked 1))
              (should (equal (scrutiny-agent-fs-tests--read-local local)
                             "theirs\n"))
              (should (buffer-modified-p))
              ;; Confirmed: ours goes in.  The first question comes from
              ;; Emacs itself this time -- it now knows the file changed
              ;; -- and it is not asked twice.
              (setq asked 0)
              (cl-letf (((symbol-function 'yes-or-no-p)
                         (lambda (&rest _) (setq asked (1+ asked)) t)))
                (save-buffer))
              (should (= asked 1))
              (should (equal (scrutiny-agent-fs-tests--read-local local)
                             "mine\nmore of mine\n")))
          (with-current-buffer buffer (set-buffer-modified-p nil))
          (kill-buffer buffer))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-new-file-can-be-created
  ;; `find-file' on a name that does not exist must produce an empty
  ;; buffer that saves, which depends on the read signalling
  ;; `file-missing' and nothing vaguer.
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (should-error (with-temp-buffer
                    (insert-file-contents (concat remote "nope.txt")))
                  :type 'file-missing)
    (let ((buffer (find-file-noselect (concat remote "fresh.txt")))
          (make-backup-files nil))
      (unwind-protect
          (with-current-buffer buffer
            (should (= (buffer-size) 0))
            (should (equal buffer-file-name (concat remote "fresh.txt")))
            (insert "created\n")
            (save-buffer)
            (should-not (buffer-modified-p)))
        (kill-buffer buffer)))
    (should (equal (scrutiny-agent-fs-tests--read-local
                    (expand-file-name "fresh.txt" root))
                   "created\n"))))

;; ---------------------------------------------------------------------
;; Nothing on the editing path goes to TRAMP
;; ---------------------------------------------------------------------

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-editing-never-reaches-tramp
  ;; Every operation handed to TRAMP is a chance for TRAMP to open a
  ;; connection of its own -- on the first keystroke in a buffer, as it
  ;; turned out, because locking a file is one of them.  Open, type,
  ;; save, list, complete: none of it may need the remote from anyone
  ;; but the agent.
  (scrutiny-agent-fs-tests--watching (root remote)
    (make-directory (expand-file-name "pkg" root))
    (scrutiny-agent-fs-tests--write-local (expand-file-name "pkg/m.py" root)
                                          "x = 1\n")
    (let ((needs-remote nil)
          (create-lockfiles t))
      (cl-letf (((symbol-function 'tramp-send-command)
                 (lambda (&rest _) (setq needs-remote t)
                   (error "TRAMP was asked to talk to the remote")))
                ((symbol-function 'tramp-maybe-open-connection)
                 (lambda (&rest _) (setq needs-remote t)
                   (error "TRAMP was asked to connect"))))
        (let ((buffer (find-file-noselect (concat remote "pkg/m.py"))))
          (unwind-protect
              (with-current-buffer buffer
                (goto-char (point-max))
                (insert "y = 2\n")                ; locks, in a local Emacs
                (save-buffer)                     ; backs up, unlocks
                (insert "z = 3\n")
                (save-buffer))
            (kill-buffer buffer)))
        (should (member "m.py" (directory-files (concat remote "pkg/"))))
        (should (file-name-all-completions "m" (concat remote "pkg/")))
        (should (file-accessible-directory-p (concat remote "pkg/")))
        (should-not (file-executable-p (concat remote "pkg/m.py")))
        (should (file-in-directory-p (concat remote "pkg/m.py") remote))
        (should (file-equal-p (concat remote "pkg/../pkg/m.py")
                              (concat remote "pkg/m.py")))
        (with-temp-buffer (dired-noselect (concat remote "pkg/"))))
      (should-not needs-remote)
      (should (equal (scrutiny-agent-fs-tests--read-local
                      (expand-file-name "pkg/m.py" root))
                     "x = 1\ny = 2\nz = 3\n")))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-home-relative-names
  ;; `~' in a remote name is the REMOTE home.  Emacs spells every name
  ;; under it that way, and none of them may be expanded against the
  ;; home directory of the machine Emacs runs on.
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (ignore root remote)
    (let* ((home (scrutiny-agent-fs--home "fstest"))
           (dir (make-temp-file (expand-file-name "scra-tilde-" home) t))
           (name (file-name-nondirectory dir))
           (tilde (format "/ssh:fstest:~/%s/h.txt" name)))
      (unwind-protect
          (progn
            (scrutiny-agent-fs-tests--write-local
             (expand-file-name "h.txt" dir) "x")
            ;; Pretend Emacs lives somewhere else entirely, as it does
            ;; on a laptop talking to a server.
            (let ((process-environment (cons "HOME=/nonexistent/laptop-home"
                                             process-environment)))
              (should (equal (file-truename tilde)
                             (format "/ssh:fstest:%s/h.txt"
                                     (file-truename dir))))
              (should (equal (nth 2 (scrutiny-agent-fs--resolve tilde))
                             (format "%s/%s/h.txt" home name)))
              (should (file-exists-p tilde))
              (should (equal 1 (file-attribute-size (file-attributes tilde))))
              (should (equal "x" (with-temp-buffer
                                   (insert-file-contents tilde)
                                   (buffer-string))))))
        (delete-directory dir t)))))

;; ---------------------------------------------------------------------
;; Directories too large to fetch for one answer
;; ---------------------------------------------------------------------

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-huge-directory-is-not-fetched-for-one-file
  (let ((scrutiny-agent-fs-cache-ttl 60.0))
    (scrutiny-agent-fs-tests--with-agent (root remote)
      (let ((scrutiny-agent-fs-cache-ttl 60.0)
            (scrutiny-agent-fs-listing-max-entries 5)
            (big (expand-file-name "big" root)))
        (make-directory big)
        (dotimes (i 12)
          (scrutiny-agent-fs-tests--write-local
           (expand-file-name (format "n%02d" i) big) "x"))
        ;; One file's attributes: answered, without the listing.
        (should (equal 1 (file-attribute-size
                          (file-attributes (concat remote "big/n03")))))
        (should-not (file-exists-p (concat remote "big/absent")))
        (should (eq 'too-large
                    (cdr (gethash (scrutiny-agent-fs--cache-key
                                   "fstest" (expand-file-name "big" root))
                                  scrutiny-agent-fs--dir-cache))))
        ;; Asked for on purpose, all of it comes.
        (should (= 12 (length (directory-files (concat remote "big/")
                                               nil "\\`n"))))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-regexp-covers-user-and-port
  (let* ((scrutiny-agent-hosts '(("devbox" :transport "ssh devbox")))
         (regexp (scrutiny-agent-fs--file-name-regexp)))
    (should (string-match-p regexp "/ssh:devbox:/src/a.py"))
    (should (string-match-p regexp "/ssh:me@devbox:/src/a.py"))
    (should (string-match-p regexp "/ssh:devbox#2222:/src/a.py"))
    (should (string-match-p regexp "/ssh:me@devbox#2222:/src/a.py"))
    (should-not (string-match-p regexp "/ssh:devbox2:/src/a.py"))
    (should-not (string-match-p regexp "/ssh:me@other:/src/devbox:/a.py"))
    (should-not (string-match-p regexp "/ssh:devbox@other:/src/a.py"))))

(provide 'scrutiny-agent-fs-tests)
;;; scrutiny-agent-fs-tests.el ends here

;; ---------------------------------------------------------------------
;; Round trips -- the thing that actually costs on a slow link
;; ---------------------------------------------------------------------

;; Two different quantities, and the difference is the point.  A REQUEST
;; is a frame sent; a ROUND TRIP is a wait for an answer that has not
;; arrived.  Requests sent together cost one round trip between them,
;; and on a slow link round trips are the whole cost.

(defmacro scrutiny-agent-fs-tests--count-requests (&rest body)
  "Number of agent requests BODY sends."
  `(let ((count 0))
     (cl-letf* ((original (symbol-function 'scrutiny-agent-async-request))
                ((symbol-function 'scrutiny-agent-async-request)
                 (lambda (&rest args)
                   (setq count (1+ count))
                   (apply original args))))
       ,@body)
     count))

(defmacro scrutiny-agent-fs-tests--count-round-trips (&rest body)
  "Number of round trips BODY costs.
A round trip is a burst of requests followed by a wait: however many
requests go out together, the link is crossed once each way for all
of them.  So what is counted is the number of times BODY sends
something after having had to wait for something else."
  `(let ((count 0) (waited t))
     (cl-letf* ((send (symbol-function 'scrutiny-agent-async-request))
                (await (symbol-function 'scrutiny-agent-await))
                ((symbol-function 'scrutiny-agent-async-request)
                 (lambda (&rest args)
                   (when waited (setq count (1+ count) waited nil))
                   (apply send args)))
                ((symbol-function 'scrutiny-agent-await)
                 (lambda (conn future &optional timeout)
                   (unless (scrutiny-agent-future-done-p future)
                     (setq waited t))
                   (funcall await conn future timeout))))
       ,@body)
     count))

(defun scrutiny-agent-fs-tests--settle (&optional predicate)
  "Let notifications arrive: until PREDICATE holds, or briefly."
  (let ((proc (scrutiny-agent--conn-process
               (scrutiny-agent-connection "fstest")))
        (deadline (+ (float-time) (if predicate 10 0.4))))
    (while (and (< (float-time) deadline)
                (not (and predicate (funcall predicate))))
      (accept-process-output proc 0.05))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-one-request-per-directory
  ;; The point of asking for attributes with the listing: a directory's
  ;; worth of questions must cost one round trip, not one apiece.
  (let ((scrutiny-agent-fs-cache-ttl 60.0))
    (scrutiny-agent-fs-tests--with-agent (root remote)
      (let ((scrutiny-agent-fs-cache-ttl 60.0))
        (dotimes (i 25)
          (scrutiny-agent-fs-tests--write-local
           (expand-file-name (format "f%02d.txt" i) root) "x"))
        (ignore (file-exists-p (concat remote "f00.txt")))  ; warm the roots
        (scrutiny-agent-fs-flush-cache)
        (let ((requests (scrutiny-agent-fs-tests--count-requests
                         (dotimes (i 25)
                           (file-attributes
                            (concat remote (format "f%02d.txt" i)))))))
          (should (= requests 1)))
        (scrutiny-agent-fs-flush-cache)
        (should (= 1 (scrutiny-agent-fs-tests--count-requests
                      (file-name-all-completions "f" remote))))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-skips-paths-outside-the-roots
  ;; An agent will refuse anything outside its roots, so asking costs a
  ;; round trip to be told no. `dired' alone probes /bin, /usr/bin and
  ;; /sbin while setting up.
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (ignore remote)
    (should (scrutiny-agent-fs--in-roots-p "fstest" root))
    (should (scrutiny-agent-fs--in-roots-p
             "fstest" (expand-file-name "deep/file.txt" root)))
    (should-not (scrutiny-agent-fs--in-roots-p "fstest" "/bin"))
    (should-not (scrutiny-agent-fs--in-roots-p "fstest" "/etc/passwd"))
    ;; A sibling whose name merely starts with a root's name is out.
    (should-not (scrutiny-agent-fs--in-roots-p "fstest" (concat root "-other")))
    ;; And no request is made for one.
    (should (= 0 (scrutiny-agent-fs-tests--count-requests
                  (ignore-errors (file-attributes "/ssh:fstest:/bin")))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-connection-facts-survive-a-flush
  ;; The allowed roots and the remote home belong to the connection,
  ;; not to any directory; re-reading them on every cache flush would
  ;; put a round trip in front of every operation.
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (ignore root)
    ;; Warm both facts, then flush the attribute cache.
    (ignore (file-exists-p (concat remote "anything")))
    (scrutiny-agent-fs--home "fstest")
    (scrutiny-agent-fs-flush-cache)
    (should (= 0 (scrutiny-agent-fs-tests--count-requests
                  (scrutiny-agent-fs--roots "fstest"))))
    (should (= 0 (scrutiny-agent-fs-tests--count-requests
                  (scrutiny-agent-fs--home "fstest"))))))

;; ---------------------------------------------------------------------
;; Outside the roots
;; ---------------------------------------------------------------------

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-facts-cost-nothing
  ;; The roots and the remote home arrive with the handshake, which has
  ;; to be waited for anyway.  Asking for them separately would put a
  ;; second round trip at the front of every session.
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (ignore remote)
    (scrutiny-agent-fs-forget-connections)
    (should (= 0 (scrutiny-agent-fs-tests--count-requests
                  (scrutiny-agent-fs--facts "fstest"))))
    (should (member root (scrutiny-agent-fs--roots "fstest")))
    ;; The home is reported even though it is not inside the roots --
    ;; it cannot be discovered by listing when it is not reachable.
    (should (equal (scrutiny-agent-fs--home "fstest")
                   (directory-file-name (expand-file-name "~"))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-ancestors-cost-nothing
  ;; `project-current' (which eglot runs from `find-file-hook') walks UP
  ;; the tree with `locate-dominating-file'. That walk leaves the roots
  ;; and continues to "/", and every level of it is on the critical path
  ;; of C-x C-f.
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (ignore remote)
    (ignore (file-exists-p (concat remote "anything")))
    (let* ((parent (file-name-directory (directory-file-name root)))
           (parent-remote (concat "/ssh:fstest:" parent)))
      (should (scrutiny-agent-fs--ancestor-of-root-p "fstest" parent))
      (should (= 0 (scrutiny-agent-fs-tests--count-requests
                    (should (file-directory-p parent-remote))
                    (should (file-exists-p parent-remote))
                    ;; ...and the thing the walk is actually looking for
                    ;; is not there, so it keeps climbing -- for free.
                    (should-not (file-exists-p (concat parent-remote ".git")))
                    (should-not (file-exists-p "/ssh:fstest:/etc/passwd"))))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-ancestors-lead-to-roots
  ;; The model is "the visible filesystem is the roots and the way down
  ;; to them", so an ancestor lists the entries that lead somewhere.
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (ignore remote)
    (let ((parent (file-name-directory (directory-file-name root))))
      (should (member (file-name-nondirectory (directory-file-name root))
                      (scrutiny-agent-fs--root-children "fstest" parent))))))

;; Outside the roots, listings are answered from structure.  They must
;; still be shaped like listings: `directory-files' names carry no
;; slash and honour MATCH, FULL, NOSORT and COUNT.  project.el walks
;; up with a marker pattern and takes any non-nil answer for a hit;
;; before this, an ancestor answered its children whatever the pattern,
;; and Emacs 30's `project-try-vc--search' recursed on that until
;; `max-lisp-eval-depth' -- on every find-file of a file not in a
;; repository.
(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-structural-listings-are-shaped-like-listings
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (let* ((scrutiny-agent-fs-roots-are-the-world t)
           (base (file-name-nondirectory (directory-file-name root)))
           (parent (file-name-directory (directory-file-name remote)))
           (delegated nil))
      (cl-letf* ((delegate (symbol-function 'scrutiny-agent-fs--delegate))
                 ((symbol-function 'scrutiny-agent-fs--delegate)
                  (lambda (operation args)
                    (push operation delegated)
                    (funcall delegate operation args))))
        (should-not (scrutiny-agent-fs--in-roots-p
                     "fstest" (file-local-name parent)))
        (should (member base (directory-files parent)))
        (should-not (member (file-name-as-directory base)
                            (directory-files parent)))
        (should (member "." (directory-files parent)))
        (should-not (directory-files parent nil "\\`\\.git\\'"))
        (should (equal (directory-files parent nil (regexp-quote base))
                       (list base)))
        (should (equal (directory-files parent t (regexp-quote base))
                       (list (concat parent base))))
        (should (= 1 (length (directory-files parent nil nil nil 1))))
        (should-not (directory-files-and-attributes parent nil "\\`\\.git\\'"))
        (should (assoc base (directory-files-and-attributes parent)))
        (should-not (file-name-all-completions "no-such-prefix" parent))
        (should (equal (file-name-all-completions base parent)
                       (list (file-name-as-directory base))))
        ;; The walk that recursed: a file in a root that is no
        ;; repository.  It must come back, and without TRAMP.
        (let ((max-lisp-eval-depth 600))
          (should-not (project-current nil remote)))
        (should-not (memq 'directory-files delegated))
        (should-not (memq 'file-name-all-completions delegated))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-can-defer-outside-roots
  ;; With the option off, an out-of-roots path is TRAMP's business
  ;; again -- the escape hatch when a project really does live above
  ;; the configured roots.
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (ignore root remote)
    (let ((scrutiny-agent-fs-roots-are-the-world nil)
          (delegated nil))
      (cl-letf (((symbol-function 'scrutiny-agent-fs--delegate)
                 (lambda (op _args) (setq delegated (cons op delegated)) nil)))
        (file-exists-p "/ssh:fstest:/etc/passwd"))
      (should (memq 'file-exists-p delegated)))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-find-file-is-one-round-trip
  ;; The property the whole file exists for.  Opening a file needs its
  ;; directory (which answers for every file beside it), every
  ;; directory above that (for `.dir-locals.el' and the project root),
  ;; and its contents.  All of it is asked for at once.
  (let ((scrutiny-agent-fs-cache-ttl 60.0))
    (scrutiny-agent-fs-tests--with-agent (root remote)
      (let ((scrutiny-agent-fs-cache-ttl 60.0)
            (deep (expand-file-name "a/b/c" root)))
        (make-directory deep t)
        (dotimes (i 10)
          (scrutiny-agent-fs-tests--write-local
           (expand-file-name (format "f%02d.py" i) deep)
           (format "# file %02d\n" i)))
        (scrutiny-agent-fs-flush-cache)
        (let (buffer)
          (should (= 1 (scrutiny-agent-fs-tests--count-round-trips
                        (setq buffer (find-file-noselect
                                      (concat remote "a/b/c/f00.py"))))))
          ;; ...and it really opened the file, rather than failing fast.
          (with-current-buffer buffer
            (should (equal buffer-file-name (concat remote "a/b/c/f00.py")))
            (should (equal (buffer-string) "# file 00\n"))
            (should-not (buffer-modified-p))
            (should (verify-visited-file-modtime buffer)))
          (kill-buffer buffer))
        ;; The next file in the same directory needs only its bytes.
        (let (second)
          (should (= 1 (scrutiny-agent-fs-tests--count-requests
                        (setq second (find-file-noselect
                                      (concat remote "a/b/c/f01.py"))))))
          (with-current-buffer second
            (should (equal (buffer-string) "# file 01\n")))
          (kill-buffer second))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-find-file-without-prefetch
  ;; With the advice off, the same open is the listing burst and then
  ;; the read: two round trips, and still correct.
  (let ((scrutiny-agent-fs-cache-ttl 60.0))
    (scrutiny-agent-fs-tests--with-agent (root remote)
      (let ((scrutiny-agent-fs-cache-ttl 60.0)
            (scrutiny-agent-fs-prefetch nil))
        (scrutiny-agent-fs-tests--write-local (expand-file-name "p.py" root)
                                              "# p\n")
        (scrutiny-agent-fs-flush-cache)
        (let (buffer)
          (should (= 2 (scrutiny-agent-fs-tests--count-round-trips
                        (setq buffer (find-file-noselect
                                      (concat remote "p.py"))))))
          (with-current-buffer buffer
            (should (equal (buffer-string) "# p\n")))
          (kill-buffer buffer))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-string-ops-need-no-remote
  ;; Delegating these hands TRAMP the first remote name it has seen,
  ;; and `abbreviate-file-name' answers by looking up the remote home --
  ;; a full connect, on the critical path of C-x C-f.
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (ignore root)
    (ignore (file-exists-p (concat remote "anything")))
    (let ((file (concat remote "dir/sub/file.py")))
      (should (= 0 (scrutiny-agent-fs-tests--count-requests
                    (should (equal (file-name-nondirectory file) "file.py"))
                    (should (equal (file-name-directory file)
                                   (concat remote "dir/sub/")))
                    (should (equal (file-remote-p file) "/ssh:fstest:"))
                    (should (equal (file-remote-p file 'host) "fstest"))
                    (should (directory-file-name (concat remote "dir/")))
                    (should (abbreviate-file-name file))))))))

(scrutiny-agent-fs-tests--deftest scrutiny-agent-fs-abbreviates-the-home
  (scrutiny-agent-fs-tests--with-agent (root remote)
    (ignore root remote)
    (let ((home (scrutiny-agent-fs--home "fstest")))
      (should (equal (abbreviate-file-name
                      (format "/ssh:fstest:%s/src/x.py" home))
                     "/ssh:fstest:~/src/x.py")))))
