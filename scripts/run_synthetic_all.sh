#!/usr/bin/env bash
# Corridas sintéticas: sparse 500MB, toggle 500MB, variable U/V (build ZDD).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
OUT="$ROOT/resultados_test"
GEN="$ROOT/scripts/generar_docs_versionados_sinteticos.py"
ZDD="$ROOT/zdd_cudd_plus_t"
LOG="$OUT/synthetic_all_batch.log"
export PYTHONUNBUFFERED=1
export LD_LIBRARY_PATH="$ROOT/cudd/cudd/.libs:${LD_LIBRARY_PATH:-}"

exec > >(tee -a "$LOG") 2>&1
log() { echo "[$(date -Is)] $*"; }

try_build() {
  local docs="$1" voc="$2" pack="$3" evol="$4"
  if [[ ! -f "$docs" || ! -f "$voc" ]]; then
    log "SKIP build (faltan docs/voc): $docs"
    return 1
  fi
  if [[ -f "$pack" ]]; then
    log "SKIP build (exists): $pack"
    return 0
  fi
  log "ZDD build: $pack"
  if "$ZDD" build u+t "$docs" "$voc" "$pack" 0 "$evol" 500; then
    "$ROOT/scripts/measure_zpack_bpi" "$pack" "$docs" || true
    return 0
  fi
  return 1
}

log "========== synthetic batch start =========="

log "--- [1/4] sparse + Zipf ~500MB ---"
python3 "$GEN" \
  --terms 10000 --versions 100 --universe-size 2097152 \
  --init-distribution zipf --init-card-mean 65 --init-card-min 1 --init-card-max 8000 \
  --zipf-alpha 1.0 --delete-prob 0.01 --add-ratio 0.01 \
  --seed 20250914 --stem synthetic_500mb_t10k_v100 \
  --max-postings 500000000 --force --run-zdd

log "--- [2/4] toggle ~500MB ---"
python3 "$GEN" \
  --terms 10000 --versions 100 --universe-size 2097152 \
  --init-distribution constant --init-card-mean 50 --init-card-min 50 --init-card-max 50 \
  --evolution-model toggle --toggle-prob 0.01 --toggle-window 128 \
  --seed 20250914 --stem synthetic_toggle_500mb \
  --max-postings 500000000 --force --run-zdd

log "--- [3/4] variable U/V ~2GB docs (build ZDD si cabe en RAM) ---"
DOCS_2G="$OUT/synthetic_2gb_var_uv_toggle.docs"
VOC_2G="$OUT/synthetic_2gb_var_uv_toggle.voc"
PACK_2G="$OUT/synthetic_2gb_var_uv_toggle_plus_t.zpack"
EVOL_2G="$OUT/synthetic_2gb_var_uv_toggle_plus_t_evol.csv"
rm -f "$PACK_2G" "$EVOL_2G"
if [[ -f "$DOCS_2G" ]]; then
  if ! try_build "$DOCS_2G" "$VOC_2G" "$PACK_2G" "$EVOL_2G"; then
    log "WARN: build 2GB var falló (probable OOM); sigue escala wiki"
  fi
else
  log "WARN: no hay $DOCS_2G; omitiendo build 2GB"
fi

log "--- [4/4] variable U/V escala wiki (~122M postings, build ZDD) ---"
python3 "$GEN" \
  --terms 12800 \
  --versions-min 80 --versions-max 200 \
  --universe-size-min 262144 --universe-size-max 16777216 \
  --init-distribution zipf --init-card-mean 58 --init-card-min 2 --init-card-max 12000 \
  --zipf-alpha 1.05 \
  --evolution-model toggle --toggle-prob 0.01 --toggle-window 128 \
  --seed 20250916 --stem synthetic_var_uv_wiki_scale \
  --max-postings 400000000 --force --run-zdd

log "========== synthetic batch done =========="
