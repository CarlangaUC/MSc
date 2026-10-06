#!/usr/bin/env python3
"""
Visualiza la vista por intervalos que usa la metatrie (RLE en rel por master).

No dibuja el trie SDSL interno (wavelet); muestra la membresía M_t como barras
[rel_a, rel_b) por master — equivalente a los spot_quad tras RLE.

Uso:
  python3 scripts/viz_metatrie_intervals.py \\
    --docs BGPs/toy_st_op.docs --term 0 --out /tmp/term0.png

  python3 scripts/viz_metatrie_intervals.py \\
    --docs resultados_test/wiki_2gb_uihrdc_packed64.docs --term 100 \\
    --out term100.png --max-masters 80
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from packed64_layout import unpack  # noqa: E402


def read_term_postings(docs: Path, term_id: int) -> list[tuple[int, int]]:
    data = docs.read_bytes()
    if len(data) < 4:
        raise ValueError(f"{docs}: vacío")
    nlists, = struct.unpack_from("<I", data, 0)
    if term_id < 0 or term_id >= nlists:
        raise ValueError(f"term_id={term_id} fuera de [0, {nlists})")
    off = 4
    for t in range(nlists):
        if off + 4 > len(data):
            raise ValueError(f"truncado en header t={t}")
        length, = struct.unpack_from("<I", data, off)
        off += 4
        if t == term_id:
            pairs: list[tuple[int, int]] = []
            for _ in range(length):
                packed, = struct.unpack_from("<Q", data, off)
                off += 8
                pairs.append(unpack(packed))
            return pairs
        off += length * 8
    raise ValueError("term no encontrado")


def postings_to_intervals(postings: list[tuple[int, int]]) -> list[tuple[int, int, int]]:
    """RLE en rel, mismo criterio que meta_trie_edd (master fijo, rel consecutivos)."""
    if not postings:
        return []
    sorted_p = sorted(postings, key=lambda x: (x[0], x[1]))
    out: list[tuple[int, int, int]] = []
    m, r = sorted_p[0]
    start = r
    prev_r = r
    for m2, r2 in sorted_p[1:]:
        if m2 == m and r2 == prev_r + 1:
            prev_r = r2
            continue
        out.append((m, start, prev_r + 1))
        m, start, prev_r = m2, r2, r2
    out.append((m, start, prev_r + 1))
    return out


def format_bytes(n: int) -> str:
    if n >= 1024**3:
        return f"{n / (1024**3):.2f} GiB"
    if n >= 1024**2:
        return f"{n / (1024**2):.0f} MiB"
    if n >= 1024:
        return f"{n / 1024:.1f} KiB"
    return f"{n} B"


def plot_intervals(
    term_id: int,
    intervals: list[tuple[int, int, int]],
    out: Path,
    title_extra: str = "",
    dpi: int = 120,
) -> None:
    import matplotlib.pyplot as plt

    if not intervals:
        fig, ax = plt.subplots(figsize=(8, 2))
        ax.text(0.5, 0.5, f"término {term_id}: sin postings", ha="center", va="center")
        ax.axis("off")
        fig.savefig(out, dpi=dpi, bbox_inches="tight")
        plt.close(fig)
        return

    masters = sorted({m for m, _, _ in intervals})
    y_map = {m: i for i, m in enumerate(masters)}
    t_max = max(b for _, _, b in intervals)

    fig_h = max(3.0, min(24.0, 0.22 * len(masters) + 1.5))
    fig, ax = plt.subplots(figsize=(10, fig_h))

    for m, a, b in intervals:
        y = y_map[m]
        ax.barh(y, b - a, left=a, height=0.7, color="#2563eb", edgecolor="#1e40af", linewidth=0.3)

    ax.set_xlabel(r"versión rel ($\tau$), intervalos half-open $[\tau_a,\tau_b)$")
    ax.set_ylabel("master $u$ (índice en fila)")
    ax.set_yticks(range(len(masters)))
    ax.set_yticklabels([str(m) for m in masters], fontsize=7 if len(masters) > 40 else 9)
    ax.set_xlim(0, max(t_max, 1))
    title = f"Metatrie — vista intervalos, término {term_id} ({len(intervals)} intervalos, {len(masters)} masters)"
    if title_extra:
        title += f"\n{title_extra}"
    ax.set_title(title, fontsize=10)
    ax.grid(axis="x", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out, dpi=dpi, bbox_inches="tight")
    plt.close(fig)


def plot_snapshots_small(
    postings: list[tuple[int, int]],
    out: Path,
    term_id: int,
    max_rel: int = 64,
    max_masters: int = 40,
) -> None:
    """Heatmap masters×rel (solo corpora chicos / recorte)."""
    import matplotlib.pyplot as plt
    import numpy as np

    if not postings:
        return
    masters = sorted({m for m, _ in postings})[:max_masters]
    rels = sorted({r for _, r in postings if r < max_rel})
    if not masters or not rels:
        return
    m_idx = {m: i for i, m in enumerate(masters)}
    r_idx = {r: j for j, r in enumerate(rels)}
    grid = np.zeros((len(masters), len(rels)), dtype=np.uint8)
    for m, r in postings:
        if m in m_idx and r in r_idx:
            grid[m_idx[m], r_idx[r]] = 1

    fig, ax = plt.subplots(figsize=(max(6, len(rels) * 0.15), max(3, len(masters) * 0.2)))
    ax.imshow(grid, aspect="auto", cmap="Blues", interpolation="nearest")
    ax.set_xlabel(r"$\tau$ (muestra)")
    ax.set_ylabel("master (muestra)")
    ax.set_title(f"Snapshots $S^\\tau$ (término {term_id}), recorte")
    fig.tight_layout()
    stem = out.with_suffix("")
    snap_path = Path(str(stem) + "_snapshots.png")
    fig.savefig(snap_path, dpi=120, bbox_inches="tight")
    plt.close(fig)
    print(f"[OK] snapshots -> {snap_path}")


def main() -> int:
    ap = argparse.ArgumentParser(description="PNG de intervalos metatrie (RLE) por término")
    ap.add_argument("--docs", type=Path, required=True)
    ap.add_argument("--term", type=int, required=True, help="term_id (índice de lista)")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument(
        "--max-masters",
        type=int,
        default=0,
        help="si >0, solo los primeros N masters distintos (por orden)",
    )
    ap.add_argument(
        "--also-snapshots",
        action="store_true",
        help="genera además *_snapshots.png si el término es pequeño",
    )
    ap.add_argument("--dpi", type=int, default=120)
    args = ap.parse_args()

    if not args.docs.is_file():
        print(f"ERROR: no existe {args.docs}", file=sys.stderr)
        return 1

    postings = read_term_postings(args.docs, args.term)
    if args.max_masters > 0:
        keep = sorted({m for m, _ in postings})[: args.max_masters]
        keep_set = set(keep)
        postings = [(m, r) for m, r in postings if m in keep_set]

    intervals = postings_to_intervals(postings)
    docs_size = args.docs.stat().st_size
    extra = f"{args.docs.name} ({format_bytes(docs_size)}), {len(postings)} postings"
    args.out.parent.mkdir(parents=True, exist_ok=True)
    plot_intervals(args.term, intervals, args.out, title_extra=extra, dpi=args.dpi)
    print(f"[OK] intervalos -> {args.out} ({len(intervals)} intervalos)")

    if args.also_snapshots and len(postings) <= 5000:
        plot_snapshots_small(postings, args.out, args.term)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
