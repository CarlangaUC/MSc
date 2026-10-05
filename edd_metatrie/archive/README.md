# Archivo: ST[OP] auxiliar (`versioned_op_metatrie`)

**Retirado:** 2026-10-04

El binario `meta_trie_edd` construye **solo** `time_first_trie` (per-term o global).
Consulta, validación, `.emt` y `bpi_file` usan esa estructura.

## Qué había antes

- Clase inline `edd::versioned_op_metatrie` (~230 líneas) en `meta_trie_edd.cpp`:
  ST[OP] con `components=1`, un `temporal_wm` por término, consulta `values_at(t,τ)` sin suelos globales.
- Métricas `bpi_query` y `bpi_total = bpi_file + bpi_query`.
- Doble validación: `validation_mismatches` (vop) y `validation_trie_mismatches` (trie).

## Por qué se eliminó

Wiki 2 GB (`--validate-terms 200`): trie y vop daban la misma membresía; el trie ya implementa
`values_at_version`. El auxiliar duplicaba ~40–50 % RAM (per-term) sin cambiar `bpi_file`.

Corrida índice único: `resultados_test/meta_trie_edd_wiki_2gb_single_index_*.log`.

## Referencia histórica ST[OP] en BGPs

Ver [BGPs/DOC-ST-OP-VERSIONADO.md](../../BGPs/DOC-ST-OP-VERSIONADO.md) (experimento CLTJ; headers
`versioned_op_metatrie.hpp` retirados del monorepo BGPs).

No se guarda copia del `.cpp` inline aquí: nunca estuvo en git del repo padre (`edd_metatrie/` untracked hasta este commit).
