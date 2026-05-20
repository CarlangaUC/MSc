import struct
import os

def calcular_jaccard(set1, set2):
    if not set1 or not set2: return 0.0
    interseccion = len(set1.intersection(set2))
    union = len(set1.union(set2))
    sim = interseccion / union
    return sim

def mapeo_avanzado_con_detalles(path_txt, path_boundaries, umbral=0.4):
    """
    Recorre los documentos, extrae el texto, calcula similitud y mapea versiones.
    """
    # 1. Cargar Offsets (8 bytes uint64 'Q')
    offsets = []
    with open(path_boundaries, 'rb') as fb:
        size = os.path.getsize(path_boundaries)
        for _ in range(size // 8):
            offsets.append(struct.unpack('Q', fb.read(8))[0])
    
    ndocs = len(offsets) - 1
    mapeo = []
    
    print(f"--- INICIANDO ANÁLISIS DINÁMICO DE {ndocs} DOCUMENTOS ---")
    print(f"Umbral de Similitud (Jaccard): {umbral}\n")

    with open(path_txt, 'rb') as ft:
        art_id = 0
        ver_id = 0
        set_previo = set()

        for i in range(ndocs):
            # Extraer texto usando los offsets (DocBoundaries)
            ft.seek(offsets[i])
            length = offsets[i+1] - offsets[i]
            # Solo leemos los primeros 2000 caracteres para velocidad del cálculo
            raw_text = ft.read(min(length, 2000)).decode('utf-8', errors='ignore').lower()
            
            # Tokenización básica para el set (palabras > 3 letras)
            set_actual = set([w for w in raw_text.split() if len(w) > 3])

            if i == 0:
                print(f"[DOC 0] Primer artículo detectado (ID: {art_id})")
            else:
                sim = calcular_jaccard(set_previo, set_actual)
                
                # DETALLES ASOCIADOS A CADA PASO
                if sim < umbral:
                    print(f"[CAMBIO] ID {i-1} vs {i} | Sim: {sim:.4f} < {umbral} -> !!! NUEVO ARTÍCULO (ArtID {art_id+1}) !!!")
                    art_id += 1
                    ver_id = 0
                else:
                    # Opcional: print de progreso silencioso para versiones similares
                    if i % 100 == 0:
                        print(f"[SIMILAR] ID {i} | Sim: {sim:.4f} -> Sigue siendo Articulo {art_id}, Version {ver_id+1}")
                    ver_id += 1
            
            mapeo.append((i, art_id, ver_id))
            set_previo = set_actual

    return mapeo

# --- CONFIGURACIÓN DE RUTAS ---
# Ajusta estas rutas a tu estructura de carpetas
base = os.path.join("uiHRDC", "uiHRDC", "data", "texts")
path_txt = os.path.join(base, "torsen.text200mb.txt")
path_bound = path_txt + ".DOCBOUNDARIES.ul"

# Ejecución
resultado = mapeo_avanzado_con_detalles(path_txt, path_bound, umbral=0.35)

# Guardar
with open("mapeo_dinamico_detallado.csv", "w") as f:
    f.write("GlobalID,ArticleID,LocalVersion\n")
    for gid, aid, vid in resultado:
        f.write(f"{gid},{aid},{vid}\n")

print(f"\n[ÉXITO] Análisis terminado. Mapeo guardado en 'mapeo_dinamico_detallado.csv'")

"""
import struct
import os

def calcular_similitud_bolsa(texto1, texto2):
    """
    Compara dos textos y devuelve qué porcentaje de palabras comparten.
    """
    set1 = set(texto1.split())
    set2 = set(texto2.split())
    if not set1 or not set2: return 0.0
    interseccion = len(set1.intersection(set2))
    return interseccion / len(set1) # Proporción de palabras que persisten

def mapeo_maestro_offsets(path_txt, path_boundaries):
    offsets = []
    # 1. Cargar Offsets (8 bytes uint64 'Q')
    with open(path_boundaries, 'rb') as fb:
        size = os.path.getsize(path_boundaries)
        for _ in range(size // 8):
            offsets.append(struct.unpack('Q', fb.read(8))[0])
    
    ndocs = len(offsets) - 1
    mapeo = []
    
    art_id = 0
    ver_local = 0
    contenido_previo = ""

    print(f"--- ANALIZANDO COLECCIÓN: {ndocs} DOCUMENTOS ---")

    with open(path_txt, 'r', encoding='utf-8', errors='ignore') as ft:
        for i in range(ndocs):
            # SALTO DETERMINÍSTICO usando Offsets
            ft.seek(offsets[i])
            length = offsets[i+1] - offsets[i]
            
            # Cargamos una muestra significativa (ej. 2000 bytes)
            # Esto es lo que guardamos en RAM para comparar
            contenido_actual = ft.read(min(length, 2000)).lower()
            
            if i == 0:
                # El primer documento siempre es el inicio del ArtID 0
                similitud = 1.0
            else:
                # COMPARACIÓN DE CONTENIDO REAL
                similitud = calcular_similitud_bolsa(contenido_previo, contenido_actual)
                
                # UMBRAL DE PERSISTENCIA (0.2 es muy conservador, evita el vandalismo)
                # Si comparten menos del 20% de las palabras, ES UN TEMA NUEVO
                if similitud < 0.2:
                    art_id += 1
                    ver_local = 0
                else:
                    ver_local += 1

            # Extraemos un "Título Visual" para el log
            titulo_visual = " ".join(contenido_actual.split()[:4])
            
            # Guardamos el mapeo
            mapeo.append((i, art_id, ver_local, similitud, titulo_visual))
            
            # Actualizamos para la siguiente iteración
            contenido_previo = contenido_actual

            if i % 1000 == 0:
                print(f"Procesando offset {offsets[i]} (Doc {i}/{ndocs})...")

    return mapeo

# --- CONFIGURACIÓN ---
base = "uiHRDC/uiHRDC/data/texts/torsen.text200mb.txt"
res = mapeo_maestro_offsets(base, base + ".DOCBOUNDARIES.ul")

# 4. Guardar resultado final
with open("mapeo_real_wikipedia.csv", "w") as f:
    f.write("GlobalID,ArticleID,LocalVersion,SimilitudContent,Preview\n")
    for gid, aid, vid, sim, pre in res:
        f.write(f"{gid},{aid},{vid},{sim:.4f},{pre.replace(',',' ')}\n")

print("\n[ÉXITO] Archivo 'mapeo_real_wikipedia.csv' generado.")
"""