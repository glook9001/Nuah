#!/usr/bin/env bash
# Capture a bounded JNI contract from one normal Sober gameplay session.
# Usage: sudo Investigation/capture_sober_jni_contract.sh PID OUTPUT.tsv
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 SOBER_PID OUTPUT.tsv" >&2
  exit 64
fi

pid=$1
output=$2
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
maps="/proc/$pid/maps"

[[ $pid =~ ^[1-9][0-9]*$ ]] || { echo "invalid PID" >&2; exit 64; }
[[ -r $maps ]] || { echo "cannot read $maps" >&2; exit 1; }
[[ ! -e $output ]] || { echo "refusing to overwrite $output" >&2; exit 1; }

# The client has appeared as both sober_rx and /memfd:sober.  The offsets below
# are tied to the observed Flatpak revision; derive the ELF PIE base from its
# executable mapping and fail closed instead of tracing an unknown map.
map_info=$(awk '$2 ~ /^r.x/ && ($0 ~ /sober_rx/ || $0 ~ /\/memfd:sober/) { split($1, range, "-"); print "0x" range[1], "0x" $3; exit }' "$maps")
[[ -n $map_info ]] || {
  echo "no executable Sober client map in $maps; launch a Roblox room in Sober first" >&2
  exit 1
}
read -r map_start map_offset <<<"$map_info"
base=$(printf '0x%x' "$((map_start - map_offset))")

: >"$output"
echo "# sober-jni-live-v1 pid=$pid base=$base" >"$output"
echo "Recording to $output. Keep this terminal open while you join/play/leave a room; Ctrl-C detaches." >&2
NUAH_JNI_TRACE="$output" gdb -q \
  -ex 'set pagination off' \
  -ex 'set confirm off' \
  -ex 'set detach-on-fork off' \
  -ex "set \$nuah_sober_base = $base" \
  -ex "attach $pid" \
  -ex "source $root/Investigation/sober_jni_contract.gdb" \
  -ex continue
