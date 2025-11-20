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


// Se calculara todo el tamñaño de la ZDD recorriendola, hay que considerar la tabla de hash que es implementada como como una matriz irregular (Jagged Array)

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


int main() {


    std::string input_path = "archivos_test/conjuntos.txt";
    std::string output_dir = "resultados_test/";

    std::ifstream infile(input_path);
    if (!infile.is_open()) {
        std::cerr << "No se pudo abrir " << input_path << std::endl;
        return 1;
    }

    // Parte vacia la DD
    tdzdd::DdStructure<2> dd;


    std::string linea;
    int paso = 0;

    std::cout << "Leyendo '" << input_path << "'..." << std::endl;

    while (std::getline(infile, linea)) {
        if (linea.empty()) continue;

        // Linea leida a set, asumido formato archivo
        std::stringstream ss(linea);
        std::string segment;
        std::set<int> current_set;
        int max_val = 0;

        while (std::getline(ss, segment, ',')) {
            try {
                int val = std::stoi(segment);
                current_set.insert(val);
                if (val > max_val) max_val = val;
            } catch (...) { continue; }
        }

        if (current_set.empty()) continue;
        paso++;

        // Molde para ese conjunto
        PathSpec spec(max_val, current_set);

        // Unión al ZDD acumulado (Operación Dinámica)
        dd = tdzdd::DdStructure<2>(tdzdd::zddUnion(dd, spec));

        // REDUCIR (Node Sharing + Zero Suppress)
        dd.zddReduce();

        // Debug
        // --- CAMBIO: Prints Verticales y Conjunto ---
        std::cout << "----------------------------" << std::endl;
        std::cout << "PASO " << paso << ":" << std::endl;
        std::cout << "Conjunto: { ";
        // Iterador inverso para mostrar 6, 5, 4...
        for (auto it = current_set.rbegin(); it != current_set.rend(); ++it) {
             std::cout << *it << " ";
        }
        std::cout << "}" << std::endl;
        std::cout << "Nodos:    " << dd.size() << std::endl;
        // --------------------------------------------

        // Guardar cada uno en cada paso para ver evolución
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
    std::cout << "Cardinalidad Final: " << dd.zddCardinality() << std::endl;

    // --- NUEVO: Print de todos los conjuntos en la ZDD final ---
    std::cout << "Conjuntos en la ZDD Final:" << std::endl;
    for (auto const& s : dd) {
        std::cout << "  { ";
        // Usamos rbegin/rend para mantener el orden descendente visual
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            std::cout << *it << " ";
        }
        std::cout << "}" << std::endl;
    }
    // -----------------------------------------------------------
    
    // ZDD Final
    std::string final_dot = output_dir + "zdd_final.dot";
    std::ofstream out_final(final_dot);
    dd.dumpDot(out_final, "ZDD_Final");
    out_final.close();
    std::cout << "Grafo final guardado en: " << final_dot << std::endl;

    return 0;
}

// 0-edge trees y ---- es 0 edge y -> es el 1 edge, el nodo terminal es 1 


// zddReduce() applies the node sharing rule and the ZDD node deletion rule,
// which deletes a node when all of its non-zero-labeled outgoing edges point to ⊥.

// Diferencia con el paper es que el codigo actual aplica el node-sharing y el node deletion rules,
// pero faltan las flags y cambiarlo a que el nodo terminal sea 0 y no 1, asi se comprime mas (de 12 nodos del ejemplo a 10 del paper)
// Osea que falta la regla ZDD + 0-element edges, la ultima regla de compactación entonces