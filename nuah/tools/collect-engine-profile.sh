#!/usr/bin/env bash
set -euo pipefail

if [[ $# -gt 3 ]]; then
  echo "usage: $0 [PID] [SECONDS=60] [OUTPUT=./nuah-engine.perf.data]" >&2
  exit 2
fi

pid=${1:-}
seconds=${2:-60}
output=${3:-./nuah-engine.perf.data}
if [[ -z $pid ]]; then
  pid=$(ps -eo pid=,comm=,args= |
    awk '$2 == "nuah" && $0 ~ / native-run / { found = $1 } END { print found }')
fi
[[ $pid =~ ^[0-9]+$ ]] || {
  echo "could not find a running nuah native-run process; pass its PID" >&2
  exit 2
}
[[ $seconds =~ ^[0-9]+$ ]] || { echo "SECONDS must be numeric" >&2; exit 2; }
kill -0 "$pid"

echo "Recording $seconds seconds from pid $pid into $output"
perf record -F 199 -g --call-graph dwarf -p "$pid" -o "$output" -- sleep "$seconds"
echo
echo "Top DSOs/threads:"
perf report -i "$output" --stdio --no-children --sort comm,dso | head -80
echo
echo "Map libroblox return PCs with:"
echo "  nuah/tools/map-libroblox-offset.sh /path/to/libroblox.so 0xOFFSET --return-pc"
