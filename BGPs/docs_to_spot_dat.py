#!/usr/bin/env python3
"""
Convierte .docs packed64 → .dat textual compatible con build-index (SPOT).

Mapeo MAGISTER → CLTJ (sin pérdida de pares (term, master, rel)):
  S = term_id
  P = 0
  O = master
  t1 = rel
  t2 = rel          (build-index hace t2+1 internamente → [rel, rel+1))

Una línea por posting. Header: n_terms max_master max_relative
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path

REL_BITS = 24
REL_MASK = (1 << REL_BITS) - 1


def unpack(p: int) -> tuple[int, int]:
    return p >> REL_BITS, p & REL_MASK


def read_docs(path: Path):
    with path.open("rb") as f:
        (nlists,) = struct.unpack("<I", f.read(4))
        for t in range(nlists):
            (length,) = struct.unpack("<I", f.read(4))
            posts = []
            if length:
                buf = f.read(8 * length)
                posts = list(struct.unpack(f"<{length}Q", buf))
            yield t, posts


def convert(docs_path: Path, dat_path: Path, max_terms: int = 0) -> dict:
    max_master = 0
    max_rel = 0
    n_postings = 0
    n_terms = 0

    with dat_path.open("w") as out:
        # placeholder header; rewritten after scan
        out.write("0 0 0\n")
        header_pos = out.tell()

        for term, posts in read_docs(docs_path):
            if max_terms and term >= max_terms:
                break
            n_terms = term + 1
            for packed in posts:
                m, r = unpack(packed)
                max_master = max(max_master, m)
                max_rel = max(max_rel, r)
                out.write(f"{term} 0 {m} {r} {r}\n")
                n_postings += 1

    text = dat_path.read_text()
    lines = text.splitlines()
    lines[0] = f"{n_terms} {max_master} {max_rel}"
    dat_path.write_text("\n".join(lines) + "\n")

    return {
        "terms": n_terms,
        "postings": n_postings,
        "max_master": max_master,
        "max_relative": max_rel,
        "dat_path": str(dat_path),
    }


def main() -> None:
    p = argparse.ArgumentParser(description="Convert packed64 .docs to CLTJ .dat (SPOT)")
    p.add_argument("docs")
    p.add_argument("dat")
    p.add_argument("--max-terms", type=int, default=0)
    args = p.parse_args()
    stats = convert(Path(args.docs), Path(args.dat), args.max_terms)
    print("[OK]", stats)


if __name__ == "__main__":
    main()
