#!/usr/bin/env bash
# Reanuda pipeline desde paso 3 si ya existen .docs + .voc (evita re-uiHRDC).
# Uso: ./scripts/resume_backend_bpi.sh wiki_2gb marcado wtp
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DATASET="${1:?dataset}"
NIVEL="${2:?nivel}"
BACKEND="${3:?backend}"
HEUR="${HEUR:-nodes_desc+sift}"
HEUR_TAG="${HEUR//+/_}"
MAX_SIFT="${MAX_SIFT:-200}"

STEM="${DATASET}_limpio_${NIVEL}"
[[ "$BACKEND" != "regex" ]] && STEM="${STEM}_${BACKEND}"

DOCS="$ROOT/resultados_test/${STEM}_uihrdc_packed64.docs"
DOCS_META="${DOCS}.meta"
LISTAS="$ROOT/resultados_test/listas_${STEM}_versionada"
PAGE_MAP="$ROOT/resultados_test/page_mapping_${STEM}.bin"
VOC="$ROOT/resultados_test/index_${STEM}_named.voc"
PACK="$ROOT/resultados_test/${STEM}_plus_t.zpack"
PACK_OPT="$ROOT/resultados_test/${STEM}_${HEUR_TAG}.zpack"
EVOL="$ROOT/resultados_test/cudd_evolucion_${STEM}.csv"
BPI_BASE="$ROOT/resultados_test/bpi_${STEM}.log"
BPI_OPT="$ROOT/resultados_test/bpi_${STEM}_${HEUR_TAG}.log"
STATE="$ROOT/resultados_test/sweep_backends_${DATASET}_${NIVEL}/state.done"
LOGDIR="$ROOT/resultados_test/sweep_backends_${DATASET}_${NIVEL}"
RUNLOG="$LOGDIR/${BACKEND}_resume.log"

log_mem() {
  echo "[mem] $(date -Is) $* — $(free -h | awk '/^Mem:/ {print $3"/"$2" used, "$7" avail"}')"
}

[[ -f "$VOC" ]] || { echo "ERROR: falta $VOC"; exit 1; }

docs_ok() {
  [[ -f "$DOCS" && -s "$DOCS_META" ]] || return 1
  python3 - "$DOCS" <<'PY'
import struct, sys
p = sys.argv[1]
data = open(p, "rb").read()
total, = struct.unpack_from("<I", data, 0)
off = 4
for i in range(total):
    if off + 4 > len(data):
        sys.exit(1)
    ln, = struct.unpack_from("<I", data, off)
    off += 4 + ln * 8
    if off > len(data):
        sys.exit(1)
sys.exit(0 if off == len(data) else 1)
PY
}

exec >> "$RUNLOG" 2>&1
echo "========== RESUME $BACKEND $(date -Is) =========="
log_mem "inicio-resume"

if ! docs_ok; then
  echo "[WARN] .docs ausente o truncado; regenerando paso 2..."
  [[ -f "$LISTAS" ]] || { echo "ERROR: faltan listas $LISTAS (correr pipeline desde paso 1)"; exit 1; }
  rm -f "$DOCS" "$DOCS_META"
  python3 scripts/convertir_versionado_input_uiHRDC.py \
    --dataset "$STEM" \
    --input-listas "listas_${STEM}_versionada" \
    --base-texts "$ROOT/resultados_test" \
    --output-bin "$DOCS" \
    --tuple-output packed64 \
    --page-map "$PAGE_MAP"
  docs_ok || { echo "ERROR: .docs sigue invalido tras conversion"; exit 1; }
  echo "[OK] paso 2 regenerado: $DOCS"
fi

export LD_LIBRARY_PATH="$ROOT/cudd/cudd/.libs:${LD_LIBRARY_PATH:-}"
export MALLOC_ARENA_MAX="${MALLOC_ARENA_MAX:-2}"

if [[ ! -f "$PACK" ]]; then
  log_mem "pre-ZDD-build"
  echo "[3/6] build..."
  rm -f "$PACK" "$EVOL"
  "$ROOT/zdd_cudd_plus_t" build u+t "$DOCS" "$VOC" "$PACK" 0 "$EVOL" 500
fi

log_mem "post-build"
echo "[4/6] measure baseline..."
"$ROOT/scripts/measure_zpack_bpi" "$PACK" "$DOCS" | tee "$BPI_BASE"

if [[ ! -f "$PACK_OPT" ]]; then
  log_mem "pre-optimize"
  echo "[5/6] optimize $HEUR..."
  "$ROOT/zdd_cudd_plus_t" optimize u+t "$PACK" "$DOCS" "$PACK_OPT" "$HEUR" "$MAX_SIFT" 0
fi

log_mem "post-optimize"
echo "[6/6] measure optimized..."
"$ROOT/scripts/measure_zpack_bpi" "$PACK_OPT" "$DOCS" | tee "$BPI_OPT"

grep -qxF "$BACKEND" "$STATE" 2>/dev/null || echo "$BACKEND" >> "$STATE"
log_mem "fin-resume"
echo "[OK] resume $BACKEND completo"
