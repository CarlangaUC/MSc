#!/usr/bin/env python3
import argparse
import os
import re
import struct
from pathlib import Path


def load_boundaries(path: Path) -> list[int]:
    data = path.read_bytes()
    if len(data) % 8 == 0:
        n = len(data) // 8
        return list(struct.unpack(f"<{n}Q", data))
    if len(data) % 4 == 0:
        n = len(data) // 4
        return list(struct.unpack(f"<{n}I", data))
    raise ValueError(f"Formato de boundaries invalido: {path} (bytes={len(data)})")


def copy_prefix(src: Path, dst: Path, total_bytes: int) -> None:
    chunk_size = 10 * 1024 * 1024
    remaining = total_bytes
    with src.open("rb") as fin, dst.open("wb") as fout:
        while remaining > 0:
            to_read = min(chunk_size, remaining)
            chunk = fin.read(to_read)
            if not chunk:
                break
            fout.write(chunk)
            remaining -= len(chunk)


def acortar_dataset_wiki(
    txt_in: Path,
    bound_in: Path,
    log_in: Path,
    txt_out: Path,
    bound_out: Path,
    log_out: Path,
    target_size_mb: int,
) -> None:
    target_size_bytes = target_size_mb * 1024 * 1024

    print(f"[INFO] Leyendo boundaries: {bound_in}")
    boundaries = load_boundaries(bound_in)
    if not boundaries:
        raise ValueError("No se encontraron boundaries")

    versiones_acumuladas = 0
    docs_maestros_guardados = 0
    lineas_log_guardar: list[str] = []

    pattern = re.compile(r"parsed\s+(\d+)\s+versions,\s+for\s+document")
    with log_in.open("r", encoding="utf-8", errors="ignore") as f_log:
        for line in f_log:
            lineas_log_guardar.append(line)
            m = pattern.search(line)
            if not m:
                continue
            num_versiones = int(m.group(1))
            versiones_acumuladas += num_versiones
            docs_maestros_guardados += 1
            if versiones_acumuladas < len(boundaries):
                size_now = boundaries[versiones_acumuladas]
                if size_now >= target_size_bytes:
                    break

    if versiones_acumuladas >= len(boundaries):
        versiones_acumuladas = len(boundaries) - 1

    bytes_a_copiar = boundaries[versiones_acumuladas]
    print(
        f"[INFO] Corte: masters={docs_maestros_guardados}, versiones={versiones_acumuladas}, "
        f"size_mb={bytes_a_copiar / (1024**2):.2f}"
    )

    log_out.parent.mkdir(parents=True, exist_ok=True)
    with log_out.open("w", encoding="utf-8") as fout:
        fout.writelines(lineas_log_guardar)
    print(f"[OK] log: {log_out}")

    nuevos_boundaries = boundaries[: versiones_acumuladas + 1]
    bound_out.parent.mkdir(parents=True, exist_ok=True)
    with bound_out.open("wb") as fout:
        fout.write(struct.pack(f"<{len(nuevos_boundaries)}Q", *nuevos_boundaries))
    print(f"[OK] boundaries: {bound_out}")

    txt_out.parent.mkdir(parents=True, exist_ok=True)
    copy_prefix(txt_in, txt_out, bytes_a_copiar)
    print(f"[OK] text: {txt_out}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Recorta dataset wiki por tamaño objetivo en MB")
    parser.add_argument("--name-input", default="wiki_src2gb")
    parser.add_argument("--name-output", required=True)
    parser.add_argument("--target-size-mb", type=int, required=True)
    parser.add_argument("--base-dir", default="archivos_test")
    args = parser.parse_args()

    base = Path(args.base_dir)
    name_input = args.name_input
    name_output = args.name_output

    acortar_dataset_wiki(
        txt_in=base / f"{name_input}.txt",
        bound_in=base / f"{name_input}.txt.DOCBOUNDARIES.ul",
        log_in=base / f"{name_input}.log",
        txt_out=base / f"{name_output}.txt",
        bound_out=base / f"{name_output}.txt.DOCBOUNDARIES.ul",
        log_out=base / f"{name_output}.log",
        target_size_mb=args.target_size_mb,
    )


if __name__ == "__main__":
    main()