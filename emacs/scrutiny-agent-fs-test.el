;;; scrutiny-agent-fs-test.el --- Tests for scrutiny-agent-fs  -*- lexical-binding: t; -*-

;;; Commentary:
;; Comprehensive tests that catch real bugs, not just happy paths.
;; Run with: emacs --batch -l ~/.emacs -l scrutiny-agent-fs-test.el -f scrutiny-agent-fs-run-all-tests

;;; Code:

(require 'scrutiny-agent)
(require 'scrutiny-agent-fs)

;; Declare variables that company-mode would define, so we can test them
;; even if company isn't fully initialized in batch mode
(defvar company-candidates nil "Company completion candidates (for testing).")

(defvar scrutiny-agent-fs-test-results nil
  "Alist of (test-name . (passed . message)).")

(defvar scrutiny-agent-fs-test-host "artifact"
  "Host to use for live tests. Must be configured in `scrutiny-agent-hosts'.")

(defmacro scrutiny-agent-fs-deftest (name &rest body)
  "Define a test NAME with BODY. BODY should return (PASSED . MESSAGE)."
  (declare (indent 1))
  `(defun ,(intern (format "scrutiny-agent-fs-test--%s" name)) ()
     (condition-case err
         (let ((result (progn ,@body)))
           (push (cons ',name result) scrutiny-agent-fs-test-results)
           result)
       (error
        (let ((result (cons nil (format "ERROR: %S" err))))
          (push (cons ',name result) scrutiny-agent-fs-test-results)
          result)))))

;; ============================================================
;; Test: interactive-p detection
;; ============================================================

(scrutiny-agent-fs-deftest interactive-p-no-minibuffer
  "interactive-p should return nil when not in minibuffer and no completion active."
  ;; This tests the ACTUAL function, not a mock
  (let ((result (scrutiny-agent-fs--interactive-p)))
    (if (null result)
        (cons t "correctly returns nil outside minibuffer")
      (cons nil (format "returned %S, expected nil - check for bound variables like ivy--index" result)))))

(scrutiny-agent-fs-deftest interactive-p-with-ivy-loaded
  "interactive-p should not be fooled by ivy--index being 0."
  ;; Simulate ivy being loaded (as it would be in user's config)
  (let ((ivy--index 0))  ; ivy sets this to 0 when loaded
    (let ((result (scrutiny-agent-fs--interactive-p)))
      (if (null result)
          (cons t "correctly ignores ivy--index=0")
        (cons nil (format "returned %S when ivy--index=0 - this was a real bug!" result))))))

(scrutiny-agent-fs-deftest interactive-p-with-company-candidates
  "interactive-p should detect active company completion."
  (let ((company-candidates '("foo" "bar")))
    (let ((result (scrutiny-agent-fs--interactive-p)))
      (if result
          (cons t "correctly detects company-candidates")
        (cons nil "failed to detect company-candidates")))))

;; ============================================================
;; Test: cache key normalization
;; ============================================================

(scrutiny-agent-fs-deftest cache-key-trailing-dot
  "Cache key should normalize /foo/bar/. to same as /foo/bar"
  (let ((key1 (scrutiny-agent-fs--cache-key "host" "/foo/bar"))
        (key2 (scrutiny-agent-fs--cache-key "host" "/foo/bar/.")))
    (if (equal key1 key2)
        (cons t "correctly normalizes trailing /.")
      (cons nil (format "keys differ: %S vs %S - this was a real bug!" key1 key2)))))

(scrutiny-agent-fs-deftest cache-key-trailing-slash
  "Cache key should normalize /foo/bar/ to same as /foo/bar"
  (let ((key1 (scrutiny-agent-fs--cache-key "host" "/foo/bar"))
        (key2 (scrutiny-agent-fs--cache-key "host" "/foo/bar/")))
    (if (equal key1 key2)
        (cons t "correctly normalizes trailing /")
      (cons nil (format "keys differ: %S vs %S" key1 key2)))))

(scrutiny-agent-fs-deftest cache-key-double-slash
  "Cache key should normalize //foo//bar to /foo/bar"
  (let ((key1 (scrutiny-agent-fs--cache-key "host" "/foo/bar"))
        (key2 (scrutiny-agent-fs--cache-key "host" "//foo//bar")))
    (if (equal key1 key2)
        (cons t "correctly normalizes double slashes")
      (cons nil (format "keys differ: %S vs %S" key1 key2)))))

(scrutiny-agent-fs-deftest cache-key-dot-dot
  "Cache key should normalize /foo/bar/../baz to /foo/baz"
  (let ((key1 (scrutiny-agent-fs--cache-key "host" "/foo/baz"))
        (key2 (scrutiny-agent-fs--cache-key "host" "/foo/bar/../baz")))
    (if (equal key1 key2)
        (cons t "correctly normalizes ..")
      (cons nil (format "keys differ: %S vs %S" key1 key2)))))

;; ============================================================
;; Test: no unexpected TRAMP delegation
;; ============================================================

(defvar scrutiny-agent-fs--test-delegation-log nil
  "Log of delegations during test.")

(defun scrutiny-agent-fs--track-delegations (body-fn)
  "Run BODY-FN while tracking all delegations to TRAMP."
  (setq scrutiny-agent-fs--test-delegation-log nil)
  (let ((orig-delegate (symbol-function 'scrutiny-agent-fs--delegate)))
    (cl-letf (((symbol-function 'scrutiny-agent-fs--delegate)
               (lambda (operation args)
                 (when (and (car args) (stringp (car args))
                            (string-match-p "artifact" (car args)))
                   (push (cons operation (car args))
                         scrutiny-agent-fs--test-delegation-log))
                 (funcall orig-delegate operation args))))
      (funcall body-fn))))

(scrutiny-agent-fs-deftest no-delegation-directory-files
  "directory-files should not delegate to TRAMP when listing fails."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (scrutiny-agent-fs--track-delegations
   (lambda ()
     ;; Access a directory that's not cached
     (let ((result (directory-files
                    (format "/ssh:%s:/data/src/scrutiny-agent/emacs/"
                            scrutiny-agent-fs-test-host))))
       (let ((delegations (cl-remove-if-not
                           (lambda (d) (eq (car d) 'directory-files))
                           scrutiny-agent-fs--test-delegation-log)))
         (if delegations
             (cons nil (format "delegated directory-files: %S" delegations))
           (cons t (format "no delegation, got %d entries" (length result)))))))))

(scrutiny-agent-fs-deftest no-delegation-insert-directory
  "insert-directory should not delegate to TRAMP when listing unavailable."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (scrutiny-agent-fs--track-delegations
   (lambda ()
     (with-temp-buffer
       (insert-directory
        (format "/ssh:%s:/data/src/scrutiny-agent/" scrutiny-agent-fs-test-host)
        "-la"))
     (let ((delegations (cl-remove-if-not
                         (lambda (d) (eq (car d) 'insert-directory))
                         scrutiny-agent-fs--test-delegation-log)))
       (if delegations
           (cons nil (format "delegated insert-directory: %S" delegations))
         (cons t "no delegation"))))))

(scrutiny-agent-fs-deftest no-delegation-insert-file-contents
  "insert-file-contents should not delegate to TRAMP on failure."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (scrutiny-agent-fs--track-delegations
   (lambda ()
     (condition-case nil
         (with-temp-buffer
           (insert-file-contents
            (format "/ssh:%s:/data/src/scrutiny-agent/README.md"
                    scrutiny-agent-fs-test-host))
           (let ((delegations (cl-remove-if-not
                               (lambda (d) (eq (car d) 'insert-file-contents))
                               scrutiny-agent-fs--test-delegation-log)))
             (if delegations
                 (cons nil (format "delegated: %S" delegations))
               (cons t (format "no delegation, read %d bytes" (buffer-size))))))
       (error
        ;; Error is OK, delegation is not
        (let ((delegations (cl-remove-if-not
                            (lambda (d) (eq (car d) 'insert-file-contents))
                            scrutiny-agent-fs--test-delegation-log)))
          (if delegations
              (cons nil (format "delegated on error: %S" delegations))
            (cons t "error without delegation (acceptable)"))))))))

(scrutiny-agent-fs-deftest no-delegation-file-local-copy
  "file-local-copy should not delegate to TRAMP on failure."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (scrutiny-agent-fs--track-delegations
   (lambda ()
     (let ((result (scrutiny-agent-fs--handle-file-local-copy
                    (format "/ssh:%s:/data/src/scrutiny-agent/README.md"
                            scrutiny-agent-fs-test-host))))
       (let ((delegations (cl-remove-if-not
                           (lambda (d) (eq (car d) 'file-local-copy))
                           scrutiny-agent-fs--test-delegation-log)))
         (when result (delete-file result))  ; cleanup
         (if delegations
             (cons nil (format "delegated: %S" delegations))
           (if result
               (cons t "no delegation, got local copy")
             (cons nil "no delegation but also no result"))))))))

;; ============================================================
;; Test: full workflow simulation
;; ============================================================

(scrutiny-agent-fs-deftest workflow-navigate-and-open
  "Simulate: navigate to dir, select '.', open file."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (let* ((dir (format "/ssh:%s:/data/src/scrutiny-agent/" scrutiny-agent-fs-test-host))
         (dir-with-dot (concat dir "."))
         (problems nil))

    ;; Step 1: Complete in directory (interactive)
    (let ((completions nil))
      ;; Simulate minibuffer active
      (cl-letf (((symbol-function 'minibuffer-window-active-p) (lambda (_) t)))
        (setq completions (file-name-all-completions "" dir)))
      (unless completions
        (push "step 1: no completions in interactive mode" problems)))

    ;; Wait for background fetch
    (sit-for 0.5)

    ;; Step 2: Check directory with trailing dot (what RET on '.' produces)
    (let ((is-dir (file-directory-p dir-with-dot)))
      (unless is-dir
        (push (format "step 2: %s not recognized as directory" dir-with-dot) problems)))

    ;; Step 3: insert-directory with trailing dot (dired does this)
    (scrutiny-agent-fs--track-delegations
     (lambda ()
       (with-temp-buffer
         (condition-case err
             (insert-directory dir-with-dot "-la")
           (error (push (format "step 3: insert-directory error: %S" err) problems))))
       (let ((delegations scrutiny-agent-fs--test-delegation-log))
         (when delegations
           (push (format "step 3: unexpected delegations: %S" delegations) problems)))))

    (if problems
        (cons nil (string-join (nreverse problems) "; "))
      (cons t "workflow completed without issues"))))

;; ============================================================
;; Test: configuration completeness
;; ============================================================

(scrutiny-agent-fs-deftest config-eglot-setup-called
  "Verify scrutiny-agent-eglot-setup was called if eglot is loaded."
  (if (not (featurep 'eglot))
      (cons 'skip "eglot not loaded")
    (if (not (featurep 'scrutiny-agent-eglot))
        (cons nil "scrutiny-agent-eglot not loaded - add (require 'scrutiny-agent-eglot)")
      ;; Check if setup was called by looking at eglot-server-programs
      (let ((has-agent-contact nil))
        (dolist (entry eglot-server-programs)
          (let ((contact (cdr entry)))
            (when (and (functionp contact)
                       ;; The contact function from scrutiny-agent-eglot-contact
                       ;; returns a lambda, check if any entry has one
                       (condition-case nil
                           (let ((result (funcall contact nil nil)))
                             (and (listp result) (eq (car result) 'eglot-lsp-server)))
                         (error nil)))
              (setq has-agent-contact t))))
        (if has-agent-contact
            (cons t "scrutiny-agent-eglot-setup was called")
          (cons nil "scrutiny-agent-eglot-setup not called - add (scrutiny-agent-eglot-setup) after require"))))))

(scrutiny-agent-fs-deftest config-fs-mode-enabled
  "Verify scrutiny-agent-fs-mode is enabled."
  (if scrutiny-agent-fs-mode
      (cons t "fs-mode is enabled")
    (cons nil "fs-mode not enabled - add (scrutiny-agent-fs-mode 1)")))

(scrutiny-agent-fs-deftest config-hosts-configured
  "Verify at least one host is configured."
  (if scrutiny-agent-hosts
      (cons t (format "%d hosts configured" (length scrutiny-agent-hosts)))
    (cons nil "no hosts configured")))

;; ============================================================
;; Test: edge cases in path handling
;; ============================================================

(scrutiny-agent-fs-deftest cache-key-root-directory
  "Cache key should handle root directory correctly."
  (let ((key1 (scrutiny-agent-fs--cache-key "host" "/"))
        (key2 (scrutiny-agent-fs--cache-key "host" "/.")))
    (if (equal key1 key2)
        (cons t "root and /. normalize to same key")
      (cons nil (format "keys differ: %S vs %S" key1 key2)))))

(scrutiny-agent-fs-deftest cache-key-empty-path
  "Cache key should handle edge cases without crashing."
  (condition-case err
      (let ((key (scrutiny-agent-fs--cache-key "host" "")))
        (cons t (format "empty path gives key: %S" key)))
    (error
     (cons nil (format "crashed on empty path: %S" err)))))

(scrutiny-agent-fs-deftest path-with-spaces
  "Paths with spaces should work correctly."
  (let ((key1 (scrutiny-agent-fs--cache-key "host" "/path with spaces"))
        (key2 (scrutiny-agent-fs--cache-key "host" "/path with spaces/")))
    (if (equal key1 key2)
        (cons t "path with spaces normalizes correctly")
      (cons nil (format "keys differ: %S vs %S" key1 key2)))))

;; ============================================================
;; Test: file existence and attribute consistency
;; ============================================================

(scrutiny-agent-fs-deftest file-exists-directory-consistency
  "file-exists-p and file-directory-p should be consistent."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (let* ((path (format "/ssh:%s:/data/src/scrutiny-agent/" scrutiny-agent-fs-test-host))
         (exists (file-exists-p path))
         (is-dir (file-directory-p path)))
    (cond
     ((and exists is-dir)
      (cons t "directory exists and is-dir both true"))
     ((and (not exists) (not is-dir))
      (cons nil "directory should exist but file-exists-p returned nil"))
     (exists
      (cons nil "exists but not a directory?"))
     (t
      (cons nil "is-dir but doesn't exist?")))))

(scrutiny-agent-fs-deftest file-attributes-match-listing
  "file-attributes should match data from directory-files-and-attributes."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (let* ((dir (format "/ssh:%s:/data/src/scrutiny-agent/" scrutiny-agent-fs-test-host))
         (listing (directory-files-and-attributes dir nil nil t))
         (problems nil))
    ;; Check first few entries
    (cl-loop for (name . attrs) in (seq-take listing 3)
             for full-path = (concat dir name)
             for direct-attrs = (file-attributes full-path)
             do (unless (or (null attrs) (null direct-attrs)
                            ;; Compare file type (first element)
                            (equal (car attrs) (car direct-attrs)))
                  (push (format "%s: listing=%S vs direct=%S"
                                name (car attrs) (car direct-attrs))
                        problems)))
    (if problems
        (cons nil (format "mismatches: %S" problems))
      (cons t "attributes consistent"))))

;; ============================================================
;; Test: symlink handling
;; ============================================================

(scrutiny-agent-fs-deftest symlink-detection
  "file-symlink-p should return link target for symlinks."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  ;; Try to find a symlink in a standard location
  (let* ((test-paths '("/usr/bin" "/etc"))
         (found-link nil))
    (catch 'done
      (dolist (base-path test-paths)
        (let ((path (format "/ssh:%s:%s" scrutiny-agent-fs-test-host base-path)))
          (condition-case nil
              (let ((entries (directory-files-and-attributes path t)))
                (dolist (entry entries)
                  (when (stringp (car (cdr entry)))  ; symlink has string target in attrs
                    (setq found-link (car entry))
                    (throw 'done nil))))
            (error nil)))))
    (if found-link
        (let ((link-target (file-symlink-p found-link)))
          (if link-target
              (cons t (format "detected symlink: %s -> %s"
                              (file-name-nondirectory found-link) link-target))
            (cons nil (format "file-symlink-p returned nil for %s" found-link))))
      (cons 'skip "no symlinks found to test"))))

;; ============================================================
;; Test: non-existent paths
;; ============================================================

(scrutiny-agent-fs-deftest nonexistent-file-exists-p
  "file-exists-p should return nil for non-existent files without delegating."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (scrutiny-agent-fs--track-delegations
   (lambda ()
     (let* ((path (format "/ssh:%s:/nonexistent/path/that/does/not/exist"
                          scrutiny-agent-fs-test-host))
            (result (file-exists-p path))
            (delegations scrutiny-agent-fs--test-delegation-log))
       (cond
        (result
         (cons nil "file-exists-p returned t for non-existent path"))
        (delegations
         (cons nil (format "delegated: %S" delegations)))
        (t
         (cons t "correctly returned nil without delegation")))))))

(scrutiny-agent-fs-deftest nonexistent-directory-completions
  "file-name-all-completions should return nil for non-existent dirs without delegating."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (scrutiny-agent-fs--track-delegations
   (lambda ()
     (let* ((path (format "/ssh:%s:/nonexistent/directory/"
                          scrutiny-agent-fs-test-host))
            (result (file-name-all-completions "" path))
            (delegations scrutiny-agent-fs--test-delegation-log))
       (cond
        (result
         (cons nil (format "got completions for non-existent dir: %S" result)))
        (delegations
         (cons nil (format "delegated: %S" delegations)))
        (t
         (cons t "correctly returned nil without delegation")))))))

;; ============================================================
;; Test: handler registration
;; ============================================================

(scrutiny-agent-fs-deftest handler-registered-in-alist
  "scrutiny-agent-fs handler should be in file-name-handler-alist when mode enabled."
  (if (not scrutiny-agent-fs-mode)
      (cons 'skip "fs-mode not enabled")
    (let ((entry (rassq 'scrutiny-agent-fs-handler file-name-handler-alist)))
      (if entry
          (cons t (format "handler registered with pattern: %S" (car entry)))
        (cons nil "handler not found in file-name-handler-alist")))))

(scrutiny-agent-fs-deftest handler-priority
  "scrutiny-agent-fs handler should be early in file-name-handler-alist."
  (if (not scrutiny-agent-fs-mode)
      (cons 'skip "fs-mode not enabled")
    (let ((pos 0)
          (found nil))
      (dolist (entry file-name-handler-alist)
        (when (eq (cdr entry) 'scrutiny-agent-fs-handler)
          (setq found pos))
        (cl-incf pos))
      (if (not found)
          (cons nil "handler not found")
        (if (< found 3)
            (cons t (format "handler at position %d (good priority)" found))
          (cons nil (format "handler at position %d (may be too late)" found)))))))

;; ============================================================
;; Test: operations list completeness
;; ============================================================

(scrutiny-agent-fs-deftest operations-include-core-ops
  "scrutiny-agent-fs-operations should include all critical file operations."
  (let ((required-ops '(file-exists-p
                        file-directory-p
                        file-attributes
                        file-name-all-completions
                        directory-files
                        directory-files-and-attributes
                        insert-file-contents
                        insert-directory
                        file-local-copy))
        (missing nil))
    (dolist (op required-ops)
      (unless (assq op scrutiny-agent-fs-operations)
        (push op missing)))
    (if missing
        (cons nil (format "missing operations: %S" missing))
      (cons t "all core operations present"))))

;; ============================================================
;; Test: file content handling
;; ============================================================

(scrutiny-agent-fs-deftest read-empty-file
  "Reading an empty file should work correctly."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  ;; /dev/null is always empty
  (let ((path (format "/ssh:%s:/dev/null" scrutiny-agent-fs-test-host)))
    (condition-case err
        (with-temp-buffer
          (insert-file-contents path)
          (if (zerop (buffer-size))
              (cons t "empty file read correctly")
            (cons nil (format "got %d bytes from /dev/null" (buffer-size)))))
      (error
       (cons nil (format "error reading empty file: %S" err))))))

(scrutiny-agent-fs-deftest file-readable-p-consistency
  "file-readable-p should match what insert-file-contents does."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (let ((path (format "/ssh:%s:/data/src/scrutiny-agent/README.md"
                      scrutiny-agent-fs-test-host)))
    (let ((readable (file-readable-p path)))
      (if readable
          (condition-case nil
              (progn
                (with-temp-buffer
                  (insert-file-contents path nil nil 100))
                (cons t "file-readable-p=t and read succeeded"))
            (error
             (cons nil "file-readable-p=t but read failed")))
        (cons nil "file-readable-p returned nil for readable file")))))

;; ============================================================
;; Test: hidden files and special entries
;; ============================================================

(scrutiny-agent-fs-deftest directory-listing-includes-dotfiles
  "directory-files should include hidden (dot) files."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  ;; Use scrutiny-agent directory which has dotfiles like .git
  (let* ((path (format "/ssh:%s:/data/src/scrutiny-agent/" scrutiny-agent-fs-test-host))
         (files (directory-files path)))
    (if (null files)
        (cons nil "no files returned")
      (let ((has-dotfile (cl-some (lambda (f) (and (string-prefix-p "." f)
                                                    (not (member f '("." "..")))))
                                  files)))
        (if has-dotfile
            (cons t (format "found dotfiles among %d entries" (length files)))
          (cons nil (format "%d entries but no dotfiles - check if .git exists" (length files))))))))

(scrutiny-agent-fs-deftest completions-include-dot-entries
  "file-name-all-completions should include . and .. entries."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (let* ((path (format "/ssh:%s:/data/src/" scrutiny-agent-fs-test-host))
         (completions (file-name-all-completions "" path)))
    (if (null completions)
        (cons nil "no completions returned")
      (let ((has-dot (member "./" completions))
            (has-dotdot (member "../" completions)))
        (cond
         ((and has-dot has-dotdot)
          (cons t (format "found ./ and ../ among %d entries" (length completions))))
         ((not has-dot)
          (cons nil "missing ./ entry"))
         ((not has-dotdot)
          (cons nil "missing ../ entry"))
         (t (cons nil "unexpected state")))))))

;; ============================================================
;; Test: file attribute edge cases
;; ============================================================

(scrutiny-agent-fs-deftest file-modes-format
  "file-modes should return an integer, not a string."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (let* ((path (format "/ssh:%s:/data/src/scrutiny-agent/README.md"
                       scrutiny-agent-fs-test-host))
         (modes (file-modes path)))
    (cond
     ((null modes)
      (cons nil "file-modes returned nil"))
     ((not (integerp modes))
      (cons nil (format "file-modes returned %S (not integer)" modes)))
     (t
      (cons t (format "file-modes=%o" modes))))))

(scrutiny-agent-fs-deftest file-size-accuracy
  "File size from attributes should match actual content length."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (let* ((path (format "/ssh:%s:/data/src/scrutiny-agent/README.md"
                       scrutiny-agent-fs-test-host))
         (attrs (file-attributes path))
         (reported-size (nth 7 attrs)))
    (if (null attrs)
        (cons nil "file-attributes returned nil")
      (with-temp-buffer
        (let ((coding-system-for-read 'binary))
          (insert-file-contents path))
        (let ((actual-size (buffer-size)))
          (if (= reported-size actual-size)
              (cons t (format "size matches: %d bytes" actual-size))
            (cons nil (format "size mismatch: attrs=%d actual=%d"
                              reported-size actual-size))))))))

;; ============================================================
;; Test: cache behavior
;; ============================================================

(scrutiny-agent-fs-deftest cache-populated-after-listing
  "After directory-files, cache should be populated for that directory."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  ;; Clear cache first
  (scrutiny-agent-fs-flush-cache)
  (let* ((dir "/data/src/scrutiny-agent/")
         (path (format "/ssh:%s:%s" scrutiny-agent-fs-test-host dir))
         (key (scrutiny-agent-fs--cache-key scrutiny-agent-fs-test-host
                                            (directory-file-name dir))))
    ;; Do a listing
    (directory-files path)
    ;; Check cache
    (let ((cached (gethash key scrutiny-agent-fs--dir-cache)))
      (if cached
          (cons t (format "cache populated with %d entries"
                          (length (cdr cached))))
        (cons nil "cache not populated after directory-files")))))

;; ============================================================
;; Test: full find-file simulation
;; ============================================================

(scrutiny-agent-fs-deftest find-file-no-delegation
  "Simulated find-file should not delegate to TRAMP."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (let ((path (format "/ssh:%s:/data/src/scrutiny-agent/README.md"
                      scrutiny-agent-fs-test-host)))
    (scrutiny-agent-fs--track-delegations
     (lambda ()
       ;; Simulate what find-file does
       (let* ((expanded (expand-file-name path))
              (truename (file-truename expanded))
              (exists (file-exists-p expanded))
              (readable (file-readable-p expanded))
              (attrs (file-attributes expanded))
              (modes (file-modes expanded)))
         ;; Now try to read the file
         (with-temp-buffer
           (insert-file-contents expanded))
         ;; Check for any delegations
         (let ((delegations scrutiny-agent-fs--test-delegation-log))
           (if delegations
               (cons nil (format "delegated during find-file: %S" delegations))
             (cons t "no delegation during find-file simulation"))))))))

(scrutiny-agent-fs-deftest file-truename-no-delegation
  "file-truename should not delegate for regular files."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (let ((path (format "/ssh:%s:/data/src/scrutiny-agent/README.md"
                      scrutiny-agent-fs-test-host)))
    (scrutiny-agent-fs--track-delegations
     (lambda ()
       (let ((truename (file-truename path)))
         (let ((delegations (cl-remove-if-not
                             (lambda (d) (eq (car d) 'file-truename))
                             scrutiny-agent-fs--test-delegation-log)))
           (if delegations
               (cons nil (format "delegated file-truename: %S" delegations))
             (cons t (format "no delegation, truename=%s" truename)))))))))

(scrutiny-agent-fs-deftest expand-file-name-tilde
  "expand-file-name should handle ~ without delegating."
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (cons 'skip "not connected"))
  (let ((path (format "/ssh:%s:~" scrutiny-agent-fs-test-host)))
    (scrutiny-agent-fs--track-delegations
     (lambda ()
       (let ((expanded (expand-file-name path)))
         (let ((delegations (cl-remove-if-not
                             (lambda (d) (eq (car d) 'expand-file-name))
                             scrutiny-agent-fs--test-delegation-log)))
           (cond
            (delegations
             (cons nil (format "delegated expand-file-name: %S" delegations)))
            ((string-match-p "~" expanded)
             (cons nil (format "~ not expanded: %s" expanded)))
            (t
             (cons t (format "expanded to: %s" expanded))))))))))

;; NOTE: This test is disabled because write operations hang in batch mode.
;; Run interactively with M-x scrutiny-agent-fs-test--cache-invalidated-after-write
;; (scrutiny-agent-fs-deftest cache-invalidated-after-write
;;   "After writing a file, parent directory cache should be invalidated."
;;   (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
;;     (cons 'skip "not connected"))
;;   (unless (scrutiny-agent-fs--writable-p scrutiny-agent-fs-test-host)
;;     (cons 'skip "host not writable"))
;;   (let* ((dir "/tmp/")
;;          (filename (format "test-%d.txt" (random 100000)))
;;          (path (format "/ssh:%s:%s%s" scrutiny-agent-fs-test-host dir filename))
;;          (key (scrutiny-agent-fs--cache-key scrutiny-agent-fs-test-host
;;                                             (directory-file-name dir))))
;;     ;; Populate cache
;;     (directory-files (format "/ssh:%s:%s" scrutiny-agent-fs-test-host dir))
;;     (let ((cached-before (gethash key scrutiny-agent-fs--dir-cache)))
;;       ;; Write a file
;;       (with-temp-buffer
;;         (insert "test")
;;         (write-region (point-min) (point-max) path))
;;       ;; Check cache is gone
;;       (let ((cached-after (gethash key scrutiny-agent-fs--dir-cache)))
;;         ;; Cleanup
;;         (delete-file path)
;;         (cond
;;          ((null cached-before)
;;           (cons nil "cache wasn't populated before write"))
;;          (cached-after
;;           (cons nil "cache not invalidated after write"))
;;          (t
;;           (cons t "cache correctly invalidated after write")))))))

;; ============================================================
;; Test: audit all handlers for delegation
;; ============================================================

(defun scrutiny-agent-fs--audit-handlers ()
  "Audit every handler in scrutiny-agent-fs-operations for TRAMP delegation.
Returns list of (operation . potential-issue) for operations that might delegate.

Only flags READ operations that require network access. Pure path manipulation
operations (like `expand-file-name') are OK to delegate unconditionally.
Write operations (like `write-region', `copy-file') are user-initiated and
blocking is acceptable for those."
  (let ((issues nil)
        ;; Operations that don't require network - just path string manipulation
        (path-only-ops '(expand-file-name
                         file-name-directory
                         file-name-nondirectory
                         file-name-as-directory
                         directory-file-name
                         file-remote-p
                         abbreviate-file-name
                         substitute-in-file-name
                         file-name-sans-versions))
        ;; Write operations - user-initiated, blocking is acceptable
        ;; These don't happen during minibuffer completion
        (write-ops '(write-region
                     copy-file
                     rename-file
                     delete-file
                     delete-directory
                     make-directory
                     make-symbolic-link
                     add-name-to-file
                     set-file-modes
                     set-file-times
                     set-visited-file-modtime))
        ;; Operations that only delegate in edge cases during user-initiated actions
        ;; Not relevant for interactive completion:
        ;; - insert-directory: only delegates for wildcards (dired with globs)
        ;; - verify-visited-file-modtime: only for unrecognized files
        ;; - file-local-copy: only for files too large for inline transfer
        ;; - file-truename: only when stat returns no path
        (edge-case-ops '(insert-directory
                         verify-visited-file-modtime
                         file-local-copy
                         file-truename)))
    (dolist (entry scrutiny-agent-fs-operations)
      (let* ((op (car entry))
             (handler (cdr entry))
             (handler-source (when (fboundp handler)
                               (prin1-to-string (symbol-function handler)))))
        (when handler-source
          ;; Check if handler calls scrutiny-agent-fs--delegate
          (when (string-match-p "scrutiny-agent-fs--delegate" handler-source)
            ;; Skip path-only, write, and edge-case operations
            (unless (or (memq op path-only-ops)
                        (memq op write-ops)
                        (memq op edge-case-ops))
              ;; Check if it's conditional on interactive-p
              (unless (string-match-p "scrutiny-agent-fs--interactive-p" handler-source)
                (push (cons op "delegates without checking interactive-p") issues)))))))
    issues))

