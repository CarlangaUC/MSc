#include <iostream>
#include <fstream> // Para escribir archivos .dot
#include <string>
#include <set>    
#include <vector> 

// TDZDD

#include <tdzdd/DdSpec.hpp> // Para DD
#include <tdzdd/DdStructure.hpp> // Luego se reduce a BDD o ZDD, reduction rules y 0-edge
#include <tdzdd/DdSpecOp.hpp> // Para operaciones sobre la ZDD

// Molde para solo un ZDD
class PathSpec : public tdzdd::StatelessDdSpec<PathSpec, 2> {
    int const n_items;
    std::set<int> const path;

public:
    PathSpec(int n, std::set<int> const& p) : n_items(n), path(p) {}

    int getRoot() const {
        return n_items;
    }

    int getChild(int level, int value) const {


        bool item_is_needed = (path.count(level) > 0);

        if (value == 1) { // TOMAR
            if (!item_is_needed) return 0; // 0 es el terminal ⊥ (Falso)
        } else { // NO TOMAR
            if (item_is_needed) return 0; // 0 es el terminal ⊥ (Falso)
        }

        if (level == 1) {
            return -1; // -1 es el terminal ⊤ (True)
        }
        return level - 1; // Continuar al siguiente nivel 
    }
};



int main() {
    int const N = 12;
    int const N_ITEMS = 6; // Maximo es el 6 
    
    auto s1 = PathSpec(N_ITEMS, {6, 5, 4, 3});
    auto s2 = PathSpec(N_ITEMS, {6, 5, 4, 2});
    auto s3 = PathSpec(N_ITEMS, {6, 5, 4, 1});
    auto s4 = PathSpec(N_ITEMS, {6, 5, 4});
    auto s5 = PathSpec(N_ITEMS, {6, 5, 2});
    auto s6 = PathSpec(N_ITEMS, {6, 5, 1});
    auto s7 = PathSpec(N_ITEMS, {6, 5});
    auto s8 = PathSpec(N_ITEMS, {6, 4, 3, 2});
    auto s9 = PathSpec(N_ITEMS, {6, 4, 3, 1});
    auto s10 = PathSpec(N_ITEMS, {6, 4, 2, 1});
    auto s11 = PathSpec(N_ITEMS, {6, 2, 1});
    auto s12 = PathSpec(N_ITEMS, {3, 2, 1});
    std::cout << "   > "<< N <<" 'moldes' listos." << std::endl;

    // Unir todos los ZDD

    std::cout << "\n2. Combinando 'moldes' con 'zddUnion'..." << std::endl;
    auto combined_spec = tdzdd::zddUnion(s1, s2, s3, s4, s5, s6, 
                                       s7, s8, s9, s10, s11, s12);
    std::cout << "   > Especificación combinada creada." << std::endl;

    std::cout << "\n3. Construyendo Estructura DD (Top-Down)..." << std::endl;
    tdzdd::DdStructure<2> dd(combined_spec);
    std::cout << "   > Nodos ANTES de la reducción: " << dd.size() << std::endl;

    // Guardar el DD sin reducir

    std::cout << "\n4. Imprimiendo DD (sin reducir) a 'unreduced.dot'..." << std::endl;
    std::ofstream unreduced_file("unreduced.dot");
    dd.dumpDot(unreduced_file, "DD_Sin_Reducir");
    unreduced_file.close();

    std::cout << "   > 'unreduced.dot' guardado." << std::endl;
    
    // Reducirlo a ZDD

    std::cout << "\n5. Reduciendo a ZDD..." << std::endl;
    dd.zddReduce();
    std::cout << "   > Nodos DESPUÉS de la reducción: " << dd.size() << std::endl;

    // Guardarlo para visualización
    
    std::cout << "\n6. Imprimiendo ZDD (reducido) a 'reduced.dot'..." << std::endl;
    std::ofstream reduced_file("reduced.dot");

    dd.dumpDot(reduced_file, "ZDD_Reducido");
    reduced_file.close();
    std::cout << "   > 'reduced.dot' guardado." << std::endl;

    // Contar los conjuntos
    std::cout << "   > Cardinalidad (conteo): " << dd.zddCardinality() << std::endl;

    // Iterar sobre los conjuntos para ver correctamente almacenamiento
    std::cout << "   > Conjuntos encontrados:" << std::endl;
    for (auto const& s : dd) {
        std::cout << "     { ";
        for (int item : s) {
            std::cout << item << " ";
        }
        // Ojo aca que son los que ya estaban, combinaciones existentes
        std::cout << "}" << std::endl;
    }

    std::cout << "\n--- Flujo completo ---" << std::endl;
    return 0;
}

