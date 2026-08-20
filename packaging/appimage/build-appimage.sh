#!/usr/bin/env bash
set -euo pipefail

# Build a portable x86_64 Nuah AppImage.  This deliberately bundles Nuah's
# Android/ART userspace and the GLib/SDL userspace it was built against, but
# leaves glibc, Vulkan, Wayland, X11 and audio drivers to the host.  Those
# pieces are host integration points and bundling them breaks GPU drivers.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${NUAH_APPIMAGE_BUILD_DIR:-$REPO_ROOT/build-appimage}"
APPDIR="$BUILD_DIR/Nuah.AppDir"
OUT_DIR="${NUAH_APPIMAGE_OUT_DIR:-$REPO_ROOT/dist-appimage}"
RUNTIME_FILE="$BUILD_DIR/runtime-x86_64"
APPIMAGE="$OUT_DIR/Nuah-x86_64.AppImage"

RUNTIME_ROOT="${NUAH_APPIMAGE_RUNTIME_ROOT:-}"
if [[ -z "$RUNTIME_ROOT" ]]; then
  RUNTIME_ROOT=$(find /var/lib/flatpak/runtime/org.gnome.Platform/x86_64/50 \
    -path '*/files' -type d -print -quit 2>/dev/null || true)
fi

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "error: required command is missing: $1" >&2
    exit 1
  }
}

for tool in cp find mksquashfs patchelf readelf; do need "$tool"; done
# The generated build-tree ATL is the paired provider for the current Nuah
# executable. The smaller dist/ copy is an older fallback and can silently
# produce a dead ART session (different api-impl.jar and JNI exports).
ATL_SOURCE="$REPO_ROOT/dist/java/dex/android_translation_layer"
if [[ -d "$REPO_ROOT/build/atl-full" ]]; then
  ATL_SOURCE="$REPO_ROOT/build/atl-full"
fi

for input in \
  "$REPO_ROOT/build/nuah" \
  "$REPO_ROOT/build/nuah-services" \
  "$REPO_ROOT/build/libnuah_host_bridge.so" \
  "$REPO_ROOT/build/libnuah_atl_resources.so" \
  "$REPO_ROOT/build/libnuah_atl_overlay.so" \
  "$REPO_ROOT/dist/art" \
  "$REPO_ROOT/dist/java/dex/art" \
  "$ATL_SOURCE" \
  "$REPO_ROOT/dist/android" \
  "$REPO_ROOT/dist/hybris/lib" \
  "$REPO_ROOT/build/atl-bionic"; do
  [[ -e "$input" ]] || { echo "error: missing package input: $input" >&2; exit 1; }
done

RELEASE_ART_DIR="$REPO_ROOT/release/nuah-rivals-turbo-1220x980/art"
[[ -d "$RELEASE_ART_DIR" ]] || {
  echo "error: missing matched release ART tree: $RELEASE_ART_DIR" >&2
  exit 1
}

if [[ -z "$RUNTIME_ROOT" || ! -d "$RUNTIME_ROOT" ]]; then
  echo "error: GNOME Platform 50 is required as a portable GLib/SDL source" >&2
  echo "       set NUAH_APPIMAGE_RUNTIME_ROOT to its extracted files directory" >&2
  exit 1
fi

DEX2OAT_BINARY="${NUAH_DEX2OAT_BINARY:-/usr/local/bin/dex2oat}"
if [[ ! -x "$DEX2OAT_BINARY" ]]; then
  DEX2OAT_BINARY="$REPO_ROOT/dist/bin/dex2oat"
fi
[[ -x "$DEX2OAT_BINARY" ]] || { echo "error: dex2oat is missing" >&2; exit 1; }

rm -rf "$APPDIR"
mkdir -p "$APPDIR"/{bin,lib,lib64,share/applications,share/icons/hicolor/256x256/apps}

