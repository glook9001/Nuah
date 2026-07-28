#!/usr/bin/env bash
# Stage AOSP host ART output for the ATL Meson/pkg-config adapter.
#
# Usage:
#   prepare_host_runtime.sh AOSP_ROOT OUTPUT_ROOT [ANDROIDFW_ROOT]
#
# This deliberately does not pretend that an ART APEX is a Linux host
# runtime. It stages the host shared objects, Android 16 boot jars, and the
# pkg-config contract consumed by android_translation_layer.
set -euo pipefail

aosp_root=${1:?AOSP source/output root is required}
output_root=${2:?output directory is required}
androidfw_root=${3:-}

mkdir -p "$output_root/lib" "$output_root/java" "$output_root/include/androidfw" \
  "$output_root/natives" "$output_root/pkgconfig"

find_one() {
  local name=$1 root=$2
  find "$root" \( -type f -o -type l \) -name "$name" -print -quit
}

copy_required() {
  local name=$1 root=$2 destination=$3
  local source
  source=$(find_one "$name" "$root")
  if [[ -z "$source" ]]; then
    echo "missing $name below $root" >&2
    return 1
  fi
  install -m 0755 -T "$source" "$destination/$name"
}

copy_required libart.so "$aosp_root" "$output_root/lib"
copy_required libnativebridge.so "$aosp_root" "$output_root/lib"

# Keep the ART dependency closure beside libart. Some of these names vary
# across AOSP branches, so stage those that the selected branch actually
# emits and let the pkg-config contract expose the directory uniformly.
for name in \
  libart-compiler.so libart-dexlayout.so libartbase.so libartpalette.so \
  libbacktrace.so libbase.so libcutils.so libdexfile.so liblog.so \
  libprofile.so libsigchain.so libunwind.so libutils.so libziparchive.so; do
  source=$(find_one "$name" "$aosp_root")
  [[ -n "$source" ]] && install -m 0755 -T "$source" "$output_root/lib/$name"
done

# androidfw is not part of the master-art manifest. A full platform checkout
# or the existing ATL-compatible androidfw build must provide it.
if [[ -n "$androidfw_root" ]]; then
  copy_required libandroidfw.so "$androidfw_root" "$output_root/lib"
  header=$(find "$androidfw_root" -type f -path '*/androidfw/androidfw_c_api.h' \
    -print -quit)
  [[ -n "$header" ]] || { echo "missing androidfw_c_api.h" >&2; exit 1; }
  install -m 0644 "$header" "$output_root/include/androidfw/androidfw_c_api.h"
else
  echo "ANDROIDFW_ROOT is required for host ATL integration" >&2
  exit 1
fi

for jar in core-oj.jar core-libart.jar; do
  source=$(find_one "$jar" "$aosp_root")
  [[ -n "$source" ]] || { echo "missing Android 16 boot jar: $jar" >&2; exit 1; }
  install -m 0644 "$source" "$output_root/java/$jar"
done

cat > "$output_root/bootclasspath.txt" <<EOF
java/core-oj.jar:java/core-libart.jar
EOF
cat > "$output_root/bootclasspath.build.txt" <<EOF
$output_root/java/core-oj.jar:$output_root/java/core-libart.jar
EOF

cat > "$output_root/pkgconfig/art-standalone.pc" <<EOF
prefix=$output_root
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: art-standalone
Description: Android 16 ART host runtime staged for ATL
Version: 16.0.0
Libs: -L\${libdir} -Wl,-rpath,\${libdir} -lart -lnativebridge -landroidfw
Cflags: -I\${includedir} -I\${includedir}/androidfw
EOF

echo "staged Android 16 host ART at $output_root"
