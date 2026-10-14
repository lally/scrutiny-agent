;;; s-magit.el --- status / refresh / stage / log / diff -*- lexical-binding: t; -*-
(require 'magit)
(setq magit-refresh-verbose nil)
(defvar B-mbuf nil)
(B-oob "printf '\\n// bench\\n' >> gitmanip/src/status.cpp")
(B-run "magit-status (cold)"
  (let ((default-directory B-root)) (setq B-mbuf (magit-status-setup-buffer default-directory))))
(B-run "magit-refresh (g)"
  (with-current-buffer B-mbuf (magit-refresh)))
(B-run "magit-refresh (g) again"
  (with-current-buffer B-mbuf (magit-refresh)))
(B-run "magit stage file + refresh"
  (with-current-buffer B-mbuf (magit-stage-files '("gitmanip/src/status.cpp"))))
(B-run "magit unstage + refresh"
  (with-current-buffer B-mbuf (magit-unstage-files '("gitmanip/src/status.cpp"))))
(B-run "magit-log (256 commits)"
  (with-current-buffer B-mbuf (magit-log-setup-buffer '("HEAD") '("-n256") nil)))
(B-run "magit-log again"
  (with-current-buffer B-mbuf (magit-log-setup-buffer '("HEAD") '("-n256") nil)))
(B-run "magit-diff-unstaged"
  (with-current-buffer B-mbuf (magit-diff-unstaged)))
(B-run "magit-diff-unstaged again"
  (with-current-buffer B-mbuf (magit-diff-unstaged)))
(B-run "magit-status in a fresh buffer"
  (progn (kill-buffer B-mbuf) (let ((default-directory B-root)) (setq B-mbuf (magit-status-setup-buffer default-directory)))))
(B-oob "git checkout -- gitmanip/src/status.cpp")
