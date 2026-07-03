;;; scrutiny-agent-tests.el --- ERT tests for scrutiny-agent.el  -*- lexical-binding: t; -*-

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; Pure-function unit tests for the wire client, plus one end-to-end
;; integration test (tagged, gated on SCRUTINY_AGENT_BIN) that runs
;; the REAL bootstrap over a local `sh' transport against a built
;; agent binary: base64 heredoc streaming, sha256 hash gate, exec,
;; meta.hello, chunked-response reassembly, and a raw-LSP tunnel
;; round trip.  CI runs it against the freshly built agent.
;;
;; Run:  emacs -Q --batch -L emacs -l scrutiny-agent-tests.el \
;;         -f ert-run-tests-batch-and-exit

;;; Code:

(require 'ert)
(require 'scrutiny-agent)
(require 'scrutiny-agent-eglot)

;; ---------------------------------------------------------------------
;; Framing
;; ---------------------------------------------------------------------

(ert-deftest scrutiny-agent-frame-roundtrip ()
  (let* ((payload '(:jsonrpc "2.0" :id 7 :method "meta.debug"
                    :params (:padBytes 3)))
         (parsed (scrutiny-agent--parse-frames
                  (scrutiny-agent--frame payload)))
         (msg (car (car parsed))))
    (should (equal (cdr parsed) ""))
    (should (equal (plist-get msg :id) 7))
    (should (equal (plist-get msg :method) "meta.debug"))
    (should (equal (plist-get (plist-get msg :params) :padBytes) 3))))

(ert-deftest scrutiny-agent-frame-partial-then-complete ()
  ;; A frame arriving split anywhere (mid-header, mid-body) parses only
  ;; once complete, and the remainder carries over byte-exactly.
  (let* ((frame (scrutiny-agent--frame '(:jsonrpc "2.0" :id 1 :result t)))
         (cut (- (length frame) 3))
         (first (scrutiny-agent--parse-frames (substring frame 0 cut))))
    (should (null (car first)))
    (should (equal (cdr first) (substring frame 0 cut)))
    (let ((second (scrutiny-agent--parse-frames
                   (concat (cdr first) (substring frame cut)))))
      (should (= 1 (length (car second))))
      (should (equal (cdr second) "")))))

(ert-deftest scrutiny-agent-frame-multiple-and-noise ()
  ;; Two frames in one buffer, preceded by a stray newline (the
  ;; bootstrap handoff race the parser explicitly tolerates).
  (let* ((f1 (scrutiny-agent--frame '(:jsonrpc "2.0" :id 1 :result 1)))
         (f2 (scrutiny-agent--frame '(:jsonrpc "2.0" :id 2 :result 2)))
         (parsed (scrutiny-agent--parse-frames (concat "\r\n\n" f1 f2))))
    (should (= 2 (length (car parsed))))
    (should (equal (mapcar (lambda (m) (plist-get m :id)) (car parsed))
                   '(1 2)))))

(ert-deftest scrutiny-agent-frame-utf8-body ()
  (let* ((payload `(:jsonrpc "2.0" :id 3 :result (:content "éñ😀")))
         (msg (car (car (scrutiny-agent--parse-frames
                         (scrutiny-agent--frame payload))))))
    (should (equal (plist-get (plist-get msg :result) :content) "éñ😀"))))

;; ---------------------------------------------------------------------
;; Chunk splitting and rpc.chunk reassembly
;; ---------------------------------------------------------------------

