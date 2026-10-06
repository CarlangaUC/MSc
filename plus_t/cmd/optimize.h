// =============================================================================
// cmd/optimize.h — reordenamiento ZDD (modo único plus_t)
// =============================================================================
// Heurísticas semánticamente seguras: permutar NIVELES CUDD (sift / shuffle).
// Semántica plus_t: varIndex es identidad (master = var - docOffset; tag fijo).
//
// HEURÍSTICAS PERMITIDAS:
//   baseline
//   Estáticos (solo masters): nodes_desc, nodes_asc, df_desc, df_asc
//   Dinámicos CUDD: sift, sift_conv, symm_sift, symm_sift_conv, random, random_pivot
//   Compuestos: unir con '+', p.ej. nodes_desc+sift_conv
//
// PROHIBIDAS — alteran var→master o no están en la API ZDD de CUDD:
//   linear, linear_conv, reverse, shuffle_rand, window*, annealing, genetic, exact
//
// Uso:
//   optimize u+t|log <in.zpack> <docs> <out.zpack|none|-> <heuristica> [max_sift_vars] [timeout_s]
//   optimize sweep u+t|log <in.zpack> <docs> [max_sift_vars] [timeout_s] [heur...]
// timeout_s=0 sin limite; p.ej. 1200 corta heuristica a los 20 min (fork + SIGKILL).
//
// Logs CSV (siempre, en resultados_test/):
//   optimize_runs.csv              — append de todas las corridas
//   optimize_sweep_<pack>_<ts>.csv — barrido (una fila por heurística)
//   optimize_<pack>_<heur>_<ts>.csv — corrida simple
// Si el esquema de columnas cambia, optimize_runs.csv se rota a .v<epoch> para
// no apilar filas nuevas bajo una cabecera vieja.
// bpi_edd usa sizeof(DdNode) (igual que measure_zpack_bpi).
// Las cotas bpi_zdd_std / bpi_level_grouped / bpi_dag_counting no requieren
// NZDD_BPI_AUDIT: sólo dependen de edd_nodes y numZddVars.
// =============================================================================

#ifndef ZDD_PLUS_T_CMD_OPTIMIZE_H
#define ZDD_PLUS_T_CMD_OPTIMIZE_H

#include "export/export.h"
#include "utils/bpi_scan.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <string>
#include <unordered_set>
#include <vector>

