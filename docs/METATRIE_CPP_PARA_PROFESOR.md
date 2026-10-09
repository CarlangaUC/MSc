# `meta_trie_edd.cpp` — explicación completa (para defender ante el profesor)

> **Actualizado:** 2026-10-09 (vocabulario \(t,u,\tau,S^\tau\); `n_components`; helpers `resolve_at_tau` / `append_masters_at`)  
> **Código:** [`edd_metatrie/meta_trie_edd.cpp`](../edd_metatrie/meta_trie_edd.cpp) (~1280 líneas)  
> **Headers:** `edd_cltj_types.hpp`, `packed64_io.hpp`, `cltj_temporal_wm_u64.hpp`  
> **Guía + wavelet:** [`METATRIE_GUIA_EXPLICITA.md`](METATRIE_GUIA_EXPLICITA.md)

Bloque a bloque: qué es, **por qué**, **cómo**, y qué decir si preguntan “¿código tuyo o de BGPs?”.

---

## 0. Frase de apertura

> Binario de la EDD metatrie: lee `.docs` packed64, convierte postings en intervalos half-open, barre suelos y updates, construye un trie time-first con wavelet temporal, y responde \(Q_\tau(t,\tau)=S^\tau\). Incluye validación, BPI y harness de latencia porque **no** es una librería importada: se ejecuta desde scripts.

No incluye los 18 tries ni el grafo RDF de CLTJ. Sí reutiliza suelos, updates, WM y `leap`, adaptados a postings.

---

## 1. Includes y namespace

| Qué | Por qué |
|-----|---------|
| STL | I/O, build, timers |
| SDSL | Suelos / \(p_l\) compactos; `.emt` |
| `edd_cltj_types.hpp` | `spot_quad`, intervalo — semántica CLTJ, definición nuestra (u64) |
| `packed64_io.hpp` | Pack/unpack 40+24 |
| `cltj_temporal_wm_u64.hpp` | Wavelet B/E + `leap` |

`namespace edd` salvo `main`.

---

## 2. Contar bytes del WM

| Función | Rol |
|---------|-----|
| `counting_streambuf::xsputn` / `overflow` | Cuenta bytes sin materializar buffer |
| `wm_serialized_bytes` | Tamaño del WM para BPI (importante en per-term × ~250k) |
| `term_master` | Elemento de \(S^\tau\): `(term, master)` con `<` / `==` |

---

## 3. `time_first_trie` — índice consultable

### 3.1 Miembros

| Miembro | Significado |
|---------|-------------|
| `m_tempint_left` / `right` | Suelos \([\tau_a,\tau_b)\) |
| `m_last_update_pos[i]` | Prefijo \(p_l\) del suelo \(i\) |
| `m_last_update_per_int` + `select1` | Auxiliar CLTJ; select se **reengancha** tras copy/move |
| `m_temporal_ds` | `temporal_wm` (B/E) |
| `m_n_components` | 1 = solo \(u\); 2 = \((t,u)\) |
| `m_root_degree` | Número de suelos (= `interval_count`) |

El WM indexa valores \(t\)/\(u\) de los **updates**, no el eje \(\tau\). Suelos + \(p_l\) son el puente \(\tau \rightarrow\) estado.

### 3.2 Funciones del trie

| Función | Qué / por qué |
|---------|----------------|
| `rebind_supports` | `init_support(select, &bv)` tras copy/move (parche SDSL) |
| `copy_from` / `move_from` | Copia/mueve y rebind |
| ctor `(intervalos, wm, bv, p_l[], n_components)` | Empaqueta y `bit_compress` |
| `interval_count` / `n_components` | Accesores |
| `get_temporal_root` | Raíz WM = todo el stream |
| `get_last_update_of_interval` | \(p_l\) |
| `get_interval_at_pos` | Suelo \(i\) |
| `temporal_successor` | Wrapper de `leap` en componente `depth` |
| `resolve_at_tau(τ)` | \(\tau \mapsto (p_l,\) raíz\()\); false si vacío |
| `append_masters_at` | Enumera \(u\) con leaps sucesivos bajo un nodo |
| `values_at_version(τ)` | \(S^\tau\) **completo** del trie |
| `interval_seek` / `version_in_interval` | Ubicar y chequear suelo half-open |
| `arrays_debug` / `size_bytes_breakdown` | DOT / BPI |
| `serialize` / `load` | Persistencia |

