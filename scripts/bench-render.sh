#!/usr/bin/env bash
# Compile benchmarks/render_bench.cpp against the current debug build of daw_core
# and print the real-time-factor table. Run scripts/build-macos.sh or the debug
# preset first; nothing here is a CI gate.
set -euo pipefail
cd "$(dirname "$0")/.."
test -f build/debug/libdaw_core.a || { echo 'build/debug missing — run: cmake --preset debug && cmake --build --preset debug' >&2; exit 1; }
c++ -std=c++20 -O2 -Iengine -Iengine/bridge benchmarks/render_bench.cpp \
  build/debug/libdaw_core.a build/debug/libdaw_au_scanner.a \
  -lsqlite3 -framework AudioToolbox -framework CoreAudio -framework Foundation \
  -o build/render_bench
build/render_bench