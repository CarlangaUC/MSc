# EDD metatrie (`meta_trie_edd`)

Índice **time-first** para conjuntos versionados `(master, rel)` en formato `.docs` packed64 (mismo input que el pipeline ZDD / uiHRDC). No modifica el monorepo [BGPs/bgps-temporal-graphs](../BGPs/bgps-temporal-graphs); reutiliza headers vía `-I`.

**Documentación para la tesis:** [`docs/METATRIE_TESIS.md`](../docs/METATRIE_TESIS.md) (modelo, **§4 pipeline función a función**, §3.3 parches, LaTeX, wiki 2 GB).

**Resultado canónico wiki 2 GB (2026-10-04, trie único):** `per-term` `bpi_file`≈5.996; `global`≈4.563; `validation_mismatches=0` con `--validate-terms 200`. Logs históricos con ST[OP] auxiliar: `meta_trie_edd_wiki_2gb_final_*` (incluían `bpi_query`; ya no aplica).

## Compilar

```bash
cd /root/MAGISTER/edd_metatrie
./build.sh
```

Requisitos: SDSL en `~/include` y `~/lib` (como el build de BGPs).

## Uso

```bash
./meta_trie_edd <global|per-term> <input.docs> \
  [--max-terms N] [--validate-terms N] [--no-rle] \
  [--csv out.csv] [--serialize out.emt]
```

| Modo | Índice | Consulta |
|------|--------|----------|
| `global` | Un `time_first_trie` sobre todos los quads `(t,u,[τ_a,τ_b))` | `values_at(t,τ)` vía `values_at_version` (payload `term→master`) |
| `per-term` | Un `time_first_trie` por término (payload solo `u`) | `values_at(t,τ)` sobre `per_term[t]` |

- **`--no-rle`**: un quad por posting (sin fusionar corridas de `rel`).
- Métricas: `bytes_total`, `bpi_file`, `bpi_total` (= `bpi_file`; mismo trie serializable en `.emt`); `bpi_over_pairs` / `bpi_over_stored` ([plus_t/utils/bpi.h](../plus_t/utils/bpi.h)).

## Relación con la tesis

Misma membresía \(M \subseteq U \times T\):

- **ZDD** (`zdd_cudd_plus_t`): snapshots \(S^t = \{u \mid (u,t)\in M\}\), sharing global en un DAG.
- **Esta EDD**: intervalos half-open por `(term, master)` tras RLE; barrido `generate_list_of_updates` → `temporal_wm` (CLTJ).

Dualidad documentada en el resumen MAGISTER: postings packed64 ↔ intervalos ↔ snapshots; consulta `(term, rel) → masters` debe coincidir con el ground truth RLE del `.docs`.

## Procedencia del código (BGPs → este archivo)

| Pieza | Origen | En `meta_trie_edd.cpp` |
|-------|--------|-------------------------|
| `temporal_wm`, `spot_quad` (64-bit masters) | Fork [cltj_temporal_wm_u64.hpp](cltj_temporal_wm_u64.hpp), [edd_cltj_types.hpp](edd_cltj_types.hpp); empaquetado [packed64_io.hpp](packed64_io.hpp) (40+24, `version_packing.h`) | Adaptado desde CLTJ; soporta masters &gt; 2³² |
| `generate_list_of_updates`, `create_time_first_trie` | [cltj_build_compact_tries.hpp](../BGPs/bgps-temporal-graphs/include/cltj_build_compact_tries.hpp) | Adaptado: orden fijo `(t1, term, master)`, `is_partial=false` |
| Trie time-first | [cltj_compact_trie_v3.hpp](../BGPs/bgps-temporal-graphs/include/cltj_compact_trie_v3.hpp) | Solo variante `TIME_FIRST_FULL_INTERVALS` → `time_first_trie` |
| Lector `.docs` / validación | [build-versioned-op.cpp](../BGPs/bgps-temporal-graphs/src/) (retirado) | `inspect_docs`, `expected_masters`, `validate_index` vs trie |

