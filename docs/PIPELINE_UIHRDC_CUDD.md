# Pipeline: uiHRDC → CUDD → logs de evolución

**Documento canónico** del pipeline operativo (pasos, CLI, arquitectura, funciones).  
Índice breve del repo: [readme.md](../readme.md) · Texto de tesis:
[BACKBONE_2_LATEX.md](BACKBONE_2_LATEX.md) · Scripts/notebooks:
[scripts/README.md](../scripts/README.md) · Stress:
[scripts/stress/README.md](../scripts/stress/README.md)

Los **pasos 1–2** son comunes. El **paso 3 activo** es `zdd_cudd_plus_t` (Backbone 2).
El flujo anterior sin tags está archivado en [`scripts_deprecados/`](../scripts_deprecados/).

| Variante | Binario | ZDD^t | Estado |
|---|---|---|---|
| **Actual (con tags)** | `zdd_cudd_plus_t` | `F_t ∪ {{u+t}}` o `F_t ∪ {φ(t)}` | Producción |
| **Anterior (sin tags)** | `nzdd_cudd_serialize` | `F_t` solo | Deprecado → `scripts_deprecados/backbone1_cudd/` |

```
wiki_Ngb.txt + page_mapping
        │
        ▼  [0] limpiar_corpus_wiki.py  (opcional: marcado | torsen)
        │      wiki_Ngb_limpio_<nivel>.txt + .DOCBOUNDARIES.ul
        │
        ▼  [1] BUILD_PFORDELTA_NOTEXT … only_list_and_voc
        │
   listas_wiki_Ngb_versionada  +  index_*.voc
        │
        ▼  [2] convertir_versionado_input_uiHRDC.py
        │
   wiki_Ngb_uihrdc_packed64.docs
        │
        ▼  [3] zdd_cudd_plus_t build|verify u+t|log …
              cudd_evolucion_Ngb_plus_t.csv  +  wiki_Ngb_plus_t.zpack
              ▼ [4] analisis_CUDD.ipynb → bpi_edd (EDD) + bpi_file + bpi_mem/bpi_build (diag.)
```

Orquestador de limpieza + BPI (pasos 0→4): [`scripts/pipeline_limpieza_bpi.sh`](../scripts/pipeline_limpieza_bpi.sh)

**Ningún modo `build`/`load`/`verify` genera `.dot`** en datasets reales. Los diagramas existen solo en `zdd_cudd_plus_t demo u+t|log`.

---

## Requisitos previos

| Componente | Ubicación / acción |
|---|---|
| Repo MAGISTER | `/root/MAGISTER` |
| uiHRDC compilado | `uiHRDC/uiHRDC/indexes/NOPOS/II_docs/BUILD_PFORDELTA_NOTEXT` |
| CUDD compilado | `cudd/cudd/.libs/libcudd.so` |
| Dataset texto | `uiHRDC/uiHRDC/data/texts/wiki_1gb.txt` |
| Fronteras doc | `wiki_1gb.txt.DOCBOUNDARIES.ul` (misma carpeta) |
| Mapping versiones | `page_mapping_wiki_1gb.bin` (obligatorio para versionado) |
| Python análisis | `python3-pandas python3-matplotlib python3-seaborn python3-nbconvert` |

### Compilar uiHRDC (una vez)

En `uiHRDC/uiHRDC/indexes/NOPOS/II_docs/Makefile`, `CFLAGS` debe incluir `-no-pie`:

```makefile
export CFLAGS = -O9 -m64 -no-pie
```

```bash
cd /root/MAGISTER/uiHRDC/uiHRDC/indexes/NOPOS/II_docs
make clean
bash compile.sh
# → BUILD_PFORDELTA_NOTEXT, SEARCH_PFORDELTA_NOTEXT, …
```

### Compilar CUDD + binarios ZDD (una vez)

```bash
cd /root/MAGISTER

# librería CUDD
cd cudd/cudd && autoreconf -i && ./configure && make -j$(nproc) && cd ../../..

# Flujo anterior — bosque sin tags (producción / bpi / pbuild)
g++ -O2 -std=c++17 -fopenmp -o scripts_deprecados/backbone1_cudd/nzdd_cudd_serialize \
  scripts_deprecados/backbone1_cudd/zdd_cudd.cpp \
  -I ./cudd/cudd -I ./cudd -L ./cudd/cudd/.libs \
  -Wl,-rpath,'$ORIGIN/cudd/cudd/.libs' -lcudd \
  -I ./TdZdd/include \
  -I ./uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils

# Flujo actual — bosque con tag canónico (`u+t`) o binario (`log`)
g++ -O2 -std=c++17 -fopenmp -o zdd_cudd_plus_t plus_t/main.cpp \
  -I plus_t -I . -I ./cudd/cudd -I ./cudd -L ./cudd/cudd/.libs \
  -Wl,-rpath,'$ORIGIN/cudd/cudd/.libs' -lcudd \
  -I ./TdZdd/include \
  -I ./uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils
```

---

## Paso 0 — Limpiar wikitext (opcional)

Los dumps wiki vienen en **wikitext de MediaWiki** (`{{plantillas}}`, `[[enlaces]]`,
entidades `&lt;`, `&quot;`, etc.). uiHRDC indexa las corridas de puntuación como
términos (`[[`, `|`, `'''`, `==`), inflando `n_raw` y el vocabulario. Torsen y el
wiki2g oficial de uiHRDC usan formatos distintos: torsen es `[a-z0-9 ]` puro; el
wiki2g del paper también viene sucio (tokens como `htmlSultan`, `2003Lowest`).

Script: [`scripts/limpiar_corpus_wiki.py`](../scripts/limpiar_corpus_wiki.py)

| Nivel | Qué hace |
|---|---|
| `marcado` | Quita wikitext/HTML; conserva mayúsculas y puntuación de prosa |
| `torsen` | `marcado` + minúsculas + solo `[a-z0-9 ]` (formato torsen) |

