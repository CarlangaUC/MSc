# Metatrie — guía explícita (código, función y estado)

> **Fecha:** 2026-10-08  
> **Código:** [`edd_metatrie/meta_trie_edd.cpp`](../edd_metatrie/meta_trie_edd.cpp), [`cltj_temporal_wm_u64.hpp`](../edd_metatrie/cltj_temporal_wm_u64.hpp), [`packed64_io.hpp`](../edd_metatrie/packed64_io.hpp)  
> **Uso / benches:** [`edd_metatrie/README.md`](../edd_metatrie/README.md)  
> **Paso a paso vs paper BGPs:** [`METATRIE_CODIGO_PASO_A_PASO.md`](METATRIE_CODIGO_PASO_A_PASO.md)  
> **Tesis (modelo + LaTeX):** [`METATRIE_TESIS.md`](METATRIE_TESIS.md)

Este documento es la **foto completa** del metatrie en MAGISTER: idea, **wavelet temporal (§3)**, **flujo end-to-end de llamadas (§4)**, catálogo de funciones, resultados y estado.

---

## 1. En qué nos quedamos

| Tema | Estado |
|------|--------|
| Índice metatrie sobre wiki 2 GB (250 550 términos) | **Listo.** Build global ~35–53 s; per-term ~9–28 s. BPI canónico: global **4.563**, per-term **5.996**. Validación OK. |
| Consulta nativa \(Q_\tau\): `(t,τ) → S^τ` | **Listo.** `values_at(t,τ)` en global salta solo al término pedido (no enumera todo el snapshot). |
| Bench vs `docs_scan` (misma lista de queries) | **Listo.** Driver en `meta_trie_edd` + `scripts/bench_query_tau.sh`. |
| Bench ZDD comparable en queries | **Listo con matices.** ZDD usa bosque **sin optimize** (`.zpack` de `build`). Solo cronometra `term < V` del forest (p.ej. 200). Mide **membresía**, no \(Q_\tau\). |
| ST[OP] auxiliar / 18 tries BGPs | **Fuera.** Solo el trie time-first medido. |
| Updates incrementales | **No implementado.** |

CSV canónico del último bench full:  
`resultados_test/bench_query_tau_wiki_2gb_{global,per-term}_full_zddbase_20261006_205708.csv`.

---

## 2. Idea en una página

### 2.1 Qué problema resuelve

Tenemos un índice invertido versionado en formato **`.docs` packed64** (mismo input que el pipeline ZDD / uiHRDC):

- Cada **término** \(t\) tiene una posting list.
- Cada posting es un `u64 = (master << 24) | τ` (layout default **40+24**).
- `master` = documento \(u\); `τ` / `rel` = versión.

La membresía es \(M_t \subseteq U \times T\): el master \(u\) está activo para el término \(t\) en ciertas versiones.

El metatrie responde la consulta de tesis:

\[
Q_\tau(t,\tau) = S^\tau = \{ u \mid (u,\tau)\in M_t \}
\]

En código: `values_at(t, τ)` → lista de masters.

### 2.2 Dualidad con el ZDD

| Vista | Representación | Implementación |
|--------|----------------|----------------|
| **Snapshot** | \(S^\tau = \{u \mid (u,\tau)\in M_t\}\) | Backbone 2: ZDD (`zdd_cudd_plus_t`) |
| **Intervalo** | RLE: \(u\) activo en \([\tau_a,\tau_b)\) | Metatrie: barrido + `time_first_trie` + `temporal_wm` |

Misma \(M\), distinta estructura. El ZDD **no** tiene \(Q_\tau\) nativa: su bench `bench-qmem` mide si un conjunto \(S\) (sacado del `.docs`) es miembro del ZDD del término \(t\).

### 2.3 Pipeline en 5 frases

1. Leer postings packed64 del `.docs`.  
2. Fusionar versiones consecutivas del mismo master → **quads** half-open `[τa, τb)`.  
3. Barrido de extremos → **suelos** (tramos donde el snapshot no cambia) + lista de updates insert/delete.  
4. Ordenar updates time-first y construir el **wavelet temporal** (bitvectors B y E del paper CLTJ).  
5. Consultar: ubicar el suelo de τ, tomar el prefijo \(p_l\) del stream, y **enumerar** \(S^\tau\) con `leap`.

