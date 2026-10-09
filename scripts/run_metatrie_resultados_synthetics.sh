#!/usr/bin/env bash
# Regenera y mide metatrie (per-term + global) sobre manifests de resultados_test.
# Uso: ./scripts/run_metatrie_resultados_synthetics.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
GEN="$ROOT/scripts/generar_docs_versionados_sinteticos.py"
META="$ROOT/edd_metatrie/meta_trie_edd"
OUT="$ROOT/resultados_test/synthetic_metatrie_run"
CSV="$OUT/metatrie_results.csv"
LOG="$OUT/run.log"
KEEP_DATASETS="${KEEP_DATASETS:-0}"
MIN_MEM_MB="${MIN_MEM_MB:-1500}"
VALIDATE_TERMS="${VALIDATE_TERMS:-20}"
MAX_POSTINGS="${MAX_POSTINGS:-260000000}"
MODES="${MODES:-per-term,global}"
# Por defecto: los 4 viables en ~6–8 GiB; el 2gb (~238M postings) va aparte si INCLUDE_2GB=1
INCLUDE_2GB="${INCLUDE_2GB:-0}"

mkdir -p "$OUT"
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
  echo "stem,mode,status,bpi_file,bpi_over_pairs,bpi_over_stored,bytes_total,n_raw,n_pairs_uniq,n_snap_elems,build_s,validation_mismatches,wall_s,note" > "$CSV"
fi

MANIFESTS=(
  "$ROOT/resultados_test/synthetic_var_uv_small_manifest.json"
  "$ROOT/resultados_test/synthetic_500mb_t10k_v100_manifest.json"
  "$ROOT/resultados_test/synthetic_toggle_500mb_manifest.json"
  "$ROOT/resultados_test/synthetic_var_uv_wiki_scale_manifest.json"
)
if [[ "$INCLUDE_2GB" == "1" ]]; then
  MANIFESTS+=("$ROOT/resultados_test/synthetic_2gb_var_uv_toggle_manifest.json")
fi

params_to_args() {
  python3 - "$1" <<'PY'
import json, sys
p = json.load(open(sys.argv[1])).get("params") or {}
args = []
args += ["--terms", str(p["terms"]), "--seed", str(p.get("seed", 20250920))]
u = int(p.get("universe_size") or p.get("universe_size_min") or 0)
umin = int(p.get("universe_size_min", u))
umax = int(p.get("universe_size_max", u))
vmin = int(p.get("versions_min", p.get("versions", 0)))
vmax = int(p.get("versions_max", p.get("versions", 0)))
if "versions" in p and vmin == 0:
    vmin = vmax = int(p["versions"])
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

stem_from_manifest() {
  python3 -c "import json,os,sys; d=json.load(open(sys.argv[1])); print(d.get('stem') or os.path.basename(d.get('docs','x')).replace('.docs',''))" "$1"
}

log "========== resultados_test synthetics START INCLUDE_2GB=$INCLUDE_2GB =========="
[[ -x "$META" ]] || { log "ERROR: falta $META"; exit 1; }

for mf in "${MANIFESTS[@]}"; do
  [[ -f "$mf" ]] || { log "MISSING $mf"; continue; }
  stem=$(stem_from_manifest "$mf")
  log ">>> $stem from $(basename "$mf")"
  if ! guard_memory; then
    for mode in ${MODES//,/ }; do
      echo "$stem,$mode,mem_guard,,,,,,,,,$(mem_available_mb),pre_gen" >> "$CSV"
    done
    continue
  fi
  args=$(params_to_args "$mf")
  # shellcheck disable=SC2086
  if ! python3 "$GEN" $args --stem "$stem" --output-dir "$OUT" --max-postings "$MAX_POSTINGS"; then
    for mode in ${MODES//,/ }; do
      echo "$stem,$mode,gen_fail,,,,,,,,,,," >> "$CSV"
    done
    continue
  fi
  docs="$OUT/${stem}.docs"
  for mode in ${MODES//,/ }; do
    t0=$SECONDS
    if ! guard_memory; then
      echo "$stem,$mode,mem_guard,,,,,,,,,$(mem_available_mb),pre_build" >> "$CSV"
      continue
    fi
    mlog="$OUT/${stem}_metatrie_${mode}.log"
    mcsv="$OUT/${stem}_metatrie_${mode}.csv"
    set +e
    "$META" "$mode" "$docs" --validate-terms "$VALIDATE_TERMS" --csv "$mcsv" > "$mlog" 2>&1
    rc=$?
    set -e
    wall=$((SECONDS - t0))
    if [[ $rc -ne 0 ]]; then
      echo "$stem,$mode,meta_fail_or_oom,,,,,,,,,$wall,rc=$rc" >> "$CSV"
      log "FAIL $stem/$mode rc=$rc wall=${wall}s"
      tail -n 15 "$mlog" || true
      continue
    fi
    parsed=$(parse_meta_out "$mlog")
    IFS=$'\t' read -r bpi_file bpi_pairs bpi_stored bytes_total n_raw n_pairs n_snap build_s mismatches <<< "$parsed"
    status=ok
    note=""
    if [[ -n "$mismatches" && "$mismatches" != "0" ]]; then
      status=validate_fail
      note="mismatches=$mismatches"
    fi
    echo "$stem,$mode,$status,$bpi_file,$bpi_pairs,$bpi_stored,$bytes_total,$n_raw,$n_pairs,$n_snap,$build_s,$mismatches,$wall,$note" >> "$CSV"
    log "OK $stem/$mode bpi_file=$bpi_file wall=${wall}s"
  done
  if [[ "$KEEP_DATASETS" != "1" ]]; then
    rm -f "$OUT/${stem}.docs" "$OUT/${stem}.docs.meta" "$OUT/${stem}.voc" \
      "$OUT/${stem}_manifest.json" "$OUT/${stem}_evol.csv" "$OUT/${stem}_terms.csv"
    log "cleanup $stem"
  fi
done

log "========== done CSV=$CSV =========="
