#!/usr/bin/env python3
"""
Valida consultas de terminos leyendo directamente `listas_*` (export uiHRDC),
sin usar `test_tdzdd_piso1.cpp`.

Soporta:
- Consulta por palabra (requiere .voc para mapear palabra -> term_id)
- Consulta por term_id
- Chequeo de consistencia entre ambas rutas

Ejemplo:
  python3 scripts/validación_no_versionado_zdd.py \
    --listas uiHRDC/uiHRDC/data/texts/listas_wiki_100mb_versionada \
    --voc uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc \
    --word Abraham --term-id 1729
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path


LINE_RE = re.compile(r"^\s*T\[(\d+)\]\s*:\s*(.*)$")
PAIR_RE = re.compile(r"\((\d+)\s*,\s*(\d+)\)")
INT_RE = re.compile(r"\d+")


def bitread32(data: list[int], pos: int, length: int) -> int:
    idx = pos // 32
    shift = pos % 32
    value = data[idx] >> shift
    if shift + length > 32:
        value |= data[idx + 1] << (32 - shift)
    if length < 32:
        value &= (1 << length) - 1
    return value


def load_vocabulary(voc_path: Path) -> list[str]:
    with voc_path.open("rb") as f:
        header = f.read(12)
        if len(header) != 12:
            raise ValueError(f"VOC invalido (header incompleto): {voc_path}")
        nwords, elem_size, zone_size = struct.unpack("<III", header)
        if nwords == 0 or elem_size == 0 or elem_size > 32:
            raise ValueError(
                f"VOC invalido: nwords={nwords}, elem_size={elem_size} (path={voc_path})"
            )

        zone = f.read(zone_size)
        if len(zone) != zone_size:
            raise ValueError(f"VOC invalido (zona incompleta): {voc_path}")

        n_offsets = nwords + 1
        n_packed = (n_offsets * elem_size + 31) // 32
        raw = f.read(n_packed * 4)
        if len(raw) != n_packed * 4:
            raise ValueError(f"VOC invalido (offsets incompletos): {voc_path}")
        packed = list(struct.unpack(f"<{n_packed}I", raw))
        packed.append(0)  # guardia para lecturas cruzando palabra de 32 bits

    words: list[str] = []
    for i in range(nwords):
        off = bitread32(packed, i * elem_size, elem_size)
        nxt = bitread32(packed, (i + 1) * elem_size, elem_size)
        if nxt < off or nxt > zone_size:
            raise ValueError(f"Offsets invalidos en VOC: i={i}, off={off}, nxt={nxt}")
        words.append(zone[off:nxt].decode("utf-8", errors="ignore"))
    return words


def masters_for_term_id(listas_path: Path, term_id: int) -> list[int]:
    with listas_path.open("r", encoding="utf-8", errors="ignore") as f:
        for raw in f:
            m = LINE_RE.match(raw.rstrip("\n"))
            if not m:
                continue
            tid = int(m.group(1))
            if tid != term_id:
                continue

            payload = m.group(2)
            pairs = PAIR_RE.findall(payload)
            if pairs:
                masters = sorted({int(master) for master, _ in pairs})
                return masters

            # fallback no-versionado: lista de enteros
            ints = [int(tok) for tok in INT_RE.findall(payload)]
            return sorted(set(ints))

    raise KeyError(f"term_id {term_id} no encontrado en {listas_path}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Valida consultas palabra/term_id contra listas_* exportadas por uiHRDC."
    )
    parser.add_argument(
        "--listas",
        required=True,
        help="Ruta a listas_* (ej: uiHRDC/uiHRDC/data/texts/listas_wiki_100mb_versionada)",
    )
    parser.add_argument(
        "--voc",
        default="",
        help="Ruta a .voc para consulta por palabra (opcional si solo usas --term-id)",
    )
    parser.add_argument("--word", default="", help="Palabra a consultar (exact match en VOC)")
    parser.add_argument("--term-id", type=int, default=-1, help="term_id a consultar")
    parser.add_argument(
        "--show-limit",
        type=int,
        default=50,
        help="Maximo de masters a mostrar (default: 50)",
    )
    args = parser.parse_args()

    listas_path = Path(args.listas)
    if not listas_path.exists():
        print(f"[ERR] No existe listas: {listas_path}")
        return 1

    words: list[str] = []
    word2id: dict[str, int] = {}

    if args.word:
        if not args.voc:
            print("[ERR] --word requiere --voc")
            return 1
        voc_path = Path(args.voc)
        if not voc_path.exists():
            print(f"[ERR] No existe voc: {voc_path}")
            return 1
        words = load_vocabulary(voc_path)
        word2id = {w: i for i, w in enumerate(words)}
        print(f"[OK] VOC cargado: nwords={len(words)}")

    results: dict[str, tuple[int, list[int]]] = {}

    if args.word:
        if args.word not in word2id:
            print(f'[INFO] palabra "{args.word}" no encontrada en VOC')
        else:
            tid = word2id[args.word]
            masters = masters_for_term_id(listas_path, tid)
            results["word"] = (tid, masters)

    if args.term_id >= 0:
        tid = args.term_id
        masters = masters_for_term_id(listas_path, tid)
        results["term_id"] = (tid, masters)

    if not results:
        print("[ERR] No hay consulta para ejecutar. Usa --word y/o --term-id.")
        return 1

    for mode, (tid, masters) in results.items():
        print(f"\n=== CONSULTA VIA {mode.upper()} ===")
        print(f"term_id = {tid}")
        if words and 0 <= tid < len(words):
            print(f'palabra = "{words[tid]}"')
        shown = masters[: args.show_limit]
        suffix = "" if len(masters) <= args.show_limit else " ..."
        print(f"masters (|S_t|={len(masters)}): {{ {', '.join(map(str, shown))}{suffix} }}")

    if "word" in results and "term_id" in results:
        w_tid, w_set = results["word"]
        t_tid, t_set = results["term_id"]
        ok = (w_tid == t_tid) and (w_set == t_set)
        print("\n=== CONSISTENCIA WORD vs TERM_ID ===")
        print(f"mismo term_id: {'SI' if w_tid == t_tid else 'NO'}")
        print(f"mismo S_t: {'SI' if w_set == t_set else 'NO'}")
        print(f"resultado: {'OK' if ok else 'MISMATCH'}")
        return 0 if ok else 2

    return 0


if __name__ == "__main__":
    sys.exit(main())