```text
.docs packed64
    → postings_to_quads_rle          → D  (quads)
    → generate_list_of_updates       → D_T (suelos + updates)
    → create_time_first_trie         → time_first_trie (suelos + temporal_wm)
    → values_at(t, τ)                → S^τ
```

---

## 3. El wavelet temporal (`temporal_wm`) — pieza central

Sin el wavelet, el metatrie sería solo una lista de suelos y updates: correcto, pero **lento y grande** para enumerar \(S^\tau\). El `temporal_wm` es lo que hace viable la consulta \(Q_\tau\).

### 3.1 Qué problema resuelve

Tras el barrido tenemos un **stream de eventos** ordenado time-first:

\[
(\text{insert}/\text{delete},\; t,\; u) \quad\text{o solo}\quad (\text{insert}/\text{delete},\; u)
\]

Ejemplo (modo global, componentes \((t,u)\)):

```text
suelo [0,3):  + (t=1,u=5)   + (t=1,u=9)   - (t=2,u=3)  ...
suelo [3,7):  + (t=1,u=12)  ...
```

Pregunta de consulta: *dado el prefijo \(p_l\) del stream (estado vigente en el suelo de \(\tau\)), ¿qué pares \((t,u)\) (o masters \(u\)) están activos?* → eso es \(S^\tau\).

Si recorriéramos el stream a mano: \(O(L)\) por query. El wavelet simula un **trie sobre los bits de \(t\)/\(u\)** y responde “siguiente valor \(\ge x\)” con `leap`, usando solo bitvectors y `rank`.

### 3.2 Intuición: no es un wavelet matrix clásico de texto

Nombre en código: `temporal_wm` (de CLTJ / paper Arroyuelo et al. §6).

- **Wavelet matrix / tree clásico:** comprime una secuencia de símbolos y soporta `access`/`rank`/`select` en el alfabeto.
- **Aquí:** la “secuencia” son los **updates** (insert/delete) del barrido; el “alfabeto” son los valores del quad no temporales (\(u\), o \((t,u)\)) bit a bit. Un delete cancela un insert con el mismo \((t,u)\)/\(u\); el bitvector **E** marca si un prefijo sigue “vivo” (insert−delete \(\neq 0\)).

En la tesis:

> Indexamos el stream de updates time-first con un wavelet (B y E) que simula un trie binario sobre \(t\)/\(u\) y enumera \(S^\tau\) bajo el prefijo \(p_l\) con `leap` / `leftmost`.

### 3.3 Los dos bitvectors: B y E

Construcción: `generate_B_E_stable_prefix_sort` en `cltj_temporal_wm_u64.hpp`.

| Bitvector | Qué guarda | Para qué sirve |
|-----------|------------|----------------|
| **B** | En cada nivel, el **bit de \(t\)/\(u\)** (0/1) de cada update | Hijo izq. (0) vs der. (1); `rank_range_B` |
| **E** | Conteo neto insert−delete \(\neq 0\) en ese prefijo | Si E=0 bajo \(p_l\), ese valor no está en \(S^\tau\) |

Tamaños (orden de magnitud):

- \(|B| \approx n_{\text{bits}} \cdot n_{\text{updates}} \cdot n_{\text{componentes}}\)
- \(|E|\) un poco más (incluye nivel extra de “existencia”)

`n_bits` = ancho para codificar el máximo master (y término, en global).  
`n_updates` = largo del stream tras el barrido.  
`n_componentes` = 1 (per-term) o 2 (global).

Eso explica gran parte del **BPI**: el WM es el grueso de `bytes_total`.

### 3.4 Cómo se construye (idea)

Para cada bit de \(t\)/\(u\) (MSB → LSB, componente a componente):

1. Escribir en **B** el bit actual de cada update (en el orden actual del índice).
2. **Stable-sort** los updates por el prefijo de bits ya emitidos (como en una wavelet matrix).
3. Recalcular conteos insert/delete por prefijo y escribir **E** (1 = prefijo aún vivo).

Al final, `rank` sobre B y E permite bajar el trie sin materializar nodos.

### 3.5 Consulta dentro del WM: `leap` y `leftmost`

Parámetros clave de `leap(depth, s, e, p, x, h, …)`:

| Parámetro | Significado |
|-----------|-------------|
| `depth` | Nivel actual en el trie de bits |
| `[s,e]` | Rango de updates que caen en este nodo |
| `p` | Prefijo relativo del stream (ligado a \(p_l\) del suelo) |
| `x` | Símbolo buscado (sucesor \(\ge x\)) |
| `h` | Bits que faltan por leer |