**Consulta pública de tesis** no es `values_at_version` en global (enumeraría todos los \(t\)): es `meta_trie_edd::values_at(t,τ)`, que salta a \(t\).

---

## 4. Barrido y `create_time_first_trie`

| Función | Rol |
|---------|-----|
| `generate_list_of_updates(D)` | Extremos → suelos + insert/delete; **parche** cierre del último suelo |
| `comparator_time_first` | Orden stream: \(\tau\) suelo, luego \(t\), \(u\) |
| `create_time_first_trie(D, D_T, n_bits, n_components)` | Suelos, \(p_l\), stream ordenado, `temporal_wm(..., is_partial=false)`, empaqueta trie |

Firma actual (simplificada): **un solo** `n_components` (antes había `n_tuple_components` + `payload_components` + `is_partial` redundantes).

| Modo | `n_components` |
|------|---------------:|
| global | 2 |
| per-term | 1 |

---

## 5. `.docs` y RLE (aporte MAGISTER)

| Función | Rol |
|---------|-----|
| `read_u32` / `read_posting_list` | Lectura binaria |
| `inspect_docs` | Offsets, máximos, conteos |
| `postings_to_quads_rle` | Postings → quads \((t,u,0,\tau_a,\tau_b)\) |
| `postings_to_quads_point` | Sin RLE (`--no-rle`) |
| `postings_to_quads_rle_per_term` / `_point_per_term` | Quads \((u,0,0,\tau_a,\tau_b)\) |
| `scan_docs_metrics` | \(n_{\mathrm{raw}}\), pares únicos → BPI |

RLE **on** por defecto (`use_rle=true`).

---

## 6. `meta_trie_edd`

| Función | Rol |
|---------|-----|
| `build_global` | Un \(D\) → un barrido → un trie (`n_components=2`) |
| `build_per_term` | Por \(t\): trie (`n_components=1`) |
| `values_at(t,τ)` | \(Q_\tau\): `resolve_at_tau` + leap a \(t\) (global) o `values_at_version` (per-term) |
| `size_report` / `serialize` / `load` | Espacio y `.emt` |

---

## 7. Wavelet (`temporal_wm`) — funciones

| Función | Rol |
|---------|-----|
| `get_component` / `top_bits` / `get_accumulated_bit` | Bits de \(t\)/\(u\) al armar B |
| `generate_B_E_stable_prefix_sort` | Construye **B** y **E** |
| `rank_range_B` / `rank_range_E` | Rank en un nivel |
| ctor | Dimensiona B/E; `is_partial` siempre false en MAGISTER |
| `get_root` / `get_n_bits` | Raíz y ancho |
| `leap` / `leftmost` | Sucesor ≥ \(x\) / mínimo del subárbol |
| `serialize` / `load` | Persistencia |

Puente: \(\tau \rightarrow\) suelo \(\rightarrow p_l \rightarrow\) leap \(\rightarrow S^\tau\).

---

## 8. Harness

| Función | Rol |
|---------|-----|
| `NzddBpi::bpiFromBytes` (`plus_t/utils/bpi.h`) | Misma fórmula canónica MAGISTER: \(8\times\mathrm{bytes}/n\); canónico vs ZDD = `bpi_file` con \(n_{\mathrm{raw}}\) |
| `dump_trie_dot` | Graphviz suelos + \(S^\tau\) (`--dump-max-answer`) |
| `print_size_report` | stdout BPI |
| `validation_times` | \(\tau\) de borde ±1 |
| `expected_masters` / `expected_triples_at` | Oráculo RLE |
| `validate_index` | Trie ≡ `.docs` |
| `sample_queries` / `run_query_bench` | Latencia docs_scan vs `values_at` |
| `parse_args` / `main` | CLI y orquestación |