**No** se incluyen los 18 tries ni el grafo RDF completo. El auxiliar ST[OP] (`versioned_op_metatrie`) se **eliminó** del binario (oct 2026): consulta y validación usan solo el trie medido.

## Smoke tests

```bash
./meta_trie_edd per-term ../BGPs/toy_st_op.docs --validate-terms 3
./meta_trie_edd global  ../BGPs/toy_st_op.docs --validate-terms 3
./meta_trie_edd global  ../resultados_test/synthetic_var_uv_small.docs --max-terms 100 --validate-terms 20
./meta_trie_edd global  ../datos_sinteticos/S_t2000_v40_c40_u2097152.docs --validate-terms 20
```

Corpus grande (wiki 2 GB): usar `--max-terms` en `global`; el barrido único es más pesado que per-term.

## Visualización (PNG)

La metatrie interna (trie SDSL + `temporal_wm`) no se dibuja bien a mano; lo que sí es legible es la **vista intervalos** que el build usa tras RLE (misma semántica que `spot_quad`).

```bash
python3 scripts/viz_metatrie_intervals.py \
  --docs BGPs/toy_st_op.docs --term 0 \
  --out resultados_test/viz_toy_term0.png --also-snapshots

# Corpus grande: recortar masters
python3 scripts/viz_metatrie_intervals.py \
  --docs resultados_test/wiki_2gb_uihrdc_packed64.docs --term 100 \
  --max-masters 80 --out resultados_test/viz_wiki2gb_term100.png
```

Salida: barras horizontales \([τ_a, τ_b)\) por master; opcional `*_snapshots.png` (heatmap) si el término es pequeño.

### Dibujar la EDD (trie time-first) — `--dump-dot`

A diferencia del PNG de intervalos (que muestra la *membresía*), este dump recorre la
**estructura compacta** (array de intervalos + `temporal_wm` vía `values_at_version`),
así que la figura corresponde al índice que mide `bpi_file`.

```bash
python3 BGPs/make_micro_versioned_docs.py           # dataset mínimo con solapamientos
edd_metatrie/dump_metatrie_dot.sh BGPs/micro_metatrie.docs 0 \
  resultados_test/metatrie_micro_term0.png

# directo
./meta_trie_edd per-term ../BGPs/micro_metatrie.docs \
  --dump-dot out.dot --dump-term 0 [--dump-max-intervals N] [--dump-max-payload N]
dot -Tpng -Gdpi=140 out.dot -o out.png
```

Niveles: raíz → intervalos suelo `[τ_a, τ_b)` (con `last_update`) → payload (`u=master`;
en `global`, `term → master`). La leyenda muestra `tempint_left/right`, `last_update_pos`,
el bitvector `B` y los bytes del trie.

**Solo para datasets pequeños** (análogo a `zdd_cudd_plus_t demo`): wiki 2 GB tiene ~3 M
intervalos y el `.dot` sería inmanejable.

## Limitaciones

- Un solo índice en RAM/disco: **trie time-first** (`.emt`).
- **`validation_mismatches`**: trie (`values_at`) vs ground truth RLE del `.docs`.
- **Correcciones oct 2026 (parcheadas solo en este `.cpp`, monorepo BGPs intacto):** narrativa
  completa (original vs MAGISTER, síntomas, validación trie/vop, por qué local) en
  [`docs/METATRIE_TESIS.md`](../docs/METATRIE_TESIS.md) **§3.3**.
  1. `generate_list_of_updates` — OOB en \(\tau_{\max}\) → último suelo `[x,0)`.
  2. `edd::temporal_wm` — `leap`/`leftmost` (bit ajeno cuando `p - p_prime < 0`).
  3. `time_first_trie` — rebind de `m_last_update_select1` tras copy/move al vector `per_term[]`.
- `--debug-mismatches N` imprime los primeros N desacuerdos del trie (`term`, `rel`, `got`, `exp`).
