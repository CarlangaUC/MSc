#!/usr/bin/env python3
"""PNG: flujo de test ST[OP] dentro de BGPs/ (bgps-temporal-graphs)."""

from __future__ import annotations

import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
from pathlib import Path

OUT = Path(__file__).resolve().parent / "flujo-st-op-bgps.png"

C_REPO = "#E8EAF6"
C_CPP = "#E8F5E9"
C_HDR = "#C8E6C9"
C_PY = "#FFF3E0"
C_IN = "#E3F2FD"
C_OUT = "#FFFDE7"
C_WHY = "#FFEBEE"
C_ORIG = "#ECEFF1"
C_BORDER = "#37474F"
C_ARROW = "#455A64"
C_MAG = "#BBDEFB"


def box(ax, x, y, w, h, text, fc, fs=8, bold=False, ec=C_BORDER):
    p = FancyBboxPatch(
        (x, y), w, h,
        boxstyle="round,pad=0.02,rounding_size=0.06",
        linewidth=1.1, edgecolor=ec, facecolor=fc,
    )
    ax.add_patch(p)
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center",
            fontsize=fs, color="#212121", weight="bold" if bold else "normal")


def arrow(ax, x1, y1, x2, y2, label="", color=C_ARROW, dashed=False):
    ls = (0, (4, 3)) if dashed else "solid"
    ax.annotate(
        "", xy=(x2, y2), xytext=(x1, y1),
        arrowprops=dict(arrowstyle="-|>", color=color, lw=1.2, linestyle=ls,
                        connectionstyle="arc3,rad=0.0"),
    )
    if label:
        ax.text((x1 + x2) / 2, (y1 + y2) / 2 + 0.12, label,
                ha="center", va="bottom", fontsize=7, color="#546E7A", style="italic")


def lane(ax, x, y, w, h, title, color):
    p = FancyBboxPatch(
        (x, y), w, h,
        boxstyle="round,pad=0.01,rounding_size=0.04",
        linewidth=1.0, edgecolor="#78909C", facecolor=color, alpha=0.4,
    )
    ax.add_patch(p)
    ax.text(x + 0.12, y + h - 0.22, title, ha="left", va="top",
            fontsize=9, weight="bold", color="#263238")


