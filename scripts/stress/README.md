# Stress Campaign Toolkit

Pipeline activo: **`zdd_cudd_plus_t`** — ver [docs/PIPELINE_UIHRDC_CUDD.md](../../docs/PIPELINE_UIHRDC_CUDD.md).

Flujos históricos (Backbone 1, TdZdd): [scripts_deprecados/](../../scripts_deprecados/).

## Runner de producción

| Runner | Binario | Uso en stress |
|---|---|---|
| **CUDD con tags** | `zdd_cudd_plus_t` | `build`, `verify u+t\|log`, ladder BPI |

### Comandos típicos (wiki_1gb)

```bash
DOCS=resultados_test/wiki_1gb_uihrdc_packed64.docs
VOC=uiHRDC/uiHRDC/data/texts/index_wiki_1gb_named.voc

# verify tag canónico u+t
./zdd_cudd_plus_t verify u+t "$DOCS" "$VOC" resultados_test/wiki_1gb_plus_t_tmp.zpack 0

# verify modo log (phi)
./zdd_cudd_plus_t verify log "$DOCS" "$VOC" resultados_test/wiki_1gb_plus_t_log_tmp.zpack 0

# build + CSV evolución (bpi_build diagnóstico)
./zdd_cudd_plus_t build u+t "$DOCS" "$VOC" none 0 resultados_test/cudd_evolucion_1gb_plus_t.csv 500
```

Layouts: `docOffset=V+1` en `u+t`; `docOffset=1+⌈log₂(V+1)⌉` en `log`. Verify debe finalizar con `tag_mismatches=0`, `roundtrip_mismatches=0` y `overall: PASS`.

**No usar `demo` en la ladder** (solo toy u=4).

## Dataset ladder

`dataset_ladder.json`: `torsen.text200mb`, `wiki_100mb` … `wiki_2gb`.

## Stop rule

Parar escalado cuando un job supere `--max-minutes` (default 30); registrar el último dataset estable.
