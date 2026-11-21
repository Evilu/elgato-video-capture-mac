#!/bin/sh
# Runs Ghidra headless over the driver binaries and exports text for analysis:
#   analysis/ghidra/out/<name>/{decomp.c,functions.tsv,strings.tsv,imports.tsv}
#   tools/decompile.sh                 the Conexant driver (both architectures) + property page
#   tools/decompile.sh x64/emBDA64.sys any other unpacked file
# Needs Ghidra and a JDK 21 (brew install ghidra openjdk@21). Run tools/unpack.sh first.
set -eu
cd "$(dirname "$0")/.."
BREW="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
: "${JAVA_HOME:=$BREW/opt/openjdk@21}"
export JAVA_HOME
HEADLESS="${GHIDRA_HEADLESS:-$(ls "$BREW"/Cellar/ghidra/*/libexec/support/analyzeHeadless 2>/dev/null | tail -1)}"
[ -x "$HEADLESS" ] || { echo "Ghidra analyzeHeadless not found (set GHIDRA_HEADLESS=...)" >&2; exit 1; }
[ -d analysis/unpacked ] || { echo "run tools/unpack.sh first" >&2; exit 1; }

[ $# -gt 0 ] || set -- x64/elvidcap.sys x86/elvidcap.sys x64/CxPolaris.ax
mkdir -p analysis/ghidra/proj analysis/ghidra/out
for t in "$@"; do
    name="$(printf %s "$t" | tr '/.' '__')"
    case "$(file -b "analysis/unpacked/$t")" in
        PE32+*) proc=x86:LE:64:default ;;
        *) proc=x86:LE:32:default ;;
    esac
    echo "analysing $t ($proc)"
    "$HEADLESS" analysis/ghidra/proj evc -import "analysis/unpacked/$t" -overwrite -processor "$proc" \
        -scriptPath tools/ghidra -postScript ExportAll.java "$PWD/analysis/ghidra/out/$name" \
        > "analysis/ghidra/out/$name.log" 2>&1 || { echo "  failed, see analysis/ghidra/out/$name.log" >&2; exit 1; }
    echo "  $(grep -c '^// ==== ' "analysis/ghidra/out/$name/decomp.c") functions -> analysis/ghidra/out/$name/"
done
