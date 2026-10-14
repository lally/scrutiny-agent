;;; scrutiny-agent-eglot-tests.el --- ERT tests for eglot over the tunnel  -*- lexical-binding: t; -*-

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; End to end: a real eglot, a real language server (pylsp) started by
;; the agent next to the code, and the whole path between them -- the
;; loopback bridge, the tunnel, compression of what the server says.
;; Skipped unless an agent binary is named and pylsp is installed.
;;
;; Run:  SCRUTINY_AGENT_BIN=build/agent/scrutiny-agent \
;;         emacs -Q --batch -L emacs -l emacs/scrutiny-agent-eglot-tests.el \
;;         -f ert-run-tests-batch-and-exit

;;; Code:

(require 'ert)
(require 'cl-lib)
(require 'python)
(require 'scrutiny-agent)
(require 'scrutiny-agent-fs)
(require 'scrutiny-agent-eglot)

(defmacro scrutiny-agent-eglot-tests--with-server (varlist &rest body)
  "Bind (BUFFER SERVER ROOT) to a remote Python buffer managed by eglot."
  (declare (indent 1) (debug t))
  (let ((buffer (nth 0 varlist)) (server (nth 1 varlist))
        (root (nth 2 varlist)))
    `(let* ((bin (getenv "SCRUTINY_AGENT_BIN"))
            (install (make-temp-file "scra-eglot-install" t))
            (,root (file-truename (make-temp-file "scra-eglot-root" t)))
            (scrutiny-agent-default-config nil)
            (scrutiny-agent-hosts
             `(("eglottest" :transport "sh"
                :install-dir ,install
                :local-binary ,bin
                :agent-args ("--allow-root" ,,root "--allow-write"))))
            (eglot-server-programs eglot-server-programs)
            (eglot-autoshutdown nil)
            ;; Wait for `initialize' to be answered.  The default (3 s)
            ;; lets `eglot--connect' return with the handshake still in
            ;; flight when the server is slow to start -- pylsp through
            ;; the agent takes longer than that on a loaded machine --
            ;; and a shutdown sent then leaves eglot's `initialize'
            ;; continuation to be failed with "Server died" from a
            ;; sentinel, which in batch ends Emacs with status 255.
            (eglot-sync-connect t)
            (,buffer nil)
            (,server nil))
       (with-temp-file (expand-file-name "calc.py" ,root)
         (insert "def add(first, second):\n"
                 "    \"\"\"Return the sum of FIRST and SECOND.\"\"\"\n"
                 "    return first + second\n\n\n"
                 "class Calculator:\n"
                 "    def double(self, value):\n"
                 "        return add(value, value)\n\n\n"
                 "result = add(1, 2)\n"))
       (scrutiny-agent-connect "eglottest")
       (scrutiny-agent-fs-mode 1)
       (scrutiny-agent-eglot-setup)
       (unwind-protect
           (progn
             (setq ,buffer (find-file-noselect
                            (format "/ssh:eglottest:%s/calc.py" ,root)))
             (with-current-buffer ,buffer
               (unless (derived-mode-p 'python-mode 'python-ts-mode)
                 (python-mode))
               (setq ,server (apply #'eglot--connect (eglot--guess-contact)))
               ,@body))
         (when ,server (ignore-errors (eglot-shutdown ,server nil 20)))
         (when ,buffer
           (with-current-buffer ,buffer (set-buffer-modified-p nil))
           (kill-buffer ,buffer))
         (scrutiny-agent-fs-mode -1)
         (scrutiny-agent-disconnect "eglottest")
         (delete-directory install t)
         (delete-directory ,root t)))))

(defmacro scrutiny-agent-eglot-tests--deftest (name &rest body)
  (declare (indent 1) (debug t))
  `(ert-deftest ,name ()
     :tags '(integration)
     (skip-unless (getenv "SCRUTINY_AGENT_BIN"))
     (skip-unless (executable-find "pylsp"))
     ,@body))

(scrutiny-agent-eglot-tests--deftest scrutiny-agent-eglot-starts-the-server-remotely
  (scrutiny-agent-eglot-tests--with-server (buffer server root)
    (ignore buffer root)
    ;; `eglot--connect' returns only once `initialize' has been
    ;; answered, so a live server here is one that completed the
    ;; handshake through the tunnel.
    (should (jsonrpc-running-p server))
    ;; ...and it is running on the host, not as a local process: what
    ;; eglot holds is our end of the bridge.
    (should (eq (process-type (jsonrpc--process server)) 'network))))

(scrutiny-agent-eglot-tests--deftest scrutiny-agent-eglot-answers-about-the-code
  (scrutiny-agent-eglot-tests--with-server (buffer server root)
    (ignore buffer)
    ;; The document is known to the server by its path ON THE HOST.
    (should (equal (plist-get (eglot--TextDocumentIdentifier) :uri)
                   (concat "file://" root "/calc.py")))
    (let* ((symbols (jsonrpc-request
                     server :textDocument/documentSymbol
                     (list :textDocument (eglot--TextDocumentIdentifier))
                     :timeout 60))
           (names (mapcar (lambda (symbol) (plist-get symbol :name))
                          (append symbols nil))))
      (should (member "add" names))
      (should (member "Calculator" names)))
    ;; Hover over the call on the last line: the docstring comes back.
    (goto-char (point-max))
    (search-backward "add(1, 2)")
    (let ((hover (jsonrpc-request server :textDocument/hover
                                  (eglot--TextDocumentPositionParams)
                                  :timeout 60)))
      (should (string-match-p
               "sum of FIRST and SECOND"
               (format "%S" (plist-get hover :contents)))))))

(scrutiny-agent-eglot-tests--deftest scrutiny-agent-eglot-a-large-document-crosses-intact
  ;; One message far larger than what a process filter is handed at a
  ;; time, in each direction: the bridge must neither reorder nor drop
  ;; the pieces, however it batches them.
  (scrutiny-agent-eglot-tests--with-server (buffer server root)
    (ignore root)
    (with-current-buffer buffer
      (goto-char (point-max))
      (dotimes (i 3000)
        (insert (format "def generated_%04d(value):\n    return value + %d\n\n"
                        i i)))
      ;; Client to server: the whole document in one notification.
      (jsonrpc-notify
       server :textDocument/didOpen
       (list :textDocument
             (list :uri (plist-get (eglot--TextDocumentIdentifier) :uri)
                   :languageId "python" :version 1
                   :text (buffer-substring-no-properties (point-min)
                                                         (point-max)))))
      ;; Server to client: three thousand symbols in one reply.
      (let* ((symbols (jsonrpc-request
                       server :textDocument/documentSymbol
                       (list :textDocument (eglot--TextDocumentIdentifier))
                       :timeout 120))
             (names (mapcar (lambda (symbol) (plist-get symbol :name))
                            (append symbols nil))))
        (should (>= (length names) 3000))
        (should (member "generated_0000" names))
        (should (member "generated_2999" names))))))

(provide 'scrutiny-agent-eglot-tests)
;;; scrutiny-agent-eglot-tests.el ends here
