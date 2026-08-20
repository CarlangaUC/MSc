#!/usr/bin/env python3
"""
Check visual y semántico de heurísticas CUDD (optimize post-build).

1. Ejecuta ./zdd_cudd_plus_t heuristics-check (toy u=4, V=2).
2. Mosaico de todas las heurísticas.
3. Panel empírico desde optimize_sweep (100 MB por defecto).
"""
from __future__ import annotations

import argparse
import glob
import os
import subprocess
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd
from matplotlib.patches import Patch
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "zdd_cudd_plus_t"
DEFAULT_OUT = ROOT / "resultados_test" / "heuristics_check"
RESULTADOS = ROOT / "resultados_test"

HEUR_ORDER = [
    "baseline",
    "nodes_desc",
    "df_desc",
    "nodes_asc",
    "df_asc",
    "nodes_desc+sift",
    "nodes_desc+sift_conv",
    "df_desc+sift_conv",
    "sift",
    "sift_conv",
    "symm_sift",
    "symm_sift_conv",
    "random",
    "random_pivot",
]


def sanitize_dir(heur: str) -> str:
    return heur.replace("+", "_plus_")


def run_heuristics_check(enc: str, out_dir: Path, skip_cpp: bool) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    if skip_cpp:
        print(f"[skip] heuristics-check (existe {out_dir / 'heuristics_check.csv'})")
        return 0
    if not BIN.is_file():
        print(f"ERROR: falta binario {BIN}", file=sys.stderr)
        return 1
    cmd = [str(BIN), "heuristics-check", enc, str(out_dir)]
    print("Ejecutando:", " ".join(cmd))
    return subprocess.run(cmd, cwd=ROOT, check=False).returncode


def load_toy_csv(out_dir: Path) -> pd.DataFrame:
    path = out_dir / "heuristics_check.csv"
    if not path.is_file():
        raise FileNotFoundError(path)
    df = pd.read_csv(path)
    rank = {h: i for i, h in enumerate(HEUR_ORDER)}
    df["_r"] = df["heuristica"].map(lambda h: rank.get(h, 999))
    return df.sort_values("_r").drop(columns=["_r"])


def find_best_sweep_csv(pack_key: str) -> Path | None:
    pattern = str(RESULTADOS / f"optimize_sweep_{pack_key}*.csv")
    best_path, best_n, best_ok = None, -1, -1
    complete = RESULTADOS / f"optimize_sweep_{pack_key}_COMPLETE.csv"
    candidates = [str(complete)] if complete.is_file() else []
    candidates += sorted(glob.glob(pattern), key=os.path.getmtime, reverse=True)
    seen = set()
    for path in candidates:
        if path in seen:
            continue
        seen.add(path)
        try:
            df = pd.read_csv(path)
        except Exception:
            continue
        n = len(df)
        n_ok = int((df["status"] == "ok").sum()) if "status" in df.columns else n
        if n > best_n or (n == best_n and n_ok > best_ok):
            best_n, best_ok, best_path = n, n_ok, Path(path)
    return best_path


def heur_category(h: str) -> str:
    if h == "baseline":
        return "baseline"
    if "+" in h:
        return "compuesta"
    if h.startswith("nodes_") or h.startswith("df_"):
        return "estatica"
    return "dinamica"


def build_mosaic(out_dir: Path, toy_df: pd.DataFrame) -> Path:
    cols, rows = 4, 4
    cell_w, cell_h = 420, 320
    mosaic = Image.new("RGB", (cols * cell_w, rows * cell_h), (250, 250, 250))
    draw = ImageDraw.Draw(mosaic)
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 14)
        font_sm = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 11)
    except OSError:
        font = ImageFont.load_default()
        font_sm = font

    for idx, row in enumerate(toy_df.itertuples()):
        if idx >= cols * rows:
            break
        heur = row.heuristica
        png = out_dir / sanitize_dir(heur) / "bosque.png"
        c, r = idx % cols, idx // cols
        x0, y0 = c * cell_w, r * cell_h

        header_h = 52
        draw.rectangle([x0, y0, x0 + cell_w - 2, y0 + header_h], fill=(240, 240, 240))
        sem = int(getattr(row, "semantics_ok", 0)) and int(getattr(row, "families_equal", 0))
        badge = "OK sem" if sem else "FAIL"
        badge_color = (46, 125, 50) if sem else (198, 40, 40)
        draw.text((x0 + 8, y0 + 6), heur, fill=(33, 33, 33), font=font)
        draw.text(
            (x0 + 8, y0 + 26),
            f"{badge}  Δ={row.delta_pct:+.1f}%  edd {row.edd_before}→{row.edd_after}",
            fill=badge_color,
            font=font_sm,
        )

        if png.is_file():
            img = Image.open(png).convert("RGB")
            img.thumbnail((cell_w - 12, cell_h - header_h - 12), Image.Resampling.LANCZOS)
            px = x0 + (cell_w - img.width) // 2
            py = y0 + header_h + (cell_h - header_h - img.height) // 2
            mosaic.paste(img, (px, py))
        else:
            draw.text((x0 + 20, y0 + 80), "(sin PNG)", fill=(120, 120, 120), font=font_sm)

    out_path = out_dir / "mosaico_heuristicas.png"
    mosaic.save(out_path, dpi=(140, 140))
    print(f"[mosaico] {out_path}")
    return out_path


