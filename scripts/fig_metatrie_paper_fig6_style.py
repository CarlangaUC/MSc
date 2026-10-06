#!/usr/bin/env python3
"""
Figura estilo Arroyuelo et al. Fig. 6 — micro_metatrie, término 0.
"""
from __future__ import annotations

from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import ConnectionPatch, FancyBboxPatch

OUT = Path(__file__).resolve().parents[1] / "resultados_test" / "metatrie_paper_fig6_style.png"

INTERVALS = [
    ("[1, 3)", {1}),
    ("[3, 5)", {1, 2}),
    ("[5, 7)", {2, 3}),
    ("[7, 8)", {3}),
]

EDITS = [
    ("τ=1", "+", "u=1"),
    ("τ=3", "+", "u=2"),
    ("τ=5", "−", "u=1"),
    ("τ=5", "+", "u=3"),
]


def link(ax, x0, y0, half_h0, x1, y1, half_h1):
    """Arista recta borde inferior del padre → borde superior del hijo."""
    y_bot = y0 - half_h0
    y_top = y1 + half_h1
    con = ConnectionPatch(
        (x0, y_bot),
        (x1, y_top),
        coordsA="data",
        coordsB="data",
        axesA=ax,
        axesB=ax,
        color="#374151",
        lw=1.15,
        zorder=1,
        clip_on=False,
    )
    ax.add_patch(con)


def draw_box_node(ax, x, y, w, h, label, fc, ec):
    ax.add_patch(
        FancyBboxPatch(
            (x - w / 2, y - h / 2), w, h, boxstyle="round,pad=0.04", fc=fc, ec=ec, lw=1.2, zorder=2
        )
    )
    ax.text(x, y, label, ha="center", va="center", fontsize=9, zorder=3)
    return (w, h)


def draw_left(ax) -> None:
    ax.set_xlim(0, 10)
    ax.set_ylim(0, 10)
    ax.axis("off")
    ax.set_title("(a) Intervalos suelo (time-first), $t{=}0$", fontsize=11, pad=12)

    rx, ry = 5.0, 8.55
    _, rh = draw_box_node(ax, rx, ry, 1.4, 0.75, "$v$", "#dbeafe", "#1d4ed8")

    xs = [1.4, 3.4, 5.6, 7.6]
    for x, (label, masters) in zip(xs, INTERVALS):
        iw, ih = draw_box_node(ax, x, 5.95, 1.35, 0.85, label.replace(" ", "\n"), "#bfdbfe", "#2563eb")
        link(ax, rx, ry, rh / 2, x, 5.95, ih / 2)

        ms = sorted(masters)
        for j, u in enumerate(ms):
            ox = x + (j - (len(ms) - 1) / 2) * 0.62
            oy = 4.05
            ax.add_patch(mpatches.Ellipse((ox, oy), 0.92, 0.56, fc="#dcfce7", ec="#15803d", lw=1.2, zorder=2))
            ax.text(ox, oy, f"$u_{u}$", ha="center", va="center", fontsize=10, zorder=3)
            link(ax, x, 5.95, ih / 2, ox, oy, 0.28)

    ax.text(5, 1.75, "$u_i$ = doc.\\ $i$ activo en el intervalo", ha="center", fontsize=9, color="#475569")


def draw_middle(ax) -> None:
    ax.set_xlim(0, 10)
    ax.set_ylim(0, 10)
    ax.axis("off")
    ax.set_title("(b) BT sobre masters $u_i$ (payload del WM)", fontsize=11, pad=12)

    # Trie 2 niveles: 1→01, 2→10, 3→11
    nodes = {
        "r": (5.0, 8.6, 0.65, 0.48, "$\\cdot$", "#ffedd5", "#ea580c"),
        "0": (3.2, 6.85, 0.52, 0.42, "0", "#fff7ed", "#c2410c"),
        "1": (6.8, 6.85, 0.52, 0.42, "1", "#fff7ed", "#c2410c"),
        "00": (2.2, 5.15, 0.52, 0.42, "0", "#fff7ed", "#c2410c"),
        "01": (4.2, 5.15, 0.52, 0.42, "1", "#fff7ed", "#c2410c"),
        "11": (6.8, 5.15, 0.52, 0.42, "1", "#fff7ed", "#c2410c"),
    }
    for key, (x, y, w, h, lab, fc, ec) in nodes.items():
        draw_box_node(ax, x, y, w, h, lab, fc, ec)

    def hh(k):
        return nodes[k][3] / 2

    def xy(k):
        return nodes[k][0], nodes[k][1]

    link(ax, *xy("r"), hh("r"), *xy("0"), hh("0"))
    link(ax, *xy("r"), hh("r"), *xy("1"), hh("1"))
    link(ax, *xy("0"), hh("0"), *xy("00"), hh("00"))
    link(ax, *xy("0"), hh("0"), *xy("01"), hh("01"))
    link(ax, *xy("1"), hh("1"), *xy("11"), hh("11"))

    leaves = [("00", 1), ("01", 2), ("11", 3)]
    for key, u in leaves:
        x, y = xy(key)
        ly = 3.35
        ax.add_patch(mpatches.Ellipse((x, ly), 1.0, 0.58, fc="#dcfce7", ec="#15803d", lw=1.2, zorder=2))
        ax.text(x, ly, f"$u_{u}$", ha="center", va="center", fontsize=10, zorder=3)
        link(ax, x, y, hh(key), x, ly, 0.29)

    ax.text(5, 2.15, "Hojas = \\texttt{master} (\\texttt{temporal\\_wm})", ha="center", fontsize=9, color="#475569")
    ax.text(5, 1.35, "BT único $V$ — Fig.~6 (centro)", ha="center", fontsize=9, color="#64748b")


