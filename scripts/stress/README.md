# Stress Campaign Toolkit

Canonical stress inputs and runners for:
- uiHRDC (`BUILD_PFORDELTA_NOTEXT`, `SEARCH_PFORDELTA_NOTEXT`)
- TdZdd single-floor (`test`)
- TdZdd piso 1 (`scripts/cpp/test_tdzdd_piso1`)
- TdZdd piso 2 / CUDD piso 2 (`scripts/cpp/test_tdzdd_piso2 tdzdd|cudd`)
- future CUDD comparison (same dataset ladder and output schema)

## Dataset ladder

`dataset_ladder.json` defines the canonical dataset sequence and artifacts:
- non-versioned baseline: `torsen.text200mb`
- versioned ladder: `wiki_100mb`, `wiki_200mb`, `wiki_500mb`, `wiki_1gb`, `wiki_2gb`

Each dataset includes:
- source text for uiHRDC build
- index basename for stress runs
- patterns file for search
- exported `listas_*` path
- optional docs paths (`_global.docs`, `_packed.docs`)
- optional `page_mapping_*.bin`

## Stop rule

Default stop rule for stress runners:
- stop escalation when one job exceeds `--max-minutes` (default: 30)
- record the last stable dataset before threshold

This keeps the campaign aligned with local-machine practical limits.
