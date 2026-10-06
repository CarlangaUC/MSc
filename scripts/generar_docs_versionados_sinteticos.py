#!/usr/bin/env python3
"""
Generador sintético de listas invertidas versionadas (.docs packed64 + .voc).

Modelo:
  - Cada término es una posting list versionada: un snapshot por versión.
  - Cada snapshot es un conjunto de documentos (master) en [0, universo).
  - sparse: borrado Bernoulli por miembro activo + inserciones binomiales.
  - toggle: ventana muestreada del universo; cada doc cambia de estado con prob p.
  - |U| y nº de versiones pueden ser fijos o variar por término.

Salida compatible con zdd_cudd_plus_t: el ancho de los campos master|rel lo
define `packed64_layout` (derivado de version_packing.h del motor uiHRDC).

Uso:
  python3 scripts/generar_docs_versionados_sinteticos.py
  python3 scripts/generar_docs_versionados_sinteticos.py --versions 50 --seed 1
  python3 scripts/generar_docs_versionados_sinteticos.py --run-zdd --terms 100 --versions 20
  python3 scripts/generar_docs_versionados_sinteticos.py --versions-min 80 --versions-max 200 \
      --universe-size-min 262144 --universe-size-max 16777216
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import struct
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

import numpy as np

from packed64_layout import (
    MASTER_BITS,
    MASTER_MASK,
    MASTER_SHIFT,
    REL_BITS,
    REL_MASK,
    UNIVERSE_SIZE,
    meta_fields,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "resultados_test"
GENERATOR = "generar_docs_versionados_sinteticos.py"

_MASTER_SHIFT = np.uint64(MASTER_SHIFT)
_REL_MASK64 = np.uint64(REL_MASK)


@dataclass
class GenParams:
    terms: int
    versions: int
    versions_min: int
    versions_max: int
    universe_size: int
    universe_size_min: int
    universe_size_max: int
    init_distribution: str
    init_card_mean: float
    init_card_min: int
    init_card_max: int
    zipf_alpha: float
    zipf_shift: float
    delete_prob: float
    add_ratio: float
    evolution_model: str
    toggle_prob: float
    toggle_window: int
    seed: int

    @property
    def versions_fixed(self) -> bool:
        return self.versions_min == self.versions_max

    @property
    def universe_fixed(self) -> bool:
        return self.universe_size_min == self.universe_size_max


@dataclass
class TermMetrics:
    term_id: int
    init_card: int
    final_card: int
    mean_card: int
    min_card: int
    max_card: int
    total_adds: int
    total_deletes: int
    total_changes: int
    distinct_snapshots: int
    repeated_snapshot_steps: int
    mean_jaccard_consecutive: float
    n_postings: int
    n_pairs_uniq: int
    n_snap_elems: int
    n_rle_intervals: int
    mean_run_len: float
    max_run_len: int
    pct_runs_len1: float


@dataclass
class GlobalSummary:
    params: GenParams
    n_terms: int
    n_versions: int
    universe_size: int
    min_versions_per_term: int
    max_versions_per_term: int
    mean_versions_per_term: float
    min_universe_per_term: int
    max_universe_per_term: int
    mean_universe_per_term: float
    n_postings: int
    n_pairs_uniq: int
    n_snap_elems: int
    n_rle_intervals: int
    bytes_docs: int
    mean_card_per_term: float
    mean_card_per_version: float
    mean_jaccard_consecutive: float
    mean_distinct_snapshots: float
    mean_changes_per_step: float
    churn_rate: float
    persistence_rate: float
    factor_raw_over_snap: float
    factor_raw_over_rle: float
    estimated_postings: int
    max_master: int
    max_rel: int


# ---------------------------------------------------------------------------
# Escritura de formatos (.voc uiHRDC, .docs packed64, .meta)
# ---------------------------------------------------------------------------


def bit_pack_offsets(offsets: list[int], elem_size: int) -> bytes:
    """Empaqueta offsets LSB-first; uiHRDC lee uint32 words (padding a 4 bytes)."""
    mask = (1 << elem_size) - 1
    buf = bytearray()
    acc = 0
    nbits = 0
    for off in offsets:
        acc |= (off & mask) << nbits
        nbits += elem_size
        while nbits >= 8:
            buf.append(acc & 0xFF)
            acc >>= 8
            nbits -= 8
    if nbits:
        buf.append(acc & 0xFF)
    buf.extend(b"\x00" * (-len(buf) % 4))
    return bytes(buf)


def write_vocabulary(path: Path, words: list[str]) -> None:
    zone = b"".join(w.encode("ascii") for w in words)
    offsets = [0]
    for w in words:
        offsets.append(offsets[-1] + len(w.encode("ascii")))
    elem_size = max(1, max(offsets).bit_length())
    if elem_size > 32:
        raise ValueError("vocabulario demasiado grande para elem_size uiHRDC")
    with path.open("wb") as out:
        out.write(struct.pack("<III", len(words), elem_size, len(zone)))
        out.write(zone)
        out.write(bit_pack_offsets(offsets, elem_size))


def write_docs_meta(path: Path, params: GenParams, total_lists: int) -> None:
    fields = {
        "format": "pisa_docs_v1",
        "generator": GENERATOR,
        "tuple_output": "packed64",
        **meta_fields(),
        "total_lists": total_lists,
        "has_pairs": 0,
        "seed": params.seed,
        "terms": params.terms,
        "versions": params.versions,
        "versions_min": params.versions_min,
        "versions_max": params.versions_max,
        "universe_size": params.universe_size,
        "universe_size_min": params.universe_size_min,
        "universe_size_max": params.universe_size_max,
    }
    meta_path = path.with_suffix(path.suffix + ".meta")
    with meta_path.open("w", encoding="utf-8") as meta:
        for key, value in fields.items():
            meta.write(f"{key}={value}\n")


def write_docs(
    path: Path, packed_lists: Iterable[np.ndarray], n_terms: int
) -> tuple[int, int]:
    """uint32 nlists, luego por lista: uint32 len + len × uint64 empaquetado."""
    n_postings = 0
    max_master = 0
    with path.open("wb") as out:
        out.write(struct.pack("<I", n_terms))
        for packed in packed_lists:
            out.write(struct.pack("<I", packed.size))
            out.write(packed.astype("<u8", copy=False).tobytes())
            n_postings += int(packed.size)
            if packed.size:  # packed viene ordenado: el último tiene el master mayor
                max_master = max(max_master, int(packed[-1] >> _MASTER_SHIFT))
    return n_postings, max_master


def write_terms_csv(path: Path, metrics: list[TermMetrics]) -> None:
    if not metrics:
        path.write_text("", encoding="utf-8")
        return
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(asdict(metrics[0]).keys()))
        writer.writeheader()
        for m in metrics:
            writer.writerow(asdict(m))


def write_manifest(path: Path, summary: GlobalSummary, docs: Path, voc: Path) -> None:
    payload = {
        "generator": GENERATOR,
        "docs": str(docs),
        "voc": str(voc),
        "params": asdict(summary.params),
        "summary": {k: v for k, v in asdict(summary).items() if k != "params"},
    }
    with path.open("w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2)


def validate_docs(path: Path) -> dict[str, int]:
    """Recorre el .docs sin cargarlo en memoria y exige .meta no vacío."""
    size = path.stat().st_size
    total_postings = 0
    with path.open("rb") as f:
        total_lists, = struct.unpack("<I", f.read(4))
        off = 4
        for i in range(total_lists):
            head = f.read(4)
            if len(head) < 4:
                raise ValueError(f"truncado en header lista {i}/{total_lists}")
            length, = struct.unpack("<I", head)
            off += 4 + length * 8
            if off > size:
                raise ValueError(f"truncado en postings lista {i}/{total_lists}")
            f.seek(length * 8, os.SEEK_CUR)
            total_postings += length
    if off != size:
        raise ValueError(f"bytes sobrantes off={off} size={size}")
    meta = path.with_suffix(path.suffix + ".meta")
    if not meta.is_file() or meta.stat().st_size == 0:
        raise ValueError(f"meta ausente o vacio: {meta}")
    return {"lists": total_lists, "postings": total_postings, "bytes": size}


# ---------------------------------------------------------------------------
# Muestreo por término: universo, nº de versiones y cardinalidad inicial
# ---------------------------------------------------------------------------


def sample_universe_per_term(params: GenParams, rng: np.random.Generator) -> np.ndarray:
    lo, hi = params.universe_size_min, params.universe_size_max
    if lo == hi:
        return np.full(params.terms, lo, dtype=np.int64)
    # Log-uniforme en [lo, hi] para más dispersión de |U| por término.
    logs = rng.uniform(np.log(float(lo)), np.log(float(hi)), size=params.terms)
    return np.clip(np.round(np.exp(logs)).astype(np.int64), lo, hi)


def sample_versions_per_term(params: GenParams, rng: np.random.Generator) -> np.ndarray:
    lo, hi = params.versions_min, params.versions_max
    if lo == hi:
        return np.full(params.terms, lo, dtype=np.int64)
    return rng.integers(lo, hi + 1, size=params.terms, dtype=np.int64)


def sample_initial_cards(params: GenParams, universe_per_term: np.ndarray) -> np.ndarray:
    """constant = todos iguales; zipf = head pesado y cola larga (determinista)."""
    n = params.terms
    lo = params.init_card_min
    hi = min(params.init_card_max, int(np.max(universe_per_term)))
    if lo > hi:
        raise ValueError("init_card_min > init_card_max o universo muy pequeño")

    if params.init_distribution == "constant":
        cards = np.full(n, int(round(params.init_card_mean)), dtype=np.int64)
    elif params.init_distribution == "zipf":
        ranks = np.arange(1, n + 1, dtype=np.float64)
        raw = 1.0 / np.power(ranks + params.zipf_shift, params.zipf_alpha)
        raw *= params.init_card_mean * n / raw.sum()
        cards = np.round(raw).astype(np.int64)
    else:
        raise ValueError(f"init_distribution desconocida: {params.init_distribution}")

    return np.minimum(np.clip(cards, lo, hi), universe_per_term)


def sample_ids(universe_size: int, count: int, rng: np.random.Generator) -> set[int]:
    if count <= 0:
        return set()
    return {int(x) for x in rng.choice(universe_size, size=count, replace=False)}


def sample_ids_from(pool: list[int], count: int, rng: np.random.Generator) -> set[int]:
    if count <= 0:
        return set()
    return {int(x) for x in rng.choice(pool, size=count, replace=False)}


def sample_from_complement(
    active: set[int], universe_size: int, n_add: int, rng: np.random.Generator
) -> list[int]:
    """Elige n_add docs fuera de `active`: exacto si queda poco libre, por rechazo si no."""
    remaining = universe_size - len(active)
    if n_add <= 0 or remaining <= 0:
        return []
    n_add = min(n_add, remaining)

    if remaining <= 65536:
        pool = np.setdiff1d(
            np.arange(universe_size, dtype=np.int64),
            np.fromiter(active, dtype=np.int64, count=len(active)),
            assume_unique=True,
        )
        return [int(x) for x in rng.choice(pool, size=n_add, replace=False)]

    chosen: set[int] = set()
    batch = max(n_add * 4, 256)
    while len(chosen) < n_add:
        for c in rng.integers(0, universe_size, size=batch, dtype=np.int64):
            ci = int(c)
            if ci not in active and ci not in chosen:
                chosen.add(ci)
                if len(chosen) >= n_add:
                    break
    return list(chosen)


# ---------------------------------------------------------------------------
# Evolución temporal de un término
# ---------------------------------------------------------------------------


class SnapshotLog:
    """Snapshots de un término más las métricas temporales acumuladas."""

    def __init__(self, active: set[int]) -> None:
        self.snapshots: list[frozenset[int]] = [frozenset(active)]
        self.cards: list[int] = [len(active)]
        self.adds = 0
        self.deletes = 0
        self.changes = 0
        self.jaccards: list[float] = []

    def record(self, prev: set[int], active: set[int], adds: int, deletes: int) -> None:
        self.snapshots.append(frozenset(active))
        self.cards.append(len(active))
        self.adds += adds
        self.deletes += deletes
        self.changes += len(prev.symmetric_difference(active))
        if prev or active:
            union = len(prev | active)
            self.jaccards.append(len(prev & active) / union if union else 1.0)

    def metrics(
        self, term_id: int, n_postings: int, rle: tuple[int, float, int, float]
    ) -> TermMetrics:
        n_rle, mean_run, max_run, pct_runs_len1 = rle
        snaps = self.snapshots
        return TermMetrics(
            term_id=term_id,
            init_card=self.cards[0],
            final_card=self.cards[-1],
            mean_card=int(round(float(np.mean(self.cards)))),
            min_card=min(self.cards),
            max_card=max(self.cards),
            total_adds=self.adds,
            total_deletes=self.deletes,
            total_changes=self.changes,
            distinct_snapshots=len(set(snaps)),
            repeated_snapshot_steps=sum(1 for a, b in zip(snaps, snaps[1:]) if a == b),
            mean_jaccard_consecutive=float(np.mean(self.jaccards)) if self.jaccards else 1.0,
            n_postings=n_postings,
            n_pairs_uniq=n_postings,
            n_snap_elems=sum(self.cards),
            n_rle_intervals=n_rle,
            mean_run_len=mean_run,
            max_run_len=max_run,
            pct_runs_len1=pct_runs_len1,
        )


def evolve_sparse(
    universe_size: int,
    init_card: int,
    num_versions: int,
    delete_prob: float,
    add_ratio: float,
    rng: np.random.Generator,
) -> SnapshotLog:
    init_card = min(init_card, universe_size)
    active = sample_ids(universe_size, init_card, rng)
    log = SnapshotLog(active)

    for _ in range(1, num_versions):
        prev = set(active)

        deletes = 0
        if active and delete_prob > 0:
            members = list(active)
            for doc, drop in zip(members, rng.random(len(members)) < delete_prob):
                if drop:
                    active.discard(doc)
                    deletes += 1

        n_add = 0
        if add_ratio > 0:
            n_add = (
                int(rng.binomial(len(active), add_ratio))
                if active
                else max(1, int(round(add_ratio * max(init_card, 1))))
            )
        added = sample_from_complement(active, universe_size, n_add, rng)
        active.update(added)

        log.record(prev, active, len(added), deletes)
    return log


def evolve_toggle(
    universe_size: int,
    init_card: int,
    num_versions: int,
    toggle_prob: float,
    toggle_window: int,
    rng: np.random.Generator,
) -> SnapshotLog:
    """Recorre una ventana muestreada del universo y voltea cada doc con prob p."""
    window_size = min(max(toggle_window, init_card), universe_size)
    window = [int(x) for x in rng.choice(universe_size, size=window_size, replace=False)]
    active = sample_ids_from(window, min(init_card, window_size), rng)
    log = SnapshotLog(active)

    docs = np.asarray(window, dtype=np.int64)
    present = np.isin(docs, np.fromiter(active, dtype=np.int64, count=len(active)))
    flips = rng.random((max(num_versions - 1, 0), window_size))

    for row in flips:
        prev = active
        flip = row < toggle_prob
        adds = int(np.count_nonzero(flip & ~present))
        deletes = int(np.count_nonzero(flip & present))
        present = present ^ flip
        active = {int(x) for x in docs[present]}
        log.record(prev, active, adds, deletes)
    return log


def evolve_term(
    params: GenParams,
    init_card: int,
    num_versions: int,
    universe_size: int,
    rng: np.random.Generator,
) -> SnapshotLog:
    if params.evolution_model == "toggle":
        return evolve_toggle(
            universe_size,
            init_card,
            num_versions,
            params.toggle_prob,
            params.toggle_window,
            rng,
        )
    return evolve_sparse(
        universe_size,
        init_card,
        num_versions,
        params.delete_prob,
        params.add_ratio,
        rng,
    )


# ---------------------------------------------------------------------------
# Postings y métricas derivadas
# ---------------------------------------------------------------------------


def snapshots_to_packed(snapshots: list[frozenset[int]]) -> np.ndarray:
    """Snapshots → uint64 (master<<MASTER_SHIFT|rel) ordenados por (master, rel)."""
    if len(snapshots) > REL_MASK:
        raise ValueError(f"versiones fuera de rango {REL_BITS}-bit: {len(snapshots)}")
    packed = np.empty(sum(len(s) for s in snapshots), dtype=np.uint64)
    pos = 0
    for rel, snap in enumerate(snapshots, start=1):
        if not snap:
            continue
        masters = np.fromiter(snap, dtype=np.uint64, count=len(snap))
        if int(masters.max()) > MASTER_MASK:
            raise ValueError(
                f"master fuera de rango {MASTER_BITS}-bit: {int(masters.max())}"
            )
        packed[pos : pos + len(snap)] = (masters << _MASTER_SHIFT) | np.uint64(rel)
        pos += len(snap)
    packed.sort()
    return packed


def rle_stats(packed: np.ndarray) -> tuple[int, float, int, float]:
    """Corridas maximales de `rel` consecutivos por master: (n, media, max, % len 1)."""
    if packed.size == 0:
        return 0, 0.0, 0, 0.0
    masters = packed >> _MASTER_SHIFT
    rels = packed & _REL_MASK64
    new_run = np.empty(packed.size, dtype=bool)
    new_run[0] = True
    new_run[1:] = (masters[1:] != masters[:-1]) | (rels[1:] != rels[:-1] + np.uint64(1))
    starts = np.flatnonzero(new_run)
    lengths = np.diff(np.append(starts, packed.size))
    return (
        int(starts.size),
        float(lengths.mean()),
        int(lengths.max()),
        100.0 * float(np.count_nonzero(lengths == 1)) / lengths.size,
    )


def estimate_postings(
    params: GenParams,
    cards: np.ndarray,
    versions_per_term: np.ndarray,
    universe_per_term: np.ndarray,
) -> int:
    if params.evolution_model == "toggle":
        window = np.minimum(np.maximum(params.toggle_window, cards), universe_per_term)
        avg_card = (cards + window / 2.0) / 2.0  # ventana en régimen estacionario ~ w/2
        return int(np.sum(np.floor(avg_card * versions_per_term)))
    return int(np.sum(cards * versions_per_term))


def estimate_bytes(n_postings: int, n_terms: int) -> int:
    return 4 + n_terms * 8 + n_postings * 8


def _mean(values: list[float] | np.ndarray) -> float:
    return float(np.mean(values)) if len(values) else 0.0


def summarize(
    params: GenParams,
    metrics: list[TermMetrics],
    bytes_docs: int,
    est: int,
    max_master: int,
    versions_per_term: np.ndarray,
    universe_per_term: np.ndarray,
) -> GlobalSummary:
    n_post = sum(m.n_postings for m in metrics)
    n_snap = sum(m.n_snap_elems for m in metrics)
    n_rle = sum(m.n_rle_intervals for m in metrics)
    mean_card = _mean([m.mean_card for m in metrics])
    mean_jaccard = _mean([m.mean_jaccard_consecutive for m in metrics])
    mean_steps = float(np.mean(np.maximum(versions_per_term - 1, 1)))
    changes_per_step = _mean([m.total_changes for m in metrics]) / mean_steps
    return GlobalSummary(
        params=params,
        n_terms=params.terms,
        n_versions=int(np.max(versions_per_term)),
        universe_size=int(np.max(universe_per_term)),
        min_versions_per_term=int(np.min(versions_per_term)),
        max_versions_per_term=int(np.max(versions_per_term)),
        mean_versions_per_term=float(np.mean(versions_per_term)),
        min_universe_per_term=int(np.min(universe_per_term)),
        max_universe_per_term=int(np.max(universe_per_term)),
        mean_universe_per_term=float(np.mean(universe_per_term)),
        n_postings=n_post,
        n_pairs_uniq=n_post,
        n_snap_elems=n_snap,
        n_rle_intervals=n_rle,
        bytes_docs=bytes_docs,
        mean_card_per_term=mean_card,
        mean_card_per_version=n_post / max(int(np.sum(versions_per_term)), 1),
        mean_jaccard_consecutive=mean_jaccard,
        mean_distinct_snapshots=_mean([m.distinct_snapshots for m in metrics]),
        mean_changes_per_step=changes_per_step,
        churn_rate=changes_per_step / max(mean_card, 1.0),
        persistence_rate=mean_jaccard,
        factor_raw_over_snap=n_post / max(n_snap, 1),
        factor_raw_over_rle=n_post / max(n_rle, 1),
        estimated_postings=est,
        max_master=max_master,
        max_rel=int(np.max(versions_per_term)),
    )


def print_summary(summary: GlobalSummary, docs_path: Path, voc_path: Path) -> None:
    p = summary.params
    print("\n=== Resumen generación sintética ===")
    print(f"docs={docs_path}")
    print(f"voc={voc_path}")
    if p.versions_fixed and p.universe_fixed:
        print(f"terms={p.terms} versions={p.versions} universe={p.universe_size}")
    else:
        print(
            f"terms={p.terms} versions∈[{p.versions_min},{p.versions_max}] "
            f"universe∈[{p.universe_size_min},{p.universe_size_max}]"
        )
        print(
            f"  mean_versions={summary.mean_versions_per_term:.1f} "
            f"mean_universe={summary.mean_universe_per_term:,.0f}"
        )
    if p.evolution_model == "toggle":
        print(
            f"model=toggle toggle_prob={p.toggle_prob} toggle_window={p.toggle_window} "
            f"init={p.init_distribution} seed={p.seed}"
        )
    else:
        print(f"model=sparse delete_prob={p.delete_prob} add_ratio={p.add_ratio} seed={p.seed}")
    print(f"n_postings={summary.n_postings:,} bytes_docs={summary.bytes_docs:,}")
    print(f"n_snap_elems={summary.n_snap_elems:,} n_rle_intervals={summary.n_rle_intervals:,}")
    print(
        f"factor_raw/snap={summary.factor_raw_over_snap:.3f}x  "
        f"factor_raw/rle={summary.factor_raw_over_rle:.3f}x"
    )
    print(
        f"mean_card/term={summary.mean_card_per_term:.2f} "
        f"mean_card/version={summary.mean_card_per_version:.2f}"
    )
    print(
        f"mean_jaccard={summary.mean_jaccard_consecutive:.4f} "
        f"distinct_snaps/term={summary.mean_distinct_snapshots:.2f}"
    )
    print(f"churn_rate={summary.churn_rate:.4f} persistence={summary.persistence_rate:.4f}")
    print(f"max_master={summary.max_master} max_rel={summary.max_rel}")


# ---------------------------------------------------------------------------
# Generación completa
# ---------------------------------------------------------------------------


def validate_params(params: GenParams) -> None:
    if not 1 <= params.versions_min <= params.versions_max <= REL_MASK:
        raise ValueError(f"versions debe estar en [1, {REL_MASK}]")
    if not 1 <= params.universe_size_min <= params.universe_size_max <= UNIVERSE_SIZE:
        raise ValueError(f"universe_size invalido para master {MASTER_BITS}-bit")


def generate_all(
    params: GenParams, out_docs: Path, out_voc: Path, force: bool, max_postings: int
) -> tuple[GlobalSummary, list[TermMetrics]]:
    validate_params(params)

    # No reordenar los spawn: define qué seed recibe cada término.
    base_ss = np.random.SeedSequence(params.seed)
    _cards_ss, struct_ss = base_ss.spawn(2)
    struct_rng = np.random.Generator(np.random.PCG64(struct_ss))
    term_seeds = base_ss.spawn(params.terms)

    universe_per_term = sample_universe_per_term(params, struct_rng)
    versions_per_term = sample_versions_per_term(params, struct_rng)
    cards = sample_initial_cards(params, universe_per_term)

    est = estimate_postings(params, cards, versions_per_term, universe_per_term)
    est_bytes = estimate_bytes(est, params.terms)
    print(f"[EST] postings~{est:,} bytes~{est_bytes:,} ({est_bytes / (1024**3):.2f} GiB)")
    if est > max_postings and not force:
        raise SystemExit(
            f"ERROR: estimación {est:,} postings supera --max-postings={max_postings:,}. "
            "Use --force o reduzca terms/versions/init-card."
        )

    write_vocabulary(out_voc, [f"syn_term_{i:06d}" for i in range(params.terms)])

    metrics: list[TermMetrics] = []

    def packed_per_term() -> Iterable[np.ndarray]:
        progress_step = max(1, params.terms // 10)
        for t in range(params.terms):
            rng = np.random.Generator(np.random.PCG64(term_seeds[t]))
            log = evolve_term(
                params,
                int(cards[t]),
                int(versions_per_term[t]),
                int(universe_per_term[t]),
                rng,
            )
            packed = snapshots_to_packed(log.snapshots)
            metrics.append(log.metrics(t, int(packed.size), rle_stats(packed)))
            yield packed
            if (t + 1) % progress_step == 0 or t + 1 == params.terms:
                print(f"[GEN] {t + 1}/{params.terms} terms...")

    _n_postings, max_master = write_docs(out_docs, packed_per_term(), params.terms)
    write_docs_meta(out_docs, params, params.terms)
    summary = summarize(
        params,
        metrics,
        out_docs.stat().st_size,
        est,
        max_master,
        versions_per_term,
        universe_per_term,
    )
    return summary, metrics


def default_stem(params: GenParams) -> str:
    vtag = f"v{params.versions}" if params.versions_fixed else f"v{params.versions_min}-{params.versions_max}"
    if params.universe_fixed:
        utag = f"u{params.universe_size}"
    else:
        utag = f"u{params.universe_size_min}-{params.universe_size_max}"
    base = f"synthetic_t{params.terms}_{vtag}_{utag}"
    if params.evolution_model == "toggle":
        return f"{base}_toggle{params.toggle_prob:g}_w{params.toggle_window}_s{params.seed}"
    return f"{base}_p{params.delete_prob:g}_a{params.add_ratio:g}_s{params.seed}"


# ---------------------------------------------------------------------------
# Pipeline ZDD opcional
# ---------------------------------------------------------------------------


def run_zdd_pipeline(
    docs: Path,
    voc: Path,
    pack: Path,
    encoding: str,
    run_optimize: bool,
    heur: str,
    max_sift: int,
) -> None:
    zdd = ROOT / "zdd_cudd_plus_t"
    measure = ROOT / "scripts" / "measure_zpack_bpi"
    if not (zdd.is_file() and os.access(zdd, os.X_OK)):
        print("[WARN] zdd_cudd_plus_t no disponible; omitiendo build", file=sys.stderr)
        return

    env = os.environ.copy()
    lib = ROOT / "cudd" / "cudd" / ".libs"
    env["LD_LIBRARY_PATH"] = f"{lib}:{env.get('LD_LIBRARY_PATH', '')}"

    def run(label: str, cmd: list[str]) -> None:
        print(f"[ZDD] {label}: {' '.join(cmd)}")
        subprocess.run(cmd, check=True, env=env)

    def measure_pack(label: str, target: Path) -> None:
        if measure.is_file() and os.access(measure, os.X_OK):
            print(f"[ZDD] measure {label}")
            subprocess.run([str(measure), str(target), str(docs)], check=True, env=env)

    evol = pack.with_name(pack.name.replace(".zpack", "_evol.csv"))
    run(
        "build",
        [str(zdd), "build", encoding, str(docs), str(voc), str(pack), "0", str(evol), "500"],
    )
    measure_pack("baseline", pack)

    if run_optimize:
        pack_opt = pack.with_name(f"{pack.stem}_{heur.replace('+', '_')}.zpack")
        run(
            "optimize",
            [
                str(zdd),
                "optimize",
                encoding,
                str(pack),
                str(docs),
                str(pack_opt),
                heur,
                str(max_sift),
                "0",
            ],
        )
        measure_pack("optimized", pack_opt)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generador sintético .docs versionados para ZDD")
    p.add_argument("--terms", type=int, default=10_000)
    p.add_argument("--versions", type=int, default=100, help="fijo si no se usa --versions-min/max")
    p.add_argument(
        "--versions-min",
        type=int,
        default=0,
        help="versiones por término (uniforme); 0 => --versions",
    )
    p.add_argument(
        "--versions-max",
        type=int,
        default=0,
        help="versiones por término (uniforme); 0 => --versions",
    )
    p.add_argument("--universe-size", type=int, default=1 << 21, help="fijo si no min/max")
    p.add_argument(
        "--universe-size-min",
        type=int,
        default=0,
        help="|U| por término log-uniforme; 0 => --universe-size",
    )
    p.add_argument(
        "--universe-size-max",
        type=int,
        default=0,
        help="|U| por término log-uniforme; 0 => --universe-size",
    )
    p.add_argument("--init-distribution", choices=["constant", "zipf"], default="zipf")
    p.add_argument("--init-card-mean", type=float, default=50.0)
    p.add_argument("--init-card-min", type=int, default=1)
    p.add_argument("--init-card-max", type=int, default=5000)
    p.add_argument("--zipf-alpha", type=float, default=1.0)
    p.add_argument("--zipf-shift", type=float, default=1.0)
    p.add_argument("--delete-prob", type=float, default=0.01)
    p.add_argument("--add-ratio", type=float, default=0.01)
    p.add_argument(
        "--evolution-model",
        choices=["sparse", "toggle"],
        default="sparse",
        help="sparse=add/delete sobre activos; toggle=flip prob p en ventana",
    )
    p.add_argument(
        "--toggle-prob",
        type=float,
        default=0.01,
        help="prob. de voltear presencia de cada doc en la ventana (modelo toggle)",
    )
    p.add_argument(
        "--toggle-window",
        type=int,
        default=0,
        help="docs monitoreados por término (0 => max(init-card-mean*4, 256))",
    )
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--output-dir", type=Path, default=DEFAULT_OUT)
    p.add_argument("--stem", type=str, default="")
    p.add_argument("--max-postings", type=int, default=500_000_000)
    p.add_argument("--force", action="store_true")
    p.add_argument("--run-zdd", action="store_true")
    p.add_argument("--run-optimize", action="store_true")
    p.add_argument("--encoding", choices=["u+t", "log"], default="u+t")
    p.add_argument("--heur", default="nodes_desc+sift")
    p.add_argument("--max-sift", type=int, default=200)
    return p.parse_args()


def params_from_args(args: argparse.Namespace) -> GenParams:
    def range_or_fixed(lo: int, hi: int, fixed: int) -> tuple[int, int]:
        return (lo if lo > 0 else fixed, hi if hi > 0 else fixed)

    versions_min, versions_max = range_or_fixed(args.versions_min, args.versions_max, args.versions)
    universe_min, universe_max = range_or_fixed(
        args.universe_size_min, args.universe_size_max, args.universe_size
    )
    return GenParams(
        terms=args.terms,
        versions=args.versions,
        versions_min=versions_min,
        versions_max=versions_max,
        universe_size=args.universe_size,
        universe_size_min=universe_min,
        universe_size_max=universe_max,
        init_distribution=args.init_distribution,
        init_card_mean=args.init_card_mean,
        init_card_min=args.init_card_min,
        init_card_max=args.init_card_max,
        zipf_alpha=args.zipf_alpha,
        zipf_shift=args.zipf_shift,
        delete_prob=args.delete_prob,
        add_ratio=args.add_ratio,
        evolution_model=args.evolution_model,
        toggle_prob=args.toggle_prob,
        toggle_window=(
            args.toggle_window
            if args.toggle_window > 0
            else max(int(round(args.init_card_mean * 4)), 256)
        ),
        seed=args.seed,
    )


def main() -> int:
    args = parse_args()
    params = params_from_args(args)
    if params.evolution_model == "toggle":
        print(
            f"[CFG] toggle p={params.toggle_prob} window={params.toggle_window} "
            f"init={params.init_distribution} mean={params.init_card_mean}"
        )

    stem = args.stem or default_stem(params)
    out_dir = args.output_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    docs_path = out_dir / f"{stem}.docs"
    voc_path = out_dir / f"{stem}.voc"
    manifest_path = out_dir / f"{stem}_manifest.json"
    csv_path = out_dir / f"{stem}_terms.csv"
    pack_path = out_dir / f"{stem}_plus_t.zpack"

    summary, metrics = generate_all(params, docs_path, voc_path, args.force, args.max_postings)
    write_terms_csv(csv_path, metrics)
    write_manifest(manifest_path, summary, docs_path, voc_path)

    stats = validate_docs(docs_path)
    print(
        f"[OK] validate docs lists={stats['lists']} postings={stats['postings']} "
        f"bytes={stats['bytes']}"
    )
    print_summary(summary, docs_path, voc_path)
    print(f"[OK] manifest={manifest_path}")
    print(f"[OK] terms_csv={csv_path}")

    if args.run_zdd:
        run_zdd_pipeline(
            docs_path,
            voc_path,
            pack_path,
            args.encoding,
            args.run_optimize,
            args.heur,
            args.max_sift,
        )
        print(f"[OK] zpack={pack_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
