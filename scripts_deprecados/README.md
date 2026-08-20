# Scripts y código ZDD histórico (deprecado)

Contenido archivado del flujo **anterior** a `plus_t/`. No forma parte del pipeline de producción actual.

## Flujo activo (no mover)

| Recurso | Ubicación |
|---------|-----------|
| Motor CUDD con tags | [`plus_t/`](../plus_t/) → binario `zdd_cudd_plus_t` |
| Headers compartidos | [`nzdd_cudd_common.h`](../nzdd_cudd_common.h), [`nzdd_cudd_pack.h`](../nzdd_cudd_pack.h) |
| Pipeline operativo | [`docs/PIPELINE_UIHRDC_CUDD.md`](../docs/PIPELINE_UIHRDC_CUDD.md) |
| Análisis BPI | [`scripts/analisis_CUDD.ipynb`](../scripts/analisis_CUDD.ipynb) |

## Contenido de esta carpeta

| Subcarpeta | Origen | Descripción |
|------------|--------|-------------|
| `backbone1_cudd/` | `zdd_cudd.cpp` | CUDD sin tags (`F_t` solo), `pbuild`, `compare-tdzdd` |
| `baseline_tdzdd/` | `test.cpp`, `script_versionado_test_tdzdd.cpp` | Baselines TdZdd Backbone 1 |
| `experimentos_cpp/` | `scripts/cpp/` | Prototipos MTZDD, blueprint, piso1/piso2 |
| `scripts_legacy/` | notebooks y scripts Python antiguos | `analisis.ipynb`, conversores legacy, harness stress |
| `diagramas/` | raíz MAGISTER | Diagramas Graphviz de tesis (MTZDD, TdZdd) |

## Compilar desde la raíz (referencia histórica)

```bash
cd /root/MAGISTER

# Backbone 1 — CUDD sin tags
g++ -O2 -std=c++17 -fopenmp -o scripts_deprecados/backbone1_cudd/nzdd_cudd_serialize \
  scripts_deprecados/backbone1_cudd/zdd_cudd.cpp \
  -I . -I ./cudd/cudd -I ./cudd -L ./cudd/cudd/.libs \
  -Wl,-rpath,'$ORIGIN/../../cudd/cudd/.libs' -lcudd \
  -I ./TdZdd/include \
  -I ./uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils

# Baseline TdZdd
g++ -O2 -std=c++17 -fopenmp -o scripts_deprecados/baseline_tdzdd/script_versionado_test_tdzdd \
  scripts_deprecados/baseline_tdzdd/script_versionado_test_tdzdd.cpp \
  -I ./TdZdd/include -lpthread
```

Los includes apuntan a `cudd/`, `TdZdd/` y `uiHRDC/` en la raíz del repo (sin mover).
