# Metatrie EDD: flujo end-to-end, funciones y ejecución

Este documento describe el camino **realmente ejecutado** por
`edd_metatrie/meta_trie_edd.cpp`: desde un `.docs` packed64 hasta la
construcción, consulta, validación, medición BPI y salidas opcionales.

## 1. Qué se necesita

### Código activo

- `edd_metatrie/meta_trie_edd.cpp`: CLI, lectura, RLE, barrido, índice,
  consulta, validación, benchmark, métricas y salidas.
- `edd_metatrie/edd_cltj_types.hpp`: layout y constructores de `spot_quad`.
- `edd_metatrie/packed64_io.hpp`: desempaquetado 40+24 y cálculo de bits.
- `edd_metatrie/cltj_temporal_wm_u64.hpp`: bitvectors B/E, `leap` y
  `leftmost`.
- `plus_t/utils/bpi.h`: fórmula común de BPI.
- SDSL, `divsufsort` y `divsufsort64`.

### Entrada

Un archivo `.docs`:

```text
uint32 nlists
repetido nlists veces:
    uint32 len
    uint64 postings[len]
```

Cada posting usa el layout packed64:

```text
posting = (master_doc << 24) | version
```

Por defecto hay 40 bits para `master_doc` y 24 para `version`.

### Qué no se necesita

- No se construye el grafo RDF de BGPs.
- No se construyen los 18 tries de CLTJ.
- No se usa CUDD ni el ZDD para construir o consultar la metatrie.
- No se usa el antiguo índice auxiliar ST[OP].
- No se modifica el monorepo BGPs.
- No se materializan todos los snapshots como estructuras independientes.
- La rama `is_partial` heredada de CLTJ no se usa: siempre se pasa `false`.

El ZDD solo aparece en experimentos comparativos externos. No forma parte del
camino de ejecución de `meta_trie_edd`.

## 2. Semántica

Para un término `t`, el `.docs` guarda pares `(master_doc, version)`.
La consulta es:

```text
Q_tau(term, version) -> todos los master_doc activos
```

En notación:

```text
S^tau = { u | (u, tau) pertenece a M_t }
```

La metatrie representa la misma membresía que el ZDD, pero usa intervalos
temporales. Si un documento está presente en versiones consecutivas 3, 4 y 5,
se almacena el intervalo half-open `[3, 6)`.

## 3. El quad

`spot_quad` es un `array<uint64_t, 5>`.

```text
Global:   (term_id, master_doc, 0, version_start, version_end)
Per-term: (master_doc, 0, 0, version_start, version_end)
```

Campos:

1. `QUAD_TERM`: término en global; master en per-term.
2. `QUAD_MASTER`: master en global; cero en per-term.
3. `QUAD_UNUSED`: hueco heredado de SPOT/CLTJ; siempre cero.
4. `QUAD_VERSION_START`: inicio inclusivo.
5. `QUAD_VERSION_END`: fin exclusivo.

Los helpers `make_global_quad` y `make_per_term_quad` evitan construir estos
arrays con índices numéricos crípticos.

## 4. Flujo completo de `main`

```text
main
├── parse_args
├── inspect_docs
├── meta_trie_edd::build_global
│   └── o meta_trie_edd::build_per_term
├── scan_docs_metrics
├── meta_trie_edd::size_report
├── print_size_report
├── validate_index
├── [opcional] sample_queries + run_query_bench
├── [opcional] dump_trie_dot
├── [opcional] sdsl::store_to_file -> serialize
└── [opcional] escribe CSV
```

El orden importa:

1. Primero se construye el índice completo.
2. Luego se miden sus bytes y BPI.
3. Después se valida contra el `.docs`.
4. Solo si la validación da cero se ejecutan benchmark, DOT, serialización y
   CSV.

Códigos de salida:

- `0`: construcción y validación correctas.
- `1`: argumentos, entrada o salida inválidos.
- `2`: `validation_mismatches != 0`.
- `3`: mismatches en el benchmark.

## 5. Lectura del `.docs`

### `parse_args`

Interpreta el modo, ruta y flags. Rechaza modos y opciones desconocidas.
Valores por defecto relevantes:

