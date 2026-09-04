;;; scrutiny-agent-remote.el --- One-call remote development setup  -*- lexical-binding: t; -*-

;; Author: Lally Singh
;; URL: https://github.com/lally/scrutiny-agent
;; Version: 0.2.0
;; (The package's requirements are declared in scrutiny-agent.el;
;; a Package-Requires header outside the main file has no effect.)
;; Keywords: tools, files, vc

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; Turns on everything at once, for the case this project exists to
;; serve: editing on a Linux server from a laptop, over a link where
;; opening a session is expensive -- Teleport, a jump host, MFA'd SSH.
;;
;;   (require 'scrutiny-agent-remote)
;;   (scrutiny-agent-remote-setup
;;    '(("devbox" :transport "tsh ssh devbox"
;;               :roots ("src" "work"))))
;;
;; That configures the host, derives the agent's argv from `:roots',
;; and enables the file accelerator, magit routing, eglot tunneling and
;; the xref/eldoc/imenu backends.  Afterwards you use Emacs exactly as
;; you always have -- `C-x C-f /ssh:devbox:~/src/proj/main.py', `M-x
;; magit-status', `M-.', `C-x C-s' -- and all of it rides one
;; connection.
;;
;; The single connection is the whole point.  TRAMP opens a shell and
;; talks to it in text, re-verifying state constantly; each operation
;; is several round trips, and `tsh' cannot amortize them the way
;; OpenSSH's ControlMaster can.  The agent answers in one framed
;; request on a connection that is already up, and answers a whole
;; directory's worth at once.
;;
;; `M-x scrutiny-agent-remote-status' reports what is actually active,
;; and `M-x scrutiny-agent-fs-benchmark' measures it.

;;; Code:

