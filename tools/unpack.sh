#!/bin/sh
# Unpacks the Elgato driver installer into analysis/unpacked/{x64,x86}.
#   tools/unpack.sh [path/to/Video_Capture_Driver_Installer.exe]
# Needs 7-Zip (brew install sevenzip). Nothing it produces is committed.
set -eu
cd "$(dirname "$0")/.."
EXE="${1:-Video_Capture_Driver_Installer.exe}"
[ -f "$EXE" ] || { echo "installer not found: $EXE (download it from Elgato and put it here)" >&2; exit 1; }
command -v 7zz >/dev/null || { echo "7zz not found: brew install sevenzip" >&2; exit 1; }

OUT=analysis/unpacked
rm -rf "$OUT"
mkdir -p "$OUT/x64" "$OUT/x86"
# The archive holds an x64 set followed by an x86 set with the same file names;
# -aou keeps both by renaming the second copy to name_1.ext.
7zz x -y -aou -o"$OUT" "$EXE" >/dev/null
cd "$OUT"
for f in *_1.*; do
    [ -e "$f" ] || continue
    base="$(printf %s "$f" | sed 's/_1\././')"
    mv "$base" x64/
    mv "$f" "x86/$base"
done
for f in emBDA64.sys emOEM64.sys emPRP64.ax; do [ -e "$f" ] && mv "$f" x64/; done
for f in emBDA.sys emOEM.sys; do [ -e "$f" ] && mv "$f" x86/; done
echo "unpacked into $OUT:"
shasum -a 256 x64/elvidcap.sys x86/elvidcap.sys