- modo: debe indicarse explícitamente;
- RLE: activado;
- `validate_terms`: 100;
- `bench_reps`: 5.

### `read_u32`

Lee un entero de 32 bits del stream.

### `inspect_docs`

Hace una pasada secuencial por el archivo para:

- leer `nlists`;
- guardar el offset de cada posting list;
- contar postings;
- hallar `max_master` y `max_relative`;
- aplicar `--max-terms`, si se indicó.

Los offsets permiten que `read_posting_list` salte después directamente a
cualquier término.

### `read_posting_list`

Recibe el offset de un término, lee `len` y copia sus postings packed64 a un
vector.

### `unpack_master` y `unpack_relative`

Separan los bits altos (`master_doc`) y bajos (`version`) del packed64.

### `bits_required_u64`

Calcula el ancho necesario para cada componente del wavelet temporal.

## 6. Postings a intervalos

### Camino por defecto: RLE

`postings_to_quads_rle`:

1. desempaqueta cada posting como `(master, version)`;
2. ordena y elimina duplicados;
3. agrupa por master;
4. fusiona versiones consecutivas;
5. emite un quad por corrida.

Ejemplo:

```text
(u=7, tau=2), (7,3), (7,4), (7,8)
    ->
(t,7,0,2,5), (t,7,0,8,9)
```

### `postings_to_quads_rle_per_term`

Reutiliza el RLE global y transforma cada resultado a:

```text
(master_doc, 0, 0, version_start, version_end)
```

El término no se guarda porque lo determina `per_term[term]`.

### Camino `--no-rle`

`postings_to_quads_point` y
`postings_to_quads_point_per_term` generan un intervalo `[tau, tau+1)` por
posting. Sirve para experimentos y depuración; normalmente ocupa más memoria y
genera más updates.

## 7. Barrido de extremos

### `generate_list_of_updates`

Recibe el vector de quads `D`.

Por cada quad crea:

- un evento insert en `version_start`;
- un evento delete en `version_end`.

Ordena todos los extremos y produce `D_T`, una secuencia de:

```text
(intervalo_suelo, updates_que_ocurren_en_el_borde)
```

Un intervalo suelo es un tramo temporal donde el conjunto activo no cambia.

Ejemplo:

```text
u=5: [1,4)
u=9: [2,3)

suelos: [1,2), [2,3), [3,4)
updates: +5, +9, -9, -5
```

### `comparator_time_first`

Ordena los updates por:

1. `version_start` del suelo;
2. `term_id` o master en per-term;
3. `master_doc`;
4. campo no usado.

## 8. Construcción de `time_first_trie`

### `create_time_first_trie`

Empaqueta cuatro piezas:

1. `interval_list`: los suelos.
2. `last_update_pos`: para cada suelo, el índice del último update aplicable
   (`p_l`).
3. stream ordenado de quads de update + flags insert/delete.
4. `temporal_wm`.

También construye `last_update_per_interval`, heredado del diseño CLTJ y
contabilizado/serializado, aunque la consulta actual obtiene `p_l` directamente
desde `last_update_pos`.

### `build_global`

Construye un único trie lógico:

```text
todas las posting lists
    -> un D global
    -> un D_T global
    -> un time_first_trie con n_components=2
```

El WM indexa `term -> master`. La consulta primero salta al término solicitado y
después enumera sus masters.

### `build_per_term`

Construye un índice lógico formado por un vector de tries:

```text
para cada term:
    posting list
        -> D_t
        -> D_T_t
        -> per_term[term] con n_components=1
```

Cada WM solo indexa masters. Esto evita almacenar el término dentro de cada
update, a costa de tener muchas estructuras SDSL pequeñas.

## 9. Wavelet temporal

`temporal_wm` no es un wavelet matrix de texto convencional. Indexa el stream
de inserts/deletes y simula un trie binario sobre uno o dos componentes.

### Constructor de `temporal_wm`

Recibe:

- updates ya ordenados;
- flag `is_delete` por update;
- `n_components`: 1 per-term, 2 global;
- `n_bits`;
- `is_partial=false`.