echo "==> Copying Nuah executable and native bridge..."
cp -a "$REPO_ROOT/build/nuah" "$APPDIR/bin/nuah"
cp -a "$REPO_ROOT/build/nuah-services" "$APPDIR/bin/nuah-services"
for lib in \
  libnuah_host_bridge.so libnuah_atl_resources.so libnuah_atl_overlay.so; do
  cp -a "$REPO_ROOT/build/$lib" "$APPDIR/lib/$lib"
  # The runtime-directory contract historically places these companions next
  # to the executable. Keep that contract in the relocatable bundle too.
  cp -a "$REPO_ROOT/build/$lib" "$APPDIR/bin/$lib"
done
if [[ -r "$REPO_ROOT/build/ispc/libnuah_ispc_asset.so" ]]; then
  cp -a "$REPO_ROOT/build/ispc/libnuah_ispc_asset.so" "$APPDIR/lib/"
fi
cp -a "$DEX2OAT_BINARY" "$APPDIR/bin/dex2oat"

ln -s ../lib/android "$APPDIR/bin/android"
ln -s ../lib/hybris "$APPDIR/bin/hybris"
ln -s ../lib/atl-bionic "$APPDIR/bin/bionic-translation"

echo "==> Copying matched Android/ART runtime..."
mkdir -p "$APPDIR/lib/art" "$APPDIR/lib/atl-bionic" \
  "$APPDIR/lib/hybris" "$APPDIR/lib/android" \
  "$APPDIR/share/java/art" "$APPDIR/share/java/atl/natives" \
  "$APPDIR/share/bionic_translation/cfg.d"
cp -a "$REPO_ROOT/dist/art/." "$APPDIR/lib/art/"
cp -a "$RELEASE_ART_DIR/." "$APPDIR/lib/art/"
# The bionic libc/pthread pair belongs only to the Android linker namespace.
# If it is visible to host dex2oat it can make pthread_cond_init fail.
rm -f "$APPDIR/lib/art/libc_bio.so.0" \
      "$APPDIR/lib/art/libpthread_bio.so.0" \
      "$APPDIR/lib/art/libdl_bio.so.0"
cp -a "$REPO_ROOT/build/atl-bionic/." "$APPDIR/lib/atl-bionic/"
cp -a "$REPO_ROOT/dist/hybris/lib/." "$APPDIR/lib/hybris/"
cp -a "$REPO_ROOT/dist/android/." "$APPDIR/lib/android/"
cp -a "$REPO_ROOT/dist/java/dex/art/." "$APPDIR/share/java/art/"
cp -a "$ATL_SOURCE/." "$APPDIR/share/java/atl/"
# Keep the installed ATL layout as well as the local Meson layout. Nuah uses
# this natives directory to register the libandroid.so.0/provider pair before
# ART resolves libtranslation_layer_main.so.
for atl_native in libandroid.so.0 libtranslation_layer_main.so; do
  if [[ -r "$ATL_SOURCE/natives/$atl_native" ]]; then
    cp -a "$ATL_SOURCE/natives/$atl_native" "$APPDIR/share/java/atl/natives/"
  elif [[ -r "$ATL_SOURCE/$atl_native" ]]; then
    cp -a "$ATL_SOURCE/$atl_native" "$APPDIR/share/java/atl/natives/"
  fi
done
# framework-res.apk is an APK asset rather than part of the ATL build tree.
if [[ -r "$REPO_ROOT/dist/java/dex/android_translation_layer/framework-res.apk" ]]; then
  cp -a "$REPO_ROOT/dist/java/dex/android_translation_layer/framework-res.apk" \
    "$APPDIR/share/java/atl/"
fi
cp -a "$REPO_ROOT/dist/share/bionic_translation/cfg.d/." \
  "$APPDIR/share/bionic_translation/cfg.d/" 2>/dev/null || true

# dex2oat's RPATH is relative to bin/. Keep those paths valid inside the
# mounted AppImage without putting a host build directory in the ELF.
ln -s ../lib/art "$APPDIR/lib64/art"
mkdir -p "$APPDIR/lib64/java/dex/art"
ln -s ../../../../share/java/art/natives "$APPDIR/lib64/java/dex/art/natives"

