#!/usr/bin/env bash
# Bench wiki 2GB: metatrie full + ZDD baseline (build sin optimize).
# ZDD: construye .zpack con `build` (no optimize), luego bench-qmem --pack.
set -euo pipefail
cd /root/MAGISTER
OUT=resultados_test
DOCS=resultados_test/wiki_2gb_packed64.docs
VOC="${VOC:-uiHRDC/uiHRDC/data/texts/index_wiki_2gb_named.voc}"
ZDD_MAX_TERMS="${ZDD_MAX_TERMS:-200}"
STAMP=$(date +%Y%m%d_%H%M%S)
MT=./edd_metatrie/meta_trie_edd
ZDD=./zdd_cudd_plus_t
PACK=$OUT/wiki_2gb_baseline_mt${ZDD_MAX_TERMS}_${STAMP}.zpack
MASTER_LOG=$OUT/run_bench_wiki2gb_full_zdd_baseline_${STAMP}.log

echo "=== ensure binaries $(date -Is) ===" | tee "$MASTER_LOG"
( cd edd_metatrie && ./build.sh ) >>"$MASTER_LOG" 2>&1
g++ -O2 -std=c++17 -fopenmp -o "$ZDD" plus_t/main.cpp \
  -I plus_t -I . -I ./cudd/cudd -I ./cudd \
  -L ./cudd/cudd/.libs -Wl,-rpath,'$ORIGIN/cudd/cudd/.libs' -lcudd \
  -I ./TdZdd/include \
  -I ./uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils >>"$MASTER_LOG" 2>&1

echo "=== ZDD build (NO optimize) max_terms=$ZDD_MAX_TERMS $(date -Is) ===" | tee -a "$MASTER_LOG"
/usr/bin/time -f 'wall_s=%e maxrss_kb=%M' "$ZDD" build u+t "$DOCS" "$VOC" "$PACK" "$ZDD_MAX_TERMS" \
  "$OUT/cudd_evolucion_wiki_2gb_bench_baseline_${STAMP}.csv" 500 \
  2>&1 | tee -a "$MASTER_LOG"

run_one() {
  local MODE=$1
  local CSV=$OUT/bench_query_tau_wiki_2gb_${MODE}_full_zddbase_${STAMP}.csv
  local QF=$OUT/bench_queries_wiki_2gb_${MODE}_full_zddbase_${STAMP}.csv
  local LOG=$OUT/bench_query_tau_wiki_2gb_${MODE}_full_zddbase_${STAMP}.log
  echo "=== metatrie $MODE FULL $(date -Is) ===" | tee "$LOG"
  /usr/bin/time -f 'wall_s=%e maxrss_kb=%M' stdbuf -oL -eL "$MT" "$MODE" "$DOCS" \
    --validate-terms 0 \
    --bench-queries 1000 --bench-reps 5 \
    --bench-csv "$CSV" --bench-queries-out "$QF" \
    2>&1 | tee -a "$LOG"
  echo "=== ZDD qmem from baseline .zpack $(date -Is) ===" | tee -a "$LOG"
  # Filtra solo terms < V del pack; no aborta el pipeline si ZDD falla
  set +e
  /usr/bin/time -f 'wall_s=%e maxrss_kb=%M' "$ZDD" bench-qmem u+t "$DOCS" "$QF" \
    --pack "$PACK" 5 "$CSV" 2>&1 | tee -a "$LOG"
  local zrc=${PIPESTATUS[0]}
  set -e
  echo "zdd_exit=$zrc pack=$PACK" | tee -a "$LOG"
  echo "DONE $MODE csv=$CSV" | tee -a "$LOG"
  cat "$CSV" | tee -a "$LOG"
  echo "$MODE" >>"$MASTER_LOG"
  cat "$CSV" >>"$MASTER_LOG"
}

run_one global
run_one per-term
echo "ALL_DONE $(date -Is) pack=$PACK" | tee -a "$MASTER_LOG"
