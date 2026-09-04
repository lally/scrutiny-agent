;;; scrutiny-agent-menu.el --- Transient menu for scrutiny-agent  -*- lexical-binding: t; -*-

;; Author: Lally Singh
;; URL: https://github.com/lally/scrutiny-agent
;; Version: 0.2.0
;; (The package's requirements are declared in scrutiny-agent.el;
;; a Package-Requires header outside the main file has no effect.)
;; Keywords: tools, vc

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; `M-x scrutiny-agent-menu' reaches this, loading it and transient on
;; demand; without transient installed that command falls back to
;; `completing-read' and this file is never loaded.
;;
;; It is a file of its own for exactly one reason: `(require
;; \='transient)' costs more than five minutes of Elsa\='s time, so any
;; file that requires it cannot be statically analysed at all.  Holding
;; it here keeps scrutiny-agent-ui.el and scrutiny-agent-verify.el --
;; 1600 lines of real logic -- inside the analyser, and leaves only
;; these forty declarative lines outside it.

;;; Code:

(require 'transient)
(require 'scrutiny-agent-ui)

(declare-function scrutiny-agent-verify "scrutiny-agent-verify")
(declare-function scrutiny-agent-code-mode "scrutiny-agent-xref")

;;;###autoload (autoload 'scrutiny-agent-transient "scrutiny-agent-menu" nil t)
(transient-define-prefix scrutiny-agent-transient ()
  "Operate a scrutiny-agent remote backend."
  [["Connection"
    ("c" "Connect"          scrutiny-agent-connect)
    ("q" "Disconnect"       scrutiny-agent-disconnect)
    ("i" "Info / posture"   scrutiny-agent-info)
    ("p" "Ping"             scrutiny-agent-ping)
    ("L" "Remote log tail"  scrutiny-agent-remote-logs)]
   ["Files"
    ("b" "Browse directory" scrutiny-agent-browse)
    ("f" "View file"        scrutiny-agent-view-file)]
   ["Git"
    ("s" "Repo status"      scrutiny-agent-status)
    ("r" "Branches"         scrutiny-agent-branches)
    ("l" "Commit log"       scrutiny-agent-log)
    ("d" "Unstaged diff"    scrutiny-agent-diff-working-tree)
    ("D" "Staged diff"      scrutiny-agent-diff-staged)
    ("o" "File at revision" scrutiny-agent-show-file-at-revision)
    ("F" "Fetch"            scrutiny-agent-fetch)
    ("C" "Clone / ensure"   scrutiny-agent-clone)]]
  [["Code intelligence"
    ("e" "xref/eldoc/imenu here" scrutiny-agent-code-mode)
    ("." "Definition"       xref-find-definitions)
    ("/" "References"       xref-find-references)
    ("m" "Symbols (imenu)"  imenu)
    ("x" "Build index"      scrutiny-agent-index)]
   ["Watch"
    ("w" "Watch HEAD"       scrutiny-agent-watch)
    ("W" "Stop watch"       scrutiny-agent-unwatch)]
   ["Diagnostics"
    ("v" "Verify all operations" scrutiny-agent-verify)
    ("k" "Credential selftest"   scrutiny-agent-credential-selftest)]])

(provide 'scrutiny-agent-menu)
;;; scrutiny-agent-menu.el ends here
