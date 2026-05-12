import struct
import os
import re

def acortar_dataset_wiki(txt_in, bound_in, log_in, txt_out, bound_out, log_out, target_size_mb):
    target_size_bytes = target_size_mb * 1024 * 1024
    
    # 1. Leer las fronteras originales del archivo binario
    print(f"Leyendo fronteras originales desde {bound_in}...")
    with open(bound_in, 'rb') as fb:
        data = fb.read()
        num_enteros = len(data) // 4
        boundaries = struct.unpack(f'<{num_enteros}I', data)
    
    print(boundaries[:10], "...", boundaries[-10:])  # Mostrar las primeras y últimas fronteras para verificación

    # 2. Analizar el log para calcular un corte limpio
    print(f"Buscando un corte limpio que alcance o supere los {target_size_mb} MB...")
    versiones_acumuladas = 0
    docs_maestros_guardados = 0
    lineas_log_guardar = []
    
    with open(log_in, 'r', encoding='utf-8', errors='ignore') as f_log:
        for line in f_log:
            # Primero guardamos la línea para que el log quede exacto (no borramos nada)
            lineas_log_guardar.append(line)
            
            match = re.search(r'parsed\s+(\d+)\s+versions,\s+for\s+document', line)
            if match:
                num_versiones = int(match.group(1))
                versiones_acumuladas += num_versiones
                docs_maestros_guardados += 1
                
                # Revisamos el tamaño del archivo hasta esta nueva versión
                if versiones_acumuladas < len(boundaries):
                    tamaño_actual = boundaries[versiones_acumuladas]
                    
                    # Si ya cruzamos el límite de los 100 MB, detenemos la lectura aquí
                    if tamaño_actual >= target_size_bytes:
                        break

    bytes_a_copiar = boundaries[versiones_acumuladas]
    print(f"Corte determinado: {docs_maestros_guardados} Documentos Maestros ({versiones_acumuladas} versiones totales).")
    print(f"Tamaño exacto del nuevo texto: {bytes_a_copiar / (1024**2):.2f} MB")

    # 3. Generar el nuevo Log
    print(f"Generando {log_out}...")
    with open(log_out, 'w', encoding='utf-8') as f_log_out:
        f_log_out.writelines(lineas_log_guardar)

    # 4. Generar el nuevo archivo de fronteras (.DOCBOUNDARIES.ul)
    print(f"Generando {bound_out}...")
    nuevos_boundaries = boundaries[:versiones_acumuladas + 1]
    with open(bound_out, 'wb') as fb_out:
        fb_out.write(struct.pack(f'<{len(nuevos_boundaries)}Q', *nuevos_boundaries))
        
    # 5. Generar el nuevo archivo de texto
    print(f"Generando {txt_out} (copiando fragmentos)...")
    with open(txt_in, 'rb') as ft_in, open(txt_out, 'wb') as ft_out:
        chunk_size = 1024 * 1024 * 10  # Copia en fragmentos de 10MB para no ahogar la RAM
        bytes_restantes = bytes_a_copiar
        while bytes_restantes > 0:
            leer = min(chunk_size, bytes_restantes)
            chunk = ft_in.read(leer)
            if not chunk:
                break
            ft_out.write(chunk)
            bytes_restantes -= len(chunk)

    print("\n¡Dataset acortado con éxito! Listo para generar page_mapping y compilar.")

# ==========================================
# EJECUCIÓN
# ==========================================
name_input = "wiki_src2gb"
name_output = "wiki_2gb"

acortar_dataset_wiki(
    txt_in = os.path.join("archivos_test", f"{name_input}.txt"),
    bound_in = os.path.join("archivos_test", f"{name_input}.txt.DOCBOUNDARIES.ul"),
    log_in = os.path.join("archivos_test", f"{name_input}.log"),
    
    txt_out = os.path.join("archivos_test", f"{name_output}.txt"),
    bound_out = os.path.join("archivos_test", f"{name_output}.txt.DOCBOUNDARIES.ul"),
    log_out = os.path.join("archivos_test", f"{name_output}.log"),
    
    target_size_mb = 2000  # Puedes ajustar este valor si después quieres probar con 500MB
)

# todo en archivos_test/wiki_100mb.* quedará el nuevo dataset acortado, listo para ser procesado por convertir_versionado_input.py