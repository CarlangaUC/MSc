#!/usr/bin/env bash
# Solo optimize (nodes_desc+sift) para sucio | marcado | torsen @ wiki_2gb.
# Reconstruye baseline .zpack/.docs si faltan antes de optimizar.
#
# Uso:
#   nohup ./scripts/run_optimize_formatos_wiki_2gb.sh >> resultados_test/optimize_formatos_batch.log 2>&1 &
#   tail -f resultados_test/optimize_formatos_batch.log
#
# Variables:
#   HEUR=nodes_desc+sift  MAX_SIFT=200  FORCE=0|1
#   FORMATOS=sucio,marcado,torsen
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
OUT="$ROOT/resultados_test"
HEUR="${HEUR:-nodes_desc+sift}"
MAX_SIFT="${MAX_SIFT:-200}"
FORCE="${FORCE:-0}"
FORMATOS="${FORMATOS:-sucio,marcado,torsen}"
ZDD="$ROOT/zdd_cudd_plus_t"
MEASURE="$ROOT/scripts/measure_zpack_bpi"
UIHRDC="$ROOT/uiHRDC/uiHRDC/indexes/NOPOS/II_docs/BUILD_PFORDELTA_NOTEXT"
TEXTS="$ROOT/uiHRDC/uiHRDC/data/texts"
export LD_LIBRARY_PATH="$ROOT/cudd/cudd/.libs:${LD_LIBRARY_PATH:-}"

log() { echo "[$(date -Is)] $*"; }

paths_for() {
  local fmt="$1"
  case "$fmt" in
    sucio)
      BASE="$OUT/wiki_2gb_plus_t.zpack"
      DOCS="$OUT/wiki_2gb_uihrdc_packed64.docs"
      VOC="$TEXTS/index_wiki_2gb_named.voc"
      OPT="$OUT/wiki_2gb_sucio_nodes_desc_sift.zpack"
      ;;
    marcado)
      BASE="$OUT/wiki_2gb_limpio_marcado_plus_t.zpack"
      DOCS="$OUT/wiki_2gb_limpio_marcado_uihrdc_packed64.docs"
      VOC="$OUT/index_wiki_2gb_limpio_marcado_named.voc"
      OPT="$OUT/wiki_2gb_limpio_marcado_nodes_desc_sift.zpack"
      ;;
    torsen)
      BASE="$OUT/wiki_2gb_limpio_torsen_plus_t.zpack"
      DOCS="$OUT/wiki_2gb_limpio_torsen_uihrdc_packed64.docs"
      VOC="$OUT/index_wiki_2gb_limpio_torsen_named.voc"
      OPT="$OUT/wiki_2gb_limpio_torsen_nodes_desc_sift.zpack"
      ;;
    *)
      echo "formato desconocido: $fmt" >&2
      return 1
      ;;
  esac
}

ensure_sucio_baseline() {
  local docs="$OUT/wiki_2gb_uihrdc_packed64.docs"
  local pack="$OUT/wiki_2gb_plus_t.zpack"
  local voc="$TEXTS/index_wiki_2gb_named.voc"
  local listas="$TEXTS/listas_wiki_2gb_versionada"
  local page_map="$TEXTS/page_mapping_wiki_2gb.bin"

  if [[ ! -f "$docs" ]]; then
    log "sucio: generando .docs desde listas versionadas..."
    [[ -f "$listas" ]] || { log "ERROR: falta $listas"; exit 1; }
    python3 "$ROOT/scripts/convertir_versionado_input_uiHRDC.py" \
      --dataset wiki_2gb \
      --input-listas listas_wiki_2gb_versionada \
      --base-texts "$TEXTS" \
      --output-bin "$docs" \
      --tuple-output packed64 \
      --page-map "$page_map"
    python3 "$ROOT/scripts/validate_packed64_docs.py" "$docs"
  fi

  if [[ ! -f "$pack" ]]; then
    log "sucio: build baseline ZDD (puede tardar)..."
    "$ZDD" build u+t "$docs" "$voc" "$pack" 0 \
      "$OUT/cudd_evolucion_wiki_2gb_plus_t.csv" 500
  fi
}

ensure_limpio_baseline() {
  local nivel="$1"
  local pack="$OUT/wiki_2gb_limpio_${nivel}_plus_t.zpack"
  if [[ -f "$pack" ]]; then
    log "$nivel: baseline ya existe ($pack)"
    return 0
  fi
  log "$nivel: pipeline baseline (pasos 0-4, sin optimize)..."
  OPTIMIZE=0 HEUR=none WORKERS="${WORKERS:-1}" \
    "$ROOT/scripts/pipeline_limpieza_bpi.sh" wiki_2gb "$nivel"
}

run_optimize_one() {
  local fmt="$1"
  paths_for "$fmt"
  local opt_log="$OUT/optimize_wiki_2gb_${fmt}_nodes_desc_sift.log"
  local bpi_log="$OUT/bpi_wiki_2gb_${fmt}_optimized.log"

  if [[ -f "$OPT" && "$FORCE" != "1" ]]; then
    log "$fmt: SKIP optimize (exists $OPT)"
    return 0
  fi

  [[ -f "$BASE" && -f "$DOCS" ]] || {
    log "ERROR: $fmt baseline incompleto base=$BASE docs=$DOCS"
    exit 1
  }

  log "$fmt: optimize heur=$HEUR -> $OPT"
  local t0=$SECONDS
  "$ZDD" optimize u+t "$BASE" "$DOCS" "$OPT" "$HEUR" "$MAX_SIFT" 0 2>&1 | tee "$opt_log"
  "$MEASURE" "$OPT" "$DOCS" 2>&1 | tee "$bpi_log"
  log "$fmt: optimize OK elapsed=$((SECONDS - t0))s"
}

log "========== optimize formatos wiki_2gb =========="
log "HEUR=$HEUR MAX_SIFT=$MAX_SIFT FORCE=$FORCE FORMATOS=$FORMATOS"

IFS=',' read -r -a FMTS <<< "$FORMATOS"
for fmt in "${FMTS[@]}"; do
  fmt="$(echo "$fmt" | xargs)"
  [[ -z "$fmt" ]] && continue
  log "---- formato=$fmt ----"
  case "$fmt" in
    sucio) ensure_sucio_baseline ;;
    marcado|torsen) ensure_limpio_baseline "$fmt" ;;
    *) log "ERROR: formato $fmt"; exit 1 ;;
  esac
  run_optimize_one "$fmt"
done

log "========== FIN optimize formatos =========="
