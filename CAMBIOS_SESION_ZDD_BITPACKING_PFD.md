# Cambios de sesión: ZDD bit-packing + fix memoria PForDelta

Documento generado con el código **actual** que reemplaza al comportamiento anterior.  
Rangos de líneas según el estado del repositorio al generar este archivo.

---

## 1. `uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils/ii.c`

### 1.1 Reemplazo: cabecera + helpers estáticos (aprox. líneas **1–225**)

**Antes (conceptual):** solo `#include "ii.h"` y arrancaba directamente `loadTextInMem`.

**Ahora:** includes estándar + `load_page_mapping`, `find_master_doc_id`, `pack_occ_lists_for_zdd` (con estadísticas 16/16), `export_occ_lists_unpacked_txt`.

```c
#include "ii.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int load_page_mapping(uint32_t **page_map, size_t *map_len) {
	const char *mapping_path = getenv("PAGE_MAPPING_BIN");
	FILE *fp;
	long file_size;
	size_t entries;
	size_t i;
	uint8_t bytes[4];

	if (mapping_path == NULL || mapping_path[0] == '\0') {
		mapping_path = "page_mapping.bin";
	}

	fp = fopen(mapping_path, "rb");
	if (fp == NULL) {
		fprintf(stderr,
		        "\n[ZDD] Warning: could not open mapping file '%s': %s. "
		        "Continuing without versioned packing/export.\n",
		        mapping_path, strerror(errno));
		return 0;
	}

	if (fseek(fp, 0L, SEEK_END) != 0) {
		fprintf(stderr,
		        "\n[ZDD] Warning: could not seek mapping file '%s': %s. "
		        "Continuing without versioned packing/export.\n",
		        mapping_path, strerror(errno));
		fclose(fp);
		return 0;
	}

	file_size = ftell(fp);
	if (file_size < 0) {
		fprintf(stderr,
		        "\n[ZDD] Warning: could not get mapping file size '%s': %s. "
		        "Continuing without versioned packing/export.\n",
		        mapping_path, strerror(errno));
		fclose(fp);
		return 0;
	}

	if (file_size % 4L != 0L) {
		fprintf(stderr,
		        "\n[ZDD] Warning: mapping file '%s' has invalid size (%ld bytes). "
		        "Continuing without versioned packing/export.\n",
		        mapping_path, file_size);
		fclose(fp);
		return 0;
	}

	entries = (size_t) (file_size / 4L);
	if (entries == 0) {
		fprintf(stderr,
		        "\n[ZDD] Warning: mapping file '%s' is empty. "
		        "Continuing without versioned packing/export.\n",
		        mapping_path);
		fclose(fp);
		return 0;
	}

	if (fseek(fp, 0L, SEEK_SET) != 0) {
		fprintf(stderr,
		        "\n[ZDD] Warning: could not rewind mapping file '%s': %s. "
		        "Continuing without versioned packing/export.\n",
		        mapping_path, strerror(errno));
		fclose(fp);
		return 0;
	}

	*page_map = (uint32_t *) malloc(entries * sizeof(uint32_t));
	if (*page_map == NULL) {
		fprintf(stderr,
		        "\n[ZDD] Warning: could not allocate %zu bytes for mapping. "
		        "Continuing without versioned packing/export.\n",
		        entries * sizeof(uint32_t));
		fclose(fp);
		return 0;
	}

	for (i = 0; i < entries; ++i) {
		if (fread(bytes, 1, 4, fp) != 4) {
			fprintf(stderr,
			        "\n[ZDD] Warning: could not read mapping file '%s': %s. "
			        "Continuing without versioned packing/export.\n",
			        mapping_path, strerror(errno));
			free(*page_map);
			*page_map = NULL;
			fclose(fp);
			return 0;
		}
		(*page_map)[i] = ((uint32_t) bytes[0]) |
		                 ((uint32_t) bytes[1] << 8) |
		                 ((uint32_t) bytes[2] << 16) |
		                 ((uint32_t) bytes[3] << 24);
	}

	fclose(fp);
	*map_len = entries;
	return 1;
}

static int find_master_doc_id(uint32_t global_id, const uint32_t *page_map, size_t map_len, uint32_t *master_out) {
	size_t lo = 0;
	size_t hi;
	size_t mid;

	if (page_map == NULL || map_len == 0 || global_id < page_map[0]) {
		return 0;
	}

	hi = map_len - 1;
	while (lo < hi) {
		mid = lo + (hi - lo + 1) / 2;
		if (page_map[mid] <= global_id) {
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}

	*master_out = (uint32_t) lo;
	return 1;
}

static int pack_occ_lists_for_zdd(uint nwords, uint *lenList, uint **occList, const uint32_t *page_map, size_t map_len) {
	ulong w;
	ulong doc;
	int warned_out_of_range = 0;
	ulong out_of_range_count = 0;
	ulong trunc_master_count = 0;
	ulong trunc_rel_count = 0;
	uint32_t max_master_seen = 0;
	uint32_t max_rel_seen = 0;
	uint32_t max_master_packed = 0;
	uint32_t max_rel_packed = 0;
	uint32_t max_packed_value = 0;

	for (w = 0; w < nwords; ++w) {
		for (doc = 0; doc < lenList[w]; ++doc) {
			uint32_t global_id = (uint32_t) occList[w][doc];
			uint32_t master;
			uint32_t rel;
			uint32_t master_packed;
			uint32_t rel_packed;
			uint32_t packed;

			if (!find_master_doc_id(global_id, page_map, map_len, &master)) {
				if (!warned_out_of_range) {
					fprintf(stderr,
					        "\n[ZDD] Warning: GlobalID fuera del rango de mapeo. "
					        "Esas entradas seran mapeadas como (0,0).\n");
					warned_out_of_range = 1;
				}
				out_of_range_count++;
				master = 0;
				rel = 0;
			} else {
				rel = global_id - page_map[master];
			}

			if (master > max_master_seen) max_master_seen = master;
			if (rel > max_rel_seen) max_rel_seen = rel;

			master_packed = master & 0xFFFFu;
			rel_packed = rel & 0xFFFFu;
			if (master != master_packed) trunc_master_count++;
			if (rel != rel_packed) trunc_rel_count++;

			if (master_packed > max_master_packed) max_master_packed = master_packed;
			if (rel_packed > max_rel_packed) max_rel_packed = rel_packed;

			packed = (master_packed << 16) | rel_packed;
			if (packed > max_packed_value) max_packed_value = packed;
			occList[w][doc] = (uint) packed;
		}
	}

	fprintf(stderr,
	        "\n[ZDD] Packing 16/16 stats: maxMaster(real)=%u, maxRel(real)=%u, "
	        "maxMaster(packed)=%u, maxRel(packed)=%u, maxPackedValue=%u (0x%08X)",
	        (uint) max_master_seen, (uint) max_rel_seen,
	        (uint) max_master_packed, (uint) max_rel_packed,
	        (uint) max_packed_value, (uint) max_packed_value);
	fprintf(stderr,
	        "\n[ZDD] Packing 16/16 warnings: truncMaster=%lu, truncRel=%lu, outOfRange=%lu\n",
	        trunc_master_count, trunc_rel_count, out_of_range_count);

	return 1;
}

static int export_occ_lists_unpacked_txt(uint nwords, uint *lenList, uint **occList, const char *output_path) {
	FILE *fzdd;
	ulong w;
	ulong doc;

	fzdd = fopen(output_path, "w");
	if (fzdd == NULL) {
		fprintf(stderr,
		        "\n[ZDD] Warning: could not create '%s': %s. "
		        "Continuing without txt export.\n",
		        output_path, strerror(errno));
		return 0;
	}

	for (w = 0; w < nwords; ++w) {
		fprintf(fzdd, "T[%lu]:", w);
		for (doc = 0; doc < lenList[w]; ++doc) {
			uint32_t packed = (uint32_t) occList[w][doc];
			uint32_t master = (packed >> 16) & 0xFFFFu;
			uint32_t rel = packed & 0xFFFFu;
			fprintf(fzdd, " (%u,%u)", (uint) master, (uint) rel);
		}
		fprintf(fzdd, "\n");
	}

	fclose(fzdd);
	return 1;
}
```

