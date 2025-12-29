import struct
import os

def convertir_txt_a_bin(input_path, output_path):
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

# --- Bloque Main ---
if __name__ == "__main__":
    nombre = input("Ingrese el nombre del archivo (ej: '1mq'): ").strip()
    if nombre.lower().endswith(".txt"):
        nombre = nombre[:-4]
        
    carpeta = "archivos_test" 
    posible_ruta_1 = os.path.join(carpeta, f"{nombre}.txt")
    posible_ruta_2 = f"{nombre}.txt"

    if os.path.exists(posible_ruta_1):
        ruta_txt = posible_ruta_1
        ruta_bin = os.path.join(carpeta, f"{nombre}.bin") # O .docs
    else:
        ruta_txt = posible_ruta_2
        ruta_bin = f"{nombre}.bin"

    convertir_txt_a_bin(ruta_txt, ruta_bin)