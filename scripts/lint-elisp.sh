#!/usr/bin/env bash
# Lint the Emacs Lisp: byte-compile warnings, regexp bugs, packaging
# conventions, and cross-file symbol collisions.
#
# Each of these catches something the others do not, which is the whole
# reason for running four:
#
#   byte-compile   undefined functions, wrong arities, unused
#                  variables. Notably does NOT reliably catch two files
#                  defining the same function -- see below.
#   relint         mistakes inside regexp and string literals, which
#                  byte-compile treats as opaque data.
#   package-lint   whether the package's declared Emacs version is
#                  actually enough for what the code uses. This is the
#                  one that found `defvar-keymap' (29.1) in a file
#                  claiming 28.1 -- a package that installs and then
#                  fails to load.
#   checkdoc       docstring conventions. Style, not bugs; reported but
#                  never fatal.
#   elsa           (--elsa) static type inference: unreachable
#                  branches, impossible conditions, wrong argument
#                  types. Compared against a per-file baseline, because
#                  the standing count is not zero and never will be --
#                  see the note at the bottom of this file. A file that
#                  goes UP is fatal; that is the signal worth having.
#
# The collision check lives in the ERT suite
# (scrutiny-agent-no-duplicate-definitions), because byte-compile only
# warns when the arities differ AND the files happen to be compiled in
# the order that exposes it. Same-arity collisions are silent.
#
# Usage: scripts/lint-elisp.sh [--strict] [--elsa] [--elsa-update]
#   --strict       also fail on checkdoc findings
#   --elsa         also run elsa, failing on any file above its baseline
#   --elsa-update  rewrite emacs/.elsa-baseline from this run instead
# ELSA_TIMEOUT (default 300) bounds each file's analysis; a file that
# exceeds it is recorded as not analysed, never as clean.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EMACS="${EMACS:-emacs}"
LINT_DIR="${SCRUTINY_LINT_ELPA:-${ROOT}/.lint-elpa}"
STRICT=0
STATUS=0
WITH_ELSA=0
UPDATE_BASELINE=0
BASELINE="${ROOT}/emacs/.elsa-baseline"
ELSA_TIMEOUT="${ELSA_TIMEOUT:-300}"

for arg in "$@"; do
    case "${arg}" in
        --strict) STRICT=1 ;;
        --elsa) WITH_ELSA=1 ;;
        --elsa-update) WITH_ELSA=1; UPDATE_BASELINE=1 ;;
        -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    esac
done

cd "${ROOT}/emacs"
FILES=()
for f in *.el; do
    case "${f}" in *-tests.el) continue ;; esac
    FILES+=("${f}")
done

