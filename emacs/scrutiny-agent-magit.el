;;; scrutiny-agent-magit.el --- Magit over a scrutiny-agent channel  -*- lexical-binding: t; -*-

;; Author: Lally Singh
;; URL: https://github.com/lally/scrutiny-agent
;; Version: 0.2.0
;; (The package's requirements are declared in scrutiny-agent.el;
;; a Package-Requires header outside the main file has no effect.)
;; Keywords: tools, vc

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; Routes magit's synchronous git invocations through the agent's
;; `git.exec' instead of a TRAMP subprocess, so magit on a remote
;; repository rides the one multiplexed connection you already
;; established.
;;
;; Why this matters: a single `magit-status' refresh runs dozens of git
;; commands.  Over TRAMP each one is a round trip through a remote
;; shell; on a link where session setup is expensive (Teleport, a jump
;; host, MFA'd SSH) that is the difference between a refresh taking a
;; moment and taking a coffee break.
;;
;; Setup, given a host in `scrutiny-agent-hosts' whose agent was
;; started with `--git-exec-preset magit':
;;
;;   (require 'scrutiny-agent-magit)
;;   (scrutiny-agent-magit-mode 1)
;;
;; Then use magit exactly as you always do on `/ssh:devbox:~/src/proj'.
;; Nothing about magit's interface changes.
;;
;; Scope, stated plainly:
;;
;;   * Synchronous git (magit's refresh, every `magit-git-*' reader)
;;     goes over the agent.  That is the bulk of what magit runs and
;;     effectively all of what makes it slow remotely.
;;   * Asynchronous git -- the commands magit shows in its process
;;     buffer, and anything needing an interactive editor (commit,
;;     interactive rebase) -- still goes through TRAMP.  Those need a
;;     live process object and `with-editor', which `git.exec' does not
;;     provide.
;;   * A command the agent's allowlist refuses falls back to TRAMP, so
;;     a narrow allowlist costs speed, never function.  Set
;;     `scrutiny-agent-magit-fallback' to nil to surface refusals
;;     instead of silently working around them.

;;; Code:

(require 'cl-lib)
(require 'subr-x)
(require 'scrutiny-agent)
(require 'scrutiny-agent-ops)

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

(defun scrutiny-agent-magit--usable (program infile buffer)
  "Host name to route this invocation to, or nil to let TRAMP have it.

Declines -- deliberately -- when:
  * we are already inside our own fallback (no recursion),
  * the program is not git,
  * stdin is redirected (`git.exec' has no stdin channel),
  * output is asked for asynchronously (BUFFER of 0),
  * the directory is not on a configured host, or
  * that host's agent does not advertise `git.exec' (it was started
    without an allowlist, so routing would fail on every call)."
  (and (zerop scrutiny-agent-magit--depth)
       (stringp program)
       (member (file-name-nondirectory program) '("git" "git.exe"))
       (null infile)
       (not (eq buffer 0))
       (let ((host (scrutiny-agent-magit--host default-directory)))
         (when host
           (let ((conn (ignore-errors (scrutiny-agent-connect host))))
             (and conn
                  (member "git.exec"
                          (scrutiny-agent--conn-capabilities conn))
                  host))))))

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
        (apply original program infile buffer display args)
      (let* ((conn (scrutiny-agent-connect host))
             (repo (file-local-name (expand-file-name default-directory)))
             (flat (flatten-tree args)))
        (when scrutiny-agent-magit-log-commands
          (scrutiny-agent--log conn "magit: git %s"
                               (string-join flat " ")))
        (condition-case err
            (let ((result (scrutiny-agent-request
                           conn "git.exec"
                           (list :repoPath repo :args (vconcat flat))
                           300)))
              (scrutiny-agent-magit--place-output buffer result)
              (when (eq (plist-get result :truncated) t)
                (message "scrutiny-agent: git output truncated at 16 MiB"))
              (plist-get result :exitCode))
          (scrutiny-agent-rpc-error
           (scrutiny-agent--log conn "magit: git.exec refused (%s: %s): %s"
                                (scrutiny-agent-ops-error-name (nth 1 err))
                                (nth 2 err) (string-join flat " "))
           (if scrutiny-agent-magit-fallback
               ;; The allowlist is a performance boundary, not a
               ;; functional one: take the slow path rather than break
               ;; a command the user asked for.
               (let ((scrutiny-agent-magit--depth 1))
                 (apply original program infile buffer display args))
             (signal (car err) (cdr err))))
          ;; A dead transport must not take magit down with it.
          (error
           (scrutiny-agent--log conn "magit: routing failed: %S" err)
           (if scrutiny-agent-magit-fallback
               (let ((scrutiny-agent-magit--depth 1))
                 (apply original program infile buffer display args))
             (signal (car err) (cdr err)))))))))

(defun scrutiny-agent-magit--advice (original program &optional infile buffer
                                              display &rest args)
  "Advice for `magit-process-file'."
  (scrutiny-agent-magit--run original program infile buffer display args))

;;;###autoload
(define-minor-mode scrutiny-agent-magit-mode
  "Route magit's synchronous git calls over scrutiny-agent connections.

Affects only remote directories on hosts configured in
`scrutiny-agent-hosts' whose agent advertises `git.exec'; local
repositories and unconfigured hosts are untouched."
  :global t
  :group 'scrutiny-agent-magit
  (if scrutiny-agent-magit-mode
      (advice-add 'magit-process-file :around
                  #'scrutiny-agent-magit--advice
                  '((name . scrutiny-agent)))
    (advice-remove 'magit-process-file #'scrutiny-agent-magit--advice)))

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