Dimensiona los bitvectors B/E y llama
`generate_B_E_stable_prefix_sort`.

### `generate_B_E_stable_prefix_sort`

Para cada bit, de MSB a LSB:

1. escribe en B el bit actual del componente;
2. ordena establemente por el prefijo ya procesado;
3. mantiene el balance insert/delete por prefijo;
4. escribe E=1 si ese prefijo sigue activo y E=0 si está cancelado.

B permite navegar por hijos 0/1 con `rank`. E indica si bajo el prefijo temporal
`p_l` queda una presencia activa.

### `rank_range_B`

Cuenta unos de B dentro del rango del nodo. Se usa para calcular los rangos de
los hijos izquierdo y derecho.

### `leap`

Devuelve el menor valor activo mayor o igual que `x` dentro de un nodo y con
updates aplicados hasta `p_l`.

### `leftmost`

Cuando `leap` no encuentra `x` en la rama esperada, obtiene el menor símbolo
activo de la siguiente rama válida.

### Funciones heredadas no usadas en la consulta actual

- `rank_range_E` está definido, pero el recorrido actual consulta E
  directamente.
- La rama `is_partial` del constructor existe por compatibilidad con CLTJ, pero
  MAGISTER siempre usa `false`.

## 10. Consulta `values_at(term, version)`

### Primitivas del trie

`interval_seek(version)` hace búsqueda binaria del suelo.

`version_in_interval` comprueba la semántica half-open:

```text
version_start <= version < version_end
```

`get_last_update_of_interval` devuelve `p_l`.

`resolve_at_tau` combina esas operaciones y entrega:

```text
(last_update_prefix, raíz del WM)
```

`temporal_successor` adapta una consulta de componente a `temporal_wm::leap`.

`append_masters_at` llama `leap` repetidamente con candidatos 0, `u+1`, etc.,
hasta enumerar todos los masters activos.

### Global

```text
values_at(term, version)
├── resolve_at_tau
├── leap(depth=0, candidate=term)
├── comprobar que el sucesor sea exactamente term
└── append_masters_at(depth=1)
```

No enumera todos los términos: hace un salto directo al solicitado.

### Per-term

```text
values_at(term, version)
└── per_term[term].values_at_version(version)
    ├── resolve_at_tau
    └── append_masters_at(depth=0)
```

La salida de ambos modos es un vector ordenado de `term_master`.

## 11. Validación

### `validation_times`

Para cada posting incluye:

- la versión exacta;
- `version-1`, si existe;
- `version+1`, si existe;
- cero;
- un valor posterior al máximo.

Así se prueban bordes y vecinos de los intervalos.

### `expected_masters`

Reconstruye por RLE el snapshot esperado directamente desde la posting list.
Es el ground truth y no consulta la metatrie.

### `expected_term_masters_at`

Convierte los masters esperados a pares `(term, master)` para compararlos con
la salida del índice.

### `validate_index`

Para los primeros `--validate-terms N` términos compara:

```text
expected_term_masters_at(postings, term, version)
vs
index.values_at(term, version)
```

Cada par `(term, version)` distinto aumenta `validation_mismatches`.
`--debug-mismatches N` imprime los primeros N desacuerdos.

Importante: `--validate-terms 0` valida cero términos; no significa “todos”.
Para validar todos, use un N mayor o igual a `nlists`.

## 12. Bytes y BPI

### `size_bytes_breakdown`

Suma:

- arrays izquierdo/derecho de suelos;
- `last_update_pos`;
- bitvector/select de last-update;
- serialización del WM.

### `wm_serialized_bytes`

Serializa hacia un stream contador, sin guardar el buffer, para conocer los
bytes exactos. Esto evita materializar cientos de serializaciones temporales en
modo per-term.

### `size_report`

- global: bytes del único trie;
- per-term: suma de todos los tries no vacíos.

### `scan_docs_metrics`

Calcula `n_raw` y denominadores auxiliares. La métrica canónica comparable con
ZDD es:

```text
bpi_file = 8 * bytes_total / n_raw
```

La fórmula se llama desde `NzddBpi::bpiFromBytes`.

`bpi_total` es actualmente un alias de `bpi_file`.

## 13. Benchmark

