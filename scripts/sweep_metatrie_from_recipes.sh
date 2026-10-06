#!/usr/bin/env bash
# Barrido EDD metatrie sobre recipes del barrido ZDD.
# Flujo: regenerar .docs → medir (per-term + global) → validar muestra → BORRAR.
#
# Uso:
#   ./scripts/sweep_metatrie_from_recipes.sh
#   STEMS=S_t2000_v40_c40_u2097152,C_sparse_constant_fixed_med_t12000 ./scripts/...
#   MODES=per-term          # omitir global (más pesado)
#   SKIP_DONE=1 KEEP_DATASETS=0 MIN_MEM_MB=2000
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-$ROOT/datos_sinteticos}"
RECIPES_DIR="${RECIPES_DIR:-$OUT/recipes}"
GEN="$ROOT/scripts/generar_docs_versionados_sinteticos.py"
META="$ROOT/edd_metatrie/meta_trie_edd"
CSV="$OUT/metatrie_results.csv"
LOG="$OUT/metatrie_sweep.log"
MIN_MEM_MB="${MIN_MEM_MB:-2000}"
KEEP_DATASETS="${KEEP_DATASETS:-0}"
SKIP_DONE="${SKIP_DONE:-1}"
VALIDATE_TERMS="${VALIDATE_TERMS:-20}"
MAX_POSTINGS="${MAX_POSTINGS:-120000000}"
MODES="${MODES:-per-term,global}"
SEED_DEFAULT="${SEED_DEFAULT:-20250920}"
export PYTHONUNBUFFERED=1

mkdir -p "$OUT" "$RECIPES_DIR"
exec > >(tee -a "$LOG") 2>&1

log() { echo "[$(date -Is)] $*"; }

mem_available_mb() {
  awk '/MemAvailable:/ {printf "%d", $2/1024; exit}' /proc/meminfo
}

guard_memory() {
  local avail
  avail=$(mem_available_mb)
  if [[ "$avail" -lt "$MIN_MEM_MB" ]]; then
    log "GUARD mem: MemAvailable=${avail}MB < ${MIN_MEM_MB}"
    return 1
  fi
  log "GUARD mem: MemAvailable=${avail}MB OK"
}

if [[ ! -f "$CSV" ]]; then
  echo "stem,mode,status,zdd_bpi_edd,bpi_file,bpi_over_pairs,bpi_over_stored,bytes_total,n_raw,n_pairs_uniq,n_snap_elems,build_s,validation_mismatches,wall_s,note" > "$CSV"
fi

already_ok() {
  local stem="$1" mode="$2"
  [[ "$SKIP_DONE" != "1" ]] && return 1
  [[ -f "$CSV" ]] || return 1
  awk -F, -v s="$stem" -v m="$mode" 'NR>1 && $1==s && $2==m && $3=="ok" {found=1; exit} END{exit !found}' "$CSV"
}

params_to_args() {
  python3 - "$1" <<'PY'
import json, sys
p = json.load(open(sys.argv[1])).get("params") or {}
args = []
args += ["--terms", str(p["terms"]), "--seed", str(p.get("seed", 20250920))]
umin, umax = int(p["universe_size_min"]), int(p["universe_size_max"])
vmin, vmax = int(p["versions_min"]), int(p["versions_max"])
if umin == umax:
    args += ["--universe-size", str(umin)]
else:
    args += ["--universe-size-min", str(umin), "--universe-size-max", str(umax)]
if vmin == vmax:
    args += ["--versions", str(vmin)]
else:
    args += ["--versions-min", str(vmin), "--versions-max", str(vmax)]
model = p["evolution_model"]
args += ["--evolution-model", model]
if model == "toggle":
    args += ["--toggle-prob", str(p.get("toggle_prob", 0.01))]
    args += ["--toggle-window", str(p.get("toggle_window", 128))]
else:
    args += ["--delete-prob", str(p.get("delete_prob", 0.01))]
    args += ["--add-ratio", str(p.get("add_ratio", 0.01))]
init = p["init_distribution"]
args += ["--init-distribution", init]
args += ["--init-card-mean", str(p["init_card_mean"])]
args += ["--init-card-min", str(p["init_card_min"])]
args += ["--init-card-max", str(p["init_card_max"])]
if init == "zipf":
    args += ["--zipf-alpha", str(p.get("zipf_alpha", 1.05))]
    if "zipf_shift" in p:
        args += ["--zipf-shift", str(p["zipf_shift"])]
print(" ".join(args))
PY
}