**`leap`:** “dame el menor valor \(\ge x\) activo bajo el prefijo \(p\)” (\(x\) es un \(t\) o un \(u\)).  
Si el bit alto de \(x\) es 1, baja al hijo derecho; si es 0, intenta izquierdo y, si no hay match, **leftmost** del hermano derecho.

**`leftmost`:** menor valor del subárbol (redondeo hacia arriba).

En el trie: `temporal_successor` → un `leap` por componente (`n_components` = 1 o 2).

### 3.6 Cómo se conecta con el suelo y \(p_l\)

El WM **no** guarda τ por sí solo. El `time_first_trie` guarda:

1. Arrays `tempint_left` / `tempint_right` — suelos \([\tau_a,\tau_b)\).
2. `last_update_pos[i]` — prefijo \(p_l\) del stream al cerrar el suelo \(i\).
3. El `temporal_wm` — el DAG/trie comprimido de todos los updates.

Flujo de consulta:

```text
τ  →  interval_seek  →  suelo i
i  →  last_update_pos[i] = p_l
(p_l, root del WM)  →  leap / leap …  →  S^τ
```

Sin \(p_l\), no sabríamos **hasta dónde** del stream cortar. Sin el WM, enumerar desde \(p_l\) sería lineal.

### 3.7 Por qué es importante en la tesis

1. **Correctitud:** \(Q_\tau\) se reduce a “estado del conjunto tras los primeros \(p_l\) updates”, evaluado por navegación en B/E.  
2. **Espacio:** el BPI del metatrie es, en la práctica, el costo de comprimir ese stream (más los suelos).  
3. **Tiempo:** `leap` evita escanear \(L\) updates; el costo crece con bits del alfabeto y tamaño de la respuesta, no con todo el historial.  
4. **Puente teórico:** es la misma pieza §6 del paper de grafos temporales (CLTJ); nosotros la aplicamos a **postings versionados** del índice invertido, no a un grafo RDF de 18 órdenes.

### 3.8 Mini-ejemplo (per-term, un término)

Quads tras RLE: \(u=5\) en \([1,4)\), \(u=9\) en \([2,3)\).

```text
Extremos: 1, 2, 3, 4
Suelos:   [1,2)   [2,3)   [3,4)
Updates:  +5      +9      -9      -5
```

En el suelo \([2,3)\), \(p_l\) apunta al update `+9`.  
`values_at_version(2)` hace `leap` desde la raíz con ese \(p_l\) y obtiene \(\{5,9\}\).  
En \([3,4)\), tras `-9`, el leap solo ve \(\{5\}\).

El wavelet es quien responde esos leaps sin rehacer el barrido.

---

## 4. Flujo end-to-end (funciones llamadas)

### 4.1 Build modo `global` (un trie)

```text
main
├─ parse_args
├─ inspect_docs(docs, max_terms)          → offsets[], max_master, max_rel
├─ meta_trie_edd::build_global
│  ├─ inspect_docs                        (otra pasada / mismos metadatos)
│  ├─ para cada term t:
│  │  ├─ read_posting_list(offset[t])
│  │  └─ postings_to_quads_rle(t, …) → append a D
│  │     (o postings_to_quads_point si --no-rle)
│  ├─ generate_list_of_updates(D) → D_T   // suelos + updates
│  └─ create_time_first_trie(D, D_T, bits, n_comp=2, payload=2)
│     ├─ arma interval_list, last_update_pos, last_update_bv
│     ├─ expande updates → new_D + is_delete
│     ├─ sort (comparator_time_first_payload)
│     ├─ temporal_wm::temporal_wm(sorted_D, sorted_del, …)
│     │  └─ generate_B_E_stable_prefix_sort  // escribe B y E
│     └─ time_first_trie(interval_list, wm, …)
├─ scan_docs_metrics                      → n_raw, n_pairs_uniq
├─ size_report / print_size_report        → BPI
├─ validate_index
│  └─ por término / τ en validation_times:
│     expected_triples_at  vs  values_at(t,τ)
└─ [opcional] run_query_bench / dump_trie_dot / serialize
```

### 4.2 Build modo `per-term` (V tries)

Igual que arriba, pero el bucle interno es:

