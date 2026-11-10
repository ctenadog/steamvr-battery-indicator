#!/usr/bin/env bash
# One-click launcher for SteamVR Battery Indicator.
# Double-click this file or run:  ./start.sh
# Builds the program on first run (needs a C++ compiler), then starts it.
set -u

cd "$(dirname "$0")" || exit 1

BIN="./steamvr_battery_indicator"

need_build=0
if [ ! -x "$BIN" ]; then
    need_build=1
elif [ -n "$(find src -newer "$BIN" 2>/dev/null)" ]; then
    need_build=1
fi

if [ "$need_build" -eq 1 ]; then
    CXX="${CXX:-}"
    if [ -z "$CXX" ]; then
        for c in g++ clang++; do
            if command -v "$c" >/dev/null 2>&1; then CXX="$c"; break; fi
        done
    fi
    if [ -z "$CXX" ]; then
        echo "A C++ compiler (g++ or clang++) is required to build the program."
        echo "Install it, e.g.:  sudo apt install g++   (Debian/Ubuntu)"
        echo "Press Enter to close."
        read -r _
        exit 1
    fi
    echo "Building with $CXX ..."
    "$CXX" -std=c++17 -O2 -Isrc src/main.cpp -o "$BIN" -ldl || {
        echo "Build failed. Press Enter to close."
        read -r _
        exit 1
    }
fi

"$BIN" "$@"
status=$?

if [ "$status" -ne 0 ]; then
    echo
    echo "The indicator exited with code $status. Press Enter to close."
    read -r _
fi
exit "$status"
