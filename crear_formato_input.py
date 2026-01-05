import struct
import os

def convertir_a_binario_pisa(input_path, output_path):
    print(f"--- Procesando archivo ---")
    print(f"Entrada: {input_path}")
    print(f"Salida:  {output_path}")
    
    if not os.path.exists(input_path):
        print(f"Error: El archivo {input_path} no existe.")
        return

    conjuntos = []
    
    print("Leyendo y limpiando datos...")

    with open(input_path, 'r') as f:
        for i, linea in enumerate(f):
            linea = linea.strip()
            if not linea: continue
            
            try:
                # Leemos los enteros
                elementos = [int(x) for x in linea.split()]
                
                if elementos:
                    # Limpieza estándar: ordenar y únicos
                    elementos_limpios = sorted(list(set(elementos)))
                    
                    # --- OPCIONAL: VALIDACIÓN DE SEGURIDAD PARA TDZDD ---
                    # Si tus datos tienen 0 y no sumas 1 aquí, 
                    # recuerda que tu C++ SÍ debe sumar 1 al leer.
                    # Si tu C++ ya suma 1, esto está bien tal cual.
                    
                    conjuntos.append(elementos_limpios)

            except ValueError:
                print(f"Advertencia: Línea {i+1} ignorada.")
                continue

    if not conjuntos:
        print("Error: No se encontraron datos válidos.")
        return

    print(f" -> Listas procesadas: {len(conjuntos)}")

    # Escritura Binaria (Formato PISA .docs)
    print(f"Escribiendo binario...")
    os.makedirs(os.path.dirname(output_path) if os.path.dirname(output_path) else ".", exist_ok=True)
    
    try:
        with open(output_path, "wb") as f:
            
            # === HEADER CORRECTO ===
            # Escribimos LA CANTIDAD DE LISTAS (1 entero de 32 bits)
            total_listas = len(conjuntos)
            f.write(struct.pack('<I', total_listas))

            # === CUERPO ===
            for s in conjuntos:
                n = len(s)
                # 1. Largo de la lista
                f.write(struct.pack('<I', n))
                # 2. Los datos
                f.write(struct.pack(f'<{n}I', *s))

        print(f"¡Éxito! Archivo generado en: {output_path}")
        
    except Exception as e:
        print(f"Error escribiendo binario: {e}")

if __name__ == "__main__":
    entrada_usuario = input("Ingrese el nombre del archivo (ej: 'webdocs' o '1mq.txt'): ").strip()
    
    carpeta = "archivos_test"
    archivo_encontrado = None

    # 1. Lista de posibles ubicaciones y extensiones a probar
    candidatos = [
        entrada_usuario,                                      # Tal cual lo escribió
        os.path.join(carpeta, entrada_usuario),               # En carpeta archivos_test
        f"{entrada_usuario}.dat",                             # Agregando .dat
        f"{entrada_usuario}.txt",                             # Agregando .txt
        os.path.join(carpeta, f"{entrada_usuario}.dat"),      # En carpeta + .dat
        os.path.join(carpeta, f"{entrada_usuario}.txt")       # En carpeta + .txt
    ]

    # 2. Buscar el primero que exista
    for ruta in candidatos:
        if os.path.exists(ruta) and os.path.isfile(ruta):
            archivo_encontrado = ruta
            break
    
    if archivo_encontrado:
        # Generar nombre de salida: cambia la extensión original (.dat/.txt) por .bin
        base, _ = os.path.splitext(archivo_encontrado)
        ruta_salida = f"{base}.bin"
        
        convertir_a_binario_pisa(archivo_encontrado, ruta_salida)

    else:
        print("\n❌ Error: No se encontró el archivo.")
        print("Buscamos en las siguientes rutas y no existían:")
        for c in candidatos:
            print(f" - {c}")