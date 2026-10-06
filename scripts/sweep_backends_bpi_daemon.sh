#!/usr/bin/env bash
# Daemon reanudable: relanza sweep hasta completar todos los backends.
# Sobrevive a OOM/kernel kill: al reiniciar, SKIP_EXISTING salta lo ya medido.
#
# Uso:
#   nohup ./scripts/sweep_backends_bpi_daemon.sh wiki_2gb marcado &
#   tail -f resultados_test/sweep_backends_wiki_2gb_marcado/daemon.log
#
# Variables:
#   WORKERS=1          limpieza streaming (default auto en wiki_2gb)
#   SLEEP_ON_FAIL=120  segundos entre reintentos tras fallo
#   MAX_ROUNDS=0       0 = ilimitado
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DATASET="${1:-wiki_2gb}"
NIVEL="${2:-marcado}"
SLEEP_ON_FAIL="${SLEEP_ON_FAIL:-120}"
SLEEP_ON_OK="${SLEEP_ON_OK:-5}"
MAX_ROUNDS="${MAX_ROUNDS:-0}"

LOGDIR="$ROOT/resultados_test/sweep_backends_${DATASET}_${NIVEL}"
DAEMON_LOG="$LOGDIR/daemon.log"
PIDFILE="$LOGDIR/daemon.pid"
LOCKFILE="$LOGDIR/daemon.lock"

mkdir -p "$LOGDIR"

if [[ -f "$PIDFILE" ]]; then
  old_pid="$(cat "$PIDFILE" 2>/dev/null || true)"
  if [[ -n "$old_pid" ]] && kill -0 "$old_pid" 2>/dev/null; then
    echo "[daemon] ya corre pid=$old_pid" | tee -a "$DAEMON_LOG"
    exit 0
  fi
fi

echo $$ > "$PIDFILE"
exec 9>"$LOCKFILE"
if ! flock -n 9; then
  echo "[daemon] otro daemon tiene el lock" | tee -a "$DAEMON_LOG"
  exit 0
fi

export WORKERS="${WORKERS:-1}"
export SKIP_EXISTING=1
export MALLOC_ARENA_MAX="${MALLOC_ARENA_MAX:-2}"

round=0
while true; do
  round=$((round + 1))
  if [[ "$MAX_ROUNDS" -gt 0 && "$round" -gt "$MAX_ROUNDS" ]]; then
    echo "[daemon] MAX_ROUNDS=$MAX_ROUNDS alcanzado" | tee -a "$DAEMON_LOG"
    break
  fi

  {
    echo "========== round=$round $(date -Is) =========="
    echo "[mem] $(free -h | awk '/^Mem:/ {printf "%s/%s used, %s avail", $3,$2,$7} /^Swap:/ {printf ", swap %s/%s", $3,$2}')"
  } | tee -a "$DAEMON_LOG"
  sync

  set +e
  "$ROOT/scripts/sweep_backends_bpi.sh" "$DATASET" "$NIVEL" >> "$DAEMON_LOG" 2>&1
  rc=$?
  set -e

  if [[ $rc -eq 0 ]]; then
    echo "[daemon] COMPLETO round=$round $(date -Is)" | tee -a "$DAEMON_LOG"
    "$ROOT/scripts/consolidar_sweep_backends.py" \
      "$ROOT/resultados_test/sweep_backends_${DATASET}_${NIVEL}.csv" \
      --out "$ROOT/resultados_test/sweep_backends_${DATASET}_${NIVEL}_REPORT.md" \
      2>&1 | tee -a "$DAEMON_LOG" || true
    break
  fi

  echo "[daemon] sweep rc=$rc; reintento en ${SLEEP_ON_FAIL}s ($(date -Is))" | tee -a "$DAEMON_LOG"
  sleep "$SLEEP_ON_FAIL"
done

rm -f "$PIDFILE"
echo "[daemon] fin $(date -Is)" | tee -a "$DAEMON_LOG"
