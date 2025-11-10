EJECUTAR CON:
g++ -std=c++11 -o test test.cpp -I ./TdZdd/include
./test

En caso de querer observar la estructura generada en modo imagen plantear:
dot -Tpng unreduced.dot -o unreduced.png
dot -Tpng reduced.dot -o reduced.png

(Considerar rutas actuales y parametros en el codigo)