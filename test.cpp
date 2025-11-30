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

/*
class MemoryEval : public tdzdd::DdEval<MemoryEval, size_t> {
public:
    // Terminales: No ocupan memoria dinámica (son 1 o -1)
    void evalTerminal(size_t& v, int id) const {
        v = 0; 
    }

    // Nodos: Su tanaño es la suma de sus hijos + su propio peso
    void evalNode(size_t& v, int level, tdzdd::DdValues<size_t, 2> const& values) const {
        
        // Memoria de los hijos
        size_t memory_children = values.get(0) + values.get(1);

        // Calcular el tamaño del nodo en que estmaos
        // Un nodo ZDD típico tiene: Index (int) + 2 Punteros/IDs (size_t)

        size_t node_self_size = sizeof(int) + 2 * sizeof(size_t); 

        // Sumar todo y asignarlo 
        v = memory_children + node_self_size;
    }
};
*/


// Gestion eficiente de la RAM! Localidad, cache y asignacion id nodos con unicidad y hash


// Canonicidad -> Con NodeTableHandler (Tabla de hash), la cual asigna por Memory Pools (chunks de memoria en vez de cada vez pedir una memoria)
// No se ocupa punteros sino nodeId

template<int ARITY> // 2 para el ZDD, recordar estamos en BDD
class MemoryEval {
private:
    // Constantes de arquitectura (coonsiderando x64, punteros de 8 bytes)
    static constexpr size_t WORD_SIZE = sizeof(void*); 
    
    // TdZdd usa NodeId para referenciar hijos. Obtenemos su tamaño real de id
    static constexpr size_t NODE_ID_SIZE = sizeof(tdzdd::NodeId);
    
    // Obtiene el tamaño físico real que ocupa un nodo en RAM tras la alineación del compilador
    static size_t getAlignedNodeSize() {

        // Estructura interna estimada de un nodo TdZdd:
        // Header: RefCount + Level Index (aprox 4 bytes combinados en implementaciones optimizadas)
        // Children: Array de ARITY NodeIds

        size_t headerSize = sizeof(int16_t) * 2; // Estimación conservadora del header
        size_t branchesSize = ARITY * NODE_ID_SIZE;
        size_t rawSize = headerSize + branchesSize;
        
        // Cálculo de Padding (Alineación a 8 bytes en x64)
        size_t remainder = rawSize % WORD_SIZE;
        size_t padding = (remainder == 0) ? 0 : (WORD_SIZE - remainder);
        
        return rawSize + padding;
    }

public:
    /**
    countTotalBytes
    Devuelve una estimación de bytes totales.
     */
    static size_t countTotalBytes(const tdzdd::DdStructure<ARITY>& dd) {
        // Nodos Únicos Activos, total nodos
        size_t activeNodes = dd.size();
        if (activeNodes == 0) return 0;

        // Memoria Payload (Nodos físicos alineados)
        size_t nodeSizeBytes = getAlignedNodeSize();
        size_t payloadMemory = activeNodes * nodeSizeBytes;

        // Overhead del MemoryPool (Fragmentación Interna)
        // Los pools piden páginas grandes (4KB-2MB). Al final de los bloques 
        // siempre queda espacio sin usar. Estimación estándar: 5%.
        double poolFragmentationRate = 0.05;
        size_t poolOverhead = static_cast<size_t>(payloadMemory * poolFragmentationRate);

        // Overhead de la Tabla Hash (NodeTableHandler)
        // Para mantener O(1), la tabla hash NUNCA está al 100%. 
        // TdZdd suele redimensionar cuando el factor de carga > 0.6 o 0.7.
        // Factor para O(1) mantenerlo con buckets extras, payoff
        
        double loadFactor = 0.6; // 60% llena
        size_t hashCapacity = static_cast<size_t>(activeNodes / loadFactor);
        
        // Cada entrada en la hash table mapea un código hash a un NodeId/Índice por este, asegurando unicidad
        size_t hashEntrySize = sizeof(size_t); 
        size_t hashTableMemory = hashCapacity * hashEntrySize;

        return payloadMemory + poolOverhead + hashTableMemory;
    }


