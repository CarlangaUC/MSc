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
ajustados; se derivan de `edd_nodes` (N) y `levels_used` (L), no se miden.

| Métrica | Bits totales | Qué asume |
|---------|--------------|-----------|
| `bpi_zdd_std` | `2N⌈log₂(N+1)⌉ + N⌈log₂(L+1)⌉` | Índice de variable + 2 punteros por nodo, punteros estrechados |
| `bpi_level_grouped` | `2N⌈log₂(N+1)⌉ + L⌈log₂(N+1)⌉` | Nodos agrupados por nivel: la etiqueta de variable se amortiza en L fronteras |
| `bpi_dag_counting` | `2·log₂((N+1)!) + L⌈log₂(N+1)⌉` | Orden topológico: el nodo *i* sólo apunta a los anteriores, así que sus punteros cuestan `log₂(i+1)` |

`L` = niveles que el DAG ocupa de verdad, contados exactos como `varIndex`
distintos del `.zpack` (`levels_used`, con `levels_exact=1`). Se midió para
descartar que las cotas cobren fronteras de niveles vacíos, y no lo hacen:
`L = numZddVars − 1` en los seis puntos del ladder, porque en `u+t` cada término
gasta su propio nivel de tag y en `log` los `tagWidth` bits se usan todos. Cuando
no hay `.zpack` a mano (`optimize`) se cae a `numZddVars` y se marca
`levels_exact=0`; el techo mueve las cotas menos de 0.01 %.

### Conteo de nodos

`edd_nodes` es ahora el conteo **exacto por alcanzabilidad** desde las raíces del
bosque (`Cudd_SharingSize`, expuesto como `cuddForestNodeCount`), no la resta
`pool − cadena_univ` de antes. La resta dependía del estado del recolector y de
que `univ` tuviera exactamente un nodo por variable; la alcanzabilidad es la
definición del DAG compartido y no depende de nada. Ambos se emiten
(`edd_nodes_exact`, `edd_nodes_pool`, `edd_nodes_delta`) como control cruzado.

La diferencia son los terminales alcanzables, que la resta no contaba: +2 a
100 MB (algún término vacío hace alcanzable también el terminal `zero`) y +1 a
1 GB y 2 GB. Es decir, `bpi_edd` a 100 MB pasa de 0.50895 a **0.50904**, y a
1 GB / 2 GB no se mueve en cinco cifras significativas (21.3417 / 35.2086).

**`bpi_zdd_std` no es una cota inferior.** Es exactamente la línea base *standard
ZDD* de la literatura de ZDDs compactos — Matsuda, Denzumi y Sadakane,
«Storing Set Families More Compactly with Top ZDDs», *Algorithms* 14(6):172, 2021,
la escriben como `2n⌊log n⌋ + n⌊log c⌋` con `c` = tamaño del universo. En ese
trabajo tanto DenseZDD como Top ZDD quedan **por debajo** de esa línea (en las
familias tipo knapsack, DenseZDD ≈ 0.71× y Top ZDD ≈ 0.66×). El piso real es
`bpi_dag_counting`.

La clave de salida `bpi_edd_min` fue **eliminada**: era un alias literal de
`bpi_zdd_std` y su nombre sugería un mínimo que esa fórmula no es. También se
eliminó `bits_per_stored_elem`, que calculaba exactamente lo mismo que
`bpi_edd_over_stored`. Los parsers deben leer `bpi_zdd_std` y
`bpi_zdd_std_over_stored`.

### Valores medidos a 2 GB (`log`, N = 122 197 799 postings)

| | baseline | `nodes_desc+sift` |
|---|---|---|
| `edd_nodes` | 16 931 566 | 14 016 378 |
| `numZddVars` / `levels_used` | 4 346 / 4 345 | 4 346 / 4 345 |
| `bpi_edd` (CUDD RAM, 256 bit/nodo) | 35.47 | **29.36** |
| `bpi_file` (`.zpack`, 160 bit/nodo) | 22.30 | 18.48 |
| `bpi_zdd_std` (63 → 61 bit/nodo) | 8.73 | 7.00 |
| `bpi_level_grouped` (50.0 → 48.0 bit/nodo) | 6.93 | 5.51 |
| `bpi_dag_counting` (45.1 → 44.6 bit/nodo) | 6.26 | **5.12** |

Dos lecturas. Primera: el término de etiquetas de variable es casi todo holgura —
con L = 4 345 y N ≈ 14–17 M, agrupar por nivel elimina 13 de los 63 bits/nodo
(`L⌈log₂ N⌉` amortizado son 0.007 bits/nodo). Segunda: el gap entre `bpi_edd` y
`bpi_dag_counting` es de representación CUDD, no de estructura del índice.

Una tercera, que sólo se ve al medir `u+t` y `log` juntos: `bpi_zdd_std` premia a
`log` (8.73 contra 9.35 en `u+t`) porque su L es 4 345 en vez de 254 877, y ese L
entra multiplicado por N en el término de etiquetas. Pero `bpi_level_grouped` da
6.9288 en las dos codificaciones. Es decir, la ventaja aparente de `log` en la
cota estándar es un artefacto de cobrar la etiqueta por nodo: en cuanto se
amortiza por nivel, las dos codificaciones representan el mismo DAG al mismo
coste. La diferencia real entre ellas está en `edd_nodes` (16.81 M contra
16.93 M), no en el encoding.

