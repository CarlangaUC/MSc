import os
import random

def recortar_dataset(input_path, output_path_base, cantidad_objetivo):
    if not os.path.exists(input_path):
        print(f"Error: No encuentro '{input_path}'")
        return

    print(f"Leyendo {input_path}...")
    
    with open(input_path, 'r') as f:
        lineas = [l.strip() for l in f if l.strip()]

    total_original = len(lineas)
    print(f"Total original: {total_original} conjuntos.")

    if cantidad_objetivo >= total_original:
        print("La cantidad pedida es mayor o igual al original. No se recorta nada.")
        seleccion = lineas
    else:
        print(f"Seleccionando {cantidad_objetivo} conjuntos aleatorios...")
        seleccion = random.sample(lineas, cantidad_objetivo)


    datos_limpios = []
    universo_local = 0
    
    for linea in seleccion:
        try:
            numeros = [int(x) for x in linea.split()]
            numeros = sorted(list(set(numeros))) # HECHO EN CODIGO IGUALMENTE
            
            if numeros:
                datos_limpios.append(numeros)
                if numeros[-1] > universo_local:
                    universo_local = numeros[-1]
        except ValueError:
            continue

    out_txt = f"{output_path_base}.txt"
    with open(out_txt, 'w') as f:
        for nums in datos_limpios:
            linea_str = " ".join(map(str, nums))
            f.write(linea_str + "\n")

    print(f"--- Resumen Recorte ---")
    print(f"Archivo generado: {out_txt}")
    print(f"Conjuntos: {len(datos_limpios)}")
    print(f"Universo detectado: {universo_local}")
    print("Ahora puedes correr tu 'crear_formato_input.py' sobre este archivo nuevo.")

if __name__ == "__main__":
    archivo_entrada = "archivos_test/1mq.txt" 
    nombre_salida = "archivos_test/1mq_mini" 
    

    N = int(input("¿Cuántos conjuntos quieres conservar? : "))
    
    recortar_dataset(archivo_entrada, nombre_salida, N)