(require 'cl-lib)
(require 'subr-x)
(require 'scrutiny-agent)
(require 'scrutiny-agent-ops)
(require 'scrutiny-agent-ui)
(require 'scrutiny-agent-fs)
(require 'scrutiny-agent-xref)

;; Optional companions: loaded at setup time when present, so magit and
;; eglot stay soft dependencies.
(declare-function scrutiny-agent-eglot-setup "scrutiny-agent-eglot")
(declare-function scrutiny-agent-magit-mode "scrutiny-agent-magit")
(defvar scrutiny-agent-magit-mode)
(defvar scrutiny-agent-magit-host-alist)
(defvar scrutiny-agent-eglot-host-alist)

(defgroup scrutiny-agent-remote nil
  "One-call remote development setup."
  :group 'scrutiny-agent
  :prefix "scrutiny-agent-remote-")

(defcustom scrutiny-agent-remote-default-roots '("src")
  "Directories (relative to the remote $HOME) the agent may touch.
This is the filesystem sandbox: nothing outside these roots can be
read or written, whatever a client asks for."
  :type '(repeat string))

(defcustom scrutiny-agent-remote-git-preset "magit"
  "`--git-exec-preset' for hosts configured by this file.
\"magit\" is what magit needs to run; \"read-only\" restricts the
agent to git subcommands that cannot modify a repository, at the cost
of magit falling back to TRAMP for anything that does."
  :type '(choice (const "magit") (const "read-only") (const :tag "none" nil)))

(defcustom scrutiny-agent-remote-allow-write t
  "Whether hosts configured by this file may write files.
Nil keeps the agent read-only: reads, listings and git stay fast, but
`save-buffer' falls back to TRAMP."
  :type 'boolean)

(defcustom scrutiny-agent-remote-log t
  "Whether to enable agent-side logging on configured hosts.
Costs nothing measurable and is what `M-x scrutiny-agent-remote-logs'
reads when something goes wrong."
  :type 'boolean)

;; ---------------------------------------------------------------------
;; Configuration
;; ---------------------------------------------------------------------

(defun scrutiny-agent-remote--config (plist)
  "Agent config file contents for a host PLIST.

Generated rather than hand-written, but installed on the host as a
real file: the policy is then readable and editable where the agent
runs, and survives reconnects, instead of living only in this
client's exec line."
  (let ((roots (or (plist-get plist :roots)
                   scrutiny-agent-remote-default-roots)))
    (concat
     "# scrutiny-agent, written by scrutiny-agent-remote-setup.\n"
     "# `key = value' per line; `#' comments. Edit freely -- the client\n"
     "# only rewrites this when its own generated default changes.\n\n"
     "# Filesystem sandbox. Relative paths anchor under $HOME.\n"
     (mapconcat (lambda (root) (format "allow-root = %s\n" root)) roots "")
     (if scrutiny-agent-remote-allow-write
         "\n# Saving goes over the agent instead of falling back to TRAMP.\nallow-write\n"
       "\n# Read-only: saves fall back to TRAMP.\n")
     (if scrutiny-agent-remote-git-preset
         (format "\n# Let magit drive git over this connection.\ngit-exec-preset = %s\n"
                 scrutiny-agent-remote-git-preset)
       "\n# git.exec disabled: magit falls back to TRAMP.\n")
     (if scrutiny-agent-remote-log
         "\nlog = scrutiny-agent.log\nlog-level = info\n"
       ""))))

(defun scrutiny-agent-remote--agent-args (plist)
  "Extra argv for a host, on top of whatever its config file says."
  (plist-get plist :agent-args))

;;;###autoload
(defun scrutiny-agent-remote-setup (hosts)
  "Configure HOSTS and enable every accelerator.

HOSTS is a list of (NAME . PLIST), where PLIST takes the keys
`scrutiny-agent-hosts' understands plus:

  :roots        directories the agent may touch (default
                `scrutiny-agent-remote-default-roots').  Relative
                entries anchor under the remote $HOME.
  :tramp-host   the TRAMP host name, when it differs from NAME.

Anything in `:agent-args' is appended, so a host can override or add
to the derived argv."
  (dolist (entry hosts)
    (let* ((name (car entry))
           (plist (cdr entry))
           (tramp-host (plist-get plist :tramp-host)))
      (setf (alist-get name scrutiny-agent-hosts nil nil #'equal)
            (append (list :transport (plist-get plist :transport))
                    (when (plist-get plist :install-dir)
                      (list :install-dir (plist-get plist :install-dir)))
                    (when (plist-get plist :local-binary)
                      (list :local-binary (plist-get plist :local-binary)))
                    (list :config (scrutiny-agent-remote--config plist)
                          :agent-args
                          (scrutiny-agent-remote--agent-args plist))))
      (when tramp-host
        (dolist (symbol '(scrutiny-agent-fs-host-alist
                          scrutiny-agent-magit-host-alist
                          scrutiny-agent-eglot-host-alist))
          (when (boundp symbol)
            (set symbol (cons (cons tramp-host name)
                              (assoc-delete-all tramp-host
                                                (symbol-value symbol)))))))))

  ;; Files first: the handler must be registered after TRAMP is loaded,
  ;; which `scrutiny-agent-fs-mode' takes care of.
  (scrutiny-agent-fs-mode 1)

  ;; git, if magit is installed.
  (when (require 'scrutiny-agent-magit nil t)
    (when (require 'magit-process nil t)
      (scrutiny-agent-magit-mode 1)))

  ;; LSP through the tunnel, for the modes eglot knows.
  (when (require 'scrutiny-agent-eglot nil t)
    (scrutiny-agent-eglot-setup))

  (message "scrutiny-agent: configured %s"
           (string-join (mapcar #'car hosts) ", ")))

;;;###autoload
(defun scrutiny-agent-remote-connect (&optional name)
  "Connect to NAME now and report what the agent will serve.

Worth doing at the start of a session: the expensive part of a
Teleport link is establishing it, and every later operation reuses
this one connection."
  (interactive)
  (let* ((host (or name (scrutiny-agent-ui-host)))
         (start (float-time))
         (conn (scrutiny-agent-connect host))
         (elapsed (- (float-time) start))
         (caps (scrutiny-agent--conn-capabilities conn)))
    (message "scrutiny-agent[%s]: connected in %.1fs -- files:%s writes:%s \
git:%s lsp:%s"
             host elapsed
             (if (scrutiny-agent-fs-active-p) "yes" "NOT ACTIVE")
             (if (member "fs.writeFile" caps) "yes" "read-only")
             (if (member "git.exec" caps) "yes" "no")
             (if (member "lsp.tunnelOpen" caps) "yes" "no"))
    conn))

;;;###autoload
(defun scrutiny-agent-remote-status ()
  "Report which accelerators are active, and what is still on TRAMP.

The failure mode worth catching is silent: a handler registered
behind TRAMP's, or an agent without `--allow-write', both leave
everything working -- just slowly."
  (interactive)
  (let* ((hosts (mapcar #'car scrutiny-agent-hosts))
         (live (hash-table-keys scrutiny-agent--connections)))
    (scrutiny-agent-ui--display
     "*scrutiny-agent-remote*" #'special-mode
     (lambda ()
       (insert (propertize "scrutiny-agent remote development\n\n"
                           'face 'bold))
       (insert (format "  file handler   %s\n"
                       (cond ((not scrutiny-agent-fs-mode) "off")
                             ((scrutiny-agent-fs-active-p) "active")
                             (t "INSTALLED BUT BEHIND TRAMP -- run \
M-x scrutiny-agent-fs-refresh"))))
       (insert (format "  magit routing  %s\n"
                       (if (and (boundp 'scrutiny-agent-magit-mode)
                                scrutiny-agent-magit-mode)
                           "on" "off")))
       (insert (format "  attribute cache %.1fs\n"
                       scrutiny-agent-fs-cache-ttl))
       (insert "\nHosts\n")
       (dolist (host hosts)
         (let ((conn (scrutiny-agent-connection host)))
           (insert (format "  %-16s %s\n" host
                           (if conn "connected" "not connected")))
           (when conn
             (let ((caps (scrutiny-agent--conn-capabilities conn)))
               (insert (format "  %-16s   agent %s, %d capabilities\n" ""
                               (scrutiny-agent--conn-agent-version conn)
                               (length caps)))
               (insert (format "  %-16s   writes %s, git.exec %s, tunnels %s\n"
                               ""
                               (if (member "fs.writeFile" caps)
                                   "enabled" "READ-ONLY (saves use TRAMP)")
                               (if (member "git.exec" caps)
                                   "enabled" "off (magit uses TRAMP)")
                               (if (member "lsp.tunnelOpen" caps)
                                   "enabled" "off")))
               (let ((roots (ignore-errors
                              (scrutiny-agent-ops-allowed-roots conn))))
                 (insert (format "  %-16s   roots: %s\n" ""
                                 (if roots (string-join roots " ")
                                   "(unknown)"))))))))
       (unless live
         (insert "\nNothing is connected yet; \
M-x scrutiny-agent-remote-connect warms one up.\n"))
       (insert "\nMeasure it with M-x scrutiny-agent-fs-benchmark.\n")))))

;;;###autoload
(defun scrutiny-agent-remote-open (host directory)
  "Open DIRECTORY on HOST, connecting first if needed.
A convenience for starting a session: connect once, then browse."
  (interactive
   (let ((host (completing-read "Host: "
                                (mapcar #'car scrutiny-agent-hosts) nil t)))
     (list host (read-string "Remote directory: " "~/src/"))))
  (scrutiny-agent-remote-connect host)
  (dired (format "/ssh:%s:%s" host directory)))

(provide 'scrutiny-agent-remote)
;;; scrutiny-agent-remote.el ends here
