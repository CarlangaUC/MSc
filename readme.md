# 📘 Proyecto MAGISTER

Este proyecto utiliza **[TdZdd](https://github.com/kunisura/TdZdd)** como librería base para la construcción y manipulación eficiente de **Diagramas de Decisión Cero-Suprimidos (ZDDs)** en C++.

El código de ejemplo (`test.cpp`) demuestra el flujo de trabajo "Top-Down" de `TdZdd` para construir una familia compleja de conjuntos y asi realizar operaciones y testeos en diversas estructuras posiblemente.

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

```
g++ -std=c++11 -o test test.cpp -I ./TdZdd/include
./test
```

## Visualización

Para poder observar los grafos generados emplear la libreria Graphviz que lee el formato dot generado por el codigo (revisar parametros tal que)

```
dot -Tpng reduced.dot -o reduced.png
dot -Tpng unreduced.dot -o unreduced.png
```