def plot_empirical_panel(sweep_path: Path, out_dir: Path) -> Path | None:
    df = pd.read_csv(sweep_path)
    if "heuristica" not in df.columns:
        return None

    rank = {h: i for i, h in enumerate(HEUR_ORDER)}
    df["_r"] = df["heuristica"].map(lambda h: rank.get(h, 999))
    df = df.sort_values("_r").drop(columns=["_r"])
    df["categoria"] = df["heuristica"].map(heur_category)

    baseline = df.loc[df["heuristica"] == "baseline", "bpi_edd_after"]
    b0 = float(baseline.iloc[0]) if len(baseline) else float(df["bpi_edd_before"].iloc[0])

    ok_mask = df["status"] == "ok"
    colors = {
        "baseline": "#9e9e9e",
        "estatica": "#42a5f5",
        "dinamica": "#66bb6a",
        "compuesta": "#ffa726",
    }

    fig, axes = plt.subplots(1, 3, figsize=(16, 7), gridspec_kw={"width_ratios": [2, 1.2, 1]})

    # bpi_edd
    ax = axes[0]
    y = range(len(df))
    bar_colors = [
        "#bdbdbd" if not ok else colors.get(c, "#ab47bc")
        for ok, c in zip(ok_mask, df["categoria"])
    ]
    vals = df["bpi_edd_after"].where(ok_mask, b0)
    ax.barh(list(y), vals, color=bar_colors, edgecolor="white", height=0.65)
    ax.axvline(b0, color="#e53935", ls="--", lw=1.2, label=f"baseline {b0:.3f}")
    ax.set_yticks(list(y))
    ax.set_yticklabels(df["heuristica"], fontsize=8)
    ax.set_xlabel("bpi_edd")
    ax.set_title(f"Empírico — {sweep_path.name}")
    ax.invert_yaxis()
    ax.legend(loc="lower right", fontsize=8)

    # semantics_ok + status
    ax = axes[1]
    sem = df.get("semantics_ok", pd.Series([1] * len(df)))
    sem_vals = sem.fillna(0).astype(int)
    ax.barh(list(y), sem_vals, color=["#66bb6a" if s else "#ef5350" for s in sem_vals], height=0.5)
    ax.set_xlim(0, 1.2)
    ax.set_xticks([0, 1])
    ax.set_xticklabels(["0", "1"])
    ax.set_xlabel("semantics_ok")
    ax.set_yticks(list(y))
    ax.set_yticklabels(df["status"], fontsize=8)
    ax.set_title("Verificación semántica (CSV sweep)")
    ax.invert_yaxis()

    # tiempo
    ax = axes[2]
    secs = df["seconds"].fillna(0)
    ax.barh(list(y), secs, color=bar_colors, edgecolor="white", height=0.5)
    ax.set_xlabel("tiempo (s)")
    ax.set_yticks(list(y))
    ax.set_yticklabels([""] * len(df))
    ax.set_title("Tiempo por heurística")
    ax.invert_yaxis()

    legend = [Patch(facecolor=colors[k], label=k) for k in colors]
    fig.legend(handles=legend, loc="lower center", ncol=4, bbox_to_anchor=(0.5, -0.02))
    fig.tight_layout()
    out = out_dir / "panel_empirico_100mb.png"
    fig.savefig(out, dpi=140, bbox_inches="tight")
    plt.close(fig)
    print(f"[panel] {out}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Check visual/semántico heurísticas optimize")
    ap.add_argument("--enc", default="u+t", choices=["u+t", "log"], help="Codificación tag")
    ap.add_argument(
        "-o",
        "--out",
        default=str(DEFAULT_OUT),
        help="Directorio de salida",
    )
    ap.add_argument(
        "--sweep-csv",
        default="",
        help="CSV optimize_sweep empírico (default: wiki_100mb_plus_t)",
    )
    ap.add_argument(
        "--skip-cpp",
        action="store_true",
        help="No re-ejecutar heuristics-check si ya hay CSV",
    )
    args = ap.parse_args()

    out_dir = Path(args.out)
    csv_exists = (out_dir / "heuristics_check.csv").is_file()
    if args.skip_cpp and csv_exists:
        rc = 0
    else:
        rc = run_heuristics_check(args.enc, out_dir, skip_cpp=False)
        if rc not in (0, 2):
            return rc

    toy_df = load_toy_csv(out_dir)
    build_mosaic(out_dir, toy_df)

    sweep_path = Path(args.sweep_csv) if args.sweep_csv else find_best_sweep_csv("wiki_100mb_plus_t")
    if sweep_path and sweep_path.is_file():
        plot_empirical_panel(sweep_path, out_dir)
    else:
        print("[aviso] sin CSV empírico 100 MB; panel omitido")

    return 0


if __name__ == "__main__":
    sys.exit(main())
