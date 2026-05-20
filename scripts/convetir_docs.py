import gzip
import struct
import sys

# Importación robusta (asumiendo que ya generaste el _pb2.py con exito antes)
try:
    from CommonIndexFileFormat_pb2 import Header, PostingsList
except ImportError:
    print("ERROR: No se encontró 'CommonIndexFileFormat_pb2.py'.")
    sys.exit(1)

# --- RUTAS ---
INPUT_FILE = "/root/MAGISTER/archivos_test/bp-msmarco-passage-esplade-quantized.ciff"
OUTPUT_FILE = "/root/MAGISTER/archivos_test/msmarco_esplade.docs"

def read_varint_manual(f):
    """Lee un Varint32 byte a byte."""
    shift = 0
    size = 0
    while True:
        b = f.read(1)
        if not b: raise EOFError
        byte = ord(b)
        size |= (byte & 0x7F) << shift
        if not (byte & 0x80): break
        shift += 7
    return size

def procesar():
    print(f"--- INICIO CONVERSIÓN CORRECTA ---")
    print(f"Entrada: {INPUT_FILE}")
    print(f"Salida:  {OUTPUT_FILE}")

    # 1. Abrir Archivo
    try:
        f_in = gzip.open(INPUT_FILE, 'rb')
        f_in.peek(1)
    except Exception:
        print("Aviso: Archivo no comprimido, abriendo modo normal.")
        f_in = open(INPUT_FILE, 'rb')

    f_out = open(OUTPUT_FILE, 'wb')

    # 2. Leer Header
    try:
        sz = read_varint_manual(f_in)
        msg = f_in.read(sz)
        header = Header()
        header.ParseFromString(msg)
        
        total_listas_esperadas = header.num_postings_lists
        print(f"Header OK.")
        print(f"Listas a procesar: {total_listas_esperadas}")
        print(f"Docs totales: {header.num_docs}")
        
    except Exception as e:
        print(f"Error crítico leyendo header: {e}")
        sys.exit(1)

    # 3. Escribir Header PISA (Cantidad de listas)
    # Escribimos el valor real que dice el header CIFF
    f_out.write(struct.pack('<I', total_listas_esperadas))

    count_guardados = 0
    
    # 4. Bucle EXACTO (Usando el número del header)
    print("Procesando...")
    
    # IMPORTANTE: Usamos un rango fijo para no leer la sección de DocRecords
    for i in range(total_listas_esperadas):
        try:
            # Leer tamaño y mensaje
            sz = read_varint_manual(f_in)
            msg_buf = f_in.read(sz)
            
            plist = PostingsList()
            plist.ParseFromString(msg_buf)
            
            # Deltas -> Absolutos
            ids_absolutos = []
            curr_id = 0
            for delta in plist.docids:
                curr_id += delta
                ids_absolutos.append(curr_id)
            
            # --- SIN FILTRO (Guardar todo lo que no esté vacío) ---
            if len(ids_absolutos) > 0:
                # Escribir tamaño
                f_out.write(struct.pack('<I', len(ids_absolutos)))
                # Escribir datos
                f_out.write(struct.pack(f'<{len(ids_absolutos)}I', *ids_absolutos))
                count_guardados += 1

            # === AGREGA ESTO AQUÍ ===
            #if count_guardados >= 20:  # <--- EL LÍMITE QUE QUIERAS
            #    print("\n¡Límite de prueba alcanzado! Parando...")
            #    break

            if (i + 1) % 5000 == 0:
                print(f"Progreso: {i + 1}/{total_listas_esperadas} listas procesadas...\r", end="")

        except Exception as e:
            print(f"\nError en lista #{i}: {e}")
            break

    # Finalizar
    # (Opcional) Si hubo listas vacías (len=0) que no guardamos, actualizamos el header
    # Pero PISA suele preferir que coincida.
    if count_guardados != total_listas_esperadas:
        print(f"\nAviso: Se guardaron {count_guardados} listas (algunas estaban vacías). Actualizando header...")
        f_out.seek(0)
        f_out.write(struct.pack('<I', count_guardados))
    
    f_in.close()
    f_out.close()

    print(f"\n\n--- FIN EXITOSO ---")
    print(f"Archivo generado: {OUTPUT_FILE}")
    print(f"Listas guardadas: {count_guardados}")

if __name__ == '__main__':
    procesar()