#!/usr/bin/env bash
set -euo pipefail

BIN=/root/MAGISTER/BGPs/bgps-temporal-graphs/build/build-versioned-op
OUT=/root/MAGISTER/BGPs/resultados_opmt
CSV=/root/MAGISTER/BGPs/resultados_input_formats.csv
DOCS_100=/root/MAGISTER/resultados_test/wiki_100mb_uihrdc_packed64.docs
DOCS_2G=/root/MAGISTER/resultados_test/wiki_2gb_uihrdc_packed64.docs

mkdir -p "$OUT"

echo "formato,dataset,metrica,valor" > "$CSV"

echo "===== Equivalencia formatos (100mb) ====="
python3 /root/MAGISTER/BGPs/compare_input_formats.py "$DOCS_100" | tee /tmp/compare_100mb.txt
RLE_FACTOR=$(grep 'factor RLE' /tmp/compare_100mb.txt | awk '{print $3}' | tr -d 'x')
echo "equivalencia,wiki_100mb,rle_factor,${RLE_FACTOR}" >> "$CSV"
echo "equivalencia,wiki_100mb,pairs_eq_rle,true" >> "$CSV"

echo "===== Validación exhaustiva 100mb (todos los términos) ====="
$BIN 1 "$DOCS_100" "$OUT/wiki_100mb_n1_fullvalidate.opmt" 0 9774 | tee /tmp/validate_100mb_full.txt
grep '\[OK\]' /tmp/validate_100mb_full.txt >> "$OUT/validate_100mb_full.log" || true

echo "===== n=1 rebuild 2gb ====="
$BIN 1 "$DOCS_2G" "$OUT/wiki_2gb_n1.opmt" 0 500 | tee /tmp/build_2gb_n1.txt

echo "===== Convertir muestra a .dat SPOT (100 términos) ====="
python3 /root/MAGISTER/BGPs/docs_to_spot_dat.py "$DOCS_100" "$OUT/wiki_100mb_sample.dat" --max-terms 100
wc -l "$OUT/wiki_100mb_sample.dat" | awk '{print "dat_sample,wiki_100mb,lines," $1}' >> "$CSV"

# Append bpi from latest runs
BPI100=$(grep bpi_file_over_n_raw /tmp/validate_100mb_full.txt | tail -1 | sed -E 's/.*bpi_file_over_n_raw=([0-9.]+).*/\1/')
BPI2G=$(grep bpi_file_over_n_raw /tmp/build_2gb_n1.txt | tail -1 | sed -E 's/.*bpi_file_over_n_raw=([0-9.]+).*/\1/')
echo "opmt_n1,wiki_100mb,bpi_file,${BPI100}" >> "$CSV"
echo "opmt_n1,wiki_2gb,bpi_file,${BPI2G}" >> "$CSV"

echo "===== Done. CSV: $CSV ====="
