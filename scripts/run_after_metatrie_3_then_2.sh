#!/usr/bin/env bash
# Tras barrido metatrie: (3) limpiar wiki regenerable → (2) parsers wiki_2gb baseline+optimize pendientes.
# Pensado para dejar corriendo horas (nohup).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
LOG="$ROOT/resultados_test/pipeline_after_metatrie.log"
exec >> "$LOG" 2>&1

log() { echo "[$(date -Is)] $*"; }

wait_metatrie() {
  log "Esperando fin de sweep_metatrie_from_recipes.sh ..."
  while pgrep -f 'scripts/sweep_metatrie_from_recipes.sh' >/dev/null; do
    sleep 60
    if [[ -f "$ROOT/datos_sinteticos/metatrie_results.csv" ]]; then
      python3 - <<'PY'
import csv, os, json
from collections import defaultdict
p="/root/MAGISTER/datos_sinteticos/metatrie_results.csv"
rows=list(csv.DictReader(open(p))) if os.path.isfile(p) else []
ok=[r for r in rows if r["status"]=="ok"]
m=defaultdict(set)
for r in ok: m[r["stem"]].add(r["mode"])
both=sum(1 for modes in m.values() if modes>={"per-term","global"})
exp=sum(1 for f in os.listdir("/root/MAGISTER/datos_sinteticos/recipes")
          if f.endswith(".json") and json.load(open(f"/root/MAGISTER/datos_sinteticos/recipes/{f}")).get("status")=="ok")
print(f"[poll] metatrie stems_completos={both}/{exp} filas_ok={len(ok)}")
PY
    fi
  done
  log "Metatrie sweep terminó."
}

step3_cleanup() {
  log "========== PASO 3: borrar datasets wiki/sintéticos regenerables en resultados_test =========="
  rm -f "$ROOT/resultados_test"/wiki_*_uihrdc_packed64.docs \
        "$ROOT/resultados_test"/wiki_*_uihrdc_packed64.docs.meta \
        "$ROOT/resultados_test"/wiki_*_plus_t.zpack \
        "$ROOT/resultados_test"/wiki_*_nodes_desc_sift.zpack \
        "$ROOT/resultados_test"/wiki_100mb.zpack \
        "$ROOT/resultados_test"/wiki_2gb_plus_t.zpack 2>/dev/null || true
  rm -f "$ROOT/resultados_test"/synthetic_*.docs "$ROOT/resultados_test"/synthetic_*.zpack \
        "$ROOT/resultados_test"/synthetic_*.voc 2>/dev/null || true
  rm -f "$ROOT/postinglists.posts."* 2>/dev/null || true
  rm -f "$ROOT/datos_sinteticos"/*.docs "$ROOT/datos_sinteticos"/*.voc \
        "$ROOT/datos_sinteticos"/*.docs.meta 2>/dev/null || true
  sync
  log "Paso 3 listo. CSV/logs/recipes metatrie y oneshot se conservan."
  df -h "$ROOT" | awk 'NR==2 {print "disk:", $3"/"$2, "avail", $4}'
  free -h | awk '/^Mem:/ {print "mem:", $3"/"$2, "avail", $7}'
}

step2_parsers() {
  log "========== PASO 2: wiki_2gb marcado — baseline+optimize backends pendientes =========="
  log "NOTA: baseline_store fue borrado antes; cada backend pendiente re-baseline (~15m) + optimize (~100m)."
  local LOGDIR="$ROOT/resultados_test/sweep_backends_wiki_2gb_marcado_oneshot"
  local STATE_OPT="$LOGDIR/state.optimize.done"
  local STATE_BASE="$LOGDIR/state.baseline.done"
  rm -f "$LOGDIR/oneshot.lock" 2>/dev/null || true

  # backends con baseline en state pero sin optimize
  mapfile -t PENDING < <(python3 - <<'PY'
import pathlib
root = pathlib.Path("/root/MAGISTER/resultados_test/sweep_backends_wiki_2gb_marcado_oneshot")
base = set()
opt = set()
for p in (root / "state.baseline.done", root / "state.done"):
    if p.is_file():
        base |= {l.strip() for l in p.read_text().splitlines() if l.strip()}
if (root / "state.optimize.done").is_file():
    opt = {l.strip() for l in (root / "state.optimize.done").read_text().splitlines() if l.strip()}
pending = sorted(base - opt)
print("\n".join(pending))
PY
)
  if [[ ${#PENDING[@]} -eq 0 ]]; then
    log "Ningún backend pendiente de optimize (state)."
    return 0
  fi
  log "Pendientes (${#PENDING[@]}): ${PENDING[*]}"

  export WORKERS=1
  export MIN_MEM_MB="${MIN_MEM_MB:-3200}"
  export HEUR="${HEUR:-nodes_desc+sift}"
  export MAX_SIFT="${MAX_SIFT:-200}"
  export SKIP_EXISTING=0

  for backend in "${PENDING[@]}"; do
    log "--- backend=$backend baseline ---"
    if ! SWEEP_MODE=baseline BACKENDS="$backend" \
      "$ROOT/scripts/sweep_backends_bpi_oneshot.sh" wiki_2gb marcado; then
      log "FAIL baseline $backend — sigo con el siguiente"
      continue
    fi
    log "--- backend=$backend optimize ---"
    if ! SWEEP_MODE=optimize BACKENDS="$backend" \
      "$ROOT/scripts/sweep_backends_bpi_oneshot.sh" wiki_2gb marcado; then
      log "FAIL optimize $backend"
    fi
  done
  log "Paso 2 terminado (revisar CSV y state.optimize.done)."
}

log "========== pipeline after metatrie START =========="
wait_metatrie
step3_cleanup
step2_parsers
log "========== pipeline after metatrie DONE =========="
