# Datos sintéticos versionados (MAGISTER)

Generación controlada de listas invertidas versionadas en formato **`.docs` packed64** + **`.voc`**, compatibles con `zdd_cudd_plus_t` y con `edd_metatrie`.

**Salida canónica de barridos:** [`datos_sinteticos/`](../datos_sinteticos/)  
**Layout bits:** [`scripts/packed64_layout.py`](../scripts/packed64_layout.py) ← lee `version_packing.h` (uiHRDC, default master 40 / rel 24).

---

## Semántica (dualidad)

Misma membresía \(M \subseteq U \times T\) (por término):

| Vista | Forma | Índice típico |
|-------|--------|----------------|
| Snapshots | \(S^t = \{u \mid (u,t)\in M\}\) | ZDD / `plus_t` |
| Intervalos | por master: corridas half-open `[t_a, t_b)` | metatrie / `edd_metatrie` |

Ejemplo: postings `(master,rel)` = `[(1,1),(1,3),(2,3)]` ≡  
\(S^1=\{1\},\; S^2=\emptyset,\; S^3=\{1,2\}\).  
Intervalos: master 1 → `[1,2)` y `[3,4)`; master 2 → `[3,4)`.

---

## Generador

```bash
python3 scripts/generar_docs_versionados_sinteticos.py \
  --terms 8000 --versions 80 --universe-size 2097152 \
  --evolution-model toggle --toggle-prob 0.01 --toggle-window 128 \
  --init-distribution zipf --init-card-mean 50 \
  --seed 20250920 --output-dir datos_sinteticos --stem mi_run \
  --max-postings 120000000   # sin --force: aborta si la estimación lo supera
  # --run-zdd [--run-optimize]
```

### Ejes parametrizables

| Eje | Flags | Notas |
|-----|--------|------|
| Evolución | `--evolution-model sparse\|toggle` | sparse: delete/add; toggle: flip en ventana |
| Cardinalidad inicial | `--init-distribution zipf\|constant` | + `--init-card-*`, `--zipf-*` |
| Universo docs \(U\) | `--universe-size` o `--universe-size-min/max` | **log-uniforme** si rango; infla `numZddVars≈max_master+nTerms` |
| Universo temporal \(T\) | `--versions` o `--versions-min/max` | uniforme si rango; infla postings/nodos, no el conteo de vars u+t |
| Escala | `--terms`, cards, V | postings ≈ Σ card×versiones; RAM del build ZDD |

**Techos de RAM (experiencia ~8 GiB):**

- `numZddVars` crece con **max_master** (U grande → OOM aunque haya pocos postings).
- **Tamaño** (postings / `edd_nodes`) también mata el build: un probe chico **sobreestima** el U seguro.
- Barridos reales deben fijar una **escala** de postings y luego variar U/V/modos.

---

## Barrido de límites + modos

Script: [`scripts/sweep_synthetic_zdd_limits.sh`](../scripts/sweep_synthetic_zdd_limits.sh)

| Fase | Qué mide |
|------|----------|
| **S** | Escalera de **tamaño** (terms:vers:card) con U/V conservadores |
| **A** | Escalera **U** a la escala de `SIZE_SAFE` / `SCALE` |
| **B** | Escalera **V** con `U_SAFE` |
| **C** | Matriz sparse\|toggle × zipf\|constant × U/V fijo\|var |

```bash
# Salida por defecto: datos_sinteticos/
# Por defecto: gen → build → medir → BORRAR .docs/.voc/.zpack (KEEP_DATASETS=0)
PHASES=S,A,B,C SCALE=med MIN_MEM_MB=2000 \
  ./scripts/sweep_synthetic_zdd_limits.sh

tail -f datos_sinteticos/sweep.log
# Resultados: datos_sinteticos/sweep_results.csv
# Recipes (replicables): datos_sinteticos/recipes.jsonl + recipes/<stem>.json
# Techos: SIZE_SAFE.txt U_SAFE.txt V_SAFE.txt (+ *_FAIL_AT.txt)
# KEEP_DATASETS=1 solo si hay RAM de sobra; SKIP_DONE=0 fuerza re-correr.
```

`SCALE=smoke|med|wiki` fija volumen objetivo y `--max-postings` (sin `--force` salvo `FORCE=1`).

**Guardas:** `MemAvailable ≥ MIN_MEM_MB` antes de gen y de build; corte de escalera en OOM/`mem_guard`; cleanup post-celda para no acumular ~GBs en disco/cache; no fuerza estimaciones que superen el tope.

---

## Barrido metatrie (mismo corpus)

```bash
./scripts/sweep_metatrie_from_recipes.sh
# CSV: datos_sinteticos/metatrie_results.csv
# MODES=per-term,global  VALIDATE_TERMS=20  KEEP_DATASETS=0
```

Compara `bpi_file` (metatrie, bytes del trie / `n_raw`) vs `zdd_bpi_edd` del CSV ZDD (mismo denominador `n_raw`).

---

## Resultados de referencia (sep 2026)

| Dataset | Notas | bpi_edd (baseline u+t) |
|---------|--------|-------------------------|
| `resultados_test/synthetic_500mb_t10k_v100` | sparse+Zipf, U=V fijos 2²¹×100 | ~156 |
| `resultados_test/synthetic_toggle_500mb` | toggle const, mismo U/V | ~65 |
| `resultados_test/synthetic_var_uv_small` | V 20–80, U 2¹⁶–2²¹, 12.8k términos | ~134 |
| Var U hasta 2²⁴ @ ~100–200 M postings | gen OK, **ZDD OOM** | — |

Corridas del barrido vivo: ver `datos_sinteticos/sweep_results.csv`.

---

## Relacionado

- Pipeline ZDD: [`docs/PIPELINE_UIHRDC_CUDD.md`](PIPELINE_UIHRDC_CUDD.md)
- Metatrie EDD (intervalos): [`edd_metatrie/README.md`](../edd_metatrie/README.md)
- Contexto tesis: [`THESIS_CONTEXTO_MAGISTER.md`](../THESIS_CONTEXTO_MAGISTER.md) §23