*(Continúa el archivo original en `loadTextInMem` inmediatamente después.)*

---

### 1.2 Reemplazo: función `prepareSourceFormatForIListBuilder` (aprox. líneas **946–978**)

**Antes:** solo calculaba `sourcelen`, `malloc` de `source`, copiaba datos; podía incluir un bloque exportador antiguo a `listas_wikipedia_zdd.txt`.

**Ahora:** intercepción al inicio con `PAGE_MAPPING_BIN`, packing, export `listas_wikipedia_zdd_versionadas.txt`, `free(page_map)`; luego el flujo original sin exportador legacy.

```c
int prepareSourceFormatForIListBuilder (uint nwords, uint maxPost, uint *lenList, 
										uint **occList, uint **formatedList, ulong *formatedLen){
	ulong i,j;
	uint32_t *page_map = NULL;
	size_t map_len = 0;
	ulong sourcelen=1+1;  //nwords and maxPost

	/* Intercept occList before build_il format generation. */
	if (load_page_mapping(&page_map, &map_len)) {
		pack_occ_lists_for_zdd(nwords, lenList, occList, page_map, map_len);
		export_occ_lists_unpacked_txt(nwords, lenList, occList, "listas_wikipedia_zdd_versionadas.txt");
		free(page_map);
	}

	for (i=0;i<nwords;i++) 	sourcelen += 1 + lenList[i];
	
	uint *source = (uint *) malloc (sourcelen * sizeof(uint));
	if (!source) {
		fprintf(stderr,"\n could not allocate %lu bytes... [PrepareSourceFormatForIlistBuilder]\nexitting!!\n",(ulong) (sourcelen * sizeof(uint)) ); exit(0);
	}
	
	ulong z=0; 
	source[z++]= nwords; 	source[z++]= maxPost;  //z=2;
	for (i=0;i<nwords;i++) {
		source[z++] = lenList[i];
		for (j=0;j<lenList[i];j++) {
			source[z++] = occList[i][j];
		}
	}
	*formatedLen  = sourcelen;
	*formatedList = source;
	return 0;
}
```