def draw_right(ax) -> None:
    ax.set_xlim(0, 10)
    ax.set_ylim(0, 10)
    ax.axis("off")
    ax.set_title("(c) Ins/del en el borde temporal", fontsize=11, pad=12)

    y_oval = 7.05
    ax.text(1.85, 8.05, "En $\\tau{=}4$", ha="center", fontsize=9.5, fontweight="bold")
    ax.text(8.15, 8.05, "En $\\tau{=}5$", ha="center", fontsize=9.5, fontweight="bold")

    for x, u in [(1.15, 1), (2.45, 2)]:
        ax.add_patch(mpatches.Ellipse((x, y_oval), 1.0, 0.58, fc="#dcfce7", ec="#15803d", lw=1.2))
        ax.text(x, y_oval, f"$u_{u}$", ha="center", va="center", fontsize=10)
    for x, u in [(7.55, 2), (8.85, 3)]:
        ax.add_patch(mpatches.Ellipse((x, y_oval), 1.0, 0.58, fc="#dcfce7", ec="#15803d", lw=1.2))
        ax.text(x, y_oval, f"$u_{u}$", ha="center", va="center", fontsize=10)

    ax.annotate(
        "",
        xy=(7.2, y_oval),
        xytext=(2.7, y_oval),
        arrowprops=dict(arrowstyle="-|>", lw=1.4, color="#64748b", shrinkA=8, shrinkB=8),
        zorder=0,
    )

    edit_box = FancyBboxPatch((3.6, 5.15), 2.8, 0.95, boxstyle="round,pad=0.06", fc="#fee2e2", ec="#dc2626", lw=1.2, zorder=1)
    ax.add_patch(edit_box)
    ax.text(5, 5.72, "borde $\\tau{=}5$", ha="center", va="center", fontsize=9.5, color="#334155")
    ax.text(5, 5.28, "$-\\,u_1$  $+$  $u_3$", ha="center", va="center", fontsize=11)

    ax.text(5, 4.35, "cierra $u_1$; abre $u_3$", ha="center", fontsize=8.5, color="#475569")

    ax.text(0.55, 3.55, "Stream (extracto):", fontsize=9, fontweight="bold", ha="left")
    y0 = 3.05
    dy = 0.82
    for k, (when, op, what) in enumerate(EDITS):
        yy = y0 - k * dy
        color = "#166534" if op == "+" else "#b91c1c"
        ax.text(0.6, yy, when, fontsize=8.5, family="monospace", ha="left", va="center")
        ax.text(2.05, yy, op, fontsize=11, color=color, fontweight="bold", ha="center", va="center")
        ax.text(2.45, yy, what, fontsize=9, ha="left", va="center")


def main() -> None:
    plt.rcParams["font.family"] = "DejaVu Sans"
    fig, axes = plt.subplots(1, 3, figsize=(15.5, 6.6), dpi=160)
    fig.suptitle(
        "Metatrie — \\texttt{micro\\_metatrie.docs}, término $t{=}0$ (análogo Fig.~6, Arroyuelo et al.)",
        fontsize=12.5,
        y=0.97,
    )
    draw_left(axes[0])
    draw_middle(axes[1])
    draw_right(axes[2])

    fig.text(
        0.5,
        0.03,
        "$u_i$ = \\texttt{master} (documento $i$); $\\tau$ = versión (\\texttt{rel}). "
        "Toy: solo docs 1–3; en wiki serían miles de masters.",
        ha="center",
        va="bottom",
        fontsize=9.5,
        color="#334155",
    )

    fig.subplots_adjust(left=0.03, right=0.99, top=0.86, bottom=0.11, wspace=0.32)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT, bbox_inches="tight", facecolor="white", pad_inches=0.12)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
