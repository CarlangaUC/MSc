#!/usr/bin/env bash
set -euo pipefail

BIN=/root/MAGISTER/BGPs/bgps-temporal-graphs/build/build-versioned-op
CSV=/root/MAGISTER/BGPs/resultados_st_op_n_tuple_components.csv
OUTDIR=/root/MAGISTER/BGPs/resultados_opmt
mkdir -p "$OUTDIR"

if [[ ! -s "$CSV" ]]; then
  echo "dataset,components,semantics,terms,n_raw,updates,bits_per_component,file_bytes,bpi_file_over_n_raw,build_seconds,wall_seconds,max_rss_kb,validation_terms,validation,roundtrip" > "$CSV"
fi

run_one() {
  local name="$1"
  local docs="$2"
  local n="$3"
  local val="${4:-500}"
  local out="$OUTDIR/${name}_n${n}.opmt"
  local log="/tmp/st_op_${name}_n${n}.log"
  local timef="/tmp/st_op_${name}_n${n}.time"
  local semantics
  if [[ "$n" == "1" ]]; then
    semantics="T=relative_payload=master"
  else
    semantics="T=relative_payload=master_relative"
  fi

  echo "===== $name n=$n ====="
  /usr/bin/time -f "WALL=%e RSS=%M" -o "$timef" "$BIN" "$n" "$docs" "$out" 0 "$val" | tee "$log"
  local wall rss
  wall=$(grep WALL= "$timef" | sed -E 's/.*WALL=([0-9.]+).*/\1/')
  rss=$(grep RSS= "$timef" | sed -E 's/.*RSS=([0-9]+).*/\1/')
  local terms nraw updates bits bytes bpi build valstat rt
  terms=$(grep -oP 'terms=\K[0-9]+' "$log" | head -1)
  nraw=$(grep -oP 'postings=\K[0-9]+' "$log" | head -1)
  bits=$(grep -oP 'bits/component=\K[0-9]+' "$log" | head -1)
  updates=$(grep -oP 'updates=\K[0-9]+' "$log" | tail -1)
  bytes=$(grep -oP 'bytes=\K[0-9]+' "$log" | tail -1)
  bpi=$(grep -oP 'bpi_file_over_n_raw=\K[0-9.]+' "$log" | tail -1)
  build=$(grep -oP 'build_s=\K[0-9.e+-]+' "$log" | tail -1)
  valstat=$(grep -oP 'validation=\K[A-Z]+' "$log" | tail -1)
  rt=$(grep -oP 'roundtrip=\K[A-Z]+' "$log" | tail -1)
  echo "$name,$n,$semantics,$terms,$nraw,$updates,$bits,$bytes,$bpi,$build,$wall,$rss,$val,$valstat,$rt" >> "$CSV"
  echo "CSV += $name n=$n bpi=$bpi rss_kb=$rss"
}

# Keep previous CSV as backup if it still has the inflated-bits n=1 rows.
cp -n "$CSV" "${CSV}.bak.pre_remeasure" 2>/dev/null || true
# Fresh file for this remeasure
echo "dataset,components,semantics,terms,n_raw,updates,bits_per_component,file_bytes,bpi_file_over_n_raw,build_seconds,wall_seconds,max_rss_kb,validation_terms,validation,roundtrip" > "$CSV"

run_one wiki_100mb /root/MAGISTER/resultados_test/wiki_100mb_uihrdc_packed64.docs 1 500
run_one wiki_100mb /root/MAGISTER/resultados_test/wiki_100mb_uihrdc_packed64.docs 2 500
run_one wiki_100mb_marcado /root/MAGISTER/resultados_test/wiki_100mb_limpio_marcado_uihrdc_packed64.docs 1 500
run_one wiki_100mb_marcado /root/MAGISTER/resultados_test/wiki_100mb_limpio_marcado_uihrdc_packed64.docs 2 500
run_one wiki_1gb /root/MAGISTER/resultados_test/wiki_1gb_uihrdc_packed64.docs 1 300
run_one wiki_1gb /root/MAGISTER/resultados_test/wiki_1gb_uihrdc_packed64.docs 2 200
run_one wiki_2gb /root/MAGISTER/resultados_test/wiki_2gb_uihrdc_packed64.docs 1 200
run_one wiki_2gb /root/MAGISTER/resultados_test/wiki_2gb_uihrdc_packed64.docs 2 100
