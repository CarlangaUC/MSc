// =============================================================================
// nzdd_cudd_common.h — utilidades compartidas por los runners CUDD
// =============================================================================
// Usado por plus_t/ (bosque con tag u+t o log) y por el flujo histórico en
// scripts_deprecados/backbone1_cudd/zdd_cudd.cpp. Capa base compartida:
// que es IDENTICA en ambos flujos: lectura de vocabulario/.docs y un puñado
// de helpers CUDD genericos. La logica de construccion del bosque (con o
// sin tag) y los comandos CLI viven en cada .cpp, porque ahi si difieren.
// =============================================================================

#ifndef NZDD_CUDD_COMMON_H
#define NZDD_CUDD_COMMON_H

#include "cudd.h"
#include "utils/bpi.h"

#include <tdzdd/util/ResourceUsage.hpp>

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

// Nivel base reservado para el ZDD de documentos: var 0 nunca se usa como
// master (algunos flujos reservan var 0..k para tags). Configurable por -D.
#ifndef NZDD_ZDD_DOC_LEVEL_OFFSET
#define NZDD_ZDD_DOC_LEVEL_OFFSET 1
#endif

// Tamaño inicial de subtablas únicas en Cudd_Init (numSlots). CUDD redondea a
// potencia de 2 con piso 8. Default CUDD_UNIQUE_SLOTS=256 reserva ~2KB/nivel
// vacío; con 8 son 64B/nivel — crítico en modo u+t (O(V) niveles de tag).
#ifndef NZDD_INIT_UNIQUE_SLOTS
#define NZDD_INIT_UNIQUE_SLOTS 8
#endif