| Backend | Librería | Notas |
|---|---|---|
| `regex` | propio | **Default producción**; más agresivo en wiki versionado |
| `mwph` | mwparserfromhell | Estándar MediaWiki (`strip_code`) |
| `wtp` | wikitextparser | Estándar alternativo (`plain_text`) |

Comparación reproducible: [`scripts/comparar_backends_limpieza.py`](../scripts/comparar_backends_limpieza.py).

**Restricción crítica:** preserva líneas 1:1 (1 línea = 1 revisión). `page_mapping.bin`
se copia sin cambios; solo se regenera `.DOCBOUNDARIES.ul`.

```bash
# Solo limpieza (default: regex)
python3 scripts/limpiar_corpus_wiki.py \
  --input uiHRDC/uiHRDC/data/texts/wiki_2gb.txt \
  --output resultados_test/wiki_2gb_limpio_marcado.txt \
  --nivel marcado

# Backend estándar declarable (tesis)
python3 scripts/limpiar_corpus_wiki.py \
  --input uiHRDC/uiHRDC/data/texts/wiki_100mb.txt \
  --output resultados_test/wiki_100mb_limpio_marcado_mwph.txt \
  --nivel marcado --backend mwph

# Pipeline completo (limpiar → uiHRDC → .docs → ZDD → BPI baseline → optimize → BPI)
./scripts/pipeline_limpieza_bpi.sh wiki_100mb marcado
./scripts/pipeline_limpieza_bpi.sh wiki_2gb marcado              # default HEUR=nodes_desc+sift
OPTIMIZE=0 ./scripts/pipeline_limpieza_bpi.sh wiki_2gb marcado   # solo baseline
BACKEND=mwph ./scripts/pipeline_limpieza_bpi.sh wiki_2gb marcado
```

Corpus ≥ 500 MB usa modo **streaming** (memoria O(1) por línea).

Backends estándar (`mwph`, `wtp`) requieren `.venv/bin/pip install mwparserfromhell wikitextparser`.
En muestras wiki_100mb el regex deja **menos marcado residual** que mwph/wtp (~86 % Jaccard);
ver `comparar_backends_limpieza.py` para reproducir.

---

## Paso 1 — uiHRDC: exportar listas + vocabulario

Modo **`only_list_and_voc`**: construye posting lists en **uint64 packed (40/24)** y escribe el vocabulario, **sin** PForDelta ni índice comprimido completo. Es el modo pensado para alimentar TdZdd/CUDD.

Desde la raíz del proyecto:

```bash
cd /root/MAGISTER/uiHRDC/uiHRDC/indexes/NOPOS/II_docs

./BUILD_PFORDELTA_NOTEXT \
  /root/MAGISTER/uiHRDC/uiHRDC/data/texts/wiki_1gb.txt \
  /root/MAGISTER/uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named \
  "only_list_and_voc"
```

### Archivos de entrada (misma carpeta que el `.txt`)

| Archivo | wiki_1gb |
|---|---|
| Texto | `wiki_1gb.txt` |
| Doc boundaries | `wiki_1gb.txt.DOCBOUNDARIES.ul` |
| Page mapping | `page_mapping_wiki_1gb.bin` |

Si falta `page_mapping_*.bin`, el build corre en modo **no versionado** y el export se llama `listas_<basename>` (sin sufijo `_versionada`).

### Salidas esperadas

| Artefacto | Ruta (wiki_1gb) |
|---|---|
| Posting lists (texto) | `uiHRDC/uiHRDC/data/texts/listas_wiki_1gb_versionada` |
| Vocabulario | `uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named.voc` |

Formato de cada línea en `listas_*`:

```text
T[42]: 526909256 526909257 526909258
```

Cada entero es un **uint64 packed (master, rel)** cuando hay versionado.

### Log esperado (fragmentos)

```text
[ZDD] occList packed64 40/24 stats: ...
[ZDD] Modo only_list_and_voc: listas 64b exportadas; omitiendo PForDelta/texto
[ZDD] Vocabulario guardado: .../index_wiki_1gb_named.voc
[ZDD] Build only_list_and_voc OK: listas_* + index_wiki_1gb_named.voc
```

### Convención de nombres `listas_*`

El motor escribe en el directorio del `.txt`:

- Versionado: `listas_<nombre_txt_sin_extension>_versionada`
- No versionado: `listas_<nombre_txt_sin_extension>`

Ejemplos:

| Dataset | Archivo exportado |
|---|---|
| `wiki_100mb.txt` | `listas_wiki_100mb_versionada` |
| `wiki_1gb.txt` | `listas_wiki_1gb_versionada` |
| `torsen.text200mb.txt` | `listas_torsen.text200mb` |

---

## Paso 2 — Convertir `listas_*` → `.docs` (entrada CUDD)

El binario CUDD lee **`.docs` packed64**, no el texto `listas_*` directamente.

```bash
cd /root/MAGISTER

python3 scripts/convertir_versionado_input_uiHRDC.py \
  --dataset wiki_1gb \
  --input-listas listas_wiki_1gb_versionada \
  --tuple-output packed64 \
  --output-bin resultados_test/wiki_1gb_uihrdc_packed64.docs
```

Parámetros relevantes:

| Flag | Default | Descripción |
|---|---|---|
| `--dataset` | `wiki_100mb` | Prefijo para rutas inferidas |
| `--input-listas` | `listas_<dataset>_versionada` | Nombre del export uiHRDC |
| `--base-texts` | `uiHRDC/uiHRDC/data/texts` | Carpeta de `listas_*` y `.voc` |
| `--tuple-output` | `packed` | `packed` o `packed64` (sinónimos): uint64 master/rel 40/24 |
| `--master-bits` / `--rel-bits` | 40 / 24 | Debe coincidir con uiHRDC |
| `--output-bin` | `resultados_test/<dataset>_uihrdc_<tuple>.docs` | Salida binaria |

