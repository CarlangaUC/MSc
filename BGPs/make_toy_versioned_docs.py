#!/usr/bin/env python3
"""Tiny packed64 .docs for ST[OP] n=1 vs n=2 ground truth."""
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


def main() -> None:
    # Term 0: master 1 lives on [1,3], master 2 lives only at 5.
    # Term 1: empty
    # Term 2: master 1 at 10, then a gap, then 12 (two RLE runs)
    lists = [
        [(1, 1), (1, 2), (1, 3), (2, 5)],
        [],
        [(1, 10), (1, 12)],
    ]
    out = Path("/root/MAGISTER/BGPs/toy_st_op.docs")
    write_docs(out, lists)
    print(f"wrote {out} terms={len(lists)} postings={sum(len(x) for x in lists)}")


if __name__ == "__main__":
    main()
