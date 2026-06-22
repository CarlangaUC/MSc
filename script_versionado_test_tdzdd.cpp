// Backbone 1 (TdZdd): ZDD^t = union {S_t^i} por termino; version fuera del ZDD.
// Compilar: g++ -O2 -std=c++17 -fopenmp -o script_versionado_test_tdzdd script_versionado_test_tdzdd.cpp -I ./TdZdd/include -lpthread

#include <tdzdd/DdSpec.hpp>
#include <tdzdd/DdSpecOp.hpp>
#include <tdzdd/DdStructure.hpp>

#include <tdzdd/util/ResourceUsage.hpp>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "uiHRDC/uiHRDC/indexes/NOPOS/II_docs/src/utils/version_packing.h"

#ifndef NZDD_ZDD_DOC_LEVEL_OFFSET
#define NZDD_ZDD_DOC_LEVEL_OFFSET 1
#endif

// PathSpec de un solo conjunto (niveles ya con offset master+1).
class FastPathSpec : public tdzdd::StatelessDdSpec<FastPathSpec, 2> {
    std::vector<int> sorted_items;

public:
    explicit FastPathSpec(std::vector<int> items) : sorted_items(std::move(items)) {
        std::sort(sorted_items.rbegin(), sorted_items.rend());
    }

    int getRoot() const {
        if (sorted_items.empty()) return -1;
        return sorted_items[0];
    }

    int getChild(int level, int value) const {
        if (value == 0) return 0;
        auto it = std::upper_bound(sorted_items.begin(), sorted_items.end(), level, std::greater<int>());
        if (it == sorted_items.end()) return -1;
        return *it;
    }
};

namespace ZddAudit {

struct MemoryStats {
    size_t totalBytes = 0;
    size_t nodePayload = 0;
    size_t indexPayload = 0;
    size_t vectorHeaders = 0;
    size_t classOverhead = 0;
};

template <int ARITY>
MemoryStats auditarMemoria(const tdzdd::DdStructure<ARITY>& dd) {
    MemoryStats stats;
    const auto& handler = dd.getDiagram();
    const auto& entity = *handler;
    int numRows = entity.numRows();

    size_t sizeStructure = sizeof(dd);
    size_t sizeEntity = sizeof(tdzdd::NodeTableEntity<ARITY>);
    size_t sizeRef = sizeof(unsigned);
    size_t sizeObjectHeap = sizeRef + sizeEntity;
    if (sizeObjectHeap % 8 != 0) sizeObjectHeap += (8 - (sizeObjectHeap % 8));
    stats.classOverhead = sizeStructure + sizeObjectHeap;

    size_t sizeMyVectorHeader = sizeof(tdzdd::MyVector<int>);
    stats.vectorHeaders = static_cast<size_t>(numRows) * sizeMyVectorHeader * 3;

    for (int i = 0; i < numRows; ++i) {
        stats.nodePayload += entity[i].capacity() * sizeof(tdzdd::Node<ARITY>);
    }
    for (int i = 0; i < numRows; ++i) {
        stats.indexPayload += entity.higherLevels(i).capacity() * sizeof(int);
        stats.indexPayload += entity.lowerLevels(i).capacity() * sizeof(int);
    }
    stats.totalBytes = stats.nodePayload + stats.indexPayload + stats.vectorHeaders + stats.classOverhead;
    return stats;
}

}  // namespace ZddAudit

void imprimirReporte(const tdzdd::DdStructure<2>& dd) {
    ZddAudit::MemoryStats s = ZddAudit::auditarMemoria(dd);
    double totalKB = s.totalBytes / 1024.0;
    double totalMB = totalKB / 1024.0;
    double bytesPerNode = (dd.size() > 0) ? static_cast<double>(s.totalBytes) / dd.size() : 0.0;

    std::cout << "\n=== Auditoria de memoria ===" << std::endl;
    std::cout << "Nodos logicos: " << dd.size() << std::endl;
    std::cout << "Niveles fisicos: " << dd.getDiagram()->numRows() << std::endl;
    std::cout << "Nodos ZDD: " << s.nodePayload << " B" << std::endl;
    std::cout << "Indices: " << s.indexPayload << " B" << std::endl;
    std::cout << "Headers: " << s.vectorHeaders << " B" << std::endl;
    std::cout << "Overhead: " << s.classOverhead << " B" << std::endl;
    std::cout << "Total: " << s.totalBytes << " B (" << std::fixed << std::setprecision(2)
              << totalKB << " KB, " << totalMB << " MB)" << std::endl;
    std::cout << "Bytes/nodo: " << bytesPerNode << std::endl;
}

