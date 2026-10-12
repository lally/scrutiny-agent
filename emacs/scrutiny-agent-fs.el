;;; scrutiny-agent-fs.el --- Fast remote file operations over the agent  -*- lexical-binding: t; -*-

;; Author: Lally Singh
;; URL: https://github.com/five-rivers/scrutiny-agent
;; Version: 0.2.0
;; (The package's requirements are declared in scrutiny-agent.el;
;; a Package-Requires header outside the main file has no effect.)
;; Keywords: tools, files

;; This file is part of scrutiny-agent (GPLv3).

;;; Commentary:

;; Serves `find-file', `save-buffer', `dired', completion and the rest
;; of Emacs's file operations over the scrutiny-agent channel instead
;; of TRAMP's shell protocol -- while leaving your file names, your
;; TRAMP configuration and everything else exactly as they are.
;;
;; It is an ACCELERATOR, not a replacement.  It installs a
;; `file-name-handler-alist' entry ahead of TRAMP's that matches only
;; the hosts in `scrutiny-agent-hosts'.  Operations it implements go
;; over the agent; everything else is handed straight back to TRAMP.
;; An unimplemented corner therefore behaves exactly as it does today
;; -- slowly, but correctly -- and turning the mode off restores
;; stock behavior completely.
;;
;;   (require 'scrutiny-agent-fs)
;;   (scrutiny-agent-fs-mode 1)
;;
;; Then `/ssh:devbox:~/src/proj/main.py' opens, saves and lists over
;; the agent.  `M-x scrutiny-agent-fs-benchmark' times the common
;; operations against TRAMP so you can see what it bought you.
;;
;; Why this is worth doing: TRAMP runs a shell on the far end and
;; talks to it in text, re-verifying state constantly.  Each operation
;; is several round trips.  On a link where a round trip is expensive
;; -- Teleport, a jump host -- that dominates everything.  The agent
;; answers the same questions in one framed request on a connection
;; that is already open, and can answer a whole directory's worth of
;; them at once.
;;
;; Saving requires the remote agent to be started with `--allow-write';
;; without it the write operations fall back to TRAMP and only reads
;; are accelerated.

;;; Code:

(require 'cl-lib)
(require 'subr-x)
;; ls-lisp is required outright rather than declared: this package uses
;; its formatter on every listing, and a real require is what lets the
;; byte-compiler and a static analyser check those calls at all --
;; `declare-function' only asserts that a name exists.
(require 'ls-lisp)
(require 'scrutiny-agent)
(require 'scrutiny-agent-ops)

;; TRAMP is the exception: it is just as much a load-time dependency
;; (see `scrutiny-agent-fs-mode', which loads it before registering our
;; handler so that ours ends up in front), but requiring it here drags
;; its whole dependency tree through Elsa, whose reader cannot parse
;; ansi-color.el -- and a crashed analyser reports nothing at all about
;; this file.  Declaring the six functions we call keeps the file
;; analysable.
(declare-function tramp-tramp-file-p "tramp")
(declare-function tramp-dissect-file-name "tramp")
(declare-function tramp-file-name-host "tramp")
(declare-function tramp-file-name-localname "tramp")
(declare-function tramp-file-name-method "tramp")
(declare-function tramp-file-name-user "tramp")

(defgroup scrutiny-agent-fs nil
  "Remote file operations over a scrutiny-agent channel."
  :group 'scrutiny-agent
  :prefix "scrutiny-agent-fs-")

(defcustom scrutiny-agent-fs-host-alist nil
  "Alist mapping TRAMP host names to `scrutiny-agent-hosts' names.
Only needed when the two differ."
  :type '(alist :key-type string :value-type string))

(defcustom scrutiny-agent-fs-cache-ttl 2.0
  "Seconds to reuse a directory's attributes before re-reading it.

Dired, minibuffer completion and `find-file' each ask about many
files in the same directory in quick succession; one listing answers
all of them.  Set to 0 to disable caching -- correct either way, but
noticeably slower.

The cache is dropped for a directory whenever this client writes into
it, so your own edits are never stale.  A change made by someone else
on the remote can be up to this many seconds old; `M-x
scrutiny-agent-fs-flush-cache' or reverting the buffer clears it."
  :type 'number)

(defcustom scrutiny-agent-fs-max-inline-bytes (* 8 1024 1024)
  "Largest file this handler will transfer over the agent channel.
Anything bigger is handed to TRAMP, which can stream it without
holding the whole thing in a base64 payload."
  :type 'integer)

(defvar scrutiny-agent-fs--dir-cache (make-hash-table :test #'equal)
  "HOST+DIR -> (TIMESTAMP . ((NAME . ATTRS-PLIST) ...)).")

(defvar scrutiny-agent-fs--pending-fetches (make-hash-table :test #'equal)
  "Directories currently being fetched in the background.")

(defvar scrutiny-agent-fs--roots-cache (make-hash-table :test #'equal)
  "HOST -> (ROOTS . HOME), the connection facts. Fixed per connection.")

(defcustom scrutiny-agent-fs-verbose nil
  "When non-nil, log diagnostic messages for fs-mode operations.
Set to t to see warnings in *Messages*, or to a buffer name for
a dedicated log.  Useful for debugging why operations fall back
to TRAMP or fail entirely."
  :type '(choice (const :tag "Off" nil)
                 (const :tag "To *Messages*" t)
                 (string :tag "To named buffer")))

(defvar scrutiny-agent-fs--last-error nil
  "The last error encountered by fs-mode, for diagnostics.
A plist with :host, :operation, :error, and :time.")

(defun scrutiny-agent-fs--warn (host format-string &rest args)
  "Log a diagnostic message for HOST using FORMAT-STRING and ARGS.
Output goes to *Messages* or a dedicated buffer per `scrutiny-agent-fs-verbose'."
  (when scrutiny-agent-fs-verbose
    (let ((msg (format "scrutiny-agent-fs[%s]: %s"
                       (or host "?")
                       (apply #'format format-string args))))
      (if (stringp scrutiny-agent-fs-verbose)
          (with-current-buffer (get-buffer-create scrutiny-agent-fs-verbose)
            (goto-char (point-max))
            (insert (format-time-string "[%H:%M:%S] ") msg "\n"))
        (message "%s" msg)))))

(defun scrutiny-agent-fs--record-error (host operation error)
  "Record ERROR for diagnostics; retrievable via `scrutiny-agent-fs-last-error'."
  (setq scrutiny-agent-fs--last-error
        (list :host host :operation operation
              :error error :time (current-time)))
  (scrutiny-agent-fs--warn host "%s failed: %S" operation error))

(defcustom scrutiny-agent-fs-roots-are-the-world t
  "Whether the agent's roots define everything visible on a host.

With this on, a path outside the agent's roots is answered from
structure alone, with no request to anyone:

  * a directory on the way down to a root exists and is a directory
    (it must -- the root beneath it resolved), and lists only the
    entries that lead to roots;
  * anything else does not exist.

This matters more than it sounds.  `project-current' -- which eglot
runs from `find-file-hook' -- calls `locate-dominating-file', which
walks UP the tree looking for `.git'.  That walk leaves your roots
and continues to `/', and with this off every level of it is a TRAMP
round trip on the critical path of `C-x C-f'.

The tradeoff is real: a project root or an include path ABOVE your
configured roots becomes invisible rather than merely unreachable.
The fix for that is to add it to `:roots'.  Set this to nil to have
such paths delegated to TRAMP and answered truthfully, slowly."
  :type 'boolean)

;; ---------------------------------------------------------------------
;; File-name parsing
;; ---------------------------------------------------------------------

(defun scrutiny-agent-fs--parse (filename)
  "Return (HOST PREFIX LOCALNAME) for FILENAME, or nil.
HOST is the `scrutiny-agent-hosts' name, PREFIX the TRAMP prefix to
put back on when returning a file name.

Uses TRAMP's dissector directly rather than `file-remote-p'. The
latter is itself a file-name operation, so calling it from inside a
handler re-enters the whole dispatch chain -- eighty-odd times for a
single `find-file', and deep enough to hit `excessive-lisp-nesting'
if anything traces it. `tramp-dissect-file-name' is a plain function
over the string."
  (when (and (stringp filename)
             (fboundp 'tramp-tramp-file-p)
             (tramp-tramp-file-p filename))
    (condition-case err
        (let ((vec (tramp-dissect-file-name filename)))
          (unless vec
            (scrutiny-agent-fs--warn nil "TRAMP dissect returned nil for %s" filename)
            (cl-return-from scrutiny-agent-fs--parse nil))
          (let ((tramp-host (tramp-file-name-host vec))
                (localname (tramp-file-name-localname vec)))
            (unless (stringp tramp-host)
              (scrutiny-agent-fs--warn nil "no host in TRAMP vector for %s" filename)
              (cl-return-from scrutiny-agent-fs--parse nil))
            (let ((host (or (cdr (assoc tramp-host scrutiny-agent-fs-host-alist))
                            tramp-host)))
              (if (not (assoc host scrutiny-agent-hosts))
                  nil  ; Not our host, silently delegate
                (list host
                      (substring filename
                                 0 (- (length filename) (length (or localname ""))))
                      (or localname ""))))))
      (error
       (scrutiny-agent-fs--record-error nil 'parse err)
       nil))))

(defcustom scrutiny-agent-fs-auto-connect nil
  "Whether to connect automatically when a file operation needs it.
When nil (the default), fs-mode only accelerates hosts you have
already connected to with `scrutiny-agent-connect'.  This avoids
blocking file operations while bootstrapping the agent.

When non-nil, the first file operation on an unconnected host will
block while the connection is established -- which can take tens of
seconds if the binary needs to be streamed over a slow link."
  :type 'boolean)

(defvar scrutiny-agent-fs--connect-hints (make-hash-table :test #'equal)
  "Hosts we've already shown the connect hint for, to avoid spam.")

(defun scrutiny-agent-fs--conn (host)
  "A live connection to HOST, or nil if one cannot be had.
Never signals: a handler that errors on a dead transport would break
every file operation instead of falling back to TRAMP.

With `scrutiny-agent-fs-auto-connect' nil, only returns an existing
connection -- so unconnected hosts fall through to TRAMP immediately
rather than blocking on bootstrap."
  (or (scrutiny-agent-connection host)
      (if scrutiny-agent-fs-auto-connect
          (condition-case err
              (scrutiny-agent-connect host)
            (error
             (scrutiny-agent-fs--record-error host 'connect err)
             nil))
        ;; Not connected and auto-connect is off: show a one-time hint
        (unless (gethash host scrutiny-agent-fs--connect-hints)
          (puthash host t scrutiny-agent-fs--connect-hints)
          (message "scrutiny-agent-fs: %s not connected, using TRAMP (connect with C-c s c)"
                   host))
        nil)))

(defun scrutiny-agent-fs--writable-p (host)
  "Whether HOST's agent serves the fs write surface."
  (let ((conn (scrutiny-agent-fs--conn host)))
    (when conn
      (and (member "fs.writeFile" (scrutiny-agent--conn-capabilities conn)) t))))

(defun scrutiny-agent-fs--interactive-p ()
  "Non-nil if we're in a context where blocking is unacceptable.
During minibuffer completion, any RPC would freeze the UI."
  (let* ((mb (minibufferp))
         (wmb (window-minibuffer-p))
         (mwa (minibuffer-window-active-p (minibuffer-window)))
         (cc (bound-and-true-p company-candidates))
         (result (or mb wmb mwa cc)))
    ;; Debug output - remove after fixing
    (when scrutiny-agent-fs-verbose
      (message "interactive-p: mb=%S wmb=%S mwa=%S cc=%S => %S"
               mb wmb mwa cc result))
    result))

(defun scrutiny-agent-fs--facts (host)
  "The agent's roots and remote home for HOST, as (ROOTS . HOME).

One `meta.capabilities' answers both, and both are fixed for the life
of a connection -- so this is asked once and the failure is cached
too. Retrying a lookup that cannot succeed puts a round trip in front
of every single file operation."
  (let ((cached (gethash host scrutiny-agent-fs--roots-cache 'miss)))
    (cond
     ;; Cache hit - return immediately (facts don't expire)
     ((not (eq cached 'miss)) cached)
     ;; Interactive context with no cache - don't block
     ((scrutiny-agent-fs--interactive-p)
      (scrutiny-agent-fs--warn host "no cached facts during completion, using TRAMP")
      nil)
     ;; Non-interactive - OK to block for the RPC
     (t
      (puthash host
               (let ((conn (scrutiny-agent-fs--conn host)))
                 (if (null conn)
                     (progn
                       (scrutiny-agent-fs--warn host "no connection, cannot fetch capabilities")
                       nil)
                   (condition-case err
                       (let* ((caps (scrutiny-agent-ops-capabilities conn))
                              (fsa (plist-get caps :fileSystemAccess))
                              (roots (scrutiny-agent-ops--list
                                      (plist-get fsa :allowedRoots)))
                              (home (plist-get fsa :home)))
                         (when (null roots)
                           (scrutiny-agent-fs--warn
                            host "no allowedRoots in capabilities (agent may not serve fs)"))
                         (cons roots
                               (and (stringp home)
                                    (not (string-empty-p home))
                                    (directory-file-name home))))
                     (error
                      (scrutiny-agent-fs--record-error host 'capabilities err)
                      nil))))
               scrutiny-agent-fs--roots-cache)))))

(defun scrutiny-agent-fs--roots (host)
  "The filesystem roots HOST's agent will serve, or nil if unknown."
  (car (scrutiny-agent-fs--facts host)))

(defun scrutiny-agent-fs--ancestor-of-root-p (host localname)
  "Whether LOCALNAME is a directory on the way down to one of HOST's roots."
  (let ((path (file-name-as-directory
               (directory-file-name (expand-file-name localname "/")))))
    (cl-some (lambda (root)
               (string-prefix-p path (file-name-as-directory root)))
             (scrutiny-agent-fs--roots host))))

(defun scrutiny-agent-fs--root-children (host localname)
  "Entries of LOCALNAME that lead to one of HOST's roots."
  (let ((path (file-name-as-directory
               (directory-file-name (expand-file-name localname "/"))))
        (out nil))
    (dolist (root (scrutiny-agent-fs--roots host))
      (let ((r (directory-file-name root)))
        (when (and (string-prefix-p path (file-name-as-directory r))
                   (> (length r) (length path)))
          (cl-pushnew (car (split-string (substring r (length path)) "/" t))
                      out :test #'equal))))
    (nreverse out)))

(defun scrutiny-agent-fs--in-roots-p (host localname)
  "Whether LOCALNAME is somewhere HOST's agent would even look.

Anything outside the agent's roots is refused by definition, so
asking costs a round trip to be told no. `dired' alone probes
/bin, /usr/bin and /sbin while setting up a buffer; on a slow link
those are pure waste, and TRAMP can answer them (and cache the
answers) perfectly well.

Unknown roots mean no opinion: everything is offered to the agent,
which is the behavior from before this check existed."
  (let ((roots (scrutiny-agent-fs--roots host)))
    (or (null roots)
        (let ((path (directory-file-name (expand-file-name localname "/"))))
          (cl-some (lambda (root)
                     (let ((r (directory-file-name root)))
                       (or (string= path r)
                           ;; On a component boundary only: "/a/bc" is
                           ;; not under "/a/b".
                           (string-prefix-p (file-name-as-directory r)
                                            (file-name-as-directory path)))))
                   roots)))))

(defun scrutiny-agent-fs--home (host)
  "The remote $HOME on HOST, resolved once per connection."
  (cdr (scrutiny-agent-fs--facts host)))

;; ---------------------------------------------------------------------
;; Delegation
;; ---------------------------------------------------------------------

(defun scrutiny-agent-fs--delegate (operation args)
  "Run OPERATION with ARGS as if this handler were not installed.
The standard idiom: inhibit ourselves for exactly this call, so the
next handler in `file-name-handler-alist' -- TRAMP -- takes it."
  ;; Only log delegations for remote files (to reduce noise from local files)
  (when (and scrutiny-agent-fs-verbose
             (car args)
             (stringp (car args))
             (file-remote-p (car args)))
    (message "DELEGATE TO TRAMP: %s %S" operation (car args)))
  (let ((inhibit-file-name-handlers
         (cons 'scrutiny-agent-fs-handler
               (and (eq inhibit-file-name-operation operation)
                    inhibit-file-name-handlers)))
        (inhibit-file-name-operation operation))
    (apply operation args)))

;; ---------------------------------------------------------------------
;; Attributes
;; ---------------------------------------------------------------------

(defun scrutiny-agent-fs--mode-string (mode isdir islink)
  "The ls-style permission string for numeric MODE."
  (let ((bits "rwxrwxrwx")
        (out (list (cond (islink ?l) (isdir ?d) (t ?-)))))
    (dotimes (i 9)
      (push (if (zerop (logand mode (ash 1 (- 8 i)))) ?- (aref bits i)) out))
    ;; setuid / setgid / sticky, in the positions ls uses for them.
    (let ((s (nreverse out)))
      (when (/= 0 (logand mode #o4000))
        (setf (nth 3 s) (if (eq (nth 3 s) ?x) ?s ?S)))
      (when (/= 0 (logand mode #o2000))
        (setf (nth 6 s) (if (eq (nth 6 s) ?x) ?s ?S)))
      (when (/= 0 (logand mode #o1000))
        (setf (nth 9 s) (if (eq (nth 9 s) ?x) ?t ?T)))
      (apply #'string s))))

(defun scrutiny-agent-fs--attributes (stat &optional id-format)
  "Emacs `file-attributes' list for a protocol STAT plist, or nil."
  (when (scrutiny-agent-ops-bool (plist-get stat :exists))
    (let* ((isdir (scrutiny-agent-ops-bool (plist-get stat :isDir)))
           (islink (scrutiny-agent-ops-bool (plist-get stat :isSymlink)))
           (target (plist-get stat :symlinkTarget))
           (mode (or (plist-get stat :mode) 0))
           (mtime (or (plist-get stat :mtime) 0))
           (time (time-convert mtime 'list))
           (uid (or (plist-get stat :uid) 0))
           (gid (or (plist-get stat :gid) 0)))
      (list (cond ((and islink (stringp target) (not (string-empty-p target)))
                   target)
                  (isdir t)
                  (t nil))
            1                                   ; link count
            (if (eq id-format 'string) (number-to-string uid) uid)
            (if (eq id-format 'string) (number-to-string gid) gid)
            time time time                      ; atime, mtime, ctime
            (or (plist-get stat :size) 0)
            (scrutiny-agent-fs--mode-string mode isdir islink)
            t                                   ; gid would change on rename
            0 0))))                             ; inode, device

(defun scrutiny-agent-fs--cache-key (host directory)
  "Cache key for DIRECTORY on HOST.
Normalizes the path so `/foo/bar/.` and `/foo/bar` use the same key.
Also collapses double slashes: `//foo//bar` becomes `/foo/bar`."
  (let* ((expanded (expand-file-name directory "/"))
         ;; Collapse multiple slashes to single slash
         (collapsed (replace-regexp-in-string "/+" "/" expanded)))
    (concat host "\0" (directory-file-name collapsed))))

(defun scrutiny-agent-fs-flush-cache (&optional host directory)
  "Forget cached attributes, for DIRECTORY on HOST or for everything."
  (interactive)
  (if (and host directory)
      (remhash (scrutiny-agent-fs--cache-key host directory)
               scrutiny-agent-fs--dir-cache)
    (clrhash scrutiny-agent-fs--dir-cache)))

(defun scrutiny-agent-fs-forget-connections ()
  "Forget per-connection facts: the allowed roots and the remote home.

Deliberately NOT part of `scrutiny-agent-fs-flush-cache': those are
properties of the agent process, not of any directory, and re-reading
them costs a round trip. They change only when the connection does."
  (clrhash scrutiny-agent-fs--roots-cache))

(defun scrutiny-agent-fs--invalidate (host localname)
  "Drop everything cached about LOCALNAME: its directory, and itself."
  (let ((clean (directory-file-name localname)))
    (scrutiny-agent-fs-flush-cache host (file-name-directory clean))
    (scrutiny-agent-fs-flush-cache host clean)
    (remhash (concat "\0stat\0" host "\0" clean)
             scrutiny-agent-fs--dir-cache)))

(defun scrutiny-agent-fs--fetch-async (host directory)
  "Fetch DIRECTORY listing for HOST in the background.
Populates the cache so the next access is fast."
  (let ((key (scrutiny-agent-fs--cache-key host directory)))
    (unless (gethash key scrutiny-agent-fs--pending-fetches)
      (puthash key t scrutiny-agent-fs--pending-fetches)
      ;; Use run-at-time with 0 delay - fires on next event loop iteration
      (run-at-time
       0 nil
       (lambda ()
         (unwind-protect
             (let ((conn (scrutiny-agent-connection host)))
               (when conn
                 (condition-case nil
                     (let* ((result (scrutiny-agent-request
                                     conn "fs.listDirectory"
                                     (list :path (directory-file-name directory)
                                           :attributes t)
                                     60))
                            (entries
                             (mapcar (lambda (entry)
                                       (cons (plist-get entry :name)
                                             (plist-put (copy-sequence entry)
                                                        :exists t)))
                                     (scrutiny-agent-ops--list
                                      (plist-get result :entries)))))
                       (puthash key (cons (current-time) entries)
                                scrutiny-agent-fs--dir-cache)
                       (when scrutiny-agent-fs-verbose
                         (message "scrutiny-agent-fs[%s]: fetched %s (%d entries)"
                                  host directory (length entries))))
                   (error nil))))
           (remhash key scrutiny-agent-fs--pending-fetches)))))))

(defun scrutiny-agent-fs--listing (host directory)
  "Cached (NAME . STAT-PLIST) alist for DIRECTORY on HOST.

Returns the symbol `unavailable' when the directory could not be read
-- NOT nil, which is the correct answer for a directory that is simply
empty. Conflating the two makes every empty directory look like a
failure and silently fall back to TRAMP.

One `fs.listDirectory' with attributes answers every question Emacs is
about to ask about the files in it."
  (let* ((key (scrutiny-agent-fs--cache-key host directory))
         (hit (gethash key scrutiny-agent-fs--dir-cache)))
    (cond
     ;; Cache hit (fresh)
     ((and hit (> scrutiny-agent-fs-cache-ttl 0)
           (< (float-time (time-since (car hit)))
              scrutiny-agent-fs-cache-ttl))
      (cdr hit))
     ;; Interactive context - use stale cache or trigger background fetch
     ((scrutiny-agent-fs--interactive-p)
      (if hit
          (cdr hit)  ; Use stale data rather than blocking
        ;; No cache - trigger background fetch for next time
        (scrutiny-agent-fs--fetch-async host directory)
        (scrutiny-agent-fs--warn host "fetching %s in background" directory)
        'unavailable))
     ;; Fetch from agent
     (t
      (let ((conn (scrutiny-agent-fs--conn host)))
        (if (null conn)
            'unavailable
          (condition-case err
              (let* ((result (scrutiny-agent-request
                              conn "fs.listDirectory"
                              (list :path (directory-file-name directory)
                                    :attributes t)
                              60))
                     (entries
                      (mapcar (lambda (entry)
                                (cons (plist-get entry :name)
                                      (plist-put (copy-sequence entry)
                                                 :exists t)))
                              (scrutiny-agent-ops--list
                               (plist-get result :entries)))))
                (puthash key (cons (current-time) entries)
                         scrutiny-agent-fs--dir-cache)
                entries)
            ;; Cache the failure as well. A directory the agent will
            ;; not serve -- outside its roots, unreadable -- is asked
            ;; about once, not once per file whose attributes are
            ;; wanted. The TTL applies equally, so a root that becomes
            ;; readable is picked up shortly.
            (scrutiny-agent-rpc-error
             (scrutiny-agent-fs--record-error host 'listing err)
             (puthash key (cons (current-time) 'unavailable)
                      scrutiny-agent-fs--dir-cache)
             'unavailable)
            (error
             (scrutiny-agent-fs--record-error host 'listing err)
             'unavailable))))))))

(defun scrutiny-agent-fs--listing-ok (entries)
  "ENTRIES if the listing succeeded (possibly empty), else nil."
  (and (not (eq entries 'unavailable)) (or entries t)))

(defun scrutiny-agent-fs--stat (host localname)
  "STAT plist for LOCALNAME on HOST, from the directory cache if possible."
  (let* ((clean (directory-file-name localname))
         (dir (file-name-directory clean))
         (base (file-name-nondirectory clean)))
    (or (and dir (> scrutiny-agent-fs-cache-ttl 0)
             (not (string-empty-p base))
             ;; Only via the parent's listing when the parent is
             ;; somewhere the agent will actually look. The roots'
             ;; own parents never are -- and stat'ing a root is
             ;; exactly what `dired' does first.
             (scrutiny-agent-fs--in-roots-p host dir)
             (let ((entries (scrutiny-agent-fs--listing host dir)))
               (and (listp entries) (cdr (assoc base entries)))))
        (scrutiny-agent-fs--stat-uncached host clean))))

(defun scrutiny-agent-fs--stat-uncached (host path)
  "One `fs.stat' for PATH on HOST, memoized for the cache window.
Emacs asks about the same file several times inside a single command
-- `file-exists-p', then `file-attributes', then `file-readable-p' --
and without this each one is its own round trip."
  (let* ((key (concat "\0stat\0" host "\0" path))
         (hit (gethash key scrutiny-agent-fs--dir-cache)))
    (cond
     ;; Cache hit (fresh)
     ((and hit (> scrutiny-agent-fs-cache-ttl 0)
           (< (float-time (time-since (car hit)))
              scrutiny-agent-fs-cache-ttl))
      (cdr hit))
     ;; Interactive context - use stale cache if available
     ((scrutiny-agent-fs--interactive-p)
      (if hit
          (cdr hit)  ; Use stale data rather than blocking
        (scrutiny-agent-fs--warn host "no cached stat for %s during completion" path)
        nil))
     ;; Fetch from agent
     (t
      (let ((result
             (let ((conn (scrutiny-agent-fs--conn host)))
               (when conn
                 (condition-case err
                     (scrutiny-agent-request conn "fs.stat" (list :path path) 30)
                   (scrutiny-agent-rpc-error
                    (scrutiny-agent-fs--record-error host 'stat err)
                    nil)
                   (error
                    (scrutiny-agent-fs--record-error host 'stat err)
                    nil))))))
        (when (> scrutiny-agent-fs-cache-ttl 0)
          (puthash key (cons (current-time) result)
                   scrutiny-agent-fs--dir-cache))
        result)))))

;; ---------------------------------------------------------------------
;; Handler helpers
;; ---------------------------------------------------------------------

(defun scrutiny-agent-fs--dispatch (filename operation args handler)
  "Route OPERATION on FILENAME to HANDLER, or hand it back to TRAMP.

HANDLER is called with (HOST PREFIX LOCALNAME) when FILENAME names a
configured host, a connection is available, and the path is inside
the roots of that agent.  Otherwise OPERATION is delegated with ARGS,
or -- for a path outside the roots -- answered structurally.

The routing lives in a function and the body arrives as a function,
so both are ordinary values that a reader, the byte-compiler and a
static analyser can all follow.  A macro would read more briefly at
each of the call sites below, but nothing outside Emacs could see
through it."
  (let ((parsed (scrutiny-agent-fs--parse filename)))
    (if (not parsed)
        (progn
          (when scrutiny-agent-fs-verbose
            (message "dispatch[%s]: not parsed, delegating" operation))
          (scrutiny-agent-fs--delegate operation args))
      (let ((host (nth 0 parsed))
            (prefix (nth 1 parsed))
            (localname (nth 2 parsed)))
        (cond
         ((not (scrutiny-agent-fs--conn host))
          (when scrutiny-agent-fs-verbose
            (message "dispatch[%s]: no conn for %s, delegating" operation host))
          (scrutiny-agent-fs--delegate operation args))
         ((not (scrutiny-agent-fs--in-roots-p host localname))
          ;; Outside the roots. Either answer from structure (no
          ;; request, no connection) or hand it to TRAMP.
          (when scrutiny-agent-fs-verbose
            (message "dispatch[%s]: %s outside roots" operation localname))
          (if scrutiny-agent-fs-roots-are-the-world
              (scrutiny-agent-fs--outside-roots host localname operation)
            (scrutiny-agent-fs--delegate operation args)))
         (t (funcall handler host prefix localname)))))))

(defun scrutiny-agent-fs--outside-roots (host localname operation)
  "Answer OPERATION for LOCALNAME, which lies outside HOST's roots.
See `scrutiny-agent-fs-roots-are-the-world'."
  (let ((ancestor (scrutiny-agent-fs--ancestor-of-root-p host localname)))
    (pcase operation
      ((or 'file-exists-p 'file-directory-p 'file-readable-p) ancestor)
      ('file-regular-p nil)
      ('file-symlink-p nil)
      ('file-writable-p nil)
      ('file-modes (and ancestor #o555))
      ('file-truename (concat (or (file-remote-p localname) "")
                              (expand-file-name localname "/")))
      ('file-attributes
       (when ancestor
         ;; Enough for a caller to see a directory it may descend; the
         ;; agent never reported its size or times, so neither do we.
         (list t 1 0 0 nil nil nil 0 "dr-xr-xr-x" t 0 0)))
      ((or 'directory-files 'file-name-all-completions)
       (when ancestor
         (mapcar #'file-name-as-directory
                 (scrutiny-agent-fs--root-children host localname))))
      ('directory-files-and-attributes
       (when ancestor
         (mapcar (lambda (name)
                   (cons (file-name-as-directory name)
                         (list t 1 0 0 nil nil nil 0 "dr-xr-xr-x" t 0 0)))
                 (scrutiny-agent-fs--root-children host localname))))
      ('vc-registered nil)
      (_ nil))))

;; ---------------------------------------------------------------------
;; Predicates and attributes
;; ---------------------------------------------------------------------

(defun scrutiny-agent-fs--handle-file-attributes (filename &optional id-format)
  (scrutiny-agent-fs--dispatch
   filename 'file-attributes (list filename id-format)
   (lambda (host _prefix localname)
     (scrutiny-agent-fs--attributes (scrutiny-agent-fs--stat host localname)
                                    id-format))))

(defun scrutiny-agent-fs--handle-file-exists-p (filename)
  (scrutiny-agent-fs--dispatch
   filename 'file-exists-p (list filename)
   (lambda (host _prefix localname)
     (scrutiny-agent-ops-bool
      (plist-get (scrutiny-agent-fs--stat host localname) :exists)))))

(defun scrutiny-agent-fs--handle-file-directory-p (filename)
  (scrutiny-agent-fs--dispatch
   filename 'file-directory-p (list filename)
   (lambda (host _prefix localname)
     (scrutiny-agent-ops-bool
      (plist-get (scrutiny-agent-fs--stat host localname) :isDir)))))

(defun scrutiny-agent-fs--handle-file-regular-p (filename)
  (scrutiny-agent-fs--dispatch
   filename 'file-regular-p (list filename)
   (lambda (host _prefix localname)
     (scrutiny-agent-ops-bool
      (plist-get (scrutiny-agent-fs--stat host localname) :isRegular)))))

(defun scrutiny-agent-fs--handle-file-symlink-p (filename)
  (scrutiny-agent-fs--dispatch
   filename 'file-symlink-p (list filename)
   (lambda (host _prefix localname)
     (let ((stat (scrutiny-agent-fs--stat host localname)))
       (and (scrutiny-agent-ops-bool (plist-get stat :isSymlink))
            (let ((target (plist-get stat :symlinkTarget)))
              (and (stringp target) (not (string-empty-p target)) target)))))))

(defun scrutiny-agent-fs--handle-file-readable-p (filename)
  (scrutiny-agent-fs--dispatch
   filename 'file-readable-p (list filename)
   (lambda (host _prefix localname)
     (scrutiny-agent-ops-bool
      (plist-get (scrutiny-agent-fs--stat host localname) :readable)))))

(defun scrutiny-agent-fs--handle-file-writable-p (filename)
  (scrutiny-agent-fs--dispatch
   filename 'file-writable-p (list filename)
   (lambda (host _prefix localname)
     (let ((stat (scrutiny-agent-fs--stat host localname)))
       (if (scrutiny-agent-ops-bool (plist-get stat :exists))
           (scrutiny-agent-ops-bool (plist-get stat :writable))
         ;; A file that does not exist is writable if its directory is.
         (let ((parent (scrutiny-agent-fs--stat
			host (directory-file-name
                              (file-name-directory
                               (directory-file-name localname))))))
           (scrutiny-agent-ops-bool (plist-get parent :writable))))))))

(defun scrutiny-agent-fs--handle-file-modes (filename &optional _flag)
  (scrutiny-agent-fs--dispatch
   filename 'file-modes (list filename)
   (lambda (host _prefix localname)
     (plist-get (scrutiny-agent-fs--stat host localname) :mode))))

(defun scrutiny-agent-fs--handle-file-newer-than-file-p (file1 file2)
  (let ((t1 (nth 5 (file-attributes file1)))
        (t2 (nth 5 (file-attributes file2))))
    (cond ((null t1) nil)
          ((null t2) t)
          (t (time-less-p t2 t1)))))

(defun scrutiny-agent-fs--handle-file-truename (filename)
  (scrutiny-agent-fs--dispatch
   filename 'file-truename (list filename)
   (lambda (host prefix localname)
     (let ((stat (scrutiny-agent-fs--stat host localname)))
       (let ((real (plist-get stat :path)))
	 (if real
	     (concat prefix real)
           (scrutiny-agent-fs--delegate 'file-truename (list filename))))))))

;; ---------------------------------------------------------------------
;; Names
;; ---------------------------------------------------------------------

(defun scrutiny-agent-fs--handle-expand-file-name (name &optional directory)
  "Expand NAME, resolving a leading `~' without a TRAMP round trip."
  (let* ((target (if (file-name-absolute-p name) name
                   (concat (file-name-as-directory (or directory
                                                       default-directory))
                           name)))
         (parsed (scrutiny-agent-fs--parse target)))
    (if (not parsed)
        (scrutiny-agent-fs--delegate 'expand-file-name (list name directory))
      (let* ((host (nth 0 parsed))
             (prefix (nth 1 parsed))
             (localname (nth 2 parsed))
             (home (and (string-prefix-p "~" localname)
                        (scrutiny-agent-fs--home host))))
        (cond
         ;; `~' and `~/...' -- but not `~user', which we cannot resolve.
         ((and home (or (string= localname "~")
                        (string-prefix-p "~/" localname)))
          (concat prefix (expand-file-name
                          (concat (file-name-as-directory home)
                                  (substring localname
                                             (min 2 (length localname)))))))
         ((string-prefix-p "~" localname)
          (scrutiny-agent-fs--delegate 'expand-file-name
                                       (list name directory)))
         (t
          ;; Plain lexical expansion of an absolute remote path: no
          ;; connection needed at all.
          (concat prefix (expand-file-name localname "/"))))))))

;; ---------------------------------------------------------------------
;; Directories
;; ---------------------------------------------------------------------

(defun scrutiny-agent-fs--names (host directory full match nosort)
  "Directory entry names, honoring `directory-files' arguments."
  (let* ((entries (scrutiny-agent-fs--listing host directory))
         (names (append '("." "..")
                        (mapcar #'car (and (listp entries) entries)))))
    (when match
      (setq names (cl-remove-if-not (lambda (n) (string-match-p match n))
                                    names)))
    (when full
      (setq names (mapcar (lambda (n)
                            (concat (file-name-as-directory directory) n))
                          names)))
    (if nosort names (sort names #'string<))))

(defun scrutiny-agent-fs--handle-directory-files
    (directory &optional full match nosort &rest rest)
  (scrutiny-agent-fs--dispatch
   directory 'directory-files (append (list directory full match nosort) rest)
   (lambda (host prefix localname)
     (let ((entries (scrutiny-agent-fs--listing host localname)))
       (cond
        ((scrutiny-agent-fs--listing-ok entries)
         (mapcar (lambda (name) (if (and full (not (file-remote-p name)))
                                    (concat prefix name)
                                  name))
                 (scrutiny-agent-fs--names host localname full match nosort)))
        ;; Listing unavailable - return empty rather than falling back to TRAMP
        (t
         (scrutiny-agent-fs--warn host "directory-files failed for %s" localname)
         nil))))))

(defun scrutiny-agent-fs--handle-directory-files-and-attributes
    (directory &optional full match nosort id-format &rest rest)
  (scrutiny-agent-fs--dispatch
   directory 'directory-files-and-attributes (append (list directory full match nosort id-format) rest)
   (lambda (host prefix localname)
     (let ((entries (scrutiny-agent-fs--listing host localname)))
       (cond
        ((scrutiny-agent-fs--listing-ok entries)
         (mapcar
          (lambda (name)
            (let* ((base (file-name-nondirectory
                          (directory-file-name name)))
                   (stat (if (member base '("." ".."))
                             (scrutiny-agent-fs--stat
                              host (expand-file-name
                                    base (file-name-as-directory localname)))
                           (cdr (assoc base entries)))))
              (cons (if (and full (not (file-remote-p name)))
                        (concat prefix name)
                      name)
                    (scrutiny-agent-fs--attributes stat id-format))))
          (scrutiny-agent-fs--names host localname full match nosort)))
        ;; Listing unavailable - return empty rather than falling back to TRAMP
        (t
         (scrutiny-agent-fs--warn host "directory-files-and-attributes failed for %s" localname)
         nil))))))

(defun scrutiny-agent-fs--handle-file-name-all-completions (file directory)
  (scrutiny-agent-fs--dispatch
   directory 'file-name-all-completions (list file directory)
   (lambda (host _prefix localname)
     (let ((entries (scrutiny-agent-fs--listing host localname)))
       (cond
        ;; Listing succeeded (possibly empty)
        ((scrutiny-agent-fs--listing-ok entries)
         (let ((completions
                (delq nil
                      (mapcar (lambda (entry)
                                (let ((name (car entry)))
                                  (when (string-prefix-p file name)
                                    (if (scrutiny-agent-ops-bool
                                         (plist-get (cdr entry) :isDir))
                                        (file-name-as-directory name)
                                      name))))
                              entries))))
           ;; Add . and .. entries (directories always include these)
           (when (string-prefix-p file ".")
             (push "./" completions)
             (when (string-prefix-p file "..")
               (push "../" completions)))
           completions))
        ;; Listing unavailable - return empty rather than falling back to TRAMP
        (t
         (scrutiny-agent-fs--warn host "completions unavailable for %s" localname)
         nil))))))

(defun scrutiny-agent-fs--handle-file-name-completion
    (file directory &optional predicate)
  (let ((completions (file-name-all-completions file directory)))
    (if predicate
        (try-completion file (mapcar #'list completions) predicate)
      (try-completion file (mapcar #'list completions)))))

;; ---------------------------------------------------------------------
;; Reading
;; ---------------------------------------------------------------------

(defun scrutiny-agent-fs--fetch (host localname)
  "Raw bytes of LOCALNAME on HOST as a unibyte string, or nil."
  (let ((conn (scrutiny-agent-fs--conn host)))
    (when conn
      (condition-case nil
          (let ((result (scrutiny-agent-request
			 conn "fs.readFile"
			 (list :path localname :base64 t :stat t)
			 300)))
            (base64-decode-string (or (plist-get result :contentBase64) "")))
	(scrutiny-agent-rpc-error nil)
	(error nil)))))

(defun scrutiny-agent-fs--handle-file-local-copy (filename)
  (scrutiny-agent-fs--dispatch
   filename 'file-local-copy (list filename)
   (lambda (host _prefix localname)
     (let ((stat (scrutiny-agent-fs--stat host localname)))
       (cond
        ;; File doesn't exist
        ((not (scrutiny-agent-ops-bool (plist-get stat :exists)))
         (scrutiny-agent-fs--warn host "file-local-copy: %s does not exist" localname)
         nil)
        ;; File too large for inline transfer - must delegate
        ((> (or (plist-get stat :size) 0) scrutiny-agent-fs-max-inline-bytes)
         (scrutiny-agent-fs--warn host "file-local-copy: %s too large, delegating" localname)
         (scrutiny-agent-fs--delegate 'file-local-copy (list filename)))
        ;; Normal case - fetch via agent
        (t
         (let ((bytes (scrutiny-agent-fs--fetch host localname)))
           (if (null bytes)
               (progn
                 (scrutiny-agent-fs--warn host "file-local-copy: failed to fetch %s" localname)
                 nil)
             (let ((tmp (make-temp-file "scrutiny-agent-fs")))
               (let ((coding-system-for-write 'binary))
                 (with-temp-file tmp
                   (set-buffer-multibyte nil)
                   (insert bytes)))
               tmp)))))))))

(defun scrutiny-agent-fs--handle-insert-file-contents
    (filename &optional visit beg end replace)
  "Insert FILENAME's contents, decoding exactly as a local read would.

The bytes are fetched over the agent into a local temp file and then
handed to the ordinary local `insert-file-contents'.  That is what
gets coding-system detection, REPLACE and BEG/END right -- reproducing
those here would be a large and subtle way to corrupt buffers."
  (scrutiny-agent-fs--dispatch
   filename 'insert-file-contents (list filename visit beg end replace)
   (lambda (host _prefix localname)
     (let ((local (scrutiny-agent-fs--handle-file-local-copy filename)))
       (if (null local)
           ;; Failed to get file - signal error instead of delegating to TRAMP
           (progn
             (scrutiny-agent-fs--warn host "insert-file-contents: failed for %s" localname)
             (error "Cannot read file %s via scrutiny-agent" filename))
         (unwind-protect
             (let ((result (insert-file-contents local nil beg end replace)))
               (when visit
                 ;; `insert-file-contents' just pointed the buffer at the
                 ;; temp file; put it back on the remote name before
                 ;; anything (autosave, the modeline) can see it.
                 (setq buffer-file-name filename)
                 (setq buffer-read-only (not (file-writable-p filename)))
                 (set-visited-file-modtime
                  (or (nth 5 (file-attributes filename)) 0))
                 (set-buffer-modified-p nil))
               (list filename (cadr result)))
           (delete-file local)))))))

;; ---------------------------------------------------------------------
;; Writing
;; ---------------------------------------------------------------------

(defun scrutiny-agent-fs--handle-write-region
    (start end filename &optional append visit lockname mustbenew)
  "Write the region to FILENAME over the agent.

The region is encoded through a local temp file first, so the buffer's
coding system applies exactly as it would locally; the resulting bytes
are what get uploaded."
  (let ((args (list start end filename append visit lockname mustbenew)))
    (scrutiny-agent-fs--dispatch
     filename 'write-region args
     (lambda (host _prefix localname)
       (if (or append                            ; needs read-modify-write
               (not (scrutiny-agent-fs--writable-p host)))
           (scrutiny-agent-fs--delegate 'write-region args)
         (when (and mustbenew (file-exists-p filename)
                    (or (not (eq mustbenew 'excl))
			(error "File exists: %s" filename)))
           (unless (y-or-n-p (format "File %s exists; overwrite? " filename))
             (error "Canceled")))
         (let ((tmp (make-temp-file "scrutiny-agent-fs-out"))
               (bytes nil))
           (unwind-protect
               (progn
                 ;; Encode via the normal local path, then read the
                 ;; bytes back verbatim.
                 (scrutiny-agent-fs--delegate
                  'write-region (list start end tmp nil 'no-message))
                 (setq bytes (with-temp-buffer
                               (set-buffer-multibyte nil)
                               (let ((coding-system-for-read 'binary))
                                 (insert-file-contents-literally tmp))
                               (buffer-string))))
             (delete-file tmp))
           (let ((conn (scrutiny-agent-fs--conn host)))
             (condition-case err
                 (let ((result (scrutiny-agent-request
				conn "fs.writeFile"
				(list :path localname
                                      :contentBase64
                                      (base64-encode-string bytes t))
				300)))
                   (scrutiny-agent-fs--invalidate host localname)
                   (when visit
                     (setq buffer-file-name filename)
                     (set-visited-file-modtime
                      (time-convert (or (plist-get result :mtime) 0) 'list))
                     (set-buffer-modified-p nil))
                   (unless (or (eq visit t) (null visit) (stringp visit))
                     (message "Wrote %s" filename))
                   nil)
               (scrutiny-agent-rpc-error
		;; Never lose a save: if the agent refused, TRAMP still
		;; knows how to write the file.
		(scrutiny-agent--log conn "fs: write refused (%s), using TRAMP"
                                     (scrutiny-agent-ops-error-name
                                      (nth 1 err)))
		(scrutiny-agent-fs--delegate 'write-region args))))))))))

(defun scrutiny-agent-fs--write-op (operation args host localname thunk)
  "Run THUNK for a mutating OPERATION, or delegate when unavailable."
  (if (not (scrutiny-agent-fs--writable-p host))
      (scrutiny-agent-fs--delegate operation args)
    (condition-case nil
        (prog1 (funcall thunk)
          (scrutiny-agent-fs--invalidate host localname))
      (scrutiny-agent-rpc-error
       (scrutiny-agent-fs--delegate operation args)))))

(defun scrutiny-agent-fs--handle-make-directory (dir &optional parents)
  (let ((args (list dir parents)))
    (scrutiny-agent-fs--dispatch
     dir 'make-directory args
     (lambda (host _prefix localname)
       (scrutiny-agent-fs--write-op
	'make-directory args host localname
	(lambda ()
          (scrutiny-agent-request (scrutiny-agent-fs--conn host) "fs.mkdir"
                                  (list :path localname
					:parents (scrutiny-agent-ops-json-bool
                                                  parents))
                                  60)
          nil))))))

(defun scrutiny-agent-fs--handle-delete-file (filename &optional trash)
  (let ((args (list filename trash)))
    (scrutiny-agent-fs--dispatch
     filename 'delete-file args
     (lambda (host _prefix localname)
       (scrutiny-agent-fs--write-op
	'delete-file args host localname
	(lambda ()
          (scrutiny-agent-request (scrutiny-agent-fs--conn host) "fs.delete"
                                  (list :path localname) 60)
          nil))))))

(defun scrutiny-agent-fs--handle-delete-directory
    (directory &optional recursive trash)
  (let ((args (list directory recursive trash)))
    (scrutiny-agent-fs--dispatch
     directory 'delete-directory args
     (lambda (host _prefix localname)
       (scrutiny-agent-fs--write-op
	'delete-directory args host localname
	(lambda ()
          (scrutiny-agent-request (scrutiny-agent-fs--conn host) "fs.delete"
                                  (list :path localname
					:recursive
					(scrutiny-agent-ops-json-bool recursive))
                                  300)
          nil))))))

(defun scrutiny-agent-fs--handle-rename-file (file newname &optional ok-if-exists)
  (let ((args (list file newname ok-if-exists))
        (from (scrutiny-agent-fs--parse file))
        (to (scrutiny-agent-fs--parse newname)))
    ;; Both ends must be on the same host for the agent to do it in one
    ;; operation; a cross-host rename is a copy plus a delete, which
    ;; TRAMP already implements.
    (if (not (and from to (equal (nth 0 from) (nth 0 to))))
        (scrutiny-agent-fs--delegate 'rename-file args)
      (let ((host (nth 0 from)))
        (unless (or ok-if-exists (not (file-exists-p newname)))
          (error "File exists: %s" newname))
        (scrutiny-agent-fs--write-op
         'rename-file args host (nth 2 from)
         (lambda ()
           (scrutiny-agent-request (scrutiny-agent-fs--conn host) "fs.rename"
                                   (list :from (nth 2 from) :to (nth 2 to))
                                   120)
           (scrutiny-agent-fs--invalidate host (nth 2 to))
           nil))))))

(defun scrutiny-agent-fs--handle-copy-file
    (file newname &optional ok-if-exists keep-time preserve-uid preserve-perms)
  (let ((args (list file newname ok-if-exists keep-time preserve-uid
                    preserve-perms))
        (from (scrutiny-agent-fs--parse file))
        (to (scrutiny-agent-fs--parse newname)))
    (if (not (and from to (equal (nth 0 from) (nth 0 to))))
        (scrutiny-agent-fs--delegate 'copy-file args)
      (let ((host (nth 0 from)))
        (unless (or ok-if-exists (not (file-exists-p newname)))
          (error "File exists: %s" newname))
        (scrutiny-agent-fs--write-op
         'copy-file args host (nth 2 to)
         (lambda ()
           (scrutiny-agent-request (scrutiny-agent-fs--conn host) "fs.copy"
                                   (list :from (nth 2 from) :to (nth 2 to)
                                         :overwrite
                                         (scrutiny-agent-ops-json-bool
                                          ok-if-exists))
                                   300)
           nil))))))

(defun scrutiny-agent-fs--handle-set-file-modes (filename mode &optional flag)
  (let ((args (list filename mode flag)))
    (scrutiny-agent-fs--dispatch
     filename 'set-file-modes args
     (lambda (host _prefix localname)
       (scrutiny-agent-fs--write-op
	'set-file-modes args host localname
	(lambda ()
          (scrutiny-agent-request (scrutiny-agent-fs--conn host) "fs.chmod"
                                  (list :path localname :mode mode)
                                  60)
          nil))))))

;; ---------------------------------------------------------------------
;; Buffer bookkeeping
;; ---------------------------------------------------------------------

(defun scrutiny-agent-fs--handle-verify-visited-file-modtime (&optional buffer)
  (with-current-buffer (or buffer (current-buffer))
    (let ((filename buffer-file-name))
      (if (not (scrutiny-agent-fs--parse filename))
          (scrutiny-agent-fs--delegate 'verify-visited-file-modtime
                                       (list buffer))
        (let ((recorded (visited-file-modtime))
              (actual (nth 5 (file-attributes filename))))
          (cond ((null actual) t)               ; gone: nothing to compare
                ((and (consp recorded) (zerop (float-time recorded))) t)
                (t (time-equal-p recorded actual))))))))

(defun scrutiny-agent-fs--handle-vc-registered (_file)
  "Answer `nil' without asking the remote.

`vc' probes every file it opens, walking up the tree looking for
version-control state -- dozens of round trips per `find-file' over
TRAMP.  magit does not use `vc', and the agent's git surface is what
this setup uses for version control, so the probe is pure cost."
  nil)

(defun scrutiny-agent-fs--handle-unhandled-file-name-directory (_filename)
  temporary-file-directory)

;; ---------------------------------------------------------------------
;; Pure string operations
;; ---------------------------------------------------------------------
;;
;; These need no remote at all -- they are string surgery on the file
;; name. They are implemented here anyway because delegating them hands
;; TRAMP the first remote name it has seen, and some of its handlers
;; open a connection to answer: `tramp-handle-abbreviate-file-name'
;; looks up the remote home directory, which is a full connect. On a
;; Teleport link that is the entire cost of `C-x C-f', spent before a
;; single byte of the file has been asked for.

(defun scrutiny-agent-fs--handle-file-name-directory (filename)
  (let ((parsed (scrutiny-agent-fs--parse filename)))
    (if (not parsed)
        (scrutiny-agent-fs--delegate 'file-name-directory (list filename))
      (concat (nth 1 parsed)
              (or (file-name-directory (nth 2 parsed)) "/")))))

(defun scrutiny-agent-fs--handle-file-name-nondirectory (filename)
  (let ((parsed (scrutiny-agent-fs--parse filename)))
    (if (not parsed)
        (scrutiny-agent-fs--delegate 'file-name-nondirectory (list filename))
      (file-name-nondirectory (nth 2 parsed)))))

(defun scrutiny-agent-fs--handle-directory-file-name (directory)
  (let ((parsed (scrutiny-agent-fs--parse directory)))
    (if (not parsed)
        (scrutiny-agent-fs--delegate 'directory-file-name (list directory))
      (concat (nth 1 parsed) (directory-file-name (nth 2 parsed))))))

(defun scrutiny-agent-fs--handle-file-name-as-directory (filename)
  (let ((parsed (scrutiny-agent-fs--parse filename)))
    (if (not parsed)
        (scrutiny-agent-fs--delegate 'file-name-as-directory (list filename))
      (concat (nth 1 parsed) (file-name-as-directory (nth 2 parsed))))))

(defun scrutiny-agent-fs--handle-abbreviate-file-name (filename)
  "Abbreviate the remote home to a tilde, without connecting to find it.
The home comes from the agent (once per connection) or, if that is
not available, is simply left alone -- an unabbreviated name is
cosmetic, and far cheaper than a connect."
  (let ((parsed (scrutiny-agent-fs--parse filename)))
    (if (not parsed)
        (scrutiny-agent-fs--delegate 'abbreviate-file-name (list filename))
      (let* ((prefix (nth 1 parsed))
             (localname (nth 2 parsed))
             (home (scrutiny-agent-fs--home (nth 0 parsed))))
        (concat prefix
                (if (and home (not (string= home "/"))
                         (string-prefix-p (file-name-as-directory home)
                                          (file-name-as-directory localname)))
                    (concat "~" (substring localname (length home)))
                  localname))))))

(defun scrutiny-agent-fs--handle-file-remote-p
    (filename &optional identification connected)
  "Answer from this client's own connection state.

TRAMP would answer the same question by consulting -- and with
CONNECTED, potentially establishing -- its own connection, which is
precisely the connection this whole file exists to avoid needing."
  (let ((parsed (scrutiny-agent-fs--parse filename)))
    (if (not parsed)
        (scrutiny-agent-fs--delegate 'file-remote-p
                                     (list filename identification connected))
      (let ((prefix (nth 1 parsed))
            (localname (nth 2 parsed)))
        (when (or (not connected)
                  (scrutiny-agent-connection (nth 0 parsed)))
          (let ((vec (ignore-errors (tramp-dissect-file-name filename))))
            (pcase identification
              ('nil prefix)
              ('localname localname)
              ('host (and vec (tramp-file-name-host vec)))
              ('method (and vec (tramp-file-name-method vec)))
              ('user (and vec (tramp-file-name-user vec)))
              (_ (scrutiny-agent-fs--delegate
                  'file-remote-p
                  (list filename identification connected))))))))))

(defun scrutiny-agent-fs--handle-dired-uncache (directory)
  "Drop DIRECTORY's cached attributes; Dired calls this when reverting."
  (let ((parsed (scrutiny-agent-fs--parse directory)))
    (when parsed
      (scrutiny-agent-fs-flush-cache (nth 0 parsed) (nth 2 parsed)))))

;; The following four are implemented -- rather than delegated -- purely
;; to stop TRAMP running a remote command to answer them. Each round
;; trip costs the same as a real operation on a slow link, and these are
;; asked on every `find-file'.

(defun scrutiny-agent-fs--handle-file-name-case-insensitive-p (_filename)
  "Nil: the agent targets POSIX hosts, whose filesystems are case-sensitive."
  nil)

(defun scrutiny-agent-fs--handle-file-acl (_filename) nil)

(defun scrutiny-agent-fs--handle-file-selinux-context (_filename)
  (list nil nil nil nil))

(defun scrutiny-agent-fs--handle-file-ownership-preserved-p
    (_filename &optional _group)
  t)

(defun scrutiny-agent-fs--handle-insert-directory
    (file switches &optional wildcard full-directory-p)
  "Render a dired listing from the agent's attributes.

`ls-lisp' formats a listing entirely from `file-attributes' and
`directory-files-and-attributes' -- both of which this handler serves
-- so Dired is drawn from the single directory request we already
made, instead of TRAMP shelling out to `ls' on the far end."
  (let ((args (list file switches wildcard full-directory-p)))
    (scrutiny-agent-fs--dispatch
     file 'insert-directory args
     (lambda (host _prefix localname)
       (cond
        ;; Wildcards need shell globbing - must delegate
        (wildcard
         (scrutiny-agent-fs--delegate 'insert-directory args))
        ;; Listing unavailable - show empty rather than falling back to TRAMP
        ((not (scrutiny-agent-fs--listing-ok
               (scrutiny-agent-fs--listing
                host (directory-file-name localname))))
         (scrutiny-agent-fs--warn host "insert-directory: listing unavailable for %s" localname)
         ;; Insert a minimal "empty" listing so dired doesn't error
         (insert "  total 0\n"))
        ;; Normal case - render via ls-lisp
        (t
         (condition-case err
             (let* ((ls-lisp-use-insert-directory-program nil)
                    (text (if (fboundp 'ls-lisp--sanitize-switches)
                              (ls-lisp--sanitize-switches (or switches ""))
                            (or switches "")))
                    (chars (delete ?\s (delete ?- (append text nil)))))
               (ls-lisp-insert-directory
                file chars (ls-lisp-time-index chars) nil full-directory-p))
           (error
            (scrutiny-agent-fs--warn host "insert-directory error: %S" err)
            (insert "  total 0\n")))))))))

;; ---------------------------------------------------------------------
;; Dispatch
;; ---------------------------------------------------------------------

(defconst scrutiny-agent-fs-operations
  '((file-attributes . scrutiny-agent-fs--handle-file-attributes)
    (file-exists-p . scrutiny-agent-fs--handle-file-exists-p)
    (file-directory-p . scrutiny-agent-fs--handle-file-directory-p)
    (file-regular-p . scrutiny-agent-fs--handle-file-regular-p)
    (file-symlink-p . scrutiny-agent-fs--handle-file-symlink-p)
    (file-readable-p . scrutiny-agent-fs--handle-file-readable-p)
    (file-writable-p . scrutiny-agent-fs--handle-file-writable-p)
    (file-modes . scrutiny-agent-fs--handle-file-modes)
    (file-newer-than-file-p . scrutiny-agent-fs--handle-file-newer-than-file-p)
    (file-truename . scrutiny-agent-fs--handle-file-truename)
    (expand-file-name . scrutiny-agent-fs--handle-expand-file-name)
    (directory-files . scrutiny-agent-fs--handle-directory-files)
    (directory-files-and-attributes
     . scrutiny-agent-fs--handle-directory-files-and-attributes)
    (file-name-all-completions
     . scrutiny-agent-fs--handle-file-name-all-completions)
    (file-name-completion . scrutiny-agent-fs--handle-file-name-completion)
    (file-local-copy . scrutiny-agent-fs--handle-file-local-copy)
    (insert-file-contents . scrutiny-agent-fs--handle-insert-file-contents)
    (write-region . scrutiny-agent-fs--handle-write-region)
    (make-directory . scrutiny-agent-fs--handle-make-directory)
    (delete-file . scrutiny-agent-fs--handle-delete-file)
    (delete-directory . scrutiny-agent-fs--handle-delete-directory)
    (rename-file . scrutiny-agent-fs--handle-rename-file)
    (copy-file . scrutiny-agent-fs--handle-copy-file)
    (set-file-modes . scrutiny-agent-fs--handle-set-file-modes)
    (verify-visited-file-modtime
     . scrutiny-agent-fs--handle-verify-visited-file-modtime)
    (vc-registered . scrutiny-agent-fs--handle-vc-registered)
    (unhandled-file-name-directory
     . scrutiny-agent-fs--handle-unhandled-file-name-directory)
    (insert-directory . scrutiny-agent-fs--handle-insert-directory)
    (dired-uncache . scrutiny-agent-fs--handle-dired-uncache)
    (file-name-case-insensitive-p
     . scrutiny-agent-fs--handle-file-name-case-insensitive-p)
    (file-name-directory . scrutiny-agent-fs--handle-file-name-directory)
    (file-name-nondirectory
     . scrutiny-agent-fs--handle-file-name-nondirectory)
    (directory-file-name . scrutiny-agent-fs--handle-directory-file-name)
    (file-name-as-directory
     . scrutiny-agent-fs--handle-file-name-as-directory)
    (abbreviate-file-name . scrutiny-agent-fs--handle-abbreviate-file-name)
    (file-remote-p . scrutiny-agent-fs--handle-file-remote-p)
    (file-acl . scrutiny-agent-fs--handle-file-acl)
    (file-selinux-context . scrutiny-agent-fs--handle-file-selinux-context)
    (file-ownership-preserved-p
     . scrutiny-agent-fs--handle-file-ownership-preserved-p))
  "Operations this handler serves over the agent.
Anything absent is delegated, so the set can grow without risk.")

;;;###autoload
(defun scrutiny-agent-fs-handler (operation &rest args)
  "`file-name-handler-alist' entry: serve OPERATION over the agent.
Unimplemented operations, unconfigured hosts, and any failure are
handed back to the next handler -- TRAMP -- unchanged."
  (let ((implementation (cdr (assq operation
                                   scrutiny-agent-fs-operations))))
    (if implementation
	(apply implementation args)
      (scrutiny-agent-fs--delegate operation args))))

(defun scrutiny-agent-fs--file-name-regexp ()
  "Regexp matching remote names on configured hosts, and nothing else.
Deliberately narrow: anything it does not match never reaches this
handler at all, so an unconfigured host cannot be affected by a bug
in here."
  (if (null scrutiny-agent-hosts)
      "\\`\\'"                          ; matches nothing
    (concat "\\`/[^:/|]+:"
            (regexp-opt (append (mapcar #'car scrutiny-agent-hosts)
                                (mapcar #'car scrutiny-agent-fs-host-alist)))
            ":")))

(defvar scrutiny-agent-fs-mode)  ; defined by define-minor-mode below

(defun scrutiny-agent-fs--on-connect (host)
  "Hook function to warm the cache after connecting to HOST."
  (when scrutiny-agent-fs-mode
    (scrutiny-agent-fs-warm-cache host)))

;;;###autoload
(define-minor-mode scrutiny-agent-fs-mode
  "Serve remote file operations over scrutiny-agent instead of TRAMP.

Applies only to hosts configured in `scrutiny-agent-hosts'.  Reads are
accelerated always; writes need the remote agent started with
`--allow-write' and otherwise fall back to TRAMP.  Turning the mode
off restores stock behavior completely."
  :global t
  :group 'scrutiny-agent-fs
  (setq file-name-handler-alist
        (cl-remove-if (lambda (entry)
                        (eq (cdr entry) 'scrutiny-agent-fs-handler))
                      file-name-handler-alist))
  (scrutiny-agent-fs-flush-cache)
  (scrutiny-agent-fs-forget-connections)
  (clrhash scrutiny-agent-fs--connect-hints)
  ;; Remove our hook first (idempotent)
  (remove-hook 'scrutiny-agent-after-connect-hook #'scrutiny-agent-fs--on-connect)
  (if scrutiny-agent-fs-mode
      (progn
        ;; Load TRAMP FIRST. It registers its handler with `add-to-list',
        ;; which prepends -- so if it loads after us (and it would, lazily,
        ;; the first time anything parses a remote name) it lands ahead of
        ;; our entry and serves every operation itself. The accelerator
        ;; would then be installed, matching, and completely inert.
        (require 'tramp)
        (push (cons (scrutiny-agent-fs--file-name-regexp)
                    'scrutiny-agent-fs-handler)
              file-name-handler-alist)
        ;; Auto-warm cache on connect
        (add-hook 'scrutiny-agent-after-connect-hook #'scrutiny-agent-fs--on-connect)
        (unless (scrutiny-agent-fs-active-p)
          (message "scrutiny-agent-fs: another handler precedes ours; \
run M-x scrutiny-agent-fs-refresh after loading it")))
    (message "scrutiny-agent-fs-mode disabled")))

(defun scrutiny-agent-fs-active-p ()
  "Non-nil when our handler would actually be reached for our hosts.
A handler registered behind TRAMP's is never consulted, so this is
the difference between the mode being on and the mode working."
  (let ((ours (cl-position 'scrutiny-agent-fs-handler file-name-handler-alist
                           :key #'cdr))
        (tramp (cl-position 'tramp-file-name-handler file-name-handler-alist
                            :key #'cdr)))
    (and ours (or (null tramp) (< ours tramp)))))

;;;###autoload
(defun scrutiny-agent-fs-refresh ()
  "Re-read `scrutiny-agent-hosts' into the handler's regexp."
  (interactive)
  (when scrutiny-agent-fs-mode
    (scrutiny-agent-fs-mode -1)
    (scrutiny-agent-fs-mode 1)))

;; ---------------------------------------------------------------------
;; Benchmark
;; ---------------------------------------------------------------------

(defun scrutiny-agent-fs--measure (label thunk)
  "Time THUNK both ways, returning (LABEL AGENT TRAMP) in seconds.
A top-level function rather than a `cl-flet': a local one is opaque
to the byte-compiler's callers and to static analysis."
  (scrutiny-agent-fs-flush-cache)
  (let ((agent (let ((scrutiny-agent-fs-mode t))
                 (scrutiny-agent-fs--time thunk)))
        (tramp (let ((file-name-handler-alist
                      (cl-remove-if
                       (lambda (e)
                         (eq (cdr e) 'scrutiny-agent-fs-handler))
                       file-name-handler-alist)))
                 (scrutiny-agent-fs--time thunk))))
    (list label agent tramp)))

(defun scrutiny-agent-fs--time (thunk)
  "Seconds THUNK takes, or nil if it fails."
  (let ((start (float-time)))
    (condition-case nil (progn (funcall thunk) (- (float-time) start))
      (error nil))))

;;;###autoload
(defun scrutiny-agent-fs-benchmark (directory)
  "Time common file operations in DIRECTORY, agent versus TRAMP.

This is the honest answer to \"is this actually helping?\" -- it runs
each operation both ways against the same remote directory and shows
the ratio.  Read-only: it lists, stats and reads, and never writes."
  (interactive "sRemote directory (e.g. /ssh:devbox:~/src/proj): ")
  (unless (scrutiny-agent-fs--parse directory)
    (user-error "%s is not on a configured scrutiny-agent host" directory))
  (let* ((results nil)
         (probe-file nil))
    (unless scrutiny-agent-fs-mode
      (user-error "Enable scrutiny-agent-fs-mode first"))
    (dolist (probe `(("directory-files"
                      ,(lambda () (directory-files directory)))
                     ("directory-files-and-attributes"
                      ,(lambda ()
                         (directory-files-and-attributes directory)))
                     ("file-attributes"
                      ,(lambda () (file-attributes directory)))
                     ("file-exists-p"
                      ,(lambda () (file-exists-p directory)))
                     ("completion"
                      ,(lambda ()
                         (file-name-all-completions "" directory)))))
      (push (scrutiny-agent-fs--measure (car probe) (cadr probe)) results))
    (setq probe-file
          (car (cl-remove-if
                (lambda (f) (or (file-directory-p f)
                                (> (or (file-attribute-size
                                        (file-attributes f)) 0)
                                   1000000)))
                (directory-files directory t "\\`[^.]"))))
    (when probe-file
      (push (scrutiny-agent-fs--measure
             "insert-file-contents"
             (lambda () (with-temp-buffer
                          (insert-file-contents probe-file))))
            results))
    (with-current-buffer (get-buffer-create "*scrutiny-agent-fs-benchmark*")
      (let ((inhibit-read-only t))
        (erase-buffer)
        (insert (format "%s\n\n" directory))
        (insert (format "%-32s %10s %10s %9s\n"
                        "operation" "agent" "tramp" "speedup"))
        (insert (make-string 64 ?-) "\n")
        (dolist (row (nreverse results))
          (cl-destructuring-bind (label agent tramp) row
            (insert (format "%-32s %9s %10s %9s\n" label
                            (if agent (format "%.0fms" (* 1000 agent)) "n/a")
                            (if tramp (format "%.0fms" (* 1000 tramp)) "n/a")
                            (if (and agent tramp (> agent 0))
                                (format "%.1fx" (/ tramp agent))
                              "n/a")))))
        (insert "\nRead-only; the agent column includes cache misses.\n"))
      (special-mode)
      (goto-char (point-min))
      (display-buffer (current-buffer)))))

;; ---------------------------------------------------------------------
;; Cache warm-up
;; ---------------------------------------------------------------------

;;;###autoload
(defun scrutiny-agent-fs-warm-cache (host)
  "Pre-populate the cache for HOST by fetching roots and listing them.
Run this after connecting to make subsequent completions fast.
Subdirectories are cached incrementally on demand via background fetches."
  (interactive (list (completing-read "Host: "
                                       (mapcar #'car scrutiny-agent-hosts))))
  (let ((conn (scrutiny-agent-connection host)))
    (unless conn
      (user-error "Not connected to %s (use C-c s c first)" host))
    (message "Warming cache for %s..." host)
    (condition-case err
        (let* ((caps (scrutiny-agent-ops-capabilities conn))
               (fsa (plist-get caps :fileSystemAccess))
               (roots (scrutiny-agent-ops--list (plist-get fsa :allowedRoots)))
               (home (plist-get fsa :home)))
          ;; Store facts in cache
          (puthash host
                   (cons roots (and (stringp home)
                                    (not (string-empty-p home))
                                    (directory-file-name home)))
                   scrutiny-agent-fs--roots-cache)
          ;; List the root directories
          (let ((count 0))
            (dolist (root roots)
              (message "Listing %s..." root)
              (condition-case nil
                  (let* ((result (scrutiny-agent-request
                                  conn "fs.listDirectory"
                                  (list :path (directory-file-name root) :attributes t)
                                  60))
                         (entries (mapcar (lambda (entry)
                                            (cons (plist-get entry :name)
                                                  (plist-put (copy-sequence entry) :exists t)))
                                          (scrutiny-agent-ops--list
                                           (plist-get result :entries)))))
                    (puthash (scrutiny-agent-fs--cache-key host root)
                             (cons (current-time) entries)
                             scrutiny-agent-fs--dir-cache)
                    (cl-incf count))
                (error nil)))
            (message "Cache warmed for %s: %d directories, roots=%S" host count roots)))
      (error
       (message "Failed to warm cache for %s: %S" host err)))))

;; ---------------------------------------------------------------------
;; Diagnostics
;; ---------------------------------------------------------------------

;;;###autoload
(defun scrutiny-agent-fs-status ()
  "Show the status of scrutiny-agent-fs-mode for all configured hosts.
Displays connection state, roots, home directory, and any recent errors."
  (interactive)
  (with-current-buffer (get-buffer-create "*scrutiny-agent-fs-status*")
    (let ((inhibit-read-only t))
      (erase-buffer)
      (insert "scrutiny-agent-fs-mode status\n")
      (insert (make-string 60 ?=) "\n\n")
      (insert (format "Mode enabled: %s\n" (if scrutiny-agent-fs-mode "yes" "NO")))
      (insert (format "Handler active: %s\n"
                      (if (scrutiny-agent-fs-active-p) "yes"
                        "NO (another handler precedes ours)")))
      (insert (format "Verbose logging: %s\n\n"
                      (if scrutiny-agent-fs-verbose
                          (if (stringp scrutiny-agent-fs-verbose)
                              (format "to buffer %s" scrutiny-agent-fs-verbose)
                            "to *Messages*")
                        "off")))
      (if (null scrutiny-agent-hosts)
          (insert "No hosts configured in `scrutiny-agent-hosts'.\n")
        (insert (format "Configured hosts: %d\n\n"
                        (length scrutiny-agent-hosts)))
        (dolist (entry scrutiny-agent-hosts)
          (let* ((name (car entry))
                 (conn (scrutiny-agent-connection name))
                 (facts (gethash name scrutiny-agent-fs--roots-cache 'miss)))
            (insert (format "Host: %s\n" name))
            (insert (format "  Connection: %s\n"
                            (if conn "live" "not connected")))
            (if (eq facts 'miss)
                (insert "  Roots: (not yet queried)\n")
              (if (null facts)
                  (insert "  Roots: FAILED to fetch capabilities\n")
                (insert (format "  Roots: %S\n" (car facts)))
                (insert (format "  Home: %s\n" (or (cdr facts) "(none)")))))
            (insert "\n"))))
      (when scrutiny-agent-fs--last-error
        (insert (make-string 60 ?-) "\n")
        (insert "Last error:\n")
        (insert (format "  Host: %s\n" (plist-get scrutiny-agent-fs--last-error :host)))
        (insert (format "  Operation: %s\n" (plist-get scrutiny-agent-fs--last-error :operation)))
        (insert (format "  Error: %S\n" (plist-get scrutiny-agent-fs--last-error :error)))
        (insert (format "  Time: %s\n"
                        (format-time-string "%Y-%m-%d %H:%M:%S"
                                            (plist-get scrutiny-agent-fs--last-error :time)))))
      (insert "\n")
      (insert "Commands:\n")
      (insert "  M-x scrutiny-agent-fs-mode      Toggle the mode\n")
      (insert "  M-x scrutiny-agent-fs-refresh   Re-read host config\n")
      (insert "  M-x scrutiny-agent-fs-flush-cache  Clear directory cache\n")
      (insert "  (setq scrutiny-agent-fs-verbose t)  Enable logging\n"))
    (special-mode)
    (goto-char (point-min))
    (display-buffer (current-buffer))))

;;;###autoload
(defun scrutiny-agent-fs-last-error ()
  "Display the last error encountered by scrutiny-agent-fs-mode."
  (interactive)
  (if (null scrutiny-agent-fs--last-error)
      (message "No errors recorded by scrutiny-agent-fs-mode.")
    (message "scrutiny-agent-fs[%s] %s: %S"
             (or (plist-get scrutiny-agent-fs--last-error :host) "?")
             (plist-get scrutiny-agent-fs--last-error :operation)
             (plist-get scrutiny-agent-fs--last-error :error))))

(provide 'scrutiny-agent-fs)
;;; scrutiny-agent-fs.el ends here
