# EDD metatrie (`meta_trie_edd`)

Índice **time-first** para conjuntos versionados `(master, rel)` en formato `.docs` packed64 (mismo input que el pipeline ZDD / uiHRDC). No modifica el monorepo [BGPs/bgps-temporal-graphs](../BGPs/bgps-temporal-graphs); reutiliza headers vía `-I`.

**Guía explícita:** [`docs/METATRIE_GUIA_EXPLICITA.md`](../docs/METATRIE_GUIA_EXPLICITA.md)  
**Flujo end-to-end + funciones + comandos:** [`docs/METATRIE_FLUJO_END_TO_END.md`](../docs/METATRIE_FLUJO_END_TO_END.md)  
**Para el profesor (funciones + vs BGPs):** [`docs/METATRIE_CPP_PARA_PROFESOR.md`](../docs/METATRIE_CPP_PARA_PROFESOR.md) §11  
**Tesis:** [`docs/METATRIE_TESIS.md`](../docs/METATRIE_TESIS.md) (modelo, §4, parches §3.3, LaTeX).

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
  [--csv out.csv] [--serialize out.emt] \
  [--bench-queries N [--bench-reps R] [--bench-csv path] [--bench-queries-out path]]
```

| Modo | Índice | Consulta |
|------|--------|----------|
| `global` | Un `time_first_trie` sobre quads `(t,u,[τ_a,τ_b))`; WM 2 componentes | `values_at(t,τ)` → \(S^\tau\) (leap a \(t\), luego \(u\)) |
| `per-term` | Un `time_first_trie` por \(t\); WM 1 componente (\(u\)) | `values_at(t,τ)` sobre `per_term[t]` |

- **`--no-rle`**: un quad por posting (sin fusionar corridas de `rel`).
- Métricas: fórmula `NzddBpi::bpiFromBytes` de [`plus_t/utils/bpi.h`](../plus_t/utils/bpi.h) (`8×bytes/n`). Canónico vs ZDD: **`bpi_file`** con \(n_{\mathrm{raw}}\). `bpi_over_pairs` / `bpi_over_stored` usan pares `(u,τ)` únicos globales (no el `nPairsUniq` del audit ZDD).

## Benchmark de consulta \(Q_\tau\): \((t,\tau)\mapsto S^\tau\)

```bash
# Driver: metatrie + docs_scan + ZDD membresía (misma lista de queries)
./scripts/bench_query_tau.sh <input.docs> [global|per-term] [n_queries] [reps] [max_terms]
```

| Estructura | Qué mide | Semántica |
|------------|----------|-----------|
| **docs_scan** | `expected_masters` sobre posting list en RAM | Baseline \(Q_\tau\) sin índice |
| **metatrie** | `values_at(t,τ)` | \(Q_\tau\) nativa del trie |
| **zdd_qmem_baseline** | `Intersect(ZDD^t, S)` + `countSubsets` | **Otra consulta**: membresía \(S\in F_t\); bosque **sin optimize** (`.zpack` de `build`); \(S\) del `.docs` |

Flags útiles en `meta_trie_edd`: `--bench-queries N`, `--bench-reps R`, `--bench-csv`, `--bench-queries-out` (CSV `term,rel` para el ZDD).

ZDD (baseline, sin optimize):

```bash
./zdd_cudd_plus_t build u+t <docs> <voc> <out.zpack> <max_terms>
./zdd_cudd_plus_t bench-qmem u+t <docs> <queries.csv> --pack <out.zpack> [reps] [out.csv]
# o in-process: bench-qmem ... <max_terms> [reps] [out.csv]
```

Solo se cronometran queries con `term < V` del pack (las demás se saltan). Full wiki 2GB: `scripts/run_bench_wiki2gb_full.sh`.

**Wiki 2 GB — índice completo** (250 550 términos, 1000×5; BPI: global 4.563 / per-term 5.996). ZDD forest `max_terms=200`, baseline sin optimize (`*_zddbase_*.csv`, 2026-10-06):

| Estructura | n_queries | ns/query |
|------------|----------:|---------:|
| docs_scan | 1000 | ~5 000–5 140 |
| metatrie **global** | 1000 | ~2 806 |
| metatrie **per-term** | 1000 | ~2 112 |
| zdd_qmem_baseline (≠ \(Q_\tau\); terms &lt; 200) | 200 | ~130–190 |

CSV: `resultados_test/bench_query_tau_wiki_2gb_{global,per-term}_full_zddbase_*.csv`.

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
  --dump-dot out.dot --dump-term 0 [--dump-max-intervals N] [--dump-max-answer N]
dot -Tpng -Gdpi=140 out.dot -o out.png
```

Niveles: raíz → intervalos suelo `[τ_a, τ_b)` (con \(p_l\)) → \(S^\tau\) (`u=master`;
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
