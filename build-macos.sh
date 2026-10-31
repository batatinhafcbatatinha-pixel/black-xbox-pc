#!/bin/sh
# macOS build (x86-64: Intel Macs, or Rosetta 2 on Apple Silicon). Same pipeline as build-linux.sh; see MACOS.md.
#   DEPS=~/.local/share/native-ports-macos ./build-macos.sh
cd "$(dirname "$0")" || exit 1
DEPS="${DEPS:-$HOME/.local/share/native-ports-macos}"
BLACK_CMAKE_ARGS="-DDXMT_DIR=$DEPS/dxmt -DVKD3D_DIR=$DEPS/vkd3d -DCMAKE_OSX_ARCHITECTURES=x86_64 $BLACK_CMAKE_ARGS"
export BLACK_CMAKE_ARGS
exec arch -x86_64 "${PYTHON:-/usr/local/bin/python3}" scripts/native_pipeline.py build
