#!/usr/bin/env python3
"""Turn sober_jni_contract.gdb output into Nuah's strict five-column TSV."""
from __future__ import annotations

import argparse
from pathlib import Path


def rows(path: Path):
    classes: dict[str, str] = {}
    pending: list[tuple[str, str, str, str]] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        if not raw or raw.startswith("#"):
            continue
        fields = raw.split("\t")
        if fields[0] == "class" and len(fields) == 3:
            classes[fields[1]] = fields[2]
        elif fields[0] == "lookup" and len(fields) == 5:
            pending.append(("lookup", fields[2], fields[3], fields[4]))
        elif fields[0] == "register" and len(fields) == 4:
            pending.append(("register", fields[1], fields[2], fields[3]))
    result: set[tuple[str, str, str, str, str]] = set()
    for kind, clazz, member, signature in pending:
        result.add((kind, classes.get(clazz, "*"), member, signature,
                    "sober-dynamic-contract"))
    return sorted(result)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"refusing to overwrite {args.output}")
    output = ["# nuah-jni-contract-v1", "# generated from a live Sober JNI trace"]
    output.extend("\t".join(row) for row in rows(args.trace))
    args.output.write_text("\n".join(output) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
