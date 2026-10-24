#!/usr/bin/env bash
# Launches the simulator on the NVIDIA GPU when its driver is loaded (PRIME render offload),
# otherwise falls back to the default (integrated) GPU.
set -euo pipefail
cd "$(dirname "$0")"
BIN="build/blackhole"
if [[ ! -x "$BIN" ]]; then
  echo "Building..."
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build
fi
if [[ -e /proc/driver/nvidia/version ]]; then
  export __NV_PRIME_RENDER_OFFLOAD=1
  export __GLX_VENDOR_LIBRARY_NAME=nvidia
  export __VK_LAYER_NV_optimus=NVIDIA_only
  export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json
else
  echo "NVIDIA kernel module is not loaded; running on the integrated GPU." >&2
  echo "See README.md, section 'Enabling the RTX 3060', for the one-time Secure Boot fix." >&2
fi
exec "$BIN" "$@"