/*
Flujo para guardar cada ZDD a medida que va creciendo
int main() {
    int const N_ITEMS = 6; // Maximo es el 6 
    
    std::vector<std::set<int>> datasets = {
        {6, 5, 4, 3}, {6, 5, 4, 2}, {6, 5, 4, 1}, {6, 5, 4},
        {6, 5, 2}, {6, 5, 1}, {6, 5}, {6, 4, 3, 2},
        {6, 4, 3, 1}, {6, 4, 2, 1}, {6, 2, 1}, {3, 2, 1}
    };
    int const N = datasets.size();
    std::cout << " > " << N << " conjuntos definidos." << std::endl;

    std::vector<PathSpec> specs;
    for (const auto& p : datasets) {
        specs.push_back(PathSpec(N_ITEMS, p));
    }
    std::cout << " > " << specs.size() << " 'moldes' listos." << std::endl;


    std::cout << "\n2. Construyendo ZDD inicial con el primer conjunto..." << std::endl;
    tdzdd::DdStructure<2> dd_creciendo(specs[0]); 
    dd_creciendo.zddReduce(); 

    std::ofstream file_paso_1("zdd_creciendo_paso_1.dot");
    dd_creciendo.dumpDot(file_paso_1, "ZDD_Paso_1"); //
    file_paso_1.close();
    std::cout << "   > 'zdd_creciendo_paso_1.dot' guardado (Nodos: " << dd_creciendo.size() << ")" << std::endl;


    std::cout << "\n3. Uniendo los ZDD restantes paso a paso..." << std::endl;
    
    for (size_t i = 1; i < specs.size(); ++i) {
        std::cout << "   > Uniendo con el conjunto " << (i + 1) << "..." << std::endl;

  
        auto spec_siguiente_paso = tdzdd::zddUnion(dd_creciendo, specs[i]);

        dd_creciendo = tdzdd::DdStructure<2>(spec_siguiente_paso);

        dd_creciendo.zddReduce();

        std::stringstream ss_filename;
        ss_filename << "zdd_creciendo_paso_" << (i + 1) << ".dot";
        std::string filename = ss_filename.str();
        
        std::stringstream ss_title;
        ss_title << "ZDD_Paso_" << (i + 1);
        std::string title = ss_title.str();
        
        std::ofstream dot_file(filename);
        dd_creciendo.dumpDot(dot_file, title); //
        dot_file.close();

        std::cout << "     > '" << filename << "' guardado (Nodos: " << dd_creciendo.size() << ")" << std::endl;
    }


    std::cout << "\n4. Evaluación del ZDD final (Paso 12)..." << std::endl;
    std::cout << "   > Nodos finales: " << dd_creciendo.size() << std::endl;
    
    std::cout << "   > Cardinalidad (conteo): " << dd_creciendo.zddCardinality() << std::endl;

    std::cout << "   > Conjuntos encontrados:" << std::endl;
    for (auto const& s : dd_creciendo) {
        std::cout << "     { ";
        for (int item : s) {
            std::cout << item << " ";
        }
        std::cout << "}" << std::endl;
    }

    std::cout << "\n--- Flujo completo ---" << std::endl;
    return 0;
}
*/

// 0-edge trees y ---- es 0 edge y -> es el 1 edge, el nodo terminal es 1 


// zddReduce() applies the node sharing rule and the ZDD node deletion rule,
// which deletes a node when all of its non-zero-labeled outgoing edges point to ⊥.

// Diferencia con el paper es que el codigo actual aplica el node-sharing y el node deletion rules,
// pero faltan las flags y cambiarlo a que el nodo terminal sea 0 y no 1, asi se comprime mas (de 12 nodos del ejemplo a 10 del paper)
// Osea que falta la regla ZDD + 0-element edges, la ultima regla de compactación entonces