---

## 9. Flujo `main` → fin de construcción

```text
main
├─ parse_args
├─ inspect_docs
└─ build_global | build_per_term
   ├─ read_posting_list
   ├─ postings_to_quads_rle[_per_term]
   ├─ generate_list_of_updates
   └─ create_time_first_trie → temporal_wm::ctor → generate_B_E_…
```

Luego (ya no es build): `scan_docs_metrics`, `print_size_report`, `validate_index`, bench/dump/serialize opcionales.

---

## 10. Cadena de una query

```text
values_at(t, τ)
├─ resolve_at_tau → interval_seek, version_in_interval, p_l, root
├─ [global] temporal_successor(depth=0, cand=t) → leap
└─ append_masters_at → leap… → S^τ
```

---

## 11. Respecto al repo BGPs: qué se ocupa, qué se reutiliza, qué se cambia

**Repo de referencia (no se modifica):**  
[`BGPs/bgps-temporal-graphs`](../BGPs/bgps-temporal-graphs) — índice CLTJ con **18 tries** (`cltj_index_temporal_metatrie.hpp`), quads del grafo temporal, LTJ, etc.

**Importante:** MAGISTER **no enlaza** ese código al compilar `meta_trie_edd`. No hay `#include` de `cltj_*.hpp` del monorepo en el build del metatrie. La lógica se **copió/adaptó** a `edd_metatrie/`. BGPs es referencia teórica y de nombres.

### 11.1 Qué se ocupa (de la idea / del paper CLTJ)

| Concepto | En BGPs / paper | En MAGISTER |
|----------|-----------------|-------------|
| Quad half-open con vida \([\tau_a,\tau_b)\) | `cltj::spot_quad` | `edd::spot_quad` (u64) |
| Barrido → suelos + insert/delete | `generate_list_of_updates` | Misma función (copiada + parche) |
| Stream time-first de updates | `create_time_first_trie` | Igual (firma simplificada) |
| Wavelet B/E + `leap` / `leftmost` | `cltj_temporal_wm.hpp` | `cltj_temporal_wm_u64.hpp` (fork) |
| Trie time-first completo | variante `TIME_FIRST_FULL_INTERVALS` en `compact_trie_v3` | Clase `time_first_trie` |
| Consulta de estado en \(\tau\) vía \(p_l\) | LTJ / iteradores en el paper | `values_at` / `values_at_version` |

Eso es lo que “ocupamos” del ecosistema CLTJ: la **vista por intervalos** y la estructura time-first + WM.

### 11.2 Qué se reutiliza (algoritmo / teoría) vs qué no

**Reutilizado (algoritmo, no el `.so` del repo):**

- Partición del eje \(\tau\) en suelos donde el snapshot es constante.  
- Stream de updates insert/delete en los bordes.  
- Codificación B/E y navegación `leap`.  
- Semántica half-open de intervalos.

**No reutilizado (del índice BGPs completo):**

| Pieza BGPs | ¿En metatrie MAGISTER? |
|------------|------------------------|
| 18 órdenes de tries (SPOT, SPT, …) | **No** — un solo orden time-first por build |
| Grafo RDF / quads SPOT de entrada | **No** — entrada = `.docs` packed64 |
| Evaluación BGP / LTJ multi-join | **No** — solo \(Q_\tau(t,\tau)\mapsto S^\tau\) |
| Tries “partial” / `is_partial=true` | **No** — siempre `false` |
| Headers `cltj_*.hpp` linkeados | **No** — copia local en `edd_metatrie/` |

### 11.3 Qué se cambia / aporta MAGISTER

