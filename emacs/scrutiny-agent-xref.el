;;; scrutiny-agent-xref.el --- xref and eldoc over a scrutiny-agent  -*- lexical-binding: t; -*-

;; Author: Lally Singh
;; URL: https://github.com/lally/scrutiny-agent
;; Version: 0.2.0
;; (The package's requirements are declared in scrutiny-agent.el;
;; a Package-Requires header outside the main file has no effect.)
;; Keywords: tools, languages

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; Exposes the agent's request-level `lsp.*' queries through the
;; interfaces Emacs already has, so `M-.', `M-?' and eldoc work in a
;; remote buffer without a full eglot session.
;;
;; This is deliberately the *lightweight* path.  For real editing use
;; eglot over the LSP tunnel (`scrutiny-agent-eglot.el'): it gets
;; completion, diagnostics, formatting, and a live document that tracks
;; your edits.  What this file is for is the case eglot does not cover
;; well -- reading code you have not opened a project for.  Jump into a
;; remote file, ask where a symbol is defined, follow it, and never
;; start a language-server session that outlives the question.
;;
;;   (require 'scrutiny-agent-xref)
;;   (add-hook 'python-ts-mode-hook #'scrutiny-agent-code-mode)
;;
;; or, in a buffer produced by `scrutiny-agent-view-file':
;;
;;   M-x scrutiny-agent-code-mode
;;
;; The agent holds one language-server session per (workspace,
;; language) and answers each query against the buffer text you send,
;; so unsaved edits are respected the same way a local LSP client
;; handles them.

;;; Code:

(require 'cl-lib)
(require 'subr-x)
(require 'url-util)                     ; url-unhex-string
(require 'xref)
(require 'scrutiny-agent)
(require 'scrutiny-agent-ops)
(require 'scrutiny-agent-ui)

(defgroup scrutiny-agent-xref nil
  "xref and eldoc backed by a scrutiny-agent."
  :group 'scrutiny-agent
  :prefix "scrutiny-agent-xref-")

(defcustom scrutiny-agent-xref-open-function #'scrutiny-agent-xref-open-tramp
  "How to open a remote file a cross-reference points at.

`scrutiny-agent-xref-open-tramp' (the default) opens it through
TRAMP, so the buffer is editable and behaves like any other file.
`scrutiny-agent-xref-open-agent' reads it over the agent channel
instead: no TRAMP setup needed and no second connection, but the
buffer is read-only."
  :type '(choice (const :tag "Through TRAMP (editable)"
                        scrutiny-agent-xref-open-tramp)
                 (const :tag "Over the agent (read-only)"
                        scrutiny-agent-xref-open-agent)
                 function))

;; ---------------------------------------------------------------------
;; Buffer context
;; ---------------------------------------------------------------------

(defun scrutiny-agent-xref--context ()
  "(HOST WORKSPACE LANGUAGE FILE CONTENT) for this buffer, or nil.
Returns nil rather than signaling, so the backend can decline and let
another xref backend answer."
  (let ((language (scrutiny-agent-ui--buffer-language))
        (file (or (and buffer-file-name
                       (file-local-name buffer-file-name))
                  scrutiny-agent-ui--path))
        (host (or scrutiny-agent-ui--host
                  (and buffer-file-name
                       (scrutiny-agent-xref--host buffer-file-name))
                  scrutiny-agent-ui-host)))
    (when (and language file host)
      (list host (file-name-directory file) language file
            (buffer-substring-no-properties (point-min) (point-max))))))

(defun scrutiny-agent-xref--host (path)
  "Configured scrutiny-agent host serving remote PATH, or nil."
  (let ((tramp-host (file-remote-p path 'host)))
    (when tramp-host
      (and (assoc tramp-host scrutiny-agent-hosts) tramp-host))))

(defun scrutiny-agent-xref--position ()
  "Point as the protocol's 0-based (LINE . CHARACTER)."
  (cons (1- (line-number-at-pos))
        (- (point) (line-beginning-position))))

;; ---------------------------------------------------------------------
;; Locations
;; ---------------------------------------------------------------------

(defun scrutiny-agent-xref--uri-path (uri)
  "Local filesystem path named by LSP URI."
  (let ((path (string-remove-prefix "file://" (or uri ""))))
    ;; Servers percent-encode; undo the encodings that actually occur in
    ;; paths rather than pulling in a full URI parser.
    (url-unhex-string path)))

(defun scrutiny-agent-xref--remote-name (host path)
  "TRAMP file name for PATH on HOST, using this buffer's method."
  (let ((prefix (and buffer-file-name (file-remote-p buffer-file-name))))
    (if prefix
        (concat prefix path)
      (format "/ssh:%s:%s" host path))))

(defun scrutiny-agent-xref-open-tramp (host path)
  "Open remote PATH on HOST through TRAMP; return the buffer."
  (find-file-noselect (scrutiny-agent-xref--remote-name host path)))

(defun scrutiny-agent-xref-open-agent (host path)
  "Read remote PATH on HOST over the agent; return a read-only buffer."
  (scrutiny-agent-view-file path host))

;; An xref location is any object implementing the three generics
;; below -- there is no class to inherit from (`xref-file-location' is
;; itself just a struct), so this is a struct too.
(cl-defstruct (scrutiny-agent-xref-location
               (:constructor scrutiny-agent-xref-location-make))
  host
  path
  line                                  ; 0-based, as the protocol reports
  character)

(cl-defmethod xref-location-group ((location scrutiny-agent-xref-location))
  (scrutiny-agent-xref-location-path location))

(cl-defmethod xref-location-line ((location scrutiny-agent-xref-location))
  ;; xref counts lines from one; the protocol counts from zero.
  (1+ (scrutiny-agent-xref-location-line location)))

(cl-defmethod xref-location-marker ((location scrutiny-agent-xref-location))
  (let ((buffer (funcall scrutiny-agent-xref-open-function
                         (scrutiny-agent-xref-location-host location)
                         (scrutiny-agent-xref-location-path location))))
    (with-current-buffer buffer
      (save-restriction
        (widen)
        (save-excursion
          (goto-char (point-min))
          (forward-line (scrutiny-agent-xref-location-line location))
          (forward-char
           (min (scrutiny-agent-xref-location-character location)
                (- (line-end-position) (point))))
          (point-marker))))))

(defun scrutiny-agent-xref--summary (host path line)
  "The text of LINE (0-based) in PATH, for the xref listing.
Read over the agent -- one small request, no TRAMP session -- and
degraded to the location itself if that fails."
  (or (ignore-errors
        (let* ((conn (scrutiny-agent-connection host))
               (content (and conn
                             (scrutiny-agent-ops-read-file conn path)))
               (lines (and content (split-string content "\n"))))
          (when (and lines (< line (length lines)))
            (string-trim (nth line lines)))))
      (format "%s:%d" (file-name-nondirectory path) (1+ line))))

(defun scrutiny-agent-xref--make (host location)
  "An `xref-item' for one protocol LOCATION, or nil if unusable."
  (let* ((uri (or (plist-get location :uri)
                  (plist-get (plist-get location :location) :uri)))
         (range (or (plist-get location :range)
                    (plist-get (plist-get location :location) :range)))
         (start (plist-get range :start))
         (path (and uri (scrutiny-agent-xref--uri-path uri)))
         (line (or (plist-get start :line) 0))
         (character (or (plist-get start :character) 0)))
    (when (and path (not (string-empty-p path)))
      (xref-make (scrutiny-agent-xref--summary host path line)
                 (scrutiny-agent-xref-location-make
                  :host host :path path
                  :line line :character character)))))

;; ---------------------------------------------------------------------
;; The xref backend
;; ---------------------------------------------------------------------

;;;###autoload
(defun scrutiny-agent-xref-backend ()
  "`xref-backend-functions' entry: `scrutiny-agent' where applicable."
  (and (scrutiny-agent-xref--context) 'scrutiny-agent))

(cl-defmethod xref-backend-identifier-at-point ((_backend (eql scrutiny-agent)))
  ;; The agent resolves by position, not by name, so the identifier is
  ;; only ever shown to the user -- but xref insists on one.
  (thing-at-point 'symbol t))

(cl-defmethod xref-backend-identifier-completion-table
  ((_backend (eql scrutiny-agent)))
  ;; Completion would need workspace symbols, which many servers do not
  ;; implement; an empty table lets the user type freely.
  nil)

(cl-defmethod xref-backend-definitions ((_backend (eql scrutiny-agent))
                                        _identifier)
  (cl-destructuring-bind (host workspace language file content)
      (or (scrutiny-agent-xref--context)
          (user-error "This buffer has no scrutiny-agent context"))
    (let* ((position (scrutiny-agent-xref--position))
           (conn (scrutiny-agent-ui-connection host))
           (locations (scrutiny-agent-ui--with-error "lsp.gotoDefinition"
						     (scrutiny-agent-ops-goto-definition
						      conn workspace language file content
						      (car position) (cdr position)))))
      (delq nil (mapcar (lambda (l) (scrutiny-agent-xref--make host l))
                        locations)))))

(cl-defmethod xref-backend-references ((_backend (eql scrutiny-agent))
                                       _identifier)
  (cl-destructuring-bind (host workspace language file content)
      (or (scrutiny-agent-xref--context)
          (user-error "This buffer has no scrutiny-agent context"))
    (let* ((position (scrutiny-agent-xref--position))
           (conn (scrutiny-agent-ui-connection host))
           (locations (scrutiny-agent-ui--with-error "lsp.findReferences"
						     (scrutiny-agent-ops-find-references
						      conn workspace language file content
						      (car position) (cdr position)))))
      (delq nil (mapcar (lambda (l) (scrutiny-agent-xref--make host l))
                        locations)))))

(cl-defmethod xref-backend-apropos ((_backend (eql scrutiny-agent)) pattern)
  (cl-destructuring-bind (host workspace language _file _content)
      (or (scrutiny-agent-xref--context)
          (user-error "This buffer has no scrutiny-agent context"))
    (let* ((conn (scrutiny-agent-ui-connection host))
           (symbols (condition-case err
                        (scrutiny-agent-ops-workspace-symbols
                         conn workspace language pattern)
                      (scrutiny-agent-rpc-error
                       ;; workspace/symbol is optional in LSP; say so
                       ;; rather than reporting "no matches".
                       (if (equal (nth 1 err) 1004)
                           (user-error
                            "This language server does not support workspace symbol search")
                         (signal (car err) (cdr err)))))))
      (delq nil (mapcar (lambda (s) (scrutiny-agent-xref--make host s))
                        symbols)))))

;; ---------------------------------------------------------------------
;; eldoc
;; ---------------------------------------------------------------------

;;;###autoload
(defun scrutiny-agent-eldoc-function (callback &rest _ignored)
  "`eldoc-documentation-functions' entry backed by the agent's hover.

Answers asynchronously: the request goes out and CALLBACK fires when
it returns, so a slow remote server never blocks redisplay."
  (let ((context (scrutiny-agent-xref--context)))
    (when context
      (cl-destructuring-bind (host workspace language file content) context
	(let* ((position (scrutiny-agent-xref--position))
               (conn (ignore-errors (scrutiny-agent-connection host))))
          (when conn
            (scrutiny-agent-async-request
             conn "lsp.hover"
             (list :workspacePath workspace
                   :language language
                   :filePath file
                   :fileContent content
                   :line (car position)
                   :character (cdr position))
             (lambda (result _error)
               (let* ((hover (plist-get result :hover))
                      (text (and hover (plist-get hover :contents))))
                 (when (and text (not (string-empty-p text)))
                   (funcall callback text)))))
            ;; Tell eldoc an answer is coming rather than that there is none.
            t))))))

;; ---------------------------------------------------------------------
;; imenu
;; ---------------------------------------------------------------------

(defconst scrutiny-agent-xref--symbol-kinds
  '((1 . "File") (2 . "Module") (3 . "Namespace") (4 . "Package")
    (5 . "Class") (6 . "Method") (7 . "Property") (8 . "Field")
    (9 . "Constructor") (10 . "Enum") (11 . "Interface") (12 . "Function")
    (13 . "Variable") (14 . "Constant") (15 . "String") (16 . "Number")
    (17 . "Boolean") (18 . "Array") (19 . "Object") (20 . "Key")
    (21 . "Null") (22 . "EnumMember") (23 . "Struct") (24 . "Event")
    (25 . "Operator") (26 . "TypeParameter"))
  "LSP SymbolKind numbers, used to group the imenu index.")

(defun scrutiny-agent-xref--symbol-position (symbol)
  "Buffer position for SYMBOL, whichever range shape the server used."
  (let* ((range (or (plist-get symbol :selectionRange)
                    (plist-get symbol :range)
                    (plist-get (plist-get symbol :location) :range)))
         (start (plist-get range :start))
         (line (or (plist-get start :line) 0))
         (character (or (plist-get start :character) 0)))
    (save-excursion
      (goto-char (point-min))
      (forward-line line)
      (forward-char (min character (- (line-end-position) (point))))
      (point))))

(defun scrutiny-agent-xref--flatten-symbols (symbols)
  "SYMBOLS and their children as one flat list.
Servers answer `documentSymbol' with either a flat SymbolInformation
list or a nested DocumentSymbol tree; imenu wants every entry either
way."
  (let (out)
    (dolist (symbol symbols)
      (push symbol out)
      (let ((children (plist-get symbol :children)))
	(when children
	  (dolist (child (scrutiny-agent-xref--flatten-symbols
                          (scrutiny-agent-ops--list children)))
            (push child out)))))
    (nreverse out)))

;;;###autoload
(defun scrutiny-agent-imenu-index ()
  "`imenu-create-index-function' backed by the agent's document symbols.
Entries are grouped by symbol kind, so `imenu' offers \"Function\",
\"Class\" and so on rather than one flat list."
  (let ((context (scrutiny-agent-xref--context)))
    (when context
      (cl-destructuring-bind (host workspace language file content) context
	(let* ((conn (scrutiny-agent-ui-connection host))
               (symbols (condition-case nil
                            (scrutiny-agent-ops-document-symbols
                             conn workspace language file content)
                          (scrutiny-agent-rpc-error nil)))
               (groups (make-hash-table :test #'equal)))
          (dolist (symbol (scrutiny-agent-xref--flatten-symbols symbols))
            (let ((name (plist-get symbol :name)))
	      (when name
		(let ((kind (or (cdr (assq (plist-get symbol :kind)
					   scrutiny-agent-xref--symbol-kinds))
				"Other")))
		  (push (cons name (scrutiny-agent-xref--symbol-position symbol))
			(gethash kind groups))))))
          (let (index)
            (maphash (lambda (kind entries)
                       (push (cons kind (nreverse entries)) index))
                     groups)
            (sort index (lambda (a b) (string< (car a) (car b))))))))))

;; ---------------------------------------------------------------------
;; Minor mode
;; ---------------------------------------------------------------------

(defvar-local scrutiny-agent-xref--saved-imenu nil
  "The buffer's previous `imenu-create-index-function', for restoring.")

;;;###autoload
(define-minor-mode scrutiny-agent-code-mode
  "Answer xref, eldoc and imenu from a scrutiny-agent language server.

`M-.', `M-?', `C-M-.', eldoc and `imenu' all query the server running
next to the code on the remote host -- no new commands to learn.  For
full editing support (completion, diagnostics, formatting) use eglot
over the LSP tunnel instead; this is the lightweight path for reading
code you have not opened a project for."
  :lighter " Scrutiny"
  :group 'scrutiny-agent-xref
  (if scrutiny-agent-code-mode
      (progn
        (add-hook 'xref-backend-functions #'scrutiny-agent-xref-backend
                  nil t)
        (add-hook 'eldoc-documentation-functions
                  #'scrutiny-agent-eldoc-function nil t)
        (setq scrutiny-agent-xref--saved-imenu imenu-create-index-function)
        (setq-local imenu-create-index-function
                    #'scrutiny-agent-imenu-index))
    (remove-hook 'xref-backend-functions #'scrutiny-agent-xref-backend t)
    (remove-hook 'eldoc-documentation-functions
                 #'scrutiny-agent-eldoc-function t)
    (when scrutiny-agent-xref--saved-imenu
      (setq-local imenu-create-index-function
                  scrutiny-agent-xref--saved-imenu))))

(provide 'scrutiny-agent-xref)
;;; scrutiny-agent-xref.el ends here
