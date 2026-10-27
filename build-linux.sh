#!/bin/sh
# Linux build: same pipeline as build-local.cmd (needs clang, cmake, ninja, python3, SDL2, SDL2_ttf,
# DXVK native and vkd3d-shader; see README "Linux"). Extra CMake options go in BLACK_CMAKE_ARGS, e.g.
#   BLACK_CMAKE_ARGS="-DDXVK_NATIVE_DIR=/opt/dxvk-native/usr -DVKD3D_DIR=/opt/vkd3d" ./build-linux.sh
cd "$(dirname "$0")" || exit 1
exec "${PYTHON:-python3}" scripts/native_pipeline.py build
