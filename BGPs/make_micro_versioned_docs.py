#!/usr/bin/env python3
"""Micro .docs packed64 para figuras de la EDD metatrie (intervalos solapados).

Pensado para `meta_trie_edd --dump-dot`: pocos masters y pocas versiones, pero
con solapamientos que generan varios intervalos suelo distintos.
"""
import struct
from pathlib import Path

REL_BITS = 24


def pack(master: int, rel: int) -> int:
    return (master << REL_BITS) | rel


def write_docs(path: Path, lists: list[list[tuple[int, int]]]) -> None:
    with path.open("wb") as out:
        out.write(struct.pack("<I", len(lists)))
        for pairs in lists:
            out.write(struct.pack("<I", len(pairs)))
            for master, rel in pairs:
                out.write(struct.pack("<Q", pack(master, rel)))


def runs(master: int, start: int, end: int) -> list[tuple[int, int]]:
    """master activo en versiones [start, end] inclusive."""
    return [(master, r) for r in range(start, end + 1)]


def main() -> None:
    # Term 0 (figura principal): tres masters con solapamiento parcial.
    #   u=1: [1,4]   u=2: [3,6]   u=3: [5,7]
    # -> intervalos suelo distintos en 1,3,5,7,8 (altas y bajas intercaladas).
    term0 = runs(1, 1, 4) + runs(2, 3, 6) + runs(3, 5, 7)
    # Term 1: un master con hueco (dos corridas RLE).
    term1 = runs(1, 1, 2) + runs(1, 5, 6)
    # Term 2: dos masters disjuntos.
    term2 = runs(2, 2, 3) + runs(4, 6, 7)

    lists = [term0, term1, term2]
    out = Path("/root/MAGISTER/BGPs/micro_metatrie.docs")
    write_docs(out, lists)
    print(f"wrote {out} terms={len(lists)} postings={sum(len(x) for x in lists)}")


if __name__ == "__main__":
    main()