def main() -> None:
    fig, ax = plt.subplots(figsize=(17, 12))
    ax.set_xlim(0, 17)
    ax.set_ylim(0, 12)
    ax.axis("off")
    ax.set_title(
        "BGPs — flujo de test ST[OP] versionado (bgps-temporal-graphs + scripts BGPs/)",
        fontsize=14, weight="bold", pad=14,
    )

    # Root folder frame
    root = FancyBboxPatch(
        (0.25, 0.35), 16.5, 11.2,
        boxstyle="round,pad=0.02,rounding_size=0.08",
        linewidth=2, edgecolor="#3949AB", facecolor=C_REPO, alpha=0.15,
    )
    ax.add_patch(root)
    ax.text(0.45, 11.35, "/root/MAGISTER/BGPs/", fontsize=10, weight="bold", color="#3949AB")

    # External input (outside BGPs but feeds it)
    lane(ax, 0.4, 9.5, 16.2, 1.55, "Input externo (MAGISTER → entra a BGPs)", C_MAG)
    box(ax, 0.6, 9.65, 3.2, 1.15,
        "wiki_*_uihrdc_packed64.docs\n(fuera de BGPs,\nresultados_test/)", C_IN, fs=8)
    box(ax, 4.1, 9.85, 4.0, 0.75,
        "packed64: master|rel\nterm_id = índice de lista", C_IN, fs=8)
    box(ax, 8.4, 9.65, 4.5, 1.15,
        "Por qué este input en BGPs:\n• Reutiliza temporal_wm (CLTJ)\n"
        "• ST[OP] con S=term, T=rel, O=master\n• Sin 18 tries ni .dat SPOT", C_WHY, fs=7.5)
    box(ax, 13.2, 9.65, 3.2, 1.15,
        "Toy: make_toy_\nversioned_docs.py\n(BGPs/)", C_PY, fs=7.5)

    # bgps-temporal-graphs repo
    lane(ax, 0.4, 4.85, 16.2, 4.4, "bgps-temporal-graphs/  —  repo CLTJ (adaptación MAGISTER)", C_CPP)

    # CMake / build
    box(ax, 0.6, 7.9, 2.0, 0.7, "CMakeLists.txt\nbuild-versioned-op", C_HDR, fs=7.5, bold=True)

    # build-versioned-op.cpp pipeline
    box(ax, 0.6, 6.5, 2.4, 1.15, "src/\nbuild-versioned-op.cpp\n(CLI)", C_CPP, fs=8, bold=True, ec="#2E7D32")
    box(ax, 3.3, 6.65, 2.0, 0.85, "inspect_docs()\nread_posting_list()", C_CPP, fs=7)
    box(ax, 5.6, 6.65, 2.1, 0.85, "append_term()\npor term_id", C_CPP, fs=7)
    box(ax, 8.0, 6.65, 2.0, 0.85, "validate()\nexpected_at()", C_CPP, fs=7)
    box(ax, 10.3, 6.65, 2.0, 0.85, "sdsl::store\n→ .opmt", C_OUT, fs=7, bold=True)

    # Headers
    box(ax, 3.3, 5.35, 3.5, 1.0,
        "include/\nversioned_op_metatrie.hpp\nterm_index, RLE, values_at()", C_HDR, fs=7.5, bold=True)
    box(ax, 7.2, 5.35, 3.2, 1.0,
        "include/\ncltj_temporal_wm.hpp\nVBT B/E, leap()", C_HDR, fs=7.5, bold=True)
    box(ax, 10.7, 5.35, 2.8, 1.0,
        "n=1: T=rel\npayload=master\n(is_partial=false)", "#FFCDD2", fs=7.5)

    # n=2 note
    box(ax, 13.7, 5.35, 2.6, 1.0,
        "n=2 control\n(descartado)\n~137 bpi", "#FFCDD2", fs=7, ec="#C62828")

    # Internal data flow annotation
    ax.text(6.0, 5.05,
            "append_term: unpack (master,rel) → RLE → eventos insert/delete → term_index::build() → temporal_wm",
            ha="center", fontsize=7, color="#33691E", style="italic")

    # Python scripts in BGPs/
    lane(ax, 0.4, 2.55, 16.2, 2.05, "Scripts y resultados en BGPs/ (no en bgps-temporal-graphs/)", C_PY)
    box(ax, 0.6, 2.75, 2.5, 0.85, "compare_input_\nformats.py", C_PY, fs=7)
    box(ax, 3.4, 2.75, 2.5, 0.85, "inspect_docs_para_\nmetatrie.py", C_PY, fs=7)
    box(ax, 6.2, 2.75, 2.5, 0.85, "docs_to_spot_dat.py\n(→ pipeline original)", C_PY, fs=7)
    box(ax, 9.0, 2.75, 2.5, 0.85, "run_st_op_n_tuple.sh\nrun_input_format_*.sh", C_PY, fs=7)
    box(ax, 11.8, 2.75, 2.4, 0.85, "resultados_opmt/\n*.opmt", C_OUT, fs=8, bold=True)
    box(ax, 14.4, 2.75, 2.0, 0.85, "resultados_*.csv\nBPI, equivalencia", C_OUT, fs=7)

    # Original CLTJ path (same repo, dashed)
    lane(ax, 0.4, 0.55, 7.5, 1.75, "Pipeline CLTJ original (mismo repo, no usado en test MAGISTER)", C_ORIG)
    box(ax, 0.6, 0.75, 2.0, 0.85, "build-index\nsrc/build-index.cpp", C_ORIG, fs=7, ec="#78909C")
    box(ax, 2.9, 0.75, 2.2, 0.85, "cltj_index_\ntemporal_metatrie\n(18 tries)", C_ORIG, fs=7, ec="#78909C")
    box(ax, 5.3, 0.75, 2.3, 0.85, ".dat SPOT\n→ .cltj", C_ORIG, fs=7, ec="#78909C")

    # Arrows: external docs to CLI
    arrow(ax, 2.2, 9.65, 1.8, 7.65, label=".docs", color="#1565C0")
    arrow(ax, 2.8, 7.15, 3.3, 7.15)
    arrow(ax, 5.3, 7.15, 5.6, 7.15)
    arrow(ax, 7.7, 7.15, 8.0, 7.15)
    arrow(ax, 10.0, 7.15, 10.3, 7.15)

    # append_term to headers
    arrow(ax, 6.65, 6.65, 5.05, 6.35, color="#388E3C")
    arrow(ax, 5.05, 6.35, 8.8, 6.35, color="#388E3C")

    # opmt to results
    arrow(ax, 11.3, 6.65, 12.9, 3.6, label="serialize", color="#F9A825")
    arrow(ax, 11.3, 6.65, 1.85, 3.6, color="#7B1FA2")
    arrow(ax, 1.85, 3.6, 1.85, 3.6)  # placeholder
    ax.annotate("", xy=(1.85, 3.6), xytext=(11.3, 6.65),
                arrowprops=dict(arrowstyle="-|>", color="#7B1FA2", lw=1.0,
                                connectionstyle="arc3,rad=0.25"))

    # scripts validate
    arrow(ax, 2.0, 3.6, 2.0, 6.5, label="equivalencia\nRLE", color="#EF6C00", dashed=True)

    # docs_to_spot -> original
    arrow(ax, 7.45, 2.75, 6.45, 1.6, label="alt.", color="#78909C", dashed=True)

    # Query semantics box
    box(ax, 13.2, 6.5, 3.2, 1.15,
        "Consulta BGPs:\n(term, rel) → masters\nvalues_at() + leap()", C_OUT, fs=7.5, bold=True)
    arrow(ax, 11.3, 7.0, 13.2, 7.0, color="#F9A825")

    # Results footer
    ax.text(8.5, 0.35,
            "Test n=1: wiki_100mb bpi≈2.5 | wiki_2gb bpi≈4.2  ·  Validación 9774/9774 PASS (100MB)",
            ha="center", fontsize=8, color="#1B5E20", weight="bold",
            bbox=dict(boxstyle="round", fc="#C8E6C9", ec="#66BB6A"))

    fig.tight_layout()
    fig.savefig(OUT, dpi=180, bbox_inches="tight", facecolor="white")
    print(f"Wrote {OUT}")


if __name__ == "__main__":
    main()
