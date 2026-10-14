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
  ;; With no agent to ask, nothing is routed: neither a command that
  ;; reads standard input nor one that wants no output waited for.
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
            ;; Each test starts knowing nothing about what runs together.
            (scrutiny-agent-magit-history-file nil)
            (scrutiny-agent-magit--history (make-hash-table :test #'equal))
            (scrutiny-agent-hosts
             `(("magitest" :transport "sh"
                :install-dir ,install
                :local-binary ,bin
                :agent-args ("--allow-root" ,tmproot "--allow-write"
                             "--git-exec-preset" "magit"))))
            (,conn nil))
       (scrutiny-agent-magit-tests--git ,repo "init" "-q"
                                        "--initial-branch=work")
       (with-temp-file (expand-file-name "a.txt" ,repo) (insert "one\n"))
       (scrutiny-agent-magit-tests--git ,repo "add" "-A")
       (scrutiny-agent-magit-tests--git ,repo "commit" "-q" "-m" "first")
       (setq ,conn (scrutiny-agent-connect "magitest"))
       (scrutiny-agent-magit--end-batch)
       (unwind-protect
           (let ((default-directory
                  (format "/ssh:magitest:%s/" ,repo)))
             ,@body)
         (scrutiny-agent-magit--end-batch)
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

;; ---------------------------------------------------------------------
;; Running a refresh's commands together
;; ---------------------------------------------------------------------

(defun scrutiny-agent-magit-tests--ask (&rest args)
  "Run git ARGS through the routing; return (EXIT . OUTPUT)."
  (with-temp-buffer
    (let ((code (scrutiny-agent-magit--run
                 (lambda (&rest _) (error "Fell back to TRAMP: %S" args))
                 "git" nil t nil args)))
      (cons code (buffer-string)))))

(defmacro scrutiny-agent-magit-tests--count-sent (&rest body)
  "Number of requests BODY sends."
  `(let ((count 0))
     (cl-letf* ((original (symbol-function 'scrutiny-agent-async-request))
                ((symbol-function 'scrutiny-agent-async-request)
                 (lambda (&rest args)
                   (setq count (1+ count))
                   (apply original args))))
       ,@body)
     count))

(defmacro scrutiny-agent-magit-tests--count-round-trips (&rest body)
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

(defconst scrutiny-agent-magit-tests--refresh
  '(("rev-parse" "--verify" "HEAD")
    ("symbolic-ref" "--short" "HEAD")
    ("status" "--porcelain" "-z")
    ("diff" "--no-ext-diff")
    ("log" "-n5" "--format=%h %s")
    ("config" "--list" "-z"))
  "A stand-in for the questions a status refresh asks.")

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-second-refresh-is-one-round-trip
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn repo)
    ;; The first time, nothing is known: one round trip per command.
    (should (= (length scrutiny-agent-magit-tests--refresh)
               (scrutiny-agent-magit-tests--count-round-trips
                (dolist (args scrutiny-agent-magit-tests--refresh)
                  (apply #'scrutiny-agent-magit-tests--ask args)))))
    (scrutiny-agent-magit--end-batch)
    ;; The second time, the first question brings the rest with it.
    (let (answers)
      (should (= 1 (scrutiny-agent-magit-tests--count-round-trips
                    (setq answers
                          (mapcar (lambda (args)
                                    (apply #'scrutiny-agent-magit-tests--ask
                                           args))
                                  scrutiny-agent-magit-tests--refresh)))))
      ;; And they are the right answers.
      (cl-mapc (lambda (args answer)
                 (should (equal (cdr answer)
                                (with-temp-buffer
                                  (let ((default-directory
                                         (file-name-as-directory repo)))
                                    (apply #'call-process "git" nil t nil args))
                                  (buffer-string)))))
               scrutiny-agent-magit-tests--refresh answers))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-nothing-early-survives-a-change
  ;; The property that makes running commands early safe: an answer
  ;; obtained before a command that changes the repository is never
  ;; given after it.
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn)
    (dolist (args scrutiny-agent-magit-tests--refresh)
      (apply #'scrutiny-agent-magit-tests--ask args))
    (scrutiny-agent-magit--end-batch)
    ;; A refresh begins; `status' is among the commands sent early...
    (scrutiny-agent-magit-tests--ask "rev-parse" "--verify" "HEAD")
    (should (scrutiny-agent-magit--answer
             (cons "magitest" (cons (concat repo "/")
                                    '("status" "--porcelain" "-z")))))
    ;; ...then the user stages a new file...
    (with-temp-file (expand-file-name "staged.txt" repo) (insert "new\n"))
    (should (zerop (car (scrutiny-agent-magit-tests--ask "add" "staged.txt"))))
    (should (= 0 (hash-table-count scrutiny-agent-magit--answers)))
    ;; ...and status is asked for.  It must show the staged file.
    (should (string-match-p
             "A  staged.txt"
             (cdr (scrutiny-agent-magit-tests--ask "status" "--porcelain"
                                                   "-z"))))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-early-answers-end-with-the-command
  ;; Answers are for the batch that asked.  Once Emacs goes back to
  ;; waiting -- for input, for anything -- they are gone, so the next
  ;; refresh sees the repository as it is then.
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn)
    (dolist (args scrutiny-agent-magit-tests--refresh)
      (apply #'scrutiny-agent-magit-tests--ask args))
    (scrutiny-agent-magit--end-batch)
    (scrutiny-agent-magit-tests--ask "rev-parse" "--verify" "HEAD")
    (should (> (hash-table-count scrutiny-agent-magit--answers) 1))
    ;; The repository changes with no help from us...
    (with-temp-file (expand-file-name "a.txt" repo) (insert "edited\n"))
    ;; ...and Emacs idles, as it does between any two commands.
    (sleep-for 0.2)
    (should (= 0 (hash-table-count scrutiny-agent-magit--answers)))
    (should (string-match-p
             "a.txt"
             (cdr (scrutiny-agent-magit-tests--ask "status" "--porcelain"
                                                   "-z"))))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-saving-a-file-ends-the-batch
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn repo)
    (scrutiny-agent-magit-tests--ask "rev-parse" "--verify" "HEAD")
    (should (> (hash-table-count scrutiny-agent-magit--answers) 0))
    (let ((scrutiny-agent-fs-change-hook
           (list #'scrutiny-agent-magit--end-batch)))
      (run-hooks 'scrutiny-agent-fs-change-hook))
    (should (= 0 (hash-table-count scrutiny-agent-magit--answers)))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-only-readers-run-early
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn repo)
    ;; A batch that mixes reading and writing teaches only the reading.
    (scrutiny-agent-magit-tests--ask "rev-parse" "--verify" "HEAD")
    (scrutiny-agent-magit-tests--ask "status" "--porcelain")
    (scrutiny-agent-magit-tests--ask "update-ref" "refs/test/x" "HEAD")
    (scrutiny-agent-magit-tests--ask "rev-parse" "--verify" "HEAD")
    (scrutiny-agent-magit-tests--ask "status" "--porcelain")
    (scrutiny-agent-magit--end-batch)
    (let ((sent nil))
      (cl-letf* ((original (symbol-function 'scrutiny-agent-async-request))
                 ((symbol-function 'scrutiny-agent-async-request)
                  (lambda (c method params &optional cb)
                    (push (append (plist-get params :args) nil) sent)
                    (funcall original c method params cb))))
        (scrutiny-agent-magit-tests--ask "rev-parse" "--verify" "HEAD"))
      (should (= 2 (length sent)))
      (should-not (cl-find-if (lambda (args) (member "update-ref" args))
                              sent)))))

(ert-deftest scrutiny-agent-magit-knows-what-only-reads ()
  (dolist (args '(("rev-parse" "HEAD") ("status" "--porcelain")
                  ("--no-pager" "-c" "color.ui=false" "log" "-1")
                  ("config" "--list" "-z") ("config" "--get" "user.name")
                  ("symbolic-ref" "--short" "HEAD") ("stash" "list")
                  ("branch" "--list") ("tag" "-l" "v*")
                  ("update-index" "--refresh") ("remote")))
    (should (scrutiny-agent-magit--pure-p args)))
  (dolist (args '(("add" "x") ("commit" "-m" "x") ("checkout" "main")
                  ("config" "user.name" "me") ("config" "--unset" "a.b")
                  ("symbolic-ref" "HEAD" "refs/heads/x")
                  ("symbolic-ref" "-d" "HEAD")
                  ("stash") ("stash" "pop") ("branch" "new") ("tag" "v1")
                  ("update-index" "--add" "x") ("update-ref" "a" "b")
                  ("remote" "add" "o" "url") ("reset" "--hard")))
    (should-not (scrutiny-agent-magit--pure-p args))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-a-new-repository-borrows-what-others-taught
  ;; The first status of a repository never seen before still goes out
  ;; as one burst, on the strength of what a status asks everywhere.
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn)
    (dolist (args scrutiny-agent-magit-tests--refresh)
      (apply #'scrutiny-agent-magit-tests--ask args))
    (scrutiny-agent-magit--end-batch)
    (let ((other (make-temp-file "scra-magit-other" t)))
      (unwind-protect
          (progn
            (scrutiny-agent-magit-tests--git other "init" "-q"
                                             "--initial-branch=work")
            (let ((default-directory (format "/ssh:magitest:%s/" other)))
              (should (= 1 (scrutiny-agent-magit-tests--count-round-trips
                            (dolist (args scrutiny-agent-magit-tests--refresh)
                              (with-temp-buffer
                                (scrutiny-agent-magit--run
                                 (lambda (&rest _) 1)
                                 "git" nil t nil args))))))))
        (delete-directory other t)))))

(ert-deftest scrutiny-agent-magit-history-survives-a-restart ()
  (let* ((file (make-temp-file "scra-history" nil ".eld"))
         (scrutiny-agent-magit-history-file file)
         (scrutiny-agent-magit--history (make-hash-table :test #'equal))
         (batch '(("rev-parse" "HEAD") ("status" "--porcelain"))))
    (unwind-protect
        (progn
          (puthash '("devbox" . "/src/proj/") (list batch)
                   scrutiny-agent-magit--history)
          (scrutiny-agent-magit-save-history)
          (setq scrutiny-agent-magit--history nil)
          (should (equal (scrutiny-agent-magit--predict
                          "devbox" "/src/proj/" '("rev-parse" "HEAD"))
                         batch)))
      (delete-file file))))

;; ---------------------------------------------------------------------
;; What a command carries besides its arguments
;; ---------------------------------------------------------------------

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-scratch-index-stays-scratch
  ;; magit stashes by binding GIT_INDEX_FILE and running index commands.
  ;; Run without the binding, those commands land on the REAL index:
  ;; the user's staged state is rewritten and the stash is built from
  ;; the wrong tree.  That is what happened before the environment was
  ;; carried across.
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn)
    (with-temp-file (expand-file-name "unstaged.txt" repo) (insert "wip\n"))
    (let ((scratch (expand-file-name ".git/index.scratch" repo)))
      (with-environment-variables (("GIT_INDEX_FILE" scratch))
        (should (zerop (car (scrutiny-agent-magit-tests--ask
                             "read-tree" "HEAD"))))
        (should (zerop (car (scrutiny-agent-magit-tests--ask
                             "update-index" "--add" "--" "unstaged.txt"))))
        (should (string-match-p
                 "unstaged.txt"
                 (cdr (scrutiny-agent-magit-tests--ask "ls-files"))))))
    ;; The real index never heard about it.
    (should-not (string-match-p
                 "unstaged.txt"
                 (cdr (scrutiny-agent-magit-tests--ask "ls-files"))))
    (should (string-match-p
             "\\?\\? unstaged.txt"
             (cdr (scrutiny-agent-magit-tests--ask "status" "--porcelain"))))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-readers-ignore-the-editor
  ;; Every reader magit runs while a commit message is being written
  ;; has GIT_EDITOR bound around it.  A command that only reads cannot
  ;; start an editor, so the variable is not sent with it -- an agent
  ;; that does not allow naming an editor would otherwise refuse the
  ;; lot.  A command that COULD start one is sent with it, always: it
  ;; must run with the editor it was given or not at all.
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn repo)
    (let ((sent nil))
      (cl-letf* ((original (symbol-function 'scrutiny-agent-async-request))
                 ((symbol-function 'scrutiny-agent-async-request)
                  (lambda (c method params &optional cb)
                    (push params sent)
                    (funcall original c method params cb))))
        ;; A value no ambient environment will already hold: only what
        ;; DIFFERS from the session's environment counts as bound.
        (with-environment-variables (("GIT_EDITOR" "true # scrutiny test"))
          (should (zerop (car (scrutiny-agent-magit-tests--ask
                               "rev-parse" "--verify" "HEAD"))))
          (should-not (plist-member (plist-get (car sent) :env) :GIT_EDITOR))
          (setq sent nil)
          (with-temp-buffer
            (scrutiny-agent-magit--run (lambda (&rest _) 0)
                                       "git" nil t nil
                                       '("commit" "--allow-empty" "-m" "x")))
          (should (equal "true # scrutiny test"
                         (plist-get (plist-get (car (last sent)) :env)
                                    :GIT_EDITOR))))))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-standard-input-is-delivered
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn repo)
    (let ((input (make-temp-file "scra-stdin")))
      (unwind-protect
          (progn
            (with-temp-file input (insert "hello\n"))
            (with-temp-buffer
              (should (zerop (scrutiny-agent-magit--run
                              (lambda (&rest _) (error "Fell back"))
                              "git" input t nil '("hash-object" "--stdin"))))
              (should (equal (string-trim (buffer-string))
                             "ce013625030ba8dba906f756967f9e9ca394464a"))))
        (delete-file input)))))

;; ---------------------------------------------------------------------
;; The seam: `process-file', and nothing of magit's
;; ---------------------------------------------------------------------

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-any-caller-of-process-file-is-served
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn repo)
    (let ((handed-to-tramp nil))
      (unwind-protect
          (progn
            (scrutiny-agent-magit-mode 1)
            (should scrutiny-agent-fs-mode)
            (cl-letf (((symbol-function 'scrutiny-agent-fs--delegate)
                       (lambda (operation args)
                         (push (cons operation (car args)) handed-to-tramp)
                         1)))
              ;; Plain `process-file', as vc, project.el or anything
              ;; else would call it.  Not a magit function in sight.
              (with-temp-buffer
                (should (zerop (process-file "git" nil t nil
                                             "rev-parse" "--verify" "HEAD")))
                (should (string-match-p "\\`[0-9a-f]\\{40\\}$"
                                        (string-trim (buffer-string)))))
              (should-not (assq 'process-file handed-to-tramp))
              ;; Something that is not git is not ours to run.
              (process-file "ls" nil nil nil)
              (should (equal (assq 'process-file handed-to-tramp)
                             '(process-file . "ls")))))
        (scrutiny-agent-magit-mode -1)
        (scrutiny-agent-fs-mode -1)))))

(ert-deftest scrutiny-agent-magit-mode-advises-nothing ()
  (let ((scrutiny-agent-magit-history-file nil))
    (unwind-protect
        (progn
          (scrutiny-agent-magit-mode 1)
          (when (fboundp 'magit-process-file)
            (should-not (advice--p (advice--symbol-function
                                    'magit-process-file))))
          (should (eq scrutiny-agent-fs-process-file-function
                      #'scrutiny-agent-magit--process-file)))
      (scrutiny-agent-magit-mode -1)
      (scrutiny-agent-fs-mode -1))
    (should-not scrutiny-agent-fs-process-file-function)))

;; ---------------------------------------------------------------------
;; Asynchronous git
;; ---------------------------------------------------------------------

(defmacro scrutiny-agent-magit-tests--with-modes (&rest body)
  "Run BODY with the file handler and git routing switched on."
  (declare (indent 0) (debug t))
  `(unwind-protect
       (progn (scrutiny-agent-magit-mode 1) ,@body)
     (scrutiny-agent-magit-mode -1)
     (scrutiny-agent-fs-mode -1)))

(defun scrutiny-agent-magit-tests--wait (process &optional seconds)
  "Wait for PROCESS to end; signal if it does not."
  (let ((deadline (+ (float-time) (or seconds 30))))
    (while (and (process-live-p process) (< (float-time) deadline))
      (accept-process-output nil 0.05))
    (when (process-live-p process)
      (delete-process process)
      (error "Process %s did not finish" (process-name process)))
    ;; One more turn of the loop, for output still in the pipe.
    (accept-process-output process 0.1)))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-async-is-a-real-process
  ;; What a caller of `start-file-process' is promised: a process
  ;; object, its output in the buffer it named, a sentinel called with
  ;; the usual words, and an exit status.
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn)
    (scrutiny-agent-magit-tests--with-modes
      (with-temp-buffer
        (let* ((events nil)
               (delegated nil)
               (process
                (cl-letf (((symbol-function 'scrutiny-agent-fs--delegate)
                           (lambda (operation _args)
                             (setq delegated operation)
                             (error "Handed to TRAMP"))))
                  (start-file-process "git" (current-buffer)
                                      "git" "rev-parse" "HEAD"))))
          (should-not delegated)
          (should (processp process))
          (set-process-sentinel process
                                (lambda (_p event) (push event events)))
          (scrutiny-agent-magit-tests--wait process)
          (should (eq (process-status process) 'exit))
          (should (= 0 (process-exit-status process)))
          (should (equal events '("finished\n")))
          (should (equal (string-trim (buffer-string))
                         (string-trim
                          (let ((default-directory
                                 (file-name-as-directory repo)))
                            (shell-command-to-string
                             "git rev-parse HEAD"))))))))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-async-reports-failure
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn repo)
    (scrutiny-agent-magit-tests--with-modes
      (with-temp-buffer
        (let ((process (start-file-process "git" (current-buffer) "git"
                                           "rev-parse" "--verify"
                                           "no-such-ref")))
          (scrutiny-agent-magit-tests--wait process)
          (should (eq (process-status process) 'exit))
          (should (= 128 (process-exit-status process)))
          ;; What git said on stderr is there to be read.
          (should (string-match-p "fatal" (buffer-string))))))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-async-takes-input
  ;; Staging a hunk, as magit does it on a remote: start `git apply',
  ;; send the patch, send end-of-input, wait.
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn)
    (with-temp-file (expand-file-name "a.txt" repo) (insert "one\ntwo\n"))
    (scrutiny-agent-magit-tests--with-modes
      (let ((patch (cdr (scrutiny-agent-magit-tests--ask "diff" "--" "a.txt"))))
        (should (string-match-p "^\\+two" patch))
        (with-temp-buffer
          (let ((process (start-file-process "git" (current-buffer) "git"
                                             "apply" "--cached" "-")))
            (process-send-string process patch)
            (process-send-eof process)
            (scrutiny-agent-magit-tests--wait process)
            (should (= 0 (process-exit-status process)))))
        (should (string-match-p
                 "^M  a.txt"
                 (cdr (scrutiny-agent-magit-tests--ask "status"
                                                       "--porcelain"))))))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-stages-a-hunk-the-way-magit-does
  ;; The same thing through magit's own function, which is the one a
  ;; keystroke on a hunk ends up in.
  (skip-unless (require 'magit nil t))
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn)
    (with-temp-file (expand-file-name "a.txt" repo) (insert "one\nhunk\n"))
    (scrutiny-agent-magit-tests--with-modes
      (let ((patch (cdr (scrutiny-agent-magit-tests--ask "diff" "--" "a.txt")))
            (handed-to-tramp nil)
            (magit-process-popup-time -1)
            ;; Staging is what is under test; the refresh that follows
            ;; it in a live session is covered on its own.
            (magit-inhibit-refresh t))
        (cl-letf* ((delegate (symbol-function 'scrutiny-agent-fs--delegate))
                   ((symbol-function 'scrutiny-agent-fs--delegate)
                    (lambda (operation args)
                      (when (memq operation '(start-file-process make-process
                                              process-file))
                        (push operation handed-to-tramp))
                      (funcall delegate operation args))))
          (with-temp-buffer
            (insert patch)
            (magit-run-git-with-input "apply" "--cached" "-")))
        (should-not handed-to-tramp)
        (should (string-match-p
                 "^M  a.txt"
                 (cdr (scrutiny-agent-magit-tests--ask "status"
                                                       "--porcelain"))))))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-commit-in-our-own-editor
  ;; with-editor's handshake, start to finish: git is given an "editor"
  ;; that announces the message file and waits; the announcement opens
  ;; the file here; finishing the edit signals the editor; git commits.
  (skip-unless (require 'with-editor nil t))
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn)
    (with-temp-file (expand-file-name "c.txt" repo) (insert "to commit\n"))
    (scrutiny-agent-magit-tests--with-modes
      (should (zerop (car (scrutiny-agent-magit-tests--ask "add" "c.txt"))))
      (let* ((make-backup-files nil)
             (process
              (with-temp-buffer
                (with-environment-variables
                    (("GIT_EDITOR" with-editor-sleeping-editor)
                     ("GIT_AUTHOR_NAME" "Scrutiny Test")
                     ("GIT_AUTHOR_EMAIL" "test@scrutiny.invalid")
                     ("GIT_COMMITTER_NAME" "Scrutiny Test")
                     ("GIT_COMMITTER_EMAIL" "test@scrutiny.invalid"))
                  (start-file-process "git" nil "git" "commit"))))
             (message-buffer nil))
        (process-put process 'default-dir default-directory)
        (set-process-filter process #'with-editor-process-filter)
        ;; The editor announces itself; with-editor visits the file.
        (let ((deadline (+ (float-time) 30)))
          (while (and (< (float-time) deadline)
                      (not (setq message-buffer
                                 (cl-find-if
                                  (lambda (buffer)
                                    (string-suffix-p
                                     "COMMIT_EDITMSG"
                                     (or (buffer-file-name buffer) "")))
                                  (buffer-list)))))
            (accept-process-output nil 0.05)))
        (should message-buffer)
        ;; with-editor posts a usage hint from a timer; let it run
        ;; while the buffer it refers to still exists.
        (sleep-for 0.2)
        (unwind-protect
            (with-current-buffer message-buffer
              (should (file-remote-p buffer-file-name))
              (should with-editor--pid)
              (goto-char (point-min))
              (insert "written in Emacs\n")
              ;; C-c C-c.
              (with-editor-return nil))
          (when (buffer-live-p message-buffer)
            (with-current-buffer message-buffer (set-buffer-modified-p nil))
            (kill-buffer message-buffer)))
        (scrutiny-agent-magit-tests--wait process)
        (should (= 0 (process-exit-status process)))
        (should (equal (string-trim
                        (cdr (scrutiny-agent-magit-tests--ask
                              "log" "-1" "--format=%s")))
                       "written in Emacs"))))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-killing-the-process-kills-git
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore repo)
    (scrutiny-agent-magit-tests--with-modes
      (let* ((process
              (with-environment-variables
                  (("GIT_EDITOR" "sh -c 'sleep 300'"))
                (start-file-process "git" nil "git" "commit"
                                    "--allow-empty")))
             (running (lambda ()
                        (plist-get (scrutiny-agent-request conn "meta.stat" nil)
                                   :lspTunnels))))
        (let ((deadline (+ (float-time) 10)))
          (while (and (< (float-time) deadline) (< (funcall running) 1))
            (accept-process-output nil 0.05)))
        (should (= 1 (funcall running)))
        (delete-process process)
        (let ((deadline (+ (float-time) 15)))
          (while (and (< (float-time) deadline) (> (funcall running) 0))
            (accept-process-output nil 0.1)))
        (should (= 0 (funcall running)))))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-async-can-be-turned-off
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn repo)
    (scrutiny-agent-magit-tests--with-modes
      (let ((scrutiny-agent-magit-async nil)
            (delegated nil))
        (cl-letf (((symbol-function 'scrutiny-agent-fs--delegate)
                   (lambda (operation _args) (setq delegated operation) nil)))
          (start-file-process "git" nil "git" "rev-parse" "HEAD"))
        (should (eq delegated 'start-file-process))))))

(ert-deftest scrutiny-agent-magit-unwraps-env ()
  ;; What with-editor turns `git commit' into in a remote directory.
  (should (equal (scrutiny-agent-magit--unwrap-env
                  '("env" "GIT_EDITOR=sh -c 'x=1; sleep'" "git" "commit" "-v"))
                 '("git" ("commit" "-v") (:GIT_EDITOR "sh -c 'x=1; sleep'"))))
  (should (equal (scrutiny-agent-magit--unwrap-env
                  '("/usr/bin/env" "A=1" "B=" "git" "status"))
                 '("git" ("status") (:A "1" :B ""))))
  ;; Anything else is left exactly as it was.
  (should (equal (scrutiny-agent-magit--unwrap-env '("git" "log" "A=1"))
                 '("git" ("log" "A=1") nil)))
  (should (equal (scrutiny-agent-magit--unwrap-env '("env"))
                 '(nil nil nil))))

(scrutiny-agent-magit-tests--deftest scrutiny-agent-magit-commit-as-with-editor-starts-it
  ;; The whole thing as it happens in a session: inside `with-editor',
  ;; in a remote directory, so the command that reaches us is
  ;; `env GIT_EDITOR=... git commit'.
  (skip-unless (require 'with-editor nil t))
  (scrutiny-agent-magit-tests--with-agent (conn repo)
    (ignore conn)
    (with-temp-file (expand-file-name "w.txt" repo) (insert "to commit\n"))
    (scrutiny-agent-magit-tests--with-modes
      (should (zerop (car (scrutiny-agent-magit-tests--ask "add" "w.txt"))))
      (let* ((make-backup-files nil)
             (handed-to-tramp nil)
             (process
              (cl-letf* ((delegate (symbol-function
                                    'scrutiny-agent-fs--delegate))
                         ((symbol-function 'scrutiny-agent-fs--delegate)
                          (lambda (operation args)
                            (when (memq operation '(start-file-process
                                                    make-process))
                              (push operation handed-to-tramp))
                            (funcall delegate operation args))))
                (with-environment-variables
                    (("GIT_AUTHOR_NAME" "Scrutiny Test")
                     ("GIT_AUTHOR_EMAIL" "test@scrutiny.invalid")
                     ("GIT_COMMITTER_NAME" "Scrutiny Test")
                     ("GIT_COMMITTER_EMAIL" "test@scrutiny.invalid"))
                  ;; As magit does: GIT_EDITOR, which git prefers over
                  ;; anything else that may be set on the host.
                  (with-editor* "GIT_EDITOR"
                    (start-file-process "git" nil "git" "commit")))))
             (message-buffer nil))
        (should-not handed-to-tramp)
        (let ((deadline (+ (float-time) 30)))
          (while (and (< (float-time) deadline)
                      (not (setq message-buffer
                                 (cl-find-if
                                  (lambda (buffer)
                                    (string-suffix-p
                                     "COMMIT_EDITMSG"
                                     (or (buffer-file-name buffer) "")))
                                  (buffer-list)))))
            (accept-process-output nil 0.05)))
        (should message-buffer)
        (sleep-for 0.2)
        (unwind-protect
            (with-current-buffer message-buffer
              (goto-char (point-min))
              (insert "through with-editor\n")
              (with-editor-return nil))
          (when (buffer-live-p message-buffer)
            (with-current-buffer message-buffer (set-buffer-modified-p nil))
            (kill-buffer message-buffer)))
        (scrutiny-agent-magit-tests--wait process)
        (should (= 0 (process-exit-status process)))
        (should (equal (string-trim
                        (cdr (scrutiny-agent-magit-tests--ask
                              "log" "-1" "--format=%s")))
                       "through with-editor"))))))

(provide 'scrutiny-agent-magit-tests)
;;; scrutiny-agent-magit-tests.el ends here
