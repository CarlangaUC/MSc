#!/usr/bin/env bash
# Generación sintética ~2 GiB (|U| y #versiones variables por término) + build ZDD;
# al terminar, relanza optimize formatos wiki_2gb.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
LOG="$ROOT/resultados_test/synthetic_2gb_var_then_optimize.log"
exec > >(tee -a "$LOG") 2>&1

log() { echo "[$(date -Is)] $*"; }

log "========== sintético 2GB variable (prioridad) =========="

export PYTHONUNBUFFERED=1
python3 "$ROOT/scripts/generar_docs_versionados_sinteticos.py" \
  --terms 25000 \
  --versions-min 80 \
  --versions-max 200 \
  --universe-size-min 262144 \
  --universe-size-max 16777216 \
  --init-distribution zipf \
  --init-card-mean 58 \
  --init-card-min 2 \
  --init-card-max 12000 \
  --zipf-alpha 1.05 \
  --evolution-model toggle \
  --toggle-prob 0.01 \
  --toggle-window 128 \
  --seed 20250916 \
  --stem synthetic_2gb_var_uv_toggle \
  --max-postings 400000000 \
  --force \
  --run-zdd

log "========== relanzando optimize wiki_2gb (sucio→marcado→torsen) =========="
# Sucio optimize quedó interrumpido: borrar salida parcial si existe.
rm -f "$ROOT/resultados_test/wiki_2gb_sucio_nodes_desc_sift.zpack"
export HEUR=nodes_desc+sift MAX_SIFT=200 FORCE=0
"$ROOT/scripts/run_optimize_formatos_wiki_2gb.sh"

log "========== pipeline completo =========="
