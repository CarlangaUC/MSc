#include <iostream>
#include <fstream> // Para escribir archivos .dot
#include <string>
#include <set>     
#include <vector> 

// TDZDD

#include <tdzdd/DdSpec.hpp> // Para DD
#include <tdzdd/DdStructure.hpp> // Luego se reduce a BDD o ZDD, reduction rules y 0-edge
#include <tdzdd/DdSpecOp.hpp> // Para operaciones sobre la ZDD
#include <tdzdd/DdEval.hpp> // Tabla de hash

// Lectura archivos
#include <sstream> // Para manejo strings archivo
#include <iomanip>   // Para formato de prints

// Para la lectura .bin formato benchmark
#include <cstdint> // Para uint32_t

// TDZDD Utilities para medir recursos y tiempo
#include <tdzdd/util/ResourceUsage.hpp>


// Molde para solo un ZDD, la libreria construye con reglas sabiendo el molde la ZDD
class PathSpec : public tdzdd::StatelessDdSpec<PathSpec, 2> { // Un DD con 2 aristas, osea un BDD, parametro 2
    int const n_items;
    std::set<int> const path;

public:
    PathSpec(int n, std::set<int> const& p) : n_items(n), path(p) {}

    int getRoot() const {
        return n_items;
    }

    int getChild(int level, int value) const {


        bool item_actual = (path.count(level) > 0);

        if (value == 1) { // TOMAR
            if (!item_actual) return 0; // 0 es el terminal ⊥ (Falso)
        } else { // NO TOMAR
            if (item_actual) return 0; // 0 es el terminal ⊥ (Falso)
        }

        if (level == 1) {
            return -1; // -1 es el terminal ⊤ (True)
        }
        return level - 1; // Continuar al siguiente nivel 
    }
};

// Recorrer la ZDD para evaluar los nodos y su cantidad total de bytes CONSIDERANDO la tabla de hash asociada y los nodos con su valor y punteros
// No se consideran los bytes del contenedor del ZDD y posiblemente de los nodos


// Se calculara todo el tamñaño de la ZDD recorriendola, hay que considerar la tabla de hash que es implementada como como una matriz irregular (Jagged Array)

// Gestion eficiente de la RAM! Localidad, cache y asignacion id nodos con unicidad y hash

// Canonicidad -> Con NodeTableHandler (Tabla de hash), la cual asigna por Memory Pools (chunks de memoria en vez de cada vez pedir una memoria)
// No se ocupa punteros sino nodeId


// DIAGRAMA MEMORIA

// Tamaño de cada nodo = 2*sizeof(NodeID) (contiene punteros a esos nodos id por eso)
// Hash table 
//      Ocupa un memory pool para pedir cierta cantidad de memoria (en vez de malloc/alloc), para conservar localidad de cache y multiplo tal que
//      Cada entrada de la tabla posee el state asociado a la DD y su nodeID + (posible padding)
//      Para mantener O(1) en la tabla de hash, esta se puede redimensionar en un factor de 0.75 como esta en el codigo de MyHashTable.hpp
//      