namespace ZddReorder {

struct Result {
    bool ok = false;
    bool semanticsOk = false;
    long eddBefore = 0;
    long eddAfter = 0;
    double deltaPct = 0.0;
    double bpiEddBefore = 0.0;
    double bpiEddAfter = 0.0;
    double seconds = 0.0;
    uint64_t totalInts = 0;
    uint32_t termsMeasured = 0;
    NzddBpi::Denominators denom;
    NzddBpi::Report bpiBefore;
    NzddBpi::Report bpiAfter;
    std::string heuristics;
    std::string rejectReason;
};

inline bool bpiAuditEnabled() {
    const char* v = std::getenv("NZDD_BPI_AUDIT");
    return v != nullptr && std::string(v) == "1";
}

inline NzddBpi::Denominators scanDocsForOptimize(const std::string& docsPath,
                                                 uint32_t maxTerms) {
    return NzddBpi::scanDocs(
        docsPath, maxTerms,
        bpiAuditEnabled() ? NzddBpi::DenomMode::FullAudit : NzddBpi::DenomMode::RawOnly);
}

// nonEmptyLevels se deja en 0 a proposito: aqui no hay ZddPackData a mano y la
// medicion dice que numZddVars ya es practicamente el conteo real (9788 de 9789
// en u+t, 28 de 29 en log), asi que el techo mueve las cotas <0.01%. El informe
// lo declara con levels_exact=0, de modo que la aproximacion queda a la vista.
inline NzddBpi::Numerators numeratorsFromEdd(DdManager* dd, long eddNodes) {
    NzddBpi::Numerators num{};
    num.eddNodes = static_cast<uint64_t>(eddNodes);
    num.numZddVars = static_cast<uint32_t>(Cudd_ReadZddSize(dd));
    return num;
}

inline const char* forbiddenTokenReason(const std::string& token) {
    if (token == "linear" || token == "linear_conv")
        return "LINEAR altera la codificacion var→master; invalida mastersOfZdd y tags";
    if (token == "reverse" || token == "shuffle_rand")
        return "heuristica retirada (no aporta compresion; use nodes_desc o sift)";
    if (token == "window2" || token == "window3" || token == "window4" ||
        token == "annealing" || token == "genetic" || token == "exact")
        return "heuristica no implementada en Cudd_zddReduceHeap para ZDD";
    return nullptr;
}

inline bool isAllowedToken(const std::string& token) {
    if (token == "baseline") return true;
    if (token == "nodes_desc" || token == "nodes_asc" || token == "df_desc" || token == "df_asc")
        return true;
    if (token == "sift" || token == "sift_conv" || token == "symm_sift" ||
        token == "symm_sift_conv" || token == "random" || token == "random_pivot")
        return true;
    return false;
}

inline bool validateHeuristic(const std::string& heur, std::string& rejectReason) {
    rejectReason.clear();
    if (heur.empty()) {
        rejectReason = "heuristica vacia";
        return false;
    }
    size_t start = 0;
    while (start <= heur.size()) {
        const size_t plus = heur.find('+', start);
        const std::string token =
            heur.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (token.empty()) {
            rejectReason = "token vacio en heuristica compuesta";
            return false;
        }
        if (const char* why = forbiddenTokenReason(token)) {
            rejectReason = std::string(why) + " [" + token + "]";
            return false;
        }
        if (!isAllowedToken(token)) {
            rejectReason = "heuristica desconocida o no permitida: " + token;
            return false;
        }
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return true;
}

inline void printAllowedHeuristics(std::ostream& out) {
    out << "Heuristicas permitidas:\n"
        << "  baseline\n"
        << "  nodes_desc | nodes_asc | df_desc | df_asc\n"
        << "  sift | sift_conv | symm_sift | symm_sift_conv | random | random_pivot\n"
        << "  compuestos: <estatico>+<dinamico>  (ej. nodes_desc+sift_conv)\n"
        << "Prohibidas: linear, linear_conv (rompen semantica var→master)\n";
}

inline std::vector<uint64_t> masterFrequencies(const std::string& docsPath, uint64_t nMasters) {
    std::vector<uint64_t> freq(nMasters, 0);
    std::ifstream docs(docsPath, std::ios::binary);
    if (!docs) return freq;
    uint32_t nlists = 0;
    docs.read(reinterpret_cast<char*>(&nlists), sizeof(nlists));
    std::vector<uint64_t> buf;
    for (uint32_t t = 0; t < nlists; ++t) {
        uint32_t len = 0;
        docs.read(reinterpret_cast<char*>(&len), sizeof(len));
        if (!docs) break;
        buf.resize(len);
        docs.read(reinterpret_cast<char*>(buf.data()),
                  static_cast<std::streamoff>(sizeof(uint64_t) * len));
        if (!docs) break;
        for (uint64_t p : buf) {
            const uint64_t m = p >> ZDD_MASTER_SHIFT;
            if (m < nMasters) ++freq[m];
        }
    }
    return freq;
}

inline std::vector<uint64_t> nodesPerVarFromForest(DdManager* dd,
                                                  const std::vector<DdNode*>& roots) {
    const int nv = Cudd_ReadZddSize(dd);
    std::vector<uint64_t> perVar(static_cast<size_t>(nv), 0);
    std::vector<DdNode*> stack;
    std::unordered_set<DdNode*> seen;
    for (DdNode* r : roots) {
        if (r != nullptr && r != Cudd_ReadZero(dd)) stack.push_back(r);
    }
    while (!stack.empty()) {
        DdNode* n = stack.back();
        stack.pop_back();
        if (n == nullptr || Cudd_IsConstant(n)) continue;
        if (!seen.insert(n).second) continue;
        const int idx = Cudd_NodeReadIndex(n);
        if (idx >= 0 && idx < nv) ++perVar[static_cast<size_t>(idx)];
        stack.push_back(Cudd_E(n));
        stack.push_back(Cudd_T(n));
    }
    return perVar;
}

inline std::vector<double> familyChecksums(DdManager* dd, const std::vector<DdNode*>& roots,
                                          size_t sample = 200) {
    std::vector<double> out;
    if (roots.empty()) return out;
    const size_t step = std::max<size_t>(1, roots.size() / sample);
    for (size_t i = 0; i < roots.size(); i += step) {
        out.push_back(roots[i] == nullptr ? -1.0 : Cudd_zddCountDouble(dd, roots[i]));
    }
    return out;
}

inline bool checksumsEqual(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return false;
    return true;
}

inline void configureSiftLimits(DdManager* dd, int maxSiftVars) {
    const int nv = Cudd_ReadZddSize(dd);
    Cudd_SetSiftMaxVar(dd, maxSiftVars > 0 ? maxSiftVars : nv);
    Cudd_SetSiftMaxSwap(dd, 2000000000);
    Cudd_SetMaxGrowth(dd, 1.2);
}

inline std::vector<int> masterPermutation(DdManager* dd, int docOffset,
                                          const std::vector<uint64_t>& key, bool desc) {
    const int nv = Cudd_ReadZddSize(dd);
    std::vector<int> masters;
    for (int idx = docOffset; idx < nv; ++idx) masters.push_back(idx);
    std::stable_sort(masters.begin(), masters.end(), [&](int a, int b) {
        const uint64_t ka =
            (static_cast<size_t>(a) < key.size()) ? key[static_cast<size_t>(a)] : 0;
        const uint64_t kb =
            (static_cast<size_t>(b) < key.size()) ? key[static_cast<size_t>(b)] : 0;
        if (ka != kb) return desc ? (ka > kb) : (ka < kb);
        return a < b;
    });
    std::vector<int> perm;
    perm.reserve(static_cast<size_t>(nv));
    for (int lvl = 0; lvl < docOffset; ++lvl) perm.push_back(Cudd_ReadInvPermZdd(dd, lvl));
    for (int idx : masters) perm.push_back(idx);
    return perm;
}

inline bool parseDynamic(const std::string& h, Cudd_ReorderingType& t) {
    if (h == "sift") { t = CUDD_REORDER_SIFT; return true; }
    if (h == "sift_conv") { t = CUDD_REORDER_SIFT_CONVERGE; return true; }
    if (h == "symm_sift") { t = CUDD_REORDER_SYMM_SIFT; return true; }
    if (h == "symm_sift_conv") { t = CUDD_REORDER_SYMM_SIFT_CONV; return true; }
    if (h == "random") { t = CUDD_REORDER_RANDOM; return true; }
    if (h == "random_pivot") { t = CUDD_REORDER_RANDOM_PIVOT; return true; }
    return false;
}

inline bool applyStaticShuffle(DdManager* dd, int docOffset, const std::string& heur,
                               const std::string& docsPath,
                               const std::vector<DdNode*>& roots) {
    const int nv = Cudd_ReadZddSize(dd);
    std::vector<int> perm;
    if (heur == "df_desc" || heur == "df_asc") {
        const uint64_t nMasters =
            (nv > docOffset) ? static_cast<uint64_t>(nv - docOffset) : 0u;
        const std::vector<uint64_t> freq = masterFrequencies(docsPath, nMasters);
        std::vector<uint64_t> key(static_cast<size_t>(nv), 0);
        for (uint64_t m = 0; m < nMasters; ++m) {
            key[static_cast<size_t>(docOffset) + m] = freq[m];
        }
        perm = masterPermutation(dd, docOffset, key, heur == "df_desc");
    } else if (heur == "nodes_desc" || heur == "nodes_asc") {
        perm = masterPermutation(dd, docOffset, nodesPerVarFromForest(dd, roots),
                                 heur == "nodes_desc");
    } else {
        return false;
    }
    return Cudd_zddShuffleHeap(dd, perm.data()) != 0;
}

inline bool applyDynamic(DdManager* dd, Cudd_ReorderingType t) {
    return Cudd_zddReduceHeap(dd, t, 0) != 0;
}

inline bool applyHeuristic(DdManager* dd, int docOffset, const std::string& docsPath,
                           const std::string& heur, int maxSiftVars,
                           const std::vector<DdNode*>& roots) {
    if (heur == "baseline") return true;

    size_t plus = heur.find('+');
    if (plus != std::string::npos) {
        const std::string a = heur.substr(0, plus);
        const std::string b = heur.substr(plus + 1);
        if (!applyHeuristic(dd, docOffset, docsPath, a, maxSiftVars, roots)) return false;
        return applyHeuristic(dd, docOffset, docsPath, b, maxSiftVars, roots);
    }

    if (heur == "df_desc" || heur == "df_asc" || heur == "nodes_desc" || heur == "nodes_asc") {
        return applyStaticShuffle(dd, docOffset, heur, docsPath, roots);
    }

    Cudd_ReorderingType t;
    if (parseDynamic(heur, t)) {
        configureSiftLimits(dd, maxSiftVars);
        return applyDynamic(dd, t);
    }
    return false;
}

inline Result optimizeForest(DdManager* dd, std::vector<DdNode*>& roots, int docOffset,
                             const std::string& docsPath, const std::string& heur,
                             int maxSiftVars) {
    Result r;
    r.heuristics = heur;
    r.termsMeasured = static_cast<uint32_t>(roots.size());
    r.denom = scanDocsForOptimize(docsPath, r.termsMeasured);
    r.totalInts = r.denom.nRaw;
    if (!validateHeuristic(heur, r.rejectReason)) {
        r.ok = false;
        r.semanticsOk = false;
        return r;
    }

    r.eddBefore = NzddCommon::cuddForestNodeCount(dd, roots);
    const std::vector<double> sumBefore = familyChecksums(dd, roots);

    const auto t0 = std::chrono::steady_clock::now();
    r.ok = applyHeuristic(dd, docOffset, docsPath, heur, maxSiftVars, roots);
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    r.eddAfter = NzddCommon::cuddForestNodeCount(dd, roots);
    const std::vector<double> sumAfter = familyChecksums(dd, roots);
    r.semanticsOk = checksumsEqual(sumBefore, sumAfter);

    if (r.eddBefore > 0) {
        r.deltaPct = 100.0 * (static_cast<double>(r.eddAfter) - r.eddBefore) /
                     static_cast<double>(r.eddBefore);
    }
    if (r.totalInts > 0) {
        r.bpiBefore = NzddBpi::compute(numeratorsFromEdd(dd, r.eddBefore), r.denom);
        r.bpiAfter = NzddBpi::compute(numeratorsFromEdd(dd, r.eddAfter), r.denom);
        r.bpiEddBefore = r.bpiBefore.bpiEdd;
        r.bpiEddAfter = r.bpiAfter.bpiEdd;
    }
    return r;
}

inline void printResult(const Result& r, const std::string& packPath, int numZddVars,
                        int docOffset) {
    std::cout << "pack=" << packPath << "\n";
    std::cout << "heuristica=" << r.heuristics << "\n";
    if (!r.rejectReason.empty()) std::cout << "reject_reason=" << r.rejectReason << "\n";
    std::cout << "reorder_ok=" << (r.ok ? 1 : 0) << "\n";
    std::cout << "numZddVars=" << numZddVars << " docOffset=" << docOffset
              << " master_vars=" << (numZddVars - docOffset) << "\n";
    std::cout << "edd_nodes_before=" << r.eddBefore << "\n";
    std::cout << "edd_nodes_after=" << r.eddAfter << "\n";
    std::cout << "delta_pct=" << r.deltaPct << "\n";
    std::cout << "bpi_edd_before=" << r.bpiEddBefore << "\n";
    std::cout << "bpi_edd_after=" << r.bpiEddAfter << "\n";
    std::cout << "seconds=" << r.seconds << "\n";
    std::cout << "semantics_preserved=" << (r.semanticsOk ? 1 : 0) << "\n";
}

inline std::vector<std::string> defaultSweepHeuristics() {
    return {
        "baseline",
        "nodes_desc", "df_desc", "nodes_asc", "df_asc",
        "nodes_desc+sift", "nodes_desc+sift_conv", "df_desc+sift_conv",
        "sift", "sift_conv", "symm_sift", "symm_sift_conv",
        "random", "random_pivot",
    };
}

}  // namespace ZddReorder

static constexpr const char* kOptimizeLogDir = "resultados_test";
static constexpr const char* kOptimizeMasterCsv = "resultados_test/optimize_runs.csv";

static void ensureLogDir() {
    mkdir(kOptimizeLogDir, 0755);
}

static std::string pathBasenameNoExt(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}

static std::string sanitizeForFilename(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') out.push_back(c);
        else if (c == '+') out.push_back('_');
        else out.push_back('_');
    }
    return out;
}

