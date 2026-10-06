#!/usr/bin/env bash
# =============================================================================
# pipeline_limpieza_bpi.sh — pasos 0→4: limpiar wikitext → uiHRDC → .docs → ZDD → BPI
# =============================================================================
# Uso:
#   ./scripts/pipeline_limpieza_bpi.sh wiki_100mb marcado
#   ./scripts/pipeline_limpieza_bpi.sh wiki_100mb torsen
#   ./scripts/pipeline_limpieza_bpi.sh wiki_2gb marcado
#   HEUR=nodes_desc+sift ./scripts/pipeline_limpieza_bpi.sh wiki_2gb marcado
#   BACKEND=mwph HEUR=nodes_desc+sift ./scripts/pipeline_limpieza_bpi.sh wiki_2gb marcado
#
# Datasets soportados: wiki_100mb, wiki_1gb, wiki_2gb (versionados con page_mapping)
# Optimize post-build: HEUR=nodes_desc+sift (default) | OPTIMIZE=0 para solo baseline
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DATASET="${1:-wiki_100mb}"
NIVEL="${2:-marcado}"
BACKEND="${BACKEND:-regex}"
WORKERS="${WORKERS:-$(( $(nproc) - 1 ))}"
HEUR="${HEUR:-nodes_desc+sift}"
MAX_SIFT="${MAX_SIFT:-200}"
OPTIMIZE="${OPTIMIZE:-1}"

TEXTS_DIR="$ROOT/uiHRDC/uiHRDC/data/texts"
OUT_DIR="$ROOT/resultados_test"
UIHRDC_BIN="$ROOT/uiHRDC/uiHRDC/indexes/NOPOS/II_docs/BUILD_PFORDELTA_NOTEXT"
ZDD_BIN="$ROOT/zdd_cudd_plus_t"
MEASURE_BIN="$ROOT/scripts/measure_zpack_bpi"

SRC_TXT="$TEXTS_DIR/${DATASET}.txt"
STEM="${DATASET}_limpio_${NIVEL}"
if [[ "$BACKEND" != "regex" ]]; then
  STEM="${STEM}_${BACKEND}"
fi
CLEAN_TXT="$OUT_DIR/${STEM}.txt"
PAGE_MAP_SRC="$TEXTS_DIR/page_mapping_${DATASET}.bin"
PAGE_MAP_DST="$OUT_DIR/page_mapping_${STEM}.bin"
LISTAS="$OUT_DIR/listas_${STEM}_versionada"
VOC="$OUT_DIR/index_${STEM}_named.voc"
DOCS="$OUT_DIR/${STEM}_uihrdc_packed64.docs"
PACK="$OUT_DIR/${STEM}_plus_t.zpack"
HEUR_TAG="${HEUR//+/_}"
PACK_OPT="$OUT_DIR/${STEM}_${HEUR_TAG}.zpack"
EVOL_CSV="$OUT_DIR/cudd_evolucion_${STEM}.csv"
BPI_LOG="$OUT_DIR/bpi_${STEM}.log"
BPI_OPT_LOG="$OUT_DIR/bpi_${STEM}_${HEUR_TAG}.log"
OPT_LOG="$OUT_DIR/optimize_${STEM}_${HEUR_TAG}.log"

if [[ ! -f "$SRC_TXT" ]]; then
  echo "ERROR: no existe $SRC_TXT" >&2
  exit 1
fi
if [[ ! -x "$UIHRDC_BIN" ]]; then
  echo "ERROR: compilar uiHRDC primero ($UIHRDC_BIN)" >&2
  exit 1
fi
if [[ ! -x "$ZDD_BIN" ]]; then
  echo "ERROR: compilar zdd_cudd_plus_t primero" >&2
  exit 1
fi
if [[ ! -x "$MEASURE_BIN" ]]; then
  echo "Compilando measure_zpack_bpi..."
  g++ -O2 -std=c++17 -fopenmp -o "$MEASURE_BIN" scripts/measure_zpack_bpi.cpp \
    -I plus_t -I . -I ./TdZdd/include -I ./cudd -I ./cudd/cudd -I ./cudd/st \
    -I ./cudd/util -I ./cudd/mtr -I ./cudd/epd \
    -I ./uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils \
    -L ./cudd/cudd/.libs -Wl,-rpath,'$ORIGIN/../cudd/cudd/.libs' -lcudd
fi

log_mem() {
  echo "[mem] $(date -Is) $* — $(free -h | awk '/^Mem:/ {print "used="$3" avail="$7}')"
}

echo "=============================================="
echo " Pipeline limpieza BPI"
echo " dataset=$DATASET  nivel=$NIVEL  backend=$BACKEND  workers=$WORKERS"
log_mem "inicio"
if [[ "$OPTIMIZE" == "1" && "$HEUR" != "none" && -n "$HEUR" ]]; then
  echo " optimize=$HEUR  max_sift=$MAX_SIFT"
else
  echo " optimize=off (baseline only)"
fi
echo "=============================================="

# --- Paso 0: limpiar wikitext ------------------------------------------------
echo
echo "[0/6] Limpiando wikitext..."
PYTHON="${PYTHON:-python3}"
if [[ -x "$ROOT/.venv/bin/python3" ]]; then
  PYTHON="$ROOT/.venv/bin/python3"
