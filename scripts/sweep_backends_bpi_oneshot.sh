#!/usr/bin/env bash
# =============================================================================
# sweep_backends_bpi_oneshot.sh — un backend a la vez, CSV acumulativo, cleanup
# =============================================================================
# Dos fases (SWEEP_MODE):
#   baseline  — pasos 0→4 sin optimize; archiva zpack+.docs+.voc; ~15 min/backend
#   optimize  — solo optimize+measure desde archivo; ~100 min/backend
#
# Uso:
#   SWEEP_MODE=baseline ./scripts/sweep_backends_bpi_oneshot.sh wiki_2gb marcado
#   SWEEP_MODE=optimize  ./scripts/sweep_backends_bpi_oneshot.sh wiki_2gb marcado
#
# Variables:
#   SWEEP_MODE        baseline | optimize (default: baseline)
#   BACKENDS          lista csv
#   HEUR              nodes_desc+sift (solo fase optimize)
#   WORKERS=1         en wiki_2gb
#   MIN_MEM_MB=5120   umbral RAM antes de cada backend
#   KEEP_REGEX=1      no borra/archiva regex en cleanup final
#   KEEP_ARTIFACTS=1  no borra nada (debug)
#   SKIP_EXISTING=1   salta según CSV + state de la fase activa
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DATASET="${1:?dataset (wiki_100mb|wiki_1gb|wiki_2gb)}"
NIVEL="${2:?nivel (marcado|torsen)}"
SWEEP_MODE="${SWEEP_MODE:-baseline}"
HEUR="${HEUR:-nodes_desc+sift}"
HEUR_TAG="${HEUR//+/_}"
MAX_SIFT="${MAX_SIFT:-200}"
SKIP_EXISTING="${SKIP_EXISTING:-1}"
MIN_MEM_MB="${MIN_MEM_MB:-5120}"
KEEP_REGEX="${KEEP_REGEX:-0}"
KEEP_ARTIFACTS="${KEEP_ARTIFACTS:-0}"

if [[ "$DATASET" == wiki_2gb ]]; then
  WORKERS="${WORKERS:-1}"
else
  WORKERS="${WORKERS:-$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))}"
fi

DEFAULT_BACKENDS="regex,mwph,mwph_u2,wtp,wtp_u2,wikiextractor,pywikibot,html_stdlib,html_lxml,html_bs4,html5lib,html_selectolax,html_inscriptis,mwph_u2_html,wtp_u2_html,wikiextractor_html,mwph_u2_regex,wtp_u2_regex,wikiextractor_regex,html_regex"
BACKENDS="${BACKENDS:-$DEFAULT_BACKENDS}"

CSV="${CSV:-$ROOT/resultados_test/sweep_backends_${DATASET}_${NIVEL}_oneshot.csv}"
LOGDIR="$ROOT/resultados_test/sweep_backends_${DATASET}_${NIVEL}_oneshot"
STORE="$LOGDIR/baseline_store"
STATE_BASELINE="$LOGDIR/state.baseline.done"
STATE_OPTIMIZE="$LOGDIR/state.optimize.done"
LOCKFILE="$LOGDIR/oneshot.lock"
UIHRDC_POSTINGS="$ROOT/uiHRDC/uiHRDC/indexes/NOPOS/II_docs/postinglists.posts."*

SRC_TXT="$ROOT/uiHRDC/uiHRDC/data/texts/${DATASET}.txt"
EXPECTED_LINES="$(wc -l < "$SRC_TXT" 2>/dev/null || echo 0)"

mkdir -p "$LOGDIR" "$STORE"
export MALLOC_ARENA_MAX="${MALLOC_ARENA_MAX:-2}"
export LD_LIBRARY_PATH="$ROOT/cudd/cudd/.libs:${LD_LIBRARY_PATH:-}"

if [[ "$SWEEP_MODE" != "baseline" && "$SWEEP_MODE" != "optimize" ]]; then
  echo "ERROR: SWEEP_MODE debe ser baseline u optimize (got=$SWEEP_MODE)" >&2
  exit 1
fi

exec 9>"$LOCKFILE"
if ! flock -n 9; then
  echo "ERROR: otro sweep_backends_bpi_oneshot en curso (lock=$LOCKFILE)" >&2
  exit 1
fi

log_mem_disk() {
  echo "[mem] $(date -Is) $* — $(free -h | awk '/^Mem:/ {print $3"/"$2" used, "$7" avail"}')"
  echo "[disk] $(date -Is) $* — $(df -h "$ROOT/resultados_test" | awk 'NR==2 {print $3"/"$2" used, "$4" avail ("$5")"}')"
}

mem_avail_mb() { free -m | awk '/^Mem:/ {print $7}'; }

