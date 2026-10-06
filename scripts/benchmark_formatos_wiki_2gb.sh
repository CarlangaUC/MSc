#!/usr/bin/env bash
# Benchmark formatos wiki_2gb: sucio | marcado | torsen × baseline | nodes_desc+sift
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
OUT="$ROOT/resultados_test"
HEUR="${HEUR:-nodes_desc+sift}"
MAX_SIFT="${MAX_SIFT:-200}"
ZDD="$ROOT/zdd_cudd_plus_t"
MEASURE="$ROOT/scripts/measure_zpack_bpi"
CSV="$OUT/benchmark_formatos_wiki_2gb.csv"
export LD_LIBRARY_PATH="$ROOT/cudd/cudd/.libs:${LD_LIBRARY_PATH:-}"

declare -A PACK DOCS VOC
PACK[sucio]="$OUT/wiki_2gb_plus_t.zpack"
DOCS[sucio]="$OUT/wiki_2gb_uihrdc_packed64.docs"
VOC[sucio]="$ROOT/uiHRDC/uiHRDC/data/texts/index_wiki_2gb_named.voc"

PACK[marcado]="$OUT/wiki_2gb_limpio_marcado_plus_t.zpack"
DOCS[marcado]="$OUT/wiki_2gb_limpio_marcado_uihrdc_packed64.docs"
VOC[marcado]="$OUT/index_wiki_2gb_limpio_marcado_named.voc"

PACK[torsen]="$OUT/wiki_2gb_limpio_torsen_plus_t.zpack"
DOCS[torsen]="$OUT/wiki_2gb_limpio_torsen_uihrdc_packed64.docs"
VOC[torsen]="$OUT/index_wiki_2gb_limpio_torsen_named.voc"

measure_one() {
  local fmt="$1" stage="$2" pack="$3" docs="$4"
  local log="$OUT/bpi_wiki_2gb_${fmt}_${stage}.log"
  echo "[measure] fmt=$fmt stage=$stage"
  "$MEASURE" "$pack" "$docs" 2>&1 | tee "$log"
}

optimize_one() {
  local fmt="$1" in="$2" docs="$3" out="$4"
  local log="$OUT/optimize_wiki_2gb_${fmt}_nodes_desc_sift.log"
  if [[ -f "$out" ]]; then
    echo "[optimize] SKIP fmt=$fmt (exists $out)"
    return 0
  fi
  echo "[optimize] fmt=$fmt heur=$HEUR -> $out"
  "$ZDD" optimize u+t "$in" "$docs" "$out" "$HEUR" "$MAX_SIFT" 0 2>&1 | tee "$log"
}

append_csv_row() {
  local fmt="$1" stage="$2" log="$3"
  python3 - "$fmt" "$stage" "$log" "$CSV" <<'PY'
import sys, re
fmt, stage, log, csv = sys.argv[1:5]
text = open(log).read()
def g(k):
    m = re.search(rf'^{re.escape(k)}=(.+)$', text, re.M)
    return m.group(1).strip() if m else ""
row = {
    "formato": fmt, "stage": stage,
    "V": g("terms_scanned"), "n_raw": g("n_raw"),
    "n_snap_elems": g("n_snap_elems"), "ratio_raw_over_stored": g("ratio_raw_over_stored"),
    "edd_nodes": g("edd_nodes"), "bpi_edd": g("bpi_edd"),
    "bpi_edd_over_stored": g("bpi_edd_over_stored"), "bpi_file": g("bpi_file"),
    "numZddVars": g("numZddVars"), "levels_used": g("levels_used"),
    "file_bytes": g("file_bytes"),
}
header = list(row.keys())
write_header = not __import__("pathlib").Path(csv).exists() or __import__("pathlib").Path(csv).stat().st_size == 0
with open(csv, "a") as f:
    if write_header:
        f.write(",".join(header) + "\n")
    f.write(",".join(row[h] for h in header) + "\n")
print(f"[csv] {fmt}/{stage} -> {csv}")
PY
}

run_format() {
  local fmt="$1"
  local base="${PACK[$fmt]}"
  local docs="${DOCS[$fmt]}"
  local opt="$OUT/wiki_2gb_${fmt/_limpio_/}_nodes_desc_sift.zpack"
  case "$fmt" in
    sucio) opt="$OUT/wiki_2gb_sucio_nodes_desc_sift.zpack" ;;
    marcado) opt="$OUT/wiki_2gb_limpio_marcado_nodes_desc_sift.zpack" ;;
    torsen) opt="$OUT/wiki_2gb_limpio_torsen_nodes_desc_sift.zpack" ;;
  esac

  if [[ ! -f "$base" ]]; then
    echo "ERROR: falta pack baseline $base" >&2
    return 1
  fi
  measure_one "$fmt" baseline "$base" "$docs"
  append_csv_row "$fmt" baseline "$OUT/bpi_wiki_2gb_${fmt}_baseline.log"

  optimize_one "$fmt" "$base" "$docs" "$opt"
  if [[ -f "$opt" ]]; then
    measure_one "$fmt" optimized "$opt" "$docs"
    append_csv_row "$fmt" optimized "$OUT/bpi_wiki_2gb_${fmt}_optimized.log"
  fi
}

MODE="${1:-all}"
rm -f "$CSV"

case "$MODE" in
  sucio|marcado|torsen) run_format "$MODE" ;;
  all)
    run_format sucio
    run_format marcado
    run_format torsen
    ;;
  report-only)
    for fmt in sucio marcado torsen; do
      for stage in baseline optimized; do
        log="$OUT/bpi_wiki_2gb_${fmt}_${stage}.log"
        [[ -f "$log" ]] && append_csv_row "$fmt" "$stage" "$log" || true
      done
    done
    ;;
  *) echo "Uso: $0 [sucio|marcado|torsen|all|report-only]" >&2; exit 1 ;;
esac

echo "[OK] CSV -> $CSV"
