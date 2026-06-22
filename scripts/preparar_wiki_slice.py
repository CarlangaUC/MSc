#!/usr/bin/env python3
"""Prepara wiki_Ngb.txt + page_mapping + DOCBOUNDARIES desde wiki_2gb."""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path

PARSED_RE = re.compile(
    r"parsed\s+(\d+)\s+versions,\s+for\s+document\s+(\d+)-->\s*(\d+)\s*bytes"
)
TOTALSIZE_RE = re.compile(r"totalsize\s*=\s*(\d+)")


def parse_log_masters(log_path: Path, byte_limit: int) -> tuple[list[int], int, int]:
    """Devuelve page_starts, num_masters, total_global_versions."""
    page_starts = [0]
    cumulative_versions = 0
    last_totalsize = 0
    last_master = -1

    with log_path.open("r", encoding="utf-8", errors="ignore") as fin:
        for line in fin:
            m = PARSED_RE.search(line)
            if m:
                n_versions = int(m.group(1))
                master = int(m.group(2))
                cumulative_versions += n_versions
                page_starts.append(cumulative_versions)
                last_master = master
                continue
            tm = TOTALSIZE_RE.search(line)
            if tm:
                last_totalsize = int(tm.group(1))
                if last_totalsize > byte_limit and len(page_starts) > 2:
                    page_starts.pop()
                    cumulative_versions = page_starts[-1]
                    last_master -= 1
                    break

    if last_master < 0:
        raise ValueError(f"No se parsearon masters en {log_path}")

    return page_starts, last_master + 1, cumulative_versions


def slice_boundaries(bound_path: Path, num_global_docs: int, out_path: Path) -> int:
    raw = bound_path.read_bytes()
    n = len(raw) // 8
    bounds = struct.unpack(f"<{n}Q", raw)
    if num_global_docs + 1 > n:
        raise ValueError(
            f"Se pidieron {num_global_docs} docs globales pero boundaries tiene {n - 1}"
        )
    end_byte = bounds[num_global_docs]
    sliced = bounds[: num_global_docs + 1]
    out_path.write_bytes(struct.pack(f"<{len(sliced)}Q", *sliced))
    return end_byte


def main() -> None:
    parser = argparse.ArgumentParser(description="Preparar slice versionado wiki desde 2gb")
    parser.add_argument("--size-gb", type=float, default=1.0)
    parser.add_argument(
        "--base-texts",
        type=Path,
        default=Path("/root/MAGISTER/uiHRDC/uiHRDC/data/texts"),
    )
    parser.add_argument("--source", default="wiki_2gb")
    parser.add_argument("--target", default="wiki_1gb")
    args = parser.parse_args()

    byte_limit = int(args.size_gb * 1024 * 1024 * 1024)
    base = args.base_texts
    src = args.source
    tgt = args.target

    log_path = base / f"{src}.log"
    txt_src = base / f"{src}.txt"
    bound_src = base / f"{src}.txt.DOCBOUNDARIES.ul"

    txt_dst = base / f"{tgt}.txt"
    bound_dst = base / f"{tgt}.txt.DOCBOUNDARIES.ul"
    map_dst = base / f"page_mapping_{tgt}.bin"
    log_dst = base / f"{tgt}.log"

    print(f"[INFO] byte_limit={byte_limit} ({args.size_gb} GiB)")
    page_starts, num_masters, num_globals = parse_log_masters(log_path, byte_limit)
    print(f"[INFO] masters={num_masters} global_versions={num_globals}")

    end_byte = slice_boundaries(bound_src, num_globals, bound_dst)
    print(f"[INFO] end_byte={end_byte}")

    with txt_src.open("rb") as fin, txt_dst.open("wb") as fout:
        remaining = end_byte
        while remaining > 0:
            chunk = fin.read(min(remaining, 8 * 1024 * 1024))
            if not chunk:
                break
            fout.write(chunk)
            remaining -= len(chunk)
    print(f"[OK] text -> {txt_dst} ({txt_dst.stat().st_size} bytes)")

    with map_dst.open("wb") as fmap:
        for start in page_starts:
            fmap.write(struct.pack("<I", start))
    print(f"[OK] page_mapping -> {map_dst} ({len(page_starts)} entries)")

    with log_path.open("r", encoding="utf-8", errors="ignore") as fin, log_dst.open(
        "w", encoding="utf-8"
    ) as fout:
        masters_seen = 0
        for line in fin:
            fout.write(line)
            m = PARSED_RE.search(line)
            if m:
                masters_seen = int(m.group(2)) + 1
                if masters_seen >= num_masters:
                    break
    print(f"[OK] log slice -> {log_dst}")


if __name__ == "__main__":
    main()