check_mem() {
  local avail
  avail="$(mem_avail_mb)"
  if [[ "$avail" -lt "$MIN_MEM_MB" ]]; then
    echo "ERROR: RAM insuficiente (${avail}Mi avail, need >=${MIN_MEM_MB}Mi)" >&2
    return 1
  fi
  return 0
}

stem_for() {
  local backend="$1"
  local stem="${DATASET}_limpio_${NIVEL}"
  [[ "$backend" != "regex" ]] && stem="${stem}_${backend}"
  printf '%s' "$stem"
}

paths_for_stem() {
  local stem="$1"
  DOCS="$ROOT/resultados_test/${stem}_uihrdc_packed64.docs"
  VOC="$ROOT/resultados_test/index_${stem}_named.voc"
  PACK="$ROOT/resultados_test/${stem}_plus_t.zpack"
  PACK_OPT="$ROOT/resultados_test/${stem}_${HEUR_TAG}.zpack"
  BPI_BASE="$ROOT/resultados_test/bpi_${stem}.log"
  BPI_OPT="$ROOT/resultados_test/bpi_${stem}_${HEUR_TAG}.log"
  OPT_LOG="$ROOT/resultados_test/optimize_${stem}_${HEUR_TAG}.log"
}

store_dir() { printf '%s/%s' "$STORE" "$1"; }

