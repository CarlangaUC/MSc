# EDD metatrie — documentación para la tesis

> **Última actualización:** 2026-10-04 (§3.3 parches vs BGPs)  
> **Código:** [`edd_metatrie/meta_trie_edd.cpp`](../edd_metatrie/meta_trie_edd.cpp), [`edd_metatrie/README.md`](../edd_metatrie/README.md)  
> **Contexto ampliado del hilo:** [`CONTEXTO_CONVERSACION_METATRIE_TESIS.md`](CONTEXTO_CONVERSACION_METATRIE_TESIS.md)

---

## 1. Rol en MAGISTER

| Vista | Representación | Implementación |
|--------|----------------|----------------|
| **Snapshot** | \(S^\tau = \{u \mid (u,\tau)\in M_t\}\) por término | Backbone 2: `zdd_cudd_plus_t` (CUDD) |
| **Intervalo** | RLE por \((t,u)\): activo en \([\tau_a,\tau_b)\) | EDD metatrie: barrido CLTJ + `time_first_trie` |

**Entrada común:** `.docs` packed64 — `u64 = (master << 24) | rel`, cabecera `[u32 nlists][por lista: u32 len, u64…]`.

**Objetivo experimental:** misma membresía \(M\), comparar **BPI** (\(8\times\) bytes \(/\, n_{\mathrm{raw}}\)) y validar consultas \((t,\tau)\mapsto S^\tau\). No incluye (aún) latencia de consulta ni updates incrementales.

**Referencia teórica BGPs / CLTJ:** Arroyuelo et al., grafos temporales (quads half-open, `temporal_wm`, LTJ/`leap`).  
**SDSL / estructuras compactas:** Navarro & Ferrada, *Compact Data Structures* (si se cita el WM).

---

## 2. Notación

| Símbolo | Significado |
|---------|-------------|
| \(t\) | Índice de posting list en el `.docs` (vocabulario); \(t=0\) = primera lista |
| \(\tau\) | Versión (`rel` en packed64); **no** confundir con \(t\) |
| \(u\) | Master (documento) |
| \(M_t \subseteq U\times T\) | Postings del término \(t\) |
| \(S^\tau\) | Snapshot en \(\tau\) para un \(t\) fijo |
| Quad / `spot_quad` | \((t,u,0,\tau_a,\tau_b)\) global; \((u,0,0,\tau_a,\tau_b)\) per-term |
| Intervalo suelo | Tramo \([\tau_a,\tau_b)\) donde \(S^\tau\) es constante |

---

## 3. Teoría reutilizada vs aporte propio

### 3.1 Reuso (BGPs, sin tocar el monorepo)

| Pieza | Header BGPs |
|--------|-------------|
| `spot_quad`, config | `cltj_config.hpp` |
| `temporal_wm` (base) | `cltj_temporal_wm.hpp` |
| Barrido + build trie | Lógica de `cltj_build_compact_tries.hpp` (copiada/adaptada) |
| Variante trie | `TIME_FIRST_FULL_INTERVALS` (`cltj_compact_trie_v3.hpp`) |

**No reutilizado:** grafo RDF, 18 tries, evaluación BGP completa.

### 3.2 Aportes MAGISTER (oct 2026)

1. Pipeline único `.docs` → RLE → **`time_first_trie`** (consulta + `.emt`) + métricas `bpi_file` / `bpi_total` (desde oct 2026 ya no hay ST[OP] auxiliar en RAM).
2. Modos CLI `per-term` y `global`; serialización `.emt` (solo trie).
3. Validación ground truth + **`validation_trie_mismatches`** (trie medido).
4. Visualización: `scripts/viz_metatrie_intervals.py`, `--dump-dot`, `micro_metatrie.docs`.
5. **Parches locales:** ver §3.3 (detalle vs BGPs original).

---

### 3.3 Parches locales respecto al código BGPs/CLTJ original

