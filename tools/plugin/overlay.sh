#!/usr/bin/env bash
# Builds tools/plugin/overlay_plugin.c (the overlay of plugin API 5: triangles, a texture, the input, the game's
# picture) into an importable mod: build/mods-import/overlay-example/plugin/ and its .zip next to it.
# Usage: tools/plugin/overlay.sh            the Linux .so, with cc
#        CC=x86_64-w64-mingw32-clang tools/plugin/overlay.sh dll      the Windows .dll, with a MinGW compiler
set -euo pipefail
cd "$(dirname "$0")/../.."
kind=${1:-so}
root=build/mods-import
mod=$root/overlay-example
rm -rf "$mod" "$root/overlay-example.zip"
mkdir -p "$mod/plugin"
flags=(-std=c11 -O2 -Wall -Wextra -Werror -shared -fvisibility=hidden -I runtime/plugin)
if [ "$kind" = dll ]; then
  "${CC:-x86_64-w64-mingw32-clang}" "${flags[@]}" tools/plugin/overlay_plugin.c -o "$mod/plugin/overlay_plugin.dll"
else
  "${CC:-cc}" "${flags[@]}" -fPIC tools/plugin/overlay_plugin.c -o "$mod/plugin/overlay_plugin.so"
fi
if command -v zip >/dev/null; then (cd "$root" && zip -qr overlay-example.zip overlay-example); fi
echo "$mod"
