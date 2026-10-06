#!/usr/bin/env bash
# Sweep de backends de limpieza → BPI (baseline + nodes_desc+sift).
# Reanudable: SKIP_EXISTING=1 salta packs ya medidos.
#
# Uso:
#   ./scripts/sweep_backends_bpi.sh wiki_100mb marcado
#   WORKERS=1 ./scripts/sweep_backends_bpi.sh wiki_2gb marcado
#   ./scripts/sweep_backends_bpi_daemon.sh wiki_2gb marcado   # auto-reintento
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DATASET="${1:-wiki_100mb}"
NIVEL="${2:-marcado}"
HEUR="${HEUR:-nodes_desc+sift}"
HEUR_TAG="${HEUR//+/_}"
MAX_SIFT="${MAX_SIFT:-200}"
OPTIMIZE="${OPTIMIZE:-1}"
SKIP_EXISTING="${SKIP_EXISTING:-1}"
# 2 GB: 1 worker (streaming); uiHRDC+ZDD ya consumen ~6 GB RAM
if [[ "$DATASET" == wiki_2gb ]]; then
  WORKERS="${WORKERS:-1}"
else
  WORKERS="${WORKERS:-$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))}"
fi

CSV="${CSV:-$ROOT/resultados_test/sweep_backends_${DATASET}_${NIVEL}.csv}"
LOGDIR="$ROOT/resultados_test/sweep_backends_${DATASET}_${NIVEL}"
STATE="$LOGDIR/state.done"
mkdir -p "$LOGDIR"

DEFAULT_BACKENDS="regex,mwph,mwph_u2,wtp,wtp_u2,wikiextractor,pywikibot,html_stdlib,html_lxml,html_bs4,html5lib,html_selectolax,html_inscriptis,mwph_u2_html,wtp_u2_html,wikiextractor_html,mwph_u2_regex,wtp_u2_regex,wikiextractor_regex,html_regex"
BACKENDS="${BACKENDS:-$DEFAULT_BACKENDS}"

SRC_TXT="$ROOT/uiHRDC/uiHRDC/data/texts/${DATASET}.txt"
EXPECTED_LINES="$(wc -l < "$SRC_TXT" 2>/dev/null || echo 0)"

