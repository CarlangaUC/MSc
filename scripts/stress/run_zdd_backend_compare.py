#!/usr/bin/env python3
"""Run comparable ZDD backend jobs (TdZdd now, CUDD-ready harness)."""

from __future__ import annotations

import argparse
import csv
import json
import shutil
import subprocess
from pathlib import Path


ROOT = Path("/root/MAGISTER")
DEFAULT_LADDER = ROOT / "scripts/stress/dataset_ladder.json"
DEFAULT_OUTDIR = ROOT / "resultados_test/stress_zdd_compare"


def parse_time_file(path: Path) -> tuple[float, int]:
    try:
        raw = path.read_text(encoding="utf-8").strip()
        sec, rss = raw.split(",", 1)
        return float(sec), int(rss)
    except Exception:
        return -1.0, -1


def timed_run(command: str, cwd: Path, log_path: Path, time_path: Path, dry_run: bool) -> tuple[int, float, int]:
    wrapper = f"/usr/bin/time -f '%e,%M' -o '{time_path}' bash -lc \"{command}\" > '{log_path}' 2>&1"
    if dry_run:
        print(f"[DRY] {wrapper}")
        return 0, 0.0, 0
    proc = subprocess.run(wrapper, shell=True, cwd=cwd)
    s, r = parse_time_file(time_path)
    return proc.returncode, s, r


def resolve_backend_command(
    backend: str, ds: dict, mode: str, max_terms_double: int, max_steps_single: int
) -> tuple[str, Path] | None:
    if backend == "tdzdd_single":
        docs = ds.get("docs_global_path")
        if not docs:
            return None
        cmd = f"./test 2 '{docs}' '{ds['id']}_single' {max_steps_single} 0"
        return cmd, ROOT

    if backend == "tdzdd_double":
        p = ds.get("docs_packed_path")
        m = ds.get("page_mapping_path") or ""
        if not p:
            return None
        cmd = (
            f"./scripts/cpp/test_tdzdd_piso1 '{p}' '{m}' 1 13 1730 1000 0 "
            f"'resultados_test/stress_zdd_compare/{ds['id']}_piso1.csv' {max_terms_double}"
        )
        return cmd, ROOT

    if backend == "cudd_single":
        binary = ROOT / "build/cudd_single_runner"
        docs = ds.get("docs_global_path")
        if not docs or not binary.exists():
            return None
        cmd = f"'{binary}' '{docs}'"
        return cmd, ROOT

    if backend == "cudd_double":
        binary = ROOT / "build/cudd_double_runner"
        g = ds.get("docs_global_path")
        p = ds.get("docs_packed_path")
        if not g or not p or not binary.exists():
            return None
        cmd = f"'{binary}' '{g}' '{p}'"
        return cmd, ROOT

    return None


def main() -> None:
    parser = argparse.ArgumentParser(description="Compare ZDD backends on same dataset ladder")
    parser.add_argument("--ladder", type=Path, default=DEFAULT_LADDER)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTDIR)
    parser.add_argument(
        "--backends",
        nargs="+",
        default=["tdzdd_single", "tdzdd_double"],
        choices=["tdzdd_single", "tdzdd_double", "cudd_single", "cudd_double"],
    )
    parser.add_argument("--dataset-filter", nargs="*", default=[])
    parser.add_argument("--max-terms-double", type=int, default=500)
    parser.add_argument("--max-steps-single", type=int, default=200)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if not shutil.which("/usr/bin/time"):
        raise RuntimeError("Missing /usr/bin/time")

    payload = json.loads(args.ladder.read_text(encoding="utf-8"))
    datasets = payload["datasets"]
    selected = set(args.dataset_filter)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    csv_path = args.output_dir / "zdd_backend_compare.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            ["dataset", "backend", "mode", "exit", "elapsed_s", "max_rss_kb", "status", "command", "log_path"]
        )

        for ds in datasets:
            if selected and ds["id"] not in selected:
                continue
            for backend in args.backends:
                mode = "double" if backend.endswith("double") else "single"
                resolved = resolve_backend_command(
                    backend, ds, mode, args.max_terms_double, args.max_steps_single
                )
                if resolved is None:
                    writer.writerow([ds["id"], backend, mode, -1, -1, -1, "skipped", "", ""])
                    continue

                command, cwd = resolved
                log_path = args.output_dir / f"{ds['id']}_{backend}.log"
                time_path = args.output_dir / f"{ds['id']}_{backend}.time"
                exit_code, elapsed_s, max_rss = timed_run(command, cwd, log_path, time_path, args.dry_run)
                status = "ok" if exit_code == 0 else "failed"
                writer.writerow(
                    [ds["id"], backend, mode, exit_code, f"{elapsed_s:.6f}", max_rss, status, command, str(log_path)]
                )

    print(f"[OK] compare_csv={csv_path}")


if __name__ == "__main__":
    main()
