#!/usr/bin/env python3
"""Consolida CSV de sweep_backends → ranking + reporte markdown."""
from __future__ import annotations

import argparse
import csv
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", type=Path)
    ap.add_argument("--baseline", default="regex", help="Backend referencia")
    ap.add_argument("--out", type=Path, help="Markdown de salida")
    args = ap.parse_args()

    rows = list(csv.DictReader(args.csv.open(encoding="utf-8")))
    opt = [r for r in rows if r.get("stage") == "optimized" and r.get("bpi_edd")]
    if not opt:
        print("Sin filas optimized con bpi_edd")
        return 1

    def fval(r: dict, k: str) -> float:
        try:
            return float(r[k])
        except (KeyError, ValueError, TypeError):
            return float("nan")

    opt.sort(key=lambda r: fval(r, "bpi_edd"))
    ref = next((r for r in opt if r["backend"] == args.baseline), None)
    ref_bpi = fval(ref, "bpi_edd") if ref else float("nan")

    lines = [
        f"# Sweep backends — {args.csv.name}",
        "",
        f"Referencia **{args.baseline}** optimized: **{ref_bpi:.6f}** bpi_edd",
        "",
        "| rank | backend | bpi_edd | V | n_raw | edd_nodes | Δ vs ref |",
        "|---:|---|---:|---:|---:|---:|---:|",
    ]
    for i, r in enumerate(opt, 1):
        bpi = fval(r, "bpi_edd")
        delta = "" if ref_bpi != ref_bpi else f"{100.0 * (bpi - ref_bpi) / ref_bpi:+.2f}%"
        lines.append(
            f"| {i} | {r['backend']} | {bpi:.6f} | {r.get('V','')} | "
            f"{r.get('n_raw','')} | {r.get('edd_nodes','')} | {delta} |"
        )

    md = "\n".join(lines) + "\n"
    out = args.out or args.csv.with_suffix(".md")
    out.write_text(md, encoding="utf-8")
    print(md)
    print(f"[OK] -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