static std::string nowTimestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

static std::string csvEscape(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    return out + "\"";
}

static uint64_t fileSizeBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return 0;
    return static_cast<uint64_t>(in.tellg());
}

static double bpiFileFromBytes(uint64_t fileBytes, const ZddReorder::Result& rr) {
    NzddBpi::Numerators num{};
    num.fileBytes = fileBytes;
    return NzddBpi::compute(num, rr.denom).bpiFile;
}

static const char* optimizeCsvHeader() {
    return "timestamp,mode,enc,in_pack,docs_path,out_pack,heuristica,max_sift_vars,"
           "terms_measured,total_ints,numZddVars,docOffset,"
           "edd_before,edd_after,delta_pct,"
           "bpi_edd_before,bpi_edd_after,bpi_file_before,bpi_file_after,"
           "seconds,semantics_ok,reorder_ok,status,reject_reason,"
           "roundtrip_edd,roundtrip_pct,"
           "levels_used,levels_exact,bpi_zdd_std,bpi_level_grouped,bpi_dag_counting,"
           "ratio_raw_over_stored,bpi_edd_over_stored,bpi_zdd_std_over_stored\n";
}

static void writeOptimizeCsvRow(std::ostream& out, const std::string& ts, const std::string& mode,
                                TagEncoding enc, const std::string& inPack,
                                const std::string& docsPath, const std::string& outPack,
                                const std::string& heur, int maxSiftVars, int numZddVars,
                                int docOffset, const ZddReorder::Result& rr,
                                const std::string& status, uint64_t bpiFileBefore,
                                uint64_t bpiFileAfter, long roundtripEdd,
                                double roundtripPct) {
    out << ts << ',' << mode << ',' << tagEncodingName(enc) << ','
        << csvEscape(inPack) << ',' << csvEscape(docsPath) << ',' << csvEscape(outPack) << ','
        << csvEscape(heur) << ',' << maxSiftVars << ',' << rr.termsMeasured << ','
        << rr.totalInts << ',' << numZddVars << ',' << docOffset << ',' << rr.eddBefore << ','
        << rr.eddAfter << ',' << std::setprecision(6) << rr.deltaPct << ','
        << std::setprecision(8) << rr.bpiEddBefore << ',' << rr.bpiEddAfter << ','
        << bpiFileFromBytes(bpiFileBefore, rr) << ','
        << bpiFileFromBytes(bpiFileAfter, rr) << ',' << std::setprecision(3)
        << rr.seconds << ',' << (rr.semanticsOk ? 1 : 0) << ',' << (rr.ok ? 1 : 0) << ','
        << status << ',' << csvEscape(rr.rejectReason) << ',' << roundtripEdd << ','
        << std::setprecision(6) << roundtripPct;
    // Cotas de encoding: derivadas de edd_nodes/levels, no requieren FullAudit.
    out << ',' << rr.bpiAfter.levelsUsedForBounds << ','
        << (rr.bpiAfter.levelsAreExact ? 1 : 0) << ',' << std::setprecision(8)
        << rr.bpiAfter.bpiZddStd << ',' << rr.bpiAfter.bpiLevelGrouped << ','
        << rr.bpiAfter.bpiDagCounting;
    if (rr.denom.deepScanned) {
        out << ',' << rr.bpiAfter.ratioRawOverStored << ',' << rr.bpiAfter.bpiEddOverStored
            << ',' << rr.bpiAfter.bpiZddStdOverStored;
    } else {
        out << ",,,";
    }
    out << '\n';
}