fi
"$PYTHON" scripts/limpiar_corpus_wiki.py \
  --input "$SRC_TXT" \
  --output "$CLEAN_TXT" \
  --nivel "$NIVEL" \
  --backend "$BACKEND" \
  --workers "$WORKERS"

# page_mapping: copiar con el stem del corpus limpio (mismos indices de linea)
if [[ -f "$PAGE_MAP_SRC" ]]; then
  cp -f "$PAGE_MAP_SRC" "$PAGE_MAP_DST"
  echo "[0/6] page_mapping copiado -> $PAGE_MAP_DST"
else
  echo "ERROR: falta $PAGE_MAP_SRC para dataset versionado" >&2
  exit 1
fi

# --- Paso 1: uiHRDC only_list_and_voc ----------------------------------------
echo
log_mem "pre-uiHRDC"
echo "[1/6] uiHRDC BUILD_PFORDELTA_NOTEXT only_list_and_voc..."
INDEX_BASE="$OUT_DIR/index_${STEM}_named"
export PAGE_MAPPING_BIN="$PAGE_MAP_DST"
"$UIHRDC_BIN" "$CLEAN_TXT" "$INDEX_BASE" "only_list_and_voc"

if [[ ! -f "$LISTAS" ]]; then
  # uiHRDC escribe listas en el directorio del .txt
  if [[ -f "$OUT_DIR/listas_${STEM}_versionada" ]]; then
    LISTAS="$OUT_DIR/listas_${STEM}_versionada"
  else
    echo "ERROR: no se genero listas versionadas" >&2
    exit 1
  fi
fi
if [[ ! -f "$VOC" ]]; then
  echo "ERROR: no se genero $VOC" >&2
  exit 1
fi
echo "[1/6] listas=$LISTAS  voc=$VOC"

# --- Paso 2: listas -> .docs packed64 ----------------------------------------
echo
echo "[2/6] convertir_versionado_input_uiHRDC.py..."
python3 scripts/convertir_versionado_input_uiHRDC.py \
  --dataset "$STEM" \
  --input-listas "listas_${STEM}_versionada" \
  --base-texts "$OUT_DIR" \
  --output-bin "$DOCS" \
  --tuple-output packed64 \
  --page-map "$PAGE_MAP_DST"
python3 "$ROOT/scripts/validate_packed64_docs.py" "$DOCS"

# --- Paso 3: build ZDD u+t ---------------------------------------------------
echo
log_mem "pre-ZDD-build"
echo "[3/6] zdd_cudd_plus_t build u+t..."
export LD_LIBRARY_PATH="$ROOT/cudd/cudd/.libs:${LD_LIBRARY_PATH:-}"
rm -f "$PACK" "$EVOL_CSV"
"$ZDD_BIN" build u+t "$DOCS" "$VOC" "$PACK" 0 "$EVOL_CSV" 500

# --- Paso 4: medir BPI baseline ------------------------------------------------
echo
echo "[4/6] measure_zpack_bpi (baseline)..."
"$MEASURE_BIN" "$PACK" "$DOCS" 2>&1 | tee "$BPI_LOG"

if [[ "$OPTIMIZE" == "1" && "$HEUR" != "none" && -n "$HEUR" ]]; then
  # --- Paso 5: optimize post-build (heuristica CUDD) ---------------------------
  echo
  log_mem "pre-optimize"
  echo "[5/6] zdd_cudd_plus_t optimize ($HEUR)..."
  "$ZDD_BIN" optimize u+t "$PACK" "$DOCS" "$PACK_OPT" "$HEUR" "$MAX_SIFT" 0 \
    2>&1 | tee "$OPT_LOG"

  # --- Paso 6: medir BPI optimized ---------------------------------------------
  echo
  echo "[6/6] measure_zpack_bpi (optimized)..."
  "$MEASURE_BIN" "$PACK_OPT" "$DOCS" 2>&1 | tee "$BPI_OPT_LOG"
else
  echo
  echo "[5/6] optimize omitido (OPTIMIZE=$OPTIMIZE HEUR=$HEUR)"
  echo "[6/6] omitido"
fi

echo
echo "=============================================="
echo " OK pipeline completo"
echo " pack_baseline=$PACK"
if [[ -f "$PACK_OPT" ]]; then
  echo " pack_optimized=$PACK_OPT"
fi
echo " docs=$DOCS"
echo " bpi_baseline=$BPI_LOG"
if [[ -f "$BPI_OPT_LOG" ]]; then
  echo " bpi_optimized=$BPI_OPT_LOG"
fi
echo "=============================================="

# Extraer metricas clave
echo "--- baseline ---"
grep -E '^(n_raw|n_snap_elems|ratio_raw_over_stored|edd_nodes|bpi_edd|bpi_file|bpi_edd_over_stored|numZddVars|levels_used)=' "$BPI_LOG" || true
if [[ -f "$BPI_OPT_LOG" ]]; then
  echo "--- optimized ($HEUR) ---"
  grep -E '^(n_raw|n_snap_elems|ratio_raw_over_stored|edd_nodes|bpi_edd|bpi_file|bpi_edd_over_stored|numZddVars|levels_used)=' "$BPI_OPT_LOG" || true
fi
