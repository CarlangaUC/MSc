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

// Ingenieria inversa basandonos en seguir todo el flujo, entender clases y DdStructure como base y asociado
// Saber como acceder a atributos, algunos son privados
// Al llamar ciertos metodos, por ejemplo reduce(), se crea un DdReduce y este se elimina en la pila asociada al metodo cuando acaba, compilador se encarga de eso

// Heap (Memoria grande) y Stack (Memoria pequeña)

namespace ZddAudit {

    struct MemoryStats {
        size_t totalBytes;          // Suma Total Absoluta
        
        // Desglose
        size_t nodePayload;         // Heap: Nodos ZDD reales (Node<Arity>)
        size_t indexPayload;        // Heap: Índices de niveles (int) en higher/lower
        size_t vectorHeaders;       // Heap/Stack: Cabeceras de los MyVector (esqueleto)
        size_t classOverhead;       // Heap: Estructuras fijas (Handler::Object, Entity)
    };

    /**
     * @brief Auditoría Profunda basada en la jerarquía exacta de TdZdd.
     */
    template <int ARITY>
    MemoryStats auditarMemoria(const tdzdd::DdStructure<ARITY>& dd) {
        MemoryStats stats = {0, 0, 0, 0, 0};

        // Acceder a los atributos de la dd

        // DdStructure contiene un NodeTableHandler 'diagram'
        const auto& handler = dd.getDiagram(); // Nos da el puntero al diagrama
        const auto& entity = *handler; // Desreferenciar para obtener el objeto         
        
        int numRows = entity.numRows(); // Cantidad de niveles (Filas del diagrama)

        // Cálculo del Overhead Fijo de Clases (Estructura)
        
        // A. DdStructure (Stack): handler (ptr) + root (8B) + useMP (bool) + padding
        size_t sizeStructure = sizeof(dd);

        // B. NodeTableHandler::Object (Heap)

        // Planteamiento
        // Estructura interna: { unsigned refCount; NodeTableEntity entity; }
        // NodeTableEntity hereda de DataTable (1 MyVector) + tiene 2 MyVector (higher/lower).
        // Total vectores en Entity = 3.
        // Size MyVector = 24 bytes (capacity 8 + size 8 + ptr 8).
        // Size Entity = 3 * 24 = 72 bytes.
        // Size Object = 4 (ref) + 4 (padding) + 72 (Entity) = ~80 bytes.
        
        size_t sizeEntity = sizeof(tdzdd::NodeTableEntity<ARITY>); // ~72 bytes
        size_t sizeRef = sizeof(unsigned); // 4 bytes

        // Alineación a 8 bytes (común en x64), mas profundo

        size_t sizeObjectHeap = sizeRef + sizeEntity; 
        if (sizeObjectHeap % 8 != 0) sizeObjectHeap += (8 - (sizeObjectHeap % 8));

        stats.classOverhead = sizeStructure + sizeObjectHeap;

        
        // Cálculo de Cabeceras de Vectores (El Esqueleto)

        // NodeTableEntity tiene 3 vectores MAESTROS:
        // 1. table (DataTable) -> Vector de vectores de Nodos por heredencia
        // 2. higherLevelTable  -> Vector de vectores de Ints propio
        // 3. lowerLevelTable   -> Vector de vectores de Ints propio
        
        // Cada uno de estos vectores maestros tiene 'numRows' elementos.
        // Cada elemento es, a su vez, un objeto MyVector (una cabecera de 24 bytes).
        
        size_t sizeMyVectorHeader = sizeof(tdzdd::MyVector<int>); // 24 bytes
        
        // Total cabeceras = (Filas de Nodos) + (Filas de Higher) + (Filas de Lower)
        stats.vectorHeaders = (numRows * sizeMyVectorHeader) * 3;


        // Cálculo del Payload (Datos en Heap)
        
        // Nodos ZDD (NodeTableEntity -> table)
        // entity[i] retorna una referencia al MyVector<Node> de esa fila

        for (int i = 0; i < numRows; ++i) {
            const auto& rowVector = entity[i];
            // Multiplicamos capacidad real por 16 bytes (Node<2>)
            stats.nodePayload += rowVector.capacity() * sizeof(tdzdd::Node<ARITY>);
        }

        // BÍndices (higherLevelTable y lowerLevelTable)
        // Estos son MyVector<int>. Accedemos vía métodos públicos que retornan const&.
        // Nota: Si los índices no existen, TdZdd los crea (Lazy), pero para auditar la memoria
        // potencial total, es correcto medirlos.

        for (int i = 0; i < numRows; ++i) {
            const auto& highVec = entity.higherLevels(i); 
            stats.indexPayload += highVec.capacity() * sizeof(int);

            const auto& lowVec = entity.lowerLevels(i);
            stats.indexPayload += lowVec.capacity() * sizeof(int);
        }

        stats.totalBytes = stats.nodePayload + 
                           stats.indexPayload + 
                           stats.vectorHeaders + 
                           stats.classOverhead;

        return stats;
    }
}

// Wrapper para imprimir

void imprimirReporte(const tdzdd::DdStructure<2>& dd) {
    ZddAudit::MemoryStats s = ZddAudit::auditarMemoria(dd);
    
    double totalKB = s.totalBytes / 1024.0;
    double totalMB = totalKB / 1024.0;
    
    // Eficiencia: Cuántos bytes gasta el sistema para guardar 1 nodo lógico, repensar probablemente
    double bytesPerNode = (dd.size() > 0) ? (double)s.totalBytes / dd.size() : 0.0;

    std::cout << "\n=== Auditoría PROFUNDA de Memoria (Traversal) ===" << std::endl;
    std::cout << "Nodos Lógicos (dd.size): " << dd.size() << std::endl;
    std::cout << "Niveles Físicos (Rows):  " << dd.getDiagram()->numRows() << std::endl;
    std::cout << "-------------------------------------------------" << std::endl;
    std::cout << " [1] Nodos ZDD (Datos):  " << s.nodePayload << " B" << std::endl;
    std::cout << " [2] Índices (Ints):     " << s.indexPayload << " B (Higher/Lower Tables)" << std::endl;
    std::cout << " [3] Headers Vectores:   " << s.vectorHeaders << " B (Esqueleto MyVector)" << std::endl;
    std::cout << " [4] Overhead Clases:    " << s.classOverhead << " B" << std::endl;
    std::cout << "-------------------------------------------------" << std::endl;
    std::cout << " TOTAL FÍSICO:           " << s.totalBytes << " Bytes" << std::endl;
    std::cout << "                         " << std::fixed << std::setprecision(2) << totalKB << " KB" << std::endl;
    std::cout << "                         " << std::fixed << std::setprecision(2) << totalMB << " MB" << std::endl;
    std::cout << "-------------------------------------------------" << std::endl;
    std::cout << " Costo Real:             " << bytesPerNode << " bytes/nodo" << std::endl;
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