// El master es un append de corridas históricas: si el esquema cambió, apilar
// filas nuevas bajo la cabecera vieja desalinea las columnas. Se rota el archivo
// y se empieza uno limpio en lugar de mezclar anchos.
static bool rotateMasterIfSchemaChanged() {
    std::ifstream in(kOptimizeMasterCsv);
    if (!in.good()) return false;
    std::string firstLine;
    if (!std::getline(in, firstLine)) return false;
    std::string expected = optimizeCsvHeader();
    if (!expected.empty() && expected.back() == '\n') expected.pop_back();
    if (!firstLine.empty() && firstLine.back() == '\r') firstLine.pop_back();
    if (firstLine == expected) return false;
    in.close();

    std::string backup = std::string(kOptimizeMasterCsv) + ".v" + std::to_string(::time(nullptr));
    if (std::rename(kOptimizeMasterCsv, backup.c_str()) != 0) {
        std::cerr << "[optimize] AVISO: esquema de " << kOptimizeMasterCsv
                  << " cambió y no se pudo rotar; las columnas quedarán desalineadas\n";
        return false;
    }
    std::cerr << "[optimize] esquema de CSV cambiado; master anterior movido a " << backup << '\n';
    return true;
}

static bool appendOptimizeMasterLog(const std::string& ts, const std::string& mode, TagEncoding enc,
                                    const std::string& inPack, const std::string& docsPath,
                                    const std::string& outPack, const std::string& heur,
                                    int maxSiftVars, int numZddVars, int docOffset,
                                    const ZddReorder::Result& rr, const std::string& status,
                                    uint64_t bpiFileBefore, uint64_t bpiFileAfter,
                                    long roundtripEdd, double roundtripPct) {
    ensureLogDir();
    rotateMasterIfSchemaChanged();
    const bool exists = std::ifstream(kOptimizeMasterCsv).good();
    std::ofstream master(kOptimizeMasterCsv, std::ios::app);
    if (!master) {
        std::cerr << "[optimize] ERROR: no se pudo abrir " << kOptimizeMasterCsv << '\n';
        return false;
    }
    if (!exists) master << optimizeCsvHeader();
    writeOptimizeCsvRow(master, ts, mode, enc, inPack, docsPath, outPack, heur, maxSiftVars,
                        numZddVars, docOffset, rr, status, bpiFileBefore, bpiFileAfter,
                        roundtripEdd, roundtripPct);
    return true;
}

