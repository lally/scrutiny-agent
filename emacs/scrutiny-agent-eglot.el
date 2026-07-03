;;; scrutiny-agent-eglot.el --- Eglot over a scrutiny-agent LSP tunnel  -*- lexical-binding: t; -*-

;; Author: Lally Singh
;; URL: https://github.com/lally/scrutiny-agent
;; Version: 0.2.0
;; Package-Requires: ((emacs "29.1"))
;; Keywords: tools, languages

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; Runs eglot's language server on the remote box, tunneled through an
;; established scrutiny-agent connection instead of a fresh TRAMP
;; subprocess.  You edit via TRAMP as usual; LSP rides the agent's
;; multiplexed channel (one persistent transport session, priority
;; lanes, no per-request connection setup) -- the difference is night
;; and day on Teleport-style links.
;;
;; How it works: `lsp.tunnelOpen' spawns the server next to the code;
;; the agent pipes raw LSP bytes both ways (it never reframes them).
;; Locally, a loopback socket pair bridges those bytes to a process
;; object eglot owns, so eglot speaks its native protocol end to end.
;; Because the buffers are TRAMP buffers, eglot already sends the
;; remote-local paths in URIs -- exactly what the remote server
;; expects.
;;
;; Setup, given a host named as in `scrutiny-agent-hosts':
;;
;;   (require 'scrutiny-agent-eglot)
;;   (scrutiny-agent-eglot-setup)   ; wire the built-in language list
;;
;; then M-x eglot in a TRAMP buffer whose host matches (see
;; `scrutiny-agent-eglot-host-alist' when the TRAMP host name differs
;; from the scrutiny-agent host name).  Non-remote buffers, and remote
;; buffers on unconfigured hosts, fall through to eglot's normal
;; server programs.

;;; Code:

(require 'scrutiny-agent)
(require 'eglot)

(defgroup scrutiny-agent-eglot nil
  "Eglot over scrutiny-agent LSP tunnels."
  :group 'scrutiny-agent)

(defcustom scrutiny-agent-eglot-host-alist nil
  "Alist mapping TRAMP host names to `scrutiny-agent-hosts' names.
Only needed when they differ; a TRAMP host whose name directly
matches a configured scrutiny-agent host needs no entry."
  :type '(alist :key-type string :value-type string))

(defcustom scrutiny-agent-eglot-languages
  '(((rust-mode rust-ts-mode) . 1)
    ((python-mode python-ts-mode) . 2)
    ((js-mode js-ts-mode javascript-mode) . 3)
    ((typescript-mode typescript-ts-mode tsx-ts-mode) . 4)
    ((go-mode go-ts-mode) . 5)
    ((c++-mode c++-ts-mode) . 6)
    ((c-mode c-ts-mode) . 7)
    ((swift-mode swift-ts-mode) . 8))
  "Major modes mapped to protocol language ints (docs/protocol.md).
The agent resolves each int to a server on the remote: rust-analyzer,
pylsp, typescript-language-server, gopls, clangd, sourcekit-lsp."
  :type '(alist :key-type (repeat symbol) :value-type integer))

(defun scrutiny-agent-eglot--language (mode)
  "Protocol language int for major MODE (parent modes included)."
  (cl-loop for (modes . lang) in scrutiny-agent-eglot-languages
           when (cl-some (lambda (m) (provided-mode-derived-p mode m))
                         modes)
           return lang))

(defun scrutiny-agent-eglot--host (dir)
  "Configured scrutiny-agent host name serving remote DIR, or nil."
  (when-let ((tramp-host (file-remote-p dir 'host)))
    (let ((name (or (cdr (assoc tramp-host scrutiny-agent-eglot-host-alist))
                    tramp-host)))
      (when (assoc name scrutiny-agent-hosts)
        name))))

(defun scrutiny-agent-eglot--bridge (host workspace language)
  "Open a tunnel on HOST for WORKSPACE/LANGUAGE and return a local
process speaking raw LSP bytes -- the object handed to eglot.

The loopback pair: eglot owns the client end; the accepted end's
filter forwards into the tunnel and tunnel output is written back to
it.  Closing either side tears down the other and the tunnel."
  (let* ((conn (scrutiny-agent-connect host))
         (accepted nil)
         (tunnel nil)
         (server
          (make-network-process
           :name (format "scrutiny-eglot-bridge[%s]" host)
           :server 1 :host 'local :service t
           :coding 'binary :noquery t
           :log (lambda (_srv client _msg)
                  (setq accepted client)
                  (set-process-query-on-exit-flag client nil)
                  (set-process-coding-system client 'binary 'binary)
                  (set-process-filter
                   client (lambda (_p bytes)
                            (when tunnel
                              (scrutiny-agent-tunnel-send tunnel bytes))))
                  (set-process-sentinel
                   client (lambda (p _e)
                            (unless (process-live-p p)
                              (when tunnel
                                (scrutiny-agent-tunnel-close tunnel)
                                (setq tunnel nil)))))))))
    (unwind-protect
        (let ((client (open-network-stream
                       (format "scrutiny-eglot[%s]" host)
                       nil "localhost"
                       (process-contact server :service)
                       :type 'plain :coding 'binary)))
          (set-process-query-on-exit-flag client nil)
          ;; Wait for the accept so tunnel output has somewhere to go
          ;; before eglot's first write can possibly round-trip.
          (let ((deadline (+ (float-time) 5)))
            (while (and (not accepted) (< (float-time) deadline))
              (accept-process-output nil 0.05))
            (unless accepted
              (delete-process client)
              (error "scrutiny-agent-eglot: loopback accept timed out")))
          (setq tunnel
                (scrutiny-agent-tunnel-open
                 conn workspace language
                 :on-bytes (lambda (bytes)
                             (when (process-live-p accepted)
                               (process-send-string accepted bytes)))
                 :on-closed (lambda (reason)
                              (scrutiny-agent--log
                               conn "eglot tunnel closed: %s" reason)
                              (setq tunnel nil)
                              (when (process-live-p accepted)
                                (delete-process accepted)))))
          (scrutiny-agent--log conn "eglot bridge up: ws=%s lang=%s server=%s"
                               workspace language
                               (scrutiny-agent-tunnel-server-path tunnel))
          client)
      ;; The listener's one job is done once the pair exists.
      (run-at-time 1 nil (lambda () (when (process-live-p server)
                                      (delete-process server)))))))

;;;###autoload
(defun scrutiny-agent-eglot-contact (&optional fallback)
  "Return a CONTACT function for `eglot-server-programs'.
In a remote buffer on a configured scrutiny-agent host with a known
language, produces an agent-tunneled server; otherwise returns
FALLBACK (e.g. the usual local command list) so non-remote buffers
behave exactly as before."
  (lambda (&optional _interactive _project)
    (let* ((host (scrutiny-agent-eglot--host default-directory))
           (lang (scrutiny-agent-eglot--language major-mode))
           (workspace (file-local-name (expand-file-name default-directory))))
      (if (and host lang)
          (list 'eglot-lsp-server
                :process (lambda ()
                           (scrutiny-agent-eglot--bridge host workspace lang)))
        fallback))))

;;;###autoload
(defun scrutiny-agent-eglot-setup ()
  "Register agent-tunneled contacts for every language in
`scrutiny-agent-eglot-languages', preserving eglot's existing entry
as the non-remote fallback."
  (interactive)
  (dolist (entry scrutiny-agent-eglot-languages)
    (let* ((modes (car entry))
           (existing (cdr (cl-find-if
                           (lambda (e)
                             (let ((k (car e)))
                               (cl-some (lambda (m)
                                          (memq m (if (listp k) (flatten-tree
                                                                 (list k))
                                                    (list k))))
                                        modes)))
                           eglot-server-programs))))
      (add-to-list 'eglot-server-programs
                   (cons modes (scrutiny-agent-eglot-contact existing))))))

(provide 'scrutiny-agent-eglot)
;;; scrutiny-agent-eglot.el ends here