/*
// Ingenieria inversa basandonos en seguir todo el flujo, entender clases y DdStructure como base y asociado
namespace ZddAudit {

    struct MemoryStats {
        size_t totalBytes;          // Total Sumado
        size_t nodeVectorPayload;   // Bytes dentro de los arrays de nodos (capacity)
        size_t nodeVectorHeaders;   // Bytes de las cabeceras de los vectores
        size_t auxIndexBytes;       // Bytes de tablas auxiliares (si existen)
        size_t structureOverhead;   // Bytes de los objetos Handler/Entity
    };

    template <int ARITY>
    MemoryStats auditarMemoria(const tdzdd::DdStructure<ARITY>& dd) {
        MemoryStats stats = {0, 0, 0, 0};

        size_t activeNodes = dd.size();
        int numNiveles = dd.topLevel();

        const size_t SIZE_VECTOR_HEADER = 24; // Ocupa 3 punteros, c/u 8 bytes
        const size_t SIZE_NODE = ARITY * 8; //NodeId es uint64_t, por ende 8 bytes por la aridad

        stats.nodesBytes = (activeNodes + 2) * SIZE_NODE;


        size_t numFilas = numNiveles + 1;
        size_t masterVectors = 3 * SIZE_VECTOR_HEADER; 
        size_t rowVectors = 3 * numFilas * SIZE_VECTOR_HEADER;


        stats.vectorHeadersBytes = masterVectors + rowVectors;

        stats.indexingBytes = 0;
        stats.totalBytes = stats.nodesBytes 
                         + stats.vectorHeadersBytes 
                         + stats.indexingBytes
                         + sizeof(dd); // El objeto DdStructure (root + handler)

        return stats;

        /*
        // Alinear los bytes, extra o no

        size_t alignedNodeSize = (rawNodeSize + 7) & ~7;
        if (ARITY == 2) alignedNodeSize = 16; // Optimización binaria exacta

        stats.payloadBytes = activeNodes * alignedNodeSize;

        // NodeTableEntity
        stats.vectorHeadersBytes += SIZEOF_MYVECTOR; 

        // Vectores Fila (Uno por cada nivel, incluyendo el 0)
        stats.vectorHeadersBytes += (numNiveles + 1) * SIZEOF_MYVECTOR;
        stats.terminalsBytes = 2 * alignedNodeSize;

        // El lower y high table vectores mutable
        stats.indexingOverhead += 2 * SIZEOF_MYVECTOR; 
        stats.indexingOverhead += 2 * (numNiveles + 1) * SIZEOF_MYVECTOR;

        // Estimación conservadora del contenido de los índices (promedio 2 ints por nivel)
        stats.indexingOverhead += 2 * (numNiveles + 1) * 2 * sizeof(int);


        stats.totalBytes = stats.payloadBytes 
                         + stats.vectorHeadersBytes 
                         + stats.terminalsBytes 
                         + stats.indexingOverhead
                         + sizeof(dd); // El objeto DdStructure en sí (puntero + root)

        return stats;
    }
}

// Wrapper para imprimir

void imprimirAuditoria(const tdzdd::DdStructure<2>& dd) {

        ZddAudit::MemoryStats s = ZddAudit::auditarMemoria(dd);
        
        // Para KB y comparar con el peak de la libreria nativa
        double totalKB = s.totalBytes / 1024.0;
        double efficiency = (dd.size() > 0) ? (double)s.totalBytes / dd.size() : 0.0;

        std::cout << "\n=== [ZddAudit] Inspección Física de Memoria ===" << std::endl;
        std::cout << "Nodos Lógicos:          " << dd.size() << std::endl;
        std::cout << "Niveles (Variables):    " << dd.topLevel() << std::endl;
        std::cout << "------------------------------------------------" << std::endl;
        std::cout << "1. Payload (Nodos):     " << s.payloadBytes << " B" << std::endl;
        std::cout << "2. Vectores (Headers):  " << s.vectorHeadersBytes << " B" << std::endl;
        std::cout << "3. Terminales (Nvl 0):  " << s.terminalsBytes << " B" << std::endl;
        std::cout << "4. Indices (Lazy Est):  " << s.indexingOverhead << " B" << std::endl;
        std::cout << "------------------------------------------------" << std::endl;
        std::cout << "TOTAL REAL:             " << s.totalBytes << " Bytes" << std::endl;
        // IMPRESIÓN EN KB
        std::cout << "                        " << std::fixed << std::setprecision(3) << totalKB << " KB" << std::endl;
        std::cout << "Ratio:                  " << efficiency << " bytes/nodo" << std::endl;
        std::cout << "================================================" << std::endl;
        
}

void imprimirReporte(const tdzdd::DdStructure<2>& dd) {
    ZddAudit::MemoryStats s = ZddAudit::auditarMemoria(dd);
    
    double totalKB = s.totalBytes / 1024.0;
    
    std::cout << "\n=== Auditoría de Memoria Estructural (TdZdd) ===" << std::endl;
    std::cout << "Nodos (Lógicos):       " << dd.size() << std::endl;
    std::cout << "Niveles (Vars):        " << dd.topLevel() << std::endl;
    std::cout << "----------------------------------------------" << std::endl;
    
    std::cout << " [1] Nodos Físicos:    " << s.nodesBytes << " bytes" << std::endl;
    
    std::cout << " [2] Estructura Vector:" << s.vectorHeadersBytes << " bytes" << std::endl;
    
    std::cout << " [3] Índices (Lazy):   " << s.indexingBytes << " bytes" << std::endl;
    
    std::cout << "----------------------------------------------" << std::endl;
    
    std::cout << " TOTAL ESTIMADO:       " << s.totalBytes << " bytes (" 
              << std::fixed << std::setprecision(2) << totalKB << " KB)" << std::endl;
    
    double ratio = (dd.size() > 0) ? (double)s.totalBytes / dd.size() : 0.0;
    std::cout << " Bytes por Nodo:       " << ratio << std::endl;
    std::cout << "==============================================" << std::endl;
}*/

