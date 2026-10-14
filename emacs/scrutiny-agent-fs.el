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
  "HOST+DIR -> (TIMESTAMP . LISTING).
LISTING is an alist ((NAME . ATTRS-PLIST) ...), or one of two symbols:
`unavailable' (the directory could not be listed) and `too-large' (it
holds more entries than `scrutiny-agent-fs-listing-max-entries', so
its files are asked about one at a time instead).")

(defvar scrutiny-agent-fs--self (make-hash-table :test #'equal)
  "HOST+DIR -> the directory's own attributes, as of its listing.")

(defvar scrutiny-agent-fs--watched (make-hash-table :test #'equal)
  "HOST+DIR -> canonical remote path, for listings the agent keeps current.
A listing in here has no expiry: the agent is watching the directory
and pushes every change to it (`fs.changed'), so the copy held in
`scrutiny-agent-fs--dir-cache' is true for as long as the connection
is up.  See `scrutiny-agent-fs-watch'.")

(defvar scrutiny-agent-fs--canonical (make-hash-table :test #'equal)
  "HOST+CANONICAL-PATH -> the cache keys that path's listing is filed under.
A directory reached through a symlink is cached under the name it was
asked for, while the agent reports changes by canonical path.")

(defvar scrutiny-agent-fs--inflight (make-hash-table :test #'equal)
  "HOST+DIR -> (FUTURE . FULL) for a listing sent and not yet answered.")

(defvar scrutiny-agent-fs--bodies (make-hash-table :test #'equal)
  "HOST+PATH -> (TIMESTAMP . FUTURE): file bodies asked for ahead of the read.")

(defvar scrutiny-agent-fs--changes nil
  "Recent `fs.changed' notifications, newest first: (SEQ HOST . PARAMS).
Kept so that a change which overtakes the listing reply it belongs
with is not lost; see `scrutiny-agent-fs--install-listing'.")

(defvar scrutiny-agent-fs--change-seq 0
  "Number of `fs.changed' notifications received so far.")

(defvar scrutiny-agent-fs--roots-cache (make-hash-table :test #'equal)
  "HOST -> (ROOTS . HOME), the connection facts. Fixed per connection.")

(defcustom scrutiny-agent-fs-watch t
  "Whether to have the agent keep cached directory listings current.

With this on (and an agent that supports it), every directory this
client lists is watched on the far end, and changes are pushed as
they happen.  A cached listing then never expires, so the questions
Emacs asks constantly -- does this file exist, has it changed on
disk, what is in this directory -- are answered from memory for as
long as the connection is up, instead of costing a round trip each
time `scrutiny-agent-fs-cache-ttl' runs out.

The agent declines to watch where it cannot see every change:
network filesystems, where another machine's writes are invisible to
it, and directories that change continuously.  Those fall back to
the time-based cache automatically.  Set to nil to use the
time-based cache everywhere."
  :type 'boolean)

(defcustom scrutiny-agent-fs-listing-max-entries 4000
  "Largest directory fetched whole just to answer a question about one file.

Asking the agent for a directory's listing answers everything Emacs
is about to ask about the files in it, which is nearly always a
bargain.  It stops being one for a directory with tens of thousands
of entries; past this size, files there are asked about one at a
time.  Listing such a directory deliberately (Dired, completion)
still fetches all of it."
  :type 'integer)

(defcustom scrutiny-agent-fs-prefetch t
  "Whether `find-file' asks for a file's contents before it needs them.

Opening a file asks about the file, then about every directory above
it (for `.dir-locals.el', the project root, version control), then
reads it.  With this on, all of those requests are sent together the
moment `find-file-noselect' is entered, so the whole open costs one
round trip.  It is implemented as advice on `find-file-noselect'
that sends requests and does nothing else."
  :type 'boolean)

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
  "Non-nil where a wait must give way to the user's next keystroke.
That is minibuffer completion, mostly: a listing that takes a second
to arrive must not hold up the characters typed in the meantime."
  (or (minibufferp)
      (window-minibuffer-p)
      (minibuffer-window-active-p (minibuffer-window))
      (bound-and-true-p company-candidates)))

(defun scrutiny-agent-fs--facts-from (fsa)
  "(ROOTS . HOME) from a `fileSystemAccess' plist FSA.

ROOTS holds every spelling the agent reports: the canonical ones it
enforces with, and -- `allowedRootsAsGiven', from a current agent --
the ones it was configured with where a symlink made realpath change
them.  Both name the same directories, and a user writes file names
the way the root was configured (\"/home/dev/src\" when /home is a
symlink), so a check against the canonical spelling alone would call
the whole sandbox \"outside the roots\" and answer it from structure."
  (let ((home (plist-get fsa :home)))
    (cons (delete-dups
           (append (scrutiny-agent-ops--list (plist-get fsa :allowedRoots))
                   (scrutiny-agent-ops--list
                    (plist-get fsa :allowedRootsAsGiven))))
          (and (stringp home) (not (string-empty-p home))
               (directory-file-name home)))))

(defun scrutiny-agent-fs--facts (host)
  "The agent's roots and remote home for HOST, as (ROOTS . HOME).

Both are fixed for the life of a connection.  A current agent states
them in its `meta.hello' reply, so they cost nothing; an older one is
asked once (`meta.capabilities'), and the failure is cached too --
retrying a lookup that cannot succeed puts a round trip in front of
every single file operation."
  (let ((cached (gethash host scrutiny-agent-fs--roots-cache 'miss)))
    (cond
     ((not (eq cached 'miss)) cached)
     (t
      (let* ((conn (scrutiny-agent-fs--conn host))
             (told (and conn (scrutiny-agent--conn-facts conn))))
        (cond
         ((null conn)
          (scrutiny-agent-fs--warn host "no connection, roots unknown")
          nil)
         (told
          (puthash host (scrutiny-agent-fs--facts-from told)
                   scrutiny-agent-fs--roots-cache))
         ;; An older agent, asked from where the user is typing: do
         ;; not block the minibuffer for it.
         ((scrutiny-agent-fs--interactive-p) nil)
         (t
          (puthash
           host
           (condition-case err
               (let ((facts (scrutiny-agent-fs--facts-from
                             (plist-get (scrutiny-agent-ops-capabilities conn)
                                        :fileSystemAccess))))
                 (when (null (car facts))
                   (scrutiny-agent-fs--warn
                    host "no allowedRoots in capabilities"))
                 facts)
             (error
              (scrutiny-agent-fs--record-error host 'capabilities err)
              nil))
           scrutiny-agent-fs--roots-cache))))))))

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

(defvar scrutiny-agent-fs-delegations nil
  "Alist of (OPERATION . COUNT): what has been handed to TRAMP so far.
Only calls naming a file on a configured host are counted.  An entry
here is not necessarily slow -- TRAMP answers many operations without
touching the network -- but when something IS slow, this is the list
of suspects, and `M-x scrutiny-agent-fs-status' prints it.")

(defun scrutiny-agent-fs--delegate (operation args)
  "Run OPERATION with ARGS as if this handler were not installed.
The standard idiom: inhibit ourselves for exactly this call, so the
next handler in `file-name-handler-alist' -- TRAMP -- takes it."
  (when (or (and (stringp (car args))
                 (scrutiny-agent-fs--parse (car args)))
            (and (memq operation '(process-file start-file-process
                                   make-process shell-command))
                 (scrutiny-agent-fs--parse default-directory)))
    (cl-incf (alist-get operation scrutiny-agent-fs-delegations 0))
    (when scrutiny-agent-fs-verbose
      (scrutiny-agent-fs--warn nil "to TRAMP: %s %S" operation (car args))))
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

(defun scrutiny-agent-fs--mtime (stat)
  "The modification time in STAT as a Lisp timestamp, or nil.
Nanoseconds when the agent reports them: two saves inside one second
must not look like the same file."
  (let ((ns (plist-get stat :mtimeNs))
        (seconds (plist-get stat :mtime)))
    (cond ((and (integerp ns) (> ns 0))
           (time-convert (cons ns 1000000000) 'list))
          ((numberp seconds) (time-convert seconds 'list)))))

(defun scrutiny-agent-fs--time-ns (time)
  "Lisp timestamp TIME as integer nanoseconds since the epoch."
  (car (time-convert time 1000000000)))

(defun scrutiny-agent-fs--attributes (stat &optional id-format)
  "Emacs `file-attributes' list for a protocol STAT plist, or nil."
  (when (scrutiny-agent-ops-bool (plist-get stat :exists))
    (let* ((isdir (scrutiny-agent-ops-bool (plist-get stat :isDir)))
           (islink (scrutiny-agent-ops-bool (plist-get stat :isSymlink)))
           (target (plist-get stat :symlinkTarget))
           (mode (or (plist-get stat :mode) 0))
           (time (or (scrutiny-agent-fs--mtime stat)
                     (time-convert 0 'list)))
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

(defun scrutiny-agent-fs--forget-key (key)
  "Drop the listing filed under KEY, and the agent's vouching for it."
  (remhash key scrutiny-agent-fs--dir-cache)
  (remhash key scrutiny-agent-fs--self)
  (remhash key scrutiny-agent-fs--watched))

(defun scrutiny-agent-fs-flush-cache (&optional host directory)
  "Forget cached attributes, for DIRECTORY on HOST or for everything."
  (interactive)
  (if (and host directory)
      (scrutiny-agent-fs--forget-key
       (scrutiny-agent-fs--cache-key host directory))
    (clrhash scrutiny-agent-fs--dir-cache)
    (clrhash scrutiny-agent-fs--self)
    (clrhash scrutiny-agent-fs--watched)
    (clrhash scrutiny-agent-fs--canonical)
    (clrhash scrutiny-agent-fs--bodies)))

(defun scrutiny-agent-fs--forget-host (host)
  "Forget everything held on the strength of HOST's connection.
A watched listing is only as good as the connection that carries the
watch; when that goes, so does every copy it vouched for."
  (let ((prefix (concat host "\0")))
    (dolist (table (list scrutiny-agent-fs--dir-cache
                         scrutiny-agent-fs--self
                         scrutiny-agent-fs--watched
                         scrutiny-agent-fs--canonical
                         scrutiny-agent-fs--inflight
                         scrutiny-agent-fs--bodies))
      (maphash (lambda (key _value)
                 (when (or (string-prefix-p prefix key)
                           (string-prefix-p (concat "\0stat\0" prefix) key))
                   (remhash key table)))
               table)))
  (remhash host scrutiny-agent-fs--roots-cache))

(defun scrutiny-agent-fs--forget-subtree (host path)
  "Drop every listing at or below PATH on HOST.
Called when PATH stops being the directory it was: what is cached
beneath it describes something that is no longer there."
  (let* ((clean (directory-file-name path))
         (exact (concat host "\0" clean))
         (below (concat exact "/")))
    (maphash (lambda (key _value)
               (when (or (string= key exact) (string-prefix-p below key))
                 (scrutiny-agent-fs--forget-key key)))
             scrutiny-agent-fs--dir-cache)))

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
    (scrutiny-agent-fs--forget-subtree host clean)
    (remhash (concat "\0stat\0" host "\0" clean)
             scrutiny-agent-fs--dir-cache)))

(defun scrutiny-agent-fs--file-entry (host localname entry)
  "File ENTRY as what the cache knows about LOCALNAME on HOST.
ENTRY is a directory entry as the agent reports one, or nil when the
name no longer exists.  Returns non-nil if a cached listing was
brought up to date -- if there was none, there was nothing to do,
and the next question about this directory fetches a current one."
  (let* ((clean (directory-file-name localname))
         (key (scrutiny-agent-fs--cache-key host (file-name-directory clean)))
         (hit (gethash key scrutiny-agent-fs--dir-cache))
         (base (file-name-nondirectory clean)))
    (remhash (concat "\0stat\0" host "\0" clean) scrutiny-agent-fs--dir-cache)
    (when (and hit (listp (cdr hit)))
      (let ((old (assoc base (cdr hit))))
        (when (and old
                   (or (null entry)
                       (not (eq (scrutiny-agent-ops-bool
                                 (plist-get (cdr old) :isDir))
                                (scrutiny-agent-ops-bool
                                 (plist-get entry :isDir))))))
          (scrutiny-agent-fs--forget-subtree host clean))
        (cond
         ((null entry) (setcdr hit (delq old (cdr hit))))
         (old (setcdr old (plist-put (copy-sequence entry) :exists t)))
         (t (setcdr hit (cons (cons base (plist-put (copy-sequence entry)
                                                    :exists t))
                              (cdr hit))))))
      t)))

;; ---------------------------------------------------------------------
;; Changes pushed by the agent
;; ---------------------------------------------------------------------

(defun scrutiny-agent-fs--apply-change (host params)
  "Bring the cache up to date with one `fs.changed' notification.
PARAMS names a directory by canonical path and carries the new state
of each name in it that changed -- attributes, or `deleted'.  State,
not an instruction to re-read: applying one twice, or one the cached
listing already reflects, changes nothing."
  (if (eq (plist-get params :all) t)
      ;; The agent lost track (its event queue overflowed).  Nothing
      ;; held for this host can be vouched for; the watches stand, so
      ;; whatever is re-read is current again.
      (let ((prefix (concat host "\0")))
        (maphash (lambda (key _value)
                   (when (string-prefix-p prefix key)
                     (remhash key scrutiny-agent-fs--dir-cache)
                     (remhash key scrutiny-agent-fs--self)))
                 scrutiny-agent-fs--dir-cache))
    (let* ((canonical (plist-get params :path))
           (ckey (concat host "\0" (or canonical "")))
           (reset (eq (plist-get params :reset) t))
           (unwatched (eq (plist-get params :unwatched) t)))
      (dolist (key (gethash ckey scrutiny-agent-fs--canonical))
        (let ((directory (substring key (1+ (length host)))))
          (cond
           (reset
            (remhash key scrutiny-agent-fs--dir-cache)
            (remhash key scrutiny-agent-fs--self)
            (when unwatched (remhash key scrutiny-agent-fs--watched)))
           ((gethash key scrutiny-agent-fs--watched)
            (dolist (entry (scrutiny-agent-ops--list
                            (plist-get params :entries)))
              (scrutiny-agent-fs--file-entry
               host
               (concat (file-name-as-directory directory)
                       (plist-get entry :name))
               (and (not (eq (plist-get entry :deleted) t)) entry)))
            (let ((self (plist-get params :self)))
              (when (and self (gethash key scrutiny-agent-fs--self))
                (puthash key (plist-put (copy-sequence self) :path canonical)
                         scrutiny-agent-fs--self)))))))
      (when unwatched (remhash ckey scrutiny-agent-fs--canonical)))))

(defun scrutiny-agent-fs--on-changed (conn params)
  "Notification handler for `fs.changed' on CONN."
  (let ((host (scrutiny-agent--conn-name conn)))
    (push (cons (cl-incf scrutiny-agent-fs--change-seq) (cons host params))
          scrutiny-agent-fs--changes)
    (let ((tail (nthcdr 127 scrutiny-agent-fs--changes)))
      (when tail (setcdr tail nil)))
    (scrutiny-agent-fs--apply-change host params)))

;; ---------------------------------------------------------------------
;; Listings
;; ---------------------------------------------------------------------
;;
;; Everything Emacs asks about a file -- does it exist, is it a
;; directory, when did it change -- is answered from the listing of the
;; directory it is in.  Three things make that cheap on a slow link:
;;
;;   * Requests are sent in bursts.  The first question about a path
;;     asks for its directory AND every directory above it at once,
;;     because `locate-dominating-file' is about to walk all of them
;;     (for `.git', `.dir-locals.el', the project root).  A burst costs
;;     one round trip however many requests are in it.
;;   * A listing proves absence.  Most of what Emacs probes for is not
;;     there; a name missing from a complete listing needs no request.
;;   * Listings stay true.  The agent watches what was listed and
;;     pushes changes, so a listing is fetched once per connection
;;     rather than once per `scrutiny-agent-fs-cache-ttl'.

(defun scrutiny-agent-fs--fresh (key)
  "The cache entry (TIMESTAMP . LISTING) for KEY if it may be used now."
  (let ((hit (gethash key scrutiny-agent-fs--dir-cache)))
    (and hit
         (> scrutiny-agent-fs-cache-ttl 0)
         (or (gethash key scrutiny-agent-fs--watched)
             (< (float-time (time-since (car hit)))
                scrutiny-agent-fs-cache-ttl))
         hit)))

(defun scrutiny-agent-fs--install-listing (host key result err seq)
  "File the reply to a listing request for KEY on HOST.
RESULT and ERR are the reply; SEQ is the value of
`scrutiny-agent-fs--change-seq' when the request was sent."
  (remhash key scrutiny-agent-fs--inflight)
  (cond
   (err
    ;; Cache the failure as well: a directory the agent will not serve
    ;; is asked about once, not once per file whose attributes are
    ;; wanted.  Not when the transport died, though -- that says
    ;; nothing about the directory.
    (unless (eql (plist-get err :code) -1)
      (puthash key (cons (current-time) 'unavailable)
               scrutiny-agent-fs--dir-cache))
    (remhash key scrutiny-agent-fs--self)
    (remhash key scrutiny-agent-fs--watched))
   (t
    (let ((canonical (plist-get result :path))
          (self (plist-get result :self)))
      (puthash key
               (cons (current-time)
                     (if (eq (plist-get result :tooLarge) t)
                         'too-large
                       (mapcar (lambda (entry)
                                 (cons (plist-get entry :name)
                                       (plist-put (copy-sequence entry)
                                                  :exists t)))
                               (scrutiny-agent-ops--list
                                (plist-get result :entries)))))
               scrutiny-agent-fs--dir-cache)
      (if self
          (puthash key (plist-put (copy-sequence self) :path canonical)
                   scrutiny-agent-fs--self)
        (remhash key scrutiny-agent-fs--self))
      (if (not (eq (plist-get result :watching) t))
          (remhash key scrutiny-agent-fs--watched)
        (puthash key canonical scrutiny-agent-fs--watched)
        (let ((ckey (concat host "\0" canonical)))
          (cl-pushnew key (gethash ckey scrutiny-agent-fs--canonical)
                      :test #'equal))
        ;; A change made while this reply was on its way may have been
        ;; announced ahead of it, when there was no listing yet to
        ;; apply it to.  Apply those now, oldest first.
        (dolist (change (reverse scrutiny-agent-fs--changes))
          (when (and (> (car change) seq)
                     (equal (cadr change) host)
                     (equal (plist-get (cddr change) :path) canonical))
            (scrutiny-agent-fs--apply-change host (cddr change)))))))))

(defun scrutiny-agent-fs--request-listing (conn host directory &optional full)
  "Send a listing request for DIRECTORY on HOST unless one is not needed.
Returns the future to wait on, or nil when a usable listing is
already cached.  FULL asks for every entry however many there are;
otherwise a directory larger than
`scrutiny-agent-fs-listing-max-entries' answers `too-large'."
  (let* ((key (scrutiny-agent-fs--cache-key host directory))
         (pending (gethash key scrutiny-agent-fs--inflight))
         (hit (scrutiny-agent-fs--fresh key)))
    (cond
     ((and pending (or (cdr pending) (not full))) (car pending))
     ((and hit (not (and full (eq (cdr hit) 'too-large)))) nil)
     (t
      (let* ((seq scrutiny-agent-fs--change-seq)
             (future
              (scrutiny-agent-send
               conn "fs.listDirectory"
               (nconc
                (list :path (directory-file-name directory) :attributes t)
                (and scrutiny-agent-fs-watch
                     (scrutiny-agent-feature-p conn "fs.watch")
                     (list :watch t))
                (and (not full)
                     (scrutiny-agent-feature-p
                      conn "fs.listDirectory.maxEntries")
                     (list :maxEntries
                           scrutiny-agent-fs-listing-max-entries)))
               (lambda (result err)
                 (scrutiny-agent-fs--install-listing
                  host key result err seq)))))
        (puthash key (cons future full) scrutiny-agent-fs--inflight)
        future)))))

(defun scrutiny-agent-fs--ancestors (host directory)
  "DIRECTORY and every directory above it inside HOST's roots."
  (let ((dir (directory-file-name (expand-file-name directory "/")))
        (out nil))
    (while (and (not (string= dir "/"))
                (scrutiny-agent-fs--in-roots-p host dir))
      (push dir out)
      (setq dir (directory-file-name (file-name-directory dir))))
    out))

(defun scrutiny-agent-fs--known-absent-p (host directory)
  "Non-nil when what is cached proves DIRECTORY is not a directory.
Looks only at listings already held, and asks nothing: if some
component on the way to DIRECTORY is missing from a current listing
of its parent, or is there as a plain file, there is no DIRECTORY."
  (let ((path (directory-file-name (expand-file-name directory "/")))
        (absent nil))
    (while (and (not absent)
                (not (string= path "/"))
                (scrutiny-agent-fs--in-roots-p host path))
      (let* ((parent (file-name-directory path))
             (hit (scrutiny-agent-fs--fresh
                   (scrutiny-agent-fs--cache-key host parent))))
        (when (and hit (listp (cdr hit)))
          (let ((entry (cdr (assoc (file-name-nondirectory path) (cdr hit)))))
            (unless (and entry (scrutiny-agent-ops-bool
                                (plist-get entry :isDir)))
              (setq absent t))))
        (setq path (directory-file-name parent))))
    absent))

(defun scrutiny-agent-fs--await (conn host future)
  "Wait for FUTURE on CONN; non-nil unless the wait was abandoned.
Where the user is typing (see `scrutiny-agent-fs--interactive-p') the
wait ends at the next keystroke -- the request stays in flight and
its reply still lands in the cache, so nothing is wasted, but Emacs
is never held up for it."
  (condition-case err
      (if (scrutiny-agent-fs--interactive-p)
          (eq 'answered
              (while-no-input
                (scrutiny-agent-await conn future 60)
                'answered))
        (scrutiny-agent-await conn future 60)
        t)
    ;; An error reply is still a reply; the callback has filed it.
    (scrutiny-agent-rpc-error t)
    (error
     (scrutiny-agent-fs--record-error host 'listing err)
     nil)))

(defun scrutiny-agent-fs--listing (host directory &optional full also)
  "The cached (NAME . STAT-PLIST) alist for DIRECTORY on HOST.

Returns the symbol `unavailable' when the directory could not be read
-- NOT nil, which is the correct answer for a directory that is simply
empty.  Without FULL it may also return `too-large', for a directory
with more entries than `scrutiny-agent-fs-listing-max-entries'; a
caller that only wants one file's attributes then asks for those
instead.  With FULL the listing is fetched whatever its size.

On a miss the request goes out together with one for every directory
above DIRECTORY that is not already cached, so that the walk up the
tree which follows costs nothing further.  ALSO, if non-nil, is one
more directory to ask for in the same burst: the caller expects to
want it next."
  (let* ((key (scrutiny-agent-fs--cache-key host directory))
         (hit (scrutiny-agent-fs--fresh key)))
    (cond
     ((and hit (not (and full (eq (cdr hit) 'too-large))))
      (cdr hit))
     ((scrutiny-agent-fs--known-absent-p host directory)
      'unavailable)
     (t
      (let ((conn (scrutiny-agent-fs--conn host)))
        (if (null conn)
            'unavailable
          (dolist (dir (scrutiny-agent-fs--ancestors host directory))
            (unless (string= (scrutiny-agent-fs--cache-key host dir) key)
              (scrutiny-agent-fs--request-listing conn host dir)))
          (when also
            (scrutiny-agent-fs--request-listing conn host also))
          (let ((future (scrutiny-agent-fs--request-listing
                         conn host directory full))
                (stale (gethash key scrutiny-agent-fs--dir-cache)))
            (cond
             ((and future
                   (not (scrutiny-agent-fs--await conn host future)))
              ;; Abandoned for a keystroke.  What we had is better than
              ;; nothing; the fresh copy arrives on its own.
              (if (and stale (not (and full (eq (cdr stale) 'too-large))))
                  (cdr stale)
                'unavailable))
             (t
              (let ((now (gethash key scrutiny-agent-fs--dir-cache)))
                (if now (cdr now) 'unavailable)))))))))))

(defun scrutiny-agent-fs--listing-ok (entries)
  "ENTRIES if the listing succeeded (possibly empty), else nil."
  (and (not (eq entries 'unavailable)) (or entries t)))

(defvar scrutiny-agent-fs--expect-directory nil
  "Non-nil while answering a question only asked about directories.")

(defun scrutiny-agent-fs--stat (host localname)
  "STAT plist for LOCALNAME on HOST, or nil if it does not exist.

Answered from the listing of the directory LOCALNAME is in, which is
fetched (together with the directories above it) if it is not held
already.  A name that is not in a complete listing does not exist;
no request is made to confirm it."
  (let* ((clean (if (string-prefix-p "~" localname)
                    localname
                  (directory-file-name (expand-file-name localname "/"))))
         (dir (file-name-directory clean))
         (base (file-name-nondirectory clean)))
    (cond
     ;; A `~' the caller did not expand: only the agent can resolve it.
     ((string-prefix-p "~" clean)
      (scrutiny-agent-fs--stat-uncached host clean))
     ;; A root, whose parent the agent will not list.  Its own listing
     ;; describes it, and is about to be wanted anyway.
     ((or (null dir) (string-empty-p base)
          (not (scrutiny-agent-fs--in-roots-p host dir)))
      (or (and (scrutiny-agent-fs--in-roots-p host clean)
               (not (eq (scrutiny-agent-fs--listing host clean)
                        'unavailable))
               (gethash (scrutiny-agent-fs--cache-key host clean)
                        scrutiny-agent-fs--self))
          (scrutiny-agent-fs--stat-uncached host clean)))
     (t
      ;; A caller asking whether this is a directory is about to ask
      ;; what is in it.  Fetch that listing in the same burst.
      (let ((entries (scrutiny-agent-fs--listing
                      host dir nil
                      (and (or scrutiny-agent-fs--expect-directory
                               (string-suffix-p "/" localname))
                           clean))))
        (cond
         ((listp entries)
          (let ((entry (cdr (assoc base entries)))
                (key (scrutiny-agent-fs--cache-key host dir)))
            ;; A symlink's attributes are its target's, and the target
            ;; may live in a directory nobody is watching.  A watched
            ;; listing is therefore only believed about a symlink for
            ;; the ordinary cache window.
            (when (and entry
                       (scrutiny-agent-ops-bool (plist-get entry :isSymlink))
                       (gethash key scrutiny-agent-fs--watched)
                       (let ((hit (gethash key scrutiny-agent-fs--dir-cache)))
                         (and hit (>= (float-time (time-since (car hit)))
                                      scrutiny-agent-fs-cache-ttl))))
              (remhash key scrutiny-agent-fs--dir-cache)
              (setq entries (scrutiny-agent-fs--listing host dir)
                    entry (and (listp entries) (cdr (assoc base entries)))))
            entry))
         ((eq entries 'too-large)
          (scrutiny-agent-fs--stat-uncached host clean))
         ;; The directory could not be listed.  If that is because it
         ;; is not there, neither is anything in it.
         ((scrutiny-agent-fs--known-absent-p host dir) nil)
         (t (scrutiny-agent-fs--stat-uncached host clean))))))))

(defun scrutiny-agent-fs--stat-uncached (host path)
  "One `fs.stat' for PATH on HOST, memoized for the cache window.
The slow path: used only where the directory listing cannot answer --
a directory too large to fetch, or one the agent will not list."
  (let* ((key (concat "\0stat\0" host "\0" path))
         (hit (gethash key scrutiny-agent-fs--dir-cache)))
    (cond
     ((and hit (> scrutiny-agent-fs-cache-ttl 0)
           (< (float-time (time-since (car hit)))
              scrutiny-agent-fs-cache-ttl))
      (cdr hit))
     (t
      (let* ((conn (scrutiny-agent-fs--conn host))
             (future (and conn
                          (scrutiny-agent-send conn "fs.stat"
                                               (list :path path))))
             (result
              (cond
               ((null future) nil)
               ((scrutiny-agent-fs--await conn host future)
                (and (null (scrutiny-agent-future-error future))
                     (scrutiny-agent-future-result future)))
               ;; Abandoned for a keystroke: the old answer, if any.
               (t (cdr hit)))))
        (when (and future (scrutiny-agent-future-done-p future)
                   (> scrutiny-agent-fs-cache-ttl 0))
          (puthash key (cons (current-time) result)
                   scrutiny-agent-fs--dir-cache))
        result)))))

;; ---------------------------------------------------------------------
;; Handler helpers
;; ---------------------------------------------------------------------

(defun scrutiny-agent-fs--home-relative (host localname)
  "LOCALNAME with a leading `~' replaced by HOST's home directory.

A remote name under the home directory is spelled with a tilde almost
everywhere -- `abbreviate-file-name' sees to that -- and the tilde
means the REMOTE home.  Nothing below this point may hand such a name
to `expand-file-name', which would substitute the home directory of
the machine Emacs is running on.  It is resolved here, once, for
every operation."
  (if (and (string-prefix-p "~" localname)
           (or (= (length localname) 1) (eq (aref localname 1) ?/)))
      (let ((home (scrutiny-agent-fs--home host)))
        (if home
            (concat home (substring localname 1))
          localname))
    localname))

(defun scrutiny-agent-fs--resolve (filename)
  "Like `scrutiny-agent-fs--parse', with the local name made absolute."
  (let ((parsed (scrutiny-agent-fs--parse filename)))
    (and parsed
         (list (nth 0 parsed) (nth 1 parsed)
               (scrutiny-agent-fs--home-relative (nth 0 parsed)
                                                 (nth 2 parsed))))))

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
      (let* ((host (nth 0 parsed))
             (prefix (nth 1 parsed))
             (conn (scrutiny-agent-fs--conn host))
             (localname (if conn
                            (scrutiny-agent-fs--home-relative
                             host (nth 2 parsed))
                          (nth 2 parsed))))
        (cond
         ((not conn)
          (when scrutiny-agent-fs-verbose
            (message "dispatch[%s]: no conn for %s, delegating" operation host))
          (scrutiny-agent-fs--delegate operation args))
         ;; `~user', or a `~' with the remote home unknown: only the
         ;; far end can say where that is.
         ((string-prefix-p "~" localname)
          (scrutiny-agent-fs--delegate operation args))
         ((not (scrutiny-agent-fs--in-roots-p host localname))
          ;; Outside the roots. Either answer from structure (no
          ;; request, no connection) or hand it to TRAMP.
          (when scrutiny-agent-fs-verbose
            (message "dispatch[%s]: %s outside roots" operation localname))
          (if scrutiny-agent-fs-roots-are-the-world
              (scrutiny-agent-fs--outside-roots host prefix localname
                                                operation args)
            (scrutiny-agent-fs--delegate operation args)))
         (t (funcall handler host prefix localname)))))))

(defun scrutiny-agent-fs--structural-names (host localname full match
                                                 nosort count)
  "What `directory-files' returns for LOCALNAME, an ancestor of a root.
The entries are the children that lead down to roots, plus `.' and
`..', shaped exactly as a real listing would be: no trailing slash,
MATCH applied, FULL names, sorted unless NOSORT, at most COUNT.  A
caller walking up the tree with a marker pattern -- project.el looking
for `.git', `locate-dominating-file' in general -- must get nil when
nothing matches, or it takes the answer for a hit."
  (let ((names (append '("." "..")
                       (scrutiny-agent-fs--root-children host localname))))
    (when match
      (setq names (cl-remove-if-not (lambda (n) (string-match-p match n))
                                    names)))
    (when full
      (let ((dir (file-name-as-directory (expand-file-name localname "/"))))
        (setq names (mapcar (lambda (n) (concat dir n)) names))))
    (unless nosort (setq names (sort names #'string<)))
    (if (and (natnump count) (< count (length names)))
        (cl-subseq names 0 count)
      names)))

(defun scrutiny-agent-fs--outside-roots (host prefix localname operation args)
  "Answer OPERATION, called with ARGS, for LOCALNAME outside HOST's roots.
See `scrutiny-agent-fs-roots-are-the-world'."
  (let ((ancestor (scrutiny-agent-fs--ancestor-of-root-p host localname))
        (dir-attributes (list t 1 0 0 nil nil nil 0 "dr-xr-xr-x" t 0 0)))
    (pcase operation
      ((or 'file-exists-p 'file-directory-p 'file-readable-p) ancestor)
      ('file-regular-p nil)
      ('file-symlink-p nil)
      ('file-writable-p nil)
      ('file-modes (and ancestor #o555))
      ('file-truename (concat prefix (expand-file-name localname "/")))
      ('file-attributes
       ;; Enough for a caller to see a directory it may descend; the
       ;; agent never reported its size or times, so neither do we.
       (and ancestor dir-attributes))
      ('directory-files
       ;; (DIRECTORY &optional FULL MATCH NOSORT COUNT)
       (when ancestor
         (mapcar (lambda (n) (if (nth 1 args) (concat prefix n) n))
                 (scrutiny-agent-fs--structural-names
                  host localname (nth 1 args) (nth 2 args) (nth 3 args)
                  (nth 4 args)))))
      ('directory-files-and-attributes
       ;; (DIRECTORY &optional FULL MATCH NOSORT ID-FORMAT COUNT)
       (when ancestor
         (mapcar (lambda (n)
                   (cons (if (nth 1 args) (concat prefix n) n)
                         dir-attributes))
                 (scrutiny-agent-fs--structural-names
                  host localname (nth 1 args) (nth 2 args) (nth 3 args)
                  (nth 5 args)))))
      ('file-name-all-completions
       ;; (FILE DIRECTORY): directories complete with a slash, as
       ;; everywhere else.
       (when ancestor
         (let ((file (or (nth 0 args) "")) (out nil))
           (dolist (n (scrutiny-agent-fs--root-children host localname))
             (when (string-prefix-p file n)
               (push (file-name-as-directory n) out)))
           (when (string-prefix-p file ".")
             (push "./" out)
             (when (string-prefix-p file "..") (push "../" out)))
           (nreverse out))))
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
     (let ((scrutiny-agent-fs--expect-directory t))
       (scrutiny-agent-ops-bool
        (plist-get (scrutiny-agent-fs--stat host localname) :isDir))))))

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
     (let* ((stat (scrutiny-agent-fs--stat host localname))
            (real (plist-get stat :path)))
       (cond
        (real (concat prefix real))
        ;; Not there: nothing to resolve in the last component.  The
        ;; directory it would be in may still be reached by a symlink.
        ((not (string-prefix-p "~" localname))
         (let* ((clean (directory-file-name (expand-file-name localname "/")))
                (dir (file-name-directory clean))
                (parent (and dir (not (string= clean "/"))
                             (scrutiny-agent-fs--in-roots-p host dir)
                             (plist-get (scrutiny-agent-fs--stat host dir)
                                        :path))))
           (concat prefix
                   (if parent
                       (concat (file-name-as-directory parent)
                               (file-name-nondirectory clean))
                     clean))))
        (t (scrutiny-agent-fs--delegate 'file-truename (list filename))))))))

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
  (let* ((entries (scrutiny-agent-fs--listing host directory t))
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
     (let ((entries (scrutiny-agent-fs--listing host localname t)))
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
     (let ((entries (scrutiny-agent-fs--listing host localname t)))
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
     (let ((entries (scrutiny-agent-fs--listing host localname t)))
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

(defun scrutiny-agent-fs--read-params (conn localname)
  "Parameters for a `fs.readFile' of LOCALNAME on CONN."
  (nconc (list :path localname :base64 t :stat t)
         (and (scrutiny-agent-feature-p conn "fs.readFile.maxBytes")
              (list :maxBytes scrutiny-agent-fs-max-inline-bytes))))

(defun scrutiny-agent-fs--fetch (host localname)
  "Raw bytes of LOCALNAME on HOST as a unibyte string.
Returns nil if it could not be read, and the symbol `too-large' if the
agent declined to send more than `scrutiny-agent-fs-max-inline-bytes'.

Uses the body `scrutiny-agent-fs--prefetch' asked for, if one is on
its way.  The attributes that come back with the bytes are filed in
the cache, so the modification time Emacs records for the buffer is
that of the bytes it actually holds."
  (let* ((conn (scrutiny-agent-fs--conn host))
         (bkey (concat host "\0" localname))
         (ahead (gethash bkey scrutiny-agent-fs--bodies)))
    (remhash bkey scrutiny-agent-fs--bodies)
    (when conn
      (condition-case err
          (let ((result (scrutiny-agent-await
                         conn
                         (or (cdr ahead)
                             (scrutiny-agent-send
                              conn "fs.readFile"
                              (scrutiny-agent-fs--read-params conn localname)))
                         300)))
            (if (eq (plist-get result :tooLarge) t)
                'too-large
              (scrutiny-agent-fs--note-attributes host localname result)
              (base64-decode-string
               (or (plist-get result :contentBase64) ""))))
        (error
         (scrutiny-agent-fs--record-error host 'read err)
         nil)))))

(defun scrutiny-agent-fs--note-attributes (host localname result)
  "Update LOCALNAME's cached size and times from RESULT, if it is cached."
  (let* ((clean (directory-file-name localname))
         (hit (gethash (scrutiny-agent-fs--cache-key
                        host (file-name-directory clean))
                       scrutiny-agent-fs--dir-cache))
         (cell (and hit (listp (cdr hit))
                    (assoc (file-name-nondirectory clean) (cdr hit)))))
    (when cell
      (let ((stat (cdr cell)))
        (dolist (key '(:size :mtime :mtimeNs))
          (when (plist-get result key)
            (setq stat (plist-put stat key (plist-get result key)))))
        (setcdr cell stat)))))

(defun scrutiny-agent-fs--prefetch (filename &rest _)
  "Ask for everything opening FILENAME will need, without waiting.
Advice before `find-file-noselect'.  The listings of FILENAME's
directory and of every directory above it, and the file's contents,
go out as one burst; by the time `find-file' asks its first question
the answers to all of them are on their way back together."
  (when (and scrutiny-agent-fs-prefetch (stringp filename))
    (let ((parsed (scrutiny-agent-fs--resolve (expand-file-name filename))))
      (when parsed
        (let* ((host (nth 0 parsed))
               (localname (nth 2 parsed))
               (conn (scrutiny-agent-connection host))
               (bkey (concat host "\0" localname)))
          (when (and conn
                     (not (string-prefix-p "~" localname))
                     (not (string-suffix-p "/" localname))
                     (scrutiny-agent-fs--in-roots-p host localname)
                     (not (get-file-buffer (expand-file-name filename))))
            (dolist (dir (scrutiny-agent-fs--ancestors
                          host (file-name-directory localname)))
              (scrutiny-agent-fs--request-listing conn host dir))
            ;; Bodies nobody collected (the name turned out to be a
            ;; directory, the open was abandoned) do not accumulate.
            (maphash (lambda (key value)
                       (when (> (float-time (time-since (car value))) 30)
                         (remhash key scrutiny-agent-fs--bodies)))
                     scrutiny-agent-fs--bodies)
            (unless (or (gethash bkey scrutiny-agent-fs--bodies)
                        ;; Known not to be a readable file: nothing to
                        ;; fetch.  Unknown is the usual case, and the
                        ;; agent answers an error for a miss.
                        (scrutiny-agent-fs--known-absent-p
                         host (file-name-directory localname)))
              (puthash bkey
                       (cons (current-time)
                             (scrutiny-agent-send
                              conn "fs.readFile"
                              (scrutiny-agent-fs--read-params
                               conn localname)))
                       scrutiny-agent-fs--bodies))))))))

(defun scrutiny-agent-fs--handle-file-local-copy (filename)
  (scrutiny-agent-fs--dispatch
   filename 'file-local-copy (list filename)
   (lambda (host _prefix localname)
     (let ((stat (scrutiny-agent-fs--stat host localname)))
       (cond
        ((not (scrutiny-agent-ops-bool (plist-get stat :exists)))
         (remhash (concat host "\0" localname) scrutiny-agent-fs--bodies)
         nil)
        ;; Too large to hold in one reply: TRAMP can stream it.
        ((> (or (plist-get stat :size) 0) scrutiny-agent-fs-max-inline-bytes)
         (remhash (concat host "\0" localname) scrutiny-agent-fs--bodies)
         (scrutiny-agent-fs--delegate 'file-local-copy (list filename)))
        (t
         (let ((bytes (scrutiny-agent-fs--fetch host localname)))
           (cond
            ((eq bytes 'too-large)
             (scrutiny-agent-fs--delegate 'file-local-copy (list filename)))
            ((null bytes)
             (scrutiny-agent-fs--warn
              host "file-local-copy: failed to fetch %s" localname)
             nil)
            (t
             (let ((tmp (make-temp-file "scrutiny-agent-fs")))
               (let ((coding-system-for-write 'binary))
                 (with-temp-file tmp
                   (set-buffer-multibyte nil)
                   (insert bytes)))
               tmp))))))))))

(defun scrutiny-agent-fs--handle-insert-file-contents
    (filename &optional visit beg end replace)
  "Insert FILENAME's contents, decoding exactly as a local read would.

The bytes are fetched over the agent into a local temp file and then
handed to the ordinary local `insert-file-contents'.  That is what
gets coding-system detection, REPLACE and BEG/END right -- reproducing
those here would be a large and subtle way to corrupt buffers.

A file that is not there signals `file-missing', as it does locally.
That is not a detail: it is how `find-file' learns it is creating a
new file, and how every caller that probes for an optional file
(`.gitmodules', `.dir-locals.el') learns there is none."
  (scrutiny-agent-fs--dispatch
   filename 'insert-file-contents (list filename visit beg end replace)
   (lambda (host _prefix localname)
     (let ((local (scrutiny-agent-fs--handle-file-local-copy filename)))
       (cond
        (local
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
           (delete-file local)))
        ((file-exists-p filename)
         (scrutiny-agent-fs--warn
          host "insert-file-contents: failed for %s" localname)
         (signal 'file-error
                 (list "Opening input file" "Could not read over scrutiny-agent"
                       filename)))
        (t
         (when visit
           (setq buffer-file-name filename)
           (set-visited-file-modtime -1)
           (set-buffer-modified-p nil))
         (signal 'file-missing
                 (list "Opening input file" "No such file or directory"
                       filename))))))))

;; ---------------------------------------------------------------------
;; Writing
;; ---------------------------------------------------------------------

(defun scrutiny-agent-fs--expected-mtime (conn host localname filename visit)
  "The mtime FILENAME must still have for a save to go ahead, or nil.

Non-nil only when the current buffer is saving the file it visits,
has a modification time on record, and nothing we know says the file
has changed since -- the case where Emacs is about to overwrite a
file on the strength of what this client has cached.  The write is
then made conditional on that time (`ifMtimeNs'), so the agent, which
can look, is the one that decides; a change that has not reached us
yet cannot be silently overwritten.

When we already know the file changed, Emacs has asked the user and
been told to go ahead, and the write is unconditional."
  (and (eq visit t)
       buffer-file-name
       (scrutiny-agent-feature-p conn "fs.writeFile.ifMtimeNs")
       (equal (expand-file-name filename) (expand-file-name buffer-file-name))
       (let ((recorded (visited-file-modtime)))
         (and (consp recorded)
              (> (float-time recorded) 0)
              (let* ((stat (scrutiny-agent-fs--stat host localname))
                     (known (and stat (scrutiny-agent-fs--mtime stat))))
                (and known
                     (time-equal-p known recorded)
                     (scrutiny-agent-fs--time-ns recorded)))))))

(defun scrutiny-agent-fs--handle-write-region
    (start end filename &optional append visit lockname mustbenew)
  "Write the region to FILENAME over the agent.

The region is encoded through a local temp file first, so the buffer's
coding system applies exactly as it would locally; the resulting bytes
are what get uploaded.

A save costs one request.  The reply describes the file as it now is,
and that goes straight into the cache, so nothing is re-read
afterwards; and the check that the file has not changed underneath
the buffer is made by the agent as part of the write rather than by a
request of its own beforehand."
  (let ((args (list start end filename append visit lockname mustbenew)))
    (scrutiny-agent-fs--dispatch
     filename 'write-region args
     (lambda (host _prefix localname)
       (run-hooks 'scrutiny-agent-fs-change-hook)
       (if (or append                            ; needs read-modify-write
               (not (scrutiny-agent-fs--writable-p host)))
           (scrutiny-agent-fs--delegate 'write-region args)
         (when (and mustbenew (file-exists-p filename)
                    (or (not (eq mustbenew 'excl))
                        (signal 'file-already-exists
                                (list "File exists" filename))))
           (unless (y-or-n-p (format "File %s exists; overwrite? " filename))
             (signal 'file-already-exists (list "File exists" filename))))
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
           (let* ((conn (scrutiny-agent-fs--conn host))
                  (expected (scrutiny-agent-fs--expected-mtime
                             conn host localname filename visit))
                  (params (list :path localname
                                :contentBase64
                                (base64-encode-string bytes t))))
             (condition-case err
                 (let ((result (scrutiny-agent-request
                                conn "fs.writeFile"
                                (if expected
                                    (append params (list :ifMtimeNs expected))
                                  params)
                                300)))
                   (when (eq (plist-get result :conflict) t)
                     ;; The file moved on after we last heard about it.
                     ;; Nothing was written.  This is the question Emacs
                     ;; asks when it notices in time; ask it now.
                     (scrutiny-agent-fs--invalidate host localname)
                     (unless (yes-or-no-p
                              (format "%s has changed since visited or \
saved.  Save anyway? "
                                      (file-name-nondirectory filename)))
                       (user-error "Save not confirmed"))
                     (setq result (scrutiny-agent-request
                                   conn "fs.writeFile" params 300)))
                   (unless (and (plist-get result :entry)
                                (scrutiny-agent-fs--file-entry
                                 host localname (plist-get result :entry)))
                     (scrutiny-agent-fs--invalidate host localname))
                   (cond
                    ((eq visit t)
                     (setq buffer-file-name filename)
                     (set-visited-file-modtime
                      (or (scrutiny-agent-fs--mtime result) 0))
                     (set-buffer-modified-p nil))
                    ((stringp visit)
                     (setq buffer-file-name visit)
                     (set-buffer-modified-p nil)))
                   ;; As the built-in does: announced unless VISIT is
                   ;; some other non-nil value, and never in batch.
                   (when (and (not noninteractive)
                              (or (eq visit t) (null visit) (stringp visit)))
                     (message "Wrote %s" filename))
                   nil)
               (scrutiny-agent-rpc-error
                ;; Never lose a save: if the agent refused, TRAMP still
                ;; knows how to write the file.
                (scrutiny-agent--log conn "fs: write refused (%s), using TRAMP"
                                     (scrutiny-agent-ops-error-name
                                      (nth 1 err)))
                (scrutiny-agent-fs--delegate 'write-region args))))))))))

(defun scrutiny-agent-fs--write-op (operation args host localname thunk
                                              &optional after)
  "Run THUNK for a mutating OPERATION on LOCALNAME, or delegate.

On success AFTER is called to bring the cache up to date; without one
everything cached about LOCALNAME is dropped.

Only a refusal on policy grounds is handed to TRAMP -- an agent
without `--allow-write', a path outside its roots -- because there
TRAMP may be allowed to do what the agent is not.  Any other failure
is the operating system's answer and is reported as one: asking TRAMP
to try again would open a second connection in order to fail the same
way."
  (run-hooks 'scrutiny-agent-fs-change-hook)
  (if (not (scrutiny-agent-fs--writable-p host))
      (scrutiny-agent-fs--delegate operation args)
    (condition-case err
        (prog1 (funcall thunk)
          (if after
              (funcall after)
            (scrutiny-agent-fs--invalidate host localname)))
      (scrutiny-agent-rpc-error
       (scrutiny-agent-fs--invalidate host localname)
       (pcase (nth 1 err)
         (1005 (scrutiny-agent-fs--delegate operation args))
         (1001 (signal 'file-missing
                       (list (symbol-name operation)
                             "No such file or directory"
                             (car args))))
         (_ (signal 'file-error
                    (list (symbol-name operation)
                          (or (nth 2 err) "failed")
                          (car args)))))))))

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
       ;; Deleting what is not there is not an error, and what is cached
       ;; usually says so without asking.
       (when (scrutiny-agent-fs--stat host localname)
         (condition-case nil
             (scrutiny-agent-fs--write-op
              'delete-file args host localname
              (lambda ()
                (scrutiny-agent-request (scrutiny-agent-fs--conn host)
                                        "fs.delete" (list :path localname) 60)
                nil)
              (lambda ()
                (unless (scrutiny-agent-fs--file-entry host localname nil)
                  (scrutiny-agent-fs--invalidate host localname))))
           (file-missing nil)))))))

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
        (from (scrutiny-agent-fs--resolve file))
        (to (scrutiny-agent-fs--resolve newname)))
    ;; Both ends must be on the same host for the agent to do it in one
    ;; operation; a cross-host rename is a copy plus a delete, which
    ;; TRAMP already implements.
    (if (not (and from to (equal (nth 0 from) (nth 0 to))))
        (scrutiny-agent-fs--delegate 'rename-file args)
      (let* ((host (nth 0 from))
             (source (nth 2 from))
             (target (nth 2 to))
             (entry (scrutiny-agent-fs--stat host source)))
        (unless (or ok-if-exists (not (file-exists-p newname)))
          (signal 'file-already-exists (list "File exists" newname)))
        (scrutiny-agent-fs--write-op
         'rename-file args host source
         (lambda ()
           (scrutiny-agent-request (scrutiny-agent-fs--conn host) "fs.rename"
                                   (list :from source :to target)
                                   120)
           nil)
         (lambda ()
           ;; The entry moves; what it says about the file is unchanged.
           (unless (scrutiny-agent-fs--file-entry host source nil)
             (scrutiny-agent-fs--invalidate host source))
           (unless (and entry
                        (scrutiny-agent-fs--file-entry
                         host target
                         (plist-put
                          (plist-put (copy-sequence entry)
                                     :name (file-name-nondirectory
                                            (directory-file-name target)))
                          :path (directory-file-name target))))
             (scrutiny-agent-fs--invalidate host target))))))))

(defun scrutiny-agent-fs--handle-copy-file
    (file newname &optional ok-if-exists keep-time preserve-uid preserve-perms)
  (let ((args (list file newname ok-if-exists keep-time preserve-uid
                    preserve-perms))
        (from (scrutiny-agent-fs--resolve file))
        (to (scrutiny-agent-fs--resolve newname)))
    (if (not (and from to (equal (nth 0 from) (nth 0 to))))
        (scrutiny-agent-fs--delegate 'copy-file args)
      (let ((host (nth 0 from)))
        (unless (or ok-if-exists (not (file-exists-p newname)))
          (signal 'file-already-exists (list "File exists" newname)))
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
       (if (eql mode (plist-get (scrutiny-agent-fs--stat host localname)
                                :mode))
           ;; Already so.  `save-buffer' restores the mode after every
           ;; write, and the agent's write preserved it.
           nil
         (scrutiny-agent-fs--write-op
          'set-file-modes args host localname
          (lambda ()
            (scrutiny-agent-request (scrutiny-agent-fs--conn host) "fs.chmod"
                                    (list :path localname :mode mode)
                                    60)
            nil)
          (lambda ()
            (let ((entry (scrutiny-agent-fs--stat host localname)))
              (unless (and entry
                           (scrutiny-agent-fs--file-entry
                            host localname
                            (plist-put (copy-sequence entry) :mode mode)))
                (scrutiny-agent-fs--invalidate host localname))))))))))

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

;; The next group is the difference between "accelerated" and "fast".
;; Each of these used to be handed to TRAMP, and the first one to run
;; made TRAMP open its own connection to the host -- on a Teleport link
;; that is seconds, spent on the first keystroke in a buffer
;; (`lock-file') or the first Dired (`file-system-info'), and then every
;; later call was a round trip on that second connection.  None of them
;; needs the remote at all, or needs more than what is already cached.

(defun scrutiny-agent-fs--handle-lock-file (file)
  "Do what locking is for, without the lock file.

Emacs locks a file on the first change to its buffer for two reasons:
to warn that the file changed on disk since it was read, and to warn
that someone else is editing it.  The first is answered here, from
attributes the agent keeps current, at no cost.  The second needs a
`.#name' file created on the remote and removed at every save -- two
round trips per save for a warning that only fires if the other
editor is also an Emacs, on a host reached through an agent that is
by construction one user's.  It is not done."
  (when (and buffer-file-truename
             (equal (expand-file-name file)
                    (expand-file-name (or buffer-file-name "")))
             (not (verify-visited-file-modtime))
             (file-exists-p file))
    (ask-user-about-supersession-threat file))
  nil)

(defun scrutiny-agent-fs--handle-file-accessible-directory-p (filename)
  (and (file-directory-p filename)
       (file-readable-p filename)
       (file-executable-p filename)))

(defun scrutiny-agent-fs--handle-exec-path ()
  "The remote search path for executables: nothing.

`executable-find' with its REMOTE argument asks for this when
`default-directory' is on a host of ours -- magit does so before
reporting that a directory is not a repository -- and TRAMP would
answer it by opening a connection and running a shell to read $PATH.
Nothing on that path is inside the roots, so no entry of it could be
examined anyway; the honest answer is an empty one, and it costs no
round trip.  Programs are run through `process-file', which does not
consult this."
  nil)

(defun scrutiny-agent-fs--handle-file-executable-p (filename)
  (scrutiny-agent-fs--dispatch
   filename 'file-executable-p (list filename)
   (lambda (host _prefix localname)
     (let* ((stat (scrutiny-agent-fs--stat host localname))
            (mode (plist-get stat :mode)))
       ;; The agent reports whether it can read and write; execute
       ;; permission is read off the mode bits, any of which will do
       ;; for the only user the agent runs as.
       (and (scrutiny-agent-ops-bool (plist-get stat :exists))
            (integerp mode)
            (/= 0 (logand mode #o111)))))))

(defun scrutiny-agent-fs--handle-file-equal-p (file1 file2)
  (let ((true1 (file-truename file1))
        (true2 (file-truename file2)))
    (and true1 true2
         (string= (directory-file-name true1) (directory-file-name true2)))))

(defun scrutiny-agent-fs--handle-file-in-directory-p (file dir)
  (let ((file (file-name-as-directory (file-truename file)))
        (dir (file-name-as-directory (file-truename dir))))
    (string-prefix-p dir file)))

(defun scrutiny-agent-fs--handle-file-name-sans-versions
    (name &optional keep-backup-version)
  "String surgery: run the built-in on the bare name."
  (let ((file-name-handler-alist nil))
    (file-name-sans-versions name keep-backup-version)))

(defun scrutiny-agent-fs--handle-file-notify-add-watch (&rest _)
  "Decline, so the caller polls instead -- which here costs nothing.
`auto-revert-mode' falls back to checking the file's modification
time on a timer, and that is answered from attributes the agent
already keeps current.  TRAMP's implementation would start a watcher
process on the remote over a connection of its own."
  (signal 'file-notify-error
          '("Not needed" "scrutiny-agent keeps file attributes current")))

(defvar scrutiny-agent-fs-change-hook nil
  "Hook run just before this client changes anything on a configured host.
That is: before a file is written, renamed, deleted or has its mode
changed through this handler, and before any process is started in a
directory there.  Whatever a layer has worked out from the state of
the remote filesystem -- answers to git commands it ran early, say --
is about to stop being true.")

(defun scrutiny-agent-fs--after-foreign-change (conn host)
  "Catch the cache up after something else changed files on HOST.
A git command that ran over CONN, for instance.  Directories the agent
watches need nothing: the changes were pushed, and a current agent
delivers them ahead of the reply to the command that made them.  The
rest are only held on a timer, so they are dropped."
  (let ((prefix (concat host "\0"))
        (ordered (scrutiny-agent-feature-p conn "fs.changed.ordered")))
    (maphash (lambda (key _value)
               (cond
                ((string-prefix-p (concat "\0stat\0" prefix) key)
                 (remhash key scrutiny-agent-fs--dir-cache))
                ((and (string-prefix-p prefix key)
                      (not (and ordered
                                (gethash key scrutiny-agent-fs--watched))))
                 (scrutiny-agent-fs--forget-key key))))
             scrutiny-agent-fs--dir-cache)))

(defvar scrutiny-agent-fs-start-process-function nil
  "Function starting an asynchronous process in a directory on a host.
Called with a `make-process' argument plist (`:name', `:buffer',
`:command', `:filter', `:sentinel', ...) and `default-directory'
remote.  Returns a process object, or the symbol `decline' to have
TRAMP start it.  Set by `scrutiny-agent-magit-mode', which runs git
this way.")

(defun scrutiny-agent-fs--start-process (operation args plist)
  "Start a process described by PLIST, or hand OPERATION ARGS to TRAMP."
  (run-hooks 'scrutiny-agent-fs-change-hook)
  (let ((process (if scrutiny-agent-fs-start-process-function
                     (funcall scrutiny-agent-fs-start-process-function plist)
                   'decline)))
    (if (eq process 'decline)
        (scrutiny-agent-fs--delegate operation args)
      process)))

(defun scrutiny-agent-fs--handle-start-file-process
    (name buffer program &rest program-args)
  (scrutiny-agent-fs--start-process
   'start-file-process (append (list name buffer program) program-args)
   (list :name name :buffer buffer
         :command (and program (cons program program-args)))))

(defun scrutiny-agent-fs--handle-make-process (&rest args)
  (scrutiny-agent-fs--start-process 'make-process args args))

(defvar scrutiny-agent-fs-process-file-function nil
  "Function serving `process-file' in a directory on a configured host.
Called with (PROGRAM INFILE BUFFER DISPLAY ARGS) and `default-directory'
remote.  Returns the exit status, or the symbol `decline' to have the
call handed to TRAMP.  Set by `scrutiny-agent-magit-mode', which
routes git through the agent; this is the one place every package
that runs git on a remote file reaches, whichever package it is.")

(defun scrutiny-agent-fs--handle-process-file
    (program &optional infile buffer display &rest args)
  (let ((status (if scrutiny-agent-fs-process-file-function
                    (funcall scrutiny-agent-fs-process-file-function
                             program infile buffer display args)
                  'decline)))
    (if (eq status 'decline)
        (scrutiny-agent-fs--delegate
         'process-file (append (list program infile buffer display) args))
      status)))

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
                host (directory-file-name localname) t)))
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
     . scrutiny-agent-fs--handle-file-ownership-preserved-p)
    (lock-file . scrutiny-agent-fs--handle-lock-file)
    (unlock-file . ignore)
    (file-locked-p . ignore)
    (make-lock-file-name . ignore)
    (set-file-acl . ignore)
    (set-file-selinux-context . ignore)
    (file-system-info . ignore)
    (file-accessible-directory-p
     . scrutiny-agent-fs--handle-file-accessible-directory-p)
    (file-executable-p . scrutiny-agent-fs--handle-file-executable-p)
    (file-equal-p . scrutiny-agent-fs--handle-file-equal-p)
    (file-in-directory-p . scrutiny-agent-fs--handle-file-in-directory-p)
    (file-name-sans-versions
     . scrutiny-agent-fs--handle-file-name-sans-versions)
    (file-notify-add-watch . scrutiny-agent-fs--handle-file-notify-add-watch)
    (process-file . scrutiny-agent-fs--handle-process-file)
    (start-file-process . scrutiny-agent-fs--handle-start-file-process)
    (make-process . scrutiny-agent-fs--handle-make-process)
    (exec-path . scrutiny-agent-fs--handle-exec-path))
  "Operations this handler serves over the agent.
Anything absent is delegated to TRAMP.  That keeps every corner
working, but it is not free: TRAMP answers over a connection of its
own, which it opens the first time it is asked anything that needs
the remote.  `scrutiny-agent-fs-delegations' shows what has gone that
way in this session.")

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
in here.

A user name and a port are part of how TRAMP spells a host, not a
different host: `/ssh:me@devbox:' and `/ssh:devbox#2222:' are served
exactly as `/ssh:devbox:' is."
  (if (null scrutiny-agent-hosts)
      "\\`\\'"                          ; matches nothing
    (concat "\\`/[^:/|]+:\\(?:[^@:/|]+@\\)?"
            (regexp-opt (append (mapcar #'car scrutiny-agent-hosts)
                                (mapcar #'car scrutiny-agent-fs-host-alist)))
            "\\(?:#[0-9]+\\)?:")))

(defvar scrutiny-agent-fs-mode)  ; defined by define-minor-mode below

(defun scrutiny-agent-fs--on-connect (host)
  "Start filling the cache for HOST the moment it connects.
Sends a listing request for each root and returns at once; nothing
here waits, so connecting is not slowed by it."
  (when scrutiny-agent-fs-mode
    (scrutiny-agent-fs--forget-host host)
    (let ((conn (scrutiny-agent-connection host)))
      (when (and conn (scrutiny-agent--conn-facts conn))
        ;; The canonical roots only: an alias spelling names the same
        ;; directory, and listing it twice is a wasted round trip.
        (dolist (root (scrutiny-agent-ops--list
                       (plist-get (scrutiny-agent--conn-facts conn)
                                  :allowedRoots)))
          (scrutiny-agent-fs--request-listing conn host root))))))

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
  (clrhash scrutiny-agent-fs--inflight)
  ;; Remove our hooks first (idempotent)
  (remove-hook 'scrutiny-agent-after-connect-hook #'scrutiny-agent-fs--on-connect)
  (remove-hook 'scrutiny-agent-disconnect-functions
               #'scrutiny-agent-fs--forget-host)
  (setq scrutiny-agent-notification-handlers
        (assoc-delete-all "fs.changed" scrutiny-agent-notification-handlers))
  (advice-remove 'find-file-noselect #'scrutiny-agent-fs--prefetch)
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
        (add-hook 'scrutiny-agent-after-connect-hook #'scrutiny-agent-fs--on-connect)
        ;; A dropped connection takes its watches with it; what they
        ;; vouched for must go at the same moment.
        (add-hook 'scrutiny-agent-disconnect-functions
                  #'scrutiny-agent-fs--forget-host)
        (push (cons "fs.changed" #'scrutiny-agent-fs--on-changed)
              scrutiny-agent-notification-handlers)
        ;; The one piece of advice in this file.  It sends requests and
        ;; does nothing else; see `scrutiny-agent-fs-prefetch'.
        (advice-add 'find-file-noselect :before #'scrutiny-agent-fs--prefetch)
        (unless (scrutiny-agent-fs-active-p)
          (message "scrutiny-agent-fs: another handler precedes ours; \
run M-x scrutiny-agent-fs-refresh after loading it")))
    nil))

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
  "List HOST's roots now, so the first completion there is immediate.
This happens by itself on connect; the command is for doing it again
after `scrutiny-agent-fs-flush-cache'."
  (interactive (list (completing-read "Host: "
                                       (mapcar #'car scrutiny-agent-hosts))))
  (let ((conn (scrutiny-agent-connection host)))
    (unless conn
      (user-error "Not connected to %s (use C-c s c first)" host))
    (let ((roots (scrutiny-agent-fs--roots host))
          (count 0))
      (dolist (root roots)
        (when (listp (scrutiny-agent-fs--listing host root t))
          (cl-incf count)))
      (message "scrutiny-agent-fs[%s]: %d of %d roots listed%s"
               host count (length roots)
               (if (and scrutiny-agent-fs-watch
                        (scrutiny-agent-feature-p conn "fs.watch"))
                   ", kept current by the agent" "")))))

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
      (insert "Handed to TRAMP (operation . count) -- every one of these\n"
              "was slow; a busy list is a configuration problem:\n")
      (if (null scrutiny-agent-fs-delegations)
          (insert "  (none)\n\n")
        (dolist (cell (sort (copy-sequence scrutiny-agent-fs-delegations)
                            (lambda (x y) (> (cdr x) (cdr y)))))
          (insert (format "  %6d  %s\n" (cdr cell) (car cell))))
        (insert "\n"))
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
