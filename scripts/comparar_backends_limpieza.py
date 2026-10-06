#!/usr/bin/env python3
"""
Compara backends de limpiar_corpus_wiki.py sobre una muestra de revisiones.

Uso:
  .venv/bin/python3 scripts/comparar_backends_limpieza.py \\
    --input uiHRDC/uiHRDC/data/texts/wiki_100mb.txt \\
    --nivel marcado \\
    --sample 500
"""

from __future__ import annotations

import argparse
import csv
import random
import re
import sys
import time
from pathlib import Path

# Importar funciones del limpiador
sys.path.insert(0, str(Path(__file__).resolve().parent))
from limpiar_corpus_wiki import BACKENDS, clean_line  # noqa: E402

WIKI_MARKUP = re.compile(r"\{\{|\}\}|\[\[|\]\]|\{\||\|\}|'{2,}|={2,}|(?:^|\s)[|!][-+|!]", re.M)
HTML_MARKUP = re.compile(r"</?[A-Za-z][^>]*>|&(?:lt|gt|quot|amp|nbsp|#\d+|#x[0-9a-f]+);", re.I)
URL_MARKUP = re.compile(r"(?:https?|ftp)://", re.I)
TORSEN_OK = re.compile(r"^[a-z0-9 ]*$")
TOKEN = re.compile(r"\w+", re.UNICODE)


def jaccard(a: set[str], b: set[str]) -> float:
    if not a and not b:
        return 1.0
    return len(a & b) / len(a | b)


def main() -> int:
    parser = argparse.ArgumentParser(description="Comparar backends de limpieza wiki")
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--nivel", default="marcado", choices=["marcado", "torsen"])
    parser.add_argument("--sample", type=int, default=500)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument(
        "--backends",
        default=",".join(BACKENDS),
        help="Lista separada por comas (default: todos)",
    )
    parser.add_argument("--csv", type=Path, help="Guardar resultados tabulares")
    args = parser.parse_args()
    backends = tuple(x.strip() for x in args.backends.split(",") if x.strip())
    unknown = sorted(set(backends) - set(BACKENDS))
    if unknown:
        parser.error(f"backends desconocidos: {', '.join(unknown)}")

    lines: list[str] = []
    with args.input.open("r", encoding="utf-8", errors="replace") as f:
        for i, ln in enumerate(f):
            lines.append(ln)
            if i >= 49999:
                break

    random.seed(args.seed)
    idx = random.sample(range(len(lines)), min(args.sample, len(lines)))

    print(f"input={args.input}  lineas_leidas={len(lines)}  muestra={len(idx)}  nivel={args.nivel}", flush=True)
    print()
    print(
        f"{'backend':<22} {'ms/rev':>8} {'bytes%':>7} {'wiki%':>7} "
        f"{'html%':>7} {'url%':>6} {'empty%':>7} {'tokens':>9} {'vocab':>8} {'J regex':>8}"
    )
    print("-" * 102)

    ref_tokens: list[set[str]] | None = None
    ref_backend = "regex"
    rows: list[dict[str, str | int | float]] = []
    raw_bytes = sum(len(lines[i].encode("utf-8")) for i in idx) or 1

    for backend in backends:
        t0 = time.perf_counter()
        cleaned: list[str] = []
        n_fail = 0
        try:
            from limpiar_corpus_wiki import check_backend_deps

            check_backend_deps(backend)  # type: ignore[arg-type]
        except SystemExit as exc:
            print(f"{backend:<22} SKIP dep: {exc}")
            continue
        for i in idx:
            try:
                cleaned.append(clean_line(lines[i], args.nivel, backend))  # type: ignore[arg-type]
            except Exception as exc:
                n_fail += 1
                if n_fail <= 3:
                    print(f"[WARN] {backend} fallo en linea {i}: {type(exc).__name__}: {exc}", file=sys.stderr)
                cleaned.append("")
        elapsed_ms = 1000.0 * (time.perf_counter() - t0) / len(idx)
        if n_fail:
            print(f"[WARN] {backend}: {n_fail}/{len(idx)} revisiones fallaron", file=sys.stderr)

        n = len(cleaned)
        torsen_pct = 100.0 * sum(1 for c in cleaned if c and TORSEN_OK.match(c)) / n
        wiki_pct = 100.0 * sum(1 for c in cleaned if WIKI_MARKUP.search(c)) / n
        html_pct = 100.0 * sum(1 for c in cleaned if HTML_MARKUP.search(c)) / n
        url_pct = 100.0 * sum(1 for c in cleaned if URL_MARKUP.search(c)) / n
        empty_pct = 100.0 * sum(1 for c in cleaned if not c.strip()) / n
        token_lists = [[t.lower() for t in TOKEN.findall(c)] for c in cleaned]
        toks = [set(ts) for ts in token_lists]
        token_count = sum(map(len, token_lists))
        vocab = len(set().union(*toks))
        output_bytes = sum(len(c.encode("utf-8")) + 1 for c in cleaned)
        bytes_pct = 100.0 * output_bytes / raw_bytes

        j_vs_ref = ""
        if backend == ref_backend:
            ref_tokens = toks
            j_vs_ref = "1.000"
        elif ref_tokens is not None:
            j_mean = sum(jaccard(a, b) for a, b in zip(toks, ref_tokens)) / n
            j_vs_ref = f"{j_mean:.3f}"

        print(f"{backend:<22} {elapsed_ms:8.1f} {bytes_pct:6.1f}% {wiki_pct:6.1f}% "
              f"{html_pct:6.1f}% {url_pct:5.1f}% {empty_pct:6.1f}% "
              f"{token_count:9d} {vocab:8d} {j_vs_ref:>8}")
        rows.append(
            {
                "backend": backend,
                "ms_per_revision": round(elapsed_ms, 4),
                "output_bytes_pct": round(bytes_pct, 4),
                "wiki_residual_pct": round(wiki_pct, 4),
                "html_residual_pct": round(html_pct, 4),
                "url_residual_pct": round(url_pct, 4),
                "empty_pct": round(empty_pct, 4),
                "torsen_pct": round(torsen_pct, 4),
                "tokens": token_count,
                "sample_vocab": vocab,
                "jaccard_vs_regex": j_vs_ref,
            }
        )

    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        print(f"\nCSV: {args.csv}")

    print()
    print("Notas:")
    print("  - wiki/html/url% miden residuos específicos; no cuentan puntuación normal.")
    print("  - Menos bytes/tokens no implica menor BPI: los finalistas deben pasar ZDD+optimize.")
    print("  - pandoc invoca un proceso por revisión: referencia, no candidato de producción.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
