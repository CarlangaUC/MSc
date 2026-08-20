// =============================================================================
// cmd/heuristics_check.h — verificación semántica + visual por heurística (toy)
// =============================================================================
// Uso: heuristics-check u+t|log [out_dir]
// Dataset toy (u=4, V=2), 14 heurísticas; enumera familias antes/después,
// escribe bosque.dot/.png por heurística y heuristics_check.csv.
// =============================================================================

#ifndef ZDD_PLUS_T_CMD_HEURISTICS_CHECK_H
#define ZDD_PLUS_T_CMD_HEURISTICS_CHECK_H

#include "cmd/optimize.h"
#include "demo/viz.h"

#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace HeuristicsCheck {

inline std::string sanitizeDirName(const std::string& heur) {
    std::string out;
    out.reserve(heur.size() + 4);
    for (char c : heur) {
        if (c == '+')
            out += "_plus_";
        else if (c == '/')
            out += '_';
        else
            out += c;
    }
    return out;
}

inline uint64_t packPosting(uint64_t master, uint64_t rel = 0) {
    return ZDD_PACK(master, rel);
}

inline bool writeToyDocs(const std::string& path) {
    // Coherente con demo: term0 masters {1,2,3}, term1 master {0}.
    const std::vector<uint64_t> term0 = {packPosting(1, 0), packPosting(2, 0), packPosting(1, 1),
                                         packPosting(2, 1), packPosting(3, 1)};
    const std::vector<uint64_t> term1 = {packPosting(0, 0)};
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const uint32_t nlists = 2;
    out.write(reinterpret_cast<const char*>(&nlists), sizeof(nlists));
    for (const auto& postings : {term0, term1}) {
        const uint32_t len = static_cast<uint32_t>(postings.size());
        out.write(reinterpret_cast<const char*>(&len), sizeof(len));
        out.write(reinterpret_cast<const char*>(postings.data()),
                  static_cast<std::streamoff>(sizeof(uint64_t) * len));
    }
    return static_cast<bool>(out);
}

inline std::string familySetKey(const std::vector<std::string>& elems) {
    std::vector<std::string> copy = elems;
    std::sort(copy.begin(), copy.end());
    std::string key;
    for (size_t i = 0; i < copy.size(); ++i) {
        if (i) key += ',';
        key += copy[i];
    }
    return key;
}

inline std::vector<std::string> familySignature(DdManager* dd, DdNode* root, int docOffset,
                                                uint64_t demoU, TagEncoding enc) {
    const auto sets = enumerateZddSets(dd, root, docOffset, demoU, enc);
    std::vector<std::string> sig;
    sig.reserve(sets.size());
    for (const auto& s : sets) sig.push_back(familySetKey(s));
    std::sort(sig.begin(), sig.end());
    return sig;
}

inline bool allFamiliesEqual(DdManager* dd, const std::vector<DdNode*>& roots, int docOffset,
                             uint64_t demoU, TagEncoding enc,
                             const std::vector<std::vector<std::string>>& before) {
    if (before.size() != roots.size()) return false;
    for (size_t i = 0; i < roots.size(); ++i) {
        const auto after = familySignature(dd, roots[i], docOffset, demoU, enc);
        if (after != before[i]) return false;
    }
    return true;
}

inline bool buildDemoForest(DdManager* dd, TagEncoding enc, int& docOffset, int& numZddVars,
                            std::vector<DdNode*>& pointerList,
                            std::vector<DdNode*>& sharedSnapshots) {
    constexpr uint32_t V = 2;
    std::vector<TermFtInputs> termData(V);
    termData[0] = makeDemoTerm({{1, 2}, {1, 2, 3}});
    termData[1] = makeDemoTerm({{0}});

    uint32_t tagWidth = 0;
    if (!computeZddLayout(3, V, enc, docOffset, numZddVars, tagWidth)) return false;

    std::unordered_map<std::string, DdNode*> snapshotCache;
    pointerList.clear();
    sharedSnapshots.clear();
    return buildFtPointersForRange(dd, termData, 0, V, docOffset, enc, snapshotCache,
                                     sharedSnapshots, pointerList);
}

inline void freeDemoForest(DdManager* dd, std::vector<DdNode*>& pointerList,
                           std::vector<DdNode*>& sharedSnapshots) {
    ZddPack::freeTermZdd(dd, pointerList);
    for (DdNode* s : sharedSnapshots) Cudd_RecursiveDerefZdd(dd, s);
    pointerList.clear();
    sharedSnapshots.clear();
}

inline bool writeHeurForestDotPng(const std::string& heurDir, DdManager* dd,
                                  const std::vector<DdNode*>& pointerList, int docOffset,
                                  uint64_t demoU, TagEncoding enc,
                                  const std::vector<std::string>& varLabels) {
    mkdir(heurDir.c_str(), 0755);
    const std::string dotPath = heurDir + "/bosque.dot";
    const std::string pngPath = heurDir + "/bosque.png";
    const std::vector<std::string> titles = {"ZDD^1", "ZDD^2"};
    if (!writePrettyBosqueDot(dotPath, dd, pointerList, titles, docOffset, demoU, enc,
                              varLabels))
        return false;
    return dotToPng(dotPath, pngPath);
}

}  // namespace HeuristicsCheck