namespace ZddAudit {

    struct MemoryStats {
        size_t totalBytes;          // Total Sumado
        size_t nodeVectorPayload;   // Bytes dentro de los arrays de nodos (capacity)
        size_t nodeVectorHeaders;   // Bytes de las cabeceras de los vectores
        size_t auxIndexBytes;       // Bytes de tablas auxiliares (si existen)
        size_t structureOverhead;   // Bytes de los objetos Handler/Entity
    };

    /**
     * @brief Auditoría Profunda (Deep Traversal).
     * Recorre cada nivel (fila) de la tabla y pregunta su capacidad real.
     */
    template <int ARITY>
    MemoryStats auditarMemoria(const tdzdd::DdStructure<ARITY>& dd) {
        MemoryStats stats = {0, 0, 0, 0, 0};

        // 1. Acceder a las entrañas
        // getDiagram() devuelve el Handler. El operador -> devuelve la Entity (DataTable).
        const auto& handler = dd.getDiagram();
        const auto& entity = *handler; // Desreferenciar para obtener la tabla física

        // 2. Estructuras Base
        stats.structureOverhead += sizeof(dd);      // DdStructure (root + handler)
        stats.structureOverhead += sizeof(handler); // NodeTableHandler (puntero)
        stats.structureOverhead += sizeof(entity);  // NodeTableEntity (vectores maestros)

        // 3. Recorrer los Niveles (Vectores de Nodos)
        // entity.numRows() devuelve N+1 (Niveles 1..N + Terminales 0)
        int rows = entity.numRows();
        
        for (int i = 0; i < rows; ++i) {
            // Accedemos al MyVector<Node> del nivel 'i'
            const auto& rowVector = entity[i];

            // A. Payload Real (Capacity):
            // capacity() nos dice cuánta RAM pidió el vector, incluyendo lo que no usa.
            // sizeof(tdzdd::Node<ARITY>) es 16 bytes (en binario).
            size_t rowBytes = rowVector.capacity() * sizeof(tdzdd::Node<ARITY>);
            stats.nodeVectorPayload += rowBytes;

            // B. Cabecera del Vector:
            // Cada fila es un objeto MyVector independiente (24 bytes).
            stats.nodeVectorHeaders += sizeof(rowVector);
        }

        // 4. Recorrer Índices Auxiliares (Higher/Lower Levels)
        // Nota: TdZdd genera estos índices bajo demanda (Lazy).
        // Si los accedemos con 'higherLevels(i)', forzamos su creación si no existen.
        // Para no alterar el objeto (auditoría pasiva), solo medimos si 'capacity' > 0
        // accediendo a través de métodos const si fuera posible, pero TdZdd oculta
        // los vectores 'higherLevelTable' privados.
        //
        // Sin embargo, sabemos que están dentro de 'NodeTableEntity'.
        // Si no podemos iterarlos directamente sin ser 'friend', asumimos 0 
        // o el costo base de sus cabeceras (ya sumado en sizeof(entity)).
        // (En una implementación estricta sin modificar la librería, esta parte es opaca).
        stats.auxIndexBytes = 0; 

        // 5. Totalización
        stats.totalBytes = stats.nodeVectorPayload + 
                           stats.nodeVectorHeaders + 
                           stats.auxIndexBytes + 
                           stats.structureOverhead;

        return stats;
    }
}

