#!/bin/sh
# Print decompiled functions by name from a Ghidra export.
# usage: tools/fn.sh <export-dir-name> FUN_xxx [FUN_yyy ...]
d="$(dirname "$0")/../analysis/ghidra/out/$1/decomp.c"; shift
for f in "$@"; do awk -v n="$f" '/^\/\/ ==== /{p=($3==n)} p' "$d"; done
