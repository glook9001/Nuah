#!/usr/bin/env bash
set -euo pipefail

BUNDLE_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
export PATH="$BUNDLE_DIR/bin:$PATH"
export XDG_DATA_DIRS="$BUNDLE_DIR/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"

DATA_DIR="${NUAH_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/nuah}"
SOBER_DATA="$HOME/.var/app/org.vinegarhq.Sober/data/sober"
mkdir -p "$DATA_DIR"

export NUAH_ART_LIBRARY_DIR="$BUNDLE_DIR/lib/art"
export NUAH_ATL_ANDROID16_HOME="$BUNDLE_DIR/share/java/art"
export NUAH_ATL_HOME="$BUNDLE_DIR/share/java/atl"
export NUAH_ATL_NATIVE_DIR="${NUAH_ATL_NATIVE_DIR:-$DATA_DIR/base.apk_/lib}"
export NUAH_ATL_LIBRARY_DIR="$BUNDLE_DIR/lib/atl-bionic"
export NUAH_HYBRIS_LIBRARY="$BUNDLE_DIR/lib/hybris/libhybris-common.so"
export HYBRIS_LINKER_DIR="$BUNDLE_DIR/lib/hybris/libhybris/linker"
export HYBRIS_LD_LIBRARY_PATH="$BUNDLE_DIR/lib/android/linker-deps:$BUNDLE_DIR/lib/android"

export LD_LIBRARY_PATH="$BUNDLE_DIR/lib/art:$BUNDLE_DIR/share/java/art/natives:$BUNDLE_DIR/lib/hybris:$BUNDLE_DIR/lib/atl-bionic:$BUNDLE_DIR/lib/android:$BUNDLE_DIR/lib:${LD_LIBRARY_PATH:-}"

image_preload=""
for library in \
  "$BUNDLE_DIR/lib/art/libpng16.so.16" \
  "$BUNDLE_DIR/lib/art/libjpeg.so.62" \
  "$BUNDLE_DIR/lib/art/libandroidfw.so"; do
  if [[ -r "$library" ]]; then
    image_preload="${image_preload:+$image_preload:}$library"
  fi
done
if [[ -n "$image_preload" ]]; then
  export LD_PRELOAD="$image_preload${LD_PRELOAD:+:$LD_PRELOAD}"
fi

export NUAH_GRAPHICS_BACKEND="${NUAH_GRAPHICS_BACKEND:-vulkan}"
export NUAH_PERFORMANCE_MODE="${NUAH_PERFORMANCE_MODE:-quality}"
export NUAH_VULKAN_PRESENT_MODE="${NUAH_VULKAN_PRESENT_MODE:-fifo}"
export NUAH_INPUT_COALESCE="${NUAH_INPUT_COALESCE:-1}"
export NUAH_MOUSE_CAPTURE="${NUAH_MOUSE_CAPTURE:-1}"
export NUAH_FAST_RENDER="${NUAH_FAST_RENDER:-1}"

exec "$BUNDLE_DIR/bin/nuah" native-run "$@"