```text
build_per_term
└─ para cada term t:
   ├─ read_posting_list
   ├─ postings_to_quads_rle_per_term → D_t   // payload solo u
   ├─ generate_list_of_updates(D_t) → D_T
   └─ create_time_first_trie(..., n_comp=1, payload=1)
      └─ per_term[t] = ese trie
```

### 4.3 Consulta \(Q_\tau\): `values_at(t, τ)`

**Global (salto directo al término):**

```text
values_at(t, τ)
├─ interval_seek(τ)                    → índice de suelo
├─ version_in_interval(pos, τ)?
├─ get_last_update_of_interval(pos)    → p_l
├─ get_temporal_root()                 → [0, n_updates)
├─ temporal_successor(depth=0, …, cand=t)   → leap en componente t
│  └─ temporal_wm::leap → (posible) leftmost
└─ mientras haya masters:
   └─ temporal_successor(depth=1, …, cand=u) → emitir (t,u)
```

**Per-term:**

```text
values_at(t, τ)
└─ per_term[t].values_at_version(τ)
   ├─ interval_seek / version_in_interval / p_l / root
   └─ bucle leap(depth=0) enumerando solo u
```

### 4.4 Bench de latencia (mismo proceso)

```text
main (--bench-queries N)
├─ sample_queries → lista (t,τ)
└─ run_query_bench
   ├─ warm-up: expected_masters + values_at
   ├─ timer docs_scan:  expected_masters(postings[t], τ)  × reps
   └─ timer metatrie:   values_at(t, τ)                   × reps
```

ZDD (script aparte, **otra** semántica):

```text
zdd_cudd_plus_t build … → .zpack (sin optimize)
zdd_cudd_plus_t bench-qmem … --pack .zpack
  └─ por query con term < V:
     Intersect(ZDD^t, S) + countSubsets   // membresía, no Q_τ
```

### 4.5 Diagrama compacto (build + query)

```text
                    .docs
                      │
              inspect_docs / read_posting_list
                      │
              postings_to_quads_rle
                      │
                      D  (intervalos / quads)
                      │
           generate_list_of_updates
                      │
              D_T (suelos + updates)
                      │
           create_time_first_trie
                 ┌────┴────┐
            suelos/p_l   temporal_wm
                         (B, E, leap)
                 └────┬────┘
               time_first_trie
                      │
              values_at(t, τ)  ──→  S^τ
```

---

## 5. Archivos del código

| Archivo | Rol |
|---------|-----|
| `edd_metatrie/meta_trie_edd.cpp` | Todo el índice: I/O `.docs`, RLE, barrido, trie, consulta, validación, bench, CLI. |
| `edd_metatrie/cltj_temporal_wm_u64.hpp` | `temporal_wm`: B/E, `leap`, `leftmost` (fork CLTJ con masters 64-bit). |
| `edd_metatrie/edd_cltj_types.hpp` | `spot_quad` = `array<u64,5>`, intervalo temporal. |
| `edd_metatrie/packed64_io.hpp` | Pack/unpack 40+24 alineado a uiHRDC. |
| `edd_metatrie/build.sh` | Compila `meta_trie_edd` (SDSL + divsufsort). |
| `scripts/bench_query_tau.sh` | Metatrie + ZDD baseline sobre la misma lista de queries. |
| `scripts/run_bench_wiki2gb_full.sh` | Full wiki 2GB: metatrie + pack ZDD sin optimize + qmem. |

No se modifica el monorepo BGPs; la lógica CLTJ está **copiada/adaptada** aquí.

---

## 6. Notación

| Símbolo | En código | Significado |
|---------|-----------|-------------|
| \(t\) | índice de lista / `term` | Término del vocabulario (0 = primera lista del `.docs`) |
| \(u\) | `master` | Documento |
| \(\tau\) | `rel` / `version` | Versión entera |
| Quad | `edd::spot_quad` | 5× u64: global `(t,u,0,τa,τb)`; per-term `(u,0,0,τa,τb)` |
| Suelo | entrada en `m_tempint_left/right` | Intervalo half-open donde \(S\) es constante |
| Payload | lo que enumera el WM | 1 componente (`u`) o 2 (`t` luego `u`) |
| `p_l` | `last_update_pos[suelo]` | Último evento del stream aplicable en ese suelo |
| BPI | `8 × bytes / n_raw` | Bits por ítem del posting crudo |

