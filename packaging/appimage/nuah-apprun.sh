#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${APPDIR:-}" ]]; then
  APPDIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
fi
export APPDIR

DATA_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}/nuah"
export NUAH_DATA_DIR="${NUAH_DATA_DIR:-$DATA_ROOT}"
mkdir -p "$NUAH_DATA_DIR"
export PATH="$APPDIR/bin:$PATH"
export XDG_DATA_DIRS="$APPDIR/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export DISPLAY="${DISPLAY:-:0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

ART_DIR="$APPDIR/lib/art"
ATL_BIO_DIR="$APPDIR/lib/atl-bionic"
HYBRIS_DIR="$APPDIR/lib/hybris"
ANDROID_DIR="$APPDIR/lib/android"
NATIVES_DIR="$APPDIR/share/java/art/natives"
ATL_DIR="$APPDIR/share/java/atl"
ISPC_DIR="$APPDIR/lib"

export NUAH_ART_LIBRARY_DIR="$ART_DIR"
export NUAH_ATL_ANDROID16_HOME="$APPDIR/share/java/art"
export NUAH_ATL_HOME="$ATL_DIR"
export NUAH_ATL_NATIVE_DIR="${NUAH_ATL_NATIVE_DIR:-$NUAH_DATA_DIR/base.apk_/lib}"
export NUAH_ATL_LIBRARY_DIR="$ATL_BIO_DIR"
export NUAH_ATL_OVERLAY_PATH="$APPDIR/lib/libnuah_atl_overlay.so"
export NUAH_HYBRIS_LIBRARY="$HYBRIS_DIR/libhybris-common.so"
export HYBRIS_LINKER_DIR="$HYBRIS_DIR/libhybris/linker"
export HYBRIS_LD_LIBRARY_PATH="$ANDROID_DIR/linker-deps:$ANDROID_DIR"

# Do not include AppImage's glibc: the host loader must remain in charge of
# Vulkan, Wayland/X11, audio, NSS and vendor driver integration.
# Prefer the host's GLib/SDL stack when it exists, because those libraries
# integrate with the host's display/audio modules. The bundled copies remain
# a fallback for systems that do not ship SDL3/GLib at all.
export LD_LIBRARY_PATH="$ART_DIR:$NATIVES_DIR:$HYBRIS_DIR:$ATL_BIO_DIR:$ANDROID_DIR:/usr/lib64:/lib64:$APPDIR/lib"

export NUAH_GRAPHICS_BACKEND="${NUAH_GRAPHICS_BACKEND:-vulkan}"
export NUAH_PERFORMANCE_MODE="${NUAH_PERFORMANCE_MODE:-quality}"
export NUAH_VULKAN_PRESENT_MODE="${NUAH_VULKAN_PRESENT_MODE:-fifo}"
export NUAH_VULKAN_SUBMIT_THREAD="${NUAH_VULKAN_SUBMIT_THREAD:-0}"
export MESA_VK_ENABLE_SUBMIT_THREAD="${MESA_VK_ENABLE_SUBMIT_THREAD:-0}"
export NUAH_INPUT_COALESCE="${NUAH_INPUT_COALESCE:-1}"
export NUAH_NONBLOCK_WAYLAND_EVENTS="${NUAH_NONBLOCK_WAYLAND_EVENTS:-0}"
export NUAH_MOUSE_CAPTURE="${NUAH_MOUSE_CAPTURE:-1}"
export NUAH_FAST_RENDER="${NUAH_FAST_RENDER:-1}"
export NUAH_ASSET_BACKGROUND="${NUAH_ASSET_BACKGROUND:-1}"
export NUAH_DISABLE_RBX_TRANSPORT_DUMMY="${NUAH_DISABLE_RBX_TRANSPORT_DUMMY:-1}"
export NUAH_TASK_THREADS="${NUAH_TASK_THREADS:-4}"
export NUAH_TARGET_FPS="${NUAH_TARGET_FPS:-60}"
export NUAH_USE_DEX2OAT="${NUAH_USE_DEX2OAT:-0}"

# Match the known-good native worker exactly. ICU is available through ART's
# normal RPATH; preloading it changes ART's global symbol ownership and can
# make Runtime::InitNativeMethods abort before the window is created.
image_preload=""
for library in "$ART_DIR/libpng16.so.16" "$ART_DIR/libjpeg.so.62" \
  "$ART_DIR/libandroidfw.so"; do
  if [[ -r "$library" ]]; then
    image_preload="${image_preload:+$image_preload:}$library"
  fi
done
if [[ -n "$image_preload" ]]; then
  export LD_PRELOAD="$image_preload${LD_PRELOAD:+:$LD_PRELOAD}"
fi

package_dir="${NUAH_PACKAGE_DIR:-$APPDIR/share/nuah/packages/x86_64/com.roblox.client}"
apk_path="${NUAH_APK_PATH:-$package_dir/base.apk}"
split_path="${NUAH_SPLIT_APK_PATH:-$package_dir/split_config.x86_64.apk}"

read_cookie() {
  if [[ -n "${NUAH_ROBLOX_COOKIE:-}" ]]; then
    printf '%s' "$NUAH_ROBLOX_COOKIE"
    return
  fi
  if [[ -n "${ROBLOX_COOKIE:-}" ]]; then
    printf '%s' "$ROBLOX_COOKIE"
    return
  fi
  local file="${NUAH_COOKIE_FILE:-}"
  if [[ -z "$file" ]]; then
    if [[ -r "$NUAH_DATA_DIR/cookies" ]]; then
      file="$NUAH_DATA_DIR/cookies"
    elif [[ -r "$HOME/.var/app/org.vinegarhq.Sober/data/sober/cookies" ]]; then
      file="$HOME/.var/app/org.vinegarhq.Sober/data/sober/cookies"
    fi
  fi
  [[ -n "$file" && -r "$file" ]] || return 0
  awk -F '\t' '$6 == ".ROBLOSECURITY" { value=$7 } END { if (value) printf "%s", value }' "$file"
}

run_rivals() {
  local threads="${1:-0}" width="${2:-1280}" height="${3:-720}"
  local cookie
  cookie="$(read_cookie)"
  [[ -n "$cookie" ]] || {
    echo "Nuah needs a Roblox session cookie. Set NUAH_ROBLOX_COOKIE or place a Netscape cookie file at $NUAH_DATA_DIR/cookies." >&2
    return 1
  }
  [[ -r "$apk_path" && -r "$split_path" ]] || {
    echo "Nuah APK pair not found: $apk_path / $split_path" >&2
    echo "Set NUAH_PACKAGE_DIR to a directory containing base.apk and split_config.x86_64.apk." >&2
    return 1
  }
  local log="/tmp/nuah-rivals-${width}x${height}-appimage-$(date +%Y%m%d-%H%M%S).log"
  echo "Launching Nuah RIVALS from AppImage at ${width}x${height}"
  echo "Frame log: $log"
  local launch=(env \
    ROBLOX_COOKIE="$cookie" \
    NUAH_ROBLOX_COOKIES=".ROBLOSECURITY=$cookie" \
    NUAH_ROBLOX_COOKIE_HEADER=".ROBLOSECURITY=$cookie" \
    NUAH_DISABLE_SESSION_DISCOVERY=1 \
    NUAH_ATL_NATIVE_DIR="$NUAH_ATL_NATIVE_DIR" \
    NUAH_HYBRIS_LIBRARY="$NUAH_HYBRIS_LIBRARY" \
    HYBRIS_LINKER_DIR="$HYBRIS_LINKER_DIR" \
    NUAH_TASK_THREADS="$threads" \
    NUAH_DESCRIPTOR_ALLOC_BATCH="${NUAH_DESCRIPTOR_ALLOC_BATCH:-0}" \
    NUAH_LIBROBLOX_MADVISE_PATCH="${NUAH_LIBROBLOX_MADVISE_PATCH:-1}" \
    NUAH_NO_SWAP="${NUAH_NO_SWAP:-0}" \
    NUAH_INTEL_NO_CCS="${NUAH_INTEL_NO_CCS:-0}" \
    NUAH_PERF_TRACE="${NUAH_PERF_TRACE:-0}" \
    NUAH_BOOTSTRAP_TRACE="${NUAH_BOOTSTRAP_TRACE:-0}" \
    NUAH_TEXTURE_MIN_LOD="${NUAH_TEXTURE_MIN_LOD:-1}" \
    NUAH_CLIENT_SETTINGS_JSON="{\"applicationSettings\":{\"DFFlagDebugDisableRbxTransportDummyClient\":true}}" \
    "$APPDIR/bin/nuah" native-run --width "$width" --height "$height" \
      --apk "$apk_path" --split "$split_path" --data "$NUAH_DATA_DIR" \
      --uri 'roblox://placeId=17625359962')
  if [[ "${NUAH_FOREGROUND_LOG:-0}" != 0 ]]; then
    "${launch[@]}"
  else
    "${launch[@]}" >"$log" 2>&1
  fi
}

case "${1:-}" in
  "")
    if [[ -r "$apk_path" && -r "$split_path" && -n "$(read_cookie)" ]]; then
      run_rivals 0 1280 720
    else
      cat <<'USAGE'
Nuah AppImage

Run RIVALS:
  NUAH_ROBLOX_COOKIE='...' ./Nuah-x86_64.AppImage run-rivals [threads] [width] [height]

Run a custom session:
  ./Nuah-x86_64.AppImage native-run --apk base.apk --split split_config.x86_64.apk ...

The package defaults to the APKs bundled in the AppImage. Override them with
NUAH_PACKAGE_DIR, NUAH_APK_PATH, and NUAH_SPLIT_APK_PATH.
USAGE
    fi
    ;;
  run-rivals|rivals)
    run_rivals "${2:-0}" "${3:-1280}" "${4:-720}"
    ;;
  native-run|config|atl-run|sober-cache-status|adopt-sober-cache)
    exec "$APPDIR/bin/nuah" "$@"
    ;;
  --help|-h)
    exec "$APPDIR/bin/nuah" --help
    ;;
  *)
    exec "$APPDIR/bin/nuah" native-run "$@"
    ;;
esac