El monorepo [`BGPs/bgps-temporal-graphs`](../BGPs/bgps-temporal-graphs) **no se modificó**. Los cambios viven solo en [`meta_trie_edd.cpp`](../edd_metatrie/meta_trie_edd.cpp). La **teoría** (RLE, suelos, WM, `leap`) es la del paper; los parches corrigen **casos borde** o **uso masivo de SDSL** en MAGISTER. **No cambian** el layout serializado del WM ni `bpi_file` en las corridas comparadas (mismos bytes, distinta corrección lógica / punteros).

#### Original vs port MAGISTER

| Original (BGPs) | En `meta_trie_edd.cpp` |
|-----------------|-------------------------|
| `cltj_temporal_wm.hpp`, `cltj_config.hpp` | `#include` sin editar |
| `generate_list_of_updates`, `create_time_first_trie` | Copiados/adaptados (orden payload, `is_partial=false`) |
| `compact_trie_v3` time-first | Clase recortada `time_first_trie` (`TIME_FIRST_FULL_INTERVALS`) |
| `versioned_op_metatrie.hpp`, `build-versioned-op` | Inline `edd::versioned_op_metatrie`; lector `.docs` |
| Grafo RDF, 18 tries | No portado |

Además (no es “fix”): pipeline `.docs`, modos `per-term`/`global`, BPI dual, validación cruzada, `--dump-dot`.

#### Parche 1 — `generate_list_of_updates` (barrido de extremos)

**Ubicación upstream:** `cltj_build_compact_tries.hpp` (función homónima).

**Comportamiento:** ordena extremos \(\tau_a,\tau_b\) de cada quad, agrupa coordenadas iguales, produce intervalos suelo \([e_{i-1},e_i)\) y la lista de inserts/deletes en cada borde.

**Bug upstream:** cuando el **último lote** de extremos iguales cae en la **coordenada temporal máxima**, el bucle deja `i == endpoints.size()` y el cierre del intervalo usa **`endpoints[i].ep`** (fuera de rango). El extremo derecho del último suelo queda **basura** (p. ej. `[4043,0)` en wiki, `[8,0)` en `micro_metatrie`).

**Efecto observable:**

- `tempint_right` incorrecto en el último tramo → `interval_seek(τ)` falla al final del eje.
- `values_at_version(τ)` **vacío** donde debería haber masters.
- Más visible en **`global`** (un barrido sobre todos los quads).
- **`vop_query`** no usa ese array de suelos → `validation_mismatches` podía ser **0** con **`validation_trie_mismatches` > 0**.

**Fix local** (`meta_trie_edd.cpp`, rama `batch_at_left`):

```cpp
const temporal_endpoint_type right =
    (i < endpoints.size()) ? endpoints[i].ep : endpoints[i - 1].ep;
interval.first.second = right;
```

Si no hay siguiente extremo distinto, cerrar en `endpoints[i-1].ep`.

**Reflejo tras el parche:** suelos bien formados; trie global consultable; `validation_trie_mismatches=0` (wiki final).

---

#### Parche 2 — `edd::temporal_wm` (`leftmost` / `leap`)

**Ubicación upstream:** `cltj_temporal_wm.hpp` (clase base incluida).

**Comportamiento:** el WM codifica el stream time-first de updates; **`leap`** devuelve el siguiente payload \(\ge x\) en el prefijo de estado \(p\); **`leftmost`** busca el mínimo en un subárbol cuando el bit pedido no existe a la izquierda.

**Bug upstream:** en `leftmost`, la condición del hijo izquierdo indexa `m_E[... + s + p - p_prime]`. Si **`p - p_prime < 0`**, el índice cae en **`s-1`** (bit **ajeno al nodo**). Si ese bit vale 1, se baja con `p = -1`, se devuelve `INFTY` y **no se prueba el hijo derecho** aunque el master exista ahí. En `leap`, ramas similares con `p_prime == 0`.

