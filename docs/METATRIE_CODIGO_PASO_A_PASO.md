# Metatrie (`meta_trie_edd`) — código paso a paso

Documento **para estudio personal**: orden de llamadas, procedencia respecto al monorepo **BGPs** (CLTJ / paper Arroyuelo et al.) y qué es propio de MAGISTER.

**Código:** [`edd_metatrie/meta_trie_edd.cpp`](../edd_metatrie/meta_trie_edd.cpp)  
**Referencia upstream (clone local):** `BGPs/bgps-temporal-graphs/` → [darroyue/bgps-temporal-graphs](https://github.com/darroyue/bgps-temporal-graphs)  
**Paper:** [`docs/papers/2607.20356v1.pdf`](papers/2607.20356v1.pdf) (Worst-Case Optimal BGPs on Temporal Graphs)

**Importante:** MAGISTER **no enlaza** el repo BGPs al compilar. La lógica CLTJ está **copiada/adaptada** en `meta_trie_edd.cpp` + `cltj_temporal_wm_u64.hpp`. BGPs es la **referencia** para comparar nombres y algoritmos.

---

## Mapa rápido: MAGISTER ↔ BGPs

| Pieza en MAGISTER | Origen BGPs / paper | Archivo BGPs típico |
|-------------------|---------------------|----------------------|
| `spot_quad`, intervalos | `cltj::spot_quad` | `include/cltj_config.hpp` |
| RLE postings → quads | *(no igual)* grafo ya trae quads | — |
| `generate_list_of_updates` | Mismo algoritmo | `include/cltj_build_compact_tries.hpp` |
| `create_time_first_trie` | Mismo build | `include/cltj_build_compact_tries.hpp` |
| Índice con tries time-first | **18 órdenes**; vos usás **uno** | `include/cltj_index_temporal_metatrie.hpp` (~L114–147) |
| `temporal_wm`, `leap`, **B/E** | §6 Fig. 6–7 | `include/cltj_temporal_wm.hpp` |
| Consulta en τ | LTJ nivel tiempo §5.3 | `include/ltj_iterator_v2.hpp` |
| `.docs` packed64 | *(MAGISTER)* | — |

En BGPs, tras construir quads `D`:

```text
D_T = generate_list_of_updates(D)
m_tries[TSO] = create_time_first_trie(..., D, D_T, ...)   // ejemplo 2 componentes
```

En MAGISTER hacés **una** variante time-first por build (`global` o `per_term`), no el bosque de 18 tries.

---

## Notación (al leer el código)

| Símbolo / nombre | En código | Significado |
|------------------|-----------|-------------|
| `t` | índice de lista en `.docs` | Término del vocabulario |
| `u` / `master` | `unpack_master(packed)` | Documento lógico |
| `τ` / `rel` | `unpack_relative(packed)` | Versión |
| Quad | `edd::spot_quad` (5× u64) | `(t,u,0,τ_a,τ_b)` global; `(u,0,0,τ_a,τ_b)` per-term |
| Payload | bits en `temporal_wm` | `u` (1 comp.) o `(t,u)` (2 comp.) |
| `D` | `vector<spot_quad>` | Todos los intervalos antes del barrido |
| `D_T` | salida de `generate_list_of_updates` | Suelos + lista de updates por borde |

---

## Fase A — `main()` antes del build

```text
main()
  parse_args()                         → modo global|per-term, flags CLI
  inspect_docs(path, max_terms)        → offsets[], nlists, max_master, max_rel
```

| Función | ¿BGPs? | Por qué |
|---------|--------|---------|
| `inspect_docs` | Inspirado en tooling versioned-op (no en índice CLTJ) | Saber offset de cada posting list y rangos para `bits` del WM |
| `packed64_io.hpp` | uiHRDC `version_packing.h` (40+24) | Decodificar postings del índice invertido |

---

## Fase B — Build: `build_global` o `build_per_term`

### B1 — Por cada término

```text
build_global / build_per_term
  inspect_docs(...)                    [otra pasada dentro del build]
  read_posting_list(offset)            → vector<uint64> packed
  postings_to_quads_rle(...)           → append a D   [--no-rle → _point]
```

| Función | BGPs / paper | Por qué |
|---------|--------------|---------|
| `postings_to_quads_rle` | **Propio MAGISTER** | BGPs asume quads del grafo temporal; vos traducís `(u,τ)` a intervalos half-open |
| `postings_to_quads_rle_per_term` | Idem | Quita `t` del quad; término = índice del trie |

**Paper:** cada tupla temporal genera insert en `ti` y delete en `tf` (§5.2, Ej. 11). El RLE fusiona versiones consecutivas en un solo quad `[τ_a, τ_b)`.

### B2 — Barrido

```text
  generate_list_of_updates(D)          → D_T
```

**Algoritmo (igual que BGPs):**

1. Por cada quad, eventos en `τ_a` (apertura) y `τ_b` (cierre).
2. Ordenar extremos → **intervalos suelo** donde el conjunto activo es constante.
3. En cada borde: `update_type { índice_quad, is_delete }`.

**BGPs:** `cltj_build_compact_tries.hpp`, llamado en `cltj_index_temporal_metatrie.hpp` L114.

**Paper:** §5.1 / Fig. 5 — partición en `[ts_i, te_i)` (Ej. 9).

**Parche MAGISTER:** cierre del último suelo en τ máximo (evita OOB upstream; visible en wiki/micro global).

### B3 — Trie + wavelet

```text
  create_time_first_trie(D, D_T, bits, n_tuple_components, is_partial, payload_components)
    → time_first_trie
```

**Dentro de `create_time_first_trie` (port de `cltj_build_compact_tries.hpp`):**

1. `interval_list` → luego `m_tempint_left` / `m_tempint_right`.
2. Expandir updates de cada suelo a stream `new_D` + `is_delete`.
3. `last_update_per_interval`, `last_update_pos[i]`.
4. Ordenar (`comparator_time_first_payload`: τ suelo, t, u, …).
5. **`temporal_wm<>(sorted_D, sorted_del, …)`** — construye **B** y **E** (paper §6).
6. Empaquetar **`time_first_trie`**.

| Parámetro | `global` | `per-term` | Analogía BGPs |
|-----------|----------|------------|---------------|
| `n_tuple_components` | 2 | 1 | Orden tipo TSO (2 comp.) vs un eje |
| `payload_components` | 2 (t→u) | 1 (solo u) | Cuántos `leap` encadenar al consultar |
| `bits` | max(bits(t), bits(u)) | bits(u) | Ancho de codificación en WM |

**Fork WM:** [`edd_metatrie/cltj_temporal_wm_u64.hpp`](../edd_metatrie/cltj_temporal_wm_u64.hpp) (masters 64-bit; parches `leftmost`/`leap`).

**Clase trie:** variante `TIME_FIRST_FULL_INTERVALS` de `cltj_compact_trie_v3.hpp` → recortada como `time_first_trie` en un solo `.cpp`.

---

## Fase C — Métricas (post-build en `main`)

```text
  scan_docs_metrics(docs)              → n_raw, n_pairs_uniq (denominador BPI)
  index.size_report()                  → bytes tempint + WM
  print_size_report(...)
```

No viene de BGPs; alineado con espíritu de `plus_t/utils/bpi.h`.

---

## Fase D — Validación

```text
validate_index(index, docs_path, meta, K, ...)
  for term in 0 .. min(K, nlists):
    read_posting_list(...)
    validation_times(postings)         → muchos τ (bordes + vecinos)
    for τ in times:
      expected_triples_at / expected_masters
      index.values_at(term, τ)
      comparar → validation_mismatches
```

| Función | BGPs | Por qué |
|---------|------|---------|
| `expected_masters` | No en CLTJ para `.docs` | Ground truth con la misma regla RLE |
| `values_at` | Similar a evaluar snapshot en trie time-first | API tesis: `(t,τ) → S^τ` |

---

## Fase E — Consulta (`values_at` → `values_at_version`)

Implementa **§5.3** (intervalos en τ) + **§6.2** (navegar WM con prefijo `p_l`).

```text
values_at(term, version)
  global: global.values_at_version(τ)     luego filtra term
  per-term: per_term[t].values_at_version(τ)

values_at_version(τ):
  1. interval_seek(τ)                   → índice del suelo
  2. version_in_interval(pos, τ)
  3. get_last_update_of_interval(pos)   → p_l (último evento aplicable)
  4. get_temporal_root()                → [1, L] en el WM
  5. bucle temporal_successor → temporal_wm.leap(...)
     payload_components == 1: enumerar u
     payload_components == 2: leap t, luego leap u
```

| Paso | Paper / BGPs | Intuición |
|------|--------------|-----------|
| `interval_seek` | LTJ en nivel tiempo §5.3 | ¿En qué tramo constante cae τ? |
| `last_update` | “cada suelo guarda p_l ∈ [1,L]” §6.1 | Prefijo del stream hasta el estado en τ |
| `leap` | Sucesor / enumerar ≥ x | Payloads activos = Fig. 6 centro + WM |

BGPs expone esto vía iteradores LTJ; MAGISTER solo expone **snapshot por término**.

---

## Fase F — Opcionales en `main`

```text
  dump_trie_dot(trie, ...)             → Graphviz (suelos + payload)
  sdsl::store_to_file(index, ...)      → .emt
  CSV métricas
```

---

## Árbol de llamadas (build global)

```text
main
└─ build_global
   ├─ inspect_docs
   ├─ for each term:
   │    ├─ read_posting_list
   │    └─ postings_to_quads_rle → D
   ├─ generate_list_of_updates(D) → D_T          ← BGPs build_compact_tries
   └─ create_time_first_trie(D, D_T, …)          ← BGPs build_compact_tries
        ├─ ordenar updates time-first
        ├─ temporal_wm::temporal_wm(...)         ← cltj_temporal_wm (§6)
        └─ time_first_trie(...)                  ← compact_trie_v3 time-first
```

**Per-term:** el subárbol `D → D_T → create_time_first_trie` se repite **V veces** (un trie por término).

---

## Per-term vs global (misma semántica)

`values_at(t, τ)` debe coincidir con el `.docs` en ambos modos (validación `validation_mismatches = 0`).

| | global | per-term |
|---|--------|----------|
| Quads | `(t,u,0,τ_a,τ_b)` | `(u,0,0,τ_a,τ_b)` |
| Barrido | uno sobre todo `D` | uno por término |
| Payload WM | 2 (t, u) | 1 (u) |
| Estructuras | 1 `time_first_trie` | ~V tries |

---

## Qué es propio MAGISTER vs reuso teórico

| Propio MAGISTER | De BGPs / paper |
|-----------------|-----------------|
| `.docs`, RLE postings, `validate_index` | Quads, barrido, WM, suelos |
| `meta_trie_edd`, modos, `.emt`, BPI CLI | Idea de `cltj_index_temporal_metatrie` (1 de 18 tries) |
| `packed64_io`, parches OOB + u64 + `leap` | Algoritmo base en `cltj_temporal_wm.hpp` |

---

## Orden sugerido para leer el fuente

1. `main` (final del `.cpp`)
2. `build_global` / `build_per_term`
3. `postings_to_quads_rle`
4. `generate_list_of_updates` — comparar con `BGPs/.../cltj_build_compact_tries.hpp`
5. `create_time_first_trie`
6. Constructor `temporal_wm` en `cltj_temporal_wm_u64.hpp`
7. `values_at_version` + `leap`
8. `validate_index`

---

## Figuras y pipeline

| Artefacto | Ruta |
|-----------|------|
| Diagrama horizontal | [`edd_metatrie/diagram_meta_trie_edd_pipeline.dot`](../edd_metatrie/diagram_meta_trie_edd_pipeline.dot) → PNG en `resultados_test/` |
| Análogo Fig. 6 paper | `scripts/fig_metatrie_paper_fig6_style.py` |
| Doc tesis ampliada | [`METATRIE_TESIS.md`](METATRIE_TESIS.md) |

---

## Una frase por capa (memoria)

1. **Postings** → quads = intervalos de validez como en el grafo temporal.  
2. **Barrido** = Fig. 5 (suelos en τ).  
3. **`create_time_first_trie`** = Fig. 6–7 (un WM para todos los eventos).  
4. **`values_at_version`** = estado en τ vía `p_l` + `leap`.  
5. **Validación** = el `.docs` manda; el trie debe coincidir.

*Última actualización: 2026-10-05*