static std::string writeOptimizeRunCsv(const std::string& runPath, const std::string& ts,
                                       const std::string& mode, TagEncoding enc,
                                       const std::string& inPack, const std::string& docsPath,
                                       const std::string& outPack, const std::string& heur,
                                       int maxSiftVars, int numZddVars, int docOffset,
                                       const ZddReorder::Result& rr, const std::string& status,
                                       uint64_t bpiFileBefore, uint64_t bpiFileAfter,
                                       long roundtripEdd, double roundtripPct,
                                       bool appendRow = false) {
    ensureLogDir();
    std::ofstream run;
    if (appendRow) {
        run.open(runPath, std::ios::app);
    } else {
        run.open(runPath, std::ios::trunc);
        if (run) run << optimizeCsvHeader();
    }
    if (!run) {
        std::cerr << "[optimize] ERROR: no se pudo abrir " << runPath << '\n';
        return "";
    }
    writeOptimizeCsvRow(run, ts, mode, enc, inPack, docsPath, outPack, heur, maxSiftVars,
                        numZddVars, docOffset, rr, status, bpiFileBefore, bpiFileAfter,
                        roundtripEdd, roundtripPct);
    return runPath;
}

static void logOptimizeResult(const std::string& runCsvPath, const std::string& ts,
                              const std::string& mode, TagEncoding enc, const std::string& inPack,
                              const std::string& docsPath, const std::string& outPack,
                              const std::string& heur, int maxSiftVars, int numZddVars,
                              int docOffset, const ZddReorder::Result& rr,
                              const std::string& status, uint64_t bpiFileBefore,
                              uint64_t bpiFileAfter, long roundtripEdd, double roundtripPct,
                              bool sweepAppend, bool verboseLog = true) {
    if (!runCsvPath.empty()) {
        writeOptimizeRunCsv(runCsvPath, ts, mode, enc, inPack, docsPath, outPack, heur,
                            maxSiftVars, numZddVars, docOffset, rr, status, bpiFileBefore,
                            bpiFileAfter, roundtripEdd, roundtripPct, sweepAppend);
    }
    appendOptimizeMasterLog(ts, mode, enc, inPack, docsPath, outPack, heur, maxSiftVars,
                            numZddVars, docOffset, rr, status, bpiFileBefore, bpiFileAfter,
                            roundtripEdd, roundtripPct);
    if (verboseLog) {
        std::cout << "[optimize] log_csv="
                  << (runCsvPath.empty() ? kOptimizeMasterCsv : runCsvPath)
                  << " master=" << kOptimizeMasterCsv << '\n';
    }
}

static bool isPureIntegerArg(const char* s) {
    if (s == nullptr || *s == '\0') return false;
    char* end = nullptr;
    std::strtol(s, &end, 10);
    return end != s && *end == '\0';
}

static std::string workerResultPath(pid_t parentPid) {
    return std::string("/tmp/opt_worker_") + std::to_string(parentPid) + ".txt";
}

static bool writeWorkerResult(const std::string& path, const ZddReorder::Result& rr,
                              int numZddVars, int docOffset, long roundtripEdd,
                              double roundtripPct) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "heuristica=" << rr.heuristics << '\n'
        << "ok=" << (rr.ok ? 1 : 0) << '\n'
        << "semantics_ok=" << (rr.semanticsOk ? 1 : 0) << '\n'
        << "reject_reason=" << rr.rejectReason << '\n'
        << "edd_before=" << rr.eddBefore << '\n'
        << "edd_after=" << rr.eddAfter << '\n'
        << "delta_pct=" << rr.deltaPct << '\n'
        << "bpi_edd_before=" << rr.bpiEddBefore << '\n'
        << "bpi_edd_after=" << rr.bpiEddAfter << '\n'
        << "seconds=" << rr.seconds << '\n'
        << "terms_measured=" << rr.termsMeasured << '\n'
        << "total_ints=" << rr.totalInts << '\n'
        << "numZddVars=" << numZddVars << '\n'
        << "docOffset=" << docOffset << '\n'
        << "roundtrip_edd=" << roundtripEdd << '\n'
        << "roundtrip_pct=" << roundtripPct << '\n';
    return true;
}