# --- linters ---------------------------------------------------------
# Fetched into a private directory; absence is reported, never fatal, so
# this is still useful offline.
if [ ! -d "${LINT_DIR}" ]; then
    echo "==> Fetching linters into ${LINT_DIR}"
    "${EMACS}" -Q --batch --eval "(progn
        (require 'package)
        (setq package-user-dir \"${LINT_DIR}\")
        (add-to-list 'package-archives
                     '(\"melpa\" . \"https://melpa.org/packages/\"))
        (package-initialize)
        (package-refresh-contents)
        (dolist (p '(package-lint relint elsa))
          (unless (package-installed-p p)
            (ignore-errors (package-install p)))))" >/dev/null 2>&1 || true
fi

LOADS=()
for d in "${LINT_DIR}"/*/; do [ -d "${d}" ] && LOADS+=(-L "${d}"); done

have() {
    "${EMACS}" -Q --batch "${LOADS[@]}" --eval \
        "(kill-emacs (if (locate-library \"$1\") 0 1))" >/dev/null 2>&1
}

# --- byte-compile ----------------------------------------------------
echo "==> byte-compile"
out="$("${EMACS}" -Q --batch "${LOADS[@]}" -L . -f batch-byte-compile \
        "${FILES[@]}" 2>&1 | grep -vE '^$|^Wrote ')"
rm -f ./*.elc
if [ -n "${out}" ]; then echo "${out}"; STATUS=1; else echo "    clean"; fi

# --- relint ----------------------------------------------------------
echo "==> relint"
if have relint; then
    out="$("${EMACS}" -Q --batch "${LOADS[@]}" -L . -l relint \
            -f relint-batch "${FILES[@]}" 2>&1 | grep -vE '^$')"
    if [ -n "${out}" ]; then echo "${out}"; STATUS=1; else echo "    clean"; fi
else
    echo "    not installed; skipped"
fi

# --- package-lint ----------------------------------------------------
echo "==> package-lint"
if have package-lint; then
    # Without the main file it treats every file as its own package and
    # reports every cross-file symbol as a prefix violation.
    out="$("${EMACS}" -Q --batch "${LOADS[@]}" -L . -l package-lint \
            --eval '(setq package-lint-main-file "scrutiny-agent.el")' \
            -f package-lint-batch-and-exit "${FILES[@]}" 2>&1 \
            | grep -vE '^$|^Entering directory')"
    if [ -n "${out}" ]; then echo "${out}"; STATUS=1; else echo "    clean"; fi
else
    echo "    not installed; skipped"
fi

# --- checkdoc --------------------------------------------------------
echo "==> checkdoc (advisory)"
found=0
for f in "${FILES[@]}"; do
    out="$("${EMACS}" -Q --batch -L . --eval "(progn
             (require 'checkdoc)
             (setq checkdoc-verb-check-experimental-flag nil)
             (checkdoc-file \"${f}\"))" 2>&1 \
           | grep -E '^[a-z].*\.el:[0-9]+:')"
    if [ -n "${out}" ]; then echo "${out}"; found=1; fi
done
[ "${found}" -eq 0 ] && echo "    clean"
[ "${found}" -eq 1 ] && [ "${STRICT}" -eq 1 ] && STATUS=1

# --- elsa (opt-in) ---------------------------------------------------
# Run per-file. Passing several files at once makes Elsa analyse the
# whole transitive dependency graph -- Emacs core, tramp, magit -- which
# did not finish two of nine files in ten minutes. Per-file it takes
# about a second and still reports the checks worth having (unreachable
# code, impossible conditions, type mismatches).
if [ "${WITH_ELSA}" -eq 1 ]; then
    echo "==> elsa (baseline-compared)"
    if have elsa; then
        : > "${BASELINE}.new"
        for f in "${FILES[@]}"; do
            raw="$(timeout "${ELSA_TIMEOUT}" \
                    "${EMACS}" -Q --batch "${LOADS[@]}" -L . -l elsa \
                    -f elsa-run "${f}" 2>&1)"
            rc=$?
            was="$(awk -v f="${f}" '$1 == f {print $2}' "${BASELINE}" \
                   2>/dev/null)"
            was="${was:-0}"
            # A timeout must not read as a clean file. Elsa analyses the
            # transitive dependency graph, so a file that reaches magit
            # or lsp-mode can run for many minutes; record that it was
            # not analysed rather than scoring it zero.
            if [ "${rc}" -eq 124 ]; then
                printf '%s timeout\n' "${f}" >> "${BASELINE}.new"
                printf '    %-34s not analysed (timed out after %ss)\n' \
                       "${f}" "${ELSA_TIMEOUT}"
                continue
            fi
            out="$(printf '%s\n' "${raw}" | sed 's/\x1b\[[0-9;]*m//g' \
                   | grep -E ":(error|warning|notice):")"
            n="$(printf '%s' "${out}" | grep -c . || true)"
            printf '%s %s\n' "${f}" "${n}" >> "${BASELINE}.new"
            # A file recorded as never analysed has no count to regress
            # against; report it and leave the baseline to be updated.
            if [ "${was}" = "timeout" ]; then
                printf '    %-34s %s findings (was not analysed)\n' \
                       "${f}" "${n}"
                continue
            fi
            if [ "${n}" -gt "${was}" ]; then
                printf '    %-34s %s findings (baseline %s) NEW\n' \
                       "${f}" "${n}" "${was}"
                [ -n "${out}" ] && printf '%s\n' "${out}" | sed 's/^/        /'
                STATUS=1
            elif [ "${n}" -lt "${was}" ]; then
                printf '    %-34s %s findings (baseline %s) improved\n' \
                       "${f}" "${n}" "${was}"
            else
                printf '    %-34s %s findings (at baseline)\n' "${f}" "${n}"
            fi
        done
        if [ "${UPDATE_BASELINE}" -eq 1 ]; then
            mv "${BASELINE}.new" "${BASELINE}"
            echo "    baseline updated"
            STATUS=0
        else
            rm -f "${BASELINE}.new"
        fi
    else
        echo "    not installed; skipped"
    fi
fi

echo ""
if [ "${STATUS}" -eq 0 ]; then
    echo "lint: clean"
else
    echo "lint: findings above"
fi
exit "${STATUS}"

# --- a note on elsa --------------------------------------------------
#
# Elsa reports a class of bug the other linters cannot: unreachable
# branches, conditions that cannot hold, arguments of the wrong type.
# It is gated on a baseline rather than on zero because a residue of
# findings is structural -- they come from what Elsa cannot see, not
# from what this code does. Knowing which is which saves chasing them:
#
#   * `cl-defstruct' accessors are invisible to it, so every
#     `scrutiny-agent--conn-process' call is "missing arglist". Likewise
#     the first argument of `funcall', which for a callback is a
#     variable by definition.
#   * Its flow analysis does not follow `setq' inside a nested form or
#     inside a closure, so a variable assigned in an inner `when' or by
#     a callback still reads as its initial nil -- reported as an
#     impossible condition and dead code.
#   * It treats a `defcustom' default as a constant, so the other
#     branches of a test over one look unreachable.
#   * `condition-case' handler names read as function calls, so every
#     `scrutiny-agent-rpc-error' handler is "missing arglist".
#   * It reports on its own macroexpansion: a `when' comes back as
#     "useless progn around body of then branch".
#
# What the code does to meet it halfway -- each of these cut real
# findings, and each is defensible on its own terms:
#
#   * plain `defun' with `&optional' rather than `cl-defun', so both
#     Elsa and a human reader can see the arglist (this alone was 48);
#   * explicit lambdas at the call site rather than a macro that binds
#     variables for its body (62);
#   * `let' plus `when' rather than `when-let', whose binding list Elsa
#     analyses as if it were a call (also deprecated in Emacs 31);
#   * top-level helpers rather than `cl-flet';
#   * a real `require' rather than `declare-function' where the
#     dependency tree is small enough to analyse -- ls-lisp yes, tramp
#     no: Elsa's reader crashes on ansi-color.el, and a crashed
#     analyser reports nothing at all about the file;
#   * the transient menu in a file of its own, because `(require
#     \'transient)' alone exceeds five minutes and takes every file
#     that requires it out of the analysis with it (measured: a file
#     containing nothing but that require and one defun, 180s+; the
#     same file with `diff-mode', 4s).
#
# To move the baseline down, fix findings and run --elsa-update.