---

## 7. Dos modos de índice

| | **global** | **per-term** |
|---|------------|--------------|
| Qué se construye | Un solo `time_first_trie` | Un `time_first_trie` por término |
| Quads en \(D\) | `(t, u, 0, τa, τb)` | `(u, 0, 0, τa, τb)` |
| Componentes del WM | 2 (`t` → `u`) | 1 (`u`) |
| Consulta `values_at(t,τ)` | `interval_seek` + leap directo a `t`, luego enumera `u` | `per_term[t].values_at_version(τ)` |
| BPI wiki 2GB | **4.563** | **5.996** |
| Latencia \(Q_\tau\) (1000×5) | ~2806 ns | ~2112 ns |

Ambos deben dar el mismo \(S^\tau\) que el ground truth RLE del `.docs` (`validation_mismatches=0`).

---

## 8. Funciones — referencia completa

Cada función del código activo tiene **una línea** en el fuente. Aquí va la explicación extendida. El wavelet se detalla en §3; esta sección es el catálogo.

### 8.1 Packed64 (`packed64_io.hpp`)

| Función | Qué hace |
|---------|----------|
| `unpack_master(packed)` | Saca el documento de los bits altos. |
| `unpack_relative(packed)` | Saca τ de los bits bajos. |
| `pack_master_rel(master, rel)` | Arma el u64; aborta si no cabe en 40/24. |
| `bits_required_u64(value)` | Ancho en bits para codificar `value` en el WM. |

### 8.2 Lectura del `.docs`

| Función | Qué hace |
|---------|----------|
| `read_u32` | Lee un `uint32` little-endian. |
| `read_posting_list(offset)` | En `offset`: `[u32 len][u64 packed…]`. |
| `inspect_docs(path, max_terms)` | Recorre listas, guarda offsets, cuenta postings, max master/τ. |
| `scan_docs_metrics` | Calcula `n_raw` y pares `(u,τ)` únicos (denominadores del BPI). |

### 8.3 Postings → quads

| Función | Qué hace |
|---------|----------|
| `postings_to_quads_rle(term, …)` | Por cada master, fusiona τ consecutivos en un quad `[τa, τb)`. Global: payload incluye `term`. |
| `postings_to_quads_point(term, …)` | Sin RLE: un quad `[τ, τ+1)` por posting (`--no-rle`). |
| `postings_to_quads_rle_per_term` | Igual que RLE pero el quad queda `(u,0,0,τa,τb)` (el término es el índice del trie). |
| `postings_to_quads_point_per_term` | Punto per-term. |

**Ejemplo RLE.** Postings del término 7: masters `(5@1), (5@2), (5@3), (9@10)` → quads  
`(7,5,0,1,4)` y `(7,9,0,10,11)`.

### 8.4 Barrido y construcción del trie

| Función | Qué hace |
|---------|----------|
| `generate_list_of_updates(D)` | Pone eventos en τa (insert) y τb (delete), ordena, arma **suelos** y la lista de updates por borde. Parche MAGISTER: cierra bien el último suelo (evita OOB del upstream). |
| `comparator_time_first_payload` | Orden del stream: τ del suelo, luego t, luego u. |
| `create_time_first_trie(D, D_T, …)` | Materializa suelos, `last_update_pos`, expande updates, ordena, construye `temporal_wm`, empaqueta `time_first_trie`. |
| `meta_trie_edd::build_global` | Lee todas las listas → un \(D\) → un trie (2 componentes). |
| `meta_trie_edd::build_per_term` | Por cada término: \(D\) → trie (1 componente). |

### 8.5 Wavelet temporal (`temporal_wm`) — catálogo

Detalle conceptual en **§3**. Resumen: bitvectors **B** y **E** que simulan un trie sobre el stream de updates.

| Función | Qué hace |
|---------|----------|
| ctor `temporal_wm(updates, deletes, …)` | Dimensiona B/E y llama al generador. |
| `generate_B_E_stable_prefix_sort` | Construye B y E con sort estable por prefijo de bits. |
| `get_component` / `top_bits` / `get_accumulated_bit` | Leen bits del payload al armar B/E. |
| `rank_range_B` / `rank_range_E` | `rank1` en un rango de un nivel. |
| `get_root` | Intervalo raíz `[0, n_updates)`. |
| `get_n_bits` | Ancho de cada componente. |
| `leap(…, x, …)` | Sucesor ≥ `x` en el nodo actual; si el bit alto es 0 y no hay match, cae a `leftmost`. |
| `leftmost` | Menor símbolo del subárbol (hijo 0, si no hijo 1). |
| `serialize` / `load` | Persistencia SDSL. |