**Cuándo aparecía en MAGISTER:**

- Masters con valores altos en el codificado (p. ej. \(u \ge 2048\)) y consultas en el **primer update** de un suelo (`rel=0`).
- Wiki **per-term**, término 1: `got={}`, `exp={3261}`.
- Toy/micro y **`vop_query`** a menudo no lo disparaban → parecía un bug “solo del trie per-term”.

**Fix local:** subclase `edd::temporal_wm : cltj::temporal_wm<>` que **solo redefine** `leftmost` y `leap`:

- Guard `p_left = p - p_prime >= 0` antes de leer el hijo izquierdo.
- Fallback explícito al hijo derecho; en `leap`, checks `p_prime <= 0`, `r == 0`, etc.

Construcción, `m_B`/`m_E`, serialización: **heredadas** → mismos bytes que el WM base.

**Reflejo tras el parche:** enumeración completa vía `leap`; `--dump-dot` y validación trie coinciden con ground truth RLE.

---

#### Parche 3 — `time_first_trie` copy/move (SDSL)

**Contexto MAGISTER:** modo **`per-term`** asigna ~250k veces `per_term[term] = create_time_first_trie(...)` (copy/move al vector).

**Estructura afectada:** `m_last_update_per_int` + `m_last_update_select1` (`select_support_mcl<1>`). Tras `init_support`, el select **apunta** al bitvector sobre el que se construyó.

**Bug:** copy/move por defecto (patrón similar a `compact_trie_v3::copy` en BGPs, que copia `m_last_update_select1` **sin** `init_support` sobre el **nuevo** bitvector) deja el select enlazado al buffer **viejo** (destruido u otro trie).

**Efecto observable:**

- `--max-terms 20`: pocos copies → a veces 0 mismatches.
- Wiki completo per-term: **`validation_trie_mismatches=13`** en muestra; trie incoherente en algunos términos.
- **`vop_query`** no usa ese select → **`validation_mismatches=0`**.

**Fix local:** `copy_from` / `move_from` + `rebind_supports()` → `sdsl::util::init_support(m_last_update_select1, &m_last_update_per_int)` después de copiar/mover el bitvector.

**Reflejo tras el parche:** cada trie del vector consulta su propio `last_update`; wiki per-term **`validation_trie_mismatches=0`**.

---

#### Tabla resumen (síntomas → índice → parche)

| Síntoma | Causa | Índice afectado | Tras parche |
|--------|--------|-----------------|-------------|
| Último suelo `[x,0)`, consultas vacías al final de \(\tau\) | Barrido OOB | Trie (sobre todo **global**) | Suelos correctos |
| Faltan masters grandes en algunos \(\tau\) | WM `leftmost`/`leap` | Trie (mismo WM en build) | `leap` completo |
| Trie mal solo wiki full **per-term** | Select SDSL colgando | Trie **per-term** | Rebind en copy/move |
| `bpi_file`, tamaño `.emt` | — | — | Sin cambio de encoding |
| `bpi_query` | — | vop | Sin cambio de diseño |

#### Validación en el binario

- **`validation_mismatches`:** ground truth RLE vs **`vop_query.values_at(t,τ)`**.
- **`validation_trie_mismatches`:** vs **`time_first_trie.values_at_version`** (índice de **`bpi_file`** y **`--dump-dot`**).

Antes era posible **vop OK y trie mal**; corrida final wiki 2026-10-04 (`--validate-terms 200`): **ambos 0**.

#### ¿Por qué parchear localmente?

1. No alterar el monorepo BGPs ajeno.
2. Bugs 1–2 latentes upstream; el benchmark CLTJ original no ejercita igual **250k términos × wiki × global**.
3. Bug 3 es patrón **SDSL + vector masivo de tries**, específico del deployment MAGISTER.

Si upstream incorpora fixes equivalentes, se podría volver a `#include` puro y eliminar la subclase `edd::temporal_wm` y la copia local del barrido.

