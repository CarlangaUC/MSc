#!/usr/bin/env bash
# Wrapper: PNG de intervalos metatrie para un término.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DOCS="${1:?usage: viz_metatrie.sh <file.docs> <term_id> [out.png]}"
TERM="${2:?usage: viz_metatrie.sh <file.docs> <term_id> [out.png]}"
OUT="${3:-$ROOT/resultados_test/viz_metatrie_term${TERM}.png}"
exec python3 "$ROOT/scripts/viz_metatrie_intervals.py" --docs "$DOCS" --term "$TERM" --out "$OUT" "${@:4}"