### Benchmark formatos wiki_2gb (2026-09-03, u+t, nodes_desc+sift)

Script: [`scripts/benchmark_formatos_wiki_2gb.sh`](../scripts/benchmark_formatos_wiki_2gb.sh).
Tabla completa: [`resultados_test/benchmark_formatos_wiki_2gb.md`](../resultados_test/benchmark_formatos_wiki_2gb.md).

| formato | stage | V | n_raw | edd_nodes | **bpi_edd** | bpi_edd_over_stored |
|---|---:|---:|---:|---:|---:|---:|
| sucio | baseline | 250 550 | 122.2 M | 16.81 M | **35.21** | 131.8 |
| sucio | optimized | 250 550 | 122.2 M | 13.88 M | **29.07** | 108.8 |
| marcado | baseline | 188 472 | 89.3 M | 11.70 M | **33.57** | 127.7 |
| marcado | optimized | 188 472 | 89.3 M | 9.58 M | **27.49** | 104.6 |
| torsen | baseline | 150 062 | 79.1 M | 10.51 M | **34.00** | 126.1 |
| torsen | optimized | 150 062 | 79.1 M | 8.58 M | **27.76** | 103.0 |

Lectura. El formato de texto mueve `bpi_edd` solo ~3–5 % en baseline; el reorder ~18 %
en los tres formatos. Mejor global: marcado + optimized (27.49). El cuello de botella
a 2 GB es el versionado (`ratio_raw/stored ≈ 3.7`), no el wikitext residual.

### Efecto de limpiar wikitext (medido, u+t)

Pipeline: [`scripts/pipeline_limpieza_bpi.sh`](../scripts/pipeline_limpieza_bpi.sh).
El marcado wiki inflaba `n_raw` con términos de puntuación (`[[`, `|`, `'''`) y
palabras de plantilla (`Category`, `quot`, `http`). Medido en wiki_2gb sucio:
**22 923 términos (9.15 % de V) son marcado puro → 11.54 % de `n_raw`**.

| | wiki_100mb sucio | marcado | torsen | wiki_2gb sucio | marcado | torsen |
|---|---:|---:|---:|---:|---:|---:|
| `V` (términos) | 9 774 | 7 132 | 5 915 | 250 550 | 188 472 | 150 062 |
| `n_raw` | 5.42 M | 3.72 M | 3.22 M | 122.2 M | 89.3 M | 79.1 M |
| `edd_nodes` | 10 787 | 7 752 | 6 444 | 16.81 M | 11.70 M | 10.51 M |
| **`bpi_edd`** | **0.509** | **0.534** | **0.513** | **35.21** | **33.57** | **34.00** |
| `bpi_edd_over_stored` | 178.9 | 186.1 | 183.9 | 131.8 | 127.7 | 126.1 |
| `ratio_raw_over_stored` | 351.5 | 348.4 | 358.8 | 3.74 | 3.80 | 3.71 |

Lecturas. Primera: a 100 MB la limpieza **sube** `bpi_edd` (0.509 → 0.534 en
marcado) porque el denominador cae más que el numerador: el marcado generaba
postings baratos y muy compartidos (`[[` en 145 K docs) que el ZDD deduplicaba
bien. Segunda: a 2 GB la limpieza **baja** `bpi_edd` (~5 %), porque el efecto de
escala domina y el ruido deja de ser tan barato de colapsar. Tercera: `bpi_edd_over_stored`
(b métrica intrínseca) baja ~3–4 % en 2 GB: la EDD mejora levemente al quitar ruido.
Cuarto: torsen reduce más `V` y `n_raw` que marcado solo, pero no siempre mejora
más el BPI — la normalización de caso/puntuación es un efecto separado del marcado.

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

## Metatrie EDD (`meta_trie_edd`) — denominadores y métricas

Misma regla: \(\mathrm{BPI} = 8 \times \mathrm{bytes} / n_{\mathrm{raw}}\) (`plus_t/utils/bpi.h`).

| Campo binario | Significado en tesis |
|---------------|----------------------|
| `bpi_file` | Trie `time_first_trie` (lo que va en `.emt`) |
| `bpi_total` | Igual a `bpi_file` (trie único; ST[OP] auxiliar retirado oct 2026) |

**No comparar** `bpi_over_stored` metatrie con `bpi_edd_over_stored` ZDD sin aclarar definiciones (`n_snap_elems` vs `n_pairs_uniq` local).

**Wiki 2 GB (corrida final 2026-10-04):** per-term `bpi_file` ≈ 5.996, `bpi_total` ≈ 10.19; global `bpi_file` ≈ 4.563, `bpi_total` ≈ 9.35. CSV: `resultados_test/meta_trie_edd_wiki_2gb_final_*.csv`. Sección LaTeX: [`METATRIE_TESIS.md`](METATRIE_TESIS.md).
