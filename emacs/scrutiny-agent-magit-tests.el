;;; scrutiny-agent-magit-tests.el --- ERT tests for the magit routing  -*- lexical-binding: t; -*-

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; Covers `scrutiny-agent-magit.el': which invocations get routed, how
;; output is placed, and -- against a real agent -- that magit's own
;; git readers return the same answers over the agent as they would
;; over a subprocess.
;;
;; The integration tests use a `/ssh:HOST:' directory name that TRAMP
;; never actually connects to: routing happens before any file
;; operation, so the whole path is exercised without needing sshd.
;;
;; Run:  SCRUTINY_AGENT_BIN=build/agent/scrutiny-agent \
;;         emacs -Q --batch -L emacs -l emacs/scrutiny-agent-magit-tests.el \
;;         -f ert-run-tests-batch-and-exit

;;; Code:

(require 'ert)
(require 'cl-lib)
(require 'scrutiny-agent)
(require 'scrutiny-agent-ops)
(require 'scrutiny-agent-magit)

(declare-function magit-git-string "magit-git")
(declare-function magit-git-lines "magit-git")

(defvar scrutiny-agent-magit-tests--have-magit
  (require 'magit-process nil t)
  "Non-nil when magit is installed, so the advice can be attached.")

;; ---------------------------------------------------------------------
;; Applicability
;; ---------------------------------------------------------------------

(ert-deftest scrutiny-agent-magit-host-mapping ()
  (let ((scrutiny-agent-hosts '(("devbox" :transport "ssh devbox")))
        (scrutiny-agent-magit-host-alist '(("devbox.corp" . "devbox"))))
    (should (equal (scrutiny-agent-magit--host "/ssh:devbox:/src/") "devbox"))
    (should (equal (scrutiny-agent-magit--host "/ssh:devbox.corp:/src/")
                   "devbox"))
    ;; An unconfigured host and a local directory are both declined, so
    ;; magit behaves exactly as it does today for them.
    (should-not (scrutiny-agent-magit--host "/ssh:other:/src/"))
    (should-not (scrutiny-agent-magit--host "/home/me/src/"))
    (should-not (scrutiny-agent-magit--host ""))))

(ert-deftest scrutiny-agent-magit-declines-local-directories ()
  (let ((default-directory "/tmp/")
        (scrutiny-agent-hosts '(("devbox" :transport "ssh devbox"))))
    (should-not (scrutiny-agent-magit--usable "git" nil t))))

(ert-deftest scrutiny-agent-magit-declines-non-git-programs ()
  (let ((default-directory "/ssh:devbox:/src/")
        (scrutiny-agent-hosts '(("devbox" :transport "ssh devbox"))))
    (should-not (scrutiny-agent-magit--usable "ls" nil t))
    (should-not (scrutiny-agent-magit--usable nil nil t))))

(ert-deftest scrutiny-agent-magit-declines-stdin-and-async ()
  ;; git.exec has no stdin channel, and no process object to hand back
  ;; for BUFFER 0; both must fall through to TRAMP untouched.
  (let ((default-directory "/ssh:devbox:/src/")
        (scrutiny-agent-hosts '(("devbox" :transport "ssh devbox"))))
    (should-not (scrutiny-agent-magit--usable "git" "/tmp/infile" t))
    (should-not (scrutiny-agent-magit--usable "git" nil 0))))

(ert-deftest scrutiny-agent-magit-recursion-guard ()
  (let ((default-directory "/ssh:devbox:/src/")
        (scrutiny-agent-magit--depth 1)
        (scrutiny-agent-hosts '(("devbox" :transport "ssh devbox"))))
    (should-not (scrutiny-agent-magit--usable "git" nil t))))

;; ---------------------------------------------------------------------
;; Output placement (process-file BUFFER semantics)
;; ---------------------------------------------------------------------

(defun scrutiny-agent-magit-tests--result (stdout stderr)
  (list :stdout stdout :stderr stderr :exitCode 0 :truncated :json-false))

(ert-deftest scrutiny-agent-magit-output-into-current-buffer ()
  (with-temp-buffer
    (scrutiny-agent-magit--place-output
     t (scrutiny-agent-magit-tests--result "out\n" "err\n"))
    ;; A single destination merges both streams, as `process-file' does.
    (should (equal (buffer-string) "out\nerr\n"))))

(ert-deftest scrutiny-agent-magit-output-discarded ()
  (with-temp-buffer
    (scrutiny-agent-magit--place-output
     nil (scrutiny-agent-magit-tests--result "out\n" "err\n"))
    (should (equal (buffer-string) ""))))

(ert-deftest scrutiny-agent-magit-output-into-named-buffer ()
  (let ((target (generate-new-buffer " *scra-target*")))
    (unwind-protect
        (progn
          (scrutiny-agent-magit--place-output
           target (scrutiny-agent-magit-tests--result "hello\n" ""))
          (with-current-buffer target
            (should (equal (buffer-string) "hello\n"))))
      (kill-buffer target))))

(ert-deftest scrutiny-agent-magit-streams-kept-separate ()
  ;; (STDOUT STDERR): magit uses this to capture stderr on its own, and
  ;; leaking stderr into stdout would corrupt every parsed result.
  (with-temp-buffer
    (scrutiny-agent-magit--place-output
     (list t nil) (scrutiny-agent-magit-tests--result "out\n" "err\n"))
    (should (equal (buffer-string) "out\n"))))

(ert-deftest scrutiny-agent-magit-stderr-to-file ()
  (let ((file (make-temp-file "scra-stderr")))
    (unwind-protect
        (with-temp-buffer
          (scrutiny-agent-magit--place-output
           (list t file) (scrutiny-agent-magit-tests--result "out\n" "err\n"))
          (should (equal (buffer-string) "out\n"))
          (should (equal (with-temp-buffer
                           (insert-file-contents file) (buffer-string))
                         "err\n")))
      (delete-file file))))

(ert-deftest scrutiny-agent-magit-stderr-t-joins-stdout ()
  (with-temp-buffer
    (scrutiny-agent-magit--place-output
     (list t t) (scrutiny-agent-magit-tests--result "out\n" "err\n"))
    (should (equal (buffer-string) "out\nerr\n"))))

;; ---------------------------------------------------------------------
;; Integration
;; ---------------------------------------------------------------------

(defun scrutiny-agent-magit-tests--git (dir &rest args)
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
        (error "git %s failed: %s" (string-join args " ") (buffer-string))))))

(defmacro scrutiny-agent-magit-tests--with-agent (varlist &rest body)
  "Bind (CONN REPO) with a real agent whose git.exec allows magit.
`default-directory' is a /ssh: name for the configured host, which
TRAMP never connects to because routing happens first."
  (declare (indent 1) (debug t))
  (let ((conn (nth 0 varlist)) (repo (nth 1 varlist)))
    `(let* ((bin (getenv "SCRUTINY_AGENT_BIN"))
            (install (make-temp-file "scra-magit-install" t))
            (,repo (make-temp-file "scra-magit-repo" t))
            (tmproot (file-name-as-directory temporary-file-directory))
            ;; These tests set the git allowlist on the exec line on
            ;; purpose. The client's default config would add its own
            ;; `git-exec-preset', which is precisely what a test of a
            ;; NARROW allowlist must not inherit.
            (scrutiny-agent-default-config nil)
            (scrutiny-agent-hosts
             `(("magitest" :transport "sh"
                :install-dir ,install
                :local-binary ,bin
                :agent-args ("--allow-root" ,tmproot
                             "--git-exec-preset" "magit"))))
            (,conn nil))
       (scrutiny-agent-magit-tests--git ,repo "init" "-q"
                                        "--initial-branch=work")
       (with-temp-file (expand-file-name "a.txt" ,repo) (insert "one\n"))
       (scrutiny-agent-magit-tests--git ,repo "add" "-A")
       (scrutiny-agent-magit-tests--git ,repo "commit" "-q" "-m" "first")
       (setq ,conn (scrutiny-agent-connect "magitest"))
       (unwind-protect
           (let ((default-directory
                  (format "/ssh:magitest:%s/" ,repo)))
             ,@body)
         (scrutiny-agent-disconnect "magitest")
         (delete-directory install t)
         (delete-directory ,repo t)))))

(defmacro scrutiny-agent-magit-tests--deftest (name &rest body)
  (declare (indent 1) (debug t))
  `(ert-deftest ,name ()
     :tags '(integration)
     (skip-unless (getenv "SCRUTINY_AGENT_BIN"))
     ,@body))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-routes-a-command
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn)
    (let ((fallback-used nil))
      (with-temp-buffer
        (let ((code (scrutiny-agent-magit--run
                     (lambda (&rest _) (setq fallback-used t) 1)
                     "git" nil t nil '("rev-parse" "HEAD"))))
          (should (equal code 0))
          (should-not fallback-used)
          (should (string-match-p "^[0-9a-f]\\{40\\}$"
                                  (string-trim (buffer-string)))))))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-answers-match-git
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn)
    (let ((direct (with-temp-buffer
                    (let ((default-directory (file-name-as-directory repo)))
                      (call-process "git" nil t nil "log" "--oneline"))
                    (buffer-string))))
      (with-temp-buffer
        (scrutiny-agent-magit--run (lambda (&rest _) (error "no fallback"))
                                   "git" nil t nil '("log" "--oneline"))
        (should (equal (buffer-string) direct))))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-passes-global-args
  ;; magit puts these before every subcommand; if the agent refuses
  ;; them, nothing is ever routed.
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn repo)
    (let ((fallback-used nil))
      (with-temp-buffer
        (scrutiny-agent-magit--run
         (lambda (&rest _) (setq fallback-used t) 1)
         "git" nil t nil
         '("--no-pager" "--literal-pathspecs"
           "-c" "core.preloadIndex=true" "-c" "log.showSignature=false"
           "-c" "color.ui=false" "-c" "color.diff=false"
           "-c" "diff.noPrefix=false"
           "status" "--porcelain"))
        (should-not fallback-used)))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-nonzero-exit-is-returned
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn repo)
    (with-temp-buffer
      (should-not
       (zerop (scrutiny-agent-magit--run (lambda (&rest _) (error "no fallback"))
                                         "git" nil t nil
                                         '("rev-parse" "no-such-ref")))))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-falls-back-when-refused
  ;; An agent with a narrow allowlist must cost speed, not function.
  (let* ((bin (getenv "SCRUTINY_AGENT_BIN"))
         (install (make-temp-file "scra-narrow-install" t))
         (repo (make-temp-file "scra-narrow-repo" t))
         (tmproot (file-name-as-directory temporary-file-directory))
         (scrutiny-agent-default-config nil)
         (scrutiny-agent-hosts
          `(("narrow" :transport "sh"
             :install-dir ,install
             :local-binary ,bin
             :agent-args ("--allow-root" ,tmproot
                          "--git-exec" "status")))))
    (scrutiny-agent-magit-tests--git repo "init" "-q" "--initial-branch=work")
    (scrutiny-agent-connect "narrow")
    (unwind-protect
        (let ((default-directory (format "/ssh:narrow:%s/" repo))
              (fallback-args nil))
          ;; `status' is allowed: routed.
          (with-temp-buffer
            (scrutiny-agent-magit--run
             (lambda (&rest args) (setq fallback-args args) 0)
             "git" nil t nil '("status" "--porcelain")))
          (should-not fallback-args)
          ;; `log' is not: the original runs instead.
          (with-temp-buffer
            (scrutiny-agent-magit--run
             (lambda (&rest args) (setq fallback-args args) 0)
             "git" nil t nil '("log")))
          (should fallback-args))
      (scrutiny-agent-disconnect "narrow")
      (delete-directory install t)
      (delete-directory repo t))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-refusal-can-be-surfaced
  (let* ((bin (getenv "SCRUTINY_AGENT_BIN"))
         (install (make-temp-file "scra-strict-install" t))
         (repo (make-temp-file "scra-strict-repo" t))
         (tmproot (file-name-as-directory temporary-file-directory))
         (scrutiny-agent-default-config nil)
         (scrutiny-agent-hosts
          `(("strict" :transport "sh"
             :install-dir ,install
             :local-binary ,bin
             :agent-args ("--allow-root" ,tmproot
                          "--git-exec" "status"))))
         (scrutiny-agent-magit-fallback nil))
    (scrutiny-agent-magit-tests--git repo "init" "-q" "--initial-branch=work")
    (scrutiny-agent-connect "strict")
    (unwind-protect
        (let ((default-directory (format "/ssh:strict:%s/" repo)))
          (with-temp-buffer
            (should-error
             (scrutiny-agent-magit--run (lambda (&rest _) 0)
                                        "git" nil t nil '("log"))
             :type 'scrutiny-agent-rpc-error)))
      (scrutiny-agent-disconnect "strict")
      (delete-directory install t)
      (delete-directory repo t))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-declines-without-git-exec
  ;; An agent started with no allowlist does not advertise git.exec, so
  ;; every invocation must go straight to TRAMP.
  (let* ((bin (getenv "SCRUTINY_AGENT_BIN"))
         (install (make-temp-file "scra-noexec-install" t))
         (repo (make-temp-file "scra-noexec-repo" t))
         (tmproot (file-name-as-directory temporary-file-directory))
         (scrutiny-agent-default-config nil)
         (scrutiny-agent-hosts
          `(("noexec" :transport "sh"
             :install-dir ,install
             :local-binary ,bin
             :agent-args ("--allow-root" ,tmproot)))))
    (scrutiny-agent-magit-tests--git repo "init" "-q" "--initial-branch=work")
    (scrutiny-agent-connect "noexec")
    (unwind-protect
        (let ((default-directory (format "/ssh:noexec:%s/" repo)))
          (should-not (scrutiny-agent-magit--usable "git" nil t)))
      (scrutiny-agent-disconnect "noexec")
      (delete-directory install t)
      (delete-directory repo t))))

;; ---------------------------------------------------------------------
;; Through magit's own API
;; ---------------------------------------------------------------------

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-through-magit-readers
  (skip-unless scrutiny-agent-magit-tests--have-magit)
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn)
    (scrutiny-agent-magit-mode 1)
    (unwind-protect
        (let ((expected (string-trim
                         (with-temp-buffer
                           (let ((default-directory
                                  (file-name-as-directory repo)))
                             (call-process "git" nil t nil
                                           "rev-parse" "HEAD"))
                           (buffer-string)))))
          ;; magit's own reader, unmodified, going over the agent.
          (should (equal (magit-git-string "rev-parse" "HEAD") expected))
          (should (equal (magit-git-string "symbolic-ref" "--short" "HEAD")
                         "work"))
          (should (member "a.txt" (magit-git-lines "ls-files"))))
      (scrutiny-agent-magit-mode -1))))

(provide 'scrutiny-agent-magit-tests)
;;; scrutiny-agent-magit-tests.el ends here
