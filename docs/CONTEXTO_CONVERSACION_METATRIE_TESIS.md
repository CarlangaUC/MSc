# Contexto conversación — metatrie tesis

> Actualizado 2026-10-04 (índice único; ST[OP] auxiliar retirado)

## 5. Pipeline metatrie (`meta_trie_edd`)

### 5.2 Procedencia

Todo en **`edd_metatrie/meta_trie_edd.cpp`** (~1100 líneas), monorepo BGPs sin modificar:

| Pieza | Origen |
|--------|--------|
| `spot_quad`, `temporal_wm` (u64) | Fork `cltj_temporal_wm_u64.hpp`, `edd_cltj_types.hpp` |
| Barrido + trie | Adaptado de `cltj_build_compact_tries.hpp` |
| `time_first_trie` | `TIME_FIRST_FULL_INTERVALS` |
| Lector / validación | Inline en este `.cpp` |

ST[OP] (`versioned_op_metatrie`): **retirado** — ver [`edd_metatrie/archive/README.md`](../edd_metatrie/archive/README.md).

### 5.3 Flujo

1. `inspect_docs` → listas packed64 por término.
2. RLE → quads; `generate_list_of_updates` → suelos; `create_time_first_trie`.
3. **Consulta y validación:** `values_at(t,τ)` → `time_first_trie::values_at_version`.
4. Métricas: `bpi_file` = `bpi_total`; `.emt` = trie(s).

### 5.4 Modos

| Modo | Índice |
|------|--------|
| `per-term` | Un trie por término (payload $u$) |
| `global` | Un trie ($t \to u$) |

---

## 6. Wiki 2 GB

**Canónico índice único (2026-10-04):** `--validate-terms 200`, `validation_mismatches=0`.

| Modo | `bpi_file` | `bytes_total` |
|------|------------|---------------|
| per-term | 5.996 | 91 593 342 |
| global | 4.563 | 69 697 405 |

Logs: `resultados_test/meta_trie_edd_wiki_2gb_single_index_*.log`

Histórico con ST[OP] y `bpi_query`: `meta_trie_edd_wiki_2gb_final_*` (solo referencia; no citar `bpi_total` antiguo).

Detalle LaTeX: [`METATRIE_TESIS.md`](METATRIE_TESIS.md).

---

## 7. Limitaciones (tesis)

1. **Un índice:** trie time-first; validación sobre el mismo objeto que `bpi_file`.
2. **`bpi_over_stored` metatrie ≠ `n_snap_elems` ZDD** (denominadores distintos).
3. **Global wiki:** build ~45 s; validación 200 términos ~20 min wall.
4. Sin benchmarks de consulta ni updates incrementales.

Parches locales vs BGPs: `METATRIE_TESIS.md` §3.3.

---

## 8. LaTeX

Bloque completo acordado en chat oct 2026 (sección `\ref{sec:metatrie}` + sintético + resultados).
