# 📘 Proyecto MAGISTER

Este proyecto utiliza **[TdZdd](https://github.com/kunisura/TdZdd)** como librería base para la construcción y manipulación eficiente de **Diagramas de Decisión Cero-Suprimidos (ZDDs)** en C++ (Por ahora).

Actualmente el código de ejemplo (`test.cpp`) demuestra el flujo de trabajo "Top-Down" de `TdZdd` para construir una familia compleja de conjuntos y asi realizar operaciones y testeos en diversas estructuras posiblemente.

---

## 🧩 Dependencias

### 🔹 Repositorios empleados como librerías
- **TdZdd**: Framework en C++ para la construcción "Top-Down" de ZDDs. Se asume que está clonado en `./TdZdd/`.

### 🔹 Instalación de dependencias del sistema
Para poder visualizar los grafos `.dot` generados por el código, es necesario instalar **Graphviz**:

```
sudo apt-get update
sudo apt-get install graphviz
```

### Creación de test

Para simular conjuntos se puede ejecutar el script **generar_archivo.py** el cual tiene como parametros:

* **`OUTPUT_DIR`**: Define el nombre del directorio donde se almacenarán los resultados (el script lo crea automáticamente si no existe).
* **`FILENAME`**: Establece el nombre del archivo de salida.
* **`NUM_SETS`**: Determina la cantidad total de conjuntos (líneas) que se generarán en el archivo.
* **`U`**: Representa el tamaño del Universo. Los números generados serán enteros aleatorios en el rango $[1, U]$.
* **`MIN_ITEMS_PER_SET`**: Fija la cantidad mínima de elementos que conformarán un conjunto.
* **`MAX_ITEMS_PER_SET`**: Fija la cantidad máxima de elementos que conformarán un conjunto.

Lo cual generara un archivo .txt con conjuntos, donde cada línea contiene una lista de enteros únicos (sampleo uniforme sin reemplazo), separados por comas y **ordenados de forma descendente**, se recibira en terminal el **FILENAME** por lo cual hay que dar el nombre del archivo con el formato incluid (Ejemplo "conjuntos.txt").

## Compilación y ejecución

Dado el codigo creado en test, se compila con:

```
g++ -std=c++11 -o test test.cpp -I ./TdZdd/include 
```

o 

```
g++ -O3 -march=native -DNDEBUG -std=c++11 -o test test.cpp -I ./TdZdd/include -lpthread
```

Y se ejecuta considerando dos posibles modos:

- Modo .txt (Usado para mayor reproducibilidad y creación de test): 0
- Modo .bin (Usado para benchmark) : 1
- Modo .docs : 2

Si es necesario correr el script **crear_formato_input.py**, el cual toma un archivo .txt en el formato acordado y lo vuelve .bin, para esto especificar el nombre en terminal del archivo a buscar para transformarlo, se tiene que dar el nombre sin la extension (Ejemplo: "conjuntos") lo cual buscara "conjuntos.txt" en los parametros de ruta asociado y lo convertira a "conjuntos.bin" en formato adecuado.

Por ende se ejecuta con
```
./test 0|1|2
```

## Visualización

Para poder observar los grafos generados emplear la libreria Graphviz que lee el formato dot generado por el codigo (revisar nombre archivos parametrizados en el codigo), ir a la carpeta asociada y correr el comando dependiendo:

```
dot -Tpng reduced.dot -o reduced.png
dot -Tpng unreduced.dot -o unreduced.png
```
## Analisis tamaño

Se implemento un cuaderno en **grafico.ipynb** el cual analiza y genera diversos graficos comparando lo creado por **test.cpp** en torno al tamaño de la DD (revisar especificaciones) v/s reducirla a ZDD, revisar para comparación y verificación empirica de resultados y analisis de datos.
