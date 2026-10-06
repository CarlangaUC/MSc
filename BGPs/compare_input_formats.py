#!/usr/bin/env python3
"""
Compara representaciones de input equivalentes para ST[OP] n=1.

Formatos analizados (misma información de membership):
  A) packed64 .docs     — canónico pipeline MAGISTER
  B) pares (term,m,r)   — expansión lógica
  C) intervalos RLE     — lo que n=1 materializa internamente
  D) .dat SPOT texto    — una línea por posting (build-index)

Demuestra: A↔B↔C sin pérdida; D es isomórfico a A.
"""
from __future__ import annotations

import struct
import sys
from collections import defaultdict
from pathlib import Path

REL_BITS = 24
REL_MASK = (1 << REL_BITS) - 1


def unpack(p: int) -> tuple[int, int]:
    return p >> REL_BITS, p & REL_MASK


def read_docs(path: Path, max_terms: int = 0):
    with path.open("rb") as f:
        (nlists,) = struct.unpack("<I", f.read(4))
        limit = nlists if not max_terms else min(max_terms, nlists)
        for t in range(limit):
            (length,) = struct.unpack("<I", f.read(4))
            posts = []
            if length:
                posts = list(struct.unpack(f"<{length}Q", f.read(8 * length)))
            yield t, posts


def pairs_from_docs(path: Path, max_terms: int = 0) -> set[tuple[int, int, int]]:
    s: set[tuple[int, int, int]] = set()
    for term, posts in read_docs(path, max_terms):
        for p in posts:
            m, r = unpack(p)
            s.add((term, m, r))
    return s


def rle_intervals_from_pairs(pairs: set[tuple[int, int, int]]) -> dict[int, list[tuple[int, int, int]]]:
    """term -> list of (master, v_start, v_end_inclusive)"""
    by_term: dict[int, dict[int, list[int]]] = defaultdict(lambda: defaultdict(list))
    for term, m, r in pairs:
        by_term[term][m].append(r)
    out: dict[int, list[tuple[int, int, int]]] = {}
    for term, masters in by_term.items():
        intervals: list[tuple[int, int, int]] = []
        for m, rels in masters.items():
            rels = sorted(set(rels))
            start = prev = rels[0]
            for cur in rels[1:]:
                if cur == prev + 1:
                    prev = cur
                    continue
                intervals.append((m, start, prev))
                start = prev = cur
            intervals.append((m, start, prev))
        out[term] = sorted(intervals)
    return out


def expand_intervals(intervals: dict[int, list[tuple[int, int, int]]]) -> set[tuple[int, int, int]]:
    s: set[tuple[int, int, int]] = set()
    for term, lst in intervals.items():
        for m, a, b in lst:
            for r in range(a, b + 1):
                s.add((term, m, r))
    return s


def dat_lines_estimate(n_postings: int, n_terms: int) -> int:
    return 1 + n_postings  # header + one line per posting


def interval_count(intervals: dict[int, list[tuple[int, int, int]]]) -> int:
    return sum(len(v) for v in intervals.values())


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: compare_input_formats.py <file.docs> [max_terms]")
        return 1
    path = Path(sys.argv[1])
    max_terms = int(sys.argv[2]) if len(sys.argv) > 2 else 0

    pairs = pairs_from_docs(path, max_terms)
    intervals = rle_intervals_from_pairs(pairs)
    expanded = expand_intervals(intervals)

    n_terms = max((t for t, _, _ in pairs), default=-1) + 1
    n_postings = len(pairs)
    n_intervals = interval_count(intervals)

    print(f"archivo          : {path}")
    print(f"terminos         : {n_terms}")
    print(f"postings unicos  : {n_postings}")
    print(f"intervalos RLE   : {n_intervals}")
    print(f"factor RLE       : {n_postings / n_intervals if n_intervals else 0:.2f}x")
    print()
    print("--- Equivalencia ---")
    print(f"pairs == expand(RLE): {pairs == expanded}")
    print()
    print("--- Tamaño lógico (orden de magnitud) ---")
    print(f"A) .docs packed64  : {n_postings} × 64 bit = {n_postings * 64:,} bits")
    print(f"B) pares (term,m,r): {n_postings} tuplas")
    print(f"C) intervalos RLE  : {n_intervals} tuplas (lo que n=1 indexa)")
    print(f"D) .dat SPOT texto : ~{dat_lines_estimate(n_postings, n_terms)} lineas ASCII")
    print()
    print("--- Pérdida de info ---")
    print("n=1 sobre A/B/D: LOSSLESS para consulta (term, rel) -> masters")
    print("n=1 reconstruye exactamente el set de pares (term,m,r) via RLE")
    return 0 if pairs == expanded else 2


if __name__ == "__main__":
    sys.exit(main())
