#!/usr/bin/env python3
"""Consolida logs BPI del benchmark formatos wiki_2gb."""
from __future__ import annotations

import csv
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "resultados_test"

ROWS = [
    ("sucio", "baseline", OUT / "bpi_wiki_2gb_sucio_baseline.log"),
    ("sucio", "optimized", OUT / "bpi_wiki_2gb_sucio_optimized.log"),
    ("marcado", "baseline", OUT / "bpi_wiki_2gb_marcado_baseline.log"),
    ("marcado", "optimized", OUT / "bpi_wiki_2gb_marcado_optimized.log"),
    ("torsen", "baseline", OUT / "bpi_wiki_2gb_torsen_baseline.log"),
    ("torsen", "optimized", OUT / "bpi_wiki_2gb_torsen_optimized.log"),
]

KEYS = [
    "terms_scanned", "n_raw", "n_snap_elems", "ratio_raw_over_stored",
    "edd_nodes", "bpi_edd", "bpi_edd_over_stored", "bpi_file",
    "numZddVars", "levels_used", "file_bytes",
]


def parse_log(path: Path) -> dict[str, str]:
    if not path.exists():
        return {}
    text = path.read_text()
    out = {}
    for k in KEYS:
        m = re.search(rf"^{k}=(.+)$", text, re.M)
        if m:
            out[k] = m.group(1).strip()
    return out


def main() -> int:
    csv_path = OUT / "benchmark_formatos_wiki_2gb.csv"
    records = []
    for fmt, stage, log in ROWS:
        d = parse_log(log)
        if not d:
            continue
        records.append({"formato": fmt, "stage": stage, **d})

    if not records:
        print("No hay logs todavía")
        return 1

    fields = ["formato", "stage"] + KEYS
    with csv_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in records:
            w.writerow({k: r.get(k, "") for k in fields})

    print(f"[OK] {csv_path} ({len(records)} filas)")

    # Tabla markdown
    md = OUT / "benchmark_formatos_wiki_2gb.md"
    lines = [
        "# Benchmark formatos wiki_2gb (u+t, nodes_desc+sift)",
        "",
        "| formato | stage | V | n_raw | edd_nodes | bpi_edd | bpi_edd_over_stored | bpi_file |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for r in records:
        lines.append(
            f"| {r['formato']} | {r['stage']} | {r.get('terms_scanned','')} | "
            f"{r.get('n_raw','')} | {r.get('edd_nodes','')} | {r.get('bpi_edd','')} | "
            f"{r.get('bpi_edd_over_stored','')} | {r.get('bpi_file','')} |"
        )

    # Deltas vs sucio baseline optimized pending
    base_sucio = next((r for r in records if r["formato"] == "sucio" and r["stage"] == "baseline"), None)
    if base_sucio and base_sucio.get("bpi_edd"):
        b0 = float(base_sucio["bpi_edd"])
        lines += ["", "## Δ bpi_edd vs sucio baseline", ""]
        for r in records:
            if r.get("bpi_edd"):
                b = float(r["bpi_edd"])
                lines.append(
                    f"- **{r['formato']} / {r['stage']}**: {b:.4f} ({100*(b-b0)/b0:+.1f} % vs sucio baseline)"
                )

    md.write_text("\n".join(lines) + "\n")
    print(f"[OK] {md}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
