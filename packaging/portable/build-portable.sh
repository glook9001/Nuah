#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUT_DIR="${NUAH_PORTABLE_OUT:-$REPO_ROOT/dist-portable/nuah-portable}"
TAR_OUT="${REPO_ROOT}/dist-portable/Nuah-Linux-x86_64.tar.gz"

echo "==> Creating clean portable bundle in $OUT_DIR..."
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"/{bin,lib,lib/host,share/java/art,share/java/atl/natives,share/bionic_translation/cfg.d}

# Copy binaries directly
cp -a "$REPO_ROOT/build/nuah" "$OUT_DIR/bin/nuah"
cp -a "$REPO_ROOT/build/nuah-services" "$OUT_DIR/bin/nuah-services"

for lib in libnuah_host_bridge.so libnuah_atl_resources.so libnuah_atl_overlay.so; do
  if [[ -f "$REPO_ROOT/build/$lib" ]]; then
    cp -a "$REPO_ROOT/build/$lib" "$OUT_DIR/lib/$lib"
    cp -a "$REPO_ROOT/build/$lib" "$OUT_DIR/bin/$lib"
  fi
done

# Copy matched runtime libraries exactly as built
cp -a "$REPO_ROOT/dist/art/." "$OUT_DIR/lib/art/" 2>/dev/null || true
if [[ -d "$REPO_ROOT/release/nuah-rivals-turbo-1220x980/art" ]]; then
  cp -a "$REPO_ROOT/release/nuah-rivals-turbo-1220x980/art/." "$OUT_DIR/lib/art/"
fi
rm -f "$OUT_DIR/lib/art/libc_bio.so.0" "$OUT_DIR/lib/art/libpthread_bio.so.0"

if [[ -d "$REPO_ROOT/build/atl-bionic" ]]; then
  cp -a "$REPO_ROOT/build/atl-bionic/." "$OUT_DIR/lib/atl-bionic/"
elif [[ -d "$REPO_ROOT/dist/atl-bionic" ]]; then
  cp -a "$REPO_ROOT/dist/atl-bionic/." "$OUT_DIR/lib/atl-bionic/"
fi

# Ensure libdl_bio.so.0 is available in art lib dir for libopenjdkjvm
cp -af "$OUT_DIR/lib/atl-bionic/libdl_bio.so.0" "$OUT_DIR/lib/art/libdl_bio.so.0"

cp -a "$REPO_ROOT/dist/hybris/lib/." "$OUT_DIR/lib/hybris/"
cp -a "$REPO_ROOT/dist/android/." "$OUT_DIR/lib/android/"
cp -a "$REPO_ROOT/dist/java/dex/art/." "$OUT_DIR/share/java/art/"

# Ensure JNI companion DSOs are present in art lib dir
for soname in libnativehelper.so libopenjdkjvm.so libjavacore.so libopenjdk.so; do
  if [[ -r "$REPO_ROOT/dist/java/dex/art/natives/$soname" ]]; then
    cp -af "$REPO_ROOT/dist/java/dex/art/natives/$soname" "$OUT_DIR/lib/art/$soname"
  fi
done

ATL_SOURCE="$REPO_ROOT/dist/java/dex/android_translation_layer"
cp -a "$ATL_SOURCE/." "$OUT_DIR/share/java/atl/"

for atl_native in libandroid.so.0 libtranslation_layer_main.so; do
  if [[ -r "$ATL_SOURCE/natives/$atl_native" ]]; then
    cp -a "$ATL_SOURCE/natives/$atl_native" "$OUT_DIR/share/java/atl/natives/"
  elif [[ -r "$ATL_SOURCE/$atl_native" ]]; then
    cp -a "$ATL_SOURCE/$atl_native" "$OUT_DIR/share/java/atl/natives/"
  fi
done

if [[ -r "$REPO_ROOT/dist/java/dex/android_translation_layer/framework-res.apk" ]]; then
  cp -a "$REPO_ROOT/dist/java/dex/android_translation_layer/framework-res.apk" "$OUT_DIR/share/java/atl/"
fi

cp -a "$REPO_ROOT/dist/share/bionic_translation/cfg.d/." "$OUT_DIR/share/bionic_translation/cfg.d/" 2>/dev/null || true