static bool readWorkerResult(const std::string& path, ZddReorder::Result& rr, int& numZddVars,
                             int& docOffset, long& roundtripEdd, double& roundtripPct) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line, key, val;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        key = line.substr(0, eq);
        val = line.substr(eq + 1);
        if (key == "heuristica") rr.heuristics = val;
        else if (key == "ok") rr.ok = (val == "1");
        else if (key == "semantics_ok") rr.semanticsOk = (val == "1");
        else if (key == "reject_reason") rr.rejectReason = val;
        else if (key == "edd_before") rr.eddBefore = std::atol(val.c_str());
        else if (key == "edd_after") rr.eddAfter = std::atol(val.c_str());
        else if (key == "delta_pct") rr.deltaPct = std::atof(val.c_str());
        else if (key == "bpi_edd_before") rr.bpiEddBefore = std::atof(val.c_str());
        else if (key == "bpi_edd_after") rr.bpiEddAfter = std::atof(val.c_str());
        else if (key == "seconds") rr.seconds = std::atof(val.c_str());
        else if (key == "terms_measured") rr.termsMeasured = static_cast<uint32_t>(std::stoul(val));
        else if (key == "total_ints") rr.totalInts = std::stoull(val);
        else if (key == "numZddVars") numZddVars = std::atoi(val.c_str());
        else if (key == "docOffset") docOffset = std::atoi(val.c_str());
        else if (key == "roundtrip_edd") roundtripEdd = std::atol(val.c_str());
        else if (key == "roundtrip_pct") roundtripPct = std::atof(val.c_str());
    }
    return true;
}

static int optimizeWorkerProcess(const std::string& inPack, const std::string& docsPath,
                                 const std::string& outPack, const std::string& heur,
                                 int maxSiftVars, const std::string& workerPath) {
    DdManager* dd = nullptr;
    std::vector<DdNode*> roots;
    int numZddVars = 0, docOffset = 0;
    if (!ZddPack::loadZddPack(dd, roots, numZddVars, docOffset, inPack)) _exit(1);

    ZddReorder::Result rr =
        ZddReorder::optimizeForest(dd, roots, docOffset, docsPath, heur, maxSiftVars);

    long roundtripEdd = 0;
    double roundtripPct = 0.0;
    if (rr.ok && rr.semanticsOk && rr.rejectReason.empty() && shouldSavePack(outPack)) {
        if (ZddPack::saveZddPack(dd, roots, numZddVars, docOffset, outPack)) {
            ZddPack::freeTermZdd(dd, roots);
            Cudd_Quit(dd);
            dd = nullptr;
            roots.clear();
            int nv2 = 0, off2 = 0;
            if (ZddPack::loadZddPack(dd, roots, nv2, off2, outPack)) {
                roundtripEdd = NzddCommon::cuddForestNodeCount(dd, roots);
                if (rr.eddAfter > 0) {
                    roundtripPct = 100.0 * static_cast<double>(roundtripEdd) /
                                   static_cast<double>(rr.eddAfter);
                }
            }
        }
    }

    if (!writeWorkerResult(workerPath, rr, numZddVars, docOffset, roundtripEdd, roundtripPct)) {
        ZddPack::freeTermZdd(dd, roots);
        Cudd_Quit(dd);
        _exit(1);
    }
    ZddPack::freeTermZdd(dd, roots);
    Cudd_Quit(dd);
    if (!rr.rejectReason.empty() || !rr.ok || !rr.semanticsOk) _exit(2);
    _exit(0);
}

struct OptimizeRunOutcome {
    ZddReorder::Result rr;
    int numZddVars = 0;
    int docOffset = 0;
    long roundtripEdd = 0;
    double roundtripPct = 0.0;
    std::string status = "ok";
    bool timedOut = false;
};