static int cmdHeuristicsCheck(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        std::cerr << "  heuristics-check u+t|log [out_dir]\n";
        return 1;
    }
    TagEncoding enc = TagEncoding::UPlusT;
    if (!parseTagEncoding(argv[2], enc)) return 1;
    const std::string outDir =
        (argc >= 4) ? argv[3] : "resultados_test/heuristics_check";
    mkdir(outDir.c_str(), 0755);

    constexpr uint64_t DEMO_U = 4;
    constexpr int MAX_SIFT = 200;
    const std::string docsPath = outDir + "/toy_heuristics.docs";
    if (!HeuristicsCheck::writeToyDocs(docsPath)) {
        std::cerr << "[heuristics-check] no pude escribir " << docsPath << "\n";
        return 1;
    }

    const auto heurs = ZddReorder::defaultSweepHeuristics();
    const std::string csvPath = outDir + "/heuristics_check.csv";
    std::ofstream csv(csvPath);
    if (!csv) {
        std::cerr << "[heuristics-check] no pude abrir " << csvPath << "\n";
        return 1;
    }
    csv << "heuristica,semantics_ok,families_equal,edd_before,edd_after,delta_pct,seconds,"
           "reorder_ok,status\n";

    std::cout << "=== heuristics-check (toy u=4, V=2, enc=" << tagEncodingName(enc)
              << ") ===\n";
    std::cout << "out_dir=" << outDir << "\n";
    std::cout << "docs=" << docsPath << "\n\n";

    int docOffset = 0;
    int numZddVars = 0;
    uint32_t tagWidth = 0;
    if (!computeZddLayout(3, 2, enc, docOffset, numZddVars, tagWidth)) {
        std::cerr << "[heuristics-check] computeZddLayout fallo\n";
        return 1;
    }

    int failures = 0;
    for (const std::string& heur : heurs) {
        std::vector<DdNode*> pointerList;
        std::vector<DdNode*> sharedSnapshots;

        DdManager* dd = Cudd_Init(0, numZddVars, NZDD_INIT_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
        if (dd == nullptr) {
            std::cerr << "Cudd_Init fallo para " << heur << "\n";
            ++failures;
            continue;
        }
        configureCuddManager(dd);

        if (!HeuristicsCheck::buildDemoForest(dd, enc, docOffset, numZddVars, pointerList,
                                              sharedSnapshots)) {
            std::cerr << "buildDemoForest fallo para " << heur << "\n";
            Cudd_Quit(dd);
            ++failures;
            continue;
        }

        const auto varLabels =
            buildDemoVarLabels(numZddVars, docOffset, DEMO_U, enc);

        std::vector<std::vector<std::string>> sigBefore;
        sigBefore.reserve(pointerList.size());
        for (DdNode* root : pointerList)
            sigBefore.push_back(
                HeuristicsCheck::familySignature(dd, root, docOffset, DEMO_U, enc));

        ZddReorder::Result rr =
            ZddReorder::optimizeForest(dd, pointerList, docOffset, docsPath, heur, MAX_SIFT);

        const bool familiesEqual = HeuristicsCheck::allFamiliesEqual(
            dd, pointerList, docOffset, DEMO_U, enc, sigBefore);

        const std::string heurDir = outDir + "/" + HeuristicsCheck::sanitizeDirName(heur);
        const bool vizOk = HeuristicsCheck::writeHeurForestDotPng(
            heurDir, dd, pointerList, docOffset, DEMO_U, enc, varLabels);

        const std::string status =
            (rr.ok && rr.semanticsOk && familiesEqual) ? "ok" : "FAIL";

        std::cout << std::left << std::setw(22) << heur << " sem=" << rr.semanticsOk
                  << " fam=" << familiesEqual << " edd " << rr.eddBefore << "->" << rr.eddAfter
                  << " (" << std::fixed << std::setprecision(2) << rr.deltaPct << "%) "
                  << rr.seconds << "s viz=" << vizOk << " " << status << "\n";

        csv << heur << ',' << (rr.semanticsOk ? 1 : 0) << ',' << (familiesEqual ? 1 : 0) << ','
            << rr.eddBefore << ',' << rr.eddAfter << ',' << std::setprecision(6) << rr.deltaPct
            << ',' << rr.seconds << ',' << (rr.ok ? 1 : 0) << ',' << status << '\n';

        if (status != "ok") ++failures;

        HeuristicsCheck::freeDemoForest(dd, pointerList, sharedSnapshots);
        Cudd_Quit(dd);
    }

    std::cout << "\n[heuristics-check] csv=" << csvPath << "\n";
    std::cout << "heuristicas=" << heurs.size() << " failures=" << failures << "\n";
    return failures > 0 ? 2 : 0;
}

#endif  // ZDD_PLUS_T_CMD_HEURISTICS_CHECK_H