| Cambio | Por qué |
|--------|---------|
| Entrada `.docs` + `packed64_io` (40+24) | Mismo input que ZDD / uiHRDC |
| `postings_to_quads_rle` | BGPs asume quads; nosotros los fabricamos desde postings |
| Modos `global` / `per-term` | Experimento de layout (1 trie vs V tries) |
| Masters u64 / WM fork | Packed64 con masters > 32 bit |
| Parche cierre último suelo en `generate_list_of_updates` | Bug OOB upstream al máximo \(\tau\) |
| Parche `leap` / `leftmost` (guards `p_left >= 0`, etc.) | Bug de índice en E con prefijos borde |
| `rebind_supports` en copy/move del trie | Select SDSL colgante con ~250k assigns per-term |
| API `n_components` (sin exponer `payload` / `is_partial`) | Alineado a modelación \(t,u,\tau,S^\tau\) |
| `resolve_at_tau` / `append_masters_at` / `values_at(t,τ)` con salto a \(t\) | Consulta tesis eficiente en global |
| Validación vs RLE del `.docs`, BPI, bench, CLI, DOT | Harness experimental; binario autocontenido |
| Monorepo BGPs **intacto** | Parches solo en `edd_metatrie/` |

### 11.4 Tabla archivo ↔ origen

| Archivo MAGISTER | Origen |
|------------------|--------|
| `edd_cltj_types.hpp` | Semántica `spot_quad` CLTJ; definición **nuestra** (u64) |
| `cltj_temporal_wm_u64.hpp` | Fork de `cltj_temporal_wm.hpp` + parches leap/leftmost + u64 |
| `generate_list_of_updates` / `create_time_first_trie` en `.cpp` | Port de `cltj_build_compact_tries.hpp` + parche / firma simple |
| `time_first_trie` | Recorte de `cltj_compact_trie_v3` (solo full intervals) |
| RLE, `inspect_docs`, `meta_trie_edd`, validate, bench, `main` | **Propio MAGISTER** |
| `packed64_io.hpp` | Alineado uiHRDC; wrapper nuestro |

### 11.5 Frase para el profesor

> Del repo BGPs/CLTJ tomamos la **teoría y el algoritmo** del trie time-first (suelos, updates, wavelet B/E, leap). No modificamos ni linkeamos ese monorepo: el código vive copiado/adaptado en `edd_metatrie/`. Lo que cambia es la **aplicación**: postings versionados `.docs` → RLE → un (o V) trie(s) time-first, consulta \(Q_\tau\), y tres parches de robustez (último suelo, leap, rebind SDSL) más el harness de BPI/latencia.

---

## 12. Guion 2 minutos + Q&A

1. \(M\) por **intervalos**; dual del ZDD (snapshots).  
2. Postings → RLE → barrido → stream → wavelet → \(Q_\tau\).  
3. Evidencia: BPI wiki 2GB ~4.56 / ~6.0; mismatches 0; latencia vs scan.  
4. Updates = eventos del barrido de **construcción**, no online.

| Pregunta | Respuesta corta |
|----------|-----------------|
| ¿Dónde está el wavelet? | `cltj_temporal_wm_u64.hpp`; se arma en `create_time_first_trie`, se consulta con `leap`. |
| ¿Qué es un update? | Insert en \(\tau_a\), delete en \(\tau_b\). |
| ¿Por qué half-open? | Convención paper; bordes limpios. |
| ¿ZDD importa esto? | No; scripts ejecutan ambos binarios; mismo `.docs`. |
| ¿Qué era “payload”? | Vocabulario viejo = componentes no temporales \(t\)/\(u\); ahora `n_components` y \(S^\tau\). |

---

## 13. Catálogo de funciones (referencia rápida)

Ver también tablas en §3–§8. Resumen de llamadas:

```text
main → build_* → read_posting_list → postings_to_quads_rle*
              → generate_list_of_updates → create_time_first_trie
              → temporal_wm::ctor → generate_B_E_stable_prefix_sort

values_at(t,τ) → resolve_at_tau → leap / append_masters_at → S^τ
```

---

## 14. Orden de lectura del fuente

1. `main` → `build_global`  
2. `postings_to_quads_rle`  
3. `generate_list_of_updates`  
4. `create_time_first_trie`  
5. ctor `temporal_wm` + `leap`  
6. `values_at` / `resolve_at_tau` / `append_masters_at`  
7. `validate_index` / `run_query_bench`
