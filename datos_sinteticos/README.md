# `datos_sinteticos/`

Barrido sintético: **gen → ZDD build → medir → borrar** (no acumular datasets).

| Archivo | Rol |
|---------|-----|
| `sweep.log` | Log del barrido |
| `sweep_results.csv` | Resultados por stem (status, postings, bpi_edd, …) |
| `recipes.jsonl` + `recipes/<stem>.json` | Params + cmdline + métricas (único archivo por stem) |
| `recipes/legacy_resultados_test/` | Manifests de sintéticos viejos en `resultados_test/` |
| `SIZE_SAFE.txt` / `U_SAFE.txt` / `V_SAFE.txt` | Techos OK |
| `*_FAIL_AT.txt` / `failures.txt` | Primer fallo OOM / mem_guard |

Los `.docs` / `.voc` / `.zpack` **no se conservan** salvo `KEEP_DATASETS=1`.

```bash
tail -f /root/MAGISTER/datos_sinteticos/sweep.log
# Replicar un stem: ver recipes/<stem>.json → campo cmdline / replicate
```

Docs: [`docs/DATOS_SINTETICOS.md`](../docs/DATOS_SINTETICOS.md).