# Symlinks for loader discovery
ln -sf ../lib/android "$OUT_DIR/bin/android"
ln -sf ../lib/hybris "$OUT_DIR/bin/hybris"
ln -sf ../lib/atl-bionic "$OUT_DIR/bin/bionic-translation"
ln -sf ../../share/java/art/natives "$OUT_DIR/lib/art/natives"

cat <<'SCRIPT' > "$OUT_DIR/nuah"
#!/usr/bin/env bash
set -euo pipefail

BUNDLE_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
export PATH="$BUNDLE_DIR/bin:$PATH"
export XDG_DATA_DIRS="$BUNDLE_DIR/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"

DATA_DIR="${NUAH_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/nuah}"
mkdir -p "$DATA_DIR"

ART_DIR="$BUNDLE_DIR/lib/art"
ATL_BIO_DIR="$BUNDLE_DIR/lib/atl-bionic"
HYBRIS_DIR="$BUNDLE_DIR/lib/hybris"
ANDROID_DIR="$BUNDLE_DIR/lib/android"
NATIVES_DIR="$BUNDLE_DIR/share/java/art/natives"
ATL_DIR="$BUNDLE_DIR/share/java/atl"

export NUAH_ART_LIBRARY_DIR="$ART_DIR"
export NUAH_ATL_ANDROID16_HOME="$BUNDLE_DIR/share/java/art"
export NUAH_ATL_HOME="$ATL_DIR"
export NUAH_ATL_NATIVE_DIR="${NUAH_ATL_NATIVE_DIR:-$DATA_DIR/base.apk_/lib}"
export NUAH_ATL_LIBRARY_DIR="$ATL_BIO_DIR"
export NUAH_HYBRIS_LIBRARY="$HYBRIS_DIR/libhybris-common.so"
export HYBRIS_LINKER_DIR="$HYBRIS_DIR/libhybris/linker"
export HYBRIS_LD_LIBRARY_PATH="$ANDROID_DIR/linker-deps:$ANDROID_DIR"

export NUAH_ART_USE_BOOT_IMAGE="${NUAH_ART_USE_BOOT_IMAGE:-0}"
export MALLOC_CHECK_=0
export GLIBC_TUNABLES="${GLIBC_TUNABLES:-glibc.malloc.check=0:glibc.malloc.tcache_count=0}"

export LD_LIBRARY_PATH="$ART_DIR:$NATIVES_DIR:$HYBRIS_DIR:$ATL_BIO_DIR:$ANDROID_DIR:$BUNDLE_DIR/lib:${LD_LIBRARY_PATH:-}"

image_preload=""
for library in \
  "$ART_DIR/libicudata.so.77" \
  "$ART_DIR/libicuuc.so.77" \
  "$ART_DIR/libicui18n.so.77" \
  "/usr/lib64/libpng16.so.16" \
  "/usr/lib64/libjpeg.so.62" \
  "$ART_DIR/libandroidfw.so"; do
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

if [[ $# -eq 0 || "${1:-}" == "config" || "${1:-}" == "services" ]]; then
  exec "$BUNDLE_DIR/bin/nuah" config
elif [[ "${1:-}" == "native-run" || "${1:-}" == "atl-run" ]]; then
  exec "$BUNDLE_DIR/bin/nuah" "$@"
else
  exec "$BUNDLE_DIR/bin/nuah" native-run "$@"
fi
SCRIPT
chmod +x "$OUT_DIR/nuah"
ln -sf nuah "$OUT_DIR/run-nuah.sh"

TAR_OUT="${REPO_ROOT}/dist-portable/Nuah-Linux-x86_64.tar.gz"
ZIP_OUT="${REPO_ROOT}/dist-portable/Nuah-Linux-x86_64.zip"

echo "==> Packaging pure untouched bundle..."
mkdir -p "$REPO_ROOT/dist-portable"
tar -C "$REPO_ROOT/dist-portable" -czf "$TAR_OUT" nuah-portable
echo "==> Done: $TAR_OUT"

echo "==> Creating zip archive in $ZIP_OUT..."
rm -f "$ZIP_OUT"
(cd "$REPO_ROOT/dist-portable" && zip -rq "$ZIP_OUT" nuah-portable)
echo "==> Done: $ZIP_OUT"
