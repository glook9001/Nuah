#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 LIBROBLOX.SO OFFSET [--return-pc]" >&2
  exit 2
fi

library=$1
offset=$2
mode=${3:-}
if [[ ! -r $library ]]; then
  echo "cannot read libroblox image: $library" >&2
  exit 2
fi
if ! [[ $offset =~ ^0x[0-9a-fA-F]+$|^[0-9]+$ ]]; then
  echo "OFFSET must be hexadecimal (for example 0x247a4c4) or decimal" >&2
  exit 2
fi

address=$((offset))
# perf and the bionic trace report return PCs.  A call instruction's return
# address often lands one byte into the next instruction, so use PC-1 to see
# the actual call site before trying to identify the enclosing function.
if [[ $mode == --return-pc ]]; then
  (( address > 0 )) || { echo "return PC must be non-zero" >&2; exit 2; }
  ((address--))
fi

printf 'libroblox offset: 0x%x\n' "$address"
printf 'build id: '
readelf -n "$library" | sed -n 's/.*Build ID: //p' | head -1
printf '\nRizin disassembly:\n'
# Begin a little before the sampled address.  Return PCs may point one byte
# past a variable-length x86 call, where starting Rizin exactly at PC-1 would
# decode the middle of that instruction as unrelated opcodes.
start=$(( address > 16 ? address - 16 : 0 ))
rizin -2 -q -c "e bin.cache=true; s 0x$(printf '%x' "$start"); pd 48" "$library"
