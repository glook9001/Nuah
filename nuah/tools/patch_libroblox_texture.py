#!/usr/bin/env python3
"""Create a guarded libroblox.so sidecar for the texture-generator A/B.

The Roblox image is a stripped Android ELF.  This tool deliberately supports
one known build and one two-byte instruction replacement.  It never modifies
the input image and refuses to patch a different build or an ambiguous match.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path


EXPECTED_BUILD_ID = "bac493cb6daa1563ab5bf983209f44b19cd010fe"
EXPECTED_SHA256 = "68c5b61e61e1e3e44156c0dd017954456206d2d33ac26cfe64ea531b6c355067"
EXPECTED_SIZE = 115_745_008
TARGET_STRING = b"TexturePackGeneratorUseOriginal"

# The constructor for the target flag has this shape:
#   xor eax,eax
#   mov byte ptr [rip+...],al
#   lea rcx,[rip+"TexturePackGeneratorUseOriginal"]
#   mov qword ptr [rip+...],0x1f
# The displacement fields are build-specific, so they are wildcards.
PREFIX = bytes.fromhex("31 c0 88 05")
LEA_PREFIX = bytes.fromhex("48 8d 0d")
LENGTH_MARKER = bytes.fromhex("48 c7 05") + b"\x00\x00\x00\x00" + bytes.fromhex(
    "1f 00 00 00"
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def read_u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def elf_segments(data: bytes) -> list[tuple[int, int, int, int]]:
    """Return (file_offset, file_size, virtual_address, flags) PT_LOADs."""
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise ValueError("input is not a little-endian ELF64 image")
    machine = read_u16(data, 18)
    if machine != 62:
        raise ValueError(f"input machine is {machine}, expected x86-64")
    phoff = read_u64(data, 32)
    phentsize = read_u16(data, 54)
    phnum = read_u16(data, 56)
    if phentsize < 56 or phoff + phentsize * phnum > len(data):
        raise ValueError("truncated ELF program-header table")
    result: list[tuple[int, int, int, int]] = []
    for index in range(phnum):
        offset = phoff + index * phentsize
        p_type, flags = struct.unpack_from("<II", data, offset)
        p_offset, p_vaddr, _, filesz, _, _ = struct.unpack_from(
            "<QQQQQQ", data, offset + 8
        )
        if p_type == 1 and p_offset <= len(data) and filesz <= len(data) - p_offset:
            result.append((p_offset, filesz, p_vaddr, flags))
    if not result:
        raise ValueError("ELF has no file-backed PT_LOAD segments")
    return result


def file_to_va(segments: list[tuple[int, int, int, int]], offset: int) -> int:
    for file_offset, file_size, virtual_address, _ in segments:
        if file_offset <= offset < file_offset + file_size:
            return virtual_address + offset - file_offset
    raise ValueError(f"file offset 0x{offset:x} is not in a PT_LOAD segment")


def va_to_file(segments: list[tuple[int, int, int, int]], address: int) -> int:
    for file_offset, file_size, virtual_address, _ in segments:
        if virtual_address <= address < virtual_address + file_size:
            return file_offset + address - virtual_address
    raise ValueError(f"virtual address 0x{address:x} is not file-backed")


def find_target(data: bytes, segments: list[tuple[int, int, int, int]]) -> int:
    string_offset = data.find(TARGET_STRING)
    if string_offset < 0:
        raise ValueError("target flag string is absent")
    string_va = file_to_va(segments, string_offset)
    matches: list[int] = []
    for file_offset, file_size, virtual_address, flags in segments:
        if not flags & 1:  # executable PT_LOAD
            continue
        text = data[file_offset : file_offset + file_size]
        cursor = 0
        while True:
            relative = text.find(PREFIX, cursor)
            if relative < 0:
                break
            candidate = file_offset + relative
            # mov byte [rip+disp32],al occupies six bytes.  The following
            # instruction must be a RIP-relative LEA to our target string.
            lea = candidate + 8
            if data[lea : lea + 3] != LEA_PREFIX:
                cursor = relative + 1
                continue
            displacement = struct.unpack_from("<i", data, lea + 3)[0]
            lea_end_va = file_to_va(segments, lea) + 7
            if lea_end_va + displacement != string_va:
                cursor = relative + 1
                continue
            # The constructor stores the string pointer in a small metadata
            # record before writing its 31-byte name length.  The length
            # marker therefore starts after xor (2), mov (6), lea (7), and
            # the pointer store (7).
            marker = candidate + 22
            if data[marker : marker + 3] != bytes.fromhex("48 c7 05"):
                cursor = relative + 1
                continue
            if data[marker + 7 : marker + 11] != b"\x1f\x00\x00\x00":
                cursor = relative + 1
                continue
            matches.append(candidate)
            cursor = relative + 1
    if len(matches) != 1:
        rendered = ", ".join(f"0x{x:x}" for x in matches) or "none"
        raise ValueError(f"expected one target signature, found {len(matches)} ({rendered})")
    return matches[0]


def make_manifest(
    source: Path,
    output: Path,
    original: bytes,
    patched: bytes,
    offset: int,
) -> dict[str, object]:
    return {
        "format": 1,
        "patch": "TexturePackGeneratorUseOriginalDefault",
        "target": "libroblox.so",
        "build_id": EXPECTED_BUILD_ID,
        "original_sha256": sha256(original),
        "patched_sha256": sha256(patched),
        "original_size": len(original),
        "patched_size": len(patched),
        "file_offset": offset,
        "original_bytes": "31c0",
        "replacement_bytes": "b001",
        "target_string": TARGET_STRING.decode("ascii"),
        "source": str(source),
        "output": str(output),
    }


def patch(input_path: Path, output_path: Path, manifest_path: Path, force: bool) -> None:
    if input_path.resolve() == output_path.resolve():
        raise ValueError("refusing to overwrite the input image")
    original = input_path.read_bytes()
    digest = sha256(original)
    if len(original) != EXPECTED_SIZE or digest != EXPECTED_SHA256:
        raise ValueError(
            "unsupported libroblox build: "
            f"size={len(original)} sha256={digest}; expected "
            f"size={EXPECTED_SIZE} sha256={EXPECTED_SHA256}"
        )
    segments = elf_segments(original)
    offset = find_target(original, segments)
    if original[offset : offset + 2] != b"\x31\xc0":
        raise ValueError("target bytes changed; refusing to patch")
    patched = bytearray(original)
    patched[offset : offset + 2] = b"\xb0\x01"  # mov al,1: default flag=true
    patched_bytes = bytes(patched)
    if not force and output_path.exists():
        raise FileExistsError(f"output already exists: {output_path} (use --force)")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_name(output_path.name + ".tmp")
    temporary.write_bytes(patched_bytes)
    os.chmod(temporary, 0o500)
    os.replace(temporary, output_path)
    manifest = make_manifest(input_path, output_path, original, patched_bytes, offset)
    manifest_tmp = manifest_path.with_name(manifest_path.name + ".tmp")
    manifest_tmp.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    os.replace(manifest_tmp, manifest_path)
    print(json.dumps(manifest, indent=2))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)
    manifest = args.manifest or args.output.with_suffix(args.output.suffix + ".json")
    try:
        patch(args.input, args.output, manifest, args.force)
    except (OSError, ValueError, struct.error) as error:
        print(f"nuah patch: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