static OptimizeRunOutcome runOptimizeWithTimeout(const std::string& inPack,
                                                 const std::string& docsPath,
                                                 const std::string& outPack,
                                                 const std::string& heur, int maxSiftVars,
                                                 int timeoutS) {
    OptimizeRunOutcome out;
    out.rr.heuristics = heur;

    if (timeoutS <= 0) {
        DdManager* dd = nullptr;
        std::vector<DdNode*> roots;
        if (!ZddPack::loadZddPack(dd, roots, out.numZddVars, out.docOffset, inPack)) {
            out.status = "FALLA";
            out.rr.ok = false;
            return out;
        }
        out.rr = ZddReorder::optimizeForest(dd, roots, out.docOffset, docsPath, heur, maxSiftVars);
        if (!out.rr.rejectReason.empty()) out.status = "RECH";
        else if (!out.rr.ok || !out.rr.semanticsOk) out.status = "FALLA";
        if (out.rr.ok && out.rr.semanticsOk && out.rr.rejectReason.empty() &&
            shouldSavePack(outPack)) {
            if (ZddPack::saveZddPack(dd, roots, out.numZddVars, out.docOffset, outPack)) {
                ZddPack::freeTermZdd(dd, roots);
                Cudd_Quit(dd);
                dd = nullptr;
                roots.clear();
                int nv2 = 0, off2 = 0;
                if (ZddPack::loadZddPack(dd, roots, nv2, off2, outPack)) {
                    out.roundtripEdd = NzddCommon::cuddForestNodeCount(dd, roots);
                    if (out.rr.eddAfter > 0) {
                        out.roundtripPct = 100.0 * static_cast<double>(out.roundtripEdd) /
                                           static_cast<double>(out.rr.eddAfter);
                    }
                }
            }
        }
        ZddPack::freeTermZdd(dd, roots);
        Cudd_Quit(dd);
        return out;
    }

    const std::string workerPath = workerResultPath(getpid());
    std::remove(workerPath.c_str());
    const pid_t pid = fork();
    if (pid < 0) {
        out.status = "FALLA";
        out.rr.rejectReason = "fork failed";
        return out;
    }
    if (pid == 0) {
        optimizeWorkerProcess(inPack, docsPath, outPack, heur, maxSiftVars, workerPath);
    }

    int remaining = timeoutS;
    int childStatus = 0;
    bool finished = false;
    while (remaining > 0) {
        const pid_t w = waitpid(pid, &childStatus, WNOHANG);
        if (w == pid) {
            finished = true;
            break;
        }
        sleep(1);
        --remaining;
    }
    if (!finished) {
        kill(pid, SIGKILL);
        waitpid(pid, &childStatus, 0);
        out.timedOut = true;
        out.status = "TIMEOUT";
        out.rr.rejectReason = "timeout_s=" + std::to_string(timeoutS);
        out.rr.seconds = static_cast<double>(timeoutS);
        std::remove(workerPath.c_str());
        std::cerr << "[optimize] TIMEOUT heuristica=" << heur << " limit=" << timeoutS << "s\n";
        return out;
    }

    if (!readWorkerResult(workerPath, out.rr, out.numZddVars, out.docOffset, out.roundtripEdd,
                          out.roundtripPct)) {
        out.status = "FALLA";
        out.rr.rejectReason = "worker result missing";
    } else if (!out.rr.rejectReason.empty()) {
        out.status = "RECH";
    } else if (!out.rr.ok || !out.rr.semanticsOk) {
        out.status = "FALLA";
    }
    std::remove(workerPath.c_str());
    return out;
}

static void fillBaselineMetrics(const std::string& docsPath, const std::string& inPack,
                                ZddReorder::Result& rr) {
    DdManager* dd = nullptr;
    std::vector<DdNode*> roots;
    int numZddVars = 0, docOffset = 0;
    if (!ZddPack::loadZddPack(dd, roots, numZddVars, docOffset, inPack)) return;
    rr.termsMeasured = static_cast<uint32_t>(roots.size());
    rr.denom = ZddReorder::scanDocsForOptimize(docsPath, rr.termsMeasured);
    rr.totalInts = rr.denom.nRaw;
    rr.eddBefore = NzddCommon::cuddForestNodeCount(dd, roots);
    rr.eddAfter = rr.eddBefore;
    if (rr.totalInts > 0) {
        rr.bpiBefore = NzddBpi::compute(ZddReorder::numeratorsFromEdd(dd, rr.eddBefore), rr.denom);
        rr.bpiAfter = rr.bpiBefore;
        rr.bpiEddBefore = rr.bpiBefore.bpiEdd;
        rr.bpiEddAfter = rr.bpiEddBefore;
    }
    ZddPack::freeTermZdd(dd, roots);
    Cudd_Quit(dd);
}

static void printOptimizeUsage(const char* prog) {
    std::cerr
        << "\n  optimize u+t|log <in.zpack> <docs> <out.zpack|none|-> <heuristica> [max_sift_vars] [timeout_s]\n"
        << "  optimize sweep u+t|log <in.zpack> <docs> [max_sift_vars] [timeout_s] [heur...]\n"
        << "  timeout_s=0 sin limite; p.ej. 1200 corta a los 20 min\n";
    ZddReorder::printAllowedHeuristics(std::cerr);
}