namespace NzddCommon {

// --- Vocabulario (.voc de uiHRDC: nwords/elemSize/zoneSize + offsets bitpacked) ---

struct Vocabulary {
    bool loaded = false;
    uint32_t nwords = 0;
    std::vector<std::string> words;
    std::unordered_map<std::string, uint32_t> word2id;
};

inline uint32_t bitread32(const uint32_t* e, uint32_t p, uint32_t len) {
    e += p / 32u;
    p %= 32u;
    uint64_t answ = static_cast<uint64_t>(*e) >> p;
    if (p + len > 32u) answ |= static_cast<uint64_t>(*(e + 1)) << (32u - p);
    if (len < 32u) answ &= ((static_cast<uint64_t>(1) << len) - 1u);
    return static_cast<uint32_t>(answ);
}

inline bool loadVocabulary(const std::string& path, Vocabulary& voc) {
    voc = Vocabulary{};
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    uint32_t nwords = 0, elemSize = 0, zoneSize = 0;
    in.read(reinterpret_cast<char*>(&nwords), sizeof(nwords));
    in.read(reinterpret_cast<char*>(&elemSize), sizeof(elemSize));
    in.read(reinterpret_cast<char*>(&zoneSize), sizeof(zoneSize));
    if (!in.good() || nwords == 0 || elemSize == 0 || elemSize > 32u) return false;
    std::vector<unsigned char> zone(zoneSize);
    if (zoneSize > 0) {
        in.read(reinterpret_cast<char*>(zone.data()), zoneSize);
        if (!in.good()) return false;
    }
    size_t nOffsets = static_cast<size_t>(nwords) + 1u;
    size_t nPacked = ((nOffsets * elemSize) + 32u - 1u) / 32u;
    std::vector<uint32_t> packed(nPacked + 1u, 0u);
    in.read(reinterpret_cast<char*>(packed.data()), nPacked * sizeof(uint32_t));
    voc.words.resize(nwords);
    for (uint32_t i = 0; i < nwords; ++i) {
        uint32_t off = bitread32(packed.data(), i * elemSize, elemSize);
        uint32_t nxt = bitread32(packed.data(), (i + 1) * elemSize, elemSize);
        if (nxt < off || nxt > zoneSize) return false;
        voc.words[i].assign(reinterpret_cast<const char*>(zone.data()) + off, nxt - off);
        voc.word2id.emplace(voc.words[i], i);
    }
    voc.nwords = nwords;
    voc.loaded = true;
    return true;
}

// --- .docs: header (nlists) + posting lists (uint64 packed master|rel) ---

inline bool readDocsHeader(std::ifstream& in, uint32_t& nlists) {
    in.read(reinterpret_cast<char*>(&nlists), sizeof(nlists));
    return in.good();
}

inline bool readPostingList(std::ifstream& in, std::vector<uint64_t>& out) {
    out.clear();
    uint32_t len = 0;
    in.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (!in.good()) return false;
    if (len == 0) return true;
    out.resize(len);
    in.read(reinterpret_cast<char*>(out.data()), sizeof(uint64_t) * len);
    return in.good();
}

inline bool readPostingListAt(const std::string& docsPath, uint64_t offset,
                              std::vector<uint64_t>& out) {
    std::ifstream in(docsPath, std::ios::binary);
    if (!in.is_open()) return false;
    in.seekg(static_cast<std::streamoff>(offset));
    return readPostingList(in, out);
}

struct DocsIndex {
    uint32_t nlists = 0;
    std::vector<uint64_t> listOffsets;
    uint64_t totalPostings = 0;
};

// Indice liviano: offset de cada posting list dentro del .docs (para leerlas
// on-demand y en paralelo sin cargar todo el archivo en memoria).
inline DocsIndex buildDocsIndex(const std::string& docsPath) {
    DocsIndex idx;
    std::ifstream in(docsPath, std::ios::binary);
    if (!in.is_open()) return idx;
    if (!readDocsHeader(in, idx.nlists)) return idx;
    idx.listOffsets.reserve(idx.nlists);
    for (uint32_t t = 0; t < idx.nlists; ++t) {
        idx.listOffsets.push_back(static_cast<uint64_t>(in.tellg()));
        uint32_t len = 0;
        in.read(reinterpret_cast<char*>(&len), sizeof(len));
        if (!in.good()) break;
        idx.totalPostings += len;
        in.seekg(static_cast<std::streamoff>(sizeof(uint64_t) * len), std::ios::cur);
        if (!in.good()) break;
    }
    return idx;
}

// --- Helpers CUDD/ZDD genericos ---

// Contrato: los nodos de `parts` siguen perteneciendo al llamador (vienen del
// cache de snapshots compartidos) y NO se liberan aqui; el resultado se devuelve
// siempre con una referencia propia que el llamador debe liberar.
//
// Los resultados intermedios del arbol de uniones se liberan en cuanto se
// consumen: si se dejan referenciados quedan vivos para siempre aunque no sean
// alcanzables desde ningun ZDD^t, inflando el pool del build.
inline DdNode* zddUnionBalancedCudd(DdManager* dd, std::vector<DdNode*> parts) {
    if (parts.empty()) {
        DdNode* z = Cudd_ReadZero(dd);
        Cudd_Ref(z);
        return z;
    }
    if (parts.size() == 1) {
        Cudd_Ref(parts[0]);
        return parts[0];
    }
    bool owned = false;  // true cuando `parts` ya son intermedios nuestros
    while (parts.size() > 1) {
        std::vector<DdNode*> next;
        next.reserve((parts.size() + 1) / 2);
        for (size_t i = 0; i < parts.size(); i += 2) {
            if (i + 1 < parts.size()) {
                DdNode* u = Cudd_zddUnion(dd, parts[i], parts[i + 1]);
                if (u != nullptr) Cudd_Ref(u);
                if (owned) {
                    Cudd_RecursiveDerefZdd(dd, parts[i]);
                    Cudd_RecursiveDerefZdd(dd, parts[i + 1]);
                }
                if (u == nullptr) {
                    for (DdNode* p : next) Cudd_RecursiveDerefZdd(dd, p);
                    if (owned) {
                        for (size_t j = i + 2; j < parts.size(); ++j)
                            Cudd_RecursiveDerefZdd(dd, parts[j]);
                    }
                    return nullptr;
                }
                next.push_back(u);
            } else {
                if (!owned) Cudd_Ref(parts[i]);  // el impar pasa a ser nuestro
                next.push_back(parts[i]);
            }
        }
        parts = std::move(next);
        owned = true;
    }
    return parts[0];
}

// Configuracion unica del manager: debe aplicarse IGUAL en build y en
// load/medicion, o Bytes_CUDD deja de ser comparable entre ambos caminos.
//
// Cuidado con el 0: en CUDD no significa "sin cache" ni "sin crecimiento".
// Cudd_SetMaxCacheHard(dd,0) y Cudd_SetLooseUpTo(dd,0) hacen que CUDD calcule
// el tope a partir de la RAM disponible (cuddAPI.c), es decir *amplian* el
// techo. Se mantiene asi a proposito (el build cae de ~50 s a ~8 s), pero
// implica que la cache crece libremente y su tamano NO es propiedad de la EDD:
// hay que descontarla al medir (ver scripts/measure_zpack_bpi.cpp).
// Bytes por nodo CUDD en este build (measure_zpack_bpi verifica sizeof(DdNode) == 32).
inline constexpr uint64_t NZDD_DDNODE_BYTES = NzddBpi::kDdNodeBytes;

inline void configureCuddManager(DdManager* dd) {
    Cudd_AutodynDisableZdd(dd);
    Cudd_SetMaxCacheHard(dd, 0);
    Cudd_SetLooseUpTo(dd, 0);
}

// Nodos que pertenecen al DAG y no al andamiaje del manager. Cudd_Init crea la
// cadena univ (1 nodo ZDD por variable), que queda contada en el pool aunque no
// forme parte de ningun ZDD^t: en modo u+t son V nodos, ~48% del pool a 100 MB.
//
// Es una medida del POOL: parte del total vivo y resta un proxy de univ. Vale
// mientras univ tenga exactamente 1 nodo por variable y no queden residuos de
// operaciones temporales, pero depende del estado del recolector. Para el numero
// que se reporta, preferir cuddForestNodeCount.
inline long cuddEddNodeCount(DdManager* dd) {
    const long pool = Cudd_zddReadNodeCount(dd);
    const long univ = static_cast<long>(Cudd_ReadZddSize(dd));
    return (pool > univ) ? (pool - univ) : pool;
}

// Nodos distintos alcanzables desde las raices del bosque: la definicion exacta
// del DAG compartido. No resta del pool, asi que no depende del recolector ni de
// residuos. Cudd_SharingSize marca y desmarca el bit de visita en node->next
// (idiom interno de CUDD, restaurado al salir), por lo que NO es reentrante:
// llamar solo con el manager en reposo y desde un unico hilo.
//
// Cuenta los terminales alcanzables (zero/one), que cuddEddNodeCount no incluye:
// espera una diferencia de 1-2 nodos entre ambos. Los ceros de roots se filtran
// porque Cudd_Regular(nullptr) no es valido.
inline long cuddForestNodeCount(DdManager* dd, const std::vector<DdNode*>& roots) {
    (void)dd;
    std::vector<DdNode*> live;
    live.reserve(roots.size());
    for (DdNode* r : roots)
        if (r != nullptr) live.push_back(r);
    if (live.empty()) return 0;
    return static_cast<long>(Cudd_SharingSize(live.data(), static_cast<int>(live.size())));
}

inline bool shouldSavePack(const std::string& path) {
    return path != "none" && path != "-";
}

// Fila del CSV de evolucion: Paso,Total_Ints,Nodos_Pool,Bytes_CUDD,RSS_KB,Tiempo_s,bpi_build
inline void writeEvolutionRow(std::ofstream& log, uint32_t paso, uint64_t totalInts, DdManager* dd,
                              double elapsedS) {
    tdzdd::ResourceUsage u;
    NzddBpi::Numerators num{};
    num.memBytes = Cudd_ReadMemoryInUse(dd);
    num.poolNodes = static_cast<uint64_t>(Cudd_zddReadNodeCount(dd));
    NzddBpi::Denominators denom{};
    denom.nRaw = totalInts;
    const NzddBpi::Report rep = NzddBpi::compute(num, denom);
    log << paso << "," << totalInts << "," << num.poolNodes << "," << num.memBytes << ","
        << u.maxrss << "," << std::fixed << std::setprecision(6) << elapsedS << ","
        << std::setprecision(8) << rep.bpiMem << "\n";
}

inline void printSet(const std::string& label, const std::vector<uint64_t>& v, size_t maxShow = 24) {
    std::cout << label << " (|set|=" << v.size() << "): { ";
    size_t show = std::min(maxShow, v.size());
    for (size_t i = 0; i < show; ++i) {
        std::cout << v[i];
        if (i + 1 < show) std::cout << ", ";
    }
    if (v.size() > show) std::cout << " ...";
    std::cout << " }\n";
}

// spot_word puede ser un term_id numerico o una palabra presente en el .voc.
inline int resolveSpot(const Vocabulary& voc, const std::string& spotWord, uint32_t limit) {
    if (spotWord.empty()) return -1;
    char* end = nullptr;
    long v = std::strtol(spotWord.c_str(), &end, 10);
    if (end != spotWord.c_str() && *end == '\0' && v >= 0 && static_cast<uint32_t>(v) < limit)
        return static_cast<int>(v);
    if (voc.loaded) {
        auto it = voc.word2id.find(spotWord);
        if (it != voc.word2id.end() && it->second < limit) return static_cast<int>(it->second);
    }
    return -1;
}

}  // namespace NzddCommon

#endif  // NZDD_CUDD_COMMON_H