// Wrapper para imprimir
void imprimirReporte(const tdzdd::DdStructure<2>& dd) {
    ZddAudit::MemoryStats s = ZddAudit::auditarMemoria(dd);
    
    double totalKB = s.totalBytes / 1024.0;
    
    // Calculamos eficiencia real (Bytes ocupados / Nodos útiles)
    double bytesPerNode = (dd.size() > 0) ? (double)s.totalBytes / dd.size() : 0.0;

    std::cout << "\n=== Auditoría PROFUNDA de Memoria (Traversal) ===" << std::endl;
    std::cout << "Nodos Lógicos (.size):   " << dd.size() << std::endl;
    std::cout << "Filas Físicas (Levels):  " << dd.getDiagram()->numRows() << std::endl;
    std::cout << "-------------------------------------------------" << std::endl;
    std::cout << " [1] Nodos (Capacity):   " << s.nodeVectorPayload << " B  (Memoria reservada)" << std::endl;
    std::cout << " [2] Headers Vectores:   " << s.nodeVectorHeaders << " B  (Estructura C++)" << std::endl;
    std::cout << " [3] Overhead Clase:     " << s.structureOverhead << " B" << std::endl;
    std::cout << "-------------------------------------------------" << std::endl;
    std::cout << " TOTAL FÍSICO:           " << s.totalBytes << " Bytes" << std::endl;
    std::cout << "                         " << std::fixed << std::setprecision(2) << totalKB << " KB" << std::endl;
    std::cout << " Eficiencia Real:        " << bytesPerNode << " bytes/nodo" << std::endl;
    std::cout << "=================================================" << std::endl;
}