// master usa el rango completo ZDD_MASTER_BITS (40); rel usa ZDD_REL_BITS (24).
static int masterToZddLevel(uint64_t master) {
    constexpr uint64_t kMaxMasterForInt =
        static_cast<uint64_t>(std::numeric_limits<int>::max()) - static_cast<uint64_t>(NZDD_ZDD_DOC_LEVEL_OFFSET);
    if (master > kMaxMasterForInt) {
        std::cerr << "[WARN] master " << master << " excede rango int para nivel ZDD; se trunca\n";
        master = kMaxMasterForInt;
    }
    return static_cast<int>(master) + NZDD_ZDD_DOC_LEVEL_OFFSET;
}



// Para la lectura del vocabulario
static uint32_t bitread32(const uint32_t* e, uint32_t p, uint32_t len) {
    e += p / 32u;
    p %= 32u;
    uint64_t answ = static_cast<uint64_t>(*e) >> p;
    if (p + len > 32u) answ |= static_cast<uint64_t>(*(e + 1)) << (32u - p);
    if (len < 32u) answ &= ((static_cast<uint64_t>(1) << len) - 1u);
    return static_cast<uint32_t>(answ);
}

// Estructura para el vocabulario
struct Vocabulary {
    bool loaded = false;
    uint32_t nwords = 0;
    std::vector<std::string> words;
    std::unordered_map<std::string, uint32_t> word2id;
};


