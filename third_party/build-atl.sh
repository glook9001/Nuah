#!/usr/bin/env bash
# Build android_translation_layer from pinned upstream source and stage a
# Nuah-ready ATL directory (api-impl.jar, framework-res.apk, gstub.jar,
# stripped natives/). Verified revision boots RIVALS natives; framework jars
# stay pinned to nuah/vendor/atl until the api-impl drift is bisected.
set -euo pipefail
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
rev=${NUAH_ATL_REV:-b7bedbb8f0a7126fdd9011b1cc2f3a096cb13b0e}
src=${NUAH_ATL_SRC:-/tmp/opencode/atl-src}
bld=${NUAH_ATL_BUILD:-/tmp/opencode/atl-build}
dest=${1:-$repo_root/nuah/vendor/atl}
fail() { echo "$1" >&2; exit "${2:-1}"; }
if [[ ! -d $src/.git ]]; then rm -rf "$src"; git clone https://gitlab.com/android_translation_layer/android_translation_layer.git "$src" || fail "ATL clone failed" 1; fi
git -C "$src" fetch --depth 1 origin "$rev" 2>/dev/null || true
git -C "$src" checkout -q "$rev" || fail "cannot check out ATL $rev" 1
meson setup --reconfigure "$bld" "$src" >/dev/null || meson setup "$bld" "$src" || fail "ATL meson setup failed" 1
ninja -C "$bld" api-impl.jar libtranslation_layer_main.so libandroid.so.0 res/framework-res/framework-res.apk gstub.jar || fail "ATL build failed" 1
mkdir -p "$dest/natives"
cp "$bld/api-impl.jar" "$bld/res/framework-res/framework-res.apk" "$bld/gstub.jar" "$dest/"
cp "$bld/libtranslation_layer_main.so" "$bld/libandroid.so.0" "$dest/natives/"
strip --strip-unneeded "$dest"/natives/*.so.0 "$dest"/natives/libtranslation_layer_main.so
ln -sf libandroid.so.0 "$dest/natives/libandroid.so"
echo "ATL staged at $dest ($(du -sh "$dest" | cut -f1)) from $rev"