---

## 4. Pipeline completo: input → función → output (ambos modos)

**Entrada del binario:** `./meta_trie_edd <global|per-term> <file.docs> [flags]`

| Flag | Efecto |
|------|--------|
| `--max-terms N` | Solo primeras `N` listas en build (0 = todas) |
| `--no-rle` | Quads punto \([\tau,\tau+1)\) en lugar de RLE |
| `--validate-terms K` | Validar `vop` + trie en términos `0..K-1` |
| `--serialize out.emt` | Solo trie(s), sin `vop_query` |
| `--dump-dot` | Recorre trie medido (Graphviz) |
| `--csv` | Una fila BPI |

### Fase A — Común a ambos modos (pre-build)

| Paso | Función | Input | Por qué | Cómo | Output |
|------|---------|-------|---------|------|--------|
| A1 | `parse_args` | `argv` | Elegir modo y rutas | CLI | `cli_options` |
| A2 | `inspect_docs(path, max_terms)` | `.docs` binario | Saber cuántas listas, offsets, máximos | Lee `u32 nlists`; por lista guarda `tellg()` antes de `u32 len` + `len`×`u64`; opcionalmente escanea masters/rel | `docs_index`: `nlists`, `offsets[]`, `postings`, `max_master`, `max_relative` |
| A3 | `meta_trie_edd::build_*` | path + flags | Construir índices | Ver §4.1 / §4.2 | `index` en RAM |
| A4 | `scan_docs_metrics` | `.docs` | Denominadores BPI | Relee todas las listas; union de pares \((u,\tau)\) únicos | `docs_metrics`: `n_raw`, `n_pairs_uniq`, `n_snap_elems` |
| A5 | `size_report()` | `index` | Bytes trie + vop | Suma `size_bytes_breakdown()` | `index_size_report` |
| A6 | `print_size_report` | report + metrics | stdout BPI | `8×bytes/n_raw` | líneas `bpi_file`, `bpi_query`, `bpi_total`, … |
| A7 | `validate_index` | index, `.docs`, K | Correctitud **vop** | Por término `<K`: tiempos borde + `expected_masters` vs `values_at` | `validation_mismatches` |
| A8 | `validate_trie_index` | idem | Correctitud **trie** (`bpi_file`) | Mismo ground truth vs `trie_values_at` | `validation_trie_mismatches` |
| A9 | opcional | `--serialize`, `--dump-dot`, `--csv` | Persistencia / figura | SDSL / Graphviz | `.emt`, `.dot`, CSV |

**Formato `.docs` (input lógico):**

```
nlists: u32
repeat nlists veces:
  len: u32
  len × u64 packed,  packed = (master << 24) | rel
```

---

### 4.1 Modo `per-term` — bucle por término (trie aislado + vop por término)

**Función:** `meta_trie_edd::build_per_term(docs_path, max_terms, use_rle)`