parse_meta_out() {
  # stdin → imprime key=value de interés en una línea CSV-friendly via env vars names
  local out="$1"
  python3 - "$out" <<'PY'
import re, sys
text = open(sys.argv[1], errors="replace").read()
keys = [
  "bpi_file","bpi_over_pairs","bpi_over_stored","bytes_total",
  "n_raw","n_pairs_uniq","n_snap_elems","build_s","validation_mismatches"
]
vals = {}
for k in keys:
    m = re.search(rf"^{k}=(.+)$", text, re.M)
    vals[k] = m.group(1).strip() if m else ""
print("\t".join(vals[k] for k in keys))
PY
}

cleanup_stem() {
  local stem="$1"
  [[ "$KEEP_DATASETS" == "1" ]] && return 0
  rm -f \
    "$OUT/${stem}.docs" "$OUT/${stem}.docs.meta" "$OUT/${stem}.voc" \
    "$OUT/${stem}_manifest.json" "$OUT/${stem}_evol.csv" "$OUT/${stem}_terms.csv" \
    "$OUT/${stem}_metatrie_"*.log "$OUT/${stem}_metatrie_"*.csv
  log "cleanup $stem"
}

run_stem() {
  local recipe="$1"
  local stem phase zdd_bpi
  stem=$(python3 -c "import json;print(json.load(open('$recipe'))['stem'])")
  phase=$(python3 -c "import json;print(json.load(open('$recipe')).get('phase',''))")
  zdd_bpi=$(python3 -c "import json;d=json.load(open('$recipe'));r=d.get('results') or {};print(r.get('bpi_edd') or '')")
  local status_rec
  status_rec=$(python3 -c "import json;print(json.load(open('$recipe')).get('status',''))")
  if [[ "$status_rec" != "ok" ]]; then
    log "SKIP $stem (recipe status=$status_rec)"
    return 0
  fi

  IFS=',' read -r -a MODE_ARR <<< "$MODES"
  local need=0
  for mode in "${MODE_ARR[@]}"; do
    already_ok "$stem" "$mode" || need=1
  done
  if [[ "$need" -eq 0 ]]; then
    log "SKIP $stem (todos los modos ok en CSV)"
    return 0
  fi

  log ">>> metatrie $stem (phase=$phase zdd_bpi_edd=$zdd_bpi)"
  if ! guard_memory; then
    for mode in "${MODE_ARR[@]}"; do
      echo "$stem,$mode,mem_guard,$zdd_bpi,,,,,,,,,$(mem_available_mb),pre_gen" >> "$CSV"
    done
    return 3
  fi

  local args
  args=$(params_to_args "$recipe")
  # shellcheck disable=SC2086
  if ! python3 "$GEN" $args --stem "$stem" --output-dir "$OUT" --max-postings "$MAX_POSTINGS"; then
    for mode in "${MODE_ARR[@]}"; do
      echo "$stem,$mode,gen_fail,$zdd_bpi,,,,,,,,,,,gen" >> "$CSV"
    done
    cleanup_stem "$stem"
    return 1
  fi

  local docs="$OUT/${stem}.docs"
  for mode in "${MODE_ARR[@]}"; do
    if already_ok "$stem" "$mode"; then
      log "SKIP $stem/$mode"
      continue
    fi
    local t0=$SECONDS
    if ! guard_memory; then
      echo "$stem,$mode,mem_guard,$zdd_bpi,,,,,,,,,$(mem_available_mb),pre_build" >> "$CSV"
      continue
    fi
    local mlog="$OUT/${stem}_metatrie_${mode}.log"
    local mcsv="$OUT/${stem}_metatrie_${mode}.csv"
    local status="ok" note=""
    set +e
    "$META" "$mode" "$docs" \
      --validate-terms "$VALIDATE_TERMS" \
      --csv "$mcsv" \
      > "$mlog" 2>&1
    local rc=$?
    set -e
    local wall=$((SECONDS - t0))
    if [[ $rc -ne 0 ]]; then
      status="meta_fail_or_oom"
      note="rc=$rc"
      echo "$stem,$mode,$status,$zdd_bpi,,,,,,,,,$wall,$note" >> "$CSV"
      log "FAIL $stem/$mode rc=$rc wall=${wall}s (tail log)"
      tail -n 20 "$mlog" || true
      # archivar tail
      mkdir -p "$RECIPES_DIR"
      tail -n 40 "$mlog" > "$RECIPES_DIR/${stem}_metatrie_${mode}_tail.txt" || true
      continue
    fi
    local parsed
    parsed=$(parse_meta_out "$mlog")
    IFS=$'\t' read -r bpi_file bpi_pairs bpi_stored bytes_total n_raw n_pairs n_snap build_s mismatches <<< "$parsed"
    if [[ -n "$mismatches" && "$mismatches" != "0" ]]; then
      status="validate_fail"
      note="mismatches=$mismatches"
    fi
    echo "$stem,$mode,$status,$zdd_bpi,$bpi_file,$bpi_pairs,$bpi_stored,$bytes_total,$n_raw,$n_pairs,$n_snap,$build_s,$mismatches,$wall,$note" >> "$CSV"
    log "OK $stem/$mode bpi_file=$bpi_file (zdd_bpi_edd=$zdd_bpi) mismatches=$mismatches wall=${wall}s"
    # enriquecer recipe
    python3 - "$RECIPES_DIR/${stem}.json" "$mode" "$bpi_file" "$bpi_stored" "$bytes_total" "$mismatches" <<'PY' || true
import json, sys
path, mode, bpi_file, bpi_stored, bytes_total, mm = sys.argv[1:7]
d = json.load(open(path))
d.setdefault("metatrie", {})[mode] = {
    "bpi_file": bpi_file,
    "bpi_over_stored": bpi_stored,
    "bytes_total": bytes_total,
    "validation_mismatches": mm,
}
json.dump(d, open(path, "w"), indent=2, sort_keys=True)
PY
  done

  cleanup_stem "$stem"
  return 0
}

