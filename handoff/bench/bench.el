;;; bench.el --- round-trip benchmark for scrutiny-agent's Emacs side -*- lexical-binding: t; -*-

;; Batch harness.  For each labelled step it prints wall time, the
;; number of agent requests, how many "bursts" they went out in (a
;; burst = requests sent without waiting in between, i.e. ONE round
;; trip), the number of TRAMP commands, and every operation that was
;; handed to TRAMP.  The two numbers that must stay small are bursts
;; and tramp-cmds.
;;
;; Everything is configured from the environment (see HANDOFF.md):
;;
;;   SCENARIO          scenario file to load (s-files.el, s-magit.el, ...)
;;   BENCH_HOST        host name as configured AND as it appears in the
;;                     TRAMP file name            (default "localhost")
;;   BENCH_TRANSPORT   :transport command         (default: relay.py + sh)
;;   BENCH_PREFIX      TRAMP prefix of the host   (default "/su:localhost:")
;;   BENCH_REPO        repository on the remote, relative to the remote
;;                     home; must be a clone of scrutiny-agent
;;                                                (default "src/proj")
;;   BENCH_ROOTS       comma-separated :roots     (default "src")
;;   BENCH_OOB         command prefix that runs a shell command on the
;;                     remote WITHOUT Emacs, for setup/cleanup
;;                                                (default "sh -c")
;;   AGENT_BIN         locally built agent binary to stream to the host
;;   DELAY             one-way delay in seconds injected by relay.py when
;;                     BENCH_TRANSPORT is unset   (default 0.1 = 200ms RTT)
;;   BENCH_INSTALL_DIR :install-dir on the remote (default: the client's)
;;   NOCOMPRESS, TTL, BT   debugging knobs
;;
;; Run:  emacs -Q --batch -l handoff/bench/bench.el </dev/null

(setq debug-on-error nil)
(defvar B-dir (file-name-directory (or load-file-name buffer-file-name)))
(defvar B-repo-dir (expand-file-name "../../" B-dir))
(defvar B-delay (string-to-number (or (getenv "DELAY") "0.1")))
(defvar B-host (or (getenv "BENCH_HOST") "localhost"))
(defvar B-prefix (or (getenv "BENCH_PREFIX") "/su:localhost:"))
(defvar B-repo (or (getenv "BENCH_REPO") "src/proj"))
(defvar B-oob (or (getenv "BENCH_OOB") "sh -c"))
(defvar B-root (concat B-prefix "~/" (file-name-as-directory B-repo)))

(dolist (d '("compat" "llama" "cond-let" "transient/lisp" "with-editor/lisp" "magit/lisp"))
  (add-to-list 'load-path (expand-file-name (concat "elpa/" d) B-dir)))
(add-to-list 'load-path (expand-file-name "emacs" B-repo-dir))
(require 'cl-lib)
(require 'tramp)
(setq tramp-verbose 0)
(require 'scrutiny-agent-remote)

(defun B-oob (command)
  "Run COMMAND in the benchmark repository on the remote, outside Emacs' agent."
  (with-temp-buffer
    (call-process-shell-command
     (concat B-oob " " (shell-quote-argument
                        (format "cd ~/%s && %s" B-repo command)))
     nil t)
    (buffer-string)))

(defvar B-rpc nil) (defvar B-tramp 0) (defvar B-deleg nil) (defvar B-times nil)
;; Round trips counted without reference to the link's latency: a wait
;; that blocks on an unanswered future, when something was sent since
;; the previous blocking wait, is a round trip.  Further blocking waits
;; with nothing sent in between are the same trip still landing.  The
;; gap-based `B-bursts' below is kept for comparison; on a fast link it
;; under-counts, since sequential requests arrive closer together than
;; the gap it looks for.
(defvar B-waits 0) (defvar B-sent-since-wait nil)
(advice-add 'scrutiny-agent-async-request :before
            (lambda (_c method &rest _) (push method B-rpc) (push (float-time) B-times)
              (setq B-sent-since-wait t)))
(advice-add 'scrutiny-agent-await :around
            (lambda (orig conn future &rest args)
              (when (and (not (aref future 0)) B-sent-since-wait)
                (cl-incf B-waits) (setq B-sent-since-wait nil))
              (apply orig conn future args)))
(advice-add 'scrutiny-agent-notify :before
            (lambda (_c method &rest _) (push (concat "~" method) B-rpc)))
(advice-add 'tramp-send-command :before (lambda (&rest _) (cl-incf B-tramp)))
(setq scrutiny-agent-magit-history-file nil) ; measure a session with no saved history
(advice-add 'scrutiny-agent-fs--delegate :before
            (lambda (op args)
              (when (or (and (stringp (car args)) (string-prefix-p B-prefix (car args)))
                        (and (memq op '(process-file start-file-process make-process shell-command exec-path))
                             (string-prefix-p B-prefix default-directory)))
                (push op B-deleg))))
(defun B-bursts ()
  (let ((ts (sort (copy-sequence B-times) #'<)) (last -1.0) sizes)
    (dolist (x ts)
      (if (> (- x last) (* 0.5 (max B-delay 0.01))) (push 1 sizes) (cl-incf (car sizes)))
      (setq last x))
    (nreverse sizes)))
(defun B-bytes ()
  (let ((f (getenv "RELAY_STATS")))
    (if (and f (file-exists-p f))
        (with-temp-buffer (insert-file-contents f) (mapcar #'string-to-number (split-string (buffer-string))))
      '(0 0))))
(defvar B-bytes0 nil)
(defun B-tally (xs)
  (let (al) (dolist (x xs) (cl-incf (alist-get x al 0 nil #'equal)))
       (sort al (lambda (a b) (> (cdr a) (cdr b))))))
(defmacro B-run (label &rest body)
  (declare (indent 1))
  `(progn
     (sleep-for 3)                      ; think time between commands
     (when (fboundp 'scrutiny-agent-magit--end-batch) (scrutiny-agent-magit--end-batch))
     (setq B-rpc nil B-tramp 0 B-deleg nil B-times nil B-bytes0 (B-bytes)
           B-waits 0 B-sent-since-wait nil)
     (let ((t0 (float-time)))
       (condition-case e (progn ,@body)
         (error (princ (format "  !! %s: %S\n" ,label e))
                (when (getenv "BT") (princ (with-output-to-string (backtrace))))))
       (let* ((dt (- (float-time) t0))
              (reqs (cl-remove-if (lambda (m) (string-prefix-p "~" m)) B-rpc)))
         (princ (format "%-50s %6.2fs  agent-rpc=%-3d round-trips=%-3d waits=%-3d tramp-cmds=%d\n"
                        ,label dt (length reqs) (length (B-bursts)) B-waits B-tramp))
         (when (getenv "RELAY_STATS") (sleep-for 0.3)
           (let ((b (B-bytes))) (princ (format "      wire: %d B up, %d B down\n" (- (car b) (car B-bytes0)) (- (cadr b) (cadr B-bytes0))))))
         (when B-rpc (princ (format "      rpc: %S\n      bursts: %S\n" (B-tally B-rpc) (B-bursts))))
         (when B-deleg (princ (format "      to-tramp: %S\n" (B-tally B-deleg))))))))

(scrutiny-agent-remote-setup
 `((,B-host
    :transport ,(or (getenv "BENCH_TRANSPORT")
                    (format "python3 %s %s sh" (expand-file-name "relay.py" B-dir) B-delay))
    :roots ,(split-string (or (getenv "BENCH_ROOTS") "src") "," t)
    ,@(when (getenv "BENCH_INSTALL_DIR") (list :install-dir (getenv "BENCH_INSTALL_DIR")))
    ,@(when (getenv "AGENT_BIN") (list :local-binary (expand-file-name (getenv "AGENT_BIN")))))))
(setq scrutiny-agent-fs-auto-connect t)
(when (getenv "NOCOMPRESS") (setq scrutiny-agent-compress nil))
(when (getenv "TTL") (setq scrutiny-agent-fs-cache-ttl (string-to-number (getenv "TTL"))))
(B-run "connect" (scrutiny-agent-connect B-host))
(when (getenv "SCENARIO") (load (expand-file-name (getenv "SCENARIO") B-dir) nil t))
(princ (format "delegations (whole run): %S\n" scrutiny-agent-fs-delegations))
;;; bench.el ends here
