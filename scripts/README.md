# Scripts de análisis y utilidades

Índice del repo: **[readme.md](../readme.md)** · Pipeline CUDD: **[docs/PIPELINE_UIHRDC_CUDD.md](../docs/PIPELINE_UIHRDC_CUDD.md)** · Stress: **[stress/README.md](stress/README.md)**

## Notebooks

| Notebook | Entrada | Qué analiza |
|---|---|---|
| **`analisis_CUDD.ipynb`** | `cudd_evolucion_*_plus_t*.csv` + `.zpack` | bpi_build (diagnóstico), versionado, **bpi_edd** (EDD en RAM), bpi_file, bpi_mem (diagnóstico) |

Notebooks y scripts legacy: [`scripts_deprecados/scripts_legacy/`](../scripts_deprecados/scripts_legacy/) (p. ej. `analisis.ipynb` para TdZdd).

```bash
sudo apt-get install -y python3-pandas python3-matplotlib python3-seaborn python3-nbconvert

cd /root/MAGISTER
python3 -m nbconvert --to notebook --execute scripts/analisis_CUDD.ipynb \
  --output analisis_CUDD.ipynb
```

Configuración típica en la celda 1:

```python
ENCODING = "u+t"          # "u+t" | "log"
DATASET_NAME = "wiki_2gb"
CSV_FILE = "cudd_evolucion_2gb_plus_t.csv"   # o cudd_evolucion_2gb_plus_t_log.csv
```

**bpi_edd** (celda 4) es la métrica de la EDD: nodos del DAG × `sizeof(DdNode)` × 8 / N.
Toda la aritmética está en [`plus_t/utils/bpi.h`](../plus_t/utils/bpi.h) (`NzddBpi::compute()`).
`bpi_mem` se conserva como diagnóstico del proceso completo, con `overhead_pct` al lado
para dejar explícito cuánto de `Cudd_ReadMemoryInUse` es andamiaje del manager y no
diagrama. La celda 1 lee **`bpi_build`** del CSV de evolución (calculado en C++ por el mismo `compute()`).

La celda **ladder** invoca `scripts/measure_zpack_bpi` sobre `.zpack` de `plus_t` (`u+t` y `log`) y escribe `resultados_test/plus_t_bpi_ladder.csv` + `.png`.

Para interpretar el denominador N, `scripts/audit_docs_ints <docs>` (o `measure_zpack_bpi … audit` / `NZDD_BPI_AUDIT=1`) reporta los cuatro denominadores y `ratio_raw_over_stored`.

### Reordenamiento de variables (`optimize`)

El DAG serializado ya es mínimo *para su orden de variables*. Para bajar `bpi_edd` hay que
reordenar con heurísticas **semánticamente seguras** (whitelist en `plus_t/cmd/optimize.h`).

**Permitidas:** `baseline`; estáticos `nodes_desc`, `nodes_asc`, `df_desc`, `df_asc`;
dinámicos `sift`, `sift_conv`, `symm_sift`, `symm_sift_conv`, `random`, `random_pivot`;
compuestos con `+` (ej. `nodes_desc+sift_conv`).

**Prohibidas:** `linear`, `linear_conv` — transformación lineal de índices CUDD; rompe la
correspondencia `varIndex → master/tag` (`mastersOfZdd`, decodificación de tags, Q1/Q2).
También retiradas: `reverse`, `shuffle_rand` (no aportan compresión).

Cada corrida verifica `Cudd_zddCountDouble` sobre una muestra de raíces; `optimize`
rechaza heurísticas fuera de la whitelist. **Logs CSV automáticos** en `resultados_test/`:
`optimize_runs.csv` (todas las corridas), `optimize_sweep_<pack>_<ts>.csv`, `optimize_<pack>_<heur>_<ts>.csv`.
Columnas incluyen `bpi_edd_before/after` (sizeof(DdNode), igual que `measure_zpack_bpi`).

```bash
# Medir sin guardar (log CSV igual se escribe)
./zdd_cudd_plus_t optimize log resultados_test/wiki_100mb_plus_t_bin.zpack \
  resultados_test/wiki_100mb_uihrdc_packed64.docs none nodes_desc+sift_conv 200

# Compactar y persistir (ZPACKv2)
./zdd_cudd_plus_t optimize log resultados_test/wiki_1gb_plus_t_bin.zpack \
  resultados_test/wiki_1gb_uihrdc_packed64.docs /tmp/opt.zpack nodes_desc+sift_conv 200

# Barrido (CSV dedicado + append a optimize_runs.csv; timeout_s corta heurísticas lentas)
./zdd_cudd_plus_t optimize sweep log resultados_test/wiki_1gb_plus_t_bin.zpack \
  resultados_test/wiki_1gb_uihrdc_packed64.docs 200 1200
```

En el cuaderno: `pd.read_csv('resultados_test/optimize_runs.csv')` o el CSV del barrido.

### Verificación con consultas CUDD (`verify`)

```bash
cd /root/MAGISTER

./zdd_cudd_plus_t verify u+t resultados_test/wiki_100mb_uihrdc_packed64.docs \
  uiHRDC/uiHRDC/data/texts/index_wiki_100mb_named.voc \
  /tmp/wiki_100mb_verify.zpack 0
```

Detalle: [docs/PIPELINE_UIHRDC_CUDD.md](../docs/PIPELINE_UIHRDC_CUDD.md#consultas-cudd-en-verify-q1-y-q2).

## Conversión uiHRDC → `.docs`

```bash
python3 scripts/convertir_versionado_input_uiHRDC.py \
  --dataset wiki_1gb \
  --input-listas listas_wiki_1gb_versionada \
  --tuple-output packed64 \
  --output-bin resultados_test/wiki_1gb_uihrdc_packed64.docs
```

Detalle del paso 2: [docs/PIPELINE_UIHRDC_CUDD.md](../docs/PIPELINE_UIHRDC_CUDD.md).

## Scripts activos

| Script | Propósito |
|---|---|
| `convertir_versionado_input_uiHRDC.py` | `.docs` packed64 (entrada CUDD) |
| [`../plus_t/utils/bpi.h`](../plus_t/utils/bpi.h) | BPI centralizado (`compute`, `printReport`) |
| [`../plus_t/utils/bpi_scan.h`](../plus_t/utils/bpi_scan.h) | Escaneo de denominadores sobre `.docs` |
| `measure_zpack_bpi.cpp` | bpi_edd / bpi_file / bpi_mem post-load + desglose de overhead |
| `audit_docs_ints.cpp` | Auditoría de los cuatro denominadores sobre el `.docs` |
| `analisis_CUDD.ipynb` | Análisis build, versionado, ladder BPI |

Scripts históricos archivados: [`scripts_deprecados/scripts_legacy/`](../scripts_deprecados/scripts_legacy/).
