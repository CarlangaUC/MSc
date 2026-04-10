import struct
import re
import os
from collections import defaultdict

def compilar_dataset(txt_path, boundaries_path, log_path, output_docs, output_mapping, output_txt):
    # ==========================================
    # 1. PARSEAR EL LOG PARA MAPEO DE PÁGINAS
    # ==========================================
    print(f"1. Analizando {log_path} para generar fronteras de páginas...")
    page_starts = [0]
    current_global_id = 0
    
    with open(log_path, 'r', encoding='utf-8', errors='ignore') as f_log:
        for line in f_log:
            # Captura ej: "parsed 24 versions, for document 0"
            match = re.search(r'parsed\s+(\d+)\s+versions,\s+for\s+document', line)
            if match:
                num_versiones = int(match.group(1))
                current_global_id += num_versiones
                page_starts.append(current_global_id)
                
    # Guardar mapeo en archivo binario (arreglo de uint32_t)
    with open(output_mapping, 'wb') as f_map:
        for p_start in page_starts:
            f_map.write(struct.pack('<I', p_start))
    print(f"   -> Mapeo guardado en {output_mapping}. Total páginas: {len(page_starts)-1}")

    # ==========================================
    # 2. LEER BOUNDARIES Y GENERAR ÍNDICE ZDD
    # ==========================================
    print(f"2. Leyendo {boundaries_path}...")
    with open(boundaries_path, 'rb') as f_bound:
        data = f_bound.read()
        num_enteros = len(data) // 4
        boundaries = struct.unpack(f'<{num_enteros}I', data)
    
    total_docs = len(boundaries) - 1 # Total de DocIDs globales
    inverted_index = defaultdict(list)
    vocab = {}
    next_term_id = 0

    print("3. Parseando texto y construyendo listas de posteo...")
    with open(txt_path, 'rb') as f_text:
        for doc_id in range(total_docs):
            start = boundaries[doc_id]
            end = boundaries[doc_id + 1]
            
            f_text.seek(start)
            raw_text = f_text.read(end - start)
            text = raw_text.decode('utf-8', errors='ignore').lower() # Lo pasa todo a minusculas IMPTE
            
            # Tokenización rápida
            words = set(re.findall(r'\b[a-z0-9]+\b', text))
            
            # CUIDADO: print(words) está comentado. 
            # Imprimir esto por cada versión en 2GB de texto colapsará tu terminal.
            # print(words) 

            for w in words:
                if w not in vocab:
                    vocab[w] = next_term_id
                    next_term_id += 1
                inverted_index[vocab[w]].append(doc_id)
                
            if (doc_id + 1) % 50000 == 0:
                print(f"   Procesados {doc_id + 1} / {total_docs} versiones...")

    # ==========================================
    # 4. EXPORTAR A BINARIO Y TEXTO PLANO
    # ==========================================
    print(f"4. Escribiendo archivo PISA en {output_docs} y archivo legible en {output_txt}...")
    
    with open(output_docs, 'wb') as f_out_bin, open(output_txt, 'w') as f_out_txt:
        
        # --- Cabecera PISA Binario ---
        f_out_bin.write(struct.pack('<I', len(vocab))) # Total de términos
        
        for t_id in range(len(vocab)):
            posting = inverted_index[t_id]
            
            # --- Escritura Binaria (.docs) ---
            f_out_bin.write(struct.pack('<I', len(posting))) # Largo de lista
            for d_id in posting:
                f_out_bin.write(struct.pack('<I', d_id)) # DocIDs globales
                
            # --- Escritura en Texto (.txt) ---
            # Formato: id1 id2 id3 ... (separados por espacio), una lista por línea.
            linea_txt = " ".join(str(d_id) for d_id in posting)
            f_out_txt.write(linea_txt + "\n")
                
    print("¡Proceso ETL completado exitosamente!")

# Ejecutar con los archivos que posees
compilar_dataset(
    txt_path = os.path.join("archivos_test","wiki_src2gb.txt"),
    boundaries_path= os.path.join("archivos_test",'wiki_src2gb.txt.DOCBOUNDARIES'),
    log_path= os.path.join("archivos_test",'wiki_src2gb.log'),
    output_docs= os.path.join("resultados_test",'wikipedia_zdd.docs'),
    output_mapping=os.path.join("resultados_test",'page_mapping.bin'),
    output_txt=os.path.join("resultados_test",'wikipedia_zdd.txt') # Nuevo archivo de salida TXT
)