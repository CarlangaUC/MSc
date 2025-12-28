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
    universo = 0
    total_ints = 0

    print("Leyendo y limpiando datos...")

    with open(input_path, 'r') as f:
        for i, linea in enumerate(f):
            linea = linea.strip()
            if not linea: continue
            
            try:

                elementos = [int(x) for x in linea.split()]
                
                if elementos:
                    # Estandarización para Índices Invertidos/ZDD
                    # Los números deben ser únicos en la lista y estar ordenados.
                    # set() quita duplicados, sorted() los ordena.
                    elementos_limpios = sorted(list(set(elementos)))
                    
                    conjuntos.append(elementos_limpios)
                    
                    # Estadísticas
                    max_val = elementos_limpios[-1] # El último es el mayor porque está ordenado
                    if max_val > universo:
                        universo = max_val
                    total_ints += len(elementos_limpios)

            except ValueError:
                print(f"Advertencia: Línea {i+1} ignorada (no numérica): {linea[:30]}...")
                continue

    if not conjuntos:
        print("Error: No se encontraron datos válidos.")
        return

    print(f" -> Listas procesadas: {len(conjuntos)}")
    print(f" -> Universo (Max ID): {universo}")
    print(f" -> Total de enteros:  {total_ints}")

    # Escritura Binaria (Formato DS2I .docs)
    print(f"Escribiendo binario...")
    os.makedirs(os.path.dirname(output_path) if os.path.dirname(output_path) else ".", exist_ok=True)
    
    # Header Estándar DS2I
    # El primer valor suele ser 1 (longitud de la secuencia singleton del universo)
    ds2i_singleton_len = 1 

    try:
        with open(output_path, "wb") as f:
            # HEADER (8 bytes): [1] [Universo]
            f.write(struct.pack('<II', ds2i_singleton_len, universo))

            # CUERPO: Secuencia de listas
            for s in conjuntos:
                n = len(s)
                # Escribir tamaño N
                f.write(struct.pack('<I', n))
                # Escribir N enteros
                # Usamos pack con '*' para desempaquetar la lista
                f.write(struct.pack(f'<{n}I', *s))

        print(f"¡Éxito! Archivo generado en: {output_path}")
        
    except Exception as e:
        print(f"Error escribiendo binario: {e}")

nombre = input("Ingrese el nombre del archivo (ej: '1mq' o '1mq (1)'): ").strip()

# Quitamos la extensión si el usuario la puso por error
if nombre.lower().endswith(".txt"):
    nombre = nombre[:-4]
    
# Asumimos que los archivos están en la carpeta 'archivos_test' o en la raíz
carpeta = "archivos_test" 

posible_ruta_1 = os.path.join(carpeta, f"{nombre}.txt")
posible_ruta_2 = f"{nombre}.txt"

if os.path.exists(posible_ruta_1):
    ruta_txt = posible_ruta_1
    ruta_bin = os.path.join(carpeta, f"{nombre}.bin") # Guardar binario ahí mismo
else:
    ruta_txt = posible_ruta_2
    ruta_bin = f"{nombre}.bin"

convertir_txt_a_bin(ruta_txt, ruta_bin)