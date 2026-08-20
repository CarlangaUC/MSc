#!/usr/bin/env python3
"""Gráfico bpi_edd por heurística desde CSV de optimize sweep."""
import argparse
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


def heur_category(h: str) -> str:
    if h == "baseline":
        return "baseline"
    if "+" in h:
        return "compuesta"
    if h.startswith("nodes_") or h.startswith("df_"):
        return "estatica"
    return "dinamica"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", help="optimize_sweep_*.csv o optimize_runs.csv filtrado")
    ap.add_argument("-o", "--out", default="", help="PNG de salida")
    args = ap.parse_args()

    path = Path(args.csv)
    if not path.is_file():
        print(f"ERROR: no existe {path}", file=sys.stderr)
        return 1

    df = pd.read_csv(path)
    if "heuristica" not in df.columns:
        print("ERROR: CSV sin columna heuristica", file=sys.stderr)
        return 1

    df = df[df["status"] == "ok"].copy()
    if df.empty:
        print("ERROR: sin filas ok", file=sys.stderr)
        return 1

    baseline = df.loc[df["heuristica"] == "baseline", "bpi_edd_after"]
    b0 = float(baseline.iloc[0]) if len(baseline) else float(df["bpi_edd_before"].iloc[0])
    df["delta_bpi_pct"] = 100.0 * (df["bpi_edd_after"] - b0) / b0
    df["categoria"] = df["heuristica"].map(heur_category)
    df = df.sort_values("bpi_edd_after")

    out = Path(args.out) if args.out else path.with_suffix(".png")

    colors = {"baseline": "#9e9e9e", "estatica": "#42a5f5", "dinamica": "#66bb6a", "compuesta": "#ffa726"}
    bar_colors = [colors.get(c, "#ab47bc") for c in df["categoria"]]

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 9), gridspec_kw={"height_ratios": [2, 1]})

    y_pos = range(len(df))
    ax1.barh(list(y_pos), df["bpi_edd_after"], color=bar_colors, edgecolor="white", height=0.7)
    ax1.axvline(b0, color="#e53935", ls="--", lw=1.5, label=f"baseline bpi_edd={b0:.3f}")
    ax1.set_yticks(list(y_pos))
    ax1.set_yticklabels(df["heuristica"])
    ax1.set_xlabel("bpi_edd (EDD en RAM, 32 B/nodo)")
    ax1.set_title(f"bpi_edd por heurística — {path.name}")
    ax1.legend(loc="lower right")
    ax1.invert_yaxis()

    ax2.barh(list(y_pos), df["delta_bpi_pct"], color=bar_colors, edgecolor="white", height=0.7)
    ax2.axvline(0, color="#424242", lw=1)
    ax2.set_yticks(list(y_pos))
    ax2.set_yticklabels(df["heuristica"])
    ax2.set_xlabel("Δ bpi_edd vs baseline (%)")
    ax2.invert_yaxis()

    from matplotlib.patches import Patch

    legend = [Patch(facecolor=colors[k], label=k) for k in colors]
    fig.legend(handles=legend, loc="upper center", ncol=4, bbox_to_anchor=(0.5, 0.02))

    for _, row in df.iterrows():
        idx = list(df["heuristica"]).index(row["heuristica"])
        ax1.text(row["bpi_edd_after"], idx, f"  {row['bpi_edd_after']:.2f}", va="center", fontsize=8)

    plt.tight_layout(rect=[0, 0.05, 1, 1])
    fig.savefig(out, dpi=150, bbox_inches="tight")
    print(f"Gráfico guardado: {out}")
    print(df[["heuristica", "categoria", "edd_after", "bpi_edd_after", "delta_bpi_pct", "seconds"]].to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
