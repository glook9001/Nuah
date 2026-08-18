#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
exec "$root/host-glibc/ld-linux-x86-64.so.2" \
  --library-path "$root/build:$root/art:$root/hybris/lib:$root/build/ispc:$root/atl-bionic:$root/vulkan:$root/host-glibc:/home/niggermonkey/.local/share/nuah/base.apk_/lib:/usr/lib64" \
  "$root/build/nuah-release" "$@"