### 8.6 Trie time-first y consulta

| Función | Qué hace |
|---------|----------|
| ctor `time_first_trie` | Guarda suelos comprimidos, bitvector/posiciones de last-update, y el WM. |
| `interval_count` | Cantidad de suelos. |
| `payload_components` | 1 (per-term) o 2 (global). |
| `get_interval_at_pos(i)` | `[τ_left, τ_right)` del suelo `i`. |
| `interval_seek(τ)` | Búsqueda binaria del suelo que contiene τ (o “ninguno”). |
| `version_in_interval(pos, τ)` | Comprueba half-open `[left, right)`. |
| `get_last_update_of_interval(pos)` | Prefijo `p_l` del stream vigente en ese suelo. |
| `get_temporal_root` | Raíz del WM. |
| `temporal_successor` | Wrapper: un `leap` en el componente `depth`. |
| `values_at_version(τ)` | Snapshot **completo** del trie en τ (enumera todo el payload). |
| `values_at(t, τ)` | **Consulta de la tesis.** Global: seek + leap al término `t` + enumera masters. Per-term: `per_term[t].values_at_version(τ)`. |
| `size_bytes_breakdown` | Bytes de tempint + last-update + WM. |
| `serialize` / `load` | Persistencia del trie (parte del `.emt`). |

#### Cómo camina `values_at(t, τ)` (global)

```text
1. pos ← interval_seek(τ)
2. si τ ∉ suelo[pos] → vacío
3. p_l ← get_last_update_of_interval(pos)
4. node ← get_temporal_root()
5. term_v ← leap(nivel 0, node, p_l, cand=t)   // ¿existe el término t?
6. si term_v ≠ t → vacío
7. mientras haya masters:
      u ← leap(nivel 1, nodo_de_t, p_l_relativo, cand=u_next)
      emitir (t, u)
```

En per-term el paso 5–6 no hace falta: el trie ya es solo de ese término.

### 8.7 Validación, métricas y bench

| Función | Qué hace |
|---------|----------|
| `expected_masters(postings, τ)` | Ground truth: masters activos en τ según la misma regla RLE. |
| `expected_triples_at` | Lo mismo como `(term, master)`. |
| `validation_times` | τ de borde y vecinos (±1, max+2, …) para tensionar el índice. |
| `validate_index` | Compara `values_at` vs expected en los primeros K términos. |
| `bpi_from_bytes` | `8 * bytes / n`. |
| `size_report` / `print_size_report` | Bytes totales y BPI por modo. |
| `sample_queries` | Lista determinística `(t,τ)` round-robin. |
| `run_query_bench` | Cronometra `docs_scan` (`expected_masters`) vs `values_at`; cuenta mismatches. |
| `dump_trie_dot` | Graphviz del trie lógico (inputs chicos). |
| `parse_args` / `main` | CLI: build → métricas → validación → bench/dump/serialize opcionales. |

### 8.8 Qué se sacó del código (no usado)

| Función | Motivo |
|---------|--------|
| `node_degree` (WM) | Nadie la llamaba; el grado se deduce vía `leap`/`rank`. |
| `root_degree` (trie) | Redundante con `interval_count`. |
| `wm_n_bits` (wrapper) | No se usaba; queda `get_n_bits` en el WM. |
| `values_at(τ)` sin término | Enumeraba todos los términos; la API de tesis es `(t,τ)`. |

---

## 9. Cómo compilar y correr

```bash
cd /root/MAGISTER/edd_metatrie
./build.sh

# Índice + validación + métricas BPI
./meta_trie_edd global  ../resultados_test/wiki_2gb_packed64.docs --validate-terms 200
./meta_trie_edd per-term ../resultados_test/wiki_2gb_packed64.docs --validate-terms 200

# Bench Q_τ (docs_scan + metatrie) y escribe CSV + lista de queries
./meta_trie_edd global ../resultados_test/wiki_2gb_packed64.docs \
  --validate-terms 0 \
  --bench-queries 1000 --bench-reps 5 \
  --bench-csv ../resultados_test/mi_bench.csv \
  --bench-queries-out ../resultados_test/mi_queries.csv

# Full wiki 2GB: metatrie + ZDD baseline (sin optimize)
bash ../scripts/run_bench_wiki2gb_full.sh
```

