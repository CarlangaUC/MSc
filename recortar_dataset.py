import os
import random
import struct

def recortar_y_generar_binario(input_path, output_path_base, cantidad_objetivo):
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
        print("La cantidad pedida es mayor o igual al original. Se conserva todo.")
        seleccion = lineas
    else:
        print(f"Seleccionando {cantidad_objetivo} conjuntos aleatorios...")
        seleccion = random.sample(lineas, cantidad_objetivo)

    datos_limpios = []
    universo_local = 0
    total_enteros = 0
    
    print("Limpiando y estructurando datos...")
    
    for linea in seleccion:
        try:
            # Parsear enteros
            numeros = [int(x) for x in linea.split()]
            
            # Limpieza para ZDD (Ordenados y Únicos)
            # Esto es vital para que la intersección/unión funcione bien
            numeros = sorted(list(set(numeros)))
            
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
    # Este formato incluye el HEADER correcto (Total de listas)
    out_bin = f"{output_path_base}.bin" # O .docs, es lo mismo
    
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
                # NOTA: Los guardamos tal cual (raw). 
                # Tu código C++ ya tiene el "+1" para manejar el ID 0 si aparece.
                f_bin.write(struct.pack(f'<{n}I', *nums))
                
    except Exception as e:
        print(f"Error escribiendo binario: {e}")
        return

    print(f"\n--- Resumen Final ---")
    print(f"Conjuntos procesados: {len(datos_limpios)}")
    print(f"Universo (Max ID):    {universo_local}")
    print(f"Archivos generados:")
    print(f"  -> {out_txt} (Texto)")
    print(f"  -> {out_bin} (Binario PISA compatible con tu test.cpp)")

if __name__ == "__main__":
    # Configuración por defecto o inputs
    archivo_entrada = "archivos_test/1mq.txt" 
    
    # Nombre base sin extensión (el script agrega .txt y .bin)
    nombre_salida = "archivos_test/1mq_mini_100" 

    try:
        N = int(input("¿Cuántos conjuntos quieres conservar? : "))
        recortar_y_generar_binario(archivo_entrada, nombre_salida, N)
    except ValueError:
        print("Por favor ingresa un número entero válido.")