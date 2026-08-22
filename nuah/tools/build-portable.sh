#!/usr/bin/env bash
# Stage a stripped, relocatable Nuah prefix. Layout matches build/: nuah sits
# beside android/, hybris/, bionic/. ART is a host runtime (like Mesa), not
# copied from the build machine. APKs and ATL jars go in the tree. Cookie is
# not shipped.
#
#   nuah/tools/build-portable.sh
#   nuah/tools/build-portable.sh --tarball
#   nuah/tools/build-portable.sh --no-apk
#   nuah/tools/build-portable.sh --with-art   # optional fat copy of host ART
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${NUAH_BUILD_DIR:-$repo_root/build}
dest=${NUAH_PORTABLE_DIR:-$repo_root/dist/nuah-portable}
sober_pkg=${NUAH_SOBER_PACKAGE:-$HOME/.var/app/org.vinegarhq.Sober/data/sober/packages/x86_64/com.roblox.client}
art_src=${NUAH_ART_LIBRARY_DIR:-/usr/local/lib64/art}
atl_src=${NUAH_ATL_HOME:-/usr/local/lib64/java/dex/android_translation_layer}
hybris_src=""
copy_apk=1
copy_art=0
copy_atl=1
make_tarball=0
rebuild=1

usage() {
  cat <<'EOF' >&2
usage: build-portable.sh [--no-apk] [--no-atl] [--with-art] [--no-rebuild] [--tarball] [--dest DIR]
EOF
  exit 2
}

while (( $# > 0 )); do
  case "$1" in
    --no-apk) copy_apk=0 ;;
    --no-atl) copy_atl=0 ;;
    --no-art) copy_art=0 ;;
    --with-art) copy_art=1 ;;
    --no-rebuild) rebuild=0 ;;
    --tarball) make_tarball=1 ;;
    --dest)
      [[ $# -ge 2 ]] || usage
      dest=$2
      shift
      ;;
    -h|--help) usage ;;
    *) usage ;;
  esac
  shift
done

if [[ -d $build_dir/hybris/lib ]]; then
  hybris_src=$build_dir/hybris
elif [[ -d $HOME/.local/share/nuah/hybris/lib ]]; then
  hybris_src=$HOME/.local/share/nuah/hybris
else
  echo "libhybris prefix not found (build/hybris or ~/.local/share/nuah/hybris)" >&2
  exit 1
fi

if (( rebuild )); then
  cmake -S "$repo_root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build "$build_dir" --target nuah nuah-services
fi

[[ -x $build_dir/nuah ]] || {
  echo "build/nuah missing; build first or omit --no-rebuild" >&2
  exit 1
}
[[ -r $build_dir/bionic/lib64/linker64 ]] || {
  echo "API-36 bionic core missing at $build_dir/bionic/lib64/linker64" >&2
  exit 1
}

# Match the in-tree build/ layout: nuah, android/, hybris/, bionic/ are
# siblings because runtime_directory() is the executable's parent.
rm -rf "$dest"
mkdir -p "$dest"/{android/linker-deps,bionic/lib64,bionic-translation,hybris,apk}

cp -a "$build_dir/nuah" "$build_dir/nuah-services" "$build_dir/libnuah_host_bridge.so" "$dest/"
cp -a "$build_dir/bionic-translation/"libpthread_bio.so* "$dest/bionic-translation/"
cp -a "$build_dir/android/"*.so "$dest/android/"
cp -a "$build_dir/android/linker-deps/"*.so "$dest/android/linker-deps/"
cp -a "$build_dir/bionic/lib64/"* "$dest/bionic/lib64/"

rsync -a --exclude='*.la' --exclude='*.a' --exclude='include/' \
  "$hybris_src/" "$dest/hybris/"

if (( copy_atl )); then
  [[ -r $atl_src/api-impl.jar ]] || {
    echo "ATL home missing api-impl.jar at $atl_src" >&2
    exit 1
  }
  mkdir -p "$dest/atl"
  rsync -a "$atl_src/" "$dest/atl/"
fi

if (( copy_art )); then
  [[ -r $art_src/libart.so && -r $art_src/libandroidfw.so ]] || {
    echo "ART runtime missing at $art_src" >&2
    exit 1
  }
  mkdir -p "$dest/art"
  cp -a "$art_src/"*.so "$dest/art/"
fi

if (( copy_apk )); then
  [[ -r $sober_pkg/base.apk && -r $sober_pkg/split_config.x86_64.apk ]] || {
    echo "Sober APK/split missing under $sober_pkg" >&2
    exit 1
  }
  cp -a "$sober_pkg/base.apk" "$sober_pkg/split_config.x86_64.apk" "$dest/apk/"
fi

install -m 0755 "$repo_root/nuah/tools/portable-run.sh" "$dest/run"

# Strip shipped ELFs. Leave the vendor bionic linker/libc and archives alone.
while IFS= read -r -d '' file; do
  rel=${file#"$dest"/}
  case "$rel" in
    bionic/*|apk/*) continue ;;
  esac
  if file -b "$file" | grep -q ELF; then
    strip --strip-unneeded "$file"
  fi
done < <(find "$dest" -type f -print0)

{
  echo "nuah portable prefix"
  echo "built: $(date -Iseconds)"
  echo "source: $repo_root"
  echo "hybris: $hybris_src"
  if (( copy_atl )); then
    echo "atl: $atl_src"
  fi
  if (( copy_art )); then
    echo "art: $art_src (optional fat copy; run prefers host ART)"
  fi
  if (( copy_apk )); then
    echo "apk: $sober_pkg"
    (cd "$dest/apk" && sha256sum base.apk split_config.x86_64.apk)
  fi
} >"$dest/MANIFEST.txt"

echo "portable prefix: $dest"
du -sh "$dest" "$dest"/art "$dest"/atl "$dest"/apk "$dest"/hybris \
  "$dest"/bionic "$dest"/android 2>/dev/null || true

if (( make_tarball )); then
  parent=$(dirname "$dest")
  name=$(basename "$dest")
  tar -C "$parent" -cf "$parent/$name.tar" "$name"
  echo "tarball: $parent/$name.tar"
fi
