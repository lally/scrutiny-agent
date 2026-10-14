#!/bin/sh
# Fetch magit and its dependencies at the revisions the numbers in
# HANDOFF.md were measured with. Nothing is installed; bench.el and the
# ERT command lines put these directories on load-path.
set -eu
cd "$(dirname "$0")"
mkdir -p elpa && cd elpa
get() { # name url rev
    [ -d "$1" ] || git clone -q "$2" "$1"
    git -C "$1" checkout -q "$3"
}
get compat      https://github.com/emacs-compat/compat 31.1.0.0
get cond-let    https://github.com/tarsius/cond-let    v1.1.5
get llama       https://github.com/tarsius/llama       v1.0.6
get magit       https://github.com/magit/magit         58a4698
get transient   https://github.com/magit/transient     c886106
get with-editor https://github.com/magit/with-editor   3c39014
echo "deps in $(pwd)"