(scrutiny-agent-fs-deftest audit-handlers-delegation
  "Audit all handlers to find potential TRAMP delegation issues."
  (let ((issues (scrutiny-agent-fs--audit-handlers)))
    (if issues
        (cons nil (format "potential issues: %S" issues))
      (cons t "all handlers look safe"))))

;; ============================================================
;; Test runner
;; ============================================================

(defun scrutiny-agent-fs-run-all-tests ()
  "Run all scrutiny-agent-fs tests and report results."
  (interactive)
  (setq scrutiny-agent-fs-test-results nil)

  ;; Ensure we're connected for live tests
  (unless (scrutiny-agent-connection scrutiny-agent-fs-test-host)
    (condition-case err
        (progn
          (message "Connecting to %s for tests..." scrutiny-agent-fs-test-host)
          (scrutiny-agent-connect scrutiny-agent-fs-test-host)
          (sit-for 2))
      (error (message "Could not connect: %S" err))))

  ;; Collect all test functions
  (let ((tests nil))
    (mapatoms (lambda (sym)
                (when (and (fboundp sym)
                           (string-prefix-p "scrutiny-agent-fs-test--"
                                            (symbol-name sym)))
                  (push sym tests))))

    ;; Run each test
    (dolist (test (sort tests (lambda (a b)
                                (string< (symbol-name a) (symbol-name b)))))
      (message "Running %s..." (symbol-name test))
      (funcall test))

    ;; Report results
    (let ((passed 0) (failed 0) (skipped 0))
      (message "\n========== TEST RESULTS ==========")
      (dolist (result (nreverse scrutiny-agent-fs-test-results))
        (let ((name (car result))
              (status (cadr result))
              (msg (cddr result)))
          (cond
           ((eq status 'skip)
            (cl-incf skipped)
            (message "SKIP: %s - %s" name msg))
           (status
            (cl-incf passed)
            (message "PASS: %s - %s" name msg))
           (t
            (cl-incf failed)
            (message "FAIL: %s - %s" name msg)))))
      (message "==================================")
      (message "Passed: %d, Failed: %d, Skipped: %d" passed failed skipped)

      ;; Return non-zero exit for CI
      (when (> failed 0)
        (kill-emacs 1)))))

(provide 'scrutiny-agent-fs-test)
;;; scrutiny-agent-fs-test.el ends here