| Paso | Función | Input | Por qué | Cómo | Output |
|------|---------|-------|---------|------|--------|
| P0 | `inspect_docs` | path | Metadatos | (igual A2) | `meta` |
| P1 | Init `vop_query` | `nlists`, `components=1`, `bits=bits_required(max_master)` | ST[OP] un WM por término, payload solo \(u\) | Constructor reserva `m_terms` | `vop_query` vacío |
| P2 | `per_term.resize(nlists)` | | Un slot por término | vector de tries | `per_term[]` |
| **Por cada** `term ∈ [0, nlists)` | | | | | |
| P3 | `read_posting_list` | offset en `.docs` | Cargar \(M_t\) crudo | seek + `len` + `u64[]` | `postings` |
| P4a | `vop_query.append_term(postings)` | postings | Índice consulta auxiliar | RLE → eventos `(time, master, _, ins/del)` → `term_index.build` → `temporal_wm` + `m_times`/`m_last_update` | `m_terms[term]` |
| P4b | RLE → trie branch | postings | Índice medido | Ver filas P5–P8 | `per_term[term]` |
| P5 | `postings_to_quads_rle_per_term` | postings | Intervalos half-open sin \(t\) en quad | Llama RLE con `term=0` y reescribe quad a `(u,0,0,τ_a,τ_b)` | `D` local (solo este término) |
| P5′ | (`--no-rle`) `postings_to_quads_point_per_term` | | Un quad por posting | \([\tau,\tau+1)\) | `D` |
| P6 | `generate_list_of_updates(D)` | quads del término | Suelos + updates | Sort extremos; barrido (parche OOB §3.3) | `D_T`: lista de `(intervalo_suelo, updates[])` |
| P7 | `create_time_first_trie(D, D_T, bits, n_comp=1, partial=false, payload=1)` | | WM time-first | Aplana updates por suelo; ordena; `temporal_wm(sorted, del, 1, bits)`; arrays `tempint_*`, `last_update_*` | `time_first_trie` |
| P8 | Asignación | | Guardar trie del término | `per_term[term] = trie` (copy/move + rebind select §3.3) | trie en vector |
| P9 | Acumula stats | | Diagnóstico | `raw_quads += |D|`, `update_events += …` | contadores globales |

**Consultas per-term (después del build):**

| API | Ruta | Input \((t,\tau)\) | Output |
|-----|------|---------------------|--------|
| `index.values_at(t, τ)` | `vop_query.values_at(t,τ)` | `state_position(τ)` → `leap` 1 nivel | `vector{(t,u)}` |
| `index.trie_values_at(t, τ)` | `per_term[t].values_at_version(τ)` | `interval_seek(τ)` → `last_update` → `leap` | `vector{(t,u)}` |
| `--dump-dot --dump-term t` | `dump_trie_dot(per_term[t], …)` | recorre suelos vía `values_at_version` | `.dot` |

**Por qué per-term:** cada \(M_t\) tiene su propio barrido → suelos mínimos para ese término; tries independientes → build paralelizable conceptualmente; `bpi_file` = suma de bytes de ~\(V\) tries.

---

### 4.2 Modo `global` — un barrido sobre todos los quads

**Función:** `meta_trie_edd::build_global(docs_path, max_terms, use_rle)`

| Paso | Función | Input | Por qué | Cómo | Output |
|------|---------|-------|---------|------|--------|
| G0 | `inspect_docs` | path | Metadatos | | `meta` |
| G1 | Init `vop_query` | `nlists`, `components=1`, `bits=max(bits(nlists-1), bits(max_master))` | Mismo ST[OP] por término (payload sigue siendo solo \(u\)) | Igual que per-term | `vop_query` |
| G2 | `D = []` global | | Acumular todos los quads | | vector vacío |
| **Por cada** `term` | | | | | |
| G3 | `read_posting_list` | offset | Postings del término | | `postings` |
| G4 | `vop_query.append_term(postings)` | | Auxiliar idéntico a per-term | | `m_terms[term]` |
| G5 | `postings_to_quads_rle(term, postings, D)` | | Quads con **\(t\)** explícito | RLE → `(t,u,0,τ_a,τ_b)` append a `D` | `D` crece |
| G5′ | (`--no-rle`) `postings_to_quads_point` | | | | |
| **Tras el bucle** | | | | | |
| G6 | `generate_list_of_updates(D)` | **todos** los quads | Suelos **compartidos** entre términos | Mismos extremos mezclados → más tramos | `D_T` |
| G7 | `create_time_first_trie(D, D_T, bits, n_comp=2, partial=false, payload=2)` | | Un solo WM; payload **term → master** | `temporal_wm` con 2 componentes; `values_at_version` hace 2× `leap` | `index.global` |
| G8 | | | | `raw_quads`, `update_events` | stats |

**Consultas global:**

