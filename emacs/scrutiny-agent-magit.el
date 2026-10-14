;;; scrutiny-agent-magit.el --- Magit over a scrutiny-agent channel  -*- lexical-binding: t; -*-

;; Author: Lally Singh
;; URL: https://github.com/five-rivers/scrutiny-agent
;; Version: 0.2.0
;; (The package's requirements are declared in scrutiny-agent.el;
;; a Package-Requires header outside the main file has no effect.)
;; Keywords: tools, vc

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; Runs git on a remote repository through the agent's `git.exec'
;; instead of a TRAMP subprocess, so magit -- and vc, project.el,
;; diff-hl, anything else that runs git -- rides the one multiplexed
;; connection you already established.
;;
;;   (require 'scrutiny-agent-magit)
;;   (scrutiny-agent-magit-mode 1)
;;
;; Then use magit exactly as you always do on `/ssh:devbox:~/src/proj'.
;;
;; Nothing in magit is advised or redefined.  The seam is
;; `process-file', the operation every package uses to run a program
;; in a remote directory: the file-name handler in scrutiny-agent-fs.el
;; receives it, and hands invocations of git to this file.  A change
;; inside magit cannot break that, and a package this file has never
;; heard of gets the same treatment.
;;
;; Being on one connection is necessary and not sufficient.  A status
;; refresh runs forty-odd git commands one after another, each waiting
;; for the answer to the last; on a link with a 200 ms round trip that
;; is nine seconds however fast the far end is.  But the commands a
;; refresh runs are the same ones the previous refresh ran.  So the
;; first read-only command of a batch is sent together with every
;; command that accompanied it last time, the agent runs them
;; concurrently, and the rest of the refresh is answered from replies
;; that are already here: one round trip instead of forty.  See
;; `scrutiny-agent-magit-speculate'.
;;
;; Scope, stated plainly:
;;
;;   * Synchronous git -- magit's refresh, every `magit-git-*' reader,
;;     staging and unstaging files -- goes over the agent, including
;;     commands that read standard input and commands magit runs
;;     against a scratch index (stashing).
;;   * Asynchronous git -- what magit shows in its process buffer:
;;     commit, push, pull, fetch, rebase, and staging individual hunks
;;     -- still starts a process through TRAMP.  Those need a live
;;     process object, which `git.exec' does not provide.
;;   * A command the agent's policy refuses falls back to TRAMP, so a
;;     narrow allowlist costs speed, never function.  Set
;;     `scrutiny-agent-magit-fallback' to nil to surface refusals
;;     instead of silently working around them.

;;; Code:

(require 'cl-lib)
(require 'subr-x)
(require 'scrutiny-agent)
(require 'scrutiny-agent-ops)
(require 'scrutiny-agent-fs)

(defgroup scrutiny-agent-magit nil
  "Magit over a scrutiny-agent channel."
  :group 'scrutiny-agent
  :prefix "scrutiny-agent-magit-")

(defcustom scrutiny-agent-magit-host-alist nil
  "Alist mapping TRAMP host names to `scrutiny-agent-hosts' names.
Only needed when the two differ."
  :type '(alist :key-type string :value-type string))

(defcustom scrutiny-agent-magit-fallback t
  "Whether to fall back to TRAMP when the agent cannot run a command.
Non-nil (the default) keeps magit fully functional on an agent with a
narrow `--git-exec' allowlist: refused commands quietly take the slow
path.  Set to nil while tuning an allowlist, to see what is missing --
refusals are then reported instead of worked around."
  :type 'boolean)

(defcustom scrutiny-agent-magit-log-commands nil
  "When non-nil, log every routed git command to the host's log buffer.
Useful for building an allowlist: run magit, then read
`*scrutiny-agent-log[HOST]*' to see which subcommands it used."
  :type 'boolean)

(defcustom scrutiny-agent-magit-speculate t
  "Whether to run the git commands a refresh is about to ask for, early.

A refresh asks the same read-only questions every time -- the current
branch, its upstream, the status, the diffs, the recent log.  With
this on, the first such command in a batch is sent together with
every command that accompanied it the last time, and the later ones
are answered from the replies as they are asked for.  A refresh then
costs about one round trip instead of one per command.

Only commands that cannot change the repository are run early, and
an early answer is only ever used within the batch that produced it:
the answers are dropped the moment Emacs runs a command that might
change something, starts any other process on that host, saves a
file there, or simply returns to waiting for input.  What a refresh
shows is therefore the repository as it was when the refresh began,
which is what magit's own per-refresh cache already assumes.

A command that was not predicted is simply run when asked for, and
the prediction is corrected for next time."
  :type 'boolean)

(defcustom scrutiny-agent-magit-speculation-window 15.0
  "Seconds an early answer may be used after it was asked for.
A backstop only: answers are normally dropped far sooner, when Emacs
next waits for input."
  :type 'number)

(defcustom scrutiny-agent-magit-history-file
  (locate-user-emacs-file "scrutiny-agent-git-history.eld")
  "File remembering which git commands run together, across sessions.
Without it the first status of each session is run one command at a
time, since nothing has been observed yet.  Holds argument lists
only -- no output.  Nil keeps the history in memory."
  :type '(choice (const :tag "Do not save" nil) file))

(defvar scrutiny-agent-magit--depth 0
  "Recursion guard: non-zero while inside our own fallback call.")

;; ---------------------------------------------------------------------
;; Applicability
;; ---------------------------------------------------------------------

(defun scrutiny-agent-magit--host (directory)
  "Configured scrutiny-agent host serving remote DIRECTORY, or nil."
  (let ((tramp-host (file-remote-p (or directory "") 'host)))
    (when tramp-host
      (let ((name (or (cdr (assoc tramp-host scrutiny-agent-magit-host-alist))
                      tramp-host)))
	(and (assoc name scrutiny-agent-hosts) name)))))

(defun scrutiny-agent-magit--connection (host)
  "The connection to use for HOST, or nil.
The file handler's policy: a live connection, or a new one only when
`scrutiny-agent-fs-auto-connect' says connecting may block.  Trying to
connect from here unconditionally would do it before every one of
the dozens of commands in a refresh whenever the host is down."
  (scrutiny-agent-fs--conn host))

(defun scrutiny-agent-magit--usable (program infile buffer)
  "Host name to route this invocation to, or nil to let TRAMP have it.

Declines -- deliberately -- when:
  * we are already inside our own fallback (no recursion),
  * the program is not git,
  * output is asked for asynchronously (BUFFER of 0),
  * the directory is not on a configured host,
  * that host's agent does not advertise `git.exec' (it was started
    without an allowlist, so routing would fail on every call), or
  * standard input is redirected and the agent cannot supply it."
  (and (zerop scrutiny-agent-magit--depth)
       (stringp program)
       (member (file-name-nondirectory program) '("git" "git.exe"))
       (not (eq buffer 0))
       (let ((host (scrutiny-agent-magit--host default-directory)))
         (when host
           (let ((conn (scrutiny-agent-magit--connection host)))
             (and conn
                  (member "git.exec"
                          (scrutiny-agent--conn-capabilities conn))
                  (or (null infile)
                      (scrutiny-agent-feature-p conn "git.exec.stdin"))
                  host))))))

;; ---------------------------------------------------------------------
;; What a command carries besides its arguments
;; ---------------------------------------------------------------------

(defun scrutiny-agent-magit--environment ()
  "The environment this invocation adds to the default, as a plist.
Exactly what TRAMP would export to a remote command: the entries of
`process-environment' that some caller bound around this call.  They
are not decoration.  magit builds a stash by binding GIT_INDEX_FILE
to a scratch index and running `update-index' and `write-tree'; run
without that binding, those same commands rewrite the real index."
  (let ((default (default-toplevel-value 'process-environment))
        (seen nil)
        (out nil))
    (dolist (entry process-environment)
      (let* ((eq (string-search "=" entry))
             (name (if eq (substring entry 0 eq) entry)))
        ;; The first entry for a name is the one in effect.
        (unless (member name seen)
          (push name seen)
          (unless (member entry default)
            (setq out (nconc out (list (intern (concat ":" name))
                                       (if eq (substring entry (1+ eq))
                                         :null))))))))
    out))

(defconst scrutiny-agent-magit--reader-irrelevant
  '(:GIT_EDITOR :GIT_SEQUENCE_EDITOR :EDITOR :VISUAL :GIT_PAGER :PAGER
    :GIT_ASKPASS :SSH_ASKPASS :GIT_TERMINAL_PROMPT)
  "Variables that cannot affect a command which only reads.
An editor or a pager is never started by `rev-parse'.  They are
dropped from such commands rather than sent, because the agent
refuses a request that names them -- correctly, for a command that
might run one -- and a refusal sends the command down the slow path.
That matters: every reader magit runs while a commit is being written
is run with GIT_EDITOR bound.")

(defun scrutiny-agent-magit--reader-environment (env)
  "ENV without the variables a read-only command ignores."
  (let (out)
    (cl-loop for (name value) on env by #'cddr
             unless (memq name scrutiny-agent-magit--reader-irrelevant)
             do (setq out (nconc out (list name value))))
    out))

(defun scrutiny-agent-magit--stdin (infile)
  "The bytes of INFILE, base64-encoded for `git.exec', or nil."
  (when infile
    (with-temp-buffer
      (set-buffer-multibyte nil)
      (let ((coding-system-for-read 'binary))
        (insert-file-contents-literally infile))
      (base64-encode-string (buffer-string) t))))

(defun scrutiny-agent-magit--params (conn repo args env infile)
  "The `git.exec' parameters for running ARGS in REPO on CONN."
  (nconc (list :repoPath repo :args (vconcat args))
         (and env (scrutiny-agent-feature-p conn "git.exec.env")
              (list :env env))
         (and infile (list :stdinBase64 (scrutiny-agent-magit--stdin infile)))))

;; ---------------------------------------------------------------------
;; Speculation
;; ---------------------------------------------------------------------

(defvar scrutiny-agent-magit--answers (make-hash-table :test #'equal)
  "(HOST REPO . ARGS) -> (TIMESTAMP . FUTURE) for commands already sent.")

(defvar scrutiny-agent-magit--asked nil
  "Keys of the read-only commands asked for in the current batch.")

(defvar scrutiny-agent-magit--history nil
  "Hash table (HOST . REPO) -> list of batches, newest first.
Each batch is the list of argument lists that were asked for together.
Nil until first needed; see `scrutiny-agent-magit--history'.")

(defvar scrutiny-agent-magit--busy nil
  "Non-nil while a git invocation is being served (dynamically bound).")

(defvar scrutiny-agent-magit--timer nil
  "Timer that closes the current batch when Emacs next goes idle.")

(defvar scrutiny-agent-magit-stats (list :asked 0 :answered-early 0 :sent-early 0)
  "Counters since load: commands asked for, answered from an early
reply, and sent early.  The gap between the last two is waste.")

(defconst scrutiny-agent-magit--read-only
  '("rev-parse" "for-each-ref" "describe" "status" "diff" "log" "show"
    "ls-files" "ls-tree" "cat-file" "rev-list" "merge-base" "name-rev"
    "show-ref" "var" "version" "diff-index" "diff-files" "diff-tree"
    "blame" "shortlog" "check-ignore" "check-attr" "count-objects"
    "cherry" "patch-id" "whatchanged")
  "Subcommands whose answer depends only on the state of the repository.")

(defun scrutiny-agent-magit--subcommand (args)
  "ARGS from its subcommand on: (SUBCOMMAND . REST)."
  (let ((rest args))
    (while (and rest (string-prefix-p "-" (car rest)))
      (setq rest (if (equal (car rest) "-c") (cddr rest) (cdr rest))))
    rest))

(defun scrutiny-agent-magit--pure-p (args)
  "Whether running ARGS early, or answering it twice, is harmless.
True for commands that only read.  Subcommands that read or write
depending on their arguments count only in the forms known to read."
  (let* ((tail (scrutiny-agent-magit--subcommand args))
         (sub (car tail))
         (rest (cdr tail)))
    (cond
     ((member sub scrutiny-agent-magit--read-only) t)
     ((equal sub "config")
      (and (cl-some (lambda (a) (member a '("--list" "-l" "--get" "--get-all"
                                            "--get-regexp" "--get-urlmatch"
                                            "--get-color" "--get-colorbool")))
                    rest)
           t))
     ((equal sub "symbolic-ref")
      (and (= 1 (cl-count-if-not (lambda (a) (string-prefix-p "-" a)) rest))
           (not (cl-intersection rest '("-d" "--delete" "-m") :test #'equal))))
     ((equal sub "stash") (equal (car rest) "list"))
     ((equal sub "remote") (member rest '(nil ("-v") ("--verbose"))))
     ((equal sub "branch")
      (and (member (car rest) '("--list" "-l" "--show-current")) t))
     ((equal sub "tag")
      (and (member (car rest) '("--list" "-l")) t))
     ((equal sub "worktree") (equal (car rest) "list"))
     ;; Re-reads stat information into the index.  It writes, but only
     ;; what it would write however many times it ran.
     ((equal sub "update-index") (equal rest '("--refresh")))
     (t nil))))

(defun scrutiny-agent-magit--wire-args (args)
  "ARGS as actually sent for an early command.
Readers running side by side must not take the index lock to save a
refreshed copy -- one of them would win and the rest would fail."
  (if (equal (cdr (scrutiny-agent-magit--subcommand args)) '("--refresh"))
      args
    (cons "--no-optional-locks" args)))

(defun scrutiny-agent-magit--history ()
  "The history table, read from disk the first time it is wanted."
  (or scrutiny-agent-magit--history
      (setq scrutiny-agent-magit--history
            (or (and scrutiny-agent-magit-history-file
                     (file-readable-p scrutiny-agent-magit-history-file)
                     (ignore-errors
                       (with-temp-buffer
                         (insert-file-contents
                          scrutiny-agent-magit-history-file)
                         (let ((table (make-hash-table :test #'equal)))
                           (dolist (cell (read (current-buffer)))
                             (puthash (car cell) (cdr cell) table))
                           table))))
                (make-hash-table :test #'equal)))))

(defun scrutiny-agent-magit-save-history ()
  "Write the history to `scrutiny-agent-magit-history-file'."
  (when (and scrutiny-agent-magit-history-file scrutiny-agent-magit--history)
    (ignore-errors
      (let ((cells nil))
        (maphash (lambda (key batches) (push (cons key batches) cells))
                 scrutiny-agent-magit--history)
        (make-directory (file-name-directory
                         scrutiny-agent-magit-history-file)
                        t)
        (with-temp-file scrutiny-agent-magit-history-file
          (let ((print-length nil) (print-level nil))
            (prin1 cells (current-buffer))))))))

(defun scrutiny-agent-magit--predict (host repo args)
  "The commands that accompanied ARGS the last time it was run.

A batch that ARGS opened is preferred to one it merely appeared in.
The same question is asked in more than one context -- magit finds
the top of the working tree before staging a file as well as during
a refresh -- and the context that begins with it is the better guess
at what follows; guessing the other way runs a whole refresh's worth
of commands on the far end for nobody.

Looked for in this repository's history first.  Failing that, in any
repository's: a status refresh asks nearly the same questions
everywhere, and a guess that turns out wrong for this repository
costs the agent a command nobody reads."
  (let* ((table (scrutiny-agent-magit--history))
         (own (gethash (cons host repo) table))
         (all nil))
    (or (cl-find-if (lambda (batch) (equal (car batch) args)) own)
        (cl-find-if (lambda (batch) (member args batch)) own)
        (progn
          (maphash (lambda (_key batches) (setq all (append batches all)))
                   table)
          (or (cl-find-if (lambda (batch) (equal (car batch) args)) all)
              (cl-find-if (lambda (batch) (member args batch)) all))))))

(defun scrutiny-agent-magit--end-batch ()
  "Close the current batch: remember what was asked, drop the answers."
  (when scrutiny-agent-magit--timer
    (cancel-timer scrutiny-agent-magit--timer)
    (setq scrutiny-agent-magit--timer nil))
  (when scrutiny-agent-magit--asked
    (let ((by-repo (make-hash-table :test #'equal))
          (table (scrutiny-agent-magit--history)))
      (dolist (key scrutiny-agent-magit--asked)
        (push (cddr key) (gethash (cons (car key) (cadr key)) by-repo)))
      (maphash
       (lambda (repo-key batch)
         ;; A lone command predicts nothing.
         (when (cdr batch)
           (puthash repo-key
                    (seq-take
                     (cons batch
                           ;; The same batch seen again replaces itself.
                           (cl-remove-if
                            (lambda (old)
                              (and (equal (car old) (car batch))
                                   (cl-subsetp old batch :test #'equal)
                                   (cl-subsetp batch old :test #'equal)))
                            (gethash repo-key table)))
                     12)
                    table)))
       by-repo))
    (setq scrutiny-agent-magit--asked nil))
  (clrhash scrutiny-agent-magit--answers))

(defun scrutiny-agent-magit--on-idle ()
  "Close the batch once Emacs is waiting for something other than git.
Timers run whenever Emacs waits -- including while it waits for a git
answer in the middle of a refresh, which is not the end of anything."
  (setq scrutiny-agent-magit--timer nil)
  (if scrutiny-agent-magit--busy
      (setq scrutiny-agent-magit--timer
            (run-at-time 0.05 nil #'scrutiny-agent-magit--on-idle))
    (scrutiny-agent-magit--end-batch)))

(defun scrutiny-agent-magit--answer (key)
  "The future for KEY if one was sent recently enough to use."
  (let ((entry (gethash key scrutiny-agent-magit--answers)))
    (and entry
         (< (float-time (time-since (car entry)))
            scrutiny-agent-magit-speculation-window)
         (cdr entry))))

(defun scrutiny-agent-magit--exec (conn host repo args env infile timeout)
  "Run git ARGS in REPO on HOST and return the `git.exec' result.
ENV is the extra environment and INFILE the file to feed it."
  (cl-incf (plist-get scrutiny-agent-magit-stats :asked))
  (if (not (and scrutiny-agent-magit-speculate
                (null infile)
                ;; Anything beyond the constant decoration (INSIDE_EMACS,
                ;; locale) means the command is aimed somewhere special
                ;; -- a scratch index, say -- and is not one to guess at.
                (cl-loop for (name _value) on env by #'cddr
                         always (memq name '(:INSIDE_EMACS :LC_ALL :LANG
                                             :LC_MESSAGES :TERM)))
                (scrutiny-agent-magit--pure-p args)))
      ;; A command that may change the repository.  Nothing asked
      ;; before it finishes may be answered after it.
      (progn
        (scrutiny-agent-magit--end-batch)
        (unwind-protect
            (scrutiny-agent-request
             conn "git.exec"
             (scrutiny-agent-magit--params conn repo args env infile)
             timeout)
          (scrutiny-agent-magit--end-batch)
          (scrutiny-agent-fs--after-foreign-change conn host)))
    (let ((key (cons host (cons repo args))))
      (cl-pushnew key scrutiny-agent-magit--asked :test #'equal)
      (if (scrutiny-agent-magit--answer key)
          (cl-incf (plist-get scrutiny-agent-magit-stats :answered-early))
        ;; Not here yet: send it, and with it everything that followed
        ;; it last time.  One burst, one round trip.
        (dolist (other (cons args (scrutiny-agent-magit--predict
                                   host repo args)))
          (let ((other-key (cons host (cons repo other))))
            (unless (scrutiny-agent-magit--answer other-key)
              (unless (equal other args)
                (cl-incf (plist-get scrutiny-agent-magit-stats :sent-early)))
              (puthash other-key
                       (cons (current-time)
                             (scrutiny-agent-send
                              conn "git.exec"
                              (scrutiny-agent-magit--params
                               conn repo
                               (scrutiny-agent-magit--wire-args other)
                               env nil)))
                       scrutiny-agent-magit--answers))))
        (unless scrutiny-agent-magit--timer
          (setq scrutiny-agent-magit--timer
                (run-at-time 0 nil #'scrutiny-agent-magit--on-idle))))
      (scrutiny-agent-await conn (cdr (gethash key scrutiny-agent-magit--answers))
                            timeout))))

;; ---------------------------------------------------------------------
;; Output placement
;; ---------------------------------------------------------------------

(defun scrutiny-agent-magit--emit (destination text)
  "Insert TEXT where `process-file' would put it for DESTINATION.
DESTINATION follows `process-file' BUFFER semantics: nil discards, t
means the current buffer, a buffer means that buffer, and a string
names a file to append to."
  (when (and text (not (string-empty-p text)))
    (cond
     ((null destination))                       ; discarded
     ((eq destination t) (insert text))
     ((bufferp destination)
      (with-current-buffer destination (insert text)))
     ((stringp destination)
      ;; A stderr file, which is how magit captures stderr separately.
      (let ((coding-system-for-write 'utf-8-unix))
        (write-region text nil destination t 'silent))))))

(defun scrutiny-agent-magit--place-output (buffer result)
  "Route RESULT's stdout/stderr per the `process-file' BUFFER argument."
  (let ((stdout (plist-get result :stdout))
        (stderr (plist-get result :stderr)))
    (if (and (consp buffer) (not (eq (car buffer) :file)))
        ;; (STDOUT-DESTINATION STDERR-DESTINATION): the two streams are
        ;; kept apart, which is exactly why git.exec returns them apart.
        (progn
          (scrutiny-agent-magit--emit (car buffer) stdout)
          (let ((err (cadr buffer)))
            (scrutiny-agent-magit--emit (if (eq err t) (car buffer) err)
                                        stderr)))
      ;; A single destination: `process-file' merges both streams into
      ;; it, so we do too.
      (scrutiny-agent-magit--emit buffer stdout)
      (scrutiny-agent-magit--emit buffer stderr))))

;; ---------------------------------------------------------------------
;; The advice
;; ---------------------------------------------------------------------

(defun scrutiny-agent-magit--run (original program infile buffer display args)
  "Run one git invocation over the agent, or hand it back to ORIGINAL."
  (let ((host (scrutiny-agent-magit--usable program infile buffer)))
    (if (not host)
        (progn
          ;; Some other program is about to run on a host we answer
          ;; for.  Whatever it does, nothing asked before it may be
          ;; answered after it.
          (when (scrutiny-agent-magit--host default-directory)
            (scrutiny-agent-magit--end-batch))
          (apply original program infile buffer display args))
      (let* ((conn (scrutiny-agent-magit--connection host))
             (repo (file-local-name (expand-file-name default-directory)))
             (flat (flatten-tree args))
             (env (let ((env (scrutiny-agent-magit--environment)))
                    (if (and (null infile)
                             (scrutiny-agent-magit--pure-p flat))
                        (scrutiny-agent-magit--reader-environment env)
                      env)))
             (scrutiny-agent-magit--busy t))
        (when scrutiny-agent-magit-log-commands
          (scrutiny-agent--log conn "magit: git %s"
                               (string-join flat " ")))
        (condition-case err
            (if (and env (not (scrutiny-agent-feature-p conn "git.exec.env"))
                     (cl-loop for (name _value) on env by #'cddr
                              thereis (not (memq name '(:INSIDE_EMACS :TERM)))))
                ;; An agent that cannot carry the environment must not
                ;; be sent a command whose meaning depends on it.
                (let ((scrutiny-agent-magit--depth 1))
                  (scrutiny-agent-magit--end-batch)
                  (apply original program infile buffer display args))
              (let ((result (scrutiny-agent-magit--exec
                             conn host repo flat env infile 300)))
                (scrutiny-agent-magit--place-output buffer result)
                (when (eq (plist-get result :truncated) t)
                  (message "scrutiny-agent: git output truncated at 16 MiB"))
                (plist-get result :exitCode)))
          (scrutiny-agent-rpc-error
           (scrutiny-agent--log conn "magit: git.exec refused (%s: %s): %s"
                                (scrutiny-agent-ops-error-name (nth 1 err))
                                (nth 2 err) (string-join flat " "))
           (if scrutiny-agent-magit-fallback
               ;; The allowlist is a performance boundary, not a
               ;; functional one: take the slow path rather than break
               ;; a command the user asked for.
               (let ((scrutiny-agent-magit--depth 1))
                 (scrutiny-agent-magit--end-batch)
                 (apply original program infile buffer display args))
             (signal (car err) (cdr err))))
          ;; A dead transport must not take magit down with it.
          (error
           (scrutiny-agent--log conn "magit: routing failed: %S" err)
           (if scrutiny-agent-magit-fallback
               (let ((scrutiny-agent-magit--depth 1))
                 (scrutiny-agent-magit--end-batch)
                 (apply original program infile buffer display args))
             (signal (car err) (cdr err)))))))))

(defun scrutiny-agent-magit--signal (program args)
  "Serve `kill -s SIGNAL PID' for a process a git command started.
This is how with-editor tells the placeholder editor holding a commit
open that the message has been written (USR1) or abandoned (USR2).
Returns the exit status, or nil if this is not such a call or the
agent will not deliver it -- the pid is not one of ours."
  (when (and (stringp program)
             (equal (file-name-nondirectory program) "kill"))
    (let* ((flat (flatten-tree args))
           (signal (cond ((equal (car flat) "-s") (nth 1 flat))
                         ((string-match "\\`-\\([A-Z0-9]+\\)\\'"
                                        (or (car flat) ""))
                          (match-string 1 (car flat)))))
           (pid (car (last flat)))
           (host (scrutiny-agent-magit--host default-directory))
           (conn (and host (scrutiny-agent-magit--connection host))))
      (when (and signal conn
                 (or (integerp pid)
                     (and (stringp pid) (string-match-p "\\`[0-9]+\\'" pid)))
                 (member "git.signal" (scrutiny-agent--conn-capabilities conn)))
        (condition-case nil
            (if (eq t (plist-get
                       (scrutiny-agent-request
                        conn "git.signal"
                        (list :pid (if (stringp pid) (string-to-number pid) pid)
                              :signal signal)
                        30)
                       :ok))
                0
              1)
          (error nil))))))

(defun scrutiny-agent-magit--process-file (program infile buffer display args)
  "Serve `process-file' for the file handler.
Returns the exit status, or `decline' for TRAMP to run it."
  (or (scrutiny-agent-magit--signal program args)
      (let ((declined nil))
        (let ((status (scrutiny-agent-magit--run
                       (lambda (&rest _) (setq declined t) nil)
                       program infile buffer display args)))
          (if declined 'decline status)))))

;; ---------------------------------------------------------------------
;; Asynchronous git: a real process object, a remote process
;; ---------------------------------------------------------------------
;;
;; A caller of `start-file-process' wants a PROCESS: something with a
;; filter and a sentinel, that it can send input and end-of-input to,
;; that it can kill, whose exit status it can read.  magit's process
;; buffer, its hunk staging and with-editor's commit handshake are all
;; written against that object, and none of it can be imitated with
;; anything less.
;;
;; So the object handed back IS a process: a small local shell, whose
;; standard input and output are joined to the remote git by two named
;; pipes, and which exits with the remote command's exit status.  The
;; remote end is a tunnel from the agent's `git.spawn'.
;;
;;   caller --stdin--> stub --[in]--> reader ==tunnel==> git on the host
;;   caller <-stdout-- stub <-[out]-- writer <==tunnel== git on the host
;;                     stub <-[rc]--- exit status, when git has ended
;;
;; The stub is `sh', `cat' and three FIFOs; nothing is installed.

(defcustom scrutiny-agent-magit-async t
  "Whether asynchronous git runs over the agent as well.

That is everything magit shows in its process buffer -- commit, push,
pull, fetch, rebase -- and staging or discarding individual hunks.
Left to TRAMP, each of these opens a new session to the host; on a
link where that takes seconds, it is seconds per hunk.  With this on
they use the connection that is already open.

Passwords and passphrases are asked for in the minibuffer: git on the
host has no terminal to prompt on, so its prompts are relayed here
(see `scrutiny-agent-credential-function').

Needs `mkfifo' locally (macOS and Linux have it) and an agent that
offers `git.spawn'.  Anything that cannot be served falls back to
TRAMP."
  :type 'boolean)

(defconst scrutiny-agent-magit--stub
  "exec 3<&0
cat <&3 >\"$1\" &
inpid=$!
cat <\"$2\"
rc=255
read rc <\"$3\"
kill \"$inpid\" 2>/dev/null
exit \"$rc\"
"
  "The local stand-in for a remote git process.
$1 is the FIFO its input is copied to, $2 the FIFO the remote output
arrives on, $3 the FIFO that delivers the exit status once the remote
command has ended.  Standard input is duplicated first because a
background command in `sh' is otherwise given /dev/null.")

(defun scrutiny-agent-magit--bridge (conn spawned plist)
  "Return a local process standing in for the remote one in SPAWNED.
SPAWNED is the result of `git.spawn' on CONN; PLIST the `make-process'
arguments the caller gave."
  (let* ((dir (make-temp-file "scrutiny-git" t))
         (in (expand-file-name "in" dir))
         (out (expand-file-name "out" dir))
         (rc (expand-file-name "rc" dir))
         (tunnel nil) (finished nil)
         (stub nil) (reader nil) (writer nil) (status nil) (watchdog nil)
         (cleanup
          ;; The helpers end by themselves when the pipes close; any
          ;; that has not is KILLED BY SIGNAL, never with
          ;; `delete-process'.  Deleting a process makes Emacs run every
          ;; sentinel that is due, synchronously, from inside the
          ;; deletion -- including the caller's sentinel for the stub.
          ;; magit's refreshes buffers, which waits on this connection,
          ;; and a wait nested inside a deletion can stall indefinitely.
          ;; A signal just ends the helper; its sentinel runs later, in
          ;; the ordinary way.
          (lambda ()
            (when watchdog (cancel-timer watchdog) (setq watchdog nil))
            (run-at-time
             0.5 nil
             (lambda ()
               (dolist (helper (list reader writer status))
                 (when (process-live-p helper)
                   (ignore-errors (signal-process helper 'SIGKILL))))
               (ignore-errors (delete-directory dir t)))))))
    (condition-case err
        (progn
          (let ((default-directory temporary-file-directory)
                (process-environment
                 (default-toplevel-value 'process-environment)))
            (unless (zerop (call-process "mkfifo" nil nil nil in out rc))
              (error "mkfifo failed"))
            ;; What git prints, on its way to the stub's standard output.
            (setq writer
                  (make-process
                   :name "scrutiny-git-out" :noquery t
                   :connection-type 'pipe :coding 'binary
                   :command (list "sh" "-c" "exec cat >\"$1\"" "sh" out)))
            ;; What the caller sends, on its way to git.
            (setq reader
                  (make-process
                   :name "scrutiny-git-in" :noquery t
                   :connection-type 'pipe :coding 'binary
                   :command (list "sh" "-c" "exec cat <\"$1\"" "sh" in)
                   :filter (lambda (_process bytes)
                             (when (and tunnel (not finished))
                               (scrutiny-agent-tunnel-send tunnel bytes)))
                   :sentinel
                   (lambda (process _event)
                     (unless (or finished (process-live-p process))
                       (if (process-live-p stub)
                           ;; The caller closed the input.
                           (scrutiny-agent-tunnel-eof tunnel)
                         ;; The caller killed the process.
                         (scrutiny-agent-tunnel-close tunnel)))))))
          ;; The process the caller gets.  Started under the caller's
          ;; own bindings: its coding systems are theirs to choose.
          (setq stub
                (let ((default-directory temporary-file-directory))
                  (make-process
                   :name (or (plist-get plist :name) "git")
                   :buffer (plist-get plist :buffer)
                   :command (list "sh" "-c" scrutiny-agent-magit--stub
                                  "sh" in out rc)
                   :connection-type 'pipe
                   :coding (plist-get plist :coding)
                   :noquery (plist-get plist :noquery)
                   :stop (plist-get plist :stop)
                   :filter (plist-get plist :filter)
                   :sentinel (plist-get plist :sentinel))))
          (process-put stub 'scrutiny-agent-remote-pid
                       (plist-get spawned :pid))
          (setq tunnel
                (scrutiny-agent-tunnel-adopt
                 conn (plist-get spawned :tunnelId)
                 (lambda (bytes)
                   (when (process-live-p writer)
                     (process-send-string writer bytes)))
                 nil
                 (lambda (exit-code _reason)
                   (setq finished t)
                   ;; End of output, then the status; the stub prints
                   ;; the one and exits with the other.
                   (when (process-live-p writer) (process-send-eof writer))
                   (if (not (process-live-p stub))
                       (funcall cleanup)
                     (let ((default-directory temporary-file-directory))
                       (setq status
                             (make-process
                              :name "scrutiny-git-rc" :noquery t
                              :command
                              (list "sh" "-c" "echo \"$2\" >\"$1\"" "sh" rc
                                    (number-to-string
                                     (if (integerp exit-code)
                                         (logand exit-code 255)
                                       255)))
                              :sentinel
                              (lambda (process _event)
                                (unless (process-live-p process)
                                  (funcall cleanup))))))))))
          ;; A caller that kills the process after closing its input
          ;; leaves no pipe to notice it by.
          (setq watchdog
                (run-at-time
                 1 1 (lambda ()
                       (unless (process-live-p stub)
                         (unless finished
                           (setq finished t)
                           (scrutiny-agent-tunnel-close tunnel))
                         (funcall cleanup)))))
          stub)
      (error
       (scrutiny-agent--log conn "magit: could not bridge a process: %S" err)
       (when (process-live-p stub)
         (ignore-errors (signal-process stub 'SIGKILL)))
       (funcall cleanup)
       (ignore-errors
         (scrutiny-agent-async-request
          conn "lsp.tunnelClose"
          (list :tunnelId (plist-get spawned :tunnelId))))
       nil))))

(defun scrutiny-agent-magit--unwrap-env (command)
  "Split COMMAND into what it runs and what it sets: (PROGRAM ARGS ENV).
with-editor cannot bind `process-environment' for a remote process,
so in a remote directory it rewrites the command it is about to start
into `env GIT_EDITOR=... git commit ...'.  That is the same request
as running git with the variable set, and is treated as one: ENV is a
plist of the assignments, PROGRAM and ARGS what follows them."
  (let ((program (car command))
        (args (cdr command))
        (env nil))
    (when (and (stringp program)
               (equal (file-name-nondirectory program) "env"))
      (while (and args (stringp (car args))
                  (string-match "\\`\\([A-Za-z_][A-Za-z0-9_]*\\)=" (car args)))
        (setq env (nconc env
                         (list (intern (concat ":" (match-string 1 (car args))))
                               (substring (car args) (match-end 0)))))
        (setq args (cdr args)))
      (setq program (car args)
            args (cdr args)))
    (list program args env)))

(defun scrutiny-agent-magit--start-process (plist)
  "Serve an asynchronous process start for the file handler.
PLIST is a `make-process' argument list.  Returns the process, or
`decline' for TRAMP to start it."
  (let* ((unwrapped (scrutiny-agent-magit--unwrap-env
                     (plist-get plist :command)))
         (program (nth 0 unwrapped))
         (host (and scrutiny-agent-magit-async
                    (null (plist-get plist :stderr))
                    (scrutiny-agent-magit--usable program nil t)))
         (conn (and host (scrutiny-agent-magit--connection host))))
    (if (not (and conn
                  (member "git.spawn" (scrutiny-agent--conn-capabilities conn))
                  (scrutiny-agent-feature-p conn "lsp.tunnelSend.eof")
                  (executable-find "mkfifo")))
        'decline
      (scrutiny-agent-magit--end-batch)
      (let* ((repo (file-local-name (expand-file-name default-directory)))
             (args (flatten-tree (nth 1 unwrapped)))
             ;; An assignment on the command line outranks the
             ;; environment it would have been run in.
             (env (let ((env (scrutiny-agent-magit--environment)))
                    (cl-loop for (name value) on (nth 2 unwrapped) by #'cddr
                             do (setq env (plist-put env name value)))
                    env))
             (spawned
              (condition-case err
                  (scrutiny-agent-request
                   conn "git.spawn"
                   (nconc (list :repoPath repo :args (vconcat args))
                          (and env (list :env env)))
                   60)
                (scrutiny-agent-rpc-error
                 (scrutiny-agent--log
                  conn "magit: git.spawn refused (%s: %s): %s"
                  (scrutiny-agent-ops-error-name (nth 1 err)) (nth 2 err)
                  (string-join args " "))
                 nil)
                (error nil))))
        (when scrutiny-agent-magit-log-commands
          (scrutiny-agent--log conn "magit: (async) git %s"
                               (string-join args " ")))
        (or (and spawned (scrutiny-agent-magit--bridge conn spawned plist))
            'decline)))))

;;;###autoload
(define-minor-mode scrutiny-agent-magit-mode
  "Run git on configured remote hosts over scrutiny-agent connections.

Affects only remote directories on hosts configured in
`scrutiny-agent-hosts' whose agent advertises `git.exec'; local
repositories and unconfigured hosts are untouched.

Despite the name this is not specific to magit, and it does not touch
magit: it serves `process-file' for git through the file handler, so
it requires `scrutiny-agent-fs-mode' and turns it on."
  :global t
  :group 'scrutiny-agent-magit
  (remove-hook 'post-command-hook #'scrutiny-agent-magit--end-batch)
  (remove-hook 'kill-emacs-hook #'scrutiny-agent-magit-save-history)
  (remove-hook 'scrutiny-agent-fs-change-hook
               #'scrutiny-agent-magit--end-batch)
  (scrutiny-agent-magit--end-batch)
  ;; An older release advised magit; make sure none of that is left.
  (advice-remove 'magit-process-file 'scrutiny-agent)
  (if (not scrutiny-agent-magit-mode)
      (progn (setq scrutiny-agent-fs-process-file-function nil
                   scrutiny-agent-fs-start-process-function nil)
             (scrutiny-agent-magit-save-history))
    (unless scrutiny-agent-fs-mode (scrutiny-agent-fs-mode 1))
    (setq scrutiny-agent-fs-process-file-function
          #'scrutiny-agent-magit--process-file
          scrutiny-agent-fs-start-process-function
          #'scrutiny-agent-magit--start-process)
    (add-hook 'post-command-hook #'scrutiny-agent-magit--end-batch)
    (add-hook 'kill-emacs-hook #'scrutiny-agent-magit-save-history)
    ;; A file saved, renamed or deleted on the host changes what git
    ;; would say about it.
    (add-hook 'scrutiny-agent-fs-change-hook
              #'scrutiny-agent-magit--end-batch)))

;;;###autoload
(defun scrutiny-agent-magit-check (&optional host)
  "Report whether HOST's agent can serve magit, and what it allows.
Names the subcommands magit is likely to need that this agent would
refuse, so an allowlist can be widened deliberately."
  (interactive)
  (let* ((name (or host
                   (completing-read "Host: "
                                    (mapcar #'car scrutiny-agent-hosts)
                                    nil t)))
         (conn (scrutiny-agent-connect name))
         (has-exec (member "git.exec"
                           (scrutiny-agent--conn-capabilities conn))))
    (if (not has-exec)
        (message
         "scrutiny-agent[%s]: agent does not serve git.exec; start it with %s"
         name "--git-exec-preset magit")
      ;; Probe with the subcommands a magit session actually leans on.
      (let* ((repo (read-string "Remote repository to probe: "))
             (refused nil))
        (dolist (subcommand '("status" "log" "diff" "rev-parse" "show"
                              "for-each-ref" "ls-files" "stash" "add"
                              "reset" "checkout" "merge-base" "cat-file"))
          (condition-case err
              (scrutiny-agent-request
               conn "git.exec"
               (list :repoPath repo :args (vector subcommand "--help-does-not-matter"))
               60)
            (scrutiny-agent-rpc-error
             ;; 1005 is the allowlist talking; anything else means the
             ;; subcommand was permitted and git simply disliked the
             ;; arguments, which is what we want to see.
             (when (equal (nth 1 err) 1005)
               (push subcommand refused)))))
        (if refused
            (message "scrutiny-agent[%s]: magit will fall back to TRAMP for: %s"
                     name (string-join (nreverse refused) " "))
          (message "scrutiny-agent[%s]: every probed subcommand is allowed"
                   name))))))

(provide 'scrutiny-agent-magit)
;;; scrutiny-agent-magit.el ends here
