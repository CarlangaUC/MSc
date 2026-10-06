#!/usr/bin/env python3
"""
Auditor del .docs packed64 con dos objetivos:

1. Determinar la semantica del campo `rel`:
   - eje GLOBAL  -> cada master ocupa un rango de rel disjunto de los demas
   - eje PER-PAGE-> los rangos de rel se solapan entre masters (todos parten de 0/1)

2. Medir el factor de compresion de RLE sobre versiones, que es lo que decide
   si conviene el metatrie por intervalos (CLTJ) frente a snapshots (ZDD).

Uso:
    python3 inspect_docs_para_metatrie.py <archivo.docs> [max_terms]
"""

import struct
import sys
from collections import defaultdict

MASTER_BITS = 40
REL_BITS = 24
REL_MASK = (1 << REL_BITS) - 1


def unpack(x):
    return (x >> REL_BITS), (x & REL_MASK)


def read_docs(path, max_terms=0):
    with open(path, "rb") as f:
        (nlists,) = struct.unpack("<I", f.read(4))
        limit = nlists if max_terms <= 0 else min(max_terms, nlists)
        for t in range(limit):
            raw = f.read(4)
            if len(raw) < 4:
                return
            (length,) = struct.unpack("<I", raw)
            if length == 0:
                yield t, []
                continue
            buf = f.read(8 * length)
            if len(buf) < 8 * length:
                return
            yield t, list(struct.unpack(f"<{length}Q", buf))


def runs_of(sorted_vals):
    """Comprime una lista ordenada de enteros en corridas consecutivas."""
    out = []
    start = prev = sorted_vals[0]
    for v in sorted_vals[1:]:
        if v == prev:
            continue
        if v == prev + 1:
            prev = v
            continue
        out.append((start, prev))
        start = prev = v
    out.append((start, prev))
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    max_terms = int(sys.argv[2]) if len(sys.argv) > 2 else 0

    with open(path, "rb") as f:
        (nlists,) = struct.unpack("<I", f.read(4))
    print(f"archivo            : {path}")
    print(f"nlists (V)         : {nlists}")

    n_postings = 0
    n_terms_nonempty = 0
    n_intervals = 0
    n_pairs_uniq = 0
    n_snap_elems = 0

    rel_min_global = None
    rel_max_global = 0
    master_max = 0

    # rango de rel por master (para decidir global vs per-page)
    master_rel_min = {}
    master_rel_max = {}

    # distribucion de largo de corrida
    run_len_hist = defaultdict(int)
    # versiones distintas por termino
    versions_per_term = []
    masters_per_term = []

    for t, postings in read_docs(path, max_terms):
        n_postings += len(postings)
        if not postings:
            continue
        n_terms_nonempty += 1

        by_master = defaultdict(list)
        by_rel = defaultdict(set)
        for x in postings:
            m, r = unpack(x)
            by_master[m].append(r)
            by_rel[r].add(m)
            if m > master_max:
                master_max = m
            if r > rel_max_global:
                rel_max_global = r
            if rel_min_global is None or r < rel_min_global:
                rel_min_global = r
            lo = master_rel_min.get(m)
            if lo is None or r < lo:
                master_rel_min[m] = r
            hi = master_rel_max.get(m)
            if hi is None or r > hi:
                master_rel_max[m] = r

        masters_per_term.append(len(by_master))
        versions_per_term.append(len(by_rel))

        # snapshots distintos (mismo criterio que collectVersionSnapshots)
        snaps = set()
        for r, ms in by_rel.items():
            snaps.add(frozenset(ms))
        for s in snaps:
            n_snap_elems += len(s)

        for m, rels in by_master.items():
            rels.sort()
            uniq = sorted(set(rels))
            n_pairs_uniq += len(uniq)
            rr = runs_of(uniq)
            n_intervals += len(rr)
            for a, b in rr:
                run_len_hist[b - a + 1] += 1

    print(f"terminos no vacios : {n_terms_nonempty}")
    print(f"n_raw (postings)   : {n_postings}")
    print(f"n_pairs_uniq       : {n_pairs_uniq}")
    print(f"n_snap_elems       : {n_snap_elems}")
    print(f"master max         : {master_max}")
    print(f"rel rango          : [{rel_min_global}, {rel_max_global}]")
    print(f"masters distintos  : {len(master_rel_min)}")

    print()
    print("--- Semantica de rel ---")
    # Si rel fuese eje global, los rangos [min,max] por master serian disjuntos.
    # Medimos solapamiento: cuantos masters cubren el rel mas frecuente.
    spans = sorted((master_rel_min[m], master_rel_max[m]) for m in master_rel_min)
    overlap = 0
    if spans:
        # barrido: maximo numero de rangos simultaneos
        events = []
        for lo, hi in spans:
            events.append((lo, 1))
            events.append((hi + 1, -1))
        events.sort()
        cur = 0
        for _, d in events:
            cur += d
            if cur > overlap:
                overlap = cur
    print(f"max masters solapados en un mismo rel : {overlap}")
    print(f"  -> {'PER-PAGE (rel relativo a cada master)' if overlap > 1 else 'GLOBAL (rangos disjuntos)'}")
    if spans[:5]:
        print(f"  primeros spans por master: {spans[:5]}")

    print()
    print("--- RLE sobre versiones (lo que consumiria el metatrie) ---")
    print(f"intervalos de presencia : {n_intervals}")
    if n_intervals:
        print(f"factor n_raw / intervalos      : {n_postings / n_intervals:.2f}x")
        print(f"factor n_pairs_uniq/intervalos : {n_pairs_uniq / n_intervals:.2f}x")
    if n_snap_elems:
        print(f"factor n_raw / n_snap_elems    : {n_postings / n_snap_elems:.2f}x  (baseline ZDD)")

    print()
    print("--- Distribucion de largo de corrida ---")
    tot = sum(run_len_hist.values())
    for k in sorted(run_len_hist)[:12]:
        v = run_len_hist[k]
        print(f"  largo {k:>6} : {v:>10}  ({100.0*v/tot:5.2f}%)")
    if run_len_hist:
        mx = max(run_len_hist)
        print(f"  largo maximo   : {mx}")
        singles = run_len_hist.get(1, 0)
        print(f"  corridas de largo 1 : {100.0*singles/tot:.2f}%")

    if masters_per_term:
        masters_per_term.sort()
        versions_per_term.sort()
        mid = len(masters_per_term) // 2
        print()
        print("--- Por termino (mediana / max) ---")
        print(f"masters por termino  : {masters_per_term[mid]} / {masters_per_term[-1]}")
        print(f"versiones por termino: {versions_per_term[mid]} / {versions_per_term[-1]}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