### `sample_queries`

Genera pares `(term, version)` round-robin:

```text
term 0, term 1, ..., term K-1, term 0, ...
```

En cada nueva ronda avanza al siguiente tiempo de validación de cada término.
Esto evita concentrar todas las consultas en los primeros términos.

### `run_query_bench`

1. carga en RAM las posting lists necesarias;
2. hace warm-up de ambos caminos;
3. cronometra `expected_masters` como `docs_scan`;
4. cronometra `values_at` como `metatrie`;
5. compara respuestas durante la primera repetición.

No mide ZDD. El ZDD se ejecuta mediante scripts/binarios separados y su
`bench-qmem` mide membresía de conjuntos, no exactamente `Q_tau`.

## 14. Salidas opcionales

### `--csv`

Escribe una fila con modo, bytes, denominadores, BPI y tiempo de build.

### `--serialize out.emt`

Llama `sdsl::store_to_file`, que termina invocando
`meta_trie_edd::serialize`, `time_first_trie::serialize` y
`temporal_wm::serialize`.

Las funciones `load` existen y son compatibles con SDSL, pero la CLI actual no
tiene un flag para cargar un `.emt`; cada ejecución construye desde `.docs`.

### `--dump-dot out.dot`

Recorre suelos y snapshots para una visualización Graphviz. Es apropiado solo
para corpus pequeños.

En per-term debe acompañarse de `--dump-term T`. En global muestra términos y
masters bajo cada suelo.

### Inventario de auxiliares y métodos

Además de las funciones del camino principal:

- `counting_streambuf::xsputn` y `overflow` cuentan los bytes que una
  serialización intentaría escribir.
- `counting_ostream` expone ese contador como `std::ostream`.
- `wm_serialized_bytes` usa ese stream para medir un WM.
- Los operadores `<` y `==` de `term_master` y `master_version` permiten
  ordenar, deduplicar y comparar respuestas.
- `time_first_trie::copy_from`, `move_from` y `rebind_supports` mantienen
  válidos los supports SDSL después de copiar/mover tries, especialmente al
  guardarlos en `per_term`.
- `interval_count`, `n_components`, `get_temporal_root`,
  `get_interval_at_pos` y `get_last_update_of_interval` son accesores.
- `values_at_version` enumera el snapshot completo de un trie. En per-term es
  el camino normal; en global completo se reserva principalmente para DOT.
- `size_bytes_breakdown`, `serialize` y `load` miden/persisten un trie.
- `CompareEndpoints` ordena extremos temporales.
- `PrefixComparator`, `top_bits`, `get_component` y
  `get_accumulated_bit` implementan el orden estable por prefijos del WM.
- Los constructores, asignaciones, `copy` y `swap` de `temporal_wm` reenganchan
  los supports rank a sus bitvectors después de copias o movimientos.
- `get_root` y `get_n_bits` exponen la raíz lógica y ancho del WM.
- `print_mismatch` muestra `got` y `expected` cuando se activa depuración.
- `print_size_report` imprime métricas legibles por scripts.
- `dump_trie_dot` no representa nodos físicos de SDSL uno a uno: produce una
  vista lógica suelo -> snapshot para explicar el índice.

## 15. Qué queda fuera del camino crítico

- `pack_master_rel`: helper disponible para generadores/tests; el binario solo
  desempaqueta.
- `meta_trie_edd::load`, `time_first_trie::load` y `temporal_wm::load`: soporte
  de carga, pero sin comando CLI.
- `values_at_version` global completo: se usa para DOT; la consulta normal
  global usa `values_at(term, version)` para no enumerar todos los términos.
- `arrays_debug`: solo para la leyenda DOT.
- `dump_trie_dot`: solo con `--dump-dot`.
- benchmark: solo con `--bench-queries`.
- serialización: solo con `--serialize`.
- escritura CSV: solo con `--csv`.
- RLE puntual: solo con `--no-rle`.
- `m_last_update_select1`: se conserva, mide y serializa por compatibilidad con
  el diseño CLTJ; la consulta usa `m_last_update_pos`.

## 16. Compilar

Desde la raíz:

