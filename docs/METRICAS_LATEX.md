# Sección Métricas — texto LaTeX (BPI Backbone 2 / plus_t)

Texto completo Backbone~2 + Métricas (+ subsección optimize): [`BACKBONE_2_LATEX.md`](BACKBONE_2_LATEX.md).

**Fuente principal:** `resultados_test/plus_t_bpi_ladder.csv` (post `NZDD_INIT_UNIQUE_SLOTS=8`, julio 2026).

Comparativa histórica Backbone 1 vs 2: `resultados_test/edd_bpi_ladder.csv` (incluye `ZDD sin tags`).

**Reordenamiento (ago 2026):**
- `resultados_test/optimize_sweep_wiki_100mb_plus_t_20260802_172326.csv`
- `resultados_test/optimize_sweep_wiki_2gb_plus_t_bin_20260802_183133.csv`
- Notebook: celdas 5–6 de `scripts/analisis_CUDD.ipynb`

**Nota:** `bpi_mem` mide RAM post-load (proceso CUDD). Para el tamaño del DAG
usar **`bpi_edd`** (`edd_nodes × 32 × 8 / n_raw`). `bpi_build` del CSV de evolución
es pre-trim y solo diagnóstico (columna calculada por `NzddBpi::compute()` en C++).
Las consultas CUDD de `verify` corren en build y loaded; ver [PIPELINE_UIHRDC_CUDD.md](PIPELINE_UIHRDC_CUDD.md#consultas-cudd-en-verify-q1-y-q2).

Implementación en [`plus_t/utils/bpi.h`](../plus_t/utils/bpi.h) y [`plus_t/utils/bpi_scan.h`](../plus_t/utils/bpi_scan.h).

## Denominadores y artefacto de escala

Un posting es un entero `(master, rel)` packed64 (64 bpi crudo). Cuatro denominadores:

| Símbolo | Definición |
|---------|------------|
| `n_raw` | Suma de longitudes de posting list (`Total_Ints`) |
| `n_pairs_uniq` | Pares distintos por término |
| `n_snap_elems` | Elementos en snapshots distintos (lo que guarda el ZDD) |
| `n_masters_uniq` | Masters distintos por término |

El cociente `n_raw / n_snap_elems` cae de **351×** (100 MB) a **3.7×** (2 GB). Por eso
`bpi_edd` sobre `n_raw` sube de 0.51 → 35 bpi en el ladder, mientras que sobre
`n_snap_elems` se mantiene ~130 bpi (la estructura no empeora al escalar; el denominador
deja de estar inflado por snapshots repetidos).

Medir ambos lados: `bpi_edd` (sobre `n_raw`, cara al usuario) y `bpi_edd_over_stored`
(sobre `n_snap_elems`, eficiencia intrínseca). Herramienta: `scripts/audit_docs_ints`.

## Cotas de encoding del DAG

`bpi_edd` usa 32 B/nodo porque CUDD almacena `DdNode` completos en RAM, y de esos
32 B sólo 20 son información del DAG (`index` + dos punteros); `ref` y `next` son
bookkeeping del motor de construcción. El `.zpack` serializa esos 20 B/nodo. Las
tres cotas siguientes describen el mismo DAG con encodings progresivamente más
ajustados; se derivan de `edd_nodes` (N) y `numZddVars` (V), no se miden.

| Métrica | Bits totales | Qué asume |
|---------|--------------|-----------|
| `bpi_zdd_std` | `2N⌈log₂(N+1)⌉ + N⌈log₂(V+1)⌉` | Índice de variable + 2 punteros por nodo, punteros estrechados |
| `bpi_level_grouped` | `2N⌈log₂(N+1)⌉ + V⌈log₂(N+1)⌉` | Nodos agrupados por nivel: la etiqueta de variable se amortiza en V fronteras |
| `bpi_dag_counting` | `2·log₂((N+1)!) + V⌈log₂(N+1)⌉` | Orden topológico: el nodo *i* sólo apunta a los anteriores, así que sus punteros cuestan `log₂(i+1)` |

**`bpi_zdd_std` no es una cota inferior.** Es exactamente la línea base *standard
ZDD* de la literatura de ZDDs compactos — Matsuda, Denzumi y Sadakane,
«Storing Set Families More Compactly with Top ZDDs», *Algorithms* 14(6):172, 2021,
la escriben como `2n⌊log n⌋ + n⌊log c⌋` con `c` = tamaño del universo. En ese
trabajo tanto DenseZDD como Top ZDD quedan **por debajo** de esa línea (en las
familias tipo knapsack, DenseZDD ≈ 0.71× y Top ZDD ≈ 0.66×). El piso real es
`bpi_dag_counting`. Se conserva la clave de salida `bpi_edd_min` como alias de
`bpi_zdd_std` por compatibilidad de parsers.

### Valores medidos a 2 GB (`log`, N = 122 197 799 postings)

| | baseline | `nodes_desc+sift` |
|---|---|---|
| `edd_nodes` | 16 931 565 | 14 016 376 |
| `numZddVars` | 4 346 | 4 346 |
| `bpi_edd` (CUDD RAM, 256 bit/nodo) | 35.47 | **29.36** |
| `bpi_file` (`.zpack`, 160 bit/nodo) | 22.30 | 18.48 |
| `bpi_zdd_std` (63 → 61 bit/nodo) | 8.73 | 7.00 |
| `bpi_level_grouped` (50.0 → 48.0 bit/nodo) | 6.93 | 5.51 |
| `bpi_dag_counting` (45.1 → 44.6 bit/nodo) | 6.26 | **5.12** |

Dos lecturas. Primera: el término de etiquetas de variable es casi todo holgura —
con V = 4 346 y N ≈ 14–17 M, agrupar por nivel elimina 13 de los 63 bits/nodo
(`V⌈log₂ N⌉` amortizado son 0.007 bits/nodo). Segunda: el gap entre `bpi_edd` y
`bpi_dag_counting` es de representación CUDD, no de estructura del índice.

### Comparación con la literatura de índices invertidos

Ottaviano y Venturini, «Partitioned Elias-Fano Indexes», SIGIR'14, Tabla 2 — bits
por docID sobre el índice completo (Gov2: 5 742 630 292 postings; ClueWeb09:
15 857 983 641):

| Método | Gov2 doc bpi | ClueWeb09 doc bpi |
|--------|--------------|-------------------|
| Interpolative | 4.03 | 5.33 |
| EF ε-optimal (PEF) | 4.10 | 5.85 |
| OptPFD | 4.72 | 6.42 |
| EF uniform | 4.63 | 6.58 |
| EF single | 7.53 | 7.46 |
| Varint-G8IU | 10.60 | 10.99 |

El protocolo de esa literatura: denominador = total de postings de la colección
completa (equivale a `n_raw`), streams de docID y frecuencia reportados por
separado, índice completo sin muestreo, y espacio medido sobre la **estructura
serializada y consultable** (memory-mapped), no sobre la RAM del constructor.
Por eso `bpi_edd` (35.47) no es comparable con los 4.10 de PEF: no mide lo mismo.
La cifra defendible es `bpi_file`, y la comparable en serio sería una
serialización estilo DenseZDD del `.zpack`.

**Baseline correcto para colecciones versionadas.** PEF no explota redundancia
inter-versión, que es precisamente lo que el ZDD comparte vía nodos. El
competidor legítimo es la línea de colecciones repetitivas: Claude, Fariña,
Martínez-Prieto y Navarro, «Universal indexes for highly repetitive document
collections», *Inf. Syst.* 61:1–23, 2016 (Re-Pair / LZMA / RLE sobre d-gaps), que
según el survey de Pibiri y Venturini le gana a los encoders específicos de IR en
colecciones tipo versiones de Wikipedia.

**Nota sobre «1–2 bits por nodo».** Hansen, Rao y Tiedemann, «Compressing Binary
Decision Diagrams», ECAI 2008, reportan 0.17–9.92 bits/nodo (mediana ~1) sobre
BDDs reales. Pero el propio título acota: *«in those cases where random access is
not required»* — requiere descomprimir el diagrama completo. Sirve como cota para
un tier frío o archivado, no como representación consultable.

## Resumen bpi_mem (plus_t)

| Escala | plus_t log | plus_t u+t | Δ u+t vs log |
|--------|------------|------------|--------------|
| 100 MB | 16.58 | 18.01 | +8.6% |
| 1 GB | 30.25 | 36.49 | +20.6% |
| 2 GB | 45.42 | 47.06 | +3.6% |

Valores de referencia; regenerar con celda 4 de `scripts/analisis_CUDD.ipynb`.

## Comparativa histórica (edd_bpi_ladder.csv)

| Escala | BB1 sin tags | BB2 log | BB2 u+t |
|--------|--------------|---------|---------|
| 100 MB | 15.57 | 16.58 | 18.01 |
| 1 GB | 28.88 | 30.25 | 36.49 |
| 2 GB | 44.30 | 45.42 | 47.06 |

## Optimización post-build (`optimize`) — bpi_edd

| Escala | Pack | Mejor ok | Δ nodos / bpi_edd | Tiempo |
|--------|------|----------|-------------------|--------|
| 100 MB | `wiki_100mb_plus_t` | `sift_conv` | −2.03% (0.509→0.499) | ~1.6 s |
| 1 GB | `wiki_1gb_plus_t_bin` (log) | `df_desc+sift_conv` | −23.9% (21.65→16.48) | ~3950 s |
| 2 GB | `wiki_2gb_plus_t_bin` (log) | `nodes_desc+sift` | −17.2% (35.47→29.36) | ~2010 s |
| 2 GB | idem | `*_conv` / `random_pivot` | — | TIMEOUT ≥10800 s (5/14) |

El reorder es **post-build** sobre el `DdManager` global; no ocurre durante la
inserción de términos. Detalle LaTeX: subsección *Optimización por
reordenamiento CUDD* en [`BACKBONE_2_LATEX.md`](BACKBONE_2_LATEX.md).
