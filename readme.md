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
* **`FILENAME`**: Establece el nombre del archivo de salida (por defecto `conjuntos.txt`).
* **`NUM_SETS`**: Determina la cantidad total de conjuntos (líneas) que se generarán en el archivo.
* **`U`**: Representa el tamaño del Universo. Los números generados serán enteros aleatorios en el rango $[1, U]$.
* **`MIN_ITEMS_PER_SET`**: Fija la cantidad mínima de elementos que conformarán un conjunto.
* **`MAX_ITEMS_PER_SET`**: Fija la cantidad máxima de elementos que conformarán un conjunto.

Lo cual generara un archivo .txt con conjuntos, donde cada línea contiene una lista de enteros únicos (sampleo uniforme sin reemplazo), separados por comas y **ordenados de forma descendente**.

## Compilación y ejecución

Dado el codigo creado en test, se compila con:

```
g++ -std=c++11 -o test test.cpp -I ./TdZdd/include
```

Y se ejecuta considerando dos posibles modos:

- Modo .bin (Usado para benchmark) : 0
- Modo .txt (Usado para mayor reproducibilidad y creación de test): 1

Si es necesario correr el script **crear_formato_input.py**, el cual toma un archivo .txt en el formato acordado y lo vuelve .bin!

Por ende se ejecuta con
```
./test 0|1
```

## Visualización

Para poder observar los grafos generados emplear la libreria Graphviz que lee el formato dot generado por el codigo (revisar nombre archivos parametrizados en el codigo)

```
dot -Tpng reduced.dot -o reduced.png
dot -Tpng unreduced.dot -o unreduced.png
```