| API | Ruta | Input | Output |
|-----|------|-------|--------|
| `index.values_at(t, τ)` | `vop_query` (igual per-term) | \((t,\tau)\) | `{(t,u)}` |
| `index.values_at(τ)` | concatena `vop` en todos los \(t\) | \(\tau\) | todos los `(t,u)` activos |
| `index.trie_values_at(τ)` | `global.values_at_version(τ)` | \(\tau\) | todos los `(t,u)` del trie |
| Validación filtro | en `validate_*` | | de `trie_values_at(τ)` se filtra `term==t` |
| `--dump-dot` | `dump_trie_dot(global, …)` | | figura multi-`term` |

**Por qué global:** un solo `temporal_wm` y un barrido → menos bytes (`bpi_file` menor en wiki); coste: build/validación sobre estructura única grande (wall ~45 min wiki con `--validate-terms 200`).

---

### 4.3 Detalle interno: `create_time_first_trie`

**Input:** quads originales `D`, suelos `D_T`, parámetros `bits`, `n_tuple_components`, `payload_components`.

| Subpaso | Qué hace | Output intermedio |
|---------|----------|-------------------|
| 1 | Por cada suelo, copia `D_T[i].second` (updates) a stream `new_D` con intervalo del suelo en quad | lista de updates etiquetados |
| 2 | Marca `last_update_per_interval` (bit 1 al final de cada batch de suelo) | `bit_vector B` |
| 3 | `last_update_pos[i]` = índice del último update del suelo \(i\) | `int_vector` |
| 4 | Ordena índices por `(τ_suelo, term, master, …)` | stream estable |
| 5 | `temporal_wm(sorted_D, is_delete, n_tuple_components, bits)` | WM serializable |
| 6 | Empaqueta en `time_first_trie` | objeto medido por `bpi_file` |

---

### 4.4 Detalle interno: consulta en \(\tau\) (trie medido)

**`time_first_trie::values_at_version(τ)`:**

1. `pos = interval_seek(τ)` — binaria en `[tempint_left, tempint_right)`.
2. Si \(\tau \notin\) suelo → vacío.
3. `l_update = last_update_pos[pos]` — prefijo del stream WM.
4. Si `payload_components == 1`: bucle `leap(0, …, cand)` → masters \(u\).
5. Si `== 2`: `leap` term, luego `leap` master bajo cada term.

**`versioned_op_metatrie::term_index::values_at(τ)`:**

1. `state_position(τ)` — binaria en `m_times` (tiempos de eventos).
2. `pos = m_last_update[…]` — mismo prefijo conceptual, **por término**.
3. `leap` con `components=1` → solo masters.

Misma semántica \(S^\tau\) para un \(t\) fijo; distinta **materialización** (suelos globales vs eventos por término).

---

### 4.5 Salidas finales del proceso

| Artefacto | Contenido | Modo |
|-----------|-----------|------|
| stdout | `mode`, bytes, BPI, `raw_quads`, `update_events`, `build_s`, validación | ambos |
| `.emt` | `mode`, metadatos, trie(s) SDSL | ambos; **sin** vop |
| `--csv` | una fila: `bpi_file`, `bpi_query`, `bpi_total`, `build_s` | ambos |
| RAM no serializada | `vop_query` completo | ambos |

---

## 4bis. Arquitectura (resumen)

```
inspect_docs → por término:
  ├─ vop_query.append_term(postings)     → ST[OP] n=1 (bpi_query)
  └─ postings → quads RLE → generate_list_of_updates → create_time_first_trie  (bpi_file)
     per-term: un barrido por término          global: un D global, un barrido
```

**Estado en \(\tau\):** no se materializa \(S^\tau\); `last_update` del suelo (trie) o `m_last_update` (vop) + prefijo WM; enumeración con `leap`.

### Modos

