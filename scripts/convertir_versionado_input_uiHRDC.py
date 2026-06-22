#!/usr/bin/env python3
"""
Convierte exportaciones uiHRDC tipo `listas_*` a binario estilo PISA (.docs/.bin)
compatible con `test.cpp` (modo 2).

Entrada esperada por linea:
  - No versionado: T[42]: 10 11 12 13
  - Versionado (preferido, uiHRDC export): T[42]: 526909256 526909257 ...  (uint64 packed 40/24)
  - Versionado (legacy):                 T[42]: (13,1558) (13,1559) ...

Salida binaria (motor 64 bits, split por defecto 40/24):
  [uint32 total_listas]
  para cada lista i en [0..total_listas):
      [uint32 largo]
      [uint64 doc_0]...[uint64 doc_(largo-1)]
"""

import re
import struct
import os
import argparse
from array import array
from pathlib import Path


LINE_RE = re.compile(r"^\s*T\[(\d+)\]\s*:\s*(.*)$")
PAIR_RE = re.compile(r"\((\d+)\s*,\s*(\d+)\)")
INT_RE = re.compile(r"\d+")


def load_page_mapping(path: Path) -> list[int]:
    data = path.read_bytes()
    if len(data) % 4 != 0:
        raise ValueError(f"page_mapping invalido (bytes no multiplo de 4): {path}")
    total = len(data) // 4
    return list(struct.unpack(f"<{total}I", data))


def pack_pair(master: int, rel: int, master_bits: int, rel_bits: int) -> int:
    master_mask = (1 << master_bits) - 1
    rel_mask = (1 << rel_bits) - 1
    return ((master & master_mask) << rel_bits) | (rel & rel_mask)


def parse_line(line: str):
    m = LINE_RE.match(line)
    if not m:
        return None
    term_id = int(m.group(1))
    payload = m.group(2).strip()
    return term_id, payload


def count_lists(input_path: Path) -> tuple[int, bool]:
    max_term = -1
    has_pairs = False

    with input_path.open("r", encoding="utf-8", errors="ignore") as fin:
        for raw in fin:
            parsed = parse_line(raw)
            if parsed is None:
                continue
            term_id, payload = parsed
            if term_id > max_term:
                max_term = term_id
            if not has_pairs and "(" in payload and ")" in payload:
                has_pairs = True

    if max_term < 0:
        raise ValueError(f"No se encontraron lineas T[x]: en {input_path}")
    return max_term + 1, has_pairs


def tuple_to_docid(
    master: int,
    rel: int,
    tuple_output: str,
    mapping: list[int] | None,
    master_bits: int,
    rel_bits: int,
) -> int:
    if tuple_output == "global":
        if mapping is None:
            raise ValueError("tuple_output=global requiere --page-map")
        if master >= len(mapping):
            raise ValueError(
                f"master {master} fuera de rango para page_map (len={len(mapping)})"
            )
        return mapping[master] + rel

    if tuple_output == "packed":
        return pack_pair(master, rel, master_bits, rel_bits)

    # auto
    if mapping is not None and master < len(mapping):
        return mapping[master] + rel
    return pack_pair(master, rel, master_bits, rel_bits)


