#!/usr/bin/env bash
# Wrapper: PNG Graphviz del trie time-first (EDD metatrie) para datasets pequeños.
#
#   ./dump_metatrie_dot.sh <file.docs> [term_id] [out.png]
#
# Recorre la estructura compacta (array de intervalos + temporal_wm), así que
# la figura refleja el índice que mide bpi_file.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/edd_metatrie/meta_trie_edd"
DOCS="${1:?usage: dump_metatrie_dot.sh <file.docs> [term_id] [out.png]}"
TERM="${2:-0}"
OUT="${3:-$ROOT/resultados_test/metatrie_term${TERM}.png}"
DOT="${OUT%.png}.dot"

"$BIN" per-term "$DOCS" --validate-terms 3 \
  --dump-dot "$DOT" --dump-term "$TERM" "${@:4}"
dot -Tpng -Gdpi=140 "$DOT" -o "$OUT"
echo "wrote $OUT"