    static void printReport(const tdzdd::DdStructure<ARITY>& dd) {
        size_t total = countTotalBytes(dd);
        size_t nodes = dd.size();
        size_t alignedSize = getAlignedNodeSize();
        
        std::cout << "\n=== Auditoría de Memoria (MemoryEval) ===" << std::endl;
        std::cout << "Nodos Únicos (Lógicos):" << nodes << std::endl;
        std::cout << "Tamaño Físico por Nodo:" << alignedSize << " bytes (alineado)" << std::endl;
        std::cout << "-----------------------------------------" << std::endl;
        std::cout << "Memoria Total (RAM):   " << total << " bytes" << std::endl;
        std::cout << "                       " << std::fixed << std::setprecision(2) 
                  << (total / 1024.0 / 1024.0) << " MB" << std::endl;
        std::cout << "Eficiencia Real:       " << (nodes > 0 ? (double)total/nodes : 0) 
                  << " bytes/nodo" << std::endl;
        std::cout << "=========================================\n" << std::endl;
    }
};


int main(int argc, char* argv[]) {

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

        
        // Crear Molde para ese conjunto
        PathSpec spec(max_val, current_set);

        // Union al ZDD acumulado (Operacion Dinamica)
        dd = tdzdd::DdStructure<2>(tdzdd::zddUnion(dd, spec));

        // Reducir (Node Sharing y Node deletion rules)
        dd.zddReduce();

        // Prints Verticales
        std::cout << "----------------------------" << std::endl;
        std::cout << "PASO " << paso << ":" << std::endl;
        std::cout << "Conjunto: { ";
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
    }
    infile.close();

    std::cout << "Final" << std::endl;
    std::cout << "Total Conjuntos: " << paso << std::endl;
    //std::cout << "Cardinalidad Final: " << dd.zddCardinality() << std::endl;

    std::cout << "Conjuntos en la ZDD Final:" << std::endl;
    for (auto const& s : dd) {
        std::cout << "  { ";
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            std::cout << *it << " ";
        }
        std::cout << "}" << std::endl;
    }
    
    std::string final_dot = output_dir + "zdd_final.dot";
    std::ofstream out_final(final_dot);
    dd.dumpDot(out_final, "ZDD_Final");
    out_final.close();
    std::cout << "Grafo final guardado en: " << final_dot << std::endl;


    std::cout << "Nodos Lógicos: " << dd.size() << std::endl;

    
    //size_t raw_bytes = dd.evaluate(MemoryEval()); // .evaluate() realiza una evaluacion de abajo hacia arriba procesando un nodo solo una vez

    // 3. Estimación Realista (con tabla hash)
    // Las tablas hash suelen tener un factor de carga de 0.5 a 0.7 (doble de espacio)
    //size_t estimated_total_bytes = raw_bytes * 2;

    //std::cout << "Memoria : " << raw_bytes << " bytes" << std::endl;
    //std::cout << "Memoria Estimada: " << estimated_total_bytes << " bytes" << std::endl;

    MemoryEval<2>::printReport(dd); // En vez de reporte dinamico, calcular

    return 0;
}

// 0-edge trees y ---- es 0 edge y -> es el 1 edge, el nodo terminal es 1 

// zddReduce() applies the node sharing rule and the ZDD node deletion rule,
// which deletes a node when all of its non-zero-labeled outgoing edges point to ⊥.

// Diferencia con el paper es que el codigo actual aplica el node-sharing y el node deletion rules,
// pero faltan las flags y cambiarlo a que el nodo terminal sea 0 y no 1, asi se comprime mas (de 12 nodos del ejemplo a 10 del paper)
// Osea que falta la regla ZDD + 0-element edges, la ultima regla de compactación entonces