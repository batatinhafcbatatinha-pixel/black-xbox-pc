#!/bin/sh
# Starts the native Linux build with game_files/ (your extracted disc). DXVK native and vkd3d-shader are in lib/
# next to the binary (found through its RPATH).
cd "$(dirname "$0")" || exit 1
BIN=project/out-linux/black_native
if [ -f build-location.txt ]; then
  dir=$(head -n 1 build-location.txt | tr -d '\r')
  [ -x "$dir/black_native" ] && BIN="$dir/black_native"
fi
if [ ! -x "$BIN" ]; then
  echo "Native executable not built yet. Run ./build-linux.sh" >&2
  exit 1
fi
exec "$BIN" "$PWD/game_files" "$@"
