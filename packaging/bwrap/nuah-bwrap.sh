#!/usr/bin/env bash
# Standalone Bubblewrap (bwrap) secure container runner for Nuah
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
APP_ROOT="${NUAH_BUNDLE_DIR:-$REPO_ROOT/dist}"

if ! command -v bwrap >/dev/null 2>&1; then
  echo "Error: bubblewrap (bwrap) is required to run in container mode." >&2
  echo "Install it via your package manager (e.g. sudo apt install bubblewrap, sudo dnf install bubblewrap, or sudo pacman -S bubblewrap)" >&2
  exit 1
fi

XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
DATA_DIR="${HOME}/.local/share/nuah"
CACHE_DIR="${HOME}/.cache/nuah"
SOBER_DIR="${HOME}/.var/app/org.vinegarhq.Sober"

mkdir -p "$DATA_DIR" "$CACHE_DIR"

BWRAP_ARGS=(
  --unshare-user
  --unshare-ipc
  --unshare-pid
  --unshare-uts
  --ro-bind /usr /usr
  --ro-bind /etc /etc
  --proc /proc
  --dev /dev
  --tmpfs /tmp
  --ro-bind "$APP_ROOT" /app
  --bind "$DATA_DIR" "$DATA_DIR"
  --bind "$CACHE_DIR" "$CACHE_DIR"
  --dir "$HOME"
  --setenv HOME "$HOME"
  --setenv USER "$USER"
  --setenv PATH "/app/bin:/usr/local/bin:/usr/bin:/bin"
  --setenv XDG_RUNTIME_DIR "$XDG_RUNTIME_DIR"
  --setenv NUAH_GRAPHICS_BACKEND "vulkan"
  --setenv NUAH_PERFORMANCE_MODE "${NUAH_PERFORMANCE_MODE:-turbo}"
  --setenv NUAH_VULKAN_PRESENT_MODE "${NUAH_VULKAN_PRESENT_MODE:-fifo}"
  --setenv NUAH_VULKAN_SUBMIT_THREAD "${NUAH_VULKAN_SUBMIT_THREAD:-1}"
  --setenv MESA_VK_ENABLE_SUBMIT_THREAD "${MESA_VK_ENABLE_SUBMIT_THREAD:-1}"
  --setenv NUAH_INPUT_COALESCE "${NUAH_INPUT_COALESCE:-1}"
  --setenv NUAH_NONBLOCK_WAYLAND_EVENTS "${NUAH_NONBLOCK_WAYLAND_EVENTS:-0}"
  --setenv NUAH_MOUSE_CAPTURE "${NUAH_MOUSE_CAPTURE:-1}"
  --setenv NUAH_FAST_RENDER "${NUAH_FAST_RENDER:-1}"
  --setenv NUAH_ASSET_BACKGROUND "${NUAH_ASSET_BACKGROUND:-1}"
  --setenv NUAH_DISABLE_RBX_TRANSPORT_DUMMY "${NUAH_DISABLE_RBX_TRANSPORT_DUMMY:-1}"
  --setenv NUAH_TASK_THREADS "${NUAH_TASK_THREADS:-4}"
  --setenv NUAH_TARGET_FPS "${NUAH_TARGET_FPS:-60}"
  --setenv NUAH_ART_LIBRARY_DIR "/app/art"
  --setenv NUAH_ATL_ANDROID16_HOME "/app/java/dex/art"
  --setenv NUAH_ATL_ANDROID16_NATIVE_DIR "/app/android"
  --setenv NUAH_ATL_HOME "/app/java/dex/android_translation_layer"
  --setenv NUAH_ATL_NATIVE_DIR "/app/android"
  --setenv NUAH_ATL_LIBRARY_DIR "/app/atl-bionic"
  --setenv NUAH_HYBRIS_LIBRARY "/app/hybris/lib/libhybris-common.so"
  --setenv HYBRIS_LINKER_DIR "/app/hybris/lib/libhybris/linker"
  --setenv HYBRIS_LD_LIBRARY_PATH "/app/android/linker-deps:/app/android"
  --setenv LD_LIBRARY_PATH "/app/art:/app/java/dex/art/natives:/app/hybris/lib:/app/atl-bionic:/app/android"
)

# Symlink compatibility paths if standard
for dir in /lib /lib64 /bin /sbin; do
  if [ -L "$dir" ]; then
    BWRAP_ARGS+=(--symlink "$(readlink "$dir")" "$dir")
  elif [ -d "$dir" ]; then
    BWRAP_ARGS+=(--ro-bind "$dir" "$dir")
  fi
done

# Windowing sockets
if [ -n "${WAYLAND_DISPLAY:-}" ] && [ -e "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
  BWRAP_ARGS+=(--bind "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" --setenv WAYLAND_DISPLAY "$WAYLAND_DISPLAY")
elif [ -e "$XDG_RUNTIME_DIR/wayland-0" ]; then
  BWRAP_ARGS+=(--bind "$XDG_RUNTIME_DIR/wayland-0" "$XDG_RUNTIME_DIR/wayland-0" --setenv WAYLAND_DISPLAY "wayland-0")
fi

if [ -d "/tmp/.X11-unix" ]; then
  BWRAP_ARGS+=(--bind /tmp/.X11-unix /tmp/.X11-unix --setenv DISPLAY "${DISPLAY:-:0}")
fi

# Audio socket
if [ -d "$XDG_RUNTIME_DIR/pulse" ]; then
  BWRAP_ARGS+=(--bind "$XDG_RUNTIME_DIR/pulse" "$XDG_RUNTIME_DIR/pulse")
fi

# GPU / DRI device access
if [ -d "/dev/dri" ]; then
  BWRAP_ARGS+=(--dev-bind /dev/dri /dev/dri)
fi

# Sober session sharing
if [ -d "$SOBER_DIR" ]; then
  BWRAP_ARGS+=(--ro-bind "$SOBER_DIR" "$SOBER_DIR")
fi

# Share network namespace for multiplayer connection
BWRAP_ARGS+=(--share-net)

exec bwrap "${BWRAP_ARGS[@]}" /app/bin/nuah native-run "$@"