int main(int argc, char* argv[]) {


    tdzdd::ResourceUsage usageStart;

    // Si es 0 lectura en .txt si es 1 en formato .bin benchmark
    if (argc < 2) {
        std::cerr << "Uso: ./test <modo>" << std::endl;
        std::cerr << "  0: Lectura Texto (.txt)" << std::endl;
        std::cerr << "  1: Lectura Binaria (.bin)" << std::endl;
        return 1;
    }

    int modo = std::atoi(argv[1]);
    std::string input_path;
    std::string output_dir = "resultados_test/";
    std::ifstream infile;

    // Debug de los modos
    if (modo == 0) {
        input_path = "archivos_test/conjuntos.txt";
        infile.open(input_path); // Modo texto por defecto
        std::cout << "MODO SELECCIONADO: 0 (Texto)" << std::endl;
    } else {
        input_path = "archivos_test/conjuntos.bin";
        infile.open(input_path, std::ios::binary); // Modo binario
        std::cout << "MODO SELECCIONADO: 1 (Binario)" << std::endl;
    }

    if (!infile.is_open()) {
        std::cerr << "ERROR: No se pudo abrir " << input_path << std::endl;
        return 1;
    }

    // Si es binario, usar cabecera formato 
    if (modo == 1) {
        uint32_t _dummy, universe;
        infile.read(reinterpret_cast<char*>(&_dummy), sizeof(_dummy));
        infile.read(reinterpret_cast<char*>(&universe), sizeof(universe));
        // No usamos _dummy ni universe en la lógica ZDD dinámica, solo avanzamos el puntero
    }

    std::cout << "Leyendo '" << input_path << "'..." << std::endl;

    // Parte vacia la DD
    tdzdd::DdStructure<2> dd;
    int paso = 0;

    tdzdd::ElapsedTimeCounter zddTimer;

    // Lectura
    while (true) {
        std::set<int> current_set;
        int max_val = 0;
        bool lectura_exitosa = false;

        if (modo == 0) { 
            // Modo .txt
            std::string linea;
            if (!std::getline(infile, linea)) break; 
            if (linea.empty()) continue;

            std::stringstream ss(linea);
            std::string segment;
            while (std::getline(ss, segment, ',')) {
                try {
                    int val = std::stoi(segment);
                    current_set.insert(val);
                    if (val > max_val) max_val = val;
                } catch (...) { continue; }
            }
            if (!current_set.empty()) lectura_exitosa = true;

        } else { 
            // Modo .bin
            uint32_t set_size;
            infile.read(reinterpret_cast<char*>(&set_size), sizeof(set_size));
            if (infile.eof()) break; 

            for (uint32_t i = 0; i < set_size; ++i) {
                uint32_t val;
                infile.read(reinterpret_cast<char*>(&val), sizeof(val));
                current_set.insert((int)val);
                if ((int)val > max_val) max_val = (int)val;
            }
            lectura_exitosa = true;
        }

        if (!lectura_exitosa) continue;
        paso++;

        zddTimer.start();
        
        // Crear Molde para ese conjunto
        PathSpec spec(max_val, current_set);

        // Union al ZDD acumulado (Operacion Dinamica)
        dd = tdzdd::DdStructure<2>(tdzdd::zddUnion(dd, spec));

        // Reducir (Node Sharing y Node deletion rules)
        dd.zddReduce();

        zddTimer.stop();

        // Prints Verticales
        std::cout << "----------------------------" << std::endl;
        std::cout << "PASO " << paso << ":" << std::endl;
        /*std::cout << "Conjunto: { ";
        for (auto it = current_set.rbegin(); it != current_set.rend(); ++it) {
             std::cout << *it << " ";
        }
        std::cout << "}" << std::endl;
        std::cout << "Nodos:    " << dd.size() << std::endl;
        
        // Guardar .dot para poder visualizar la zdd
        std::stringstream ss_fname;
        ss_fname << output_dir << "zdd_paso_" << paso << ".dot";
        std::ofstream out(ss_fname.str());
        if (out.good()) {
            std::stringstream title;
            title << "ZDD_Paso_" << paso;
            dd.dumpDot(out, title.str());
            out.close();
        }
        */
    }
    infile.close();

    // Calculamos el uso final usando operadores de TdZdd
    tdzdd::ResourceUsage usageEnd;
    tdzdd::ResourceUsage usageDiff = usageEnd - usageStart;

    std::cout << "\n=== REPORTE DE RENDIMIENTO (TdZdd Native) ===" << std::endl;

    std::cout << "Tiempo Real:     " << usageDiff.etime << " s" << std::endl;
    std::cout << "Tiempo CPU:      " << usageDiff.utime << " s" << std::endl;
    std::cout << "PICO MEMORIA:    " << usageDiff.maxrss << " KB" << std::endl;


    double zddSeconds = zddTimer; 
    std::cout << "Tiempo Algoritmo ZDD: " << zddSeconds << " s" << std::endl;
    std::cout << "---------------------------------------------" << std::endl;
    std::cout << "\n";

    std::cout << "Final" << std::endl;
    std::cout << "Total Conjuntos: " << paso << std::endl;
    //std::cout << "Cardinalidad Final: " << dd.zddCardinality() << std::endl;

    /*std::cout << "Conjuntos en la ZDD Final:" << std::endl;
    for (auto const& s : dd) {
        std::cout << "  { ";
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            std::cout << *it << " ";
        }
        std::cout << "}" << std::endl;
    }*/

    //imprimirAuditoria(dd);
    imprimirReporte(dd);

    std::string final_dot = output_dir + "zdd_final.dot";
    std::ofstream out_final(final_dot);
    dd.dumpDot(out_final, "ZDD_Final");
    out_final.close();
    std::cout << "Grafo final guardado en: " << final_dot << std::endl;


    std::cout << "Nodos Lógicos: " << dd.size() << std::endl;
    
    return 0;
}

// 0-edge trees y ---- es 0 edge y -> es el 1 edge, el nodo terminal es 1 

// zddReduce() applies the node sharing rule and the ZDD node deletion rule,
// which deletes a node when all of its non-zero-labeled outgoing edges point to ⊥.

// Diferencia con el paper es que el codigo actual aplica el node-sharing y el node deletion rules,
// pero faltan las flags y cambiarlo a que el nodo terminal sea 0 y no 1, asi se comprime mas (de 12 nodos del ejemplo a 10 del paper)
// Osea que falta la regla ZDD + 0-element edges, la ultima regla de compactación entonces