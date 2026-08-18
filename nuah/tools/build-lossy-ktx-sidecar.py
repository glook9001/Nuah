#!/usr/bin/env python3
"""Build a non-destructive, lower-mip KTX2 sidecar from Roblox rbx-storage.

The source cache is never modified.  Every transformed record keeps its RBXH
prefix and replaces only the embedded KTX2 body with a valid KTX2 file whose
largest `--drop-mips` levels were removed.  This reduces both upload bytes and
GPU texture residency if a future loader hook elects to consume the sidecar.

This tool deliberately does *not* install the result into rbx-storage.  Cache
key and integrity semantics belong to Roblox and are not yet proven safe to
rewrite in place.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import sqlite3
import struct
from pathlib import Path


KTX2_MAGIC = b"\xabKTX 20\xbb\r\n\x1a\n"
RBXH_MAGIC = b"RBXH"
KTX2_HEADER = struct.Struct("<13I2Q")
LEVEL_INDEX = struct.Struct("<3Q")


def align(value: int, alignment: int = 8) -> int:
    return (value + alignment - 1) & -alignment


ELIGIBLE_FORMATS = {147, 151}  # VK_FORMAT_ETC2_R8G8B8(_A8)_UNORM_BLOCK


class IspcCopy:
    """Optional AVX2/SSE4 retained-mip copier, never used on render threads."""

    def __init__(self, requested: Path | None) -> None:
        default = Path(__file__).resolve().parents[2] / "build/ispc/libnuah_ispc_asset.so"
        path = requested or Path(os.environ.get("NUAH_ISPC_ASSET_LIB", default))
        self.enabled = False
        self.reason = "scalar"
        self.copy_bytes = 0
        self.classified_records = 0
        if not path.is_file():
            return
        try:
            library = ctypes.CDLL(path)
            function = library.nuah_ispc_copy
            function.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32)
            function.restype = None
            classifier = library.nuah_ispc_mark_rbxh_ktx2
            classifier.argtypes = (ctypes.c_void_p, ctypes.c_int32, ctypes.c_int32,
                                   ctypes.c_void_p)
            classifier.restype = None
            bytes_address = ctypes.pythonapi.PyBytes_AsString
            bytes_address.argtypes = (ctypes.py_object,)
            bytes_address.restype = ctypes.c_void_p
        except (AttributeError, OSError):
            self.reason = "scalar (ISPC library unavailable)"
            return
        self._function = function
        self._classifier = classifier
        self._bytes_address = bytes_address
        self.enabled = True
        self.reason = f"ISPC ({path})"

    def copy(self, destination: bytearray, destination_offset: int, source: bytes,
             source_offset: int, length: int) -> None:
        # Calls below 1 KiB cost more in dispatch than they save. Python's
        # bytearray slice is the scalar fallback for those small mips.
        if not self.enabled or length < 1024 or length > 0x7fff_ffff:
            destination[destination_offset:destination_offset + length] = \
                source[source_offset:source_offset + length]
            return
        destination_address = ctypes.addressof(ctypes.c_ubyte.from_buffer(destination))
        source_address = self._bytes_address(source)
        self._function(destination_address + destination_offset,
                       source_address + source_offset, length)
        self.copy_bytes += length

    def candidates(self, records: list[tuple[bytes, bytes]]) -> list[bool]:
        if not self.enabled:
            return [True] * len(records)
        # The ISPC entry point consumes a compact, fixed-stride prefix table;
        # malformed/short records are padded and simply do not match.
        stride = 49
        prefixes = bytearray(len(records) * stride)
        for index, (_record_id, content) in enumerate(records):
            if content:
                start = index * stride
                prefixes[start:start + min(stride, len(content))] = content[:stride]
        matches = bytearray(len(records))
        if records:
            prefix_address = ctypes.addressof(ctypes.c_ubyte.from_buffer(prefixes))
            match_address = ctypes.addressof(ctypes.c_ubyte.from_buffer(matches))
            self._classifier(prefix_address, len(records), stride, match_address)
        self.classified_records += len(records)
        return [bool(match) for match in matches]


def transform_record(data: bytes, drop_mips: int, copier: IspcCopy) -> tuple[bytes, str] | None:
    if not data.startswith(RBXH_MAGIC):
        return None
    payload_start = data.find(KTX2_MAGIC)
    # Current RIVALS records use a 37-byte envelope.  Require it explicitly;
    # accepting a later format blindly would risk constructing bad KTX files.
    if payload_start != 37 or len(data) < payload_start + 80:
        return None
    values = list(KTX2_HEADER.unpack_from(data, payload_start + 12))
    vk_format, width, height, depth, levels = (
        values[0], values[2], values[3], values[4], values[7]
    )
    if (vk_format not in ELIGIBLE_FORMATS or not width or not height or
            not levels or drop_mips >= levels):
        return None
    index_start = payload_start + 80
    if len(data) < index_start + levels * LEVEL_INDEX.size:
        return None
    indexes = [LEVEL_INDEX.unpack_from(data, index_start + i * LEVEL_INDEX.size)
               for i in range(levels)]
    selected = indexes[drop_mips:]
    for offset, length, _ in selected:
        if offset + length > len(data) - payload_start:
            return None

    # Preserve DFD/KVD/SGD bytes at their existing offsets.  KTX2's level
    # index is shortened, but the original metadata end is the first legal
    # position for rebuilt level payloads.
    dfd_offset, dfd_length = values[9], values[10]
    kvd_offset, kvd_length = values[11], values[12]
    sgd_offset, sgd_length = values[13], values[14]
    metadata_end = max(index_start - payload_start + len(selected) * LEVEL_INDEX.size,
                       dfd_offset + dfd_length, kvd_offset + kvd_length,
                       sgd_offset + sgd_length)
    metadata_end = align(metadata_end)
    body = bytearray(data[payload_start:payload_start + metadata_end])
    if len(body) < metadata_end:
        return None

    values[2] = max(1, width >> drop_mips)
    values[3] = max(1, height >> drop_mips)
    values[4] = max(1, depth >> drop_mips) if depth else 0
    values[7] = len(selected)
    body[12:80] = KTX2_HEADER.pack(*values)

    rebuilt_indexes = []
    cursor = metadata_end
    for offset, length, uncompressed in selected:
        cursor = align(cursor)
        if len(body) < cursor:
            body.extend(b"\0" * (cursor - len(body)))
        raw_start = payload_start + offset
        destination_offset = len(body)
        body.extend(b"\0" * length)
        copier.copy(body, destination_offset, data, raw_start, length)
        rebuilt_indexes.append((cursor, length, uncompressed))
        cursor += length
    for index, entry in enumerate(rebuilt_indexes):
        LEVEL_INDEX.pack_into(body, 80 + index * LEVEL_INDEX.size, *entry)

    # Keep the opaque RBXH envelope byte-for-byte. Only the KTX2 body changes.
    result = data[:payload_start] + body
    return result, hashlib.sha256(data).hexdigest()


def transform(source: Path, destination: Path, drop_mips: int,
              copier: IspcCopy) -> tuple[int, int, str] | None:
    data = source.read_bytes()
    result = transform_record(data, drop_mips, copier)
    if result is None:
        return None
    transformed, digest = result
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".tmp")
    with open(temporary, "xb") as output:
        output.write(transformed)
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, destination)
    return len(data), len(transformed), digest


def build_sqlite_sidecar(source: Path, destination: Path, drop_mips: int,
                         profile: str, limit: int,
                         copier: IspcCopy) -> tuple[int, int, int]:
    """Copy an RbxStorage DB and replace eligible KTX2 BLOBs transactionally.

    SQLite owns the real RIVALS cache boundary.  The source database is opened
    read-only and never changed; the destination is a complete, independently
    writable cache database that Nuah may redirect Roblox to at launch.
    """
    if destination.exists():
        raise ValueError("destination database already exists; refusing to mix profiles")
    destination.parent.mkdir(parents=True, exist_ok=True)
    source_uri = f"file:{source}?mode=ro"
    source_db = sqlite3.connect(source_uri, uri=True)
    target_db = sqlite3.connect(destination)
    try:
        source_db.backup(target_db)
        converted = original_bytes = sidecar_bytes = 0
        rows = target_db.execute(
            "SELECT id, content FROM files WHERE content IS NOT NULL").fetchall()
        candidates = copier.candidates(rows)
        updates: list[tuple[bytes, int, bytes]] = []
        for (record_id, content), candidate in zip(rows, candidates):
            if not candidate:
                continue
            result = transform_record(content, drop_mips, copier)
            if result is None:
                continue
            transformed, _digest = result
            updates.append((transformed, len(transformed), record_id))
            converted += 1
            original_bytes += len(content)
            sidecar_bytes += len(transformed)
            if limit and converted >= limit:
                break
        target_db.executemany("UPDATE files SET content=?, size=? WHERE id=?", updates)
        target_db.commit()
        target_db.execute("VACUUM")
        target_db.execute("ANALYZE")
        target_db.commit()
        manifest = destination.with_suffix(destination.suffix + ".nuah-manifest.json")
        manifest.write_text(json.dumps({
            "format": 1, "profile": profile, "drop_mips": drop_mips,
            "source": str(source), "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "converted": converted, "original_blob_bytes": original_bytes,
            "sidecar_blob_bytes": sidecar_bytes,
        }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return converted, original_bytes, sidecar_bytes
    finally:
        target_db.close()
        source_db.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="rbx-storage directory or rbx-storage.db")
    parser.add_argument("destination", type=Path, help="new sidecar directory or database")
    parser.add_argument("--drop-mips", type=int, default=4,
                        help="number of largest mip levels to discard (default: 4)")
    parser.add_argument("--profile", default="potato4",
                        help="cache-profile identity recorded in the manifest")
    parser.add_argument("--limit", type=int, default=0,
                        help="transform at most this many files (0 means all)")
    parser.add_argument("--ispc-copy-lib", type=Path,
                        help="optional libnuah_ispc_asset.so path for AVX2/SSE4 bulk copies")
    args = parser.parse_args()
    if args.drop_mips < 1:
        parser.error("--drop-mips must be at least 1")
    copier = IspcCopy(args.ispc_copy_lib)
    if args.source.is_file():
        try:
            converted, original_bytes, sidecar_bytes = build_sqlite_sidecar(
                args.source, args.destination, args.drop_mips, args.profile, args.limit, copier)
        except (OSError, sqlite3.Error, ValueError) as error:
            parser.error(str(error))
        print(f"converted={converted} original_bytes={original_bytes} sidecar_bytes={sidecar_bytes} profile={args.profile} boundary=sqlite copier={copier.reason} ispc_classified={copier.classified_records} ispc_copy_bytes={copier.copy_bytes}")
        return 0
    if not args.source.is_dir():
        parser.error("source must be an rbx-storage directory or rbx-storage.db")
    if args.destination.exists() and any(args.destination.iterdir()):
        parser.error("destination must be empty to prevent mixing profiles")

    converted = original_bytes = sidecar_bytes = 0
    manifest: dict[str, object] = {
        "format": 1,
        "profile": args.profile,
        "drop_mips": args.drop_mips,
        "records": {},
    }
    for source in args.source.rglob("*"):
        if not source.is_file():
            continue
        result = transform(source, args.destination / source.relative_to(args.source),
                           args.drop_mips, copier)
        if result is None:
            continue
        original, sidecar, digest = result
        converted += 1
        original_bytes += original
        sidecar_bytes += sidecar
        relative = source.relative_to(args.source).as_posix()
        manifest["records"][relative] = {
            "source_sha256": digest,
            "source_bytes": original,
            "sidecar_bytes": sidecar,
        }
        if args.limit and converted >= args.limit:
            break
    manifest_path = args.destination / "nuah-sidecar-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                             encoding="utf-8")
    print(f"converted={converted} original_bytes={original_bytes} sidecar_bytes={sidecar_bytes} profile={args.profile} copier={copier.reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
