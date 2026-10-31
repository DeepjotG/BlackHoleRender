#!/usr/bin/env bash
# Builds the simulator (incrementally, so edits and updates are always picked up) and starts it.
# The app picks the RTX 3060 by itself whenever the NVIDIA driver is loaded; add --igpu to force
# the integrated GPU. Any other arguments are passed through (see ./run.sh --help).
set -euo pipefail
cd "$(dirname "$0")"
if [[ ! -f build/build.ninja && ! -f build/Makefile ]]; then   # never configured, or the last configure failed
  echo "Configuring (the first run downloads GLFW, GLM, Dear ImGui, glad and stb)..."
  if [[ -f build/CMakeCache.txt ]]; then cmake -S . -B build   # keep the generator the cache recorded
  elif command -v ninja >/dev/null 2>&1; then cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  else cmake -S . -B build -DCMAKE_BUILD_TYPE=Release; fi
fi
echo "Building..."
cmake --build build
if [[ ! -e /proc/driver/nvidia/version ]]; then
  echo "NVIDIA kernel module is not loaded; running on the integrated GPU." >&2
  echo "See README.md, section 'Enabling the RTX 3060', for the one-time Secure Boot fix." >&2
fi
# VS Code's snap terminal points GTK/GIO at the snap's own libraries and newer settings schemas;
# the system GTK that draws the window title bar then aborts on a missing schema key. Put back the
# values the snap saved in *_VSCODE_SNAP_ORIG (an empty saved value means it was unset).
while IFS='=' read -r name value; do
  var=${name%_VSCODE_SNAP_ORIG}
  if [[ -n $value ]]; then export "$var=$value"; else unset "$var"; fi
  unset "$name"
done < <(env | grep '^[A-Z_]*_VSCODE_SNAP_ORIG=' || true)
echo "Starting... (Tab releases the mouse, Esc quits)"
exec build/blackhole "$@"
