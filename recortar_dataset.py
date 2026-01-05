import os
import random
import struct

def recortar_y_generar(input_path, output_path_base, cantidad_objetivo, min_elementos):
    if not os.path.exists(input_path):
        print(f"Error: No encuentro '{input_path}'")
        return

    print(f"Leyendo {input_path} en memoria...")
    
    # Lectura robusta ignorando líneas vacías
    with open(input_path, 'r') as f:
        lineas = [l.strip() for l in f if l.strip()]

    total_original = len(lineas)
    print(f"Total original: {total_original} conjuntos.")

    # Selección Aleatoria
    if cantidad_objetivo >= total_original:
        print("La cantidad pedida es mayor o igual al original. Se procesará todo el archivo.")
        seleccion = lineas
    else:
        print(f"Seleccionando {cantidad_objetivo} conjuntos aleatorios (antes del filtro)...")
        seleccion = random.sample(lineas, cantidad_objetivo)

    datos_limpios = []
    universo_local = 0
    total_enteros = 0
    
    # Contadores para el reporte
    descartados_por_longitud = 0
    
    print(f"Limpiando, ordenando y filtrando (Mínimo {min_elementos} elementos)...")
    
    for linea in seleccion:
        try:
            # Parsear enteros
            numeros = [int(x) for x in linea.split()]
            
            # Limpieza para ZDD (Ordenados y Únicos)
            numeros = sorted(list(set(numeros)))
            
            # --- FILTRO DE TAMAÑO ---
            if len(numeros) < min_elementos:
                descartados_por_longitud += 1
                continue # Saltamos a la siguiente iteración
            
            # Si pasa el filtro, lo guardamos
            if numeros:
                datos_limpios.append(numeros)
                total_enteros += len(numeros)
                if numeros[-1] > universo_local:
                    universo_local = numeros[-1]
                    
        except ValueError:
            continue

    if not datos_limpios:
        print("Error: No quedaron datos válidos tras el filtro.")
        return

    # --- 1. GUARDAR TXT (Visualización) ---
    out_txt = f"{output_path_base}.txt"
    with open(out_txt, 'w') as f:
        for nums in datos_limpios:
            linea_str = " ".join(map(str, nums))
            f.write(linea_str + "\n")

    # --- 2. GUARDAR BINARIO PISA .docs (Para tu C++) ---
    out_bin = f"{output_path_base}.bin"
    
    try:
        with open(out_bin, 'wb') as f_bin:
            # HEADER GLOBAL PISA: Cantidad total de listas (1 entero de 4 bytes)
            total_listas = len(datos_limpios)
            f_bin.write(struct.pack('<I', total_listas))
            
            # CUERPO
            for nums in datos_limpios:
                n = len(nums)
                # Largo de la lista
                f_bin.write(struct.pack('<I', n))
                # Datos de la lista
                f_bin.write(struct.pack(f'<{n}I', *nums))
                
    except Exception as e:
        print(f"Error escribiendo binario: {e}")
        return

    print(f"\n--- Resumen Final ---")
    print(f"Conjuntos seleccionados inicialmente: {len(seleccion)}")
    print(f"Descartados (len < {min_elementos}):      {descartados_por_longitud}")
    print(f"Conjuntos FINALES guardados:      {len(datos_limpios)}")
    print(f"Universo (Max ID):                {universo_local}")
    print(f"Total Enteros:                    {total_enteros}")
    print(f"Archivos generados:")
    print(f"  -> {out_txt}")
    print(f"  -> {out_bin}")

if __name__ == "__main__":
    # Configuración por defecto
    archivo_entrada = "archivos_test/webdocs.dat" 
    nombre_base = "archivos_test/webdocs_filtrado" 

    try:
        N = int(input("¿Cuántos conjuntos quieres intentar tomar? : "))
        m = int(input("¿Cuál es la cantidad mínima de elementos por conjunto? : "))
        
        # Generamos un nombre de archivo descriptivo automáticamente
        # Ej: 1mq_mini_1000_min10
        nombre_salida = f"{nombre_base}_{N}_min_{m}"
        
        recortar_y_generar(archivo_entrada, nombre_salida, N, m)
        
    except ValueError:
        print("Por favor ingresa un número entero válido.")