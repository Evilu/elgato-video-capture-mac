#!/bin/sh
# One command for the whole pipeline.
#   ./build.sh            build the driver library, command-line tools and OBS plugin
#   ./build.sh analyse    also unpack the installer and decompile it (for studying the driver;
#                         not needed to build or use the macOS driver)
#   ./build.sh install    build, then install the OBS plugin for the current user
#   ./build.sh package [version]   build the downloadable installer (out/*.pkg)
set -eu
cd "$(dirname "$0")"
case "${1:-build}" in
    analyse|analyze)
        tools/unpack.sh
        tools/decompile.sh
        ;;
esac
if [ "${1:-}" = package ]; then
    tools/package.sh "${2:-0.0.1-alpha}"
    exit 0
fi
if [ -f "$(brew --prefix 2>/dev/null || echo /opt/homebrew)/lib/libusb-1.0.dylib" ]; then
    make
else
    echo "libusb not installed (brew install libusb): skipping the command-line tools"
fi
if [ -d "${OBS_APP:-/Applications/OBS.app}" ]; then
    if [ "${1:-}" = install ]; then tools/build-obs-plugin.sh --install; else tools/build-obs-plugin.sh; fi
else
    echo "OBS Studio not found; skipped the plugin (command-line tools are in out/)"
fi
