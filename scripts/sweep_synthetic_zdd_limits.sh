#!/usr/bin/env bash
# Barrido sintético ZDD: techos de U, V y TAMAÑO del dataset + matriz de modos.
#
# Por qué el tamaño importa:
#   numZddVars ≈ maxMaster + nTerms   → techo “duro” de variables (U)
#   postings / edd_nodes              → techo de RAM durante el build (tamaño)
# Un probe chico (pocos postings) SOBREESTIMA el U seguro a escala wiki.
#
# Fases:
#   S  — escalera de TAMAÑO (terms×versions×card) con U,V fijos conservadores
#   A  — escalera U a una escala fija (SCALE o la última S OK)
#   B  — escalera V con U_SAFE a esa escala
#   C  — matriz modos (sparse|toggle × zipf|constant × U/V fijo|var) dentro de techos
#
# Guardas:
#   - MemAvailable mínima antes de cada build (MIN_MEM_MB, default 1500)
#   - --max-postings por SCALE (sin --force salvo FORCE=1)
#   - corte de escalera al primer zdd_oom_or_fail / mem_guard
#   - no reanuda fases fallidas automáticamente
#   - por defecto gen→build→medir→BORRAR artefactos pesados (KEEP_DATASETS=0)
#     y archiva recipe+manifest en recipes/ para poder replicar
#
# Uso:
#   PHASES=S SCALE=med ./scripts/sweep_synthetic_zdd_limits.sh
#   PHASES=S,A,B,C SCALE=med ./scripts/sweep_synthetic_zdd_limits.sh
#   PHASES=A SCALE=wiki U_LADDER=1048576,2097152,4194304 ./scripts/sweep_synthetic_zdd_limits.sh
#   KEEP_DATASETS=1  → no borrar .docs/.voc/.zpack (peligroso en ~8GiB)
#   SKIP_DONE=1      → saltar stems ya ok en sweep_results.csv
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-$ROOT/datos_sinteticos}"
GEN="$ROOT/scripts/generar_docs_versionados_sinteticos.py"
ZDD="$ROOT/zdd_cudd_plus_t"
MEASURE="$ROOT/scripts/measure_zpack_bpi"
CSV="$OUT/sweep_results.csv"
LOG="$OUT/sweep.log"
RECIPES_DIR="$OUT/recipes"
RECIPES_JSONL="$OUT/recipes.jsonl"
PHASES="${PHASES:-S}"
SEED="${SEED:-20250920}"
SCALE="${SCALE:-med}"          # smoke | med | wiki  (volumen objetivo en A/B/C)
FORCE="${FORCE:-0}"
MIN_MEM_MB="${MIN_MEM_MB:-2000}"
KEEP_DATASETS="${KEEP_DATASETS:-0}"
SKIP_DONE="${SKIP_DONE:-1}"
export PYTHONUNBUFFERED=1
export LD_LIBRARY_PATH="$ROOT/cudd/cudd/.libs:${LD_LIBRARY_PATH:-}"

mkdir -p "$OUT" "$RECIPES_DIR"
exec > >(tee -a "$LOG") 2>&1

log() { echo "[$(date -Is)] $*"; }

mem_available_mb() {
  awk '/MemAvailable:/ {printf "%d", $2/1024; exit}' /proc/meminfo
}