echo "==> Bundling the portable GLib/SDL userspace..."
runtime_find() {
  local soname="$1" source
  source=$(find "$RUNTIME_ROOT/lib" "$RUNTIME_ROOT/usr/lib" \
    -name "$soname" -print -quit 2>/dev/null || true)
  [[ -n "$source" && -r "$source" ]] || {
    echo "error: GNOME Platform is missing $soname" >&2
    exit 1
  }
  cp -L "$source" "$APPDIR/lib/$soname"
}

# No libc/libm/ld-linux is copied.  The AppImage intentionally uses the
# machine's glibc so Vulkan, NSS, PAM and vendor drivers remain compatible.
for soname in \
  libSDL3.so.0 libgio-2.0.so.0 libgobject-2.0.so.0 libgmodule-2.0.so.0 \
  libglib-2.0.so.0 libffi.so.8 libmount.so.1 libblkid.so.1 libselinux.so.1 \
  libpcre2-8.so.0 libz.so.1 libgcc_s.so.1 libstdc++.so.6; do
  runtime_find "$soname"
done

# ART/ATL use these stable compression/unwind libraries. Use the bundled
# platform versions when available, otherwise the host will resolve them.
for soname in liblz4.so.1 liblzma.so.5 libzstd.so.1 libelf.so.1 libunwind.so.8; do
  source=$(find "$RUNTIME_ROOT/lib" "$RUNTIME_ROOT/usr/lib" \
    -name "$soname" -print -quit 2>/dev/null || true)
  [[ -n "$source" ]] && cp -L "$source" "$APPDIR/lib/$soname" || true
done

# Use the release binary, never the old dist copy, and remove the developer
# machine's absolute build RPATH. The launcher supplies the remaining dirs.
patchelf --set-rpath '$ORIGIN/../lib' "$APPDIR/bin/nuah"
patchelf --set-rpath '$ORIGIN/../lib' "$APPDIR/bin/nuah-services"

echo "==> Copying launcher and metadata..."
cp -a "$SCRIPT_DIR/nuah-apprun.sh" "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"
cp -a "$SCRIPT_DIR/nuah.desktop" "$APPDIR/"
if [[ -r "$SCRIPT_DIR/nuah.png" ]]; then
  cp -a "$SCRIPT_DIR/nuah.png" "$APPDIR/"
else
  : > "$APPDIR/nuah.png"
fi

# Package the current APK pair when available. A release can omit this and
# point NUAH_PACKAGE_DIR at a user-provided Roblox package instead.
if [[ -r "$REPO_ROOT/dist/apk/base.apk" && -r "$REPO_ROOT/dist/apk/split_config.x86_64.apk" ]]; then
  mkdir -p "$APPDIR/share/nuah/packages/x86_64/com.roblox.client"
  cp -a "$REPO_ROOT/dist/apk/base.apk" \
    "$APPDIR/share/nuah/packages/x86_64/com.roblox.client/"
  cp -a "$REPO_ROOT/dist/apk/split_config.x86_64.apk" \
    "$APPDIR/share/nuah/packages/x86_64/com.roblox.client/"
fi

if [[ ! -x "$RUNTIME_FILE" ]]; then
  command -v curl >/dev/null 2>&1 || { echo "error: curl is required to fetch AppImage runtime" >&2; exit 1; }
  echo "==> Downloading AppImage runtime..."
  curl --fail --location --retry 3 --output "$RUNTIME_FILE" \
    https://github.com/AppImage/AppImageKit/releases/download/continuous/runtime-x86_64
  chmod +x "$RUNTIME_FILE"
fi

mkdir -p "$OUT_DIR"
SQUASHFS="$BUILD_DIR/nuah.squashfs"
rm -f "$SQUASHFS" "$APPIMAGE"
echo "==> Creating squashfs image with low-overhead compression..."
# Use gzip compression and limit processors to 2 so it doesn't freeze the system
mksquashfs "$APPDIR" "$SQUASHFS" -root-owned -noappend -comp gzip -processors 2 >/dev/null
cat "$RUNTIME_FILE" "$SQUASHFS" > "$APPIMAGE"
chmod +x "$APPIMAGE"

echo "==> AppImage created successfully: $APPIMAGE"
du -h "$APPIMAGE"
