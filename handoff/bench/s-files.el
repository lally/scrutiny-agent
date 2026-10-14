;;; s-files.el --- open / edit / save / dired / completion -*- lexical-binding: t; -*-
(defvar B-buf nil)
(B-run "find-file (5 deep, c++-mode)"
  (setq B-buf (find-file-noselect (concat B-root "gitmanip/src/status.cpp"))))
(B-run "first keystroke (modify)"
  (with-current-buffer B-buf (goto-char (point-min)) (insert "// x\n")))
(B-run "save-buffer #1"
  (with-current-buffer B-buf (save-buffer)))
(B-run "edit+save-buffer #2"
  (with-current-buffer B-buf (goto-char (point-min)) (insert "// y\n") (save-buffer)))
(B-run "find-file sibling"
  (find-file-noselect (concat B-root "gitmanip/src/diff.cpp")))
(B-run "find-file same dir again (python)"
  (find-file-noselect (concat B-root "tests/test_fs.py")))
(B-run "dired"
  (dired-noselect (concat B-root "gitmanip/src/")))
(B-run "completion C-x C-f in dir"
  (file-name-all-completions "st" (concat B-root "gitmanip/src/")))
(B-run "project-current + project-files"
  (let ((default-directory (concat B-root "gitmanip/src/")))
    (require 'project)
    (let ((pr (project-current)))
      (princ (format "      project=%S files=%d\n" (and pr (project-root pr))
                     (length (and pr (project-files pr))))))))
(B-run "revert-buffer"
  (with-current-buffer B-buf (revert-buffer t t)))
(with-current-buffer B-buf (goto-char (point-min)) (delete-region (point) (progn (forward-line 2) (point))) (save-buffer))