# Volumen objetivo aproximado (postings) y tope duro del generador.
scale_limits() {
  case "$SCALE" in
    smoke)
      TARGET_POSTINGS=5_000_000
      MAX_POSTINGS=20_000_000
      A_TERMS=800;  A_VERS=40;  A_CARD=40; A_WIN=64
      B_TERMS=800;  B_CARD=40;  B_WIN=64
      C_TERMS=800
      U_CONSERV=1048576
      V_CONSERV=40
      ;;
    med)
      TARGET_POSTINGS=50_000_000
      MAX_POSTINGS=120_000_000
      A_TERMS=8000; A_VERS=80;  A_CARD=50; A_WIN=128
      B_TERMS=8000; B_CARD=50;  B_WIN=128
      C_TERMS=8000
      U_CONSERV=2097152
      V_CONSERV=80
      ;;
    wiki)
      TARGET_POSTINGS=120_000_000
      MAX_POSTINGS=200_000_000
      A_TERMS=12000; A_VERS=100; A_CARD=58; A_WIN=128
      B_TERMS=12000; B_CARD=58;  B_WIN=128
      C_TERMS=12000
      U_CONSERV=2097152
      V_CONSERV=100
      ;;
    *)
      log "SCALE desconocida: $SCALE (smoke|med|wiki)"; exit 1
      ;;
  esac
  # bash no interpreta 50_000_000; normalizar
  TARGET_POSTINGS=${TARGET_POSTINGS//_/}
  MAX_POSTINGS=${MAX_POSTINGS//_/}
}

guard_memory() {
  local avail
  avail=$(mem_available_mb)
  if [[ "$avail" -lt "$MIN_MEM_MB" ]]; then
    log "GUARD mem: MemAvailable=${avail}MB < MIN_MEM_MB=${MIN_MEM_MB} — skip/abort"
    return 1
  fi
  log "GUARD mem: MemAvailable=${avail}MB OK"
  return 0
}

if [[ ! -f "$CSV" ]]; then
  echo "phase,stem,status,scale,terms,versions_spec,universe_spec,model,init,n_postings,bytes_docs,max_master,max_rel,numZddVars,edd_nodes,bpi_edd,wall_s,note" > "$CSV"
fi

csv_row() {
  # args: phase stem status scale terms vspec uspec model init post bytes maxm maxr nvars edd bpi wall note
  local IFS=,
  echo "$*" >> "$CSV"
}

already_ok() {
  local stem="$1"
  [[ "$SKIP_DONE" != "1" ]] && return 1
  [[ -f "$CSV" ]] || return 1
  awk -F, -v s="$stem" 'NR>1 && $2==s && $3=="ok" {found=1; exit} END{exit !found}' "$CSV"
}

# Guarda recipe reproducible + resumen; borra .docs/.voc/.zpack y logs pesados.
archive_and_cleanup() {
  local stem="$1" phase="$2" status="$3" wall="$4" note="$5"
  shift 5
  local args=("$@")
  local manif="$OUT/${stem}_manifest.json"
  local recipe="$RECIPES_DIR/${stem}.json"
  local cmdline
  cmdline=$(printf '%q ' python3 "$GEN" "${args[@]}" --stem "$stem" --output-dir "$OUT" --max-postings "$MAX_POSTINGS")

  python3 - "$recipe" "$RECIPES_JSONL" "$manif" "$stem" "$phase" "$status" "$wall" "$note" "$SCALE" "$SEED" "$cmdline" <<'PY'
import json, sys, os, time
recipe_path, jsonl_path, manif, stem, phase, status, wall, note, scale, seed, cmdline = sys.argv[1:12]
rec = {
    "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
    "stem": stem,
    "phase": phase,
    "status": status,
    "scale": scale,
    "seed": int(seed),
    "wall_s": int(wall) if str(wall).isdigit() else wall,
    "note": note,
    "cmdline": cmdline.strip(),
    "replicate": "KEEP_DATASETS=1 OUT=datos_sinteticos " + cmdline.strip(),
}
  if os.path.isfile(manif):
    with open(manif) as f:
        m = json.load(f)
    rec["params"] = m.get("params")
    rec["summary"] = m.get("summary")
with open(recipe_path, "w") as f:
    json.dump(rec, f, indent=2, sort_keys=True)
with open(jsonl_path, "a") as f:
    f.write(json.dumps(rec, sort_keys=True) + "\n")
print(recipe_path)
PY

  if [[ "$KEEP_DATASETS" == "1" ]]; then
    log "KEEP_DATASETS=1 — se conservan artefactos de $stem"
    return 0
  fi

  # pesados + auxiliares; recipes/ y CSV/log globales se quedan
  rm -f \
    "$OUT/${stem}.docs" "$OUT/${stem}.docs.meta" "$OUT/${stem}.voc" \
    "$OUT/${stem}_plus_t.zpack" \
    "$OUT/${stem}_evol.csv" "$OUT/${stem}_terms.csv" \
    "$OUT/${stem}_build.log" "$OUT/${stem}_bpi.log" \
    "$OUT/${stem}_manifest.json"
  log "cleanup $stem (datasets borrados; recipe → $recipe)"
}

run_one() {
  local phase="$1" stem="$2" note="$3"
  shift 3
  local t0=$SECONDS
  local docs="$OUT/${stem}.docs"
  local voc="$OUT/${stem}.voc"
  local pack="$OUT/${stem}_plus_t.zpack"
  local manif="$OUT/${stem}_manifest.json"
  local status="ok" bpi="" edd="" nvars="" post="" bytes="" maxm="" maxr=""
  local terms="" model="" init="" vspec="" uspec=""
  local force_flag=()
  local gen_args=("$@")
  local rc=0
  [[ "$FORCE" == "1" ]] && force_flag=(--force)

  if already_ok "$stem"; then
    log "SKIP $stem (ya ok en CSV; SKIP_DONE=$SKIP_DONE)"
    return 0
  fi

  log ">>> [$phase/$SCALE] $stem — $note"
  log "    args: $*"
  log "    MemAvailable=$(mem_available_mb)MB KEEP_DATASETS=$KEEP_DATASETS"

  # limpia restos previos del mismo stem (p.ej. crash a medias)
  rm -f \
    "$docs" "$OUT/${stem}.docs.meta" "$voc" "$pack" \
    "$OUT/${stem}_evol.csv" "$OUT/${stem}_terms.csv" \
    "$OUT/${stem}_build.log" "$OUT/${stem}_bpi.log" \
    "$OUT/${stem}_manifest.json" 2>/dev/null || true

  if ! guard_memory; then
    csv_row "$phase" "$stem" "mem_guard" "$SCALE" "" "" "" "" "" "" "" "" "" "" "" "" "$((SECONDS - t0))" "$note"
    archive_and_cleanup "$stem" "$phase" "mem_guard" "$((SECONDS - t0))" "$note" "${gen_args[@]}"
    return 3
  fi

  if ! python3 "$GEN" "${gen_args[@]}" \
      --stem "$stem" --output-dir "$OUT" \
      --max-postings "$MAX_POSTINGS" "${force_flag[@]}"; then
    status="gen_fail_or_guard"
    csv_row "$phase" "$stem" "$status" "$SCALE" "" "" "" "" "" "" "" "" "" "" "" "" "$((SECONDS - t0))" "$note"
    log "FAIL gen/guard $stem"
    archive_and_cleanup "$stem" "$phase" "$status" "$((SECONDS - t0))" "$note" "${gen_args[@]}"
    return 1
  fi

  post=$(python3 -c "import json;print(json.load(open('$manif'))['summary']['n_postings'])")
  bytes=$(python3 -c "import json;print(json.load(open('$manif'))['summary']['bytes_docs'])")
  maxm=$(python3 -c "import json;print(json.load(open('$manif'))['summary']['max_master'])")
  maxr=$(python3 -c "import json;print(json.load(open('$manif'))['summary']['max_rel'])")
  terms=$(python3 -c "import json;print(json.load(open('$manif'))['params']['terms'])")
  model=$(python3 -c "import json;print(json.load(open('$manif'))['params']['evolution_model'])")
  init=$(python3 -c "import json;print(json.load(open('$manif'))['params']['init_distribution'])")
  vspec=$(python3 -c "import json;d=json.load(open('$manif'));p=d['params'];print(f\"{p['versions_min']}-{p['versions_max']}\")")
  uspec=$(python3 -c "import json;d=json.load(open('$manif'));p=d['params'];print(f\"{p['universe_size_min']}-{p['universe_size_max']}\")")

  log "    gen OK postings=$post max_master=$maxm max_rel=$maxr bytes=$bytes"

  if ! guard_memory; then
    csv_row "$phase" "$stem" "mem_guard" "$SCALE" "$terms" "$vspec" "$uspec" "$model" "$init" \
      "$post" "$bytes" "$maxm" "$maxr" "" "" "" "$((SECONDS - t0))" "$note"
    archive_and_cleanup "$stem" "$phase" "mem_guard" "$((SECONDS - t0))" "$note" "${gen_args[@]}"
    return 3
  fi

  if ! "$ZDD" build u+t "$docs" "$voc" "$pack" 0 "$OUT/${stem}_evol.csv" 500 \
      > "$OUT/${stem}_build.log" 2>&1; then
    status="zdd_oom_or_fail"
    local wall=$((SECONDS - t0))
    csv_row "$phase" "$stem" "$status" "$SCALE" "$terms" "$vspec" "$uspec" "$model" "$init" \
      "$post" "$bytes" "$maxm" "$maxr" "" "" "" "$wall" "$note"
    echo "$phase,$stem,scale=$SCALE,max_master=$maxm,postings=$post,wall=$wall" >> "$OUT/failures.txt"
    log "FAIL ZDD $stem (OOM/error). Ver build log antes de cleanup."
    # captura tail del build log en recipe dir
    [[ -f "$OUT/${stem}_build.log" ]] && tail -n 40 "$OUT/${stem}_build.log" > "$RECIPES_DIR/${stem}_build_tail.txt" || true
    archive_and_cleanup "$stem" "$phase" "$status" "$wall" "$note" "${gen_args[@]}"
    return 2
  fi

  local meas
  meas=$("$MEASURE" "$pack" "$docs" 2>&1 | tee "$OUT/${stem}_bpi.log")
  bpi=$(echo "$meas" | awk -F= '/^bpi_edd=/{print $2; exit}')
  edd=$(echo "$meas" | awk -F= '/^edd_nodes=/{print $2; exit}')
  nvars=$(echo "$meas" | awk -F= '/^numZddVars=/{print $2; exit}')
  local wall=$((SECONDS - t0))

  csv_row "$phase" "$stem" "$status" "$SCALE" "$terms" "$vspec" "$uspec" "$model" "$init" \
    "$post" "$bytes" "$maxm" "$maxr" "$nvars" "$edd" "$bpi" "$wall" "$note"
  log "OK $stem bpi_edd=$bpi numZddVars=$nvars edd=$edd wall=${wall}s"
  archive_and_cleanup "$stem" "$phase" "$status" "$wall" "$note" "${gen_args[@]}"
  # métricas ZDD dentro del recipe (sin archivo extra)
  python3 -c "import json;p='$RECIPES_DIR/${stem}.json';d=json.load(open(p));d['results']={'bpi_edd':'$bpi','edd_nodes':'$edd','numZddVars':'$nvars','n_postings':'$post','bytes_docs':'$bytes'};json.dump(d,open(p,'w'),indent=2,sort_keys=True)"
  return 0
}

# ---------------------------------------------------------------------------
# Fase S: escalera de TAMAÑO (U,V fijos conservadores)
# ---------------------------------------------------------------------------
phase_S() {
  scale_limits
  log "========== FASE S: escalera TAMAÑO (U=$U_CONSERV V=$V_CONSERV) =========="
  # (terms, versions, card_mean) — volumen creciente; corta en primer OOM
  local S_LADDER="${S_LADDER:-2000:40:40,5000:60:50,8000:80:50,12000:100:58}"
  IFS=',' read -r -a STEPS <<< "$S_LADDER"
  local last_ok_terms="" last_ok_v="" last_ok_card=""
  for step in "${STEPS[@]}"; do
    IFS=':' read -r terms vers card <<< "$step"
    local stem="S_t${terms}_v${vers}_c${card}_u${U_CONSERV}"
    if run_one S "$stem" "size_ladder t=$terms v=$vers card=$card" \
      --terms "$terms" --versions "$vers" --universe-size "$U_CONSERV" \
      --init-distribution zipf --init-card-mean "$card" --init-card-min 2 --init-card-max 4000 \
      --zipf-alpha 1.05 \
      --evolution-model toggle --toggle-prob 0.01 --toggle-window 128 \
      --seed "$SEED"
    then
      last_ok_terms=$terms
      last_ok_v=$vers
      last_ok_card=$card
      echo "$terms:$vers:$card" > "$OUT/SIZE_SAFE.txt"
    else
      local rc=$?
      if [[ $rc -eq 2 || $rc -eq 3 ]]; then
        log "Techo TAMAÑO: fallo en t=$terms v=$vers card=$card (rc=$rc). Deteniendo S."
        echo "$terms:$vers:$card" > "$OUT/SIZE_FAIL_AT.txt"
        break
      fi
    fi
  done
  if [[ -n "$last_ok_terms" ]]; then
    log "[S] ultimo tamaño OK: terms=$last_ok_terms versions=$last_ok_v card=$last_ok_card → $OUT/SIZE_SAFE.txt"
  else
    log "[S] ningún tamaño OK con U=$U_CONSERV V fijo — bajar U_CONSERV o usar SCALE=smoke"
  fi
}

# ---------------------------------------------------------------------------
# Fase A: escalera U a escala fija (volumen ~ SCALE)
# ---------------------------------------------------------------------------
phase_A() {
  scale_limits
  # Si S dejó SIZE_SAFE, usarlo para terms/vers/card
  if [[ -f "$OUT/SIZE_SAFE.txt" ]]; then
    IFS=':' read -r A_TERMS A_VERS A_CARD < "$OUT/SIZE_SAFE.txt"
    log "Usando SIZE_SAFE de fase S: t=$A_TERMS v=$A_VERS card=$A_CARD"
  fi
  log "========== FASE A: escalera U @ SCALE=$SCALE (t=$A_TERMS v=$A_VERS card~$A_CARD) =========="
  local U_LADDER="${U_LADDER:-524288,1048576,2097152,4194304}"
  IFS=',' read -r -a US <<< "$U_LADDER"
  for u in "${US[@]}"; do
    local stem="A_u${u}_t${A_TERMS}_v${A_VERS}_${SCALE}"
    if run_one A "$stem" "U_ladder u=$u scale=$SCALE" \
      --terms "$A_TERMS" --versions "$A_VERS" --universe-size "$u" \
      --init-distribution zipf --init-card-mean "$A_CARD" --init-card-min 2 --init-card-max 4000 \
      --zipf-alpha 1.05 \
      --evolution-model toggle --toggle-prob 0.01 --toggle-window "$A_WIN" \
      --seed "$SEED"
    then
      echo "$u" > "$OUT/U_SAFE.txt"
    else
      local rc=$?
      if [[ $rc -eq 2 || $rc -eq 3 ]]; then
        log "Techo U @ SCALE=$SCALE: fallo en u=$u. Deteniendo A."
        echo "$u" > "$OUT/U_FAIL_AT.txt"
        break
      fi
    fi
  done
  if [[ -f "$OUT/U_SAFE.txt" ]]; then
    log "[A] U_SAFE=$(cat "$OUT/U_SAFE.txt")"
  fi
}

# ---------------------------------------------------------------------------
# Fase B: escalera V con U_SAFE
# ---------------------------------------------------------------------------
phase_B() {
  scale_limits
  if [[ -f "$OUT/SIZE_SAFE.txt" ]]; then
    IFS=':' read -r B_TERMS _ B_CARD < "$OUT/SIZE_SAFE.txt"
  fi
  local U_SAFE="${U_SAFE:-}"
  [[ -z "$U_SAFE" && -f "$OUT/U_SAFE.txt" ]] && U_SAFE=$(cat "$OUT/U_SAFE.txt")
  U_SAFE="${U_SAFE:-$U_CONSERV}"
  log "========== FASE B: escalera V @ SCALE=$SCALE U_SAFE=$U_SAFE t=$B_TERMS =========="
  local V_LADDER="${V_LADDER:-40,80,120,200,400}"
  IFS=',' read -r -a VS <<< "$V_LADDER"
  for v in "${VS[@]}"; do
    local stem="B_u${U_SAFE}_t${B_TERMS}_v${v}_${SCALE}"
    if run_one B "$stem" "V_ladder v=$v U=$U_SAFE scale=$SCALE" \
      --terms "$B_TERMS" --versions "$v" --universe-size "$U_SAFE" \
      --init-distribution zipf --init-card-mean "$B_CARD" --init-card-min 2 --init-card-max 4000 \
      --zipf-alpha 1.05 \
      --evolution-model toggle --toggle-prob 0.01 --toggle-window "$B_WIN" \
      --seed "$SEED"
    then
      echo "$v" > "$OUT/V_SAFE.txt"
    else
      local rc=$?
      if [[ $rc -eq 2 || $rc -eq 3 ]]; then
        log "Techo V @ SCALE=$SCALE: fallo en v=$v. Deteniendo B."
        echo "$v" > "$OUT/V_FAIL_AT.txt"
        break
      fi
    fi
  done
  if [[ -f "$OUT/V_SAFE.txt" ]]; then
    log "[B] V_SAFE=$(cat "$OUT/V_SAFE.txt")"
  fi
}

# ---------------------------------------------------------------------------
# Fase C: matriz de modos dentro de techos
# ---------------------------------------------------------------------------
phase_C() {
  scale_limits
  local U_SAFE="${U_SAFE:-}" V_SAFE="${V_SAFE:-}"
  [[ -z "$U_SAFE" && -f "$OUT/U_SAFE.txt" ]] && U_SAFE=$(cat "$OUT/U_SAFE.txt")
  [[ -z "$V_SAFE" && -f "$OUT/V_SAFE.txt" ]] && V_SAFE=$(cat "$OUT/V_SAFE.txt")
  if [[ -f "$OUT/SIZE_SAFE.txt" ]]; then
    IFS=':' read -r C_TERMS _ _ < "$OUT/SIZE_SAFE.txt"
  fi
  U_SAFE="${U_SAFE:-$U_CONSERV}"
  V_SAFE="${V_SAFE:-$V_CONSERV}"
  log "========== FASE C: matriz modos @ SCALE=$SCALE U=$U_SAFE V=$V_SAFE t=$C_TERMS =========="

  local U_LO=$((U_SAFE / 8))
  [[ $U_LO -lt 65536 ]] && U_LO=65536
  local V_LO=$((V_SAFE / 4))
  [[ $V_LO -lt 10 ]] && V_LO=10

  for model in sparse toggle; do
    for init in zipf constant; do
      local extra=()
      if [[ "$model" == "toggle" ]]; then
        extra=(--evolution-model toggle --toggle-prob 0.01 --toggle-window 128)
      else
        extra=(--evolution-model sparse --delete-prob 0.01 --add-ratio 0.01)
      fi
      if [[ "$init" == "zipf" ]]; then
        extra+=(--init-distribution zipf --init-card-mean 50 --init-card-min 2 --init-card-max 4000 --zipf-alpha 1.05)
      else
        extra+=(--init-distribution constant --init-card-mean 50 --init-card-min 50 --init-card-max 50)
      fi

      run_one C "C_${model}_${init}_fixed_${SCALE}_t${C_TERMS}" "fixed scale=$SCALE" \
        --terms "$C_TERMS" --versions "$V_SAFE" --universe-size "$U_SAFE" \
        --seed "$SEED" "${extra[@]}" || true

      run_one C "C_${model}_${init}_var_${SCALE}_t${C_TERMS}" "var within safe scale=$SCALE" \
        --terms "$C_TERMS" \
        --versions-min "$V_LO" --versions-max "$V_SAFE" \
        --universe-size-min "$U_LO" --universe-size-max "$U_SAFE" \
        --seed "$SEED" "${extra[@]}" || true
    done
  done
}

scale_limits
log "========== sweep start PHASES=$PHASES SCALE=$SCALE MAX_POSTINGS=$MAX_POSTINGS MIN_MEM_MB=$MIN_MEM_MB =========="
IFS=',' read -r -a PH <<< "$PHASES"
for ph in "${PH[@]}"; do
  case "$ph" in
    S|s) phase_S ;;
    A|a) phase_A ;;
    B|b) phase_B ;;
    C|c) phase_C ;;
    *) log "fase desconocida: $ph" ;;
  esac
done
log "========== done — CSV=$CSV =========="