static int cmdOptimizeSweep(int argc, char** argv) {
    if (argc < 6) {
        usage(argv[0]);
        printOptimizeUsage(argv[0]);
        return 1;
    }
    TagEncoding enc = TagEncoding::UPlusT;
    if (!parseTagEncoding(argv[3], enc)) return 1;
    (void)enc;

    const std::string inPack = argv[4];
    const std::string docsPath = argv[5];
    int maxSiftVars = 0;
    int timeoutS = 0;
    std::vector<std::string> heurs;
    int argi = 6;
    if (argc > argi && isPureIntegerArg(argv[argi])) maxSiftVars = std::atoi(argv[argi++]);
    if (argc > argi && isPureIntegerArg(argv[argi])) timeoutS = std::atoi(argv[argi++]);
    for (; argi < argc; ++argi) heurs.emplace_back(argv[argi]);
    if (heurs.empty()) heurs = ZddReorder::defaultSweepHeuristics();

    const std::string ts = nowTimestamp();
    const std::string packBase = pathBasenameNoExt(inPack);
    const std::string sweepCsv =
        std::string(kOptimizeLogDir) + "/optimize_sweep_" + packBase + '_' + ts + ".csv";
    const uint64_t bpiFileBefore = fileSizeBytes(inPack);

    std::cout << std::left << std::setw(22) << "HEURISTICA"
              << std::right << std::setw(12) << "ANTES"
              << std::setw(12) << "DESPUES"
              << std::setw(8) << "DELTA%"
              << std::setw(10) << "BPI_EDD"
              << std::setw(8) << "SEG"
              << std::setw(5) << "SEM"
              << "\n";
    std::cout << "[optimize] sweep_log=" << sweepCsv << " master=" << kOptimizeMasterCsv;
    if (timeoutS > 0) std::cout << " timeout_s=" << timeoutS;
    std::cout << '\n';

    int worstExit = 0;
    bool firstRow = true;
    for (const std::string& h : heurs) {
        OptimizeRunOutcome outcome =
            runOptimizeWithTimeout(inPack, docsPath, "none", h, maxSiftVars, timeoutS);
        if (outcome.timedOut) fillBaselineMetrics(docsPath, inPack, outcome.rr);

        const ZddReorder::Result& rr = outcome.rr;
        const std::string& status = outcome.status;
        if (status != "ok") worstExit = 2;

        std::cout << std::left << std::setw(22) << h
                  << std::right << std::setw(12) << rr.eddBefore
                  << std::setw(12) << rr.eddAfter
                  << std::setw(8) << std::fixed << std::setprecision(2) << rr.deltaPct
                  << std::setw(10) << std::setprecision(4) << rr.bpiEddAfter
                  << std::setw(8) << std::setprecision(2) << rr.seconds
                  << std::setw(5) << status
                  << "\n";

        logOptimizeResult(sweepCsv, ts, "sweep", enc, inPack, docsPath, "none", h, maxSiftVars,
                          outcome.numZddVars, outcome.docOffset, rr, status, bpiFileBefore, 0, 0,
                          0.0, !firstRow, firstRow);
        firstRow = false;
    }
    return worstExit;
}

static int cmdOptimize(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[2]) == "sweep") return cmdOptimizeSweep(argc, argv);

    if (argc < 7) {
        usage(argv[0]);
        printOptimizeUsage(argv[0]);
        return 1;
    }
    TagEncoding enc = TagEncoding::UPlusT;
    if (!parseTagEncoding(argv[2], enc)) return 1;
    (void)enc;

    const std::string inPack = argv[3];
    const std::string docsPath = argv[4];
    const std::string outPack = argv[5];
    const std::string heur = argv[6];
    const int maxSiftVars = (argc >= 8) ? std::atoi(argv[7]) : 0;
    const int timeoutS = (argc >= 9) ? std::atoi(argv[8]) : 0;
    const bool saveOut = shouldSavePack(outPack);
    const std::string ts = nowTimestamp();
    const std::string runCsv = std::string(kOptimizeLogDir) + "/optimize_" +
                               pathBasenameNoExt(inPack) + '_' + sanitizeForFilename(heur) + '_' +
                               ts + ".csv";
    const uint64_t bpiFileBefore = fileSizeBytes(inPack);

    if (timeoutS > 0) std::cout << "[optimize] timeout_s=" << timeoutS << '\n';

    auto t0 = std::chrono::steady_clock::now();
    OptimizeRunOutcome outcome =
        runOptimizeWithTimeout(inPack, docsPath, outPack, heur, maxSiftVars, timeoutS);
    if (outcome.timedOut) fillBaselineMetrics(docsPath, inPack, outcome.rr);

    const ZddReorder::Result& rr = outcome.rr;
    ZddReorder::printResult(rr, inPack, outcome.numZddVars, outcome.docOffset);

    auto logAndExit = [&](int code, const std::string& status, uint64_t bpiFileAfter,
                          long roundtripEdd, double roundtripPct) {
        logOptimizeResult(runCsv, ts, "single", enc, inPack, docsPath, outPack, heur, maxSiftVars,
                          outcome.numZddVars, outcome.docOffset, rr, status, bpiFileBefore,
                          bpiFileAfter, roundtripEdd, roundtripPct, false);
        return code;
    };

    if (outcome.status == "TIMEOUT") {
        return logAndExit(2, "TIMEOUT", 0, 0, 0.0);
    }
    if (!rr.ok || !rr.semanticsOk || !rr.rejectReason.empty()) {
        if (!rr.rejectReason.empty()) std::cerr << "[optimize] " << rr.rejectReason << '\n';
        const std::string st = !rr.rejectReason.empty() ? "RECH" : "FALLA";
        return logAndExit(2, st, 0, 0, 0.0);
    }

    if (!saveOut) {
        std::cout << "[optimize] serializacion omitida (out=" << outPack << ")\n";
        return logAndExit(0, "ok", 0, 0, 0.0);
    }

    if (timeoutS > 0) {
        std::cout << "roundtrip_edd_nodes=" << outcome.roundtripEdd << "\n";
        std::cout << "roundtrip_vs_optimized_pct=" << outcome.roundtripPct << "\n";
    } else {
        std::cout << "roundtrip_edd_nodes=" << outcome.roundtripEdd << "\n";
        std::cout << "roundtrip_vs_optimized_pct=" << outcome.roundtripPct << "\n";
    }

    const double totalS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "\n=== optimize OK ===\n";
    std::cout << "out=" << outPack << " total=" << totalS << "s\n";

    const uint64_t bpiFileAfter = shouldSavePack(outPack) ? fileSizeBytes(outPack) : 0;
    return logAndExit(0, "ok", bpiFileAfter, outcome.roundtripEdd, outcome.roundtripPct);
}

#endif  // ZDD_PLUS_T_CMD_OPTIMIZE_H