(ert-deftest scrutiny-agent-split-bytes ()
  (should (equal (scrutiny-agent--split-bytes "abcdef" 2) '("ab" "cd" "ef")))
  (should (equal (scrutiny-agent--split-bytes "abcde" 2) '("ab" "cd" "e")))
  (should (equal (scrutiny-agent--split-bytes "" 2) nil))
  (should (equal (scrutiny-agent--split-bytes "ab" 10) '("ab"))))

(ert-deftest scrutiny-agent-assemble-chunks ()
  ;; Out-of-order (SEQ . DATA) parts reassemble into the envelope, as
  ;; the agent's streamed-response path produces them.
  (let* ((envelope "{\"jsonrpc\":\"2.0\",\"id\":42,\"result\":{\"pad\":\"xyzzy\"}}")
         (b64 (base64-encode-string envelope t))
         (third (/ (length b64) 3))
         (parts (list (cons 1 (substring b64 third (* 2 third)))
                      (cons 0 (substring b64 0 third))
                      (cons 2 (substring b64 (* 2 third)))))
         (msg (scrutiny-agent--assemble-chunks parts)))
    (should (equal (plist-get msg :id) 42))
    (should (equal (plist-get (plist-get msg :result) :pad) "xyzzy"))))

;; ---------------------------------------------------------------------
;; Bootstrap building blocks
;; ---------------------------------------------------------------------

(ert-deftest scrutiny-agent-arch-asset ()
  (should (equal (scrutiny-agent--arch-asset "Linux" "x86_64") "x86_64"))
  (should (equal (scrutiny-agent--arch-asset "Linux" "aarch64") "aarch64"))
  (should (equal (scrutiny-agent--arch-asset "Linux" "arm64") "aarch64"))
  (should (null (scrutiny-agent--arch-asset "Darwin" "arm64")))
  (should (null (scrutiny-agent--arch-asset "Linux" "riscv64"))))

(ert-deftest scrutiny-agent-bootstrap-scripts ()
  (let ((st (scrutiny-agent--bootstrap-script "$HOME/.x" "agent-1-x86_64"
                                              "cafe")))
    (should (string-search "__SCRA_ST__" st))
    (should (string-search "agent-1-x86_64" st))
    (should (string-search "cafe" st))
    (should (string-search "shasum -a 256" st)))  ; sha256sum fallback
  (let ((inst (scrutiny-agent--install-script "$HOME/.x" "agent-1-x86_64")))
    (should (string-search "<<'__SCRB64__'" inst))
    (should (string-search "base64 -d" inst)))
  (let ((ep (scrutiny-agent--install-epilogue "$HOME/.x" "agent-1-x86_64"
                                              "cafe")))
    (should (string-search "__SCRA_INST__" ep))
    (should (string-search "chmod +x" ep))
    (should (string-search "mv" ep))))

(ert-deftest scrutiny-agent-file-sha256 ()
  (let ((f (make-temp-file "scra-hash")))
    (unwind-protect
        (progn
          (with-temp-file f (set-buffer-multibyte nil) (insert "abc"))
          (should (equal (scrutiny-agent--file-sha256 f)
                         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")))
      (delete-file f))))

;; ---------------------------------------------------------------------
;; Eglot glue
;; ---------------------------------------------------------------------

(ert-deftest scrutiny-agent-eglot-language-lookup ()
  (should (equal (scrutiny-agent-eglot--language 'python-mode) 2))
  (should (equal (scrutiny-agent-eglot--language 'rust-ts-mode) 1))
  (should (null (scrutiny-agent-eglot--language 'fundamental-mode))))

(ert-deftest scrutiny-agent-eglot-host-lookup ()
  (let ((scrutiny-agent-hosts '(("devbox" :transport "ssh devbox")))
        (scrutiny-agent-eglot-host-alist '(("devbox.corp" . "devbox"))))
    (should (equal (scrutiny-agent-eglot--host "/ssh:devbox:/src/") "devbox"))
    (should (equal (scrutiny-agent-eglot--host "/ssh:devbox.corp:/src/")
                   "devbox"))
    (should (null (scrutiny-agent-eglot--host "/ssh:other:/src/")))
    (should (null (scrutiny-agent-eglot--host "/home/me/src/")))))

(ert-deftest scrutiny-agent-eglot-contact-fallback ()
  ;; In a non-remote directory the contact function must return the
  ;; fallback untouched, so local editing is byte-identical to stock
  ;; eglot behavior.
  (let* ((default-directory "/tmp/")
         (contact (scrutiny-agent-eglot-contact '("pylsp"))))
    (should (equal (funcall contact) '("pylsp")))))

;; ---------------------------------------------------------------------
;; End-to-end integration (gated; CI sets SCRUTINY_AGENT_BIN)
;; ---------------------------------------------------------------------

(defun scrutiny-agent-tests--wait (pred timeout)
  (let ((deadline (+ (float-time) timeout)))
    (while (and (not (funcall pred)) (< (float-time) deadline))
      (accept-process-output nil 0.05))
    (funcall pred)))

(ert-deftest scrutiny-agent-integration ()
  :tags '(integration)
  (skip-unless (getenv "SCRUTINY_AGENT_BIN"))
  (let* ((bin (getenv "SCRUTINY_AGENT_BIN"))
         (install (make-temp-file "scra-install" t))
         (tmproot (file-name-as-directory temporary-file-directory))
         (scrutiny-agent-hosts
          `(("itest" :transport "sh"
             :install-dir ,install
             :local-binary ,bin
             :agent-args ("--allow-root" ,tmproot))))
         (conn (scrutiny-agent-connect "itest")))
    (unwind-protect
        (progn
          ;; Bootstrap streamed + hash-gated + exec'd; handshake done.
          (should (member "lsp.tunnelOpen"
                          (scrutiny-agent--conn-capabilities conn)))
          ;; Plain request.
          (let ((r (scrutiny-agent-request conn "meta.debug"
                                           '(:padBytes 16))))
            (should (equal (plist-get r :pad) (make-string 16 ?x))))
          ;; Over-cap response: exercises rpc.chunk reassembly here.
          (let ((r (scrutiny-agent-request conn "meta.debug"
                                           '(:padBytes 300000)
                                           :timeout 60)))
            (should (= (length (plist-get r :pad)) 300000)))
          ;; Raw-LSP tunnel round trip (python -> pylsp when present;
          ;; a clean LSP_FAILED is also conformant).
          (let* ((ws (make-temp-file "scra-ws" t))
                 (stream "")
                 (closed nil)
                 (tunnel
                  (condition-case err
                      (scrutiny-agent-tunnel-open
                       conn ws 2
                       :on-bytes (lambda (b)
                                   (setq stream (concat stream b)))
                       :on-closed (lambda (r) (setq closed r)))
                    (scrutiny-agent-rpc-error
                     (should (= (nth 1 err) 1004))
                     nil))))
            (when tunnel
              (let* ((body (json-serialize
                            `(:jsonrpc "2.0" :id 1 :method "initialize"
                              :params (:processId :null
                                       :rootUri ,(concat "file://" ws)
                                       :capabilities ,(make-hash-table)))))
                     (frame (concat (format "Content-Length: %d\r\n\r\n"
                                            (length body))
                                    body))
                     (cut (/ (length frame) 2)))
                ;; Split mid-frame: chunk boundaries are not message
                ;; boundaries, order must hold.
                (scrutiny-agent-tunnel-send tunnel (substring frame 0 cut))
                (scrutiny-agent-tunnel-send tunnel (substring frame cut))
                (should (scrutiny-agent-tests--wait
                         (lambda ()
                           (let ((msgs (car (ignore-errors
                                              (scrutiny-agent--parse-frames
                                               stream)))))
                             (cl-find-if
                              (lambda (m) (and (equal (plist-get m :id) 1)
                                               (plist-get m :result)))
                              msgs)))
                         45))
                (scrutiny-agent-tunnel-close tunnel)
                (should (scrutiny-agent-tests--wait
                         (lambda () closed) 15))))))
      (scrutiny-agent-disconnect "itest")
      (delete-directory install t))))

(provide 'scrutiny-agent-tests)
;;; scrutiny-agent-tests.el ends here