log "========== metatrie sweep start MODES=$MODES MIN_MEM_MB=$MIN_MEM_MB =========="
[[ -x "$META" ]] || { log "ERROR: falta $META (./edd_metatrie/build.sh)"; exit 1; }

mapfile -t RECIPE_FILES < <(
  if [[ -n "${STEMS:-}" ]]; then
    IFS=',' read -r -a SS <<< "$STEMS"
    for s in "${SS[@]}"; do
      f="$RECIPES_DIR/${s}.json"
      [[ -f "$f" ]] && echo "$f"
    done
  else
    # orden: S chicos primero, luego A/B/C (excluye fallidos vía status en run_stem)
    python3 - <<'PY'
import json, os
d="/root/MAGISTER/datos_sinteticos/recipes"
order_phase = {"S":0,"A":1,"B":2,"C":3}
items=[]
for f in os.listdir(d):
    if not f.endswith(".json") or f.endswith("_manifest.json"): continue
    path=os.path.join(d,f)
    try:
        rec=json.load(open(path))
    except Exception:
        continue
    if rec.get("status")!="ok": continue
    ph=rec.get("phase","Z")
    # ordenar por postings ascendente para fallar temprano en memoria
    post=0
    s=rec.get("summary") or {}
    r=rec.get("results") or {}
    post=int(s.get("n_postings") or r.get("n_postings") or 0)
    items.append((order_phase.get(ph,9), post, path))
for _,_,p in sorted(items):
    print(p)
PY
  fi
)

for recipe in "${RECIPE_FILES[@]}"; do
  run_stem "$recipe" || true
done

log "========== done — CSV=$CSV =========="
