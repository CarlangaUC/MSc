import struct
import os

def convertir_txt_a_bin(input_path, output_path):
    print(f"Leyendo desde: {input_path}")
    
    if not os.path.exists(input_path):
        print(f"Error: El archivo {input_path} no existe.")
        return

    conjuntos = []
    universo = 0

    # Leer el archivo .txt y procesar los conjuntos
    with open(input_path, 'r') as f:
        for linea in f:
            linea = linea.strip()
            if not linea: continue
            
            # Separar por comas, convertir a int y guardar como lista
            # Ejemplo: "6, 5, 4, 3" -> [6, 5, 4, 3]
            try:
                elementos = [int(x.strip()) for x in linea.split(',')]
                if elementos:
                    conjuntos.append(elementos)
                    # Actualizar el universo (el valor máximo encontrado)
                    max_val = max(elementos)
                    if max_val > universo:
                        universo = max_val
            except ValueError:
                print(f"Advertencia: Línea ignorada (formato incorrecto): {linea}")
                continue

    if not conjuntos:
        print("Error: No se encontraron conjuntos válidos en el archivo .txt")
        return

    print(f"Se encontraron {len(conjuntos)} conjuntos.")
    print(f"Universo detectado (valor máximo): {universo}")
    print(f"Generando {output_path}...")

    # Escribir el archivo .bin
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    
    dummy_val = 0 # Valor '_1' del header

    with open(output_path, "wb") as f:
        # HEADER (8 bytes) 
        # Escribir _1 (4 bytes) y u (4 bytes)
        # 'I' indica unsigned int de 32 bits (4 bytes)
        # '<' indica Little Endian (estándar en x86/x64)
        f.write(struct.pack('<II', dummy_val, universo))

        # --- CUERPO ---
        for s in conjuntos:
            n = len(s)
            # 1. Escribir tamaño del conjunto (4 bytes)
            f.write(struct.pack('<I', n))
            
            # 2. Escribir los elementos (n * 4 bytes)
            for item in s:
                f.write(struct.pack('<I', item))

    print("¡Archivo binario generado exitosamente!")


ruta_txt = os.path.join("archivos_test", "conjuntos.txt")

ruta_bin = os.path.join("archivos_test", "conjuntos.bin")

convertir_txt_a_bin(ruta_txt, ruta_bin)