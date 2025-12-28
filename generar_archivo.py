import random
import os

OUTPUT_DIR = "archivos_test"
FILENAME = f"{input('Ingrese el nombre del archivo final (sin extensión): ')}.txt" 

NUM_SETS = 1000           
U = 500000                # Universo aumentado 
MIN_ITEMS_PER_SET = 2     # Algunos sets en 1mq eran pequeños
MAX_ITEMS_PER_SET = 20    # Ajustable

if not os.path.exists(OUTPUT_DIR):
    os.makedirs(OUTPUT_DIR)

filepath = os.path.join(OUTPUT_DIR, FILENAME)

print(f"Generando {NUM_SETS} conjuntos en '{filepath}'...")
print(f"Universo: 1..{U} | Formato: Separado por espacios (Estilo 1mq.txt)")

with open(filepath, "w") as f:
    for i in range(NUM_SETS):

        # Tamaño aleatorio del conjunto
        k = random.randint(MIN_ITEMS_PER_SET, MAX_ITEMS_PER_SET)
        
        # Sampleo uniforme (sin repetidos en la misma línea)
        items = random.sample(range(1, U + 1), k)
        
        # Nota: No se ordenan los numeros, en archivos tipo dataset no vienen de ese modo asi que para ser realistas
        line = " ".join(map(str, items))
        f.write(line + "\n")
        
        if i % (NUM_SETS // 10) == 0 and NUM_SETS >= 10:
            print(f"  Progreso: {i} conjuntos generados...")

print("¡Generación completada!")