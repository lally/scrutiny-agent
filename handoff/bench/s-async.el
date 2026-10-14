;;; s-async.el --- hunk staging and a commit through with-editor -*- lexical-binding: t; -*-
;; Restores the repository (reset --hard HEAD~1) only if its commit succeeded.
(require 'magit)
(defvar B-mbuf nil)
(B-oob "printf '\\n// bench hunk\\n' >> gitmanip/src/status.cpp")
(let ((default-directory B-root)) (setq B-mbuf (magit-status-setup-buffer default-directory)))
(with-current-buffer B-mbuf (magit-refresh))
(defvar B-patch (B-oob "git diff -- gitmanip/src/status.cpp"))
(B-run "stage a hunk (git apply --cached -)"
  (with-current-buffer B-mbuf
    (let ((magit-inhibit-refresh t))
      (with-temp-buffer (insert B-patch) (let ((default-directory B-root)) (magit-run-git-with-input "apply" "--cached" "-"))))))
(B-run "refresh after it"
  (with-current-buffer B-mbuf (magit-refresh)))
(B-run "unstage the hunk (git apply --reverse --cached -)"
  (let ((magit-inhibit-refresh t))
    (with-temp-buffer (insert B-patch) (let ((default-directory B-root)) (magit-run-git-with-input "apply" "--reverse" "--cached" "-")))))
(B-run "commit: start, until the message buffer opens"
  (let ((default-directory B-root) (magit-inhibit-refresh t))
    (with-temp-buffer (insert B-patch) (magit-run-git-with-input "apply" "--cached" "-"))
    (setq B-proc (magit-with-editor (magit-start-git nil "commit")))
    (let ((deadline (+ (float-time) 30)))
      (while (and (< (float-time) deadline)
                  (not (setq B-msg (cl-find-if (lambda (b) (string-suffix-p "COMMIT_EDITMSG" (or (buffer-file-name b) ""))) (buffer-list)))))
        (accept-process-output nil 0.02)))))
(defvar B-proc nil) (defvar B-msg nil)
(sleep-for 0.3)
(B-run "commit: C-c C-c until git has finished"
  (let ((make-backup-files nil) (magit-inhibit-refresh t))
    (with-current-buffer B-msg (goto-char (point-min)) (insert "bench commit\n") (with-editor-return nil))
    (let ((deadline (+ (float-time) 30)))
      (while (and (process-live-p B-proc) (< (float-time) deadline)) (accept-process-output nil 0.02)))
    (princ (format "      exit=%s\n" (process-exit-status B-proc)))))
(princ (format "      HEAD is now: %s" (B-oob "git log -1 --format=%s")))
(B-oob "git reset -q --hard HEAD~1")