```bash
cd /root/MAGISTER
./edd_metatrie/build.sh
```

Equivalente desde el subdirectorio:

```bash
cd /root/MAGISTER/edd_metatrie
./build.sh
```

El script usa C++17, `-O3`, SDSL y divsufsort. El binario queda en:

```text
/root/MAGISTER/edd_metatrie/meta_trie_edd
```

## 17. Ejecuciones recomendadas

Desde `/root/MAGISTER`:

### Smoke test, ambos modos

```bash
edd_metatrie/meta_trie_edd per-term BGPs/micro_metatrie.docs \
  --validate-terms 3

edd_metatrie/meta_trie_edd global BGPs/micro_metatrie.docs \
  --validate-terms 3
```

Debe aparecer:

```text
validation_mismatches=0
```

### Métricas y CSV

```bash
edd_metatrie/meta_trie_edd per-term BGPs/micro_metatrie.docs \
  --validate-terms 3 \
  --csv resultados_test/micro_per_term.csv

edd_metatrie/meta_trie_edd global BGPs/micro_metatrie.docs \
  --validate-terms 3 \
  --csv resultados_test/micro_global.csv
```

### Construir solo los primeros términos de un corpus grande

```bash
edd_metatrie/meta_trie_edd global resultados_test/wiki_2gb_packed64.docs \
  --max-terms 1000 \
  --validate-terms 100
```

`--max-terms` cambia el índice y también los denominadores medidos: el resultado
describe el prefijo del corpus, no el corpus completo.

### Benchmark de consulta

```bash
edd_metatrie/meta_trie_edd global BGPs/micro_metatrie.docs \
  --validate-terms 3 \
  --bench-queries 30 \
  --bench-reps 10 \
  --bench-csv resultados_test/micro_bench_global.csv \
  --bench-queries-out resultados_test/micro_queries.csv
```

### Serializar

```bash
edd_metatrie/meta_trie_edd global BGPs/micro_metatrie.docs \
  --validate-terms 3 \
  --serialize resultados_test/micro_global.emt
```

### Visualizar

```bash
edd_metatrie/meta_trie_edd per-term BGPs/micro_metatrie.docs \
  --validate-terms 3 \
  --dump-term 0 \
  --dump-dot resultados_test/micro_term0.dot \
  --dump-max-intervals 40 \
  --dump-max-answer 16

dot -Tpng -Gdpi=140 resultados_test/micro_term0.dot \
  -o resultados_test/micro_term0.png
```

### Comparar con y sin RLE

```bash
edd_metatrie/meta_trie_edd global BGPs/micro_metatrie.docs \
  --validate-terms 3 \
  --csv resultados_test/micro_global_rle.csv

edd_metatrie/meta_trie_edd global BGPs/micro_metatrie.docs \
  --no-rle \
  --validate-terms 3 \
  --csv resultados_test/micro_global_no_rle.csv
```

### Barrer las 18 recetas sintéticas

```bash
SKIP_DONE=0 KEEP_DATASETS=0 MODES=per-term,global \
  ./scripts/sweep_metatrie_from_recipes.sh
```

Salida:

```text
datos_sinteticos/metatrie_results.csv
```

### Sintéticos de `resultados_test`

```bash
KEEP_DATASETS=0 INCLUDE_2GB=0 \
  ./scripts/run_metatrie_resultados_synthetics.sh
```

Use `INCLUDE_2GB=1` solo si hay memoria y tiempo suficientes.

## 18. Cómo leer el resultado

Los campos principales de stdout son:

```text
mode
bytes_total
n_raw
bpi_file
raw_quads
update_events
build_s
validation_mismatches
```

Interpretación:

- `raw_quads`: intervalos RLE antes del barrido.
- `update_events`: inserts + deletes.
- `build_s`: solo construcción; no incluye métricas ni validación.
- `bytes_total`: tamaño serializado lógico del índice medido.
- `bpi_file`: métrica canónica.
- `validation_mismatches`: debe ser cero.

Para comparar modos use el mismo `.docs`, el mismo `--max-terms` y la misma
configuración RLE. Para reportar tesis use `bpi_file`, no mezcle denominadores
auxiliares.
