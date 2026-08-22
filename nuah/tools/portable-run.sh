#!/usr/bin/env bash
# Relocatable launcher for a Nuah portable prefix. Paths are rooted at this
# script. The Roblox session cookie is read from Sober on the host; it is
# never stored in the prefix and never printed.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
NUAH="$ROOT/nuah"
HYBRIS_LIB="$ROOT/hybris/lib"
HYBRIS="$HYBRIS_LIB/libhybris-common.so"
HYBRIS_LINKER="$HYBRIS_LIB/libhybris/linker"
APK="$ROOT/apk/base.apk"
SPLIT="$ROOT/apk/split_config.x86_64.apk"
DATA=${NUAH_DATA_DIR:-$HOME/.local/share/nuah}

# ART is a host runtime (same class as Mesa): use the machine we launch on,
# never a glibc copied from the build box. ATL jars can live in the prefix.
pick_art() {
  if [[ -n ${NUAH_ART_LIBRARY_DIR:-} && -r $NUAH_ART_LIBRARY_DIR/libandroidfw.so ]]; then
    printf '%s' "$NUAH_ART_LIBRARY_DIR"
    return
  fi
  local cand
  for cand in /usr/local/lib64/art "$HOME/.local/share/nuah/art"; do
    if [[ -r $cand/libandroidfw.so && -r $cand/libart.so ]]; then
      printf '%s' "$cand"
      return
    fi
  done
  echo "host ART missing (libart.so / libandroidfw.so under /usr/local/lib64/art)" >&2
  exit 2
}
pick_atl() {
  if [[ -n ${NUAH_ATL_HOME:-} && -d $NUAH_ATL_HOME ]]; then
    printf '%s' "$NUAH_ATL_HOME"
    return
  fi
  local cand
  for cand in /usr/local/lib64/java/dex/android_translation_layer \
              "$HOME/.local/share/nuah/atl" "$ROOT/atl"; do
    if [[ -r $cand/api-impl.jar ]]; then
      printf '%s' "$cand"
      return
    fi
  done
  echo "ATL missing (api-impl.jar); put android_translation_layer in $ROOT/atl" >&2
  exit 2
}
ART=$(pick_art)
ATL=$(pick_atl)

host_so() {
  local name=$1
  local found=""
  found=$(ldconfig -p 2>/dev/null | awk -v n="$name" '$1 == n { print $NF; exit }') || true
  if [[ -z $found ]]; then
    for cand in "/lib64/$name" "/usr/lib64/$name" "/usr/lib/x86_64-linux-gnu/$name"; do
      if [[ -r $cand ]]; then
        found=$cand
        break
      fi
    done
  fi
  [[ -r ${found:-} ]] || {
    echo "host library missing: $name" >&2
    exit 1
  }
  printf '%s' "$found"
}

[[ -x $NUAH ]] || {
  echo "portable nuah missing: $NUAH" >&2
  exit 2
}
[[ -r $HYBRIS && -d $HYBRIS_LINKER ]] || {
  echo "portable hybris missing: $HYBRIS" >&2
  exit 2
}
[[ -r $APK && -r $SPLIT ]] || {
  echo "portable APK missing under $ROOT/apk" >&2
  exit 2
}

png=$(host_so libpng16.so.16)
jpeg=$(host_so libjpeg.so.62)

cookie=${NUAH_ROBLOX_COOKIE:-}
if [[ -z $cookie ]]; then
  cookie_file=${NUAH_SOBER_COOKIES:-$HOME/.var/app/org.vinegarhq.Sober/data/sober/cookies}
  cookie=$(python3 - "$cookie_file" <<'PY'
import re
import sys

try:
    contents = open(sys.argv[1], encoding="utf-8").read()
except OSError:
    raise SystemExit
matches = re.findall(r"(?:^|[;\t\s])\.ROBLOSECURITY(?:[=\t\s]+)([^;\t\s]+)", contents)
if matches:
    print(matches[-1], end="")
PY
  )
fi
[[ -n $cookie ]] || {
  echo "no Roblox session cookie; sign in through Sober or set NUAH_ROBLOX_COOKIE" >&2
  exit 1
}

