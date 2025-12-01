import random
import os

OUTPUT_DIR = "archivos_test"
FILENAME = "conjuntos.txt"  
NUM_SETS = 1000           
U = 100           # Universo(del 1 al 100)
MIN_ITEMS_PER_SET = 5       # Mínimo de elementos por fila
MAX_ITEMS_PER_SET = 50      # Máximo de elementos por fila

if not os.path.exists(OUTPUT_DIR):
    os.makedirs(OUTPUT_DIR)

filepath = os.path.join(OUTPUT_DIR, FILENAME)

print(f"Generando {NUM_SETS} conjuntos en '{filepath}'...")
print(f"Universo: 1..{U} | Densidad: {MIN_ITEMS_PER_SET}-{MAX_ITEMS_PER_SET} items/set")

with open(filepath, "w") as f:
    for i in range(NUM_SETS):

        # Tamaño aleatorio del conjunto
        k = random.randint(MIN_ITEMS_PER_SET, MAX_ITEMS_PER_SET)
        
        # Sample uniforme
        items = random.sample(range(1, U + 1), k)
        
        # Ordenar descendente 
        items.sort(reverse=True)
        
        line = ", ".join(map(str, items))
        f.write(line + "\n")
        
        if i % (NUM_SETS // 10) == 0:
            print(f"  Progreso: {i} conjuntos generados...")

print("¡Generación completada!")
print(f"Ahora ejecuta tu programa C++: ./test 0")