ZDD solo (bosque sin optimize):

```bash
./zdd_cudd_plus_t build u+t <docs> <voc> <out.zpack> 200
./zdd_cudd_plus_t bench-qmem u+t <docs> <queries.csv> --pack <out.zpack> 5 <out.csv>
```

---

## 10. Resultados canónicos (wiki 2 GB)

### 10.1 Espacio (BPI)

| Modo | `bytes_total` (aprox.) | `bpi_file` | `n_raw` |
|------|------------------------:|-----------:|--------:|
| global | ~69.7 MB | **4.563** | 122 197 799 |
| per-term | ~91.6 MB | **5.996** | 122 197 799 |

### 10.2 Latencia de consulta (2026-10-06, `*_zddbase_*`)

1000 queries × 5 reps; mismatches = 0.

| Estructura | Modo | n | ns/query | Semántica |
|------------|------|--:|---------:|-----------|
| docs_scan | global | 1000 | 5140 | \(Q_\tau\) sin índice (RLE en RAM) |
| **metatrie** | global | 1000 | **2806** | \(Q_\tau\) nativa |
| docs_scan | per-term | 1000 | 5007 | idem |
| **metatrie** | per-term | 1000 | **2112** | \(Q_\tau\) nativa |
| zdd_qmem_baseline | u+t | 200 | 129–190 | Membresía \(S\in F_t\); forest V=200 sin optimize |

**Importante:** la fila ZDD **no** es \(Q_\tau\). Es otra operación, sobre una muestra de términos que caben en el forest. Sirve como referencia de costo de membership ZDD, no para decir “el ZDD es X veces más rápido en \(Q_\tau\)”.

---

## 11. Relación con BGPs / paper (ocupa / reutiliza / cambia)

Detalle: [`METATRIE_CPP_PARA_PROFESOR.md`](METATRIE_CPP_PARA_PROFESOR.md) **§11**.

| | |
|--|--|
| **Ocupa** | Teoría CLTJ: suelos, updates, wavelet B/E, `leap`, intervalos half-open |
| **Reutiliza** | Algoritmos **copiados** a `edd_metatrie/` — el monorepo BGPs **no se modifica ni se linkea** |
| **No usa** | 18 tries, grafo RDF, LTJ multi-join, `is_partial` |
| **Cambia / aporta** | `.docs`+RLE, modos global/per-term, masters u64, 3 parches (último suelo, leap, rebind SDSL), \(Q_\tau\), BPI/bench |

Lectura del fuente: `main` → `build_global` → RLE → barrido → `create_time_first_trie` → WM → `values_at`.

---

## 12. Mapa de documentación

| Documento | Contenido |
|-----------|-----------|
| **Este archivo** | Guía única: wavelet §3, flujo end-to-end §4, funciones, resultados. |
| [`METATRIE_CPP_PARA_PROFESOR.md`](METATRIE_CPP_PARA_PROFESOR.md) | Bloque a bloque + **§11 ocupa/reutiliza/cambia vs BGPs**. |
| [`METATRIE_CODIGO_PASO_A_PASO.md`](METATRIE_CODIGO_PASO_A_PASO.md) | Orden de llamadas cruzado con BGPs y el paper. |
| [`METATRIE_TESIS.md`](METATRIE_TESIS.md) | Modelo, parches §3.3, pipeline función a función, LaTeX. |
| [`CONTEXTO_CONVERSACION_METATRIE_TESIS.md`](CONTEXTO_CONVERSACION_METATRIE_TESIS.md) | Notas del hilo de desarrollo. |
| [`edd_metatrie/README.md`](../edd_metatrie/README.md) | Compilar, CLI, tablas de bench. |

---

## 13. Pendiente / fuera de alcance actual

- Updates incrementales sobre el trie (solo build estático desde `.docs`).
- Forest ZDD con todos los términos del wiki 2GB en el bench de membresía (hoy `max_terms=200` por RAM/tiempo).
- Optimize CUDD en el path de query-bench (explícitamente **excluido**: se usa baseline `build`).
- Comparar \(Q_\tau\) “justa” con ZDD requeriría implementar recuperación de snapshot desde el ZDD (hoy no existe API nativa).
