#!/usr/bin/env bash
# RIVALS worker-budget A/B run (close game first; never prints cookie).
set -euo pipefail
die(){ echo "$2" >&2; exit "$1"; }; need_int() { [[ ${2:-} =~ ^[0-9]+$ ]] || { echo "$1 must be a non-negative integer" >&2; exit 2; }; }; need_range() { ((" $2 >= $3 && $2 <= $4 ")) || { echo "$1 must be between $3 and $4" >&2; exit 2; }; }
need_enum() { local _v=$1 _n=$2; shift 2; local _a; for _a in "$@"; do [[ $_v == "$_a" ]] && return 0; done; echo "$_n must be one of: $*" >&2; exit 2; }
need_file() { [[ -r $2 ]] || { echo "$1 missing: $2" >&2; exit 2; }; }
first_hit() { local probe=$1 dir; shift; for dir in "$@"; do [[ -r $dir/$probe ]] && { printf '%s' "$dir"; return 0; } done; return 1; }
if (( $# > 3 )); then die 2 "usage: $0 [TASK_THREADS=0] [WIDTH=1280] [HEIGHT=720]"; fi
if [[ ${NUAH_TASK_THREADS+x} ]]; then threads=$NUAH_TASK_THREADS; else threads=${1:-0}; fi
width=${2:-1280}; height=${3:-720}
performance_mode=${NUAH_PERFORMANCE_MODE:-turbo}; async_transcode=${NUAH_ASSET_TRANSCODE_ASYNC:-1}; mip_bias=${NUAH_RENDER_TEXTURE_MIP_BIAS:-1}
client_settings_default="{\"applicationSettings\":{\"DFFlagDebugDisableRbxTransportDummyClient\":true,\"FIntRenderTextureMipBias\":\"$mip_bias\"}}"
texture_budget=${NUAH_RENDER_TEXTURE_BUDGET_MS:-0}; texture_sidecar=${NUAH_TEXTURE_SIDECAR:-0}; intel_no_ccs=${NUAH_INTEL_NO_CCS:-0}; memory_low_mb=${NUAH_MEMORY_LOW_MB:-0}; memory_min_mb=${NUAH_MEMORY_MIN_MB:-0}
descriptor_batch=${NUAH_DESCRIPTOR_ALLOC_BATCH:-4}; descriptor_trace=${NUAH_DESCRIPTOR_ALLOC_TRACE:-0}; submit_thread=${NUAH_VULKAN_SUBMIT_THREAD:-1}; vulkan_icd=${VK_ICD_FILENAMES:-}; min_image_count=${NUAH_VULKAN_MIN_IMAGE_COUNT:-4}; copy_trace=${NUAH_VULKAN_COPY_TRACE:-0}
texture_trace=${NUAH_TEXTURE_UPLOAD_TRACE:-0}; texture_hash_trace=${NUAH_TEXTURE_UPLOAD_HASH_TRACE:-0}; texture_dedup=${NUAH_TEXTURE_UPLOAD_DEDUP:-1}; dynamic_kpx=${NUAH_DYNAMIC_RENDER_KILOPIXELS:-460}
upload_fingerprint=${NUAH_UPLOAD_FINGERPRINT:-0}; madvise_patch=${NUAH_LIBROBLOX_MADVISE_PATCH:-0}; asset_background=${NUAH_ASSET_BACKGROUND:-1}; texture_min_lod=${NUAH_TEXTURE_MIN_LOD:-1}
descriptor_bind_dedup=${NUAH_DESCRIPTOR_BIND_DEDUP:-1}; command_state_dedup=${NUAH_COMMAND_STATE_DEDUP:-1}; disable_msaa=${NUAH_DISABLE_MSAA:-1}; frm_quality=${NUAH_FRM_QUALITY:-1}; perf_trace=${NUAH_PERF_TRACE:-1}; android_preload=${NUAH_ANDROID_PRELOAD:-}
for value in "$threads" "$width" "$height" "$async_transcode" "$mip_bias" "$texture_budget" "$texture_sidecar" "$intel_no_ccs" "$memory_low_mb" "$memory_min_mb" "$descriptor_batch" "$descriptor_trace" "$min_image_count" "$copy_trace" "$texture_trace" "$texture_hash_trace" "$texture_dedup" "$upload_fingerprint" "$madvise_patch" "$asset_background" "$descriptor_bind_dedup" "$command_state_dedup" "$disable_msaa" "$frm_quality"; do need_int "worker count/dimensions" "$value"; done
(( threads <= 64 && width >= 320 && height >= 200 )) || die 2 "invalid worker count or dimensions"
need_enum "$performance_mode" "NUAH_PERFORMANCE_MODE" quality turbo; need_enum "$perf_trace" "NUAH_PERF_TRACE" 0 1; need_enum "$asset_background" "NUAH_ASSET_BACKGROUND" 0 1; need_enum "$submit_thread" "NUAH_VULKAN_SUBMIT_THREAD" 0 1 auto; if (( threads == 2 )); then die 2 "TASK_THREADS=2 is disabled because it causes frame hitches; use 0 for automatic workers"; fi
need_range "NUAH_MEMORY_LOW_MB" "$memory_low_mb" 0 4096; need_range "NUAH_MEMORY_MIN_MB" "$memory_min_mb" 0 4096; need_range "NUAH_DESCRIPTOR_ALLOC_BATCH" "$descriptor_batch" 0 16; need_range "NUAH_VULKAN_MIN_IMAGE_COUNT" "$min_image_count" 1 64
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
nuah_binary=${NUAH_BINARY:-$repo_root/build/nuah}; nuah_data=${NUAH_DATA_DIR:-$HOME/.local/share/nuah}
lock_file="$nuah_data/nuah-runtime.lock"
art_library_dir=${NUAH_ART_LIBRARY_DIR:-}
if [[ -z $art_library_dir ]]; then art_library_dir=$(first_hit libandroidfw.so /usr/local/lib64/art "$HOME/.local/share/nuah/art" "$repo_root/art" "$repo_root/build/art") || true; art_library_dir=${art_library_dir:-/usr/local/lib64/art}; fi
atl_home=${NUAH_ART_HOME:-${NUAH_ATL_HOME:-/usr/local/lib64/java/dex/android_translation_layer}}
hybris_library_dir=""; if [[ -n "${NUAH_HYBRIS_LIBRARY_DIR:-}" ]]; then hybris_library_dir="$NUAH_HYBRIS_LIBRARY_DIR"; else hybris_library_dir="$repo_root/build/hybris/lib"; fi
hybris_library=${NUAH_HYBRIS_LIBRARY:-$hybris_library_dir/libhybris-common.so}; hybris_linker_dir=${HYBRIS_LINKER_DIR:-$hybris_library_dir/libhybris/linker}
vulkan_library_dir=${NUAH_VULKAN_LIBRARY_DIR:-}
[[ -x "$nuah_binary" ]] || die 2 "build/nuah is missing; build Nuah first"
need_file "ART runtime is" "$art_library_dir/libandroidfw.so"
[[ -r "$hybris_library" && -d "$hybris_linker_dir" ]] || die 2 "libhybris runtime is missing: $hybris_library / $hybris_linker_dir"
if [[ -n "$vulkan_library_dir" ]]; then need_file "Vulkan host loader is" "$vulkan_library_dir/libvulkan.so.1"; fi
if [[ -e $lock_file ]] && lsof "$lock_file" >/dev/null 2>&1; then die 1 "Nuah already owns $lock_file; close that session before an A/B run"; fi
sidecar_source="$nuah_data/base.apk_/files/appData/rbx-storage.db"
sidecar_dir=""; sidecar_data=""; sidecar_db=""; sidecar_manifest=""; sidecar_ready=""; runtime_data="$nuah_data"
if (( texture_sidecar != 0 )); then
  [[ -r "$sidecar_source" ]] || die 1 "Roblox rbx-storage database is missing: $sidecar_source"
  sidecar_key=$(sha256sum "$sidecar_source" | awk '{print substr($1, 1, 16)}')
  sidecar_dir="$nuah_data/nuah-texture-sidecar/potato4-$sidecar_key"; sidecar_data="$sidecar_dir/runtime-data"
  sidecar_db="$sidecar_data/base.apk_/files/appData/rbx-storage.db"; sidecar_manifest="$sidecar_db.nuah-manifest.json"; sidecar_ready="$sidecar_data/.nuah-potato4-ready"
  if [[ ! -f "$sidecar_ready" || ! -f "$sidecar_db" || ! -f "$sidecar_manifest" ]]; then
    echo "Cloning the Nuah app-data tree with Btrfs copy-on-write..."; mkdir -p "$sidecar_data"
    cp -a --reflink=always "$nuah_data/base.apk_/." "$sidecar_data/base.apk_/"
    echo "Building separate potato4 KTX2 SQLite cache..."
    python3 "$repo_root/nuah/tools/build-lossy-ktx-sidecar.py" "$sidecar_source" "$sidecar_db.derived" --drop-mips 4 --profile potato4
    mv -f "$sidecar_db.derived" "$sidecar_db"; mv -f "$sidecar_db.derived.nuah-manifest.json" "$sidecar_manifest"; touch "$sidecar_ready"
  fi
  profile=$(python3 - "$sidecar_manifest" <<'PY'
import json, sys
try: print(json.load(open(sys.argv[1], encoding="utf-8"))["profile"], end="")
except (OSError, KeyError, ValueError): pass
PY
)
  [[ $profile == potato4 ]] || die 1 "invalid SQLite sidecar manifest at $sidecar_manifest"
  texture_min_lod=0; runtime_data="$sidecar_data"
fi
apk_path=${NUAH_APK_PATH:-}; split_apk_path=${NUAH_SPLIT_APK_PATH:-}
[[ -r "$apk_path" && -r "$split_apk_path" ]] || die 1 "Nuah APK/split not found (set NUAH_APK_PATH / NUAH_SPLIT_APK_PATH): $apk_path / $split_apk_path"
cookie=${NUAH_ROBLOX_COOKIE:-}
[[ -n $cookie ]] || die 1 "no Roblox session cookie (set NUAH_ROBLOX_COOKIE)"
timestamp=$(date +%Y%m%d-%H%M%S); log="/tmp/nuah-rivals-${width}x${height}-workers${threads}-${timestamp}.log"
echo "Launching RIVALS at ${width}x${height} with NUAH_TASK_THREADS=${threads}"; echo "Performance mode: $performance_mode"; echo "RuntimeContent async transcode: $async_transcode"; echo "Roblox RenderTexture mip bias: $mip_bias"; echo "Roblox RenderTexture budget: ${texture_budget} ms"; echo "KTX2 potato4 SQLite app-data profile: $texture_sidecar"
echo "Intel Gen9 CCS diagnostic disable: $intel_no_ccs"; echo "Cgroup memory.low protection: ${memory_low_mb} MiB"; echo "Cgroup memory.min protection: ${memory_min_mb} MiB"; echo "Descriptor allocator batch: $descriptor_batch"; echo "Descriptor allocator trace: $descriptor_trace"; echo "Mesa submit thread request: $submit_thread"
echo "Vulkan minimum swapchain images: $min_image_count"; echo "Vulkan ICD override: ${vulkan_icd:-system default}"; echo "Vulkan image-copy trace: $copy_trace"; echo "Texture upload trace/hash/dedup: $texture_trace/$texture_hash_trace/$texture_dedup"; echo "Upload fingerprint: $upload_fingerprint"; echo "libroblox madvise patch: $madvise_patch"
echo "Asset background scheduling: $asset_background"; echo "Texture minimum LOD: $texture_min_lod"; echo "Perf trace: $perf_trace"; echo "Frame log: $log"
cd "$repo_root"
launch_prefix=(setsid -f)
if [[ ${NUAH_NO_SWAP:-0} != 0 || $memory_low_mb != 0 || $memory_min_mb != 0 ]]; then
  command -v systemd-run >/dev/null 2>&1 || die 1 "NUAH_NO_SWAP=1 requires systemd-run"
  launch_prefix=(setsid -f systemd-run --user --scope --quiet)
  if [[ ${NUAH_NO_SWAP:-0} != 0 ]]; then launch_prefix+=(-p MemorySwapMax=0); echo "Per-process swap disabled with MemorySwapMax=0 (zram remains global)"; fi
  if (( memory_low_mb != 0 )); then launch_prefix+=(-p "MemoryLow=${memory_low_mb}M"); echo "Cgroup memory.low set to ${memory_low_mb} MiB"; fi
  if (( memory_min_mb != 0 )); then launch_prefix+=(-p "MemoryMin=${memory_min_mb}M"); echo "Cgroup memory.min set to ${memory_min_mb} MiB (OOM tradeoff)"; fi
fi
intel_debug=${INTEL_DEBUG:-}; vulkan_icd_env=(); [[ -n $vulkan_icd ]] && vulkan_icd_env+=("VK_ICD_FILENAMES=$vulkan_icd"); if (( intel_no_ccs != 0 )); then if [[ -n $intel_debug ]]; then intel_debug="$intel_debug,noccs"; else intel_debug="noccs"; fi; fi
icu_preload=""; for icu_lib in "$art_library_dir/libicudata.so.77" "$art_library_dir/libicuuc.so.77" "$art_library_dir/libicui18n.so.77"; do [[ -r "$icu_lib" ]] && icu_preload="${icu_preload:+$icu_preload:}$icu_lib"; done
"${launch_prefix[@]}" env ROBLOX_COOKIE="$cookie" NUAH_ROBLOX_COOKIES=".ROBLOSECURITY=$cookie" NUAH_ROBLOX_COOKIE_HEADER=".ROBLOSECURITY=$cookie" \
  ${NUAH_ROBLOX_USER_ID:+NUAH_ROBLOX_USER_ID="$NUAH_ROBLOX_USER_ID"} NUAH_FRM_QUALITY="$frm_quality" NUAH_ASSET_ROOTS="${NUAH_ASSET_ROOTS:-}" NUAH_CLIENT_SETTINGS_JSON="${NUAH_CLIENT_SETTINGS_JSON:-$client_settings_default}" \
  NUAH_ART_HOME="$atl_home" NUAH_ATL_HOME="$atl_home" NUAH_HYBRIS_LIBRARY="$hybris_library" HYBRIS_LINKER_DIR="$hybris_linker_dir" LD_LIBRARY_PATH="$repo_root/build:$repo_root/build/bionic-translation:$art_library_dir:$art_library_dir/natives:$hybris_library_dir${vulkan_library_dir:+:$vulkan_library_dir}" LD_PRELOAD="${icu_preload:+$icu_preload:}${android_preload:+$android_preload:}/usr/lib64/libpng16.so.16:/usr/lib64/libjpeg.so.62:$art_library_dir/libandroidfw.so" \
  NUAH_DISABLE_MSAA="$disable_msaa" ${NUAH_DISABLE_TEXTURE_PACK_GENERATOR:+NUAH_DISABLE_TEXTURE_PACK_GENERATOR="$NUAH_DISABLE_TEXTURE_PACK_GENERATOR"} NUAH_GRAPHICS_BACKEND=vulkan INTEL_DEBUG="$intel_debug" NUAH_PERFORMANCE_MODE="$performance_mode" NUAH_VULKAN_PRESENT_MODE="${NUAH_VULKAN_PRESENT_MODE:-mailbox}" NUAH_VULKAN_SUBMIT_THREAD="$submit_thread" "${vulkan_icd_env[@]}" \
  NUAH_VULKAN_MIN_IMAGE_COUNT="$min_image_count" NUAH_RENDER_TEXTURE_BUDGET_MS="$texture_budget" MESA_VK_ENABLE_SUBMIT_THREAD="$submit_thread" NUAH_VULKAN_COPY_TRACE="$copy_trace" NUAH_TEXTURE_UPLOAD_TRACE="$texture_trace" NUAH_TEXTURE_UPLOAD_HASH_TRACE="$texture_hash_trace" NUAH_TEXTURE_UPLOAD_DEDUP="$texture_dedup" \
  NUAH_DYNAMIC_RENDER_KILOPIXELS="$dynamic_kpx" NUAH_UPLOAD_FINGERPRINT="$upload_fingerprint" NUAH_LIBROBLOX_MADVISE_PATCH="$madvise_patch" NUAH_INPUT_COALESCE=1 NUAH_NONBLOCK_WAYLAND_EVENTS=0 NUAH_MOUSE_CAPTURE=1 NUAH_FAST_RENDER=1 \
  NUAH_ASSET_BACKGROUND="$asset_background" NUAH_TASK_THREADS="$threads" NUAH_DESCRIPTOR_ALLOC_BATCH="$descriptor_batch" NUAH_DESCRIPTOR_ALLOC_TRACE="$descriptor_trace" NUAH_ASSET_TRANSCODE_ASYNC="$async_transcode" NUAH_DESCRIPTOR_BIND_DEDUP="$descriptor_bind_dedup" NUAH_COMMAND_STATE_DEDUP="$command_state_dedup" \
  NUAH_TARGET_FPS="${NUAH_TARGET_FPS:-240}" NUAH_TEXTURE_MIN_LOD="$texture_min_lod" NUAH_TEXTURE_SIDECAR=0 NUAH_PERF_TRACE="$perf_trace" NUAH_SHADER_CACHE_DIR="$nuah_data/base.apk_/mesa-shader-cache" MESA_SHADER_CACHE_DIR="$nuah_data/base.apk_/mesa-shader-cache" MESA_SHADER_CACHE_DISABLE=false \
  MESA_VK_WSI_PRESENT_MODE="${NUAH_VULKAN_PRESENT_MODE:-mailbox}" ANV_ENABLE_PIPELINE_CACHE=1 vblank_mode=0 "$nuah_binary" native-run --width "$width" --height "$height" --apk "$apk_path" --split "$split_apk_path" --data "$runtime_data" --uri 'roblox://placeId=17625359962' >"$log" 2>&1
