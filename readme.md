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

## Compilación y ejecución

Dado el codigo creado en test, se compila con:

```
g++ -std=c++11 -o test test.cpp -I ./TdZdd/include
```

Y se ejecuta considerando dos posibles modos:

- Modo .bin (Usado para benchmark) : 0
- Modo .txt (Usado para mayor reproducibilidad y creación de test): 1

Si es necesario correr oel script **crear_formato_input.py**, el cual toma un archivo .txt en el formato acordado y lo vuelve .bin!

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