---

## 2. `uiHRDC/uiHRDC/indexes/NOPOS/II_docs/ilists.imp/8.pfordelta/src/ilspire07.c`

### 2.1 Reemplazo: buffer de validación `extract_no_malloc` en `build_il` (aprox. líneas **363–383**)

**Antes:**
```c
uint *list = (uint *) malloc(sizeof(uint) * (il->maxPostingValue+1+ PFD_BS2));
```

**Ahora:** tamaño basado en `maxlenlist + PFD_BS2` (coherente con el contrato de `extractListNoMalloc_il`).

```c
		fprintf(stderr,"\n ! CHECKING that the decoded lists are identical to the original ones (extract_no_malloc). ");
		{ //checking:: decoding a list
			uint i, id ,len;
			size_t scratch_len = (size_t) maxlenlist + (size_t) PFD_BS2;
			uint *list = (uint *) malloc(sizeof(uint) * scratch_len);
			if (!list) {
				fprintf(stderr,"\n could not allocate %lu bytes for extract_no_malloc validation buffer\n",
				               (ulong) (sizeof(uint) * scratch_len));
				exit(0);
			}
			
			for (id=0;id < il->nlists;id++) {
				extractListNoMalloc_il(*ail,id,list,&len);	
				for (i=0;i<len;i++){
					if (list[i] != (occList[id][i]-DOCid_ADD)) {
						fprintf(stderr,"\n decoding of lists failed for id = %d: DIFFERENT!!: (%d,%d)",id, list[i],occList[id][i]);
						exit(0);
					}
				} 
			}			
			free(list);				

		}
```

---

## 3. `uiHRDC/uiHRDC/indexes/POS/II_docs/ilists.gap.imp/8.pfordelta/src/ilspire07.c`

### 3.1 Mismo parche que en §2 (aprox. líneas **363–383**)

Misma sustitución del `malloc` basado en `maxPostingValue` por `scratch_len = maxlenlist + PFD_BS2` y comprobación de fallo de `malloc`.

*(El bloque de código es idéntico al de la sección 2.1.)*

---

## Resumen de archivos tocados

| Archivo | Cambio principal |
|---------|------------------|
| `NOPOS/II_docs/src/utils/ii.c` | ZDD: mapeo LE, búsqueda binaria, packing 16/16, stats stderr, TXT versionado, hook en `prepareSourceFormatForIListBuilder` |
| `NOPOS/II_docs/ilists.imp/8.pfordelta/src/ilspire07.c` | Fix memoria validación `extract_no_malloc` |
| `POS/II_docs/ilists.gap.imp/8.pfordelta/src/ilspire07.c` | Idem fix memoria |

---

## Notas

- Variables de entorno: `PAGE_MAPPING_BIN` (default `page_mapping.bin`).
- Salida texto: `listas_wikipedia_zdd_versionadas.txt`.
- Otras copias del proyecto (`II_docs_large`, `EliasFano`, etc.) **no** están incluidas en este documento si no fueron editadas en esta sesión.
