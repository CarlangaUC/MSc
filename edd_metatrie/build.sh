#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${SCRIPT_DIR}"
g++ -std=c++17 -O3 -Wall -fpermissive \
  -I"${HOME}/include" \
  -I"$(dirname "$0")" \
  -I"${ROOT}/plus_t" \
  -I"${ROOT}/uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils" \
  -I"${ROOT}/BGPs/bgps-temporal-graphs/include" \
  -L"${HOME}/lib" \
  -o meta_trie_edd meta_trie_edd.cpp \
  -lsdsl -ldivsufsort -ldivsufsort64 -pthread
echo "Built ./meta_trie_edd"