| | `per-term` | `global` |
|---|------------|----------|
| Quads | \((u,0,0,[\tau_a,\tau_b))\) | \((t,u,0,[\tau_a,\tau_b))\) |
| Tries | Uno por \(t\) | Uno para todo el corpus |
| Suelos (micro) | 4 (solo \(t=0\)) | Más finos (mezclan \(t=0,1,2\)) |

Figuras: `resultados_test/metatrie_micro_term0.png`, `metatrie_micro_global.png` → Overleaf `img/`.

---

## 5. Métricas (alineadas con `plus_t/utils/bpi.h`)

| Métrica | Qué mide | Persiste en disco |
|---------|----------|-------------------|
| `bpi_file` | `time_first_trie`(s) | Sí (`.emt`) |
| `bpi_total` | Igual a `bpi_file` (índice único desde oct 2026) | — |

`bpi_over_stored` del binario metatrie **≠** `n_snap_elems` del escaneo ZDD.

---

## 6. Resultado canónico wiki 2 GB (índice único, 2026-10-04)

**Dataset:** `resultados_test/wiki_2gb_uihrdc_packed64.docs`  
**\(n_{\mathrm{raw}} = 122\,197\,799\)**  
**Comando:** `meta_trie_edd {per-term|global} … --validate-terms 200`

| Modo | bytes trie | `bpi_file` | `validation_mismatches` | build_s | wall (validación) |
|------|------------|------------|---------------------------|---------|-------------------|
| per-term | 91 593 342 | **5.996** | **0** | ~12–15 s | ~35 s |
| global | 69 697 405 | **4.563** | **0** | ~45 s | ~20 min |

**Artefactos:** `resultados_test/meta_trie_edd_wiki_2gb_single_index_*.{log,csv}`

Histórico con ST[OP] auxiliar (`bpi_query`, `bpi_total`≈2×): `meta_trie_edd_wiki_2gb_final_*`.

---

## 7. Limitaciones (tesis)

1. Comparación ZDD–metatrie por **tamaño** bajo \(n_{\mathrm{raw}}\); no latencia.
2. Un índice: consulta = trie serializable; `bpi_total` = `bpi_file`.
3. `global` a escala wiki: validación 200 términos domina el wall time.
4. Validación en muestra (`--validate-terms 200`); corrida `single_index` oct 2026.

---

## 8. Bloque LaTeX — sección metatrie (copiar a Overleaf)

```latex
\section{Índice EDD \emph{time-first} (\texttt{meta\_trie\_edd})}
\label{sec:metatrie-edd}

\subsection{Motivación}
Misma membresía versionada que Backbone~2 en archivos \texttt{.docs} packed64,
representada como intervalos half-open tras RLE y trie \emph{time-first}
\cite{Arroyuelo2026TemporalBGPs}, en contraste con familias ZDD por snapshot.
Objetivo: BPI comparable ($8\times\mathrm{bytes}/n_{\mathrm{raw}}$) y
equivalencia de consultas $(t,\tau)\mapsto S^\tau$.

\subsection{Modelo y build}
Por término $M_t\subseteq U\times T$; RLE fusiona corridas $(u,\tau)$ en
$[\tau_a,\tau_b)$. \texttt{generate\_list\_of\_updates} produce intervalos
\emph{suelo} donde $S^\tau$ es constante; \texttt{create\_time\_first\_trie}
materializa \texttt{tempint\_left/right}, \texttt{last\_update} y un
\texttt{temporal\_wm} consultable con \texttt{leap}.
Implementación en \texttt{edd\_metatrie/meta\_trie\_edd.cpp} (port CLTJ sin
grafo RDF completo).

Modos \texttt{per-term} (payload $u$) y \texttt{global} ($t\to u$).
Consulta y validación: \texttt{values\_at\_version} / \texttt{values\_at}$(t,\tau)$ sobre el trie.
Ver Fig.~\ref{fig:metatrie-micro-intervalos}--\ref{fig:metatrie-micro-global}.

\subsection{Resultados wiki 2~GB}
Sobre \texttt{wiki\_2gb\_uihrdc\_packed64.docs}, validación
\texttt{--validate-terms 200}, octubre 2026 (índice único):
\texttt{per-term} $\mathrm{bpi\_file}\approx 5{,}996$;
\texttt{global} $\approx 4{,}563$; $\mathrm{bpi\_total}=\mathrm{bpi\_file}$;
\texttt{validation\_mismatches}=0.
```

