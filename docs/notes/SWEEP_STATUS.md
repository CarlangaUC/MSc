# Sweep parsers wiki_2gb — estado

> **Histórico (sep 2026).** El barrido activo de sintéticos está en
> [`datos_sinteticos/`](datos_sinteticos/) — ver [`docs/DATOS_SINTETICOS.md`](docs/DATOS_SINTETICOS.md).
> Parsers @ 2 GB: baseline 20/20; optimize completo solo regex/mwph/mwph_u2 (ver CSV oneshot).

**Actualizado:** 2026-09-10T11:29:36-03:00 (snapshot del daemon parsers)

| | |
|---|---|
| Progreso | **3/20** backends completados |
| Corriendo ahora | paso=3-ZDD-build (pico RAM) |
| RAM | 7.6Gi/7.6Gi used, 55Mi avail |
| Swap | 659Mi/2.0Gi |
| Daemon log | `resultados_test/sweep_backends_wiki_2gb_marcado/daemon.log` |

## Completados (bpi_edd optimized)

| backend | bpi_edd |
|---|---:|
| mwph | 27.161926 |
| mwph_u2 | 27.244141 |
| regex | 27.489952 |

## state.done
```
regex
mwph
mwph_u2
```

## Limpieza en curso (lineas / 149761)
```
```

## Comandos
```bash
tail -f resultados_test/sweep_backends_wiki_2gb_marcado/daemon.log
./scripts/status_sweep.sh   # refrescar este archivo
nohup ./scripts/sweep_backends_bpi_daemon.sh wiki_2gb marcado &  # reanudar tras crash
```