También escribe un sidecar `.docs.meta` con estadísticas de conversión.

### Salida

| Artefacto | Ruta |
|---|---|
| Binario postings | `resultados_test/wiki_1gb_uihrdc_packed64.docs` |
| Metadata | `resultados_test/wiki_1gb_uihrdc_packed64.docs.meta` |

---

## Paso 3 — CUDD: build + log de evolución

Esta sección describe el **flujo anterior** (`nzdd_cudd_serialize`). El flujo con tags está en **[Paso 3b — Bosque con tags](#paso-3b--bosque-con-tags-zdd_cudd_plus_t)**.

### 3a. Build serial con CSV de evolución (sin `.zpack`)

Ideal para medir **bpi** y memoria término a término:

```bash
cd /root/MAGISTER

./scripts_deprecados/backbone1_cudd/nzdd_cudd_serialize build \
  resultados_test/wiki_1gb_uihrdc_packed64.docs \
  uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named.voc \
  none 0 \
  resultados_test/cudd_evolucion_1gb.csv 500
```

Argumentos:

| Posición | Valor wiki_1gb | Significado |
|---|---|---|
| `docs` | `…packed64.docs` | Entrada del paso 2 |
| `voc` | `index_wiki_1gb_named.voc` | Vocabulario del paso 1 |
| `out.zpack` | `none` | No serializar (solo métricas) |
| `max_terms` | `0` | Todos los términos |
| `log_csv` | `cudd_evolucion_1gb.csv` | Ruta del log |
| `log_every` | `500` | Una fila cada N términos |

Columnas del CSV:

```text
Paso,Total_Ints,Nodos_Pool,Bytes_CUDD,RSS_KB,Tiempo_s
```

- **Total_Ints**: enteros acumulados (suma de posting lists) → no hace falta releer `.docs` para bpi.
- **Nodos_Pool**: `Cudd_zddReadNodeCount` durante el build (incluye snapshots temporales).
- **Bytes_CUDD**: memoria del manager CUDD.
- **RSS_KB**: memoria del proceso (SO).

### 3b. Build con persistencia `.zpack` (opcional)

```bash
./scripts_deprecados/backbone1_cudd/nzdd_cudd_serialize build \
  resultados_test/wiki_1gb_uihrdc_packed64.docs \
  uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named.voc \
  resultados_test/wiki_1gb_build.zpack 0
```

### 3c. Build paralelo + verificación (opcional)

```bash
./scripts_deprecados/backbone1_cudd/nzdd_cudd_serialize pbuild \
  resultados_test/wiki_1gb_uihrdc_packed64.docs \
  uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named.voc \
  resultados_test/wiki_1gb_pbuild.zpack 0 12

./scripts_deprecados/backbone1_cudd/nzdd_cudd_serialize verify-pbuild \
  resultados_test/wiki_1gb_uihrdc_packed64.docs \
  uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named.voc \
  0 12
```

### 3d. Comparar contra baseline TdZdd (opcional)

Primero generar sidecar TdZdd:

```bash
g++ -O2 -std=c++17 -fopenmp -o scripts_deprecados/baseline_tdzdd/script_versionado_test_tdzdd \
  scripts_deprecados/baseline_tdzdd/script_versionado_test_tdzdd.cpp \
  -I ./TdZdd/include \
  -I ./uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils

./scripts_deprecados/baseline_tdzdd/script_versionado_test_tdzdd \
  resultados_test/wiki_1gb_uihrdc_packed64.docs \
  uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named.voc \
  resultados_test/wiki_1gb_tdzdd_metrics.csv 0
# → resultados_test/wiki_1gb_tdzdd_metrics.csv.nodes
```

Comparación semántica:

```bash
./scripts_deprecados/backbone1_cudd/nzdd_cudd_serialize compare-tdzdd \
  resultados_test/wiki_1gb_build.zpack \
  resultados_test/wiki_1gb_tdzdd_metrics.csv.nodes \
  uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named.voc \
  Abraham
```

---

## Paso 3b — Bosque con tags (`zdd_cudd_plus_t`, modos `u+t` y `log`)

Fuente principal: [`plus_t/main.cpp`](../plus_t/main.cpp) (dispatcher). Lógica en subcarpetas `engine/`, `cmd/`, `export/`, `demo/`. Entrada idéntica al paso 3a (`.docs` + `.voc`).

### Semántica: flujo anterior vs motor unificado

`F_t` es una **familia de conjuntos** (snapshots). Por ello, el tag que se une también
debe escribirse como una familia con un solo subconjunto:

| | Sin tags (`scripts_deprecados/backbone1_cudd/zdd_cudd.cpp`) | Modo `u+t` | Modo `log` |
|---|---|---|---|
| Definición | `ZDD^t = F_t` | `ZDD^t = F_t ∪ {{u+t}}` | `ZDD^t = F_t ∪ {φ(t)}` |
| Tag físico | No existe | Subconjunto singleton `{u+t}` | Subconjunto de bits `φ(t)={τ_b : bit_b(t)=1}` |
| Identidad lógica | Solo `pointerList[t]` | `{u+t}` | `φ(t)` se decodifica a `{u+t}` |
| `docOffset` | `1` | `V + 1` | `1 + ⌈log₂(V+1)⌉` |
| `numZddVars` | `maxMaster + 2` | `maxMaster + docOffset + 1` | Igual fórmula, con `docOffset` logarítmico |
| Masters | `uint64_t` packed 40/24 | Igual | Igual |

**Universo lógico** (no confundir con índice de var CUDD):

```
masters  m  ∈ [0, U)           U = 2^40  (ZDD_MASTER_BITS)
tags     {u+t} ∈ [U+1, U+V]    V = número de términos del vocabulario
```

**Mapeo CUDD** (vars comprimidas 0 … `numZddVars−1`):

```
modo u+t:
  tag t (1-based)  → var t                         (vars 1 … V)
  docOffset = V + 1

modo log:
  bit b de t       → var (1 + b)                   (τ_b, LSB en b=0)
  tagWidth = ceil(log2(V + 1))
  docOffset = 1 + tagWidth

ambos:
  master m         → var (docOffset + m)
  numZddVars = maxMaster + docOffset + 1
```

La codificación se añade **inline en el bucle por término**
(`addEncodingToFt` en `plus_t/engine/engine.h`):

```cpp
// Por cada término t (0-based en pointerList):
termOneBased = t + 1;
encodingSet = (enc == u+t) ? {u + termOneBased} : phi(termOneBased);
ZDD^t = Union(F_t, {encodingSet});
pointerList[t] = &ZDD^t;
```

`F_t` sí existe como resultado local intermedio mientras se construye el término; lo que
no ocurre es una segunda pasada global para etiquetar el bosque. Después de la unión se
libera la referencia temporal a `F_t` y `pointerList[t]` conserva la raíz identificable.

### Arquitectura modular del flujo actual

| Archivo | Responsabilidad |
|---|---|
| [`plus_t/main.cpp`](../plus_t/main.cpp) | `main()`: dispatcher a modos CLI |
| [`plus_t/engine/engine.h`](../plus_t/engine/engine.h) | Motor compartido; `TagEncoding`, layout, snapshots, ambas codificaciones y lifecycle |
| [`plus_t/export/export.h`](../plus_t/export/export.h) | `saveBuildResult`, `spotCheck`, `metricsEqual`, `usage()` |
| [`plus_t/cmd/build.h`](../plus_t/cmd/build.h) | Orquestación modo `build` |
| [`plus_t/cmd/load.h`](../plus_t/cmd/load.h) | Orquestación modo `load` |
| [`plus_t/cmd/verify.h`](../plus_t/cmd/verify.h) | Orquestación modo `verify` |
| [`plus_t/cmd/optimize.h`](../plus_t/cmd/optimize.h) | Modo `optimize`: heurísticas CUDD + ZPACKv2 |
| [`plus_t/demo/viz.h`](../plus_t/demo/viz.h) | Modo `demo` + export Graphviz |
| [`nzdd_cudd_common.h`](../nzdd_cudd_common.h) | I/O compartido: `.voc`, `.docs`, helpers CUDD |
| [`nzdd_cudd_pack.h`](../nzdd_cudd_pack.h) | Formato `.zpack` (v1 identidad, v2 + invPerm) |
| [`version_packing.h`](../uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils/version_packing.h) | Macros `ZDD_UNPACK_MASTER/REL` (40/24) |
| [`scripts/packed64_layout.py`](../scripts/packed64_layout.py) | Python: mismo layout (lee el header; override `ZDD_*_BITS`) |

### Pipeline interno (build)

```
.docs
  → buildDocsIndex + readPostingListAt          (nzdd_cudd_common.h)
  → collectVersionSnapshots: (m,r) → S_i^v       (ZDD_UNPACK_* en version_packing.h)
  → computeZddLayout(enc):
       u+t → docOffset=V+1
       log → docOffset=1+ceil(log2(V+1))
  → Cudd_Init + configureCuddManager   (misma config en build y en load)
  → buildFtPointersForRange (por término):
       snapshotCache → buildVersionSnapshotZdd
       zddUnionBalancedCudd → F_t
       addEncodingToFt(enc) → F_t ∪ {{u+t}} o F_t ∪ {φ(t)}
       pointerList[t] = &ZDD^t
  → fillMetrics / saveZddPack
```

### Funciones clave del motor

| Función | Archivo | Descripción |
|---|---|---|
| `parseTagEncoding` | `plus_t/engine/engine.h` | Convierte `u+t` / `log` a `TagEncoding` |
| `parseDocsTerms` | `plus_t/engine/engine.h` | Parseo paralelo OpenMP de todas las posting lists |
| `collectVersionSnapshots` | `plus_t/engine/engine.h` | Agrupa por `rel` y deduplica snapshots |
| `buildVersionSnapshotZdd` | `plus_t/engine/engine.h` | Construye un `S_i^v` vía `Cudd_zddChange` |
| `computeZddLayout` | `plus_t/engine/engine.h` | Calcula `tagWidth`, `docOffset` y `numZddVars` según modo |
| `addEncodingToFt` | `plus_t/engine/engine.h` | Une `{u+t}` o `φ(t)` a `F_t` |
| `readTermIdFromTagSearch` | `plus_t/engine/engine.h` | Recupera `term_id` según la codificación |
| `buildFtPointersForRange` | `plus_t/engine/engine.h` | Bucle central y caché compartida de snapshots |
| `buildForest` | `plus_t/engine/engine.h` | Orquesta parseo + build + métricas |
| `saveBuildResult` | `plus_t/export/export.h` | Wrapper de serialización con timing |
| `ZddPack::saveZddPack` | `nzdd_cudd_pack.h` | Serializa bosque a `.zpack` |
| `ZddPack::loadZddPack` | `nzdd_cudd_pack.h` | Deserializa y reconstruye pool |

### Modos de `zdd_cudd_plus_t`

```
build  u+t|log <docs> <voc> <out.zpack|none> [max_terms] [log_csv] [log_every]
load   u+t|log <in.zpack> [voc] [spot_word]
optimize u+t|log <in.zpack> <docs> <out.zpack> <heuristica> [max_sift_vars]
verify u+t|log <docs> <voc> <tmp.zpack> [max_terms]
demo   u+t|log [out_dir]
```

| Modo | Qué hace | ¿Genera `.dot`? |
|---|---|---|
| `build` | Parse `.docs`, bosque compartido CUDD, save opcional `.zpack`, log CSV opcional | **No** |
| `load` | Carga `.zpack` (v1 o v2), pool/RSS y spot-check | **No** |
| `optimize` | Carga `.zpack`, aplica heurística de reorden, guarda ZPACKv2 si el orden cambió | **No** |
| `verify` | Build + tags + round-trip + consultas CUDD (build y loaded) + paridad semántica | **No** |
| `demo` | Toy `u=4`, `V=2`; etiquetas directas (`u+t`) o bits `τ_k` (`log`) | **Sí** (solo toy) |

Argumentos compartidos con el flujo anterior:

- `out.zpack = none` o `-`: build sin escribir archivo.
- `max_terms = 0`: todos los términos del `.docs`.
- `log_csv` / `log_every`: evolución bpi/memoria (mismas columnas que `nzdd_cudd_serialize build`).

### Build + verify (wiki_1gb)

```bash
cd /root/MAGISTER

DOCS=resultados_test/wiki_1gb_uihrdc_packed64.docs
VOC=uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named.voc
PACK=resultados_test/wiki_1gb_plus_t.zpack

./zdd_cudd_plus_t build u+t "$DOCS" "$VOC" "$PACK" 0

./zdd_cudd_plus_t verify u+t "$DOCS" "$VOC" resultados_test/wiki_1gb_plus_t_tmp.zpack 0

# Mismo motor, codificación binaria:
./zdd_cudd_plus_t verify log "$DOCS" "$VOC" resultados_test/wiki_1gb_plus_t_log_tmp.zpack 0
```

Salida esperada de `verify`:

```text
leaked_build_nodes=0 (esperado 0)
tag_mismatches=0/N
roundtrip_mismatches=0/N
query_mismatches=0
overall: PASS
```

`leaked_build_nodes` = `pool_build_trimmed - pool_loaded`. Tras `trimSnapshotRefs` el manager del build
debe tener exactamente los mismos nodos vivos que un manager limpio con el `.zpack` cargado; el exceso son
nodos referenciados pero inalcanzables desde algún `ZDD^t`. Infla `bpi_build` y el RSS del build sin
aportar nada a la EDD, así que un valor distinto de 0 hace fallar `verify`.

#### Comprobaciones estructurales de `verify`

Implementación: [`plus_t/cmd/verify.h`](../plus_t/cmd/verify.h),
consultas en [`plus_t/export/export.h`](../plus_t/export/export.h).

Flujo completo:

```text
build (manager sucio, post-trim)
  → validación de tags + |ZDD^t|
  → Q1 + Q2 sobre bosque en build
  → save .zpack
  → libera manager de build
load (manager limpio, solo nodos alcanzables)
  → round-trip métricas por término
  → Q1 + Q2 sobre bosque cargado
  → paridad build vs loaded
  → spot-check Abraham
```

1. **Tags**: cada `ZDD^t` no vacío decodifica a `t` (`{u+t}` o bits `φ(t)`).
2. **Cardinalidad**: `countSubsets(ZDD^t) == nSnapshots + 1` (el `+1` es el subconjunto tag).
3. **Round-trip**: métricas tras save/load (`dagSize`, `nSubsets`, `termTagLogical`, `masters`).
4. **Consultas CUDD** (empíricas): mismas operaciones en build y en loaded; deben coincidir.
5. **Paridad build/loaded**: demuestra que serializar no altera la semántica consultable aunque
   `pool_loaded < pool_build` y `bytes_cudd_loaded << bytes_cudd_build` (basura del build eliminada).

#### Consultas CUDD en `verify` (Q1 y Q2)

Ambas usan **`Cudd_zddIntersect`** sobre el bosque (`pointerList` en build o `loaded` tras
`loadZddPack`). El ground truth sale del `.docs` reparsed (`termData` / snapshots versionados).

**Selección de términos** (`resolveQueryTerms`):

| Rol | Resolución | Fallback |
|---|---|---|
| **termA** (Q1, lado A de Q2) | palabra `"Abraham"` en `.voc` | `term_id=0` |
| **termB** (lado B de Q2) | palabra `"beta"` en `.voc` | `"0"` numérico → otro término distinto de A |

**Q1 — Membresia de snapshot versionado**

```text
hits = | ZDD^{termA}  ∩  snapshot(S_i^v) |
```

- Toma el **primer snapshot** `S_i^v` de `termA` en el `.docs` (conjunto de `master_id`).
- Construye su ZDD con `buildVersionSnapshotZdd` (vars `docOffset + m` por cada master `m`).
- Intersecta con `ZDD^{termA}`.
- **Esperado**: `hits = 1` (exactamente ese snapshot pertenece a `F_{termA}`; el tag no coincide
  con un snapshot puro de masters).

Ejemplo wiki_2gb (`u+t`):

```text
term_id=40422 palabra='Abraham' |snapshot|=2 hits=1 gt=1
  masters: { 13, 4044 }
```

**Q2 — Intersección entre dos términos**

```text
shared = | ZDD^{termA}  ∩  ZDD^{termB} |
```

- **Esperado**: número de snapshots **idénticos** (como conjuntos de masters) entre `termA` y
  `termB` en el `.docs`.
- Los singletons de tag **no cruzan** entre términos (codificación distinta por `t`), así que la
  intersección solo captura snapshots compartidos, no tags.

Ejemplo wiki_100mb: `Abraham ∩ term_0` → `shared=1` (un snapshot común).
Ejemplo wiki_2gb: `Abraham ∩ beta` → `shared=0`.

**Paridad build vs loaded**

Tras ejecutar Q1 y Q2 en ambas fases:

```text
=== Query parity: build vs loaded ===
Q1 snapshot hits: build=1 loaded=1 OK
Q2 intersect: build=0 loaded=0 OK
semantica empirica build == loaded (misma respuesta a consultas CUDD)
```

Si `build.q1_hits ≠ loaded.q1_hits` o `build.q2_shared ≠ loaded.q2_shared` → `[QUERY_PARITY]`
y `overall: FAIL`.

**Memoria reportada** (wiki_2gb, modo `u+t`, jul 2026):

| Fase | pool nodos | bytes CUDD |
|---|---:|---:|
| build (trimmed) | ~27.5 M | ~1.26 GB |
| loaded | ~17.1 M | ~719 MB (~57%) |

La EDD loaded responde igual en consultas pero ocupa menos RAM: el `.zpack` solo serializa el DAG
alcanzable; el manager de build arrastra nodos muertos estructurales del pool.

#### Reordenamiento de variables (ZPACKv2)

El DAG en ZPACKv1 ya es mínimo **para el orden identidad** (tags, luego masters por id). Para bajar
`bpi_edd` hay que cambiar el orden con CUDD (`Cudd_zddReduceHeap` / `Cudd_zddShuffleHeap`). Sin
persistir la permutación, un reload v1 **reexpande** el DAG al reconstruir con `rebuildInto`.

- **ZPACKv1** — orden identidad implícito (compatible con packs existentes).
- **ZPACKv2** — header + `invPerm[level]=varIndex`; el loader aplica `ShuffleHeap` antes de
  `rebuildInto`. Se escribe automáticamente cuando el manager no está en identidad al guardar.

```bash
./zdd_cudd_plus_t optimize log resultados_test/wiki_1gb_plus_t_bin.zpack \
  resultados_test/wiki_1gb_uihrdc_packed64.docs /tmp/opt.zpack nodes_desc+sift_conv 200
```

Heurísticas compuestas con `+` (p.ej. `nodes_desc+sift_conv`). Solo se admiten las de la
whitelist en [`plus_t/cmd/optimize.h`](../plus_t/cmd/optimize.h): **prohibidas** `linear` /
`linear_conv` (rompen `var→master`) y `reverse` / `shuffle_rand` (retiradas). Barrido:
`optimize sweep ...`.

Ejemplo verificado en wiki_1gb — término `Abraham` (`t=23828`):

```text
DagSize=63 |ZDD^t|=31 tag_var=23829 singleton={u+23829} |M_t|=20
```

Aquí `|F_t|=30` y `|ZDD^t|=31`: son los 30 snapshots más el subconjunto de tag.

Resultados de referencia del motor unificado:

| Modo / dataset | Layout | Nodos `.zpack` | Verificación |
|---|---|---:|---|
| `u+t`, wiki_100mb | `docOffset=9775` | 10 785 | PASS 0/9774 |
| `u+t`, wiki_1gb | `docOffset=147993` | 5 122 668 | PASS 0/147992 |
| `u+t`, wiki_2gb | `docOffset=250551` | 16 806 290 | PASS 0/250550 |
| `log`, wiki_100mb | `tagWidth=14`, `docOffset=15` | 15 671 | PASS 0/9774 |
| `log`, wiki_1gb | `tagWidth=18`, `docOffset=19` | 5 196 663 | PASS 0/147992 |
| `log`, wiki_2gb | `tagWidth=18`, `docOffset=19` | 16 931 564 | PASS 0/250550 |

Los conteos no implican “un nodo exacto por tag”: CUDD comparte subgrafos y `φ(t)`
puede recorrer varios niveles. La reducción garantizada de `log` está en el número de
variables reservadas para tags (`O(log(V+1))` frente a `O(V)`), no en un número fijo de
nodos por término.

### Load + spot-check

```bash
./zdd_cudd_plus_t load u+t resultados_test/wiki_1gb_plus_t.zpack \
  uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named.voc Abraham
```

En `u+t` muestra `tag_var`/`singleton`; en `log`, `tagWidth`/`tag_decoded`. Ambos muestran
`DagSize`, número de subconjuntos y `|M_t|`.

### Modo `demo` — visualización (no usar en 1GB)

Dataset artificial minúsculo para inspeccionar la estructura del bosque:

```bash
./zdd_cudd_plus_t demo u+t resultados_test/demo_u4_t2
./zdd_cudd_plus_t demo log resultados_test/demo_u4_t2_bin
```

Genera:

| Archivo | Contenido |
|---|---|
| `zdd_demo_t1.dot/.png` | ZDD¹ = `{{1,2}, {1,2,3}, {5}}` |
| `zdd_demo_t2.dot/.png` | ZDD² = `{{0}, {6}}` |
| `zdd_demo_bosque.dot/.png` | Bosque compartido: mismos nodos CUDD, entradas ZDD¹/ZDD² |

Implementación en [`plus_t/demo/viz.h`](../plus_t/demo/viz.h), que reutiliza
[`plus_t/engine/engine.h`](../plus_t/engine/engine.h):

- **`PrettyZddDot`**: walker del pool CUDD → Graphviz.
- Nodos con **literales** (5, 1, 2…), no `m1` ni `u+1=5`.
- Colores: amarillo = tag, azul = master; terminales 0/1 en cajas.
- Aristas: `1` sólida (pertenece), `0` punteada (no pertenece).
- **`writePrettyBosqueDot`**: un solo recorrido → subgrafos compartidos visibles (sin prefijos `t0_`/`t1_`).
- **`writeRankConstraints`**: orden vertical mayor→menor (6 arriba, 0 abajo).

Requiere Graphviz (`dot` en PATH). **No invocar en wiki_1gb/2gb**: el `.dot` sería prohibitivo.

### Referencia rápida — ejecutar todos los modos

Binario: `./zdd_cudd_plus_t`. Codificación **`u+t` o `log`** va siempre como **segundo argumento**
(tras el modo). El `.zpack` **no guarda** la codificación: `load`/`verify` deben usar la misma
que en `build`.

Variables de entorno típicas (ajustar dataset):

```bash
cd /root/MAGISTER

# --- wiki_100mb ---
DOCS=resultados_test/wiki_100mb_uihrdc_packed64.docs
VOC=uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc
PACK=resultados_test/wiki_100mb_plus_t.zpack
LOG=resultados_test/cudd_evolucion_100mb_plus_t.csv

# --- wiki_1gb ---
DOCS=resultados_test/wiki_1gb_uihrdc_packed64.docs
VOC=uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named.voc
PACK=resultados_test/wiki_1gb_plus_t.zpack
LOG=resultados_test/cudd_evolucion_1gb_plus_t.csv

# --- wiki_2gb ---
DOCS=resultados_test/wiki_2gb_uihrdc_packed64.docs
VOC=uiHRDC/uiHRDC/data/texts/index_wiki_2gb_named.voc
PACK=resultados_test/wiki_2gb_plus_t.zpack
LOG=resultados_test/cudd_evolucion_2gb_plus_t.csv
```

#### Modo `build` — construir bosque + opcional `.zpack` y CSV de evolución

```bash
# Persistir .zpack (todos los términos)
./zdd_cudd_plus_t build u+t "$DOCS" "$VOC" "$PACK" 0
./zdd_cudd_plus_t build log  "$DOCS" "$VOC" "${PACK%.zpack}_log.zpack" 0

# Solo métricas de evolución (sin .zpack), fila cada 500 términos
./zdd_cudd_plus_t build u+t "$DOCS" "$VOC" none 0 "$LOG" 500
./zdd_cudd_plus_t build log  "$DOCS" "$VOC" none 0 "${LOG%.csv}_log.csv" 500

# Subconjunto de términos (ej. primeros 1000)
./zdd_cudd_plus_t build u+t "$DOCS" "$VOC" none 1000
```

#### Modo `load` — EDD operativa desde `.zpack`

```bash
./zdd_cudd_plus_t load u+t "$PACK"
./zdd_cudd_plus_t load log  "${PACK%.zpack}_log.zpack"

# Con vocabulario + spot-check por palabra
./zdd_cudd_plus_t load u+t "$PACK" "$VOC" Abraham
./zdd_cudd_plus_t load log  "${PACK%.zpack}_log.zpack" "$VOC" Abraham
```

#### Modo `verify` — validación completa + consultas CUDD

Incluye build, tags, round-trip, Q1/Q2 en build y loaded, paridad semántica.

```bash
./zdd_cudd_plus_t verify u+t "$DOCS" "$VOC" /tmp/verify_uplus.zpack 0
./zdd_cudd_plus_t verify log  "$DOCS" "$VOC" /tmp/verify_log.zpack 0

# Guardar log (ej. wiki_2gb)
./zdd_cudd_plus_t verify u+t "$DOCS" "$VOC" /tmp/verify_uplus.zpack 0 \
  2>&1 | tee resultados_test/wiki_2gb_verify_queries_uplus.log
```

#### Modo `demo` — toy visual (Graphviz)

```bash
./zdd_cudd_plus_t demo u+t resultados_test/demo_u4_t2
./zdd_cudd_plus_t demo log  resultados_test/demo_u4_t2_bin
```

#### Flujo anterior sin tags (`nzdd_cudd_serialize`)

```bash
./scripts_deprecados/backbone1_cudd/nzdd_cudd_serialize build "$DOCS" "$VOC" none 0 resultados_test/cudd_evolucion_1gb.csv 500
./scripts_deprecados/backbone1_cudd/nzdd_cudd_serialize build "$DOCS" "$VOC" resultados_test/wiki_1gb_build.zpack 0
./scripts_deprecados/backbone1_cudd/nzdd_cudd_serialize load  resultados_test/wiki_1gb_build.zpack "$VOC" Abraham
```

#### Medidor bpi post-load (notebook ladder)

```bash
g++ -O2 -std=c++17 -o scripts/measure_zpack_bpi scripts/measure_zpack_bpi.cpp \
  -I . -I ./TdZdd/include -I ./cudd -I ./cudd/cudd \
  -I ./cudd/st -I ./cudd/util -I ./cudd/mtr -I ./cudd/epd \
  -L ./cudd/cudd/.libs -Wl,-rpath,'$ORIGIN/../cudd/cudd/.libs' -lcudd

LD_LIBRARY_PATH=./cudd/cudd/.libs \
  ./scripts/measure_zpack_bpi "$PACK" "$DOCS"
```

Emite `bpi_edd` (métrica de la EDD), `bpi_file`, `bpi_mem` (diagnóstico) y el desglose
del overhead del manager: `bytes_cache`, `bytes_subtables`, `bytes_hash_slots`,
`univ_nodes`, `dead_nodes`, `overhead_pct`.

#### Auditor del denominador N

```bash
g++ -O2 -std=c++17 -fopenmp -o scripts/audit_docs_ints scripts/audit_docs_ints.cpp \
  -I . -I ./TdZdd/include -I ./cudd -I ./cudd/cudd \
  -I ./uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils \
  -L ./cudd/cudd/.libs -Wl,-rpath,'$ORIGIN/../cudd/cudd/.libs' -lcudd

./scripts/audit_docs_ints "$DOCS"
```

Contrasta `n_raw` (= `Total_Ints`) contra `n_snap_elems`, que es lo que el bosque
almacena tras deduplicar snapshots. El cociente no es constante entre escalas.

### Diferencias operativas respecto a `nzdd_cudd_serialize`

| Capacidad | `nzdd_cudd_serialize` | `zdd_cudd_plus_t` |
|---|---|---|
| `pbuild` / `verify-pbuild` | Sí | No (solo build serial) |
| `compare-tdzdd` | Sí | No |
| Log CSV bpi | Sí | Sí |
| Tags en ZDD | No | Sí (`u+t` o `log`) |
| Overhead serializado | baseline (~4.97M nodos wiki_1gb) | `u+t`: +2.9% (~5.12M nodos wiki_1gb) |
| Código | monolítico (`scripts_deprecados/backbone1_cudd/zdd_cudd.cpp`) | `plus_t/{engine,cmd,export,demo}/` |

Un `.zpack` no guarda explícitamente `TagEncoding`: al cargarlo se debe pasar el mismo
modo (`u+t` o `log`) con que fue construido. Tampoco se debe interpretar como bosque con
tags un `.zpack` producido por `nzdd_cudd_serialize`.

---

## Paso 4 — Análisis: notebook BPI / memoria (`plus_t`)

```bash
cd /root/MAGISTER

python3 -m nbconvert --to notebook --execute scripts/analisis_CUDD.ipynb \
  --output analisis_CUDD.ipynb
```

O abrir interactivamente: `jupyter notebook scripts/analisis_CUDD.ipynb`

El notebook (`ENCODING = "u+t" | "log"`) lee `resultados_test/cudd_evolucion_*_plus_t*.csv` y genera:

1. **Celda 1** — evolución build: pool, memoria, **bpi_build** (pre-trim, diagnóstico)
2. **Celda 2** — exploratorio versionado (`.docs`)
3. **Celda 3** — resumen build (incluye bpi_build pre-trim)
4. **Celda 4** — ladder **bpi_edd** / **bpi_file** / **bpi_mem** post-load → `plus_t_bpi_ladder.csv`

**Métrica definitiva:** `bpi_edd = nodos_DAG × sizeof(DdNode) × 8 / N` tras `load` (celda 4).

`bpi_mem = Cudd_ReadMemoryInUse × 8 / N` mide el proceso completo, no la EDD: incluye la
caché de operaciones, una `DdSubtable` por variable ZDD, los slots de la tabla única, la
cadena `univ` de `Cudd_Init` y los nodos muertos sin recolectar. En `wiki_100mb` eso es
el 97 % del total y en `wiki_1gb` el modo `u+t` paga ~48 MB extra sobre `log` sólo por
tener una variable por término. `bpi_build` del CSV tampoco representa la EDD operativa
(manager pre-trim, con todos los snapshots compartidos aún vivos).

Comparativa histórica Backbone 1: `resultados_test/edd_bpi_ladder.csv` (generado antes del aislamiento plus_t).

---

## Script todo-en-uno (wiki_1gb)

Asume uiHRDC ya compilado y `listas_wiki_1gb_versionada` existente (o ejecuta paso 1).

```bash
set -e
ROOT=/root/MAGISTER
cd "$ROOT"

DOCS=resultados_test/wiki_1gb_uihrdc_packed64.docs
VOC=uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named.voc
LISTAS=uiHRDC/uiHRDC/data/texts/listas_wiki_1gb_versionada
LOG=resultados_test/cudd_evolucion_1gb.csv

# [1] uiHRDC export (omitir si listas_* ya existe)
# cd uiHRDC/uiHRDC/indexes/NOPOS/II_docs
# ./BUILD_PFORDELTA_NOTEXT "$ROOT/uiHRDC/uiHRDC/data/texts/wiki_1gb.txt" \
#   "$ROOT/uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named" "only_list_and_voc"
# cd "$ROOT"

# [2] listas → .docs
python3 scripts/convertir_versionado_input_uiHRDC.py \
  --dataset wiki_1gb \
  --input-listas listas_wiki_1gb_versionada \
  --tuple-output packed64 \
  --output-bin "$DOCS"

# [3] CUDD build + log
./scripts_deprecados/backbone1_cudd/nzdd_cudd_serialize build "$DOCS" "$VOC" none 0 "$LOG" 500

# [4] notebook
python3 -m nbconvert --to notebook --execute scripts/analisis_CUDD.ipynb \
  --output analisis_CUDD.ipynb

echo "OK: $LOG"
```

---

## Otros datasets (wiki_100mb, wiki_2gb, …)

Sustituir `wiki_1gb` por el id del dataset en todos los paths. Referencia de rutas: [`scripts/stress/dataset_ladder.json`](../scripts/stress/dataset_ladder.json).

| Dataset | `listas_*` | `page_mapping` |
|---|---|---|
| wiki_100mb | `listas_wiki_100mb_versionada` | `page_mapping_wiki_100mb.bin` |
| wiki_1gb | `listas_wiki_1gb_versionada` | `page_mapping_wiki_1gb.bin` |
| wiki_2gb | `listas_wiki_2gb_versionada` | `page_mapping_wiki_2gb.bin` |
| torsen (no versionado) | `listas_torsen.text200mb` | — |

---

## Troubleshooting

| Síntoma | Causa probable | Acción |
|---|---|---|
| `only_list_and_voc requiere indexbasename=` | Opciones mal pasadas | Usar basename absoluto como en los ejemplos |
| Build versionado sin `_versionada` | Falta `page_mapping_*.bin` | Copiar/crear mapping junto al `.txt` |
| CUDD: `ERROR: vocabulario` | Ruta `.voc` incorrecta | Usar `.voc` del mismo build uiHRDC |
| Notebook: CSV no encontrado | Ejecutado desde subcarpeta | Correr desde raíz MAGISTER (el notebook hace `chdir`) |
| bpi muy alto en última fila del log | Pool incluye snapshots pre-trim | Normal; bosque `.zpack` post-trim ~4.97M nodos en wiki_1gb |

---

## Referencias

- Índice del repo: [`readme.md`](../readme.md) — comparativa de binarios, modos CLI y arquitectura modular
- Texto LaTeX Backbone 2: [`BACKBONE_2_LATEX.md`](BACKBONE_2_LATEX.md)
- Motor con tags: [`plus_t/engine/engine.h`](../plus_t/engine/engine.h)
- Visualización demo: [`plus_t/demo/viz.h`](../plus_t/demo/viz.h)
- Formato `.zpack`: [`nzdd_cudd_pack.h`](../nzdd_cudd_pack.h)
- I/O compartido: [`nzdd_cudd_common.h`](../nzdd_cudd_common.h)
- Layout packed64: [`version_packing.h`](../uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils/version_packing.h)
- Build/search uiHRDC general: [`PLAN_A_SEGUIR_MOTOR`](../PLAN_A_SEGUIR_MOTOR) (anexo compilación)
- Stress campaign: [`scripts/stress/README.md`](../scripts/stress/README.md)
- Notebooks: [`scripts/README.md`](../scripts/README.md)