---

## 9. Bloque LaTeX — ejemplo micro (\(t=0\))

Ver tablas y figuras en [`CONTEXTO_CONVERSACION_METATRIE_TESIS.md`](CONTEXTO_CONVERSACION_METATRIE_TESIS.md) §8–9; números suelo:

| \([\tau_a,\tau_b)\) | \([1,3)\) | \([3,5)\) | \([5,7)\) | \([7,8)\) |
|---------------------|-----------|-----------|-----------|-----------|
| \(S^\tau\) | \(\{u_1\}\) | \(\{u_1,u_2\}\) | \(\{u_2,u_3\}\) | \(\{u_3\}\) |

Comandos figuras:

```bash
cd edd_metatrie
./meta_trie_edd per-term ../BGPs/micro_metatrie.docs --dump-dot ../resultados_test/metatrie_micro_term0.dot --dump-term 0
./meta_trie_edd global  ../BGPs/micro_metatrie.docs --dump-dot ../resultados_test/metatrie_micro_global.dot
dot -Tpng -Gdpi=140 ../resultados_test/metatrie_micro_term0.dot -o ../resultados_test/metatrie_micro_term0.png
dot -Tpng -Gdpi=140 ../resultados_test/metatrie_micro_global.dot -o ../resultados_test/metatrie_micro_global.png
```

---

## 10. Verificación LaTeX vs artefactos (2026-10-04)

Fuente wiki: `resultados_test/meta_trie_edd_wiki_2gb_final_{per-term,global}.csv` y log `..._final_20261004.log`.

| Campo | per-term | global | Notas |
|-------|----------|--------|--------|
| MiB trie | 87.4 | 66.5 | OK (bytes/2²⁰) |
| `bpi_file` | **5.996** | **4.563** | OK (`single_index` log) |
| `bpi_total` | **5.996** | **4.563** | = `bpi_file` |
| `build_s` (solo construcción) | **15** | **40** | No confundir con wall |
| Wall total (`/usr/bin/time`) | **31 s** | **45 min** | Validación trie en `global` domina |
| Validación | 200 términos | 200 términos | Citar corrida **final**, no `postfix` |

ZDD ref. mismo $N$: `bpi_edd` ≈ **35.21** (`u+t` sucio baseline), **35.47** (`log` baseline sin optimize) — `benchmark_formatos_wiki_2gb.csv`.

Fuente sintético comparativa: `datos_sinteticos/metatrie_results.csv` + `sweep_results.csv`.

**Correcciones típicas al pegar en Overleaf:**

- Tab. comparativa: fila `C_toggle_zipf_fixed` → MT PT **3.65**, GL **5.52** (no 3.01/4.58).
- Fila `C_sparse_zipf_fixed` → MT PT **3.31**, GL **5.01** (no 3.55/5.24); ratio ≈ **45×**.
- Wiki: validación **200** términos; logs `meta_trie_edd_wiki_2gb_final_*`.
- Obs. wiki: `global` wall **≈ 45 min**, no «~3 min»; `build_s` global ≈ 40 s es solo el build.

---

## 11. Orden sugerido en capítulo experimental

1. Datasets (wiki + sintéticos).  
2. Backbone 2 ZDD + optimize.  
3. **Esta sección metatrie** (modelo + dualidad snapshot/intervalo).  
4. Resultados metatrie (tabla §6).  
5. Limitaciones §7.