dedupe_csv() {
  [[ -f "$CSV" ]] || return 0
  python3 - "$CSV" <<'PY'
import csv, sys
from pathlib import Path
p = Path(sys.argv[1])
if not p.exists():
    raise SystemExit(0)
rows = list(csv.DictReader(p.open(encoding="utf-8")))
if not rows:
    raise SystemExit(0)
seen = set()
out = []
for r in rows:
    key = (r.get("backend", ""), r.get("stage", ""))
    if key in seen:
        continue
    seen.add(key)
    out.append(r)
with p.open("w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=rows[0].keys())
    w.writeheader()
    w.writerows(out)
print(f"[csv] dedupe -> {len(out)} filas")
PY
}

is_done() {
  local backend="$1"
  grep -qxF "$backend" "$STATE" 2>/dev/null
}

mark_done() {
  local backend="$1"
  grep -qxF "$backend" "$STATE" 2>/dev/null || echo "$backend" >> "$STATE"
}

append_from_log() {
  local backend="$1" stage="$2" log="$3" pack="$4"
  python3 - "$backend" "$NIVEL" "$DATASET" "$stage" "$log" "$pack" "$CSV" <<'PY'
import csv, re, sys
from pathlib import Path
backend, nivel, dataset, stage, log, pack, csv = sys.argv[1:8]
text = Path(log).read_text(encoding="utf-8", errors="replace")
def grab(key):
    m = re.search(rf"^{re.escape(key)}=(\S+)", text, re.M)
    return m.group(1) if m else ""
row = {
    "backend": backend, "nivel": nivel, "dataset": dataset, "stage": stage,
    "pack": pack, "bpi_edd": grab("bpi_edd"), "bpi_file": grab("bpi_file"),
    "edd_nodes": grab("edd_nodes"), "n_raw": grab("n_raw"),
    "V": grab("terms_scanned"), "ratio_raw_over_stored": grab("ratio_raw_over_stored"),
    "seconds_note": "",
}
path = Path(csv)
rows = []
if path.exists():
    rows = list(csv.DictReader(path.open(encoding="utf-8")))
    if any(r.get("backend") == backend and r.get("stage") == stage for r in rows):
        print(f"[csv] skip dup {backend}/{stage}")
        sys.exit(0)
else:
    path.write_text(
        "backend,nivel,dataset,stage,pack,bpi_edd,bpi_file,edd_nodes,n_raw,V,ratio_raw_over_stored,seconds_note\n",
        encoding="utf-8",
    )
with path.open("a", newline="", encoding="utf-8") as f:
    csv.DictWriter(f, fieldnames=row.keys()).writerow(row)
print(f"[csv] {backend}/{stage} bpi_edd={row['bpi_edd']}")
PY
}

cleanup_partial() {
  local stem="$1"
  local txt="$ROOT/resultados_test/${stem}.txt"
  [[ -f "$txt" ]] || return 0
  local n
  n="$(wc -l < "$txt")"
  if [[ "$EXPECTED_LINES" -gt 0 && "$n" -ne "$EXPECTED_LINES" ]]; then
    echo "[cleanup] corpus parcial ${stem}.txt ($n/$EXPECTED_LINES lineas) -> borrar"
    rm -f "$txt" "${txt}.DOCBOUNDARIES.ul"
  fi
}

backend_complete() {
  local backend="$1"
  local stem="$2"
  local pack_opt="$ROOT/resultados_test/${stem}_${HEUR_TAG}.zpack"
  local bpi_opt="$ROOT/resultados_test/bpi_${stem}_${HEUR_TAG}.log"
  [[ -f "$pack_opt" && -f "$bpi_opt" ]]
}

if [[ ! -f "$CSV" ]]; then
  echo "backend,nivel,dataset,stage,pack,bpi_edd,bpi_file,edd_nodes,n_raw,V,ratio_raw_over_stored,seconds_note" > "$CSV"
fi
dedupe_csv
touch "$STATE"

IFS=',' read -r -a LIST <<< "$BACKENDS"
echo "=============================================="
echo " Sweep backends BPI (reanudable)"
echo " dataset=$DATASET  nivel=$NIVEL  n=${#LIST[@]}  heur=$HEUR"
echo " workers=$WORKERS  expected_lines=$EXPECTED_LINES"
echo " csv=$CSV  state=$STATE"
echo "=============================================="

PENDING=0
DONE=0
FAILED=0

for BACKEND in "${LIST[@]}"; do
  BACKEND="$(echo "$BACKEND" | xargs)"
  [[ -z "$BACKEND" ]] && continue

  STEM="${DATASET}_limpio_${NIVEL}"
  if [[ "$BACKEND" != "regex" ]]; then
    STEM="${STEM}_${BACKEND}"
  fi
  PACK_OPT="$ROOT/resultados_test/${STEM}_${HEUR_TAG}.zpack"
  BPI_OPT="$ROOT/resultados_test/bpi_${STEM}_${HEUR_TAG}.log"
  RUNLOG="$LOGDIR/${BACKEND}.log"

  if is_done "$BACKEND" || { [[ "$SKIP_EXISTING" == "1" ]] && backend_complete "$BACKEND" "$STEM"; }; then
    echo "[skip] $BACKEND completo"
    mark_done "$BACKEND"
    append_from_log "$BACKEND" optimized "$BPI_OPT" "$PACK_OPT" 2>/dev/null || true
    DONE=$((DONE + 1))
    continue
  fi

  cleanup_partial "$STEM"
  PENDING=$((PENDING + 1))

  echo
  echo "========== BACKEND=$BACKEND ($(date -Is)) =========="
  set +e
  BACKEND="$BACKEND" HEUR="$HEUR" MAX_SIFT="$MAX_SIFT" OPTIMIZE="$OPTIMIZE" WORKERS="$WORKERS" \
    "$ROOT/scripts/pipeline_limpieza_bpi.sh" "$DATASET" "$NIVEL" \
    2>&1 | tee -a "$RUNLOG"
  rc=${PIPESTATUS[0]}
  set -e

  if [[ $rc -ne 0 ]]; then
    echo "[FAIL] $BACKEND rc=$rc ($(date -Is))"
    cleanup_partial "$STEM"
    FAILED=$((FAILED + 1))
    continue
  fi

  BPI_BASE="$ROOT/resultados_test/bpi_${STEM}.log"
  PACK_BASE="$ROOT/resultados_test/${STEM}_plus_t.zpack"
  [[ -f "$BPI_BASE" ]] && append_from_log "$BACKEND" baseline "$BPI_BASE" "$PACK_BASE"
  [[ -f "$BPI_OPT" ]] && append_from_log "$BACKEND" optimized "$BPI_OPT" "$PACK_OPT"
  mark_done "$BACKEND"
  DONE=$((DONE + 1))
  sync
  echo "[mem] post-${BACKEND}: $(free -h | awk '/^Mem:/ {print $3"/"$2" used, "$7" avail}')"
done

dedupe_csv
echo
echo "[OK] sweep: done=$DONE pending=$PENDING failed_this_run=$FAILED"
echo "[OK] CSV -> $CSV"
echo "[OK] state -> $STATE ($(wc -l < "$STATE") backends)"

# exit 2 si quedaron pendientes (util para el daemon)
if [[ $PENDING -gt $FAILED ]]; then
  exit 2
fi
exit 0
