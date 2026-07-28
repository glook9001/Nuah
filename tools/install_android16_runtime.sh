#!/usr/bin/env bash
# Install the Android 16 ATL runtime produced by GitHub Actions.
# This script never compiles anything locally.
set -euo pipefail

archive=${1:?usage: install_android16_runtime.sh <android16-atl-runtime.tar.gz> [destination]}
destination=${2:-${XDG_DATA_HOME:-$HOME/.local/share}/nuah/android16-runtime}

[[ -f "$archive" ]] || { echo "runtime archive not found: $archive" >&2; exit 1; }
command -v tar >/dev/null || { echo "tar is required" >&2; exit 1; }

root=$(mktemp -d "${TMPDIR:-/tmp}/nuah-android16.XXXXXX")
cleanup() { rm -rf "$root"; }
trap cleanup EXIT

tar -xzf "$archive" -C "$root"
staged="$root/android16-runtime"
[[ -x "$staged/android-translation-layer" ]] || {
  echo "archive is missing android16-runtime/android-translation-layer" >&2; exit 1;
}
[[ -s "$staged/bootclasspath.txt" ]] || {
  echo "archive is missing Android 16 bootclasspath.txt" >&2; exit 1;
}
[[ -s "$staged/natives/libtranslation_layer_main.so" ]] || {
  echo "archive is missing ATL native library" >&2; exit 1;
}

mkdir -p "$(dirname "$destination")"
if [[ -e "$destination" ]]; then
  echo "destination already exists; refusing to overwrite: $destination" >&2
  exit 1
fi
mv "$staged" "$destination"
printf 'Installed Android 16 ATL runtime at %s\n' "$destination"
printf 'export NUAH_ATL_RUNTIME=android16\nexport NUAH_ATL_ANDROID16_HOME=%q\n' "$destination"
