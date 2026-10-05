#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
g++ -std=c++17 -O3 -Wall -fpermissive \
  -I"${HOME}/include" \
  -I"$(dirname "$0")" \
  -I"/root/MAGISTER/uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils" \
  -I"/root/MAGISTER/BGPs/bgps-temporal-graphs/include" \
  -L"${HOME}/lib" \
  -o meta_trie_edd meta_trie_edd.cpp \
  -lsdsl -ldivsufsort -ldivsufsort64 -pthread
echo "Built ./meta_trie_edd"