def convert(
    input_path: Path,
    output_bin: Path,
    output_txt: Path | None,
    page_map_path: Path | None,
    tuple_output: str,
    master_bits: int,
    rel_bits: int,
):
    if master_bits + rel_bits != 64:
        raise ValueError("master_bits + rel_bits debe ser 64")

    total_lists, has_pairs = count_lists(input_path)
    print(f"[INFO] total_listas={total_lists}")
    print(f"[INFO] detectado_versionado={has_pairs}")

    mapping = None
    if page_map_path is not None:
        mapping = load_page_mapping(page_map_path)
        print(f"[INFO] page_map cargado: {len(mapping)} entradas")

    output_bin.parent.mkdir(parents=True, exist_ok=True)
    if output_txt is not None:
        output_txt.parent.mkdir(parents=True, exist_ok=True)

    expected_term = 0
    tuples_seen = 0
    ints_seen = 0
    empty_lists = 0

    fout_txt = None
    if output_txt is not None:
        fout_txt = output_txt.open("w", encoding="utf-8")

    with input_path.open("r", encoding="utf-8", errors="ignore") as fin, output_bin.open(
        "wb"
    ) as fout_bin:
        fout_bin.write(struct.pack("<I", total_lists))

        for raw in fin:
            parsed = parse_line(raw)
            if parsed is None:
                continue

            term_id, payload = parsed
            if term_id < expected_term:
                raise ValueError(
                    f"Orden de terminos no monotono: {term_id} despues de {expected_term-1}"
                )

            # Completar listas faltantes con largo 0
            while expected_term < term_id:
                fout_bin.write(struct.pack("<I", 0))
                if fout_txt is not None:
                    fout_txt.write("\n")
                empty_lists += 1
                expected_term += 1

            docs: list[int] = []
            if payload:
                pairs = PAIR_RE.findall(payload)
                if pairs:
                    for m_txt, r_txt in pairs:
                        master = int(m_txt)
                        rel = int(r_txt)
                        docid = tuple_to_docid(
                            master,
                            rel,
                            tuple_output=tuple_output,
                            mapping=mapping,
                            master_bits=master_bits,
                            rel_bits=rel_bits,
                        )
                        if docid < 0 or docid > 0xFFFFFFFFFFFFFFFF:
                            raise ValueError(f"docid fuera de rango uint64: {docid}")
                        docs.append(docid)
                    tuples_seen += len(pairs)
                else:
                    ints = INT_RE.findall(payload)
                    for token in ints:
                        value = int(token)
                        if value < 0 or value > 0xFFFFFFFFFFFFFFFF:
                            raise ValueError(f"valor fuera de rango uint64: {value}")
                        docs.append(value)
                    ints_seen += len(ints)

            fout_bin.write(struct.pack("<I", len(docs)))
            if docs:
                arr = array("Q", docs)
                if arr.itemsize != 8:
                    raise RuntimeError("array('Q') no es uint64 en esta plataforma")
                fout_bin.write(arr.tobytes())

            if fout_txt is not None:
                fout_txt.write(" ".join(str(x) for x in docs) + "\n")

            expected_term += 1

        while expected_term < total_lists:
            fout_bin.write(struct.pack("<I", 0))
            if fout_txt is not None:
                fout_txt.write("\n")
            empty_lists += 1
            expected_term += 1

    if fout_txt is not None:
        fout_txt.close()

    print("[OK] Conversion finalizada")
    print(f"[OK] output_bin={output_bin}")
    if output_txt is not None:
        print(f"[OK] output_txt={output_txt}")
    print(f"[STATS] postings_desde_tuplas={tuples_seen}")
    print(f"[STATS] postings_desde_enteros={ints_seen}")
    print(f"[STATS] listas_vacias={empty_lists}")
    print(f"[STATS] tuple_output={tuple_output}")

    # Metadata sidecar para validacion de split en consumidores C++.
    meta_path = output_bin.with_suffix(output_bin.suffix + ".meta")
    with meta_path.open("w", encoding="utf-8") as meta:
        meta.write("format=pisa_docs_v1\n")
        meta.write(f"input_path={input_path}\n")
        meta.write(f"tuple_output={tuple_output}\n")
        meta.write(f"master_bits={master_bits}\n")
        meta.write(f"rel_bits={rel_bits}\n")
        meta.write(f"total_lists={total_lists}\n")
        meta.write(f"has_pairs={int(has_pairs)}\n")
    print(f"[OK] meta={meta_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Convierte listas uiHRDC a formato .docs para TdZdd"
    )
    parser.add_argument("--dataset", default="wiki_100mb")
    parser.add_argument("--input-listas", default="")
    parser.add_argument("--tuple-output", default="packed", choices=["auto", "global", "packed"])
    parser.add_argument("--master-bits", type=int, default=40)
    parser.add_argument("--rel-bits", type=int, default=24)
    parser.add_argument("--export-txt", action="store_true", default=False)
    parser.add_argument(
        "--base-texts",
        default=os.path.join("/root", "MAGISTER", "uiHRDC", "uiHRDC", "data", "texts"),
    )
    parser.add_argument(
        "--base-output",
        default=os.path.join("/root", "MAGISTER", "resultados_test"),
    )
    parser.add_argument("--output-bin", default="")
    parser.add_argument("--page-map", default="")
    args = parser.parse_args()

    dataset = args.dataset
    input_listas_filename = args.input_listas.strip()
    if not input_listas_filename:
        if dataset.startswith("wiki_"):
            input_listas_filename = f"listas_wikipedia_zdd_{dataset}_versionada"
        else:
            input_listas_filename = f"listas_{dataset}"

    input_path = Path(os.path.join(args.base_texts, input_listas_filename))
    if args.output_bin:
        output_bin = Path(args.output_bin)
    else:
        output_bin = Path(
            os.path.join(args.base_output, f"{dataset}_uihrdc_{args.tuple_output}.docs")
        )

    output_txt = None
    if args.export_txt:
        output_txt = Path(
            os.path.join(args.base_output, f"{dataset}_uihrdc_{args.tuple_output}.txt")
        )

    page_map_path = None
    if args.page_map.strip():
        page_map_path = Path(args.page_map.strip())
    elif dataset.startswith("wiki_"):
        inferred = Path(os.path.join(args.base_texts, f"page_mapping_{dataset}.bin"))
        if inferred.exists():
            page_map_path = inferred

    convert(
        input_path=input_path,
        output_bin=output_bin,
        output_txt=output_txt,
        page_map_path=page_map_path,
        tuple_output=args.tuple_output,
        master_bits=args.master_bits,
        rel_bits=args.rel_bits,
    )


if __name__ == "__main__":
    main()