dedupe_csv() {
  [[ -f "$CSV" ]] || return 0
  python3 - "$CSV" <<'PY'
import csv, sys
from pathlib import Path
p = Path(sys.argv[1])
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

state_file() {
  [[ "$SWEEP_MODE" == "baseline" ]] && echo "$STATE_BASELINE" || echo "$STATE_OPTIMIZE"
}

is_done() {
  grep -qxF "$1" "$(state_file)" 2>/dev/null
}

mark_done() {
  local backend="$1"
  grep -qxF "$backend" "$(state_file)" 2>/dev/null || echo "$backend" >> "$(state_file)"
}

csv_has_stage() {
  local backend="$1" stage="$2"
  [[ -f "$CSV" ]] || return 1
  python3 - "$CSV" "$backend" "$stage" <<'PY'
import csv, sys
path, backend, stage = sys.argv[1:4]
with open(path, encoding="utf-8") as f:
    for row in csv.DictReader(f):
        if row.get("backend") == backend and row.get("stage") == stage:
            raise SystemExit(0)
raise SystemExit(1)
PY
}

should_skip() {
  local backend="$1"
  local stage="baseline"
  [[ "$SWEEP_MODE" == "optimize" ]] && stage="optimized"
  [[ "$SKIP_EXISTING" == "1" ]] && { is_done "$backend" || csv_has_stage "$backend" "$stage"; }
}

append_from_log() {
  local backend="$1" stage="$2" log="$3" pack="$4" note="${5:-}"
  python3 - "$backend" "$NIVEL" "$DATASET" "$stage" "$log" "$pack" "$CSV" "$note" <<'PY'
import csv, re, sys
from pathlib import Path

backend, nivel, dataset, stage, log, pack, csv_path, note = sys.argv[1:9]
text = Path(log).read_text(encoding="utf-8", errors="replace")

def grab(key):
    m = re.search(rf"^{re.escape(key)}=(\S+)", text, re.M)
    return m.group(1) if m else ""

def grab_clean_stats():
    m = re.search(
        r"bytes:\s*([\d,]+)\s*->\s*([\d,]+)\s*\(([\d.]+)% reduccion\)",
        text,
    )
    if not m:
        return "", "", ""
    return m.group(1).replace(",", ""), m.group(2).replace(",", ""), m.group(3)

b_in, b_out, red = grab_clean_stats()
row = {
    "backend": backend, "nivel": nivel, "dataset": dataset, "stage": stage,
    "pack": pack, "bpi_edd": grab("bpi_edd"), "bpi_file": grab("bpi_file"),
    "edd_nodes": grab("edd_nodes"), "n_raw": grab("n_raw"),
    "V": grab("terms_scanned"), "ratio_raw_over_stored": grab("ratio_raw_over_stored"),
    "bytes_in": b_in, "bytes_out": b_out, "reduccion_pct": red,
    "seconds_note": note,
}
path = Path(csv_path)
fieldnames = list(row.keys())
if path.exists():
    rows = list(csv.DictReader(path.open(encoding="utf-8")))
    if rows:
        fieldnames = list(rows[0].keys())
        for k in row:
            if k not in fieldnames:
                fieldnames.append(k)
    if any(r.get("backend") == backend and r.get("stage") == stage for r in rows):
        print(f"[csv] skip dup {backend}/{stage}")
        sys.exit(0)
else:
    path.write_text(",".join(fieldnames) + "\n", encoding="utf-8")
for k in fieldnames:
    row.setdefault(k, "")
with path.open("a", newline="", encoding="utf-8") as f:
    csv.DictWriter(f, fieldnames=fieldnames).writerow(row)
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
    echo "[cleanup] corpus parcial ${stem}.txt ($n/$EXPECTED_LINES) -> borrar"
    rm -f "$txt" "${txt}.DOCBOUNDARIES.ul"
  fi
}

cleanup_intermediates() {
  local stem="$1"
  rm -f "$ROOT/resultados_test/${stem}.txt"
  rm -f "$ROOT/resultados_test/${stem}.txt.DOCBOUNDARIES.ul"
  rm -f "$ROOT/resultados_test/listas_${stem}_versionada"
  rm -f "$ROOT/resultados_test/page_mapping_${stem}.bin"
  rm -f "$ROOT/resultados_test/cudd_evolucion_${stem}.csv"
  rm -f ${UIHRDC_POSTINGS}* 2>/dev/null || true
}

archive_baseline() {
  local backend="$1" stem="$2"
  local dir
  dir="$(store_dir "$backend")"
  mkdir -p "$dir"
  echo "[archive] $backend -> $dir"
  mv -f "$ROOT/resultados_test/${stem}_plus_t.zpack" "$dir/plus_t.zpack"
  mv -f "$ROOT/resultados_test/${stem}_uihrdc_packed64.docs" "$dir/uihrdc_packed64.docs"
  mv -f "$ROOT/resultados_test/${stem}_uihrdc_packed64.docs.meta" "$dir/uihrdc_packed64.docs.meta" 2>/dev/null || true
  mv -f "$ROOT/resultados_test/index_${stem}_named.voc" "$dir/named.voc"
  cp -f "$ROOT/resultados_test/bpi_${stem}.log" "$dir/bpi_baseline.log" 2>/dev/null || true
}

restore_baseline() {
  local backend="$1" stem="$2"
  local dir
  dir="$(store_dir "$backend")"
  [[ -d "$dir" ]] || return 1
  [[ -f "$dir/plus_t.zpack" && -f "$dir/uihrdc_packed64.docs" && -f "$dir/named.voc" ]] || return 1
  echo "[restore] $backend <- $dir"
  cp -f "$dir/plus_t.zpack" "$ROOT/resultados_test/${stem}_plus_t.zpack"
  cp -f "$dir/uihrdc_packed64.docs" "$ROOT/resultados_test/${stem}_uihrdc_packed64.docs"
  cp -f "$dir/uihrdc_packed64.docs.meta" "$ROOT/resultados_test/${stem}_uihrdc_packed64.docs.meta" 2>/dev/null || true
  cp -f "$dir/named.voc" "$ROOT/resultados_test/index_${stem}_named.voc"
  return 0
}

cleanup_all_backend() {
  local backend="$1" stem="$2"
  if [[ "$KEEP_ARTIFACTS" == "1" ]]; then
    echo "[cleanup] KEEP_ARTIFACTS=1 — no borro $backend"
    return 0
  fi
  if [[ "$KEEP_REGEX" == "1" && "$backend" == "regex" ]]; then
    echo "[cleanup] KEEP_REGEX=1 — conservo regex"
    return 0
  fi
  echo "[cleanup] borrando todo de backend=$backend"
  cleanup_intermediates "$stem"
  rm -f "$ROOT/resultados_test/index_${stem}_named.voc"
  rm -f "$ROOT/resultados_test/${stem}_uihrdc_packed64.docs"
  rm -f "$ROOT/resultados_test/${stem}_uihrdc_packed64.docs.meta"
  rm -f "$ROOT/resultados_test/${stem}_plus_t.zpack"
  rm -f "$ROOT/resultados_test/${stem}_${HEUR_TAG}.zpack"
  rm -f "$ROOT/resultados_test/bpi_${stem}.log"
  rm -f "$ROOT/resultados_test/bpi_${stem}_${HEUR_TAG}.log"
  rm -f "$ROOT/resultados_test/optimize_${stem}_${HEUR_TAG}.log"
  rm -rf "$(store_dir "$backend")"
  sync
  log_mem_disk "post-cleanup-$backend"
}

run_baseline() {
  local backend="$1"
  local stem="$2"
  local runlog="$3"
  local t0=$SECONDS
  set +e
  BACKEND="$backend" OPTIMIZE=0 HEUR="$HEUR" MAX_SIFT="$MAX_SIFT" WORKERS="$WORKERS" \
    "$ROOT/scripts/pipeline_limpieza_bpi.sh" "$DATASET" "$NIVEL" \
    2>&1 | tee "$runlog"
  local rc=${PIPESTATUS[0]}
  set -e
  local elapsed="$((SECONDS - t0))"
  if [[ $rc -ne 0 ]]; then
    echo "[FAIL] $backend baseline rc=$rc elapsed=${elapsed}s"
    cleanup_partial "$stem"
    return 1
  fi
  paths_for_stem "$stem"
  [[ -f "$BPI_BASE" && -f "$PACK" ]] || { echo "[FAIL] $backend sin pack/bpi baseline"; return 1; }
  append_from_log "$backend" baseline "$BPI_BASE" "$PACK" "${elapsed}s"
  archive_baseline "$backend" "$stem"
  cleanup_intermediates "$stem"
  rm -f "$BPI_BASE"
  mark_done "$backend"
  log_mem_disk "post-baseline-$backend"
  return 0
}

run_optimize() {
  local backend="$1"
  local stem="$2"
  local runlog="$3"
  paths_for_stem "$stem"

  if ! restore_baseline "$backend" "$stem"; then
    echo "[FAIL] $backend — sin baseline archivado en $(store_dir "$backend")" >&2
    return 1
  fi

  local t0=$SECONDS
  {
    echo "[5/6] zdd_cudd_plus_t optimize ($HEUR)..."
    log_mem_disk "pre-optimize-$backend"
    "$ROOT/zdd_cudd_plus_t" optimize u+t "$PACK" "$DOCS" "$PACK_OPT" "$HEUR" "$MAX_SIFT" 0
    echo "[6/6] measure_zpack_bpi (optimized)..."
    "$ROOT/scripts/measure_zpack_bpi" "$PACK_OPT" "$DOCS" | tee "$BPI_OPT"
  } 2>&1 | tee "$runlog"
  local elapsed="$((SECONDS - t0))"

  [[ -f "$BPI_OPT" && -f "$PACK_OPT" ]] || { echo "[FAIL] $backend optimize incompleto"; return 1; }
  append_from_log "$backend" optimized "$BPI_OPT" "$PACK_OPT" "${elapsed}s"
  mark_done "$backend"
  cleanup_all_backend "$backend" "$stem"
  return 0
}

if [[ ! -f "$CSV" ]]; then
  echo "backend,nivel,dataset,stage,pack,bpi_edd,bpi_file,edd_nodes,n_raw,V,ratio_raw_over_stored,bytes_in,bytes_out,reduccion_pct,seconds_note" > "$CSV"
fi
dedupe_csv
touch "$(state_file)"

IFS=',' read -r -a LIST <<< "$BACKENDS"

echo "=============================================="
echo " Sweep backends BPI — one-shot (SWEEP_MODE=$SWEEP_MODE)"
echo " dataset=$DATASET  nivel=$NIVEL  n=${#LIST[@]}  heur=$HEUR"
echo " workers=$WORKERS  expected_lines=$EXPECTED_LINES"
echo " csv=$CSV  store=$STORE"
echo " state=$(state_file)"
echo " MIN_MEM_MB=$MIN_MEM_MB"
echo "=============================================="
log_mem_disk "inicio"

DONE=0
SKIPPED=0
FAILED=0

for BACKEND in "${LIST[@]}"; do
  BACKEND="$(echo "$BACKEND" | xargs)"
  [[ -z "$BACKEND" ]] && continue

  STEM="$(stem_for "$BACKEND")"
  RUNLOG="$LOGDIR/${BACKEND}.${SWEEP_MODE}.log"

  if should_skip "$BACKEND"; then
    echo "[skip] $BACKEND ($SWEEP_MODE ya en csv/state)"
    mark_done "$BACKEND"
    SKIPPED=$((SKIPPED + 1))
    continue
  fi

  cleanup_partial "$STEM"

  if ! check_mem; then
    echo "[FAIL] $BACKEND — memoria baja; aborto sweep" >&2
    exit 2
  fi

  echo
  echo "========== BACKEND=$BACKEND mode=$SWEEP_MODE ($(date -Is)) =========="
  log_mem_disk "pre-$BACKEND"

  if [[ "$SWEEP_MODE" == "baseline" ]]; then
    if run_baseline "$BACKEND" "$STEM" "$RUNLOG"; then
      DONE=$((DONE + 1))
    else
      FAILED=$((FAILED + 1))
    fi
  else
    if run_optimize "$BACKEND" "$STEM" "$RUNLOG"; then
      DONE=$((DONE + 1))
    else
      FAILED=$((FAILED + 1))
    fi
  fi
  dedupe_csv
done

dedupe_csv
echo
echo "[OK] one-shot mode=$SWEEP_MODE: done=$DONE skipped=$SKIPPED failed=$FAILED"
echo "[OK] CSV -> $CSV"
echo "[OK] state -> $(state_file) ($(wc -l < "$(state_file)" 2>/dev/null || echo 0) backends)"
log_mem_disk "fin"

[[ $FAILED -gt 0 ]] && exit 2
exit 0