width=${NUAH_WIDTH:-1280}
height=${NUAH_HEIGHT:-720}
uri=${NUAH_URI:-roblox://placeId=17625359962}
threads=${NUAH_TASK_THREADS:-0}
performance_mode=${NUAH_PERFORMANCE_MODE:-quality}
submit_thread=${NUAH_VULKAN_SUBMIT_THREAD:-0}

# ART first: libart DT_NEEDED libdl_bio from that same host tree.
ld_path="$ART"
if [[ -d $ART/natives ]]; then
  ld_path="$ld_path:$ART/natives"
fi
dex=${NUAH_ATL_ANDROID16_HOME:-/usr/local/lib64/java/dex/art}
if [[ -d $dex/natives ]]; then
  ld_path="$ld_path:$dex/natives"
  export NUAH_ATL_ANDROID16_HOME="$dex"
fi
ld_path="$ld_path:$ROOT:$HYBRIS_LIB"
if [[ -n ${LD_LIBRARY_PATH:-} ]]; then
  ld_path="$ld_path:$LD_LIBRARY_PATH"
fi
# Prefer the host pthread bridge when ART shipped it. Do not force the
# prefix copy; mixing that with host libdl_bio crashes NativeEngine init.

preload="$png:$jpeg:$ART/libandroidfw.so"
if [[ -n ${LD_PRELOAD:-} ]]; then
  preload="$preload:$LD_PRELOAD"
fi

mkdir -p "$DATA"

extra=()
if (( $# > 0 )); then
  extra=("$@")
else
  extra=(native-run --width "$width" --height "$height" --apk "$APK" --split "$SPLIT"
         --data "$DATA" --uri "$uri")
fi

export ROBLOX_COOKIE="$cookie"
export NUAH_ROBLOX_COOKIES=".ROBLOSECURITY=$cookie"
export NUAH_ROBLOX_COOKIE_HEADER=".ROBLOSECURITY=$cookie"
export NUAH_ART_HOME="$ATL"
export NUAH_ATL_HOME="$ATL"
export NUAH_ART_LIBRARY_DIR="$ART"
export NUAH_ART_LIBRARY="$ART/libart.so"
export NUAH_HYBRIS_LIBRARY="$HYBRIS"
export NUAH_HYBRIS_LIBRARY_DIR="$HYBRIS_LIB"
export HYBRIS_LINKER_DIR="$HYBRIS_LINKER"
export LD_LIBRARY_PATH="$ld_path"
export LD_PRELOAD="$preload"
export NUAH_GRAPHICS_BACKEND="${NUAH_GRAPHICS_BACKEND:-vulkan}"
export NUAH_PERFORMANCE_MODE="$performance_mode"
export NUAH_VULKAN_PRESENT_MODE="${NUAH_VULKAN_PRESENT_MODE:-fifo}"
export NUAH_VULKAN_SUBMIT_THREAD="$submit_thread"
export MESA_VK_ENABLE_SUBMIT_THREAD="$submit_thread"
export NUAH_INPUT_COALESCE="${NUAH_INPUT_COALESCE:-1}"
export NUAH_NONBLOCK_WAYLAND_EVENTS="${NUAH_NONBLOCK_WAYLAND_EVENTS:-0}"
export NUAH_MOUSE_CAPTURE="${NUAH_MOUSE_CAPTURE:-1}"
export NUAH_FAST_RENDER="${NUAH_FAST_RENDER:-1}"
export NUAH_ASSET_BACKGROUND="${NUAH_ASSET_BACKGROUND:-1}"
export NUAH_TASK_THREADS="$threads"
export NUAH_TARGET_FPS="${NUAH_TARGET_FPS:-60}"
export NUAH_SHADER_CACHE_DIR="${NUAH_SHADER_CACHE_DIR:-$DATA/base.apk_/mesa-shader-cache}"

echo "Nuah portable root: $ROOT"
echo "APK: $APK"
echo "ART (host): $ART"
echo "ATL: $ATL"
exec "$NUAH" "${extra[@]}"
