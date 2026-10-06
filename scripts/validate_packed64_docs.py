#!/usr/bin/env python3
"""Valida integridad de un .docs packed64 (header + posting lists uint64)."""
from __future__ import annotations

import struct
import sys
from pathlib import Path


def validate_docs(path: Path) -> dict[str, int]:
    data = path.read_bytes()
    if len(data) < 4:
        raise SystemExit(f"ERROR: {path} demasiado pequeno ({len(data)} bytes)")

    total_lists, = struct.unpack_from("<I", data, 0)
    off = 4
    total_postings = 0

    for i in range(total_lists):
        if off + 4 > len(data):
            raise SystemExit(f"ERROR: truncado en header lista {i}/{total_lists}")
        length, = struct.unpack_from("<I", data, off)
        off += 4
        need = length * 8
        if off + need > len(data):
            raise SystemExit(
                f"ERROR: truncado en postings lista {i}/{total_lists} "
                f"len={length} remain={len(data) - off}"
            )
        total_postings += length
        off += need

    if off != len(data):
        raise SystemExit(f"ERROR: bytes sobrantes off={off} size={len(data)}")

    meta = path.with_suffix(path.suffix + ".meta")
    if not meta.is_file() or meta.stat().st_size == 0:
        raise SystemExit(f"ERROR: meta ausente o vacio: {meta}")

    return {
        "lists": total_lists,
        "postings": total_postings,
        "bytes": len(data),
    }


def main() -> int:
    if len(sys.argv) != 2:
        print(f"uso: {sys.argv[0]} <path.docs>", file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    if not path.is_file():
        print(f"ERROR: no existe {path}", file=sys.stderr)
        return 1
    stats = validate_docs(path)
    print(
        f"[OK] docs={path} lists={stats['lists']} "
        f"postings={stats['postings']} bytes={stats['bytes']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
