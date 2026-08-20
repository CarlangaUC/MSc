import struct
import os
from collections import Counter # Necesario para contar frecuencias eficientemente

def convertir_a_binario_pisa(input_path, output_path, reordenar_frec=True):
    print(f"--- Procesando archivo ---")
    print(f"Entrada: {input_path}")
    print(f"Salida:  {output_path}")
    print(f"Modo Reordenamiento Frecuencia: {'ACTIVADO' if reordenar_frec else 'DESACTIVADO'}")
    
    if not os.path.exists(input_path):
        print(f"Error: El archivo {input_path} no existe.")
        return

    conjuntos = []
    
    print("Leyendo y limpiando datos...")

    try:
        with open(input_path, 'r') as f:
            for i, linea in enumerate(f):
                linea = linea.strip()
                if not linea: continue
                
                try:

                    # Limpieza asumiendo que en puede venir separado por "," "" ";" y demas
                    linea_limpia = linea.replace(',', ' ').replace('[', ' ').replace(']', ' ').replace(';', ' ')
                    
                    # Leemos los enteros
                    elementos = [int(x) for x in linea_limpia.split()]
                    
                    if elementos:
                        # Limpieza estándar: ordenar y únicos
                        elementos_limpios = sorted(list(set(elementos)))
                        conjuntos.append(elementos_limpios)

                except ValueError:
                    print(f"Advertencia: Línea {i+1} ignorada (formato incorrecto).")
                    continue
    except Exception as e:
        print(f"Error leyendo el archivo: {e}")
        return

    if not conjuntos:
        print("Error: No se encontraron datos válidos.")
        return

    print(f" -> Listas leídas: {len(conjuntos)}")

    # Reordenar ids por frecuencia, buscando compartir y explotar propiedad zdd

    if reordenar_frec:
        print("Calculando frecuencias y remapeando IDs...")
        
        # 1. Contar ocurrencias globales
        frecuencias = Counter()
        for s in conjuntos:
            frecuencias.update(s)
        
        print(f" -> Items únicos encontrados: {len(frecuencias)}")

        # 2. Crear Ranking: (Frecuencia Descendente, Valor Ascendente para desempatar)
        # El item más frecuente tendrá el ID 1 (Base del ZDD)
        items_ordenados = sorted(frecuencias.items(), key=lambda x: (-x[1], x[0]))
        
        mapping = {}
        current_id = 1
        
        for item, freq in items_ordenados:
            mapping[item] = current_id
            current_id += 1
            
        # Mostrar TOP 5 para verificar
        print(" -> Top 5 Frecuentes (Original -> NuevoID):")
        for k in range(min(5, len(items_ordenados))):
            orig = items_ordenados[k][0]
            count = items_ordenados[k][1]
            print(f"    Item {orig} ({count} veces) -> ID {mapping[orig]}")

        # 3. Aplicar el mapeo a los conjuntos
        conjuntos_optimizados = []
        for s in conjuntos:
            # Traducir IDs
            nuevo_s = [mapping[x] for x in s]
            # IMPORTANTE: Al cambiar IDs, el orden se pierde.
            # El formato PISA y ZDD requieren listas ordenadas crecientemente.
            nuevo_s.sort() 
            conjuntos_optimizados.append(nuevo_s)
            
        conjuntos = conjuntos_optimizados # Reemplazamos la lista original

    # Formato BIN PISA
    os.makedirs(os.path.dirname(output_path) if os.path.dirname(output_path) else ".", exist_ok=True)
    
    try:
        with open(output_path, "wb") as f:
            
            # === HEADER ===
            # Total de listas (32-bit uint)
            total_listas = len(conjuntos)
            f.write(struct.pack('<I', total_listas))

            # === CUERPO ===
            # Formato: [Largo] [id1, id2, ..., idN]
            for s in conjuntos:
                n = len(s)
                # 1. Largo de la lista
                f.write(struct.pack('<I', n))
                # 2. Los datos (ahora remapeados y ordenados)
                if n > 0:
                    f.write(struct.pack(f'<{n}I', *s))

        print(f"¡Éxito! Archivo generado en: {output_path}")
        if reordenar_frec:
            print("NOTA: Los IDs en el binario NO son los originales, han sido optimizados.")
        
    except Exception as e:
        print(f"Error escribiendo binario: {e}")

if __name__ == "__main__":

    # Lo convierte desde .txt/.dat a .bin en formato adecuado y formato
    
    entrada_usuario = input("Ingrese el nombre del archivo (ej: 'retail.txt'): ").strip()
    
    # Opción para forzar el reordenamiento desde el main
    usar_optimizacion = False 

    carpeta = "archivos_test"
    archivo_encontrado = None

    candidatos = [
        entrada_usuario,
        os.path.join(carpeta, entrada_usuario),
        f"{entrada_usuario}.dat",
        f"{entrada_usuario}.txt",
        os.path.join(carpeta, f"{entrada_usuario}.dat"),
        os.path.join(carpeta, f"{entrada_usuario}.txt")
    ]

    for ruta in candidatos:
        if os.path.exists(ruta) and os.path.isfile(ruta):
            archivo_encontrado = ruta
            break
    
    if archivo_encontrado:
        # Generamos un nombre de salida que indique si está optimizado
        base, _ = os.path.splitext(archivo_encontrado)
        sufijo = "_opt" if usar_optimizacion else ""
        ruta_salida = f"{base}{sufijo}.bin"
        
        convertir_a_binario_pisa(archivo_encontrado, ruta_salida, reordenar_frec=usar_optimizacion)

    else:
        print("\n❌ Error: No se encontró el archivo.")