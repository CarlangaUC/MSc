#!/usr/bin/env bash
# Muestra progreso del sweep de parsers @ 2GB (visible en terminal).
# Uso: ./scripts/status_sweep.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATASET="${1:-wiki_2gb}"
NIVEL="${2:-marcado}"
OUT="$ROOT/SWEEP_STATUS.md"
LOGDIR="$ROOT/resultados_test/sweep_backends_${DATASET}_${NIVEL}"
CSV="$LOGDIR/../sweep_backends_${DATASET}_${NIVEL}.csv"
CSV="$ROOT/resultados_test/sweep_backends_${DATASET}_${NIVEL}.csv"
STATE="$LOGDIR/state.done"
SRC="$ROOT/uiHRDC/uiHRDC/data/texts/${DATASET}.txt"
EXPECTED="$(wc -l < "$SRC" 2>/dev/null || echo 0)"

TOTAL=20
DONE=0
[[ -f "$STATE" ]] && DONE=$(wc -l < "$STATE")

RUNNING="(idle)"
if pgrep -f "limpiar_corpus_wiki.py" >/dev/null 2>&1; then
  RUNNING="paso=0-limpieza ($(pgrep -af 'limpiar_corpus_wiki.py' 2>/dev/null | grep -oE 'backend [a-z0-9_+]+' | head -1 || echo 'backend=?'))"
elif pgrep -f "zdd_cudd_plus_t optimize" >/dev/null 2>&1; then
  RUNNING="paso=5-optimize (RAM+CPU alto ~1h)"
elif pgrep -f "zdd_cudd_plus_t build" >/dev/null 2>&1; then
  RUNNING="paso=3-ZDD-build (pico RAM)"
elif pgrep -f "BUILD_PFORDELTA_NOTEXT" >/dev/null 2>&1; then
  RUNNING="paso=1-uiHRDC (pico RAM)"
elif pgrep -f "pipeline_limpieza_bpi.sh ${DATASET}" >/dev/null 2>&1; then
  RUNNING="pipeline activo"
fi

MEM=$(free -h | awk '/^Mem:/ {printf "%s/%s used, %s avail", $3, $2, $7}')
SWAP=$(free -h | awk '/^Swap:/ {printf "%s/%s", $3, $2}')

{
  echo "# Sweep parsers wiki_2gb — estado"
  echo ""
  echo "**Actualizado:** $(date -Is)"
  echo ""
  echo "| | |"
  echo "|---|---|"
  echo "| Progreso | **${DONE}/${TOTAL}** backends completados |"
  echo "| Corriendo ahora | ${RUNNING:-(idle)} |"
  echo "| RAM | ${MEM} |"
  echo "| Swap | ${SWAP} |"
  echo "| Daemon log | \`resultados_test/sweep_backends_wiki_2gb_marcado/daemon.log\` |"
  echo ""
  echo "## Completados (bpi_edd optimized)"
  echo ""
  echo "| backend | bpi_edd |"
  echo "|---|---:|"
  if [[ -f "$CSV" ]]; then
    awk -F, '$4=="optimized" && $6!="" {print "| "$1" | "$6" |"}' "$CSV" | sort -u
  fi
  echo ""
  echo "## state.done"
  echo "\`\`\`"
  cat "$STATE" 2>/dev/null || echo "(vacío)"
  echo "\`\`\`"
  echo ""
  echo "## Limpieza en curso (lineas / ${EXPECTED})"
  echo "\`\`\`"
  for f in "$ROOT/resultados_test/${DATASET}_limpio_${NIVEL}"*.txt; do
    [[ -f "$f" ]] || continue
    n=$(wc -l < "$f")
    if [[ "$n" -lt "$EXPECTED" ]]; then
      echo "$(basename "$f"): $n"
    fi
  done
  echo "\`\`\`"
  echo ""
  echo "## Comandos"
  echo "\`\`\`bash"
  echo "tail -f resultados_test/sweep_backends_wiki_2gb_marcado/daemon.log"
  echo "./scripts/status_sweep.sh   # refrescar este archivo"
  echo "nohup ./scripts/sweep_backends_bpi_daemon.sh wiki_2gb marcado &  # reanudar tras crash"
  echo "\`\`\`"
} > "$OUT"

cat "$OUT"