// Función para cargar el vocabulario
static bool loadVocabulary(const std::string& path, Vocabulary& voc) {
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

// Función para leer el encabezado de los documentos
static bool readDocsHeader(std::ifstream& in, uint32_t& nlists) {
    in.read(reinterpret_cast<char*>(&nlists), sizeof(nlists));
    return in.good();
}

// Función para leer una posting list desde AQUI (sin offset) 
static bool readPostingList(std::ifstream& in, std::vector<uint64_t>& out) {
    out.clear();
    uint32_t len = 0;
    in.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (!in.good()) return false;
    if (len == 0) return true;
    out.resize(len);
    in.read(reinterpret_cast<char*>(out.data()), sizeof(uint64_t) * len);
    return in.good();
}


// Función para leer una posting list desde AQUI (con offset) para paralelizar en OpenMP
static bool readPostingListAt(const std::string& docsPath, uint64_t offset, std::vector<uint64_t>& out) {
    std::ifstream in(docsPath, std::ios::binary);
    if (!in.is_open()) return false;
    in.seekg(static_cast<std::streamoff>(offset));
    return readPostingList(in, out);
}

// Validar el split meta del archivo .docs
static bool validarSplitMeta(const std::string& docsPath) {
    std::ifstream in(docsPath + ".meta");
    if (!in.is_open()) {
        std::cerr << "[WARN] Sin .meta; asumiendo split compilado "
                  << ZDD_MASTER_BITS << "/" << ZDD_REL_BITS << std::endl;
        return true;
    }
    std::string line;
    uint32_t mb = 0, rb = 0;
    while (std::getline(in, line)) {
        if (line.rfind("master_bits=", 0) == 0) mb = static_cast<uint32_t>(std::stoul(line.substr(12)));
        if (line.rfind("rel_bits=", 0) == 0) rb = static_cast<uint32_t>(std::stoul(line.substr(9)));
    }
    if (mb != ZDD_MASTER_BITS || rb != ZDD_REL_BITS) {
        std::cerr << "[WARN] .meta split " << mb << "/" << rb
                  << " != compilado " << ZDD_MASTER_BITS << "/" << ZDD_REL_BITS << std::endl;
    }
    return true;
}

// Estructura para el indice del archivo para un acceso aleatorio (en OpenMP)
struct DocsIndex {
    uint32_t nlists = 0;
    std::vector<uint64_t> listOffsets;
    uint64_t totalPostings = 0;
};

// Una pasada ligera: offsets por lista (sin decodificar postings).
static DocsIndex buildDocsIndex(const std::string& docsPath) {
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

struct TermMetrics {
    uint32_t nVersions = 0;
    uint32_t nSnapshots = 0;
    uint64_t nodes = 0;
    size_t bytes = 0;
};


// ZDD union de snapshots parte por parte para asi acumular y no saturar
static tdzdd::DdStructure<2> zddUnionBalanced(std::vector<tdzdd::DdStructure<2>> parts) {
    if (parts.empty()) return tdzdd::DdStructure<2>();
    while (parts.size() > 1) {
        std::vector<tdzdd::DdStructure<2>> next;
        next.reserve((parts.size() + 1) / 2);
        for (size_t i = 0; i < parts.size(); i += 2) {
            if (i + 1 < parts.size()) {
                next.emplace_back(tdzdd::zddUnion(parts[i], parts[i + 1]));
            } else {
                next.push_back(std::move(parts[i]));
            }
        }
        parts = std::move(next);
    }
    parts[0].zddReduce();
    return std::move(parts[0]);
}

// Agrupa por rel, construye S_t^i, union balanceada -> ZDD^t.
static tdzdd::DdStructure<2> buildWordFamilyZdd(const std::vector<uint64_t>& postings,
                                                  TermMetrics& metrics) {
    std::unordered_map<uint64_t, std::vector<uint64_t>> snapsByRel;
    snapsByRel.reserve(postings.size() / 4 + 1);

    for (uint64_t x : postings) {
        const uint64_t master = ZDD_UNPACK_MASTER(x);
        const uint64_t rel = ZDD_UNPACK_REL(x);
        snapsByRel[rel].push_back(master);
    }

    metrics.nVersions = static_cast<uint32_t>(snapsByRel.size());

    std::vector<tdzdd::DdStructure<2>> partsForUnion;
    partsForUnion.reserve(snapsByRel.size());
    std::unordered_map<std::string, tdzdd::DdStructure<2>> builtByMasters;
    builtByMasters.reserve(snapsByRel.size());
    uint32_t nVersionsWithContent = 0;

    for (auto& kv : snapsByRel) {
        std::vector<uint64_t>& masters = kv.second;
        std::sort(masters.begin(), masters.end());
        masters.erase(std::unique(masters.begin(), masters.end()), masters.end());
        if (masters.empty()) continue;

        ++nVersionsWithContent;

        std::string key;
        key.reserve(masters.size() * sizeof(uint64_t));
        for (uint64_t m : masters) {
            key.append(reinterpret_cast<const char*>(&m), sizeof(m));
        }

        auto it = builtByMasters.find(key);
        if (it == builtByMasters.end()) {
            std::vector<int> levels;
            levels.reserve(masters.size());
            for (uint64_t m : masters) levels.push_back(masterToZddLevel(m));

            FastPathSpec spec(std::move(levels));
            tdzdd::DdStructure<2> snap(spec);
            snap.zddReduce();
            it = builtByMasters.emplace(std::move(key), std::move(snap)).first;
            partsForUnion.push_back(it->second);
        }
    }

    metrics.nSnapshots = nVersionsWithContent;

    if (partsForUnion.empty()) {
        metrics.nodes = 0;
        metrics.bytes = 0;
        return tdzdd::DdStructure<2>();
    }

    tdzdd::DdStructure<2> family = zddUnionBalanced(std::move(partsForUnion));
    metrics.nodes = family.size();
    metrics.bytes = ZddAudit::auditarMemoria(family).totalBytes;
    return family;
}

struct TermRow {
    TermMetrics metrics;
    bool built = false;
};

static void usage(const char* prog) {
    std::cerr << "Uso: " << prog
              << " <docs> [voc] [csv_out] [max_terms] [spot_term_id]\n"
              << "  docs: .docs packed64 (uint64 por posting)\n"
              << "  voc:  vocabulario uiHRDC (.voc), opcional\n"
              << "  csv_out: default resultados_test/script_versionado_metrics.csv\n"
              << "  max_terms: 0 = todas\n"
              << "  spot_term_id: imprime auditoria de ese term_id (-1 = omitir)\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const std::string docsPath = argv[1];
    const std::string vocPath = (argc >= 3) ? argv[2] : "";
    const std::string csvPath = (argc >= 4) ? argv[3] : "resultados_test/script_versionado_metrics.csv";
    const uint32_t maxTerms = (argc >= 5) ? static_cast<uint32_t>(std::stoul(argv[4])) : 0u;
    const int spotTermId = (argc >= 6) ? std::stoi(argv[5]) : -1;

    validarSplitMeta(docsPath);

    Vocabulary voc;
    if (!vocPath.empty()) {
        if (!loadVocabulary(vocPath, voc)) {
            std::cerr << "[WARN] No se pudo cargar vocabulario: " << vocPath << std::endl;
        } else {
            std::cout << "[INFO] Vocabulario: " << voc.nwords << " palabras" << std::endl;
        }
    }

    DocsIndex docsIdx = buildDocsIndex(docsPath);
    if (docsIdx.nlists == 0 || docsIdx.listOffsets.empty()) {
        std::cerr << "ERROR: indice invalido para " << docsPath << std::endl;
        return 1;
    }

    std::cout << "[INFO] Split empaquetado: " << ZDD_MASTER_BITS << "/" << ZDD_REL_BITS
              << " (master/rel en uint64, sin truncar a 32 bits)\n";
    std::cout << "[INFO] Listas: " << docsIdx.nlists
              << " postings=" << docsIdx.totalPostings << std::endl;

#ifdef _OPENMP
    std::cout << "[INFO] OpenMP hilos: " << omp_get_max_threads() << std::endl;
#else
    std::cout << "[WARN] Compilado sin OpenMP (-fopenmp); corrida secuencial" << std::endl;
#endif

    const uint32_t limit = (maxTerms > 0 && maxTerms < docsIdx.nlists) ? maxTerms : docsIdx.nlists;

    std::vector<TermRow> rows(limit);

    tdzdd::ResourceUsage usageStart;
    auto t0 = std::chrono::steady_clock::now();

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 16)
#endif
    for (int t = 0; t < static_cast<int>(limit); ++t) {
        std::vector<uint64_t> postings;
        if (!readPostingListAt(docsPath, docsIdx.listOffsets[static_cast<uint32_t>(t)], postings)) {
            continue;
        }

        TermMetrics m;
        tdzdd::DdStructure<2> family = buildWordFamilyZdd(postings, m);

        rows[static_cast<uint32_t>(t)].metrics = m;
        rows[static_cast<uint32_t>(t)].built = true;
    }

    auto t1 = std::chrono::steady_clock::now();
    tdzdd::ResourceUsage usageEnd;
    tdzdd::ResourceUsage usageDiff = usageEnd - usageStart;
    double secs = std::chrono::duration<double>(t1 - t0).count();

    std::ofstream csv(csvPath);
    if (!csv.is_open()) {
        std::cerr << "ERROR: no se pudo escribir " << csvPath << std::endl;
        return 1;
    }
    csv << "term_id,palabra,n_versiones,n_snapshots,nodos_zdd,bytes_zdd,kb_zdd\n";

    uint64_t sumNodes = 0;
    size_t sumBytes = 0;
    uint32_t termsBuilt = 0;

    for (uint32_t t = 0; t < limit; ++t) {
        if (!rows[t].built) {
            std::cerr << "ERROR: term " << t << " no procesado" << std::endl;
            return 1;
        }
        const TermMetrics& m = rows[t].metrics;
        std::string word = (voc.loaded && t < voc.nwords) ? voc.words[t] : "";
        double kb = m.bytes / 1024.0;
        csv << t << "," << word << "," << m.nVersions << "," << m.nSnapshots << ","
            << m.nodes << "," << m.bytes << "," << std::fixed << std::setprecision(2) << kb << "\n";
        sumNodes += m.nodes;
        sumBytes += m.bytes;
        ++termsBuilt;
    }
    csv.close();

    tdzdd::DdStructure<2> spotFamily;
    if (spotTermId >= 0 && static_cast<uint32_t>(spotTermId) < limit) {
        std::vector<uint64_t> postings;
        if (readPostingListAt(docsPath, docsIdx.listOffsets[static_cast<uint32_t>(spotTermId)], postings)) {
            TermMetrics spotMetrics;
            spotFamily = buildWordFamilyZdd(postings, spotMetrics);
        }
    }

    std::cout << "\n=== Resumen ===" << std::endl;
    std::cout << "Terminos procesados: " << termsBuilt << std::endl;
    std::cout << "Nodos ZDD (suma familias): " << sumNodes << std::endl;
    std::cout << "Bytes ZDD (suma auditoria): " << sumBytes << " ("
              << std::fixed << std::setprecision(2) << (sumBytes / 1024.0 / 1024.0) << " MB)" << std::endl;
    std::cout << "Tiempo build: " << secs << " s" << std::endl;
    std::cout << "RSS delta (TdZdd): " << usageDiff.maxrss << " KB ("
              << std::fixed << std::setprecision(2) << (usageDiff.maxrss / 1024.0) << " MB)"
              << std::endl;
    std::cout << "CSV: " << csvPath << std::endl;

    if (spotTermId >= 0) {
        std::cout << "\n=== Spot-check term_id=" << spotTermId << " ===" << std::endl;
        if (voc.loaded && static_cast<uint32_t>(spotTermId) < voc.nwords) {
            std::cout << "Palabra: " << voc.words[static_cast<uint32_t>(spotTermId)] << std::endl;
        }
        imprimirReporte(spotFamily);
    }

    return 0;
}
