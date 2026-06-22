#!/usr/bin/env python3
"""Run uiHRDC stress matrix (versioned and non-versioned) with metrics."""

from __future__ import annotations

import argparse
import csv
import glob
import json
import os
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT = Path("/root/MAGISTER")
II_DOCS_DIR = ROOT / "uiHRDC/uiHRDC/indexes/NOPOS/II_docs"
DEFAULT_LADDER = ROOT / "scripts/stress/dataset_ladder.json"
DEFAULT_OUTDIR = ROOT / "resultados_test/stress_motor"


@dataclass
class RunResult:
    phase: str
    dataset_id: str
    mode: str
    versioned: int
    command: str
    exit_code: int
    elapsed_s: float
    max_rss_kb: int
    index_bytes: int
    msec_pat: str
    usec_occ: str
    status: str
    log_path: str


def load_ladder(path: Path) -> list[dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    datasets = payload.get("datasets", [])
    if not datasets:
        raise ValueError(f"Dataset ladder is empty: {path}")
    return datasets


def parse_time_file(path: Path) -> tuple[float, int]:
    try:
        raw = path.read_text(encoding="utf-8").strip()
        seconds_str, rss_str = raw.split(",", 1)
        return float(seconds_str), int(rss_str)
    except Exception:
        return -1.0, -1


def parse_search_metrics(log_path: Path) -> tuple[str, str]:
    raw = log_path.read_text(encoding="utf-8", errors="ignore")
    msec_pat = ""
    usec_occ = ""

    m_msec = re.findall(r"msec/pat\s*[:=]\s*([0-9]+(?:\.[0-9]+)?)", raw, flags=re.IGNORECASE)
    if m_msec:
        msec_pat = m_msec[-1]

    m_usec = re.findall(r"usec/occ\s*[:=]\s*([0-9]+(?:\.[0-9]+)?)", raw, flags=re.IGNORECASE)
    if m_usec:
        usec_occ = m_usec[-1]

    return msec_pat, usec_occ


def compute_index_bytes(index_basename: str) -> int:
    total = 0
    for path in glob.glob(index_basename + "*"):
        p = Path(path)
        if p.is_file():
            total += p.stat().st_size
    return total


def run_timed(command: str, out_log: Path, out_time: Path, dry_run: bool) -> tuple[int, float, int]:
    out_log.parent.mkdir(parents=True, exist_ok=True)
    out_time.parent.mkdir(parents=True, exist_ok=True)
    shell_cmd = f"/usr/bin/time -f '%e,%M' -o '{out_time}' bash -lc \"{command}\" > '{out_log}' 2>&1"
    if dry_run:
        print(f"[DRY] {shell_cmd}")
        return 0, 0.0, 0

    proc = subprocess.run(shell_cmd, shell=True, cwd=II_DOCS_DIR)
    elapsed_s, max_rss_kb = parse_time_file(out_time)
    return proc.returncode, elapsed_s, max_rss_kb


def build_command(text_path: str, index_basename: str) -> str:
    return f"./BUILD_PFORDELTA_NOTEXT '{text_path}' '{index_basename}' 'nooptions'"


def search_command(index_basename: str, mode: str, patterns_path: str, repeats: int) -> str:
    if mode == "F":
        return (
            f"./SEARCH_PFORDELTA_NOTEXT '{index_basename}' F {repeats} 3 nooptions < '{patterns_path}'"
        )
    return f"./SEARCH_PFORDELTA_NOTEXT '{index_basename}' {mode} {repeats} nooptions < '{patterns_path}'"


def main() -> None:
    parser = argparse.ArgumentParser(description="Stress uiHRDC build/search with resource metrics")
    parser.add_argument("--ladder", type=Path, default=DEFAULT_LADDER)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTDIR)
    parser.add_argument("--max-minutes", type=float, default=30.0)
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--dataset-filter", nargs="*", default=[])
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    datasets = load_ladder(args.ladder)
    selected = set(args.dataset_filter)
    rows: list[RunResult] = []
    stop_due_to_time = False

    for ds in datasets:
        ds_id = ds["id"]
        if selected and ds_id not in selected:
            continue

        versioned = 1 if ds["versioned"] else 0
        mode_list = ["E", "V", "F"] if versioned else ["E"]
        dataset_dir = args.output_dir / ds_id
        dataset_dir.mkdir(parents=True, exist_ok=True)

        build_log = dataset_dir / "build.log"
        build_time = dataset_dir / "build.time"
        b_cmd = build_command(ds["text_path"], ds["index_basename"])
        b_exit, b_elapsed, b_rss = run_timed(b_cmd, build_log, build_time, args.dry_run)
        b_index_bytes = compute_index_bytes(ds["index_basename"]) if not args.dry_run else 0

        build_status = "ok"
        if b_exit != 0:
            build_status = "failed"
        elif b_elapsed > args.max_minutes * 60:
            build_status = "threshold_exceeded"
            stop_due_to_time = True

        rows.append(
            RunResult(
                phase="build",
                dataset_id=ds_id,
                mode="-",
                versioned=versioned,
                command=b_cmd,
                exit_code=b_exit,
                elapsed_s=b_elapsed,
                max_rss_kb=b_rss,
                index_bytes=b_index_bytes,
                msec_pat="",
                usec_occ="",
                status=build_status,
                log_path=str(build_log),
            )
        )

        if b_exit != 0 or stop_due_to_time:
            break

        for mode in mode_list:
            s_log = dataset_dir / f"search_{mode}.log"
            s_time = dataset_dir / f"search_{mode}.time"
            s_cmd = search_command(ds["index_basename"], mode, ds["patterns_path"], args.repeats)
            s_exit, s_elapsed, s_rss = run_timed(s_cmd, s_log, s_time, args.dry_run)
            msec_pat, usec_occ = ("", "") if args.dry_run else parse_search_metrics(s_log)
            s_status = "ok"

            if s_exit != 0:
                s_status = "failed"
            elif s_elapsed > args.max_minutes * 60:
                s_status = "threshold_exceeded"
                stop_due_to_time = True

            rows.append(
                RunResult(
                    phase="search",
                    dataset_id=ds_id,
                    mode=mode,
                    versioned=versioned,
                    command=s_cmd,
                    exit_code=s_exit,
                    elapsed_s=s_elapsed,
                    max_rss_kb=s_rss,
                    index_bytes=compute_index_bytes(ds["index_basename"]) if not args.dry_run else 0,
                    msec_pat=msec_pat,
                    usec_occ=usec_occ,
                    status=s_status,
                    log_path=str(s_log),
                )
            )

            if s_exit != 0 or stop_due_to_time:
                break

        if stop_due_to_time:
            break

    args.output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = args.output_dir / "motor_stress_results.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "phase",
                "dataset",
                "mode",
                "versioned",
                "exit",
                "elapsed_s",
                "max_rss_kb",
                "index_bytes",
                "msec_pat",
                "usec_occ",
                "status",
                "command",
                "log_path",
            ]
        )
        for r in rows:
            writer.writerow(
                [
                    r.phase,
                    r.dataset_id,
                    r.mode,
                    r.versioned,
                    r.exit_code,
                    f"{r.elapsed_s:.6f}",
                    r.max_rss_kb,
                    r.index_bytes,
                    r.msec_pat,
                    r.usec_occ,
                    r.status,
                    r.command,
                    r.log_path,
                ]
            )

    print(f"[OK] results_csv={csv_path}")


if __name__ == "__main__":
    main()
