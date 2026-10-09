#!/usr/bin/env bash
# Benchmark Q_τ: metatrie values_at vs docs_scan; luego ZDD Q_mem (membresía).
# Uso:
#   scripts/bench_query_tau.sh <docs> [mode=global|per-term] [n_queries] [reps] [max_terms]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DOCS="${1:?docs path}"
MODE="${2:-global}"
NQ="${3:-500}"
REPS="${4:-5}"
MAX_TERMS="${5:-0}"
OUT_DIR="$ROOT/resultados_test"
mkdir -p "$OUT_DIR"
STAMP="$(date +%Y%m%d_%H%M%S)"
BASE="$(basename "$DOCS" .docs)"
CSV="$OUT_DIR/bench_query_tau_${BASE}_${MODE}_${STAMP}.csv"
QFILE="$OUT_DIR/bench_queries_${BASE}_${MODE}_${STAMP}.csv"
LOG="$OUT_DIR/bench_query_tau_${BASE}_${MODE}_${STAMP}.log"

MT="$ROOT/edd_metatrie/meta_trie_edd"
ZDD="$ROOT/zdd_cudd_plus_t"

echo "[1/3] build meta_trie_edd" | tee "$LOG"
( cd "$ROOT/edd_metatrie" && ./build.sh ) >>"$LOG" 2>&1

echo "[2/3] build zdd_cudd_plus_t" | tee -a "$LOG"
g++ -O2 -std=c++17 -fopenmp -o "$ZDD" "$ROOT/plus_t/main.cpp" \
  -I "$ROOT/plus_t" -I "$ROOT" -I "$ROOT/cudd/cudd" -I "$ROOT/cudd" \
  -L "$ROOT/cudd/cudd/.libs" -Wl,-rpath,'$ORIGIN/cudd/cudd/.libs' -lcudd \
  -I "$ROOT/TdZdd/include" \
  -I "$ROOT/uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils" \
  >>"$LOG" 2>&1

MT_ARGS=("$MODE" "$DOCS" --validate-terms 50 --bench-queries "$NQ" --bench-reps "$REPS"
         --bench-csv "$CSV" --bench-queries-out "$QFILE")
if [[ "$MAX_TERMS" != "0" ]]; then
  MT_ARGS+=(--max-terms "$MAX_TERMS")
fi

echo "[3/3] metatrie bench → $CSV" | tee -a "$LOG"
"$MT" "${MT_ARGS[@]}" 2>&1 | tee -a "$LOG"

ZDD_MAX="$MAX_TERMS"
if [[ "$ZDD_MAX" == "0" ]]; then ZDD_MAX=200; fi
echo "[3b] zdd bench-qmem baseline (build sin optimize, max_terms=$ZDD_MAX)" | tee -a "$LOG"
PACK="$OUT_DIR/bench_${BASE}_baseline_mt${ZDD_MAX}_${STAMP}.zpack"
VOC="${VOC:-}"
if [[ -z "$VOC" ]]; then
  # best-effort voc next to common wiki names
  for cand in \
    "$ROOT/uiHRDC/uiHRDC/data/texts/index_${BASE}_named.voc" \
    "$ROOT/uiHRDC/uiHRDC/data/texts/index_wiki_2gb_named.voc" \
    "$ROOT/uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc"
  do
    [[ -f "$cand" ]] && VOC="$cand" && break
  done
fi
if [[ -n "$VOC" && -f "$VOC" ]]; then
  echo "[3b] ZDD build (NO optimize) → $PACK voc=$VOC" | tee -a "$LOG"
  set +e
  "$ZDD" build u+t "$DOCS" "$VOC" "$PACK" "$ZDD_MAX" \
    "$OUT_DIR/cudd_evolucion_bench_${BASE}_${STAMP}.csv" 500 >>"$LOG" 2>&1
  brc=$?
  set -e
  if [[ $brc -eq 0 ]]; then
    set +e
    "$ZDD" bench-qmem u+t "$DOCS" "$QFILE" --pack "$PACK" "$REPS" "$CSV" 2>&1 | tee -a "$LOG"
    set -e
  else
    echo "[3b] ZDD build failed rc=$brc; fallback buildForest in-process" | tee -a "$LOG"
    set +e
    "$ZDD" bench-qmem u+t "$DOCS" "$QFILE" "$ZDD_MAX" "$REPS" "$CSV" 2>&1 | tee -a "$LOG"
    set -e
  fi
else
  echo "[3b] no .voc found; ZDD buildForest in-process (no optimize)" | tee -a "$LOG"
  set +e
  "$ZDD" bench-qmem u+t "$DOCS" "$QFILE" "$ZDD_MAX" "$REPS" "$CSV" 2>&1 | tee -a "$LOG"
  set -e
fi

echo "DONE csv=$CSV queries=$QFILE log=$LOG" | tee -a "$LOG"
