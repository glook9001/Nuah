#!/usr/bin/env bash
# Relocatable launcher for a Nuah portable prefix (cookie never stored/printed).
set -euo pipefail
die(){ echo "$2" >&2; exit "$1"; }
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); NUAH="$ROOT/nuah"; HYBRIS_LIB="$ROOT/hybris/lib"; HYBRIS="$HYBRIS_LIB/libhybris-common.so"; HYBRIS_LINKER="$HYBRIS_LIB/libhybris/linker"; APK="$ROOT/apk/base.apk"; SPLIT="$ROOT/apk/split_config.x86_64.apk"; DATA=${NUAH_DATA_DIR:-$HOME/.local/share/nuah}
first_hit() { local probe=$1 dir; shift; for dir in "$@"; do [[ -r $dir/$probe ]] && { printf '%s' "$dir"; return 0; } done; return 1; }
pick_art() { [[ -n ${NUAH_ART_LIBRARY_DIR:-} && -r $NUAH_ART_LIBRARY_DIR/libandroidfw.so ]] && { printf '%s' "$NUAH_ART_LIBRARY_DIR"; return; }; local dir; for dir in /usr/local/lib64/art "$HOME/.local/share/nuah/art"; do if [[ -r $dir/libandroidfw.so && -r $dir/libart.so ]]; then printf '%s' "$dir"; return; fi; done; die 2 "host ART missing (libart.so / libandroidfw.so under /usr/local/lib64/art)"; }
pick_atl() { [[ -n ${NUAH_ATL_HOME:-} && -d $NUAH_ATL_HOME ]] && { printf '%s' "$NUAH_ATL_HOME"; return; }; local found=""; found=$(first_hit api-impl.jar /usr/local/lib64/java/dex/android_translation_layer "$HOME/.local/share/nuah/atl" "$ROOT/atl") || true; [[ -n $found ]] && { printf '%s' "$found"; return; }; die 2 "ATL missing (api-impl.jar); put android_translation_layer in $ROOT/atl"; }
ART=$(pick_art); ATL=$(pick_atl)
host_so() { local name=$1 found=""; found=$(ldconfig -p 2>/dev/null | awk -v n="$name" '$1 == n { print $NF; exit }') || true; [[ -n $found ]] || found=$(first_hit "$name" /lib64 /usr/lib64 /usr/lib/x86_64-linux-gnu) || true; [[ -r ${found:-} ]] || die 1 "host library missing: $name"; printf '%s' "$found"; }
need_exec() { [[ -x $2 ]] || die 2 "$1 missing: $2"; }; need_hybris() { [[ -r $HYBRIS && -d $HYBRIS_LINKER ]] || die 2 "portable hybris missing: $HYBRIS"; }; need_apk() { [[ -r $APK && -r $SPLIT ]] || die 2 "portable APK missing under $ROOT/apk"; }
need_exec "portable nuah" "$NUAH"; need_hybris; need_apk
png=$(host_so libpng16.so.16); jpeg=$(host_so libjpeg.so.62)
cookie=${NUAH_ROBLOX_COOKIE:-}
[[ -n $cookie ]] || die 1 "no Roblox session cookie; set NUAH_ROBLOX_COOKIE"
width=${NUAH_WIDTH:-1280}; height=${NUAH_HEIGHT:-720}; uri=${NUAH_URI:-roblox://placeId=17625359962}; threads=${NUAH_TASK_THREADS:-0}; performance_mode=${NUAH_PERFORMANCE_MODE:-quality}; submit_thread=${NUAH_VULKAN_SUBMIT_THREAD:-0}
ld_path="$ART"; [[ -d $ART/natives ]] && ld_path="$ld_path:$ART/natives"
dex=${NUAH_ATL_ANDROID16_HOME:-/usr/local/lib64/java/dex/art}; if [[ -d $dex/natives ]]; then ld_path="$ld_path:$dex/natives"; export NUAH_ATL_ANDROID16_HOME="$dex"; fi
ld_path="$ld_path:$ROOT:$HYBRIS_LIB"; [[ -n ${LD_LIBRARY_PATH:-} ]] && ld_path="$ld_path:$LD_LIBRARY_PATH"
preload="$png:$jpeg:$ART/libandroidfw.so"; [[ -n ${LD_PRELOAD:-} ]] && preload="$preload:$LD_PRELOAD"
mkdir -p "$DATA"; extra=()
if (( $# > 0 )); then extra=("$@"); else extra=(native-run --width "$width" --height "$height" --apk "$APK" --split "$SPLIT" --data "$DATA" --uri "$uri"); fi
export ROBLOX_COOKIE="$cookie" NUAH_ROBLOX_COOKIES=".ROBLOSECURITY=$cookie" NUAH_ROBLOX_COOKIE_HEADER=".ROBLOSECURITY=$cookie" NUAH_ART_HOME="$ATL" NUAH_ATL_HOME="$ATL" NUAH_ART_LIBRARY_DIR="$ART" NUAH_ART_LIBRARY="$ART/libart.so"
export NUAH_HYBRIS_LIBRARY="$HYBRIS" NUAH_HYBRIS_LIBRARY_DIR="$HYBRIS_LIB" HYBRIS_LINKER_DIR="$HYBRIS_LINKER" LD_LIBRARY_PATH="$ld_path" LD_PRELOAD="$preload" NUAH_GRAPHICS_BACKEND="${NUAH_GRAPHICS_BACKEND:-vulkan}" NUAH_PERFORMANCE_MODE="$performance_mode"
export NUAH_VULKAN_PRESENT_MODE="${NUAH_VULKAN_PRESENT_MODE:-fifo}" NUAH_VULKAN_SUBMIT_THREAD="$submit_thread" MESA_VK_ENABLE_SUBMIT_THREAD="$submit_thread" NUAH_INPUT_COALESCE="${NUAH_INPUT_COALESCE:-1}" NUAH_NONBLOCK_WAYLAND_EVENTS="${NUAH_NONBLOCK_WAYLAND_EVENTS:-0}" NUAH_MOUSE_CAPTURE="${NUAH_MOUSE_CAPTURE:-1}"
export NUAH_FAST_RENDER="${NUAH_FAST_RENDER:-1}" NUAH_ASSET_BACKGROUND="${NUAH_ASSET_BACKGROUND:-1}" NUAH_TASK_THREADS="$threads" NUAH_TARGET_FPS="${NUAH_TARGET_FPS:-60}" NUAH_SHADER_CACHE_DIR="${NUAH_SHADER_CACHE_DIR:-$DATA/base.apk_/mesa-shader-cache}"; echo "Nuah portable root: $ROOT"; echo "APK: $APK"; echo "ART (host): $ART"; echo "ATL: $ATL"
exec "$NUAH" "${extra[@